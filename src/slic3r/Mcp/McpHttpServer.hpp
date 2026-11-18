#ifndef slic3r_Mcp_HttpServer_hpp_
#define slic3r_Mcp_HttpServer_hpp_

// Streamable HTTP endpoint for MCP: POST /mcp on 127.0.0.1, one network thread that never waits
// for the GUI. Checks, in this order: path, Host, Origin, bearer token, method, content type,
// rate, body size; then hands the body to Protocol.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "McpCall.hpp"
#include "McpProtocol.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

struct HttpServerConfig
{
    // 0 = let the system pick a free port (the port in use is then reported by HttpServer::port()).
    uint16_t                 port { 13720 };
    // Required as "Authorization: Bearer <token>" on every request. Never empty.
    std::string              token;
    // Requests carrying an Origin header are rejected unless the origin is listed here.
    std::vector<std::string> allowed_origins;
    size_t                   max_body_bytes { 8 * 1024 * 1024 };
    int                      max_connections { 32 };
    int                      max_requests_per_second { 200 };
};

// One line of the activity log.
struct ActivityEntry
{
    std::string what;    // "tools/call scene_get", "tools/list", "401" ...
    std::string outcome; // "ok", an error code, or an HTTP status
    int         duration_ms { 0 };
};

class HttpServer
{
public:
    struct Hooks
    {
        // Queue a call for the main thread. Called on the network thread; must not block.
        std::function<void(std::shared_ptr<Call>)> submit;
        // Called on the network thread for every answered request.
        std::function<void(const ActivityEntry&)>  activity;
    };

    HttpServer(HttpServerConfig config, const Protocol& protocol, Hooks hooks);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds 127.0.0.1:<port> and starts the network thread. On failure returns false and sets
    // `error` to a sentence for the user ("port 13720 is in use").
    bool     start(std::string& error);
    // Closes the listener and every connection, then joins the thread. Calls still in flight are
    // cancelled; their results are dropped. Safe to call twice.
    void     stop();
    // The port actually bound; valid after start() succeeded.
    uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}}} // namespace Slic3r::GUI::Mcp

#endif
