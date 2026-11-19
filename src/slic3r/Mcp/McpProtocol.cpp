#include "McpProtocol.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "McpSchema.hpp"
#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// JSON-RPC and MCP error codes.
const int PARSE_ERROR = -32700;
const int INVALID_REQUEST = -32600;
const int METHOD_NOT_FOUND = -32601;
const int INVALID_PARAMS = -32602;
const int HEADER_MISMATCH = -32020;
const int UNSUPPORTED_PROTOCOL_VERSION = -32022;

const char* const META_VERSION = "io.modelcontextprotocol/protocolVersion";
const char* const META_CLIENT_CAPABILITIES = "io.modelcontextprotocol/clientCapabilities";
const char* const META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";
const char* const META_SUBSCRIPTION_ID = "io.modelcontextprotocol/subscriptionId";

const int LIST_TTL_MS = 3600000;

// Deepest nesting of arrays and objects accepted in a request body. Real requests stay far below;
// json values are copied and destroyed recursively, so a body of thousands of '[' would overflow the
// stack long after parsing.
const int MAX_JSON_DEPTH = 64;

// True when `body` nests arrays / objects deeper than MAX_JSON_DEPTH. Brackets inside strings (with
// their escapes) do not count. A scan of the raw text: runs before anything is parsed.
bool nested_too_deep(const std::string& body)
{
    int  depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : body) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                in_string = false;
            continue;
        }
        if (ch == '"')
            in_string = true;
        else if (ch == '[' || ch == '{') {
            if (++depth > MAX_JSON_DEPTH)
                return true;
        } else if ((ch == ']' || ch == '}') && depth > 0)
            --depth;
    }
    return false;
}

