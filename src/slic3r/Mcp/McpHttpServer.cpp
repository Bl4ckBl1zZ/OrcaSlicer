#include "McpHttpServer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <future>
#include <map>
#include <optional>
#include <thread>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

const char* const ENDPOINT_PATH = "/mcp";
const auto        TICK = std::chrono::milliseconds(250);
const auto        IDLE_TIMEOUT = std::chrono::seconds(120);     // keep-alive connection with no request
const auto        KEEPALIVE_COMMENT = std::chrono::seconds(15); // event streams
const auto        PROGRESS_INTERVAL = std::chrono::milliseconds(1000);

int elapsed_ms(Clock::time_point since)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

std::string str(beast::string_view view)
{
    return std::string(view.data(), view.size());
}

std::string chunk(const std::string& data)
{
    char size[32];
    std::snprintf(size, sizeof(size), "%zx\r\n", data.size());
    return std::string(size) + data + "\r\n";
}

std::string sse_event(const json& message)
{
    return "event: message\r\ndata: " + dump_json(message) + "\r\n\r\n";
}

// State shared by the listener and its connections. Touched on the network thread only, except
// `stopping`.
struct Shared
{
    Shared(HttpServerConfig cfg, const Protocol& proto, HttpServer::Hooks hk)
        : config(std::move(cfg)), protocol(proto), hooks(std::move(hk)), tokens(static_cast<double>(config.max_requests_per_second)), last_refill(Clock::now())
    {}

    HttpServerConfig   config;
    const Protocol&    protocol;
    HttpServer::Hooks  hooks;
    uint16_t           port { 0 };
    std::atomic<bool>  stopping { false };
    // In-flight calls by JSON-RPC id, for notifications/cancelled.
    std::map<std::string, std::weak_ptr<Call>> calls;

    bool allow_request()
    {
        const auto   now = Clock::now();
        const double rate = static_cast<double>(config.max_requests_per_second);
        tokens = std::min(rate, tokens + rate * std::chrono::duration<double>(now - last_refill).count());
        last_refill = now;
        if (tokens < 1.)
            return false;
        tokens -= 1.;
        return true;
    }

    void activity(const std::string& what, const std::string& outcome, int duration_ms) const
    {
        if (hooks.activity)
            hooks.activity(ActivityEntry{what, outcome, duration_ms});
    }

private:
    double            tokens;
    Clock::time_point last_refill;
};

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket socket, std::shared_ptr<Shared> shared)
        : m_socket(std::move(socket)), m_timer(m_socket.get_executor()), m_shared(std::move(shared))
    {}

    void run()
    {
        tick();
        read_request();
    }

    // Server shutdown: end event streams gracefully, drop everything else.
    void shutdown()
    {
        if (m_closed)
            return;
        if (m_call)
            m_call->cancel();
        if (m_listening) {
            m_listening = false;
            queue_write(chunk(sse_event(m_shared->protocol.listen_closed(m_listen_id))) + "0\r\n\r\n", true);
            return;
        }
        close();
    }

