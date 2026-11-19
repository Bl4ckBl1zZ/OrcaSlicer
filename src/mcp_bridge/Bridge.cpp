// The forwarder: newline-delimited JSON-RPC on stdio <-> Streamable HTTP POSTs (and SSE).
//
// Threads: the main thread runs one io_context that owns all bridge state; a reader thread
// blocks on stdin and posts complete lines to it; a writer thread drains the stdout queue so a
// slow reader of a multi-megabyte result never stalls other requests.
//
// Protocol eras (see docs/mcp/orca-mcp-bridge.md): the bridge does not implement MCP. It looks
// at each message only to compute the HTTP headers the Streamable HTTP binding requires, to
// match responses to requests, and to translate stdio cancellation into closing a stream.
#include "BridgeCommon.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orca_mcp_bridge {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace json  = boost::json;
using tcp       = asio::ip::tcp;
using Clock     = std::chrono::steady_clock;

namespace {

constexpr std::size_t MAX_LINE_BYTES       = std::size_t(256) << 20; // a single stdin message
constexpr std::size_t MAX_ERROR_BODY_BYTES = std::size_t(1) << 20;   // body of a non-2xx response
constexpr auto        CONNECT_TIMEOUT      = std::chrono::seconds(10);
constexpr int         MAX_RESUME_ATTEMPTS  = 5;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// ---------------------------------------------------------------------------------------------
// stdout writer thread
// ---------------------------------------------------------------------------------------------
class Output
{
public:
    explicit Output(std::function<void()> on_broken) : m_on_broken(std::move(on_broken)) {}

    void start()
    {
        m_thread = std::thread([this] { loop(); });
    }

    void send(std::string line)
    {
        line += '\n';
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_broken)
                return;
            m_queue.push_back(std::move(line));
        }
        m_cv.notify_all();
    }

    // Flushes what is queued (bounded wait), then stops the thread.
    void finish(std::chrono::milliseconds max_wait)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_closing = true;
            m_cv.notify_all();
            m_cv.wait_for(lock, max_wait, [this] { return m_exited; });
            if (!m_exited) {
                lock.unlock();
                m_thread.detach(); // the client stopped reading; do not hang on exit
                return;
            }
        }
        if (m_thread.joinable())
            m_thread.join();
    }

    bool broken() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_broken;
    }

private:
    void loop()
    {
        for (;;) {
            std::string item;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_queue.empty() || m_closing; });
                if (m_queue.empty()) {
                    m_exited = true;
                    m_cv.notify_all();
                    return;
                }
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }
            if (!stdout_write_all(item.data(), item.size())) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_broken = true;
                    m_queue.clear();
                    m_exited = true;
                }
                m_cv.notify_all();
                m_on_broken();
                return;
            }
        }
    }

    std::function<void()>   m_on_broken;
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    bool                    m_closing = false;
    bool                    m_broken  = false;
    bool                    m_exited  = false;
    std::thread             m_thread;
};

// ---------------------------------------------------------------------------------------------
// One HTTP exchange
// ---------------------------------------------------------------------------------------------
enum class Era { Unknown, Modern, Legacy };
enum class Kind {
    Request,      // client request: answer must reach stdout
    Notification, // client notification (legacy era) or bridge-internal notification
    Response,     // client's response to a server-initiated request (legacy era)
    Internal,     // bridge-internal request (re-initialize, tools/list refresh)
    Listen,       // legacy GET stream for server-initiated messages
    Delete        // legacy session termination
};

const char *era_name(Era e) { return e == Era::Modern ? "modern" : e == Era::Legacy ? "legacy" : "unknown"; }

using Parser = http::response_parser<http::buffer_body>;

struct Exchange
{
    explicit Exchange(asio::io_context &ioc) : timer(ioc), deadline(ioc) {}

    Kind        kind = Kind::Request;
    Era         era  = Era::Unknown;
    json::value msg;           // parsed body (object), null for GET/DELETE
    std::string body;          // exact bytes sent as the POST body
    json::value id;            // request id
    std::string id_key;        // serialized id
    std::string method;
    std::string modern_version;
    bool        is_resume = false; // GET with Last-Event-ID (legacy)

    Endpoint ep;
    std::optional<beast::tcp_stream>               stream;
    std::optional<http::request<http::string_body>> req;
    std::optional<Parser>                          parser;
    beast::flat_buffer                             buf;
    std::vector<char>                              chunk = std::vector<char>(64 * 1024);
    int                                            status = 0;
    bool                                           sse    = false;
    std::string                                    body_in; // non-SSE response body
    bool                                           body_truncated = false;
    SseParser                                      sse_parser;
    std::string                                    last_event_id;
    long                                           retry_ms = 1000;
    bool                                           got_response = false;
    bool                                           sent_session = false;
    std::string                                    session_used;

    unsigned gen   = 0;     // attempt number; handlers of older attempts do nothing
    bool done      = false; // finished (answered or abandoned)
    bool cancelled = false;

    // retry state
    bool                      reread_after_refused = false;
    bool                      reread_after_auth    = false;
    bool                      session_retry        = false;
    bool                      mismatch_retry       = false;
    std::optional<Clock::time_point> first_unreachable;
    std::optional<Clock::time_point> first_busy;
    int                       resume_attempts = 0;
    int                       listen_failures = 0;

    asio::steady_timer timer;
    asio::steady_timer deadline;
    std::function<void(const json::value *)> on_done; // internal exchanges
};
using ExPtr = std::shared_ptr<Exchange>;

// ---------------------------------------------------------------------------------------------
// The bridge
// ---------------------------------------------------------------------------------------------
class Bridge
{
public:
    Bridge(asio::io_context &ioc, const Options &opts, std::function<void(std::string)> sink)
        : m_ioc(ioc), m_opts(opts), m_resolver_cfg(opts), m_dns(ioc), m_sink(std::move(sink))
    {}

    // -----------------------------------------------------------------------------------------
    // stdin side
    // -----------------------------------------------------------------------------------------
    void on_line(std::string line)
    {
        if (m_finalizing)
            return;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        if (start == line.size())
            return; // blank line
        if (start > 0)
            line.erase(0, start);

        boost::system::error_code ec;
        json::value v = json::parse(line, ec);
        if (ec) {
            log_error("stdin: not valid JSON (" + ec.message() + "); answered with a parse error");
            emit_error(json::value(nullptr), -32700, "Parse error: the line is not valid UTF-8 JSON (" + ec.message() + ")",
                       "parse_error");
            return;
        }
        if (!v.is_object()) {
            log_error("stdin: JSON-RPC message is not an object");
            emit_error(json::value(nullptr), -32600,
                       v.is_array() ? "Invalid Request: JSON-RPC batches are not supported by MCP" : "Invalid Request: not a JSON object",
                       "invalid_request");
            return;
        }
        json::object &o      = v.as_object();
        const json::value *m = o.if_contains("method");
        const json::value *id = o.if_contains("id");
        const bool has_id     = id && !id->is_null();
        if (m && m->is_string() && has_id && (id->is_string() || id->is_int64() || id->is_uint64())) {
            handle_request(std::move(v), std::move(line));
        } else if (m && m->is_string() && !id) {
            handle_notification(std::move(v), std::move(line));
        } else if (!m && has_id && (o.contains("result") || o.contains("error"))) {
            handle_client_response(std::move(v), std::move(line));
        } else {
            log_error("stdin: not a JSON-RPC request, notification or response");
            emit_error(has_id ? *id : json::value(nullptr), -32600, "Invalid Request: not a JSON-RPC 2.0 request, notification or response",
                       "invalid_request");
        }
    }

