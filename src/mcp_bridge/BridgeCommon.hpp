// orca-mcp-bridge: stdio <-> Streamable HTTP forwarder for OrcaSlicer's MCP server.
// Declarations shared by the bridge's translation units.
#pragma once

#include <boost/json/value.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orca_mcp_bridge {

constexpr const char *BRIDGE_NAME    = "orca-mcp-bridge";
constexpr const char *BRIDGE_VERSION = "1.0.0";

// ---------------------------------------------------------------------------------------------
// Options (command line and environment)
// ---------------------------------------------------------------------------------------------
struct Options
{
    std::string url;            // --url / ORCA_MCP_URL (or ORCA_MCP_PORT)
    std::string url_source;     // where url came from, for messages
    std::string token;          // --token / ORCA_MCP_TOKEN; never printed
    std::string token_source;
    std::string token_file;     // --token-file / ORCA_MCP_TOKEN_FILE
    std::string endpoint_file;  // --endpoint-file / ORCA_MCP_ENDPOINT_FILE
    std::string datadir;        // --datadir / ORCA_MCP_DATADIR
    std::string log_file;       // --log-file / ORCA_MCP_BRIDGE_LOG
    bool        allow_remote   = false;
    int         verbosity      = 1;     // 0 quiet, 1 normal, 2 verbose (debug)
    double      request_timeout_s = 0;  // 0 = no limit
    double      connect_wait_s    = 0;  // keep retrying a refused connection this long
    double      busy_wait_s       = 60; // keep retrying HTTP 429 this long
    double      eof_grace_s       = 0;  // wait this long for in-flight requests after stdin EOF
    bool        listen_stream     = true; // legacy era: open the optional GET stream
    bool        check_only        = false;
};

// Exit codes.
enum ExitCode : int {
    EXIT_OK            = 0, // stdin reached EOF (or a signal asked us to stop)
    EXIT_INTERNAL      = 1, // unexpected internal failure
    EXIT_USAGE         = 2, // bad command line / refused configuration
    EXIT_STDOUT_CLOSED = 3, // the client stopped reading our stdout
    EXIT_CHECK_FAILED  = 4, // --check could not reach a working server
};

// Bridge-local JSON-RPC error codes. The MCP specification asks implementations to allocate
// their own codes outside the JSON-RPC reserved range (-32768..-32000); these never collide
// with codes a server sends. Each error also carries data.source = "orca-mcp-bridge".
enum BridgeError : int {
    ERR_UNREACHABLE   = -31000, // app not running / server disabled / connection lost
    ERR_AUTH          = -31001, // token rejected (401) or request forbidden (403)
    ERR_CONFIG        = -31002, // no usable endpoint configuration
    ERR_TIMEOUT       = -31003, // --request-timeout elapsed, or server busy for too long
    ERR_BAD_RESPONSE  = -31004, // HTTP error without a JSON-RPC body, malformed response
    ERR_NO_RESPONSE   = -31005, // stream ended without the response
};

// ---------------------------------------------------------------------------------------------
// Logging (stderr and optional file). Never pass the token to these.
// ---------------------------------------------------------------------------------------------
void log_init(int verbosity, const std::string &log_file_utf8);
void log_error(const std::string &msg);
void log_info(const std::string &msg);
void log_debug(const std::string &msg);
bool log_debug_enabled();

// ---------------------------------------------------------------------------------------------
// Platform helpers (Platform.cpp)
// ---------------------------------------------------------------------------------------------
void        stdio_init_binary();
// Blocking read from stdin; returns bytes read, 0 at EOF, <0 on error.
long        stdin_read(char *buf, std::size_t len);
// Blocking write of the whole buffer to stdout; false when stdout is closed/broken.
bool        stdout_write_all(const char *data, std::size_t len);
std::optional<std::string> get_env(const char *name);       // UTF-8
bool        read_file_utf8_path(const std::string &path, std::string &out, std::string &err);
// Returns modification time + size as an opaque stamp, or nullopt if the file does not exist.
std::optional<std::string> file_stamp(const std::string &path);
bool        file_exists(const std::string &path);
std::string executable_dir();                                // directory of this executable
std::vector<std::string> default_data_dirs();                // candidates, most specific first
// true: alive, false: definitely gone, nullopt: cannot tell
std::optional<bool> process_alive(long long pid);
std::string join_path(const std::string &a, const std::string &b);