private:
    // --- reading ----------------------------------------------------------------------------------
    void read_request()
    {
        m_parser.emplace();
        m_parser->body_limit(m_shared->config.max_body_bytes);
        m_reading = true;
        m_read_deadline = Clock::now() + IDLE_TIMEOUT;
        http::async_read(m_socket, m_buffer, *m_parser, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    void on_read(beast::error_code ec)
    {
        m_reading = false;
        if (m_closed)
            return;
        if (ec == http::error::body_limit) {
            m_shared->activity("request", "413", 0);
            send_plain(http::status::payload_too_large, "Request body too large", false);
            return;
        }
        if (ec) {
            close(); // end of stream, reset, malformed request, shutdown
            return;
        }
        handle_request(m_parser->release());
    }

    // --- request handling ---------------------------------------------------------------------------
    void handle_request(http::request<http::string_body> request)
    {
        const bool        keep_alive = request.keep_alive();
        const auto        started = Clock::now();
        const std::string target = str(request.target());
        const std::string path = target.substr(0, target.find('?'));

        if (path != ENDPOINT_PATH) {
            m_shared->activity("request", "404", 0);
            return send_plain(http::status::not_found, "Not found", keep_alive);
        }

        // Host: defeats DNS rebinding even when a browser sends no Origin.
        const std::string host = to_lower_ascii(str(request[http::field::host]));
        const std::string port = std::to_string(m_shared->port);
        if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
            m_shared->activity("request", "403 host", 0);
            return send_json(http::status::forbidden, Protocol::transport_error(-32600, "Forbidden: unexpected Host header"), keep_alive);
        }

        // Origin: native clients send none; a browser page always does.
        if (request.find(http::field::origin) != request.end()) {
            const std::string  origin = str(request[http::field::origin]);
            const auto&        allowed = m_shared->config.allowed_origins;
            if (std::find(allowed.begin(), allowed.end(), origin) == allowed.end()) {
                m_shared->activity("request", "403 origin", 0);
                return send_json(http::status::forbidden, Protocol::transport_error(-32600, "Forbidden: this Origin is not allowed"), keep_alive);
            }
        }

        // Bearer token.
        {
            const std::string authorization = str(request[http::field::authorization]);
            static const std::string scheme = "bearer ";
            const bool has_scheme = authorization.size() > scheme.size() && to_lower_ascii(authorization.substr(0, scheme.size())) == scheme;
            const std::string presented = has_scheme ? authorization.substr(scheme.size()) : std::string();
            if (!has_scheme || !constant_time_equals(presented, m_shared->config.token)) {
                m_shared->activity("request", "401", 0);
                auto response = make_response(http::status::unauthorized, keep_alive);
                response->set(http::field::www_authenticate, "Bearer");
                response->prepare_payload();
                return send(std::move(response), keep_alive);
            }
        }

        if (request.method() != http::verb::post) {
            m_shared->activity(str(request.method_string()), "405", 0);
            auto response = make_response(http::status::method_not_allowed, keep_alive);
            response->set(http::field::allow, "POST");
            response->prepare_payload();
            return send(std::move(response), keep_alive);
        }

        {
            const std::string content_type = to_lower_ascii(str(request[http::field::content_type]));
            if (content_type.rfind("application/json", 0) != 0) {
                m_shared->activity("request", "415", 0);
                return send_json(http::status::unsupported_media_type, Protocol::transport_error(-32600, "Content-Type must be application/json"), keep_alive);
            }
        }

        if (!m_shared->allow_request()) {
            m_shared->activity("request", "429", 0);
            return send_json(http::status::too_many_requests, Protocol::transport_error(-32600, "Too many requests"), keep_alive);
        }

        RequestHead head;
        if (auto it = request.find("MCP-Protocol-Version"); it != request.end()) {
            head.has_version = true;
            head.version = str(it->value());
        }
        if (auto it = request.find("Mcp-Method"); it != request.end()) {
            head.has_method = true;
            head.method = str(it->value());
        }
        if (auto it = request.find("Mcp-Name"); it != request.end()) {
            head.has_name = true;
            head.name = str(it->value());
        }
        head.accepts_event_stream = to_lower_ascii(str(request[http::field::accept])).find("text/event-stream") != std::string::npos;

        Reply reply = m_shared->protocol.handle(head, request.body());

        switch (reply.kind) {
        case Reply::Kind::Accepted: {
            if (!reply.cancel_key.empty()) {
                if (auto it = m_shared->calls.find(reply.cancel_key); it != m_shared->calls.end())
                    if (auto call = it->second.lock())
                        call->cancel();
            }
            m_shared->activity(reply.method.empty() ? "notification" : reply.method, "202", elapsed_ms(started));
            auto response = make_response(http::status::accepted, keep_alive);
            response->prepare_payload();
            return send(std::move(response), keep_alive);
        }
        case Reply::Kind::Json: {
            std::string outcome = "ok";
            if (reply.body.is_object() && reply.body.contains("error"))
                outcome = "error " + std::to_string(reply.body["error"].value("code", 0));
            m_shared->activity(reply.method.empty() ? "request" : reply.method, outcome, elapsed_ms(started));
            return send_json(static_cast<http::status>(reply.status), reply.body, keep_alive);
        }
        case Reply::Kind::Listen: {
            m_listening = true;
            m_listen_id = reply.id;
            m_shared->activity(reply.method, "listening", elapsed_ms(started));
            start_event_stream();
            queue_write(chunk(sse_event(Protocol::listen_acknowledgement(m_listen_id))), false);
            monitor_disconnect();
            return;
        }
        case Reply::Kind::CallTool:
            return start_call(std::move(reply), started);
        }
    }

    // --- tool calls ---------------------------------------------------------------------------------
    void start_call(Reply reply, Clock::time_point started)
    {
        m_call = std::make_shared<Call>(*reply.tool, std::move(reply.args));
        m_call_id = reply.id;
        m_call_modern = reply.modern;
        m_call_started = started;
        m_call_what = reply.method;
        m_call_key = Protocol::call_key(reply.id);
        m_streaming = reply.stream;
        m_progress_token = reply.progress_token;

        std::weak_ptr<Session> weak = weak_from_this();
        auto                   executor = m_socket.get_executor();
        m_call->on_complete = [weak, executor](CallResult result) {
            auto shared_result = std::make_shared<CallResult>(std::move(result));
            net::post(executor, [weak, shared_result]() {
                if (auto self = weak.lock())
                    self->on_call_complete(std::move(*shared_result));
            });
        };
        if (m_streaming) {
            m_call->on_progress = [weak, executor](double percent, const std::string& text) {
                net::post(executor, [weak, percent, text]() {
                    if (auto self = weak.lock())
                        self->on_call_progress(percent, text);
                });
            };
            start_event_stream();
        }

        m_shared->calls[m_call_key] = m_call;
        monitor_disconnect();
        if (m_shared->hooks.submit)
            m_shared->hooks.submit(m_call);
        else
            m_call->complete(make_error_result(ToolError("internal", "No dispatcher.")));
    }

    void on_call_progress(double percent, const std::string& text)
    {
        if (m_closed || !m_call || m_call->finished())
            return;
        const auto now = Clock::now();
        if (now - m_last_progress < PROGRESS_INTERVAL)
            return;
        m_last_progress = now;
        queue_write(chunk(sse_event(Protocol::progress_notification(m_progress_token, percent, text))), false);
    }

    void on_call_complete(CallResult result)
    {
        if (m_closed || !m_call)
            return;
        forget_call_key();
        m_shared->activity(m_call_what, result.outcome.empty() ? (result.is_error ? "error" : "ok") : result.outcome, elapsed_ms(m_call_started));
        const json response = m_shared->protocol.tool_response(m_call_id, m_call_modern, result);
        m_call.reset();
        // The connection is closed after a tool call: the disconnect monitor owns the read side, so
        // it cannot be handed back to the request parser. Clients open a new one per request anyway.
        if (m_streaming)
            queue_write(chunk(sse_event(response)) + "0\r\n\r\n", true);
        else
            send_json(http::status::ok, response, false);
    }

    void forget_call_key()
    {
        if (auto it = m_shared->calls.find(m_call_key); it != m_shared->calls.end()) {
            auto current = it->second.lock();
            if (!current || current == m_call)
                m_shared->calls.erase(it);
        }
    }

    // While a call or an event stream is open nothing more is expected from the client; a read
    // that fails means it went away, which is how a client cancels on Streamable HTTP.
    void monitor_disconnect()
    {
        m_socket.async_read_some(m_buffer.prepare(1024), [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (self->m_closed || !ec)
                return; // data from the client is ignored: the connection closes after the response
            if (ec == net::error::operation_aborted)
                return;
            if (self->m_call) {
                BOOST_LOG_TRIVIAL(info) << "MCP: client closed the connection during " << self->m_call_what << "; cancelling";
                self->m_call->cancel();
                self->forget_call_key();
                self->m_shared->activity(self->m_call_what, "cancelled", elapsed_ms(self->m_call_started));
            }
            self->close();
        });
    }

    // --- periodic work --------------------------------------------------------------------------------
    void tick()
    {
        m_timer.expires_after(TICK);
        m_timer.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || self->m_closed)
                return;
            self->on_tick();
            self->tick();
        });
    }

    void on_tick()
    {
        const auto now = Clock::now();
        if (m_shared->stopping.load()) {
            shutdown();
            return;
        }
        if (m_call && !m_call->finished() && m_call->hard_deadline_passed(now)) {
            // The main thread did not answer in time; answer for it. The handler's own result, if
            // one ever arrives, is dropped.
            if (!m_call->started()) {
                const std::string reason = m_call->blocked_reason();
                m_call->complete(make_error_result(ToolError(
                    "ui_busy", "OrcaSlicer did not get to this call in time" + (reason.empty() ? std::string(": the window is not responding.") : ": " + reason + "."),
                    "Retry shortly. app_status shows what is busy.", json::object({{"reason", reason.empty() ? "not_responding" : reason}}))));
            } else {
                m_call->complete(make_error_result(ToolError(
                    "timeout", "The call did not finish in time. It may still be running in OrcaSlicer.",
                    "Check the state with scene_get or app_status before repeating the call.")));
            }
        }
        if ((m_streaming || m_listening) && m_stream_open && now - m_last_write > KEEPALIVE_COMMENT)
            queue_write(chunk(": keep-alive\r\n\r\n"), false);
        if (m_reading && now > m_read_deadline)
            close();
    }

    // --- writing ---------------------------------------------------------------------------------------
    using Response = http::response<http::string_body>;

    std::shared_ptr<Response> make_response(http::status status, bool keep_alive) const
    {
        auto response = std::make_shared<Response>(status, 11);
        response->set(http::field::server, "OrcaSlicer-MCP");
        response->set(http::field::cache_control, "no-store");
        response->keep_alive(keep_alive);
        return response;
    }

    void send_json(http::status status, const json& body, bool keep_alive)
    {
        auto response = make_response(status, keep_alive);
        response->set(http::field::content_type, "application/json");
        response->body() = dump_json(body);
        response->prepare_payload();
        send(std::move(response), keep_alive);
    }

    void send_plain(http::status status, const std::string& text, bool keep_alive)
    {
        auto response = make_response(status, keep_alive);
        response->set(http::field::content_type, "text/plain");
        response->body() = text;
        response->prepare_payload();
        send(std::move(response), keep_alive);
    }

    void send(std::shared_ptr<Response> response, bool keep_alive)
    {
        http::async_write(m_socket, *response, [self = shared_from_this(), response, keep_alive](beast::error_code ec, std::size_t) {
            if (ec || !keep_alive || self->m_closed)
                self->close();
            else
                self->read_request();
        });
    }

    void start_event_stream()
    {
        m_stream_open = true;
        queue_write("HTTP/1.1 200 OK\r\n"
                    "Server: OrcaSlicer-MCP\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-store\r\n"
                    "X-Accel-Buffering: no\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n",
                    false);
    }

    void queue_write(std::string data, bool close_after)
    {
        if (m_closed)
            return;
        m_out.push_back(std::move(data));
        m_close_after_write = m_close_after_write || close_after;
        m_last_write = Clock::now();
        if (!m_writing)
            write_next();
    }

    void write_next()
    {
        if (m_out.empty()) {
            m_writing = false;
            if (m_close_after_write)
                close();
            return;
        }
        m_writing = true;
        net::async_write(m_socket, net::buffer(m_out.front()), [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec || self->m_closed) {
                self->close();
                return;
            }
            self->m_out.pop_front();
            self->write_next();
        });
    }

    void close()
    {
        if (m_closed)
            return;
        m_closed = true;
        if (m_call) {
            m_call->cancel();
            forget_call_key();
        }
        beast::error_code ec;
        m_timer.cancel();
        m_socket.shutdown(tcp::socket::shutdown_both, ec);
        m_socket.close(ec);
    }

    tcp::socket             m_socket;
    net::steady_timer       m_timer;
    std::shared_ptr<Shared> m_shared;
    beast::flat_buffer      m_buffer;
    std::optional<http::request_parser<http::string_body>> m_parser;

    bool              m_closed { false };
    bool              m_reading { false };
    Clock::time_point m_read_deadline;

    std::shared_ptr<Call> m_call;
    json                  m_call_id;
    bool                  m_call_modern { false };
    Clock::time_point     m_call_started;
    std::string           m_call_what;
    std::string           m_call_key;
    bool                  m_streaming { false };
    json                  m_progress_token;
    Clock::time_point     m_last_progress;

    bool m_listening { false };
    json m_listen_id;

    bool                    m_stream_open { false };
    std::deque<std::string> m_out;
    bool                    m_writing { false };
    bool                    m_close_after_write { false };
    Clock::time_point       m_last_write;
};

} // namespace