    void on_line_too_long()
    {
        log_error("stdin: message larger than " + std::to_string(MAX_LINE_BYTES >> 20) + " MB discarded");
        emit_error(json::value(nullptr), -32600, "Invalid Request: message too large", "message_too_large");
    }

    void on_eof()
    {
        log_info("stdin closed; shutting down");
        m_stdin_closed = true;
        if (m_opts.eof_grace_s > 0 && !m_inflight.empty()) {
            auto t = std::make_shared<asio::steady_timer>(m_ioc);
            t->expires_after(std::chrono::milliseconds(long(m_opts.eof_grace_s * 1000)));
            t->async_wait([this, t](const boost::system::error_code &) { finalize(EXIT_OK); });
            return;
        }
        finalize(EXIT_OK);
    }

    void on_stdout_broken()
    {
        log_error("stdout closed by the client; exiting");
        finalize(EXIT_STDOUT_CLOSED);
    }

    void finalize(int code)
    {
        if (m_finalizing)
            return;
        m_finalizing = true;
        m_exit_code  = code;
        // Abandon everything in flight. On HTTP, closing a request's stream is its cancellation.
        auto active = m_active;
        for (const auto &ex : active) {
            ex->cancelled = true;
            ex->done      = true;
            close_stream(ex);
        }
        m_inflight.clear();
        if (!m_session_id.empty() && m_endpoint_for_delete) {
            // Legacy session: tell the server we are leaving (best effort, bounded).
            auto ex  = std::make_shared<Exchange>(m_ioc);
            ex->kind = Kind::Delete;
            ex->ep   = *m_endpoint_for_delete;
            ex->on_done = [this](const json::value *) { m_ioc.stop(); };
            auto t = std::make_shared<asio::steady_timer>(m_ioc);
            t->expires_after(std::chrono::seconds(2));
            t->async_wait([this, t](const boost::system::error_code &) { m_ioc.stop(); });
            start_http(ex);
            return;
        }
        asio::post(m_ioc, [this] { m_ioc.stop(); });
    }

    int exit_code() const { return m_exit_code; }
    bool idle() const { return m_inflight.empty(); }

private:
    // -----------------------------------------------------------------------------------------
    // Message classification
    // -----------------------------------------------------------------------------------------
    static std::string modern_version_of(const json::object &msg)
    {
        const json::value *p = msg.if_contains("params");
        if (!p || !p->is_object())
            return std::string();
        const json::value *meta = p->as_object().if_contains("_meta");
        if (!meta || !meta->is_object())
            return std::string();
        const json::value *v = meta->as_object().if_contains(META_PROTOCOL_VERSION);
        if (!v || !v->is_string())
            return std::string();
        return std::string(v->as_string());
    }

    void handle_request(json::value v, std::string line)
    {
        auto ex    = std::make_shared<Exchange>(m_ioc);
        ex->kind   = Kind::Request;
        ex->id     = v.as_object().at("id");
        ex->id_key = json::serialize(ex->id);
        ex->method = std::string(v.as_object().at("method").as_string());
        ex->modern_version = modern_version_of(v.as_object());
        ex->era    = !ex->modern_version.empty() ? Era::Modern : Era::Legacy;
        ex->msg    = std::move(v);
        ex->body   = std::move(line);
        if (ex->era == Era::Modern)
            m_era = Era::Modern;
        else if (ex->method == "initialize")
            m_era = Era::Legacy;
        if (m_inflight.count(ex->id_key))
            log_error("request id " + ex->id_key + " reused while the previous request with that id is still running");
        m_inflight[ex->id_key] = ex;
        log_debug("-> " + ex->method + " id=" + ex->id_key + " (" + era_name(ex->era) + ")");
        if (m_opts.request_timeout_s > 0 && ex->method != "subscriptions/listen") {
            ex->deadline.expires_after(std::chrono::milliseconds(long(m_opts.request_timeout_s * 1000)));
            ex->deadline.async_wait([this, ex](const boost::system::error_code &ec) {
                if (ec || ex->done)
                    return;
                log_error(ex->method + " id=" + ex->id_key + " timed out");
                if (ex->era == Era::Legacy)
                    send_legacy_cancel(ex, "timed out in orca-mcp-bridge");
                fail(ex, ERR_TIMEOUT,
                     "OrcaSlicer did not answer within " + std::to_string(long(m_opts.request_timeout_s)) +
                         " s (--request-timeout); the request was cancelled.",
                     "timeout");
            });
        }
        start_http(ex);
    }

    void handle_notification(json::value v, std::string line)
    {
        const json::object &o = v.as_object();
        const std::string method(o.at("method").as_string());
        if (method == "notifications/cancelled") {
            std::string key;
            if (const json::value *p = o.if_contains("params"); p && p->is_object())
                if (const json::value *rid = p->as_object().if_contains("requestId"))
                    key = json::serialize(*rid);
            auto it = key.empty() ? m_inflight.end() : m_inflight.find(key);
            if (it != m_inflight.end()) {
                ExPtr ex = it->second;
                log_info("cancel " + ex->method + " id=" + key + " (" + era_name(ex->era) + ")");
                // Modern: closing the stream IS the cancellation on HTTP; the notification is
                // stdio-only and is not forwarded. Legacy: disconnecting is not cancellation, so the
                // notification is forwarded, and the stream is closed as well.
                if (ex->era == Era::Legacy)
                    post_notification(std::move(v), std::move(line), Era::Legacy);
                ex->cancelled = true;
                complete(ex);
                return;
            }
            if (m_era == Era::Legacy)
                post_notification(std::move(v), std::move(line), Era::Legacy);
            else
                log_debug("cancel for unknown/finished request " + key + " ignored");
            return;
        }
        const Era era = !modern_version_of(o).empty() ? Era::Modern : (m_era == Era::Modern ? Era::Modern : Era::Legacy);
        post_notification(std::move(v), std::move(line), era);
    }

    void post_notification(json::value v, std::string line, Era era, std::function<void(const json::value *)> done = {})
    {
        auto ex     = std::make_shared<Exchange>(m_ioc);
        ex->kind    = Kind::Notification;
        ex->method  = std::string(v.as_object().at("method").as_string());
        ex->era     = era;
        ex->msg     = std::move(v);
        ex->body    = std::move(line);
        ex->on_done = std::move(done);
        log_debug("-> notification " + ex->method);
        start_http(ex);
    }

