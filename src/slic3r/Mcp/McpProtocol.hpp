#ifndef slic3r_Mcp_Protocol_hpp_
#define slic3r_Mcp_Protocol_hpp_

// JSON-RPC and MCP protocol logic for both protocol eras:
//   modern  2026-07-28: stateless, per-request _meta, mirrored HTTP headers, server/discover;
//   legacy  2025-11-25 / 2025-06-18 / 2025-03-26: initialize handshake (no session id is issued).
// Pure functions of the request: no sockets, no wx, no per-connection state. Runs on the network
// thread; unit-testable.

#include <string>
#include <vector>

#include "McpCall.hpp"
#include "McpJson.hpp"
#include "McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// The HTTP headers the protocol layer looks at.
struct RequestHead
{
    bool        has_version { false };
    std::string version; // MCP-Protocol-Version
    bool        has_method { false };
    std::string method;  // Mcp-Method
    bool        has_name { false };
    std::string name;    // Mcp-Name, possibly in the =?base64?...?= form
    bool        accepts_event_stream { false };
};

struct ServerInfo
{
    std::string name;    // "orcaslicer"
    std::string title;   // "OrcaSlicer"
    std::string version; // application version
    std::string instructions;
};

struct Reply
{
    enum class Kind {
        Json,     // answer now with `status` and `body`
        Accepted, // notification or client response: HTTP 202, no body
        CallTool, // run `tool` with `args` on the main thread, then answer with tool_response()
        Listen    // subscriptions/listen: acknowledge on an event stream and keep it open
    };
    Kind kind { Kind::Json };
    int  status { 200 };
    json body;

    // Kind::CallTool and Kind::Listen
    json           id;
    bool           modern { false };
    // Kind::CallTool
    const ToolDef* tool { nullptr };
    json           args;
    bool           stream { false }; // answer on an event stream with progress notifications
    json           progress_token;
    // Kind::Accepted for notifications/cancelled: the request to cancel (see Protocol::call_key).
    std::string    cancel_key;
    // For the log.
    std::string    method;
};

class Protocol
{
public:
    Protocol(const ToolRegistry& registry, ServerInfo info);

    Reply handle(const RequestHead& head, const std::string& body) const;

    // The JSON-RPC response carrying a tool result.
    json tool_response(const json& id, bool modern, const CallResult& result) const;
    // notifications/progress for an event stream.
    static json progress_notification(const json& token, double percent, const std::string& text);
    // First message of a subscriptions/listen stream, and the result that closes it gracefully.
    static json listen_acknowledgement(const json& id);
    json        listen_closed(const json& id) const;
    // A JSON-RPC error with no id, for failures before the body was understood.
    static json transport_error(int code, const std::string& message);

    // Identifies an in-flight request for notifications/cancelled.
    static std::string call_key(const json& id);

    static const std::vector<std::string>& modern_versions();
    static const std::vector<std::string>& legacy_versions();

private:
    json server_info_json() const;
    void finish_modern(json& result) const;

    const ToolRegistry& m_registry;
    ServerInfo          m_info;
};

}}} // namespace Slic3r::GUI::Mcp

#endif