struct HttpServer::Impl
{
    Impl(HttpServerConfig config, const Protocol& protocol, Hooks hooks)
        : shared(std::make_shared<Shared>(std::move(config), protocol, std::move(hooks))), acceptor(io)
    {}

    net::io_context                     io { 1 };
    std::shared_ptr<Shared>             shared;
    tcp::acceptor                       acceptor;
    std::thread                         thread;
    std::vector<std::weak_ptr<Session>> sessions;
    bool                                running { false };

    size_t prune_sessions()
    {
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [](const std::weak_ptr<Session>& s) { return s.expired(); }), sessions.end());
        return sessions.size();
    }

    void accept()
    {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (ec == net::error::operation_aborted || !acceptor.is_open())
                return;
            if (!ec) {
                beast::error_code ignored;
                socket.set_option(tcp::no_delay(true), ignored);
                if (static_cast<int>(prune_sessions()) >= shared->config.max_connections) {
                    static const std::string busy = "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    auto                     holder = std::make_shared<tcp::socket>(std::move(socket));
                    net::async_write(*holder, net::buffer(busy), [holder](beast::error_code, std::size_t) {
                        beast::error_code e;
                        holder->shutdown(tcp::socket::shutdown_both, e);
                        holder->close(e);
                    });
                    shared->activity("connection", "429", 0);
                } else {
                    auto session = std::make_shared<Session>(std::move(socket), shared);
                    sessions.push_back(session);
                    session->run();
                }
            }
            accept();
        });
    }
};