    void handle_client_response(json::value v, std::string line)
    {
        // Answer to a server-initiated request (legacy era only: the current revision has no
        // server-to-client requests). POSTed like a notification.
        auto ex  = std::make_shared<Exchange>(m_ioc);
        ex->kind = Kind::Response;
        ex->era  = Era::Legacy;
        ex->msg  = std::move(v);
        ex->body = std::move(line);
        log_debug("-> response to a server request");
        start_http(ex);
    }

    void send_legacy_cancel(const ExPtr &ex, const char *reason)
    {
        json::object params{{"requestId", ex->id}, {"reason", reason}};
        json::value  n = json::object{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", params}};
        std::string  text = json::serialize(n);
        post_notification(std::move(n), std::move(text), Era::Legacy);
    }

    // -----------------------------------------------------------------------------------------
    // HTTP
    // -----------------------------------------------------------------------------------------
    void start_http(const ExPtr &ex)
    {
        if (ex->done)
            return;
        m_active.insert(ex);
        if (ex->kind != Kind::Delete) {
            ResolveResult r = m_resolver_cfg.resolve(false);
            if (!r.endpoint) {
                unreachable(ex, r.error, r.reason);
                return;
            }
            ex->ep = *r.endpoint;
        }
        m_endpoint_for_delete = ex->ep;
        const unsigned g = ++ex->gen;
        build_request(ex);
        ex->stream.emplace(m_ioc);
        ex->status = 0;
        ex->sse    = false;
        ex->body_in.clear();
        ex->body_truncated = false;
        ex->buf.consume(ex->buf.size());
        ex->parser.reset();
        m_dns.async_resolve(ex->ep.url.host, ex->ep.url.port,
                            [this, ex, g](const boost::system::error_code &ec, tcp::resolver::results_type results) {
                                if (ex->done || g != ex->gen)
                                    return;
                                if (ec) {
                                    connect_failed(ex, "cannot resolve " + ex->ep.url.host + ": " + ec.message());
                                    return;
                                }
                                std::vector<tcp::endpoint> eps;
                                for (const auto &e : results)
                                    if (m_opts.allow_remote || e.endpoint().address().is_loopback())
                                        eps.push_back(e.endpoint());
                                if (eps.empty()) {
                                    fail(ex, ERR_CONFIG, ex->ep.url.host + " does not resolve to a loopback address", "non_loopback");
                                    return;
                                }
                                ex->stream->expires_after(CONNECT_TIMEOUT);
                                ex->stream->async_connect(eps, [this, ex, g](const boost::system::error_code &ec2, const tcp::endpoint &) {
                                    if (ex->done || g != ex->gen)
                                        return;
                                    if (ec2) {
                                        connect_failed(ex, ec2.message());
                                        return;
                                    }
                                    ex->stream->expires_never();
                                    beast::error_code ignore;
                                    ex->stream->socket().set_option(asio::socket_base::keep_alive(true), ignore);
                                    ex->stream->socket().set_option(tcp::no_delay(true), ignore);
                                    write_request(ex, g);
                                });
                            });
    }

    void build_request(const ExPtr &ex)
    {
        http::verb verb = http::verb::post;
        if (ex->kind == Kind::Listen || ex->is_resume)
            verb = http::verb::get;
        else if (ex->kind == Kind::Delete)
            verb = http::verb::delete_;
        ex->req.emplace(verb, ex->ep.url.target, 11);
        auto &req = *ex->req;
        req.set(http::field::host, ex->ep.url.host_header);
        req.set(http::field::user_agent, std::string(BRIDGE_NAME) + "/" + BRIDGE_VERSION);
        req.set(http::field::connection, "close");
        if (!ex->ep.token.empty())
            req.set(http::field::authorization, "Bearer " + ex->ep.token);
        ex->sent_session = false;

        if (verb == http::verb::get) {
            req.set(http::field::accept, "text/event-stream");
            if (!ex->last_event_id.empty())
                req.set("Last-Event-ID", ex->last_event_id);
        } else if (verb == http::verb::post) {
            req.set(http::field::accept, "application/json, text/event-stream");
            req.set(http::field::content_type, "application/json");
            req.body() = ex->body;
            req.prepare_payload();
        }

        if (ex->era == Era::Modern) {
            // The 2026-07-28 binding: headers mirrored from the body. No session id.
            req.set("MCP-Protocol-Version", ex->modern_version);
            add_method_headers(ex, req, true);
        } else {
            // Legacy (2025-03-26 .. 2025-11-25): the negotiated version on every request after
            // initialize, and the session id if the server issued one.
            if (!m_legacy_version.empty() && ex->method != "initialize")
                req.set("MCP-Protocol-Version", m_legacy_version);
            if (!m_session_id.empty() && ex->method != "initialize") {
                req.set("Mcp-Session-Id", m_session_id);
                ex->sent_session = true;
                ex->session_used = m_session_id;
            }
            if (verb == http::verb::post && ex->kind != Kind::Response)
                add_method_headers(ex, req, false);
        }
    }

    // Mcp-Method / Mcp-Name (and, modern era, Mcp-Param-*). Values that cannot be sent are left
    // out; the server then rejects the request with a HeaderMismatch error that is forwarded.
    void add_method_headers(const ExPtr &ex, http::request<http::string_body> &req, bool modern)
    {
        if (ex->method.empty() || !ex->msg.is_object())
            return;
        if (is_safe_header_value(ex->method))
            req.set("Mcp-Method", ex->method);
        const json::value *p = ex->msg.as_object().if_contains("params");
        if (!p || !p->is_object())
            return;
        const json::object &params = p->as_object();
        const char *name_field = nullptr;
        if (ex->method == "tools/call" || ex->method == "prompts/get")
            name_field = "name";
        else if (ex->method == "resources/read")
            name_field = "uri";
        if (name_field)
            if (const json::value *n = params.if_contains(name_field); n && n->is_string())
                req.set("Mcp-Name", encode_header_value(std::string(n->as_string())));
        if (modern && ex->method == "tools/call")
            for (const auto &h : m_tool_headers.headers_for(params))
                req.set(h.first, h.second);
    }

    void write_request(const ExPtr &ex, unsigned g)
    {
        http::async_write(*ex->stream, *ex->req, [this, ex, g](const beast::error_code &ec, std::size_t) {
            if (ex->done || g != ex->gen)
                return;
            if (ec) {
                lost(ex, "sending the request failed: " + ec.message());
                return;
            }
            ex->parser.emplace();
            ex->parser->body_limit(boost::none);
            ex->parser->header_limit(64 * 1024);
            http::async_read_header(*ex->stream, ex->buf, *ex->parser, [this, ex, g](const beast::error_code &ec2, std::size_t) {
                if (ex->done || g != ex->gen)
                    return;
                if (ec2) {
                    lost(ex, "no HTTP response: " + ec2.message());
                    return;
                }
                on_header(ex, g);
            });
        });
    }

    void on_header(const ExPtr &ex, unsigned g)
    {
        auto &res  = ex->parser->get();
        ex->status = int(res.result_int());
        const std::string ctype = lower(std::string(res[http::field::content_type]));
        ex->sse = ex->status >= 200 && ex->status < 300 && ctype.rfind("text/event-stream", 0) == 0;
        log_debug("<- HTTP " + std::to_string(ex->status) + (ex->sse ? " (SSE)" : "") + " for " +
                  (ex->method.empty() ? std::string("GET/DELETE") : ex->method));
        if (ex->method == "initialize" && ex->status >= 200 && ex->status < 300) {
            const std::string sid(res["Mcp-Session-Id"]);
            if (!sid.empty() && is_safe_header_value(sid) && sid != m_session_id) {
                m_session_id = sid;
                log_info("server issued a legacy session id");
            }
        }
        if (ex->is_resume && ex->status >= 200 && ex->status < 300 && ex->sse)
            ex->sse_parser = SseParser();
        read_body(ex, g);
    }

    void read_body(const ExPtr &ex, unsigned g)
    {
        if (ex->parser->is_done()) {
            on_body_end(ex, {});
            return;
        }
        auto &b = ex->parser->get().body();
        b.data  = ex->chunk.data();
        b.size  = ex->chunk.size();
        http::async_read_some(*ex->stream, ex->buf, *ex->parser, [this, ex, g](beast::error_code ec, std::size_t) {
            if (ex->done || g != ex->gen)
                return;
            if (ec == http::error::need_buffer)
                ec = {};
            const std::size_t n = ex->chunk.size() - ex->parser->get().body().size;
            if (n > 0)
                on_body_bytes(ex, ex->chunk.data(), n);
            if (ex->done || g != ex->gen)
                return;
            if (ec) {
                on_body_end(ex, ec);
                return;
            }
            if (ex->parser->is_done()) {
                on_body_end(ex, {});
                return;
            }
            read_body(ex, g);
        });
    }

    void on_body_bytes(const ExPtr &ex, const char *data, std::size_t n)
    {
        if (ex->sse) {
            std::vector<SseParser::Event> events;
            ex->sse_parser.feed(data, n, events);
            if (!ex->sse_parser.last_event_id().empty())
                ex->last_event_id = ex->sse_parser.last_event_id();
            if (auto r = ex->sse_parser.retry_ms())
                ex->retry_ms = *r;
            const unsigned g = ex->gen;
            for (auto &e : events) {
                if (ex->done || g != ex->gen)
                    return;
                if (!e.type.empty() && e.type != "message") {
                    log_debug("ignoring SSE event of type '" + e.type + "'");
                    continue;
                }
                if (e.data.empty())
                    continue; // priming event (legacy) or empty keep-alive
                on_server_message(ex, e.data);
            }
            return;
        }
        const bool ok_status = ex->status >= 200 && ex->status < 300;
        if (!ok_status && ex->body_in.size() + n > MAX_ERROR_BODY_BYTES) {
            ex->body_in.append(data, MAX_ERROR_BODY_BYTES - std::min(MAX_ERROR_BODY_BYTES, ex->body_in.size()));
            ex->body_truncated = true;
            return;
        }
        ex->body_in.append(data, n);
    }

    void on_body_end(const ExPtr &ex, const beast::error_code &ec)
    {
        close_stream(ex);
        if (ex->done)
            return;
        if (ex->sse) {
            if (ex->got_response)
                return complete(ex);
            switch (ex->kind) {
            case Kind::Listen: return listen_ended(ex);
            case Kind::Request:
            case Kind::Internal: {
                if (ex->era == Era::Legacy && !ex->last_event_id.empty() && ex->resume_attempts < MAX_RESUME_ATTEMPTS) {
                    // 2025-11-25: the server may close the connection before the response and expects
                    // the client to resume with GET + Last-Event-ID after `retry` ms.
                    ++ex->resume_attempts;
                    ex->is_resume = true;
                    log_info("SSE stream for " + ex->method + " id=" + ex->id_key + " closed early; resuming from event " +
                             ex->last_event_id);
                    retry_after(ex, std::max(0L, ex->retry_ms));
                    return;
                }
                if (ec && ec != http::error::end_of_stream)
                    return lost(ex, "the event stream broke: " + ec.message());
                return fail(ex, ERR_NO_RESPONSE,
                            "OrcaSlicer closed the event stream for this request without sending a response. "
                            "The app may have closed, restarted or stopped its MCP server; the request may or may not "
                            "have been carried out.",
                            "stream_ended");
            }
            default: return complete(ex);
            }
        }
        if (ec && ex->status >= 200 && ex->status < 300 && ex->kind != Kind::Notification && ex->kind != Kind::Response)
            return lost(ex, "the response broke off: " + ec.message());
        on_complete_body(ex);
    }

    void on_complete_body(const ExPtr &ex)
    {
        const int st = ex->status;

        if (st == 401 || st == 403)
            return auth_failed(ex);
        if (st == 429)
            return busy(ex);
        if (st == 404 && ex->sent_session && !ex->session_retry && !m_stored_initialize.empty() &&
            (ex->kind == Kind::Request || ex->kind == Kind::Internal || ex->kind == Kind::Notification)) {
            // Legacy: the session expired (server restarted). Start a new one and retry once.
            ex->session_retry = true;
            if (!m_session_id.empty() && m_session_id != ex->session_used)
                return start_http(ex); // another request already started a new session
            if (m_session_id == ex->session_used)
                m_session_id.clear();
            log_info("legacy session expired (HTTP 404); re-initializing");
            return reinitialize([this, ex](bool ok) {
                if (ex->done)
                    return;
                if (ok)
                    start_http(ex);
                else
                    fail(ex, ERR_NO_RESPONSE, "OrcaSlicer ended the MCP session and starting a new one failed; restart the MCP server in the client.",
                         "session_lost");
            });
        }

        switch (ex->kind) {
        case Kind::Notification:
        case Kind::Response:
            if (st < 200 || st >= 300)
                log_error("server answered HTTP " + std::to_string(st) + " to " + (ex->method.empty() ? std::string("a response") : ex->method) +
                          body_excerpt(ex));
            return complete(ex);
        case Kind::Delete: return complete(ex);
        case Kind::Listen:
            if (st == 405) {
                log_debug("server offers no GET stream (HTTP 405)");
                return complete(ex);
            }
            return listen_ended(ex);
        case Kind::Request:
        case Kind::Internal: break;
        }

        if (ex->is_resume && (st < 200 || st >= 300))
            return fail(ex, ERR_NO_RESPONSE,
                        "OrcaSlicer closed the event stream for this request and could not resume it (HTTP " + std::to_string(st) + ").",
                        "resume_failed");

        if (st >= 200 && st < 300) {
            if (ex->body_in.empty())
                return fail(ex, ERR_NO_RESPONSE,
                            "OrcaSlicer accepted the request (HTTP " + std::to_string(st) + ") but sent no response.", "empty_response");
            on_server_message(ex, ex->body_in);
            if (!ex->done)
                fail(ex, ERR_NO_RESPONSE, "OrcaSlicer's reply did not contain a response to this request.", "no_response");
            return;
        }

        // Error status: forward a JSON-RPC error body, or synthesize one.
        boost::system::error_code jec;
        json::value v = ex->body_truncated ? json::value() : json::parse(ex->body_in, jec);
        if (!jec && v.is_object() && v.as_object().contains("error") && v.as_object().at("error").is_object()) {
            json::object &o = v.as_object();
            o["jsonrpc"]    = "2.0";
            o["id"]         = ex->id; // errors such as 403/400 may come without id
            return deliver(ex, std::move(v), std::string(), true);
        }
        synthesize_http_error(ex);
    }

    std::string body_excerpt(const ExPtr &ex) const
    {
        if (ex->body_in.empty())
            return std::string();
        std::string s = to_single_line(ex->body_in.substr(0, 300));
        return ": " + s;
    }

    void synthesize_http_error(const ExPtr &ex)
    {
        const int   st  = ex->status;
        const std::string url = ex->ep.url.text;
        std::string msg;
        std::string reason = "http_" + std::to_string(st);
        if (st == 400) {
            msg = "OrcaSlicer's MCP server rejected the request with HTTP 400 and no JSON-RPC error.";
            if (ex->era == Era::Modern)
                msg += " The server may not support MCP protocol version " + ex->modern_version +
                       "; a client that also speaks the legacy initialize handshake should fall back to it.";
            else if (ex->method == "initialize")
                msg += " The server may not accept the legacy initialize handshake (it may only speak MCP 2026-07-28 "
                       "or later); update the MCP client.";
            reason = "protocol_mismatch";
        } else if (st == 404) {
            msg = "The URL " + url + " is not an MCP endpoint (HTTP 404). Check the path, which is normally /mcp.";
        } else if (st == 405) {
            msg = "The server at " + url + " does not accept POST requests (HTTP 405); it is not a Streamable HTTP MCP endpoint.";
        } else if (st == 413) {
            msg = "The request is too large for OrcaSlicer's MCP server (HTTP 413).";
        } else if (st >= 500) {
            msg = "OrcaSlicer's MCP server failed with HTTP " + std::to_string(st) + body_excerpt(ex);
        } else {
            msg = "OrcaSlicer's MCP server answered HTTP " + std::to_string(st) + body_excerpt(ex);
        }
        fail(ex, ERR_BAD_RESPONSE, msg, reason, json::object{{"httpStatus", st}});
    }

    // A JSON-RPC message from the server (an SSE event's data or a JSON body).
    void on_server_message(const ExPtr &ex, const std::string &text)
    {
        boost::system::error_code ec;
        json::value v = json::parse(text, ec);
        if (ec || !v.is_object()) {
            log_error("server sent something that is not a JSON-RPC object" + (ec ? " (" + ec.message() + ")" : std::string()) +
                      "; dropped");
            return;
        }
        const json::object &o = v.as_object();
        const bool is_response = !o.contains("method") && o.contains("id") && (o.contains("result") || o.contains("error"));
        if (is_response) {
            const json::value &rid = o.at("id");
            if ((ex->kind == Kind::Request || ex->kind == Kind::Internal) && (rid == ex->id || rid.is_null())) {
                ex->got_response = true;
                if (rid.is_null()) {
                    v.as_object()["id"] = ex->id;
                    return deliver(ex, std::move(v), std::string(), true);
                }
                return deliver(ex, std::move(v), text, false);
            }
            // A response on another stream (legacy resumption / GET stream): route it by id.
            auto it = m_inflight.find(json::serialize(rid));
            if (it != m_inflight.end() && it->second != ex) {
                it->second->got_response = true;
                deliver(it->second, std::move(v), text, false);
                return;
            }
            log_error("dropping a response for unknown request id " + json::serialize(rid));
            return;
        }
        if (ex->kind == Kind::Internal)
            return; // progress etc. of bridge-internal requests are not the client's business
        if (ex->cancelled)
            return;
        // Notifications (progress, logging, subscription events) and, legacy era, server-initiated
        // requests: forward unchanged.
        m_sink(to_single_line(text));
    }

    // Final response for an exchange.
    void deliver(const ExPtr &ex, json::value v, const std::string &raw, bool modified)
    {
        if (ex->done)
            return;
        json::object &o = v.as_object();
        json::object *result = o.contains("result") && o.at("result").is_object() ? &o.at("result").as_object() : nullptr;
        json::object *error  = o.contains("error") && o.at("error").is_object() ? &o.at("error").as_object() : nullptr;

        if (ex->method == "initialize" && result) {
            if (const json::value *pv = result->if_contains("protocolVersion"); pv && pv->is_string()) {
                m_legacy_version = std::string(pv->as_string());
                if (!is_safe_header_value(m_legacy_version))
                    m_legacy_version.clear();
            }
            if (ex->kind == Kind::Request) {
                m_stored_initialize = ex->body;
                log_info("legacy handshake: protocol version " + m_legacy_version + (m_session_id.empty() ? "" : ", with session"));
            }
        }
        if (ex->method == "tools/list" && result && ex->era == Era::Modern) {
            const auto invalid = m_tool_headers.learn(*result);
            if (!invalid.empty() && result->contains("tools") && result->at("tools").is_array()) {
                auto &arr = result->at("tools").as_array();
                arr.erase(std::remove_if(arr.begin(), arr.end(),
                                         [&](const json::value &t) {
                                             const json::value *n = t.is_object() ? t.as_object().if_contains("name") : nullptr;
                                             return n && n->is_string() &&
                                                    std::find(invalid.begin(), invalid.end(), std::string(n->as_string())) != invalid.end();
                                         }),
                          arr.end());
                modified = true;
            }
        }
        if (error) {
            const json::value *code = error->if_contains("code");
            const long long c = code && code->is_number() ? code->to_number<long long>() : 0;
            if (c == -32020 && ex->method == "tools/call" && ex->era == Era::Modern && !ex->mismatch_retry &&
                ex->kind == Kind::Request) {
                // HeaderMismatch: the tool's x-mcp-header annotations may have changed. Re-read the
                // tool list and retry once, as the specification recommends.
                ex->mismatch_retry = true;
                ex->got_response   = false;
                ++ex->gen; // the current stream's handlers stand down
                close_stream(ex);
                log_info("HeaderMismatch on tools/call; refreshing tools/list and retrying");
                return refresh_tools(ex, [this, ex] {
                    if (!ex->done)
                        start_http(ex);
                });
            }
            if (ex->method == "initialize" && ex->kind == Kind::Request &&
                (c == -32020 || c == -32022 || c == -32600 || c == -32601 || c == -32602)) {
                std::string note = " [orca-mcp-bridge: the server rejected the legacy initialize handshake; it may only speak "
                                   "MCP 2026-07-28 or later";
                if (const json::value *d = error->if_contains("data"); d && d->is_object())
                    if (const json::value *sup = d->as_object().if_contains("supported"))
                        note += " (supported: " + json::serialize(*sup) + ")";
                note += ". Update the MCP client.]";
                const json::value *m = error->if_contains("message");
                (*error)["message"] = (m && m->is_string() ? std::string(m->as_string()) : std::string()) + note;
                modified = true;
            }
        }

        if (ex->kind == Kind::Internal) {
            auto cb = ex->on_done;
            complete(ex);
            if (cb)
                cb(&v);
            return;
        }
        if (!ex->cancelled)
            m_sink(modified ? json::serialize(v) : to_single_line(raw));
        log_debug("<- " + std::string(error ? "error" : "result") + " for " + ex->method + " id=" + ex->id_key);
        complete(ex);
    }

    void complete(const ExPtr &ex)
    {
        if (ex->done)
            return;
        ex->done = true;
        close_stream(ex);
        ex->timer.cancel();
        ex->deadline.cancel();
        m_active.erase(ex);
        if (ex->kind == Kind::Request) {
            auto it = m_inflight.find(ex->id_key);
            if (it != m_inflight.end() && it->second == ex)
                m_inflight.erase(it);
        }
        if (ex->kind != Kind::Internal && ex->on_done) {
            auto cb = std::move(ex->on_done);
            cb(nullptr);
        }
        if (ex->kind == Kind::Notification && ex->method == "notifications/initialized" && ex->status == 202)
            maybe_open_listen_stream();
        if (m_stdin_closed && !m_finalizing && m_inflight.empty())
            finalize(EXIT_OK);
    }

    void close_stream(const ExPtr &ex)
    {
        if (!ex->stream)
            return;
        beast::error_code ec;
        ex->stream->socket().shutdown(tcp::socket::shutdown_both, ec);
        ex->stream->close();
    }

    // Answers an exchange with a bridge error.
    void fail(const ExPtr &ex, int code, const std::string &message, const std::string &reason, json::object extra = {})
    {
        if (ex->done)
            return;
        if (ex->kind == Kind::Request) {
            log_error(ex->method + " id=" + ex->id_key + ": " + message);
            if (!ex->cancelled)
                emit_error(ex->id, code, message, reason, std::move(extra));
        } else if (ex->kind == Kind::Notification || ex->kind == Kind::Response) {
            log_error("could not deliver " + (ex->method.empty() ? std::string("a response") : ex->method) + ": " + message);
        } else {
            log_debug("internal exchange failed: " + message);
        }
        auto cb = ex->kind == Kind::Internal ? ex->on_done : std::function<void(const json::value *)>();
        complete(ex);
        if (cb)
            cb(nullptr);
    }

    void emit_error(const json::value &id, int code, const std::string &message, const std::string &reason, json::object extra = {})
    {
        json::object data = std::move(extra);
        data["source"]    = BRIDGE_NAME;
        data["reason"]    = reason;
        json::object err{{"code", code}, {"message", message}, {"data", std::move(data)}};
        json::object resp{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(err)}};
        m_sink(json::serialize(resp));
    }

