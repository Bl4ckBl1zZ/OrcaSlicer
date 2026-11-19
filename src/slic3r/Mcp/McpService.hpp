#ifndef slic3r_Mcp_Service_hpp_
#define slic3r_Mcp_Service_hpp_

// The MCP server as the application sees it: started when the app is ready (if enabled), stopped
// at shutdown, restarted when its preferences change.
//
// Preferences (app_config, section "app"):
//   mcp_enabled   "true" / "false"   off by default
//   mcp_port      TCP port on 127.0.0.1, default 13720
// The access token lives in <data_dir>/mcp/token. While the server runs, <data_dir>/mcp/endpoint.json
// holds {"schema": 1, "url", "port", "token", "pid", "protocolVersions", "app", "appVersion"} for
// local helpers (the stdio bridge) and tests. It is written through a temporary file and a rename,
// readable by the user only, rewritten when the port or the token changes, and removed when the
// server stops.
//
// Environment overrides, read once at start-up, for automated test launches:
//   ORCA_MCP_ENABLE=1|0        force the server on or off regardless of preferences
//   ORCA_MCP_PORT=<n>          port; 0 picks any free port (reported in endpoint.json)
//   ORCA_MCP_TOKEN=<token>     use this token instead of the stored one (not saved)
//   ORCA_MCP_DEBUG_TOOLS=1     also register the debug_* tools used by the test-suite
//   ORCA_MCP_ENDPOINT_FILE=<path>   write a second copy of endpoint.json there
//   ORCA_MCP_WINDOW=minimized  minimise the main window once the application is up
//
// Main thread only, except recent_activity() and status(), which may be called from any thread.

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

class DialogGuard;
class Dispatcher;
class HttpServer;
class Protocol;
class ToolRegistry;

struct ServiceStatus
{
    bool        enabled { false };
    bool        running { false };
    uint16_t    port { 0 };
    std::string url;   // http://127.0.0.1:<port>/mcp when running
    std::string error; // why the server is enabled but not running
};

struct ActivityRecord
{
    std::string time;    // HH:MM:SS local
    std::string what;
    std::string outcome;
    int         duration_ms { 0 };
};

class Service
{
public:
    static Service& instance();

    // Called once the main window exists and the event loop runs. Idempotent.
    void on_app_ready();
    // Called at application shutdown, before windows are destroyed. Idempotent.
    void stop();
    // Re-reads the preferences and starts, stops or restarts the server accordingly.
    void apply_preferences();

    ServiceStatus status() const;
    // The token clients must present; created on first use.
    std::string   token();
    // Replaces the token and restarts the server if it is running.
    void          regenerate_token();
    // `claude mcp add ...` for the running configuration.
    std::string   claude_code_command();
    std::string   mcp_json_snippet();
    std::vector<ActivityRecord> recent_activity() const;

    // Parts, valid while running (nullptr otherwise).
    Dispatcher*         dispatcher() const { return m_dispatcher.get(); }
    DialogGuard*        dialog_guard() const { return m_guard.get(); }
    const ToolRegistry* registry() const { return m_registry.get(); }
    bool                debug_tools() const { return m_debug_tools; }

    static const int DEFAULT_PORT = 13720;

private:
    Service();
    ~Service();

    bool start();
    void write_endpoint_files();
    void remove_endpoint_files();
    void record(const std::string& what, const std::string& outcome, int duration_ms);

    bool m_ready { false };
    bool m_env_read { false };
    // Environment overrides.
    int         m_env_enable { -1 }; // -1 = not set
    int         m_env_port { -1 };
    std::string m_env_token;
    std::string m_env_endpoint_file;
    bool        m_debug_tools { false };

    std::unique_ptr<ToolRegistry> m_registry;
    std::unique_ptr<Protocol>     m_protocol;
    std::unique_ptr<DialogGuard>  m_guard;
    std::unique_ptr<Dispatcher>   m_dispatcher;
    std::unique_ptr<HttpServer>   m_server;
    int                           m_running_port_setting { -1 };

    mutable std::mutex         m_mutex; // guards m_status and m_activity
    ServiceStatus              m_status;
    std::deque<ActivityRecord> m_activity;
};

// The `instructions` text sent to clients (McpInstructions.cpp).
const char* server_instructions();

}}} // namespace Slic3r::GUI::Mcp

#endif