HttpServer::HttpServer(HttpServerConfig config, const Protocol& protocol, Hooks hooks)
    : m_impl(std::make_unique<Impl>(std::move(config), protocol, std::move(hooks)))
{}

HttpServer::~HttpServer() { stop(); }

uint16_t HttpServer::port() const { return m_impl->shared->port; }

bool HttpServer::start(std::string& error)
{
    Impl& impl = *m_impl;
    if (impl.running)
        return true;
    if (impl.shared->config.token.empty()) {
        error = "no access token";
        return false;
    }

    beast::error_code   ec;
    const tcp::endpoint endpoint(net::ip::make_address_v4("127.0.0.1"), impl.shared->config.port);
    impl.acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        error = "cannot open a socket: " + ec.message();
        return false;
    }
#ifndef _WIN32
    // Rebind right after a restart. Not on Windows, where this option lets two processes share a port.
    impl.acceptor.set_option(net::socket_base::reuse_address(true), ec);
#else
    // No other process may bind the same address and port, not even one that sets SO_REUSEADDR
    // (which on Windows would let it take over or share the port and read the requests).
    {
        int exclusive = 1;
        if (::setsockopt(impl.acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                         static_cast<int>(sizeof(exclusive))) != 0) {
            error = "cannot claim the port exclusively (SO_EXCLUSIVEADDRUSE, error " + std::to_string(::WSAGetLastError()) + ")";
            beast::error_code ignored;
            impl.acceptor.close(ignored);
            return false;
        }
    }