    // -----------------------------------------------------------------------------------------
    // Failure handling and retries
    // -----------------------------------------------------------------------------------------
    void retry_after(const ExPtr &ex, long ms)
    {
        ex->timer.expires_after(std::chrono::milliseconds(ms));
        ex->timer.async_wait([this, ex](const boost::system::error_code &ec) {
            if (ec || ex->done)
                return;
            start_http(ex);
        });
    }

    // Endpoint unknown or connection refused: the request never reached a server, so it is safe
    // to retry (endpoint file re-read once immediately, then for --connect-wait seconds).
    void unreachable(const ExPtr &ex, const std::string &message, const std::string &reason)
    {
        if (ex->kind == Kind::Listen)
            return listen_ended(ex);
        if (ex->kind == Kind::Delete)
            return complete(ex);
        if (!ex->first_unreachable)
            ex->first_unreachable = Clock::now();
        if (m_opts.connect_wait_s > 0 && seconds_since(*ex->first_unreachable) < m_opts.connect_wait_s && !m_finalizing) {
            log_debug("server unreachable (" + reason + "); retrying");
            return retry_after(ex, 500);
        }
        fail(ex, reason == "non_loopback" || reason == "bad_url" || reason == "bad_token" ? ERR_CONFIG
                 : reason == "no_endpoint_file" || reason == "bad_endpoint_file" || reason == "bad_token_file"
                     ? ERR_CONFIG
                     : ERR_UNREACHABLE,
             message, reason);
    }