// ---------------------------------------------------------------------------------------------
// URL and endpoint discovery (Endpoint.cpp)
// ---------------------------------------------------------------------------------------------
struct Url
{
    std::string scheme;      // "http"
    std::string host;        // without brackets
    std::string port;        // decimal
    std::string target;      // path + query, at least "/"
    std::string host_header; // value for the Host header (host:port, IPv6 bracketed)
    std::string text;        // normalised URL for messages
};

bool parse_url(const std::string &text, Url &out, std::string &err);
bool is_loopback_host(const std::string &host);

struct Endpoint
{
    Url         url;
    std::string token;          // may be empty (then no Authorization header is sent)
    std::string url_source;     // "--url", "ORCA_MCP_URL", "endpoint file <path>"...
    std::string token_source;
    std::string endpoint_file;  // file it came from, if any
    std::optional<long long> pid;
    std::vector<std::string> versions;
};

struct ResolveResult
{
    std::optional<Endpoint> endpoint;
    std::string error;          // human readable, actionable
    std::string reason;         // short machine-readable reason
};

class EndpointResolver
{
public:
    explicit EndpointResolver(const Options &opts);
    // Returns the cached endpoint unless the endpoint file changed or `force` is set.
    ResolveResult resolve(bool force);
    // Describes why `ep` cannot be reached (connection refused), using the endpoint file's pid.
    std::string   describe_refused(const Endpoint &ep, std::string &reason);
    // Validates static configuration at startup (bad --url, non-loopback, unreadable token file).
    bool          validate_static(std::string &err) const;
    std::vector<std::string> endpoint_file_candidates() const;

private:
    ResolveResult resolve_uncached();
    const Options &m_opts;
    std::optional<std::string> m_stamp;     // stamp of the file the cache was built from
    std::string                m_stamp_path;
    std::optional<ResolveResult> m_cache;
};

// ---------------------------------------------------------------------------------------------
// MCP helpers (McpHeaders.cpp)
// ---------------------------------------------------------------------------------------------
constexpr const char *META_PROTOCOL_VERSION = "io.modelcontextprotocol/protocolVersion";

// Encodes a value for Mcp-Name / Mcp-Param-*: plain when header-safe, otherwise the
// "=?base64?...?=" sentinel form of the UTF-8 bytes.
std::string encode_header_value(const std::string &utf8);
bool        is_header_token(const std::string &s);       // RFC 9110 tchar+
bool        is_safe_header_value(const std::string &s);  // visible ASCII + SP/HTAB, no edge ws

struct ParamHeader
{
    std::vector<std::string> path; // chain of property names
    std::string              name; // the x-mcp-header value
};

// Registry of x-mcp-header annotations learned from tools/list results.
class ToolHeaderRegistry
{
public:
    // Inspects a tools/list result. Returns the names of tools whose annotations are invalid
    // (the caller removes them from the result, as the specification requires of HTTP clients).
    std::vector<std::string> learn(const boost::json::object &result);
    // Mcp-Param-* headers for a tools/call with these params.
    std::vector<std::pair<std::string, std::string>> headers_for(const boost::json::object &params) const;

private:
    std::map<std::string, std::vector<ParamHeader>> m_tools;
};

// Remove characters that may not appear inside a stdio frame (raw CR/LF). In valid JSON
// these can only be insignificant whitespace, so replacing them by spaces preserves meaning.
std::string to_single_line(std::string text);

// ---------------------------------------------------------------------------------------------
// SSE parser (SseParser.cpp) — WHATWG event-stream interpretation.
// ---------------------------------------------------------------------------------------------
class SseParser
{
public:
    struct Event
    {
        std::string type;  // "" means "message"
        std::string data;
        std::string id;    // last event id at dispatch time
    };
    // Feeds bytes; complete events are appended to `out`.
    void feed(const char *data, std::size_t len, std::vector<Event> &out);
    const std::string &last_event_id() const { return m_last_id; }
    std::optional<long> retry_ms() const { return m_retry; }

private:
    void line(const std::string &l, std::vector<Event> &out);
    std::string m_line;
    std::string m_data;
    std::string m_type;
    std::string m_last_id;
    std::optional<long> m_retry;
    bool m_have_data = false;
    bool m_last_was_cr = false;
    bool m_bom_checked = false;
};

// ---------------------------------------------------------------------------------------------
// Bridge (Bridge.cpp)
// ---------------------------------------------------------------------------------------------
int run_bridge(const Options &opts);
int run_check(const Options &opts);

} // namespace orca_mcp_bridge