#endif
    impl.acceptor.bind(endpoint, ec);
    if (ec) {
        error = "port " + std::to_string(impl.shared->config.port) + " is not available (" + ec.message() + ")";
        beast::error_code ignored;
        impl.acceptor.close(ignored);
        return false;
    }
    impl.acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        error = "cannot listen: " + ec.message();
        beast::error_code ignored;
        impl.acceptor.close(ignored);
        return false;
    }
    impl.shared->port = impl.acceptor.local_endpoint(ec).port();
    impl.shared->stopping.store(false);
    impl.accept();
    impl.running = true;
    impl.thread = std::thread([&impl]() {
        try {
            impl.io.run();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "MCP: network thread stopped: " << e.what();
        }
    });
    return true;
}

void HttpServer::stop()
{
    Impl& impl = *m_impl;
    if (!impl.running)
        return;
    impl.running = false;
    impl.shared->stopping.store(true);

    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    net::post(impl.io, [&impl, done]() {
        beast::error_code ec;
        impl.acceptor.close(ec);
        for (const std::weak_ptr<Session>& weak : impl.sessions)
            if (auto session = weak.lock())
                session->shutdown();
        done->set_value();
    });
    // Give graceful closes a moment, then stop whatever is left.
    if (finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    impl.io.stop();
    if (impl.thread.joinable())
        impl.thread.join();
    impl.sessions.clear();
    impl.shared->calls.clear();
}

}}} // namespace Slic3r::GUI::Mcp