    void connect_failed(const ExPtr &ex, const std::string &what)
    {
        close_stream(ex);
        if (!ex->reread_after_refused) {
            // The app may have restarted on another port or with another token: re-read the
            // endpoint file and retry immediately if it changed.
            ex->reread_after_refused = true;
            ResolveResult r = m_resolver_cfg.resolve(true);
            if (r.endpoint && r.endpoint->url.text != ex->ep.url.text) {
                log_info("endpoint changed to " + r.endpoint->url.text + "; retrying");
                return start_http(ex);
            }
        }
        std::string reason;
        std::string msg = m_resolver_cfg.describe_refused(ex->ep, reason);
        log_debug("connect failed: " + what);
        unreachable(ex, msg, reason);
    }

    // The connection broke after the request may have been delivered: never retried.
    void lost(const ExPtr &ex, const std::string &what)
    {
        close_stream(ex);
        if (ex->kind == Kind::Listen)
            return listen_ended(ex);
        if (ex->kind == Kind::Delete || ex->kind == Kind::Notification || ex->kind == Kind::Response) {
            log_debug("connection problem: " + what);
            return complete(ex);
        }
        fail(ex, ERR_UNREACHABLE,
             "The connection to OrcaSlicer was lost before it answered (" + what +
                 "). The app may have closed or restarted; the request may or may not have been carried out.",
             "connection_lost");
    }