bool contains(const std::vector<std::string>& list, const std::string& value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

json error_body(const json& id, int code, const std::string& message, json data = nullptr)
{
    json error = json::object();
    error["code"] = code;
    error["message"] = message;
    if (!data.is_null())
        error["data"] = std::move(data);
    json out = json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = std::move(error);
    return out;
}

Reply error_reply(int status, const json& id, int code, const std::string& message, json data = nullptr)
{
    Reply reply;
    reply.kind = Reply::Kind::Json;
    reply.status = status;
    reply.body = error_body(id, code, message, std::move(data));
    return reply;
}

Reply result_reply(const json& id, json result)
{
    Reply reply;
    reply.kind = Reply::Kind::Json;
    reply.status = 200;
    reply.body = json::object();
    reply.body["jsonrpc"] = "2.0";
    reply.body["id"] = id;
    reply.body["result"] = std::move(result);
    return reply;
}

json supported_versions()
{
    json out = json::array();
    for (const std::string& v : Protocol::modern_versions())
        out.push_back(v);
    for (const std::string& v : Protocol::legacy_versions())
        out.push_back(v);
    return out;
}

Reply unsupported_version(const json& id, const std::string& requested)
{
    json data = json::object();
    data["supported"] = supported_versions();
    data["requested"] = requested;
    return error_reply(400, id, UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version", std::move(data));
}

// Mcp-Name may carry the value as =?base64?<utf-8 in base64>?= when it is not header-safe.
bool decode_header_value(const std::string& raw, std::string& out)
{
    static const std::string prefix = "=?base64?";
    static const std::string suffix = "?=";
    if (raw.size() >= prefix.size() + suffix.size() && raw.compare(0, prefix.size(), prefix) == 0 &&
        raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0)
        return base64_decode(raw.substr(prefix.size(), raw.size() - prefix.size() - suffix.size()), out);
    out = raw;
    return true;
}

} // namespace

const std::vector<std::string>& Protocol::modern_versions()
{
    static const std::vector<std::string> versions = {"2026-07-28"};
    return versions;
}

const std::vector<std::string>& Protocol::legacy_versions()
{
    // Newest first: the first entry is offered when a legacy client asks for a version we lack.
    static const std::vector<std::string> versions = {"2025-11-25", "2025-06-18", "2025-03-26"};
    return versions;
}

Protocol::Protocol(const ToolRegistry& registry, ServerInfo info) : m_registry(registry), m_info(std::move(info)) {}

json Protocol::server_info_json() const
{
    json info = json::object();
    info["name"] = m_info.name;
    info["title"] = m_info.title;
    info["version"] = m_info.version;
    return info;
}

void Protocol::finish_modern(json& result) const
{
    result["resultType"] = "complete";
    json meta = json::object();
    meta[META_SERVER_INFO] = server_info_json();
    result["_meta"] = std::move(meta);
}

std::string Protocol::call_key(const json& id)
{
    // "1" (number) and "\"1\"" (string) are different ids in JSON-RPC.
    return id.dump();
}

json Protocol::transport_error(int code, const std::string& message)
{
    return error_body(nullptr, code, message);
}

json Protocol::progress_notification(const json& token, double percent, const std::string& text)
{
    json params = json::object();
    params["progressToken"] = token;
    params["progress"] = std::round(std::max(0., std::min(100., percent)) * 10.) / 10.;
    params["total"] = 100;
    if (!text.empty())
        params["message"] = text;
    json out = json::object();
    out["jsonrpc"] = "2.0";
    out["method"] = "notifications/progress";
    out["params"] = std::move(params);
    return out;
}

json Protocol::listen_acknowledgement(const json& id)
{
    json meta = json::object();
    meta[META_SUBSCRIPTION_ID] = id;
    json params = json::object();
    params["_meta"] = std::move(meta);
    // The tool list never changes while the server runs and there are no resources or prompts, so
    // no notification type is honoured.
    params["notifications"] = json::object();
    json out = json::object();
    out["jsonrpc"] = "2.0";
    out["method"] = "notifications/subscriptions/acknowledged";
    out["params"] = std::move(params);
    return out;
}

json Protocol::listen_closed(const json& id) const
{
    json result = json::object();
    result["resultType"] = "complete";
    json meta = json::object();
    meta[META_SUBSCRIPTION_ID] = id;
    meta[META_SERVER_INFO] = server_info_json();
    result["_meta"] = std::move(meta);
    json out = json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}

json Protocol::tool_response(const json& id, bool modern, const CallResult& call_result) const
{
    json content = json::array();
    {
        json block = json::object();
        block["type"] = "text";
        block["text"] = call_result.text;
        content.push_back(std::move(block));
    }
    for (const Image& image : call_result.images) {
        json block = json::object();
        block["type"] = "image";
        block["data"] = base64_encode(image.bytes);
        block["mimeType"] = image.mime_type;
        content.push_back(std::move(block));
    }
    json result = json::object();
    result["content"] = std::move(content);
    result["isError"] = call_result.is_error;
    if (modern)
        finish_modern(result);
    json out = json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}

Reply Protocol::handle(const RequestHead& head, const std::string& body) const
{
    json message;
    if (nested_too_deep(body))
        return error_reply(400, nullptr, PARSE_ERROR,
                           "Parse error: the request body nests arrays or objects deeper than " + std::to_string(MAX_JSON_DEPTH) + " levels");
    try {
        message = json::parse(body);
    } catch (const std::exception&) {
        return error_reply(400, nullptr, PARSE_ERROR, "Parse error: the request body is not valid JSON");
    }
    if (message.is_array())
        return error_reply(400, nullptr, INVALID_REQUEST, "JSON-RPC batches are not supported: send one message per POST");
    if (!message.is_object() || message.value("jsonrpc", json()) != json("2.0"))
        return error_reply(400, nullptr, INVALID_REQUEST, "Invalid request: expected a JSON-RPC 2.0 object");

    const auto method_it = message.find("method");
    if (method_it == message.end()) {
        // A response to a server request. This server never sends requests; accept and ignore.
        if (message.contains("result") || message.contains("error")) {
            Reply reply;
            reply.kind = Reply::Kind::Accepted;
            return reply;
        }
        return error_reply(400, nullptr, INVALID_REQUEST, "Invalid request: no method");
    }
    if (!method_it->is_string())
        return error_reply(400, nullptr, INVALID_REQUEST, "Invalid request: method must be a string");
    const std::string method = method_it->get<std::string>();

    const bool is_notification = !message.contains("id");
    json id;
    if (!is_notification) {
        id = message["id"];
        if (!id.is_string() && !id.is_number())
            return error_reply(400, nullptr, INVALID_REQUEST, "Invalid request: id must be a string or a number");
    }

    const json params = message.contains("params") ? message["params"] : json(json::object());
    if (!params.is_object())
        return error_reply(400, id, INVALID_PARAMS, "Invalid params: params must be an object");
    const json meta = (params.contains("_meta") && params["_meta"].is_object()) ? params["_meta"] : json(json::object());

    // --- notifications (either era) ------------------------------------------------------------
    if (is_notification) {
        Reply reply;
        reply.kind = Reply::Kind::Accepted;
        reply.method = method;
        if (method == "notifications/cancelled" && params.contains("requestId"))
            reply.cancel_key = call_key(params["requestId"]);
        return reply;
    }

    // --- which era ------------------------------------------------------------------------------
    const bool        has_body_version = meta.contains(META_VERSION) && meta[META_VERSION].is_string();
    const std::string body_version = has_body_version ? meta[META_VERSION].get<std::string>() : std::string();
    bool modern = false;
    if (has_body_version) {
        if (contains(modern_versions(), body_version))
            modern = true;
        else if (!contains(legacy_versions(), body_version))
            return unsupported_version(id, body_version);
        // A legacy version in _meta: served with legacy semantics.
    } else if (method != "initialize" && head.has_version) {
        if (contains(modern_versions(), head.version))
            return error_reply(400, id, INVALID_PARAMS,
                               std::string("Invalid params: params._meta must carry \"") + META_VERSION + "\" and \"" + META_CLIENT_CAPABILITIES + "\"");
        if (!contains(legacy_versions(), head.version))
            return unsupported_version(id, head.version);
    } else if (method == "server/discover" || method == "subscriptions/listen") {
        return error_reply(400, id, INVALID_PARAMS,
                           std::string("Invalid params: params._meta must carry \"") + META_VERSION + "\" and \"" + META_CLIENT_CAPABILITIES + "\"");
    }

    // --- modern: mirrored headers and required metadata -------------------------------------------
    if (modern) {
        if (!head.has_version)
            return error_reply(400, id, HEADER_MISMATCH, "Header mismatch: the MCP-Protocol-Version header is missing");
        if (head.version != body_version)
            return error_reply(400, id, HEADER_MISMATCH,
                               "Header mismatch: MCP-Protocol-Version header value '" + head.version + "' does not match body value '" + body_version + "'");
        if (!head.has_method)
            return error_reply(400, id, HEADER_MISMATCH, "Header mismatch: the Mcp-Method header is missing");
        if (head.method != method)
            return error_reply(400, id, HEADER_MISMATCH,
                               "Header mismatch: Mcp-Method header value '" + head.method + "' does not match body value '" + method + "'");
        if (method == "tools/call") {
            const std::string body_name = (params.contains("name") && params["name"].is_string()) ? params["name"].get<std::string>() : std::string();
            std::string header_name;
            if (!head.has_name)
                return error_reply(400, id, HEADER_MISMATCH, "Header mismatch: the Mcp-Name header is missing");
            if (!decode_header_value(head.name, header_name))
                return error_reply(400, id, HEADER_MISMATCH, "Header mismatch: the Mcp-Name header is not valid base64");
            if (header_name != body_name)
                return error_reply(400, id, HEADER_MISMATCH,
                                   "Header mismatch: Mcp-Name header value '" + header_name + "' does not match body value '" + body_name + "'");
        }
        if (!meta.contains(META_CLIENT_CAPABILITIES) || !meta[META_CLIENT_CAPABILITIES].is_object())
            return error_reply(400, id, INVALID_PARAMS, std::string("Invalid params: params._meta must carry \"") + META_CLIENT_CAPABILITIES + "\"");
    }

    // --- methods ----------------------------------------------------------------------------------
    if (modern && method == "server/discover") {
        json result = json::object();
        result["supportedVersions"] = supported_versions();
        result["capabilities"] = json::object({{"tools", json::object()}});
        result["instructions"] = m_info.instructions;
        result["ttlMs"] = LIST_TTL_MS;
        result["cacheScope"] = "private";
        finish_modern(result);
        Reply reply = result_reply(id, std::move(result));
        reply.method = method;
        return reply;
    }

    if (modern && method == "subscriptions/listen") {
        Reply reply;
        reply.kind = Reply::Kind::Listen;
        reply.id = id;
        reply.modern = true;
        reply.method = method;
        return reply;
    }

    if (!modern && method == "initialize") {
        std::string requested;
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
            requested = params["protocolVersion"].get<std::string>();
        json result = json::object();
        // The requested version when we speak it, otherwise our newest legacy version; the client
        // disconnects if it cannot use that.
        result["protocolVersion"] = contains(legacy_versions(), requested) ? requested : legacy_versions().front();
        result["capabilities"] = json::object({{"tools", json::object({{"listChanged", false}})}});
        result["serverInfo"] = server_info_json();
        result["instructions"] = m_info.instructions;
        Reply reply = result_reply(id, std::move(result));
        reply.method = method;
        return reply;
    }

    if (!modern && method == "ping") {
        Reply reply = result_reply(id, json::object());
        reply.method = method;
        return reply;
    }

    if (method == "tools/list") {
        json result = json::object();
        result["tools"] = m_registry.list_json();
        if (modern) {
            result["ttlMs"] = LIST_TTL_MS;
            result["cacheScope"] = "private";
            finish_modern(result);
        }
        Reply reply = result_reply(id, std::move(result));
        reply.method = method;
        return reply;
    }

    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string())
            return error_reply(modern ? 400 : 200, id, INVALID_PARAMS, "Invalid params: tools/call needs params.name");
        const std::string name = params["name"].get<std::string>();
        const ToolDef*    tool = m_registry.find(name);
        if (tool == nullptr)
            return error_reply(modern ? 400 : 200, id, INVALID_PARAMS, "Unknown tool: " + name + ". Call tools/list for the tools this server has.");
        json args = params.contains("arguments") && !params["arguments"].is_null() ? params["arguments"] : json(json::object());
        if (!args.is_object())
            return error_reply(modern ? 400 : 200, id, INVALID_PARAMS, "Invalid params: tools/call arguments must be an object");

        if (const std::string problem = Schema::validate(tool->input_schema, args); !problem.empty()) {
            // A mistake the model can fix: a tool result with isError, not a protocol error.
            const ToolError error("invalid_argument", problem, "Fix the argument and call " + name + " again. The tool's input schema lists every argument.");
            Reply reply;
            reply.kind = Reply::Kind::Json;
            reply.status = 200;
            reply.body = tool_response(id, modern, make_error_result(error));
            reply.method = method + " " + name + " (invalid_argument)";
            return reply;
        }

        Reply reply;
        reply.kind = Reply::Kind::CallTool;
        reply.id = id;
        reply.modern = modern;
        reply.tool = tool;
        reply.args = std::move(args);
        reply.method = method + " " + name;
        if (meta.contains("progressToken") && (meta["progressToken"].is_string() || meta["progressToken"].is_number())) {
            reply.progress_token = meta["progressToken"];
            reply.stream = head.accepts_event_stream;
        }
        return reply;
    }

    // HTTP 404 identifies an unknown method to a modern client; legacy clients expect the JSON-RPC
    // error in a normal response.
    return error_reply(modern ? 404 : 200, id, METHOD_NOT_FOUND, "Method not found: " + method);
}

}}} // namespace Slic3r::GUI::Mcp