    void auth_failed(const ExPtr &ex)
    {
        if (!ex->reread_after_auth) {
            ex->reread_after_auth = true;
            ResolveResult r = m_resolver_cfg.resolve(true);
            if (r.endpoint && (r.endpoint->token != ex->ep.token || r.endpoint->url.text != ex->ep.url.text)) {
                log_info("credentials changed; retrying");
                return start_http(ex);
            }
        }
        if (ex->kind == Kind::Listen)
            return complete(ex);
        if (ex->kind != Kind::Request && ex->kind != Kind::Internal)
            return fail(ex, ERR_AUTH, "HTTP " + std::to_string(ex->status), "auth");
        std::string msg;
        if (ex->status == 401) {
            if (ex->ep.token.empty())
                msg = "OrcaSlicer's MCP server requires an access token, and none is configured. Use the endpoint file the app "
                      "writes (default), or pass --token-file.";
            else
                msg = "OrcaSlicer rejected the access token (HTTP 401). The token from " + ex->ep.token_source +
                      " does not match the one in OrcaSlicer's Preferences (MCP server)." +
                      (ex->ep.token_source.rfind("endpoint file", 0) == 0
                           ? std::string(" The endpoint file may be stale: stop and start the MCP server in Preferences.")
                           : std::string(" Update the token you configured, or drop --token/--token-file to use the endpoint file."));
            return fail(ex, ERR_AUTH, msg, "token_rejected");
        }
        std::string detail;
        boost::system::error_code jec;
        json::value v = json::parse(ex->body_in, jec);
        if (!jec && v.is_object())
            if (const json::value *e = v.as_object().if_contains("error"); e && e->is_object())
                if (const json::value *m = e->as_object().if_contains("message"); m && m->is_string())
                    detail = " (" + std::string(m->as_string()) + ")";
        msg = "OrcaSlicer refused the request (HTTP 403)" + detail +
              ". The server accepts only requests addressed to 127.0.0.1 or localhost; check the URL " + ex->ep.url.text + ".";
        fail(ex, ERR_AUTH, msg, "forbidden");
    }

    void busy(const ExPtr &ex)
    {
        if (!ex->first_busy)
            ex->first_busy = Clock::now();
        if (seconds_since(*ex->first_busy) < m_opts.busy_wait_s && !m_finalizing) {
            long ms = 500;
            if (ex->parser) {
                const std::string ra(ex->parser->get()[http::field::retry_after]);
                if (!ra.empty() && std::all_of(ra.begin(), ra.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && ra.size() < 4)
                    ms = std::min(5000L, std::stol(ra) * 1000L);
            }
            log_debug("server busy (HTTP " + std::to_string(ex->status) + "); retrying in " + std::to_string(ms) + " ms");
            return retry_after(ex, std::max(100L, ms));
        }
        if (ex->kind == Kind::Request || ex->kind == Kind::Internal)
            return fail(ex, ERR_TIMEOUT,
                        "OrcaSlicer's MCP server stayed busy (HTTP " + std::to_string(ex->status) + ") for " +
                            std::to_string(long(m_opts.busy_wait_s)) + " s; try again later.",
                        "busy");
        complete(ex);
    }

    // -----------------------------------------------------------------------------------------
    // Legacy-era helpers
    // -----------------------------------------------------------------------------------------
    void maybe_open_listen_stream()
    {
        if (!m_opts.listen_stream || m_listen || m_finalizing)
            return;
        auto ex  = std::make_shared<Exchange>(m_ioc);
        ex->kind = Kind::Listen;
        ex->era  = Era::Legacy;
        m_listen = ex;
        start_http(ex);
    }

    void listen_ended(const ExPtr &ex)
    {
        close_stream(ex);
        if (m_finalizing || ex->done)
            return complete(ex);
        if (ex->status == 405 || ex->status == 404 || ex->status == 400) {
            log_debug("GET stream not available (HTTP " + std::to_string(ex->status) + ")");
            return complete(ex);
        }
        // Reconnect with backoff (also covers the app restarting).
        ++ex->listen_failures;
        const long ms = std::min(30000L, std::max(ex->retry_ms, 1000L) * (1L << std::min(ex->listen_failures, 5)));
        ex->is_resume = false;
        retry_after(ex, ms);
    }

    // Starts a new legacy session; concurrent callers share one initialize.
    void reinitialize(std::function<void(bool)> then)
    {
        m_reinit_waiters.push_back(std::move(then));
        if (m_reinit_waiters.size() > 1)
            return;
        auto finish = [this](bool ok) {
            auto waiters = std::move(m_reinit_waiters);
            m_reinit_waiters.clear();
            for (auto &w : waiters)
                w(ok);
        };
        boost::system::error_code ec;
        json::value init = json::parse(m_stored_initialize, ec);
        if (ec || !init.is_object())
            return finish(false);
        init.as_object()["id"] = "orca-mcp-bridge/init-" + std::to_string(++m_internal_ids);
        auto ex    = std::make_shared<Exchange>(m_ioc);
        ex->kind   = Kind::Internal;
        ex->era    = Era::Legacy;
        ex->method = "initialize";
        ex->id     = init.as_object()["id"];
        ex->id_key = json::serialize(ex->id);
        ex->body   = json::serialize(init);
        ex->msg    = std::move(init);
        ex->on_done = [this, finish](const json::value *resp) {
            if (!resp || !resp->is_object() || !resp->as_object().contains("result"))
                return finish(false);
            json::value n = json::object{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
            std::string text = json::serialize(n);
            post_notification(std::move(n), std::move(text), Era::Legacy, [finish](const json::value *) { finish(true); });
        };
        start_http(ex);
    }

    void refresh_tools(const ExPtr &orig, std::function<void()> then, std::string cursor = std::string())
    {
        json::object params;
        if (const json::value *p = orig->msg.as_object().if_contains("params"); p && p->is_object())
            if (const json::value *meta = p->as_object().if_contains("_meta"); meta && meta->is_object()) {
                json::object m;
                for (const auto &kv : meta->as_object())
                    if (kv.key() != "progressToken")
                        m[kv.key()] = kv.value();
                params["_meta"] = std::move(m);
            }
        if (!cursor.empty())
            params["cursor"] = cursor;
        json::value req = json::object{{"jsonrpc", "2.0"},
                                       {"id", "orca-mcp-bridge/tools-" + std::to_string(++m_internal_ids)},
                                       {"method", "tools/list"},
                                       {"params", std::move(params)}};
        auto ex  = std::make_shared<Exchange>(m_ioc);
        ex->kind = Kind::Internal;
        ex->era  = Era::Modern;
        ex->method = "tools/list";
        ex->modern_version = orig->modern_version;
        ex->id     = req.as_object()["id"];
        ex->id_key = json::serialize(ex->id);
        ex->body   = json::serialize(req);
        ex->msg    = std::move(req);
        ex->on_done = [this, orig, then](const json::value *resp) {
            std::string next;
            if (resp && resp->is_object())
                if (const json::value *r = resp->as_object().if_contains("result"); r && r->is_object())
                    if (const json::value *nc = r->as_object().if_contains("nextCursor"); nc && nc->is_string())
                        next = std::string(nc->as_string());
            if (!next.empty())
                return refresh_tools(orig, then, next);
            then();
        };
        start_http(ex);
    }

    asio::io_context                &m_ioc;
    const Options                   &m_opts;
    EndpointResolver                 m_resolver_cfg;
    tcp::resolver                    m_dns;
    std::function<void(std::string)> m_sink;

    std::map<std::string, ExPtr> m_inflight; // client requests by serialized id
    std::set<ExPtr>              m_active;   // every exchange with network activity
    ExPtr                        m_listen;
    Era                          m_era = Era::Unknown;
    std::string                  m_legacy_version;
    std::string                  m_session_id;
    std::string                  m_stored_initialize;
    std::vector<std::function<void(bool)>> m_reinit_waiters;
    std::optional<Endpoint>      m_endpoint_for_delete;
    ToolHeaderRegistry           m_tool_headers;
    unsigned                     m_internal_ids = 0;
    bool                         m_stdin_closed = false;
    bool                         m_finalizing   = false;
    int                          m_exit_code    = EXIT_OK;
};

// ---------------------------------------------------------------------------------------------
// stdin reader thread
// ---------------------------------------------------------------------------------------------
struct ReaderGate
{
    std::mutex         mutex;
    bool               open = true;
    asio::io_context  *ioc  = nullptr;
    Bridge            *bridge = nullptr;
};

void reader_loop(std::shared_ptr<ReaderGate> gate)
{
    std::vector<char> buf(1 << 16);
    std::string line;
    bool discarding = false;
    auto post = [&gate](std::function<void(Bridge &)> fn) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->open)
            return false;
        Bridge *b = gate->bridge;
        asio::post(*gate->ioc, [b, fn = std::move(fn)] { fn(*b); });
        return true;
    };
    for (;;) {
        const long n = stdin_read(buf.data(), buf.size());
        if (n <= 0) {
            if (n < 0)
                log_error("reading stdin failed; treating as end of input");
            if (!line.empty() && !discarding)
                post([l = std::move(line)](Bridge &b) mutable { b.on_line(std::move(l)); });
            post([](Bridge &b) { b.on_eof(); });
            return;
        }
        const char *p   = buf.data();
        const char *end = p + n;
        while (p < end) {
            const char *nl = static_cast<const char *>(std::memchr(p, '\n', std::size_t(end - p)));
            const char *stop = nl ? nl : end;
            if (!discarding) {
                if (line.size() + std::size_t(stop - p) > MAX_LINE_BYTES) {
                    discarding = true;
                    line.clear();
                    line.shrink_to_fit();
                } else {
                    line.append(p, stop);
                }
            }
            if (!nl)
                break;
            if (discarding) {
                discarding = false;
                if (!post([](Bridge &b) { b.on_line_too_long(); }))
                    return;
            } else {
                if (!post([l = std::move(line)](Bridge &b) mutable { b.on_line(std::move(l)); }))
                    return;
                line = std::string();
            }
            p = nl + 1;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
int run_bridge(const Options &opts)
{
    asio::io_context ioc(1);
    Bridge *bridge_ptr = nullptr;
    Output out([&ioc, &bridge_ptr] {
        asio::post(ioc, [&bridge_ptr] {
            if (bridge_ptr)
                bridge_ptr->on_stdout_broken();
        });
    });
    Bridge bridge(ioc, opts, [&out](std::string line) { out.send(std::move(line)); });
    bridge_ptr = &bridge;
    out.start();

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&bridge](const boost::system::error_code &ec, int sig) {
        if (ec)
            return;
        log_info("signal " + std::to_string(sig) + "; shutting down");
        bridge.finalize(EXIT_OK);
    });

    auto gate    = std::make_shared<ReaderGate>();
    gate->ioc    = &ioc;
    gate->bridge = &bridge;
    std::thread reader([gate] { reader_loop(gate); });
    reader.detach(); // may stay blocked in read() after we finish; the process exit ends it

    log_info(std::string("started (version ") + BRIDGE_VERSION + ")");
    try {
        ioc.run();
    } catch (const std::exception &e) {
        log_error(std::string("internal error: ") + e.what());
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->open = false;
        }
        out.finish(std::chrono::seconds(2));
        return EXIT_INTERNAL;
    }
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->open = false;
    }
    out.finish(std::chrono::seconds(5));
    const int code = bridge.exit_code();
    log_info("exit " + std::to_string(code));
    return code;
}

// --check: one modern server/discover (and a legacy initialize if that is not understood),
// reported on stderr. Nothing is written to stdout.
int run_check(const Options &opts)
{
    EndpointResolver resolver(opts);
    for (const auto &c : resolver.endpoint_file_candidates())
        log_info("endpoint file: " + c + (file_exists(c) ? "" : " (missing)"));
    ResolveResult r = resolver.resolve(true);
    if (!r.endpoint) {
        log_error(r.error);
        return EXIT_CHECK_FAILED;
    }
    log_info("server URL: " + r.endpoint->url.text + " (from " + r.endpoint->url_source + ")");
    log_info(std::string("token: ") + (r.endpoint->token.empty() ? "none" : "present") +
             (r.endpoint->token_source.empty() ? "" : " (from " + r.endpoint->token_source + ")"));

    std::vector<std::string> answers;
    auto exchange = [&](const std::string &line) -> std::optional<json::value> {
        asio::io_context ioc(1);
        answers.clear();
        Bridge bridge(ioc, opts, [&](std::string s) {
            answers.push_back(std::move(s));
            asio::post(ioc, [&ioc] { ioc.stop(); });
        });
        asio::post(ioc, [&bridge, line] { bridge.on_line(line); });
        ioc.run();
        if (answers.empty())
            return std::nullopt;
        boost::system::error_code ec;
        json::value v = json::parse(answers.back(), ec);
        if (ec)
            return std::nullopt;
        return v;
    };

    const std::string discover =
        R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"io.modelcontextprotocol/clientInfo":{"name":"orca-mcp-bridge --check","version":"1"}}}})";
    auto v = exchange(discover);
    if (v && v->is_object() && v->as_object().contains("result")) {
        const auto &res = v->as_object().at("result");
        std::string versions = res.is_object() && res.as_object().contains("supportedVersions")
                                   ? json::serialize(res.as_object().at("supportedVersions"))
                                   : std::string("?");
        log_info("OK: server/discover answered; supportedVersions " + versions);
        return EXIT_OK;
    }
    if (v && v->is_object() && v->as_object().contains("error")) {
        const auto &err = v->as_object().at("error");
        log_info("server/discover failed: " + json::serialize(err));
        const json::value *data = err.is_object() ? err.as_object().if_contains("data") : nullptr;
        const bool bridge_err   = data && data->is_object() && data->as_object().contains("source");
        const json::value *reason = bridge_err ? data->as_object().if_contains("reason") : nullptr;
        if (bridge_err && !(reason && reason->is_string() && reason->as_string() == "protocol_mismatch"))
            return EXIT_CHECK_FAILED;
    }
    const std::string init =
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"orca-mcp-bridge --check","version":"1"}}})";
    v = exchange(init);
    if (v && v->is_object() && v->as_object().contains("result")) {
        log_info("OK (legacy): initialize answered: " + json::serialize(v->as_object().at("result")).substr(0, 300));
        return EXIT_OK;
    }
    log_error("the server did not answer server/discover or initialize" +
              (v ? std::string(": ") + json::serialize(*v).substr(0, 400) : std::string()));
    return EXIT_CHECK_FAILED;
}

} // namespace orca_mcp_bridge
