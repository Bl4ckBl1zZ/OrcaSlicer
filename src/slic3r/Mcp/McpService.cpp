#include "McpService.hpp"

#include <cstdlib>
#include <ctime>
#include <exception>
#include <random>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/rand.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"

#include "McpDialogGuard.hpp"
#include "McpDispatcher.hpp"
#include "McpHttpServer.hpp"
#include "McpJobs.hpp"
#include "McpJson.hpp"
#include "McpProtocol.hpp"
#include "McpTool.hpp"
#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

namespace {

const size_t MAX_ACTIVITY = 50;

fs::path mcp_dir() { return fs::path(Slic3r::data_dir()) / "mcp"; }
fs::path token_file() { return mcp_dir() / "token"; }
fs::path endpoint_file() { return mcp_dir() / "endpoint.json"; }

std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

void restrict_to_owner(const fs::path& path)
{
#ifndef _WIN32
    boost::system::error_code ec;
    fs::permissions(path, fs::owner_read | fs::owner_write, ec);
#else
    // The data directory sits in the user's profile, whose ACL already limits it to that user.
    (void) path;
#endif
}

bool write_private_file(const fs::path& path, const std::string& content)
{
    boost::system::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = fs::path(path.string() + ".tmp");
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << content;
    }
    restrict_to_owner(tmp);
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string read_file(const fs::path& path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    std::string             content;
    if (in)
        std::getline(in, content);
    while (!content.empty() && (content.back() == '\r' || content.back() == '\n' || content.back() == ' '))
        content.pop_back();
    return content;
}

std::string new_token()
{
    unsigned char bytes[32];
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) {
        std::random_device device;
        for (unsigned char& b : bytes)
            b = static_cast<unsigned char>(device());
    }
    return base64_encode(std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes)), true);
}

std::string now_hms()
{
    // Called from the network thread: use the re-entrant variants.
    const std::time_t t = std::time(nullptr);
    std::tm           local {};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[16] = "";
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

} // namespace

Service& Service::instance()
{
    static Service service;
    return service;
}

Service::Service() = default;
Service::~Service() = default;

void Service::on_app_ready()
{
    if (m_ready)
        return;
    m_ready = true;
    if (!m_env_read) {
        m_env_read = true;
        const std::string enable = env("ORCA_MCP_ENABLE");
        if (!enable.empty())
            m_env_enable = (enable == "1" || enable == "true") ? 1 : 0;
        const std::string port = env("ORCA_MCP_PORT");
        if (!port.empty()) {
            const int value = std::atoi(port.c_str());
            if (value >= 0 && value <= 65535)
                m_env_port = value;
        }
        m_env_token = env("ORCA_MCP_TOKEN");
        m_env_endpoint_file = env("ORCA_MCP_ENDPOINT_FILE");
        const std::string debug = env("ORCA_MCP_DEBUG_TOOLS");
        m_debug_tools = debug == "1" || debug == "true";
        // Test launches on a desktop somebody is using: get the window out of the way. By now
        // OpenGL is initialised, and offscreen rendering does not need the window on screen.
        if (env("ORCA_MCP_WINDOW") == "minimized" && wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->Iconize(true);
    }
    apply_preferences();
}

void Service::apply_preferences()
{
    if (!m_ready)
        return;
    AppConfig* config = wxGetApp().app_config;
    const bool enabled = m_env_enable >= 0 ? m_env_enable == 1 : (config != nullptr && config->get_bool("mcp_enabled"));
    int        port = DEFAULT_PORT;
    if (m_env_port >= 0)
        port = m_env_port;
    else if (config != nullptr) {
        const int value = std::atoi(config->get("mcp_port").c_str());
        if (value > 0 && value <= 65535)
            port = value;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.enabled = enabled;
    }
    const bool running = m_server != nullptr;
    if (running && (!enabled || port != m_running_port_setting))
        stop();
    if (enabled && m_server == nullptr) {
        m_running_port_setting = port;
        start();
    }
    if (!enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.error.clear();
    }
}

std::string Service::token()
{
    if (!m_env_token.empty())
        return m_env_token;
    std::string stored = read_file(token_file());
    if (stored.size() < 16) {
        stored = new_token();
        if (!write_private_file(token_file(), stored + "\n"))
            BOOST_LOG_TRIVIAL(error) << "MCP: cannot store the access token in " << token_file().string();
    }
    return stored;
}

void Service::regenerate_token()
{
    if (!write_private_file(token_file(), new_token() + "\n"))
        BOOST_LOG_TRIVIAL(error) << "MCP: cannot store the access token in " << token_file().string();
    if (m_server != nullptr) {
        stop();
        apply_preferences();
    }
}

bool Service::start()
{
    std::string error;
    try {
        m_registry = ToolRegistry::create(m_debug_tools);

        ServerInfo info;
        info.name = "orcaslicer";
        info.title = SLIC3R_APP_NAME;
        info.version = SoftFever_VERSION;
        info.instructions = server_instructions();
        m_protocol = std::make_unique<Protocol>(*m_registry, std::move(info));

        m_guard = std::make_unique<DialogGuard>();
        m_guard->install();
        Jobs::instance().set_guard(m_guard.get());
        m_dispatcher = std::make_unique<Dispatcher>(*m_guard);

        HttpServerConfig config;
        config.port = static_cast<uint16_t>(m_running_port_setting);
        config.token = token();

        HttpServer::Hooks hooks;
        Dispatcher*       dispatcher = m_dispatcher.get();
        hooks.submit = [dispatcher](std::shared_ptr<Call> call) { dispatcher->submit(std::move(call)); };
        hooks.activity = [this](const ActivityEntry& entry) { record(entry.what, entry.outcome, entry.duration_ms); };

        m_server = std::make_unique<HttpServer>(std::move(config), *m_protocol, std::move(hooks));
        if (!m_server->start(error))
            m_server.reset();
    } catch (const std::exception& e) {
        error = e.what();
        m_server.reset();
    }

    if (m_server == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "MCP: server not started: " << error;
        if (m_dispatcher)
            m_dispatcher->shutdown();
        m_dispatcher.reset();
        Jobs::instance().set_guard(nullptr);
        m_guard.reset();
        m_protocol.reset();
        m_registry.reset();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.running = false;
        m_status.port = 0;
        m_status.url.clear();
        m_status.error = error;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.running = true;
        m_status.port = m_server->port();
        m_status.url = "http://127.0.0.1:" + std::to_string(m_server->port()) + "/mcp";
        m_status.error.clear();
    }
    write_endpoint_files();
    BOOST_LOG_TRIVIAL(info) << "MCP: listening on " << status().url << " with " << m_registry->tools().size() << " tools"
                            << (m_debug_tools ? " (debug tools on)" : "");
    return true;
}

void Service::stop()
{
    if (m_server == nullptr)
        return;
    // Listener first: after this no call can be submitted and none can complete on the network.
    m_server->stop();
    if (m_dispatcher)
        m_dispatcher->shutdown();
    Jobs::instance().clear();
    Jobs::instance().set_guard(nullptr);
    m_server.reset();
    m_dispatcher.reset();
    if (m_guard)
        m_guard->uninstall();
    m_guard.reset();
    m_protocol.reset();
    m_registry.reset();
    remove_endpoint_files();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.running = false;
        m_status.port = 0;
        m_status.url.clear();
    }
    BOOST_LOG_TRIVIAL(info) << "MCP: stopped";
}

void Service::write_endpoint_files()
{
    const ServiceStatus current = status();
    // The format the stdio bridge (orca-mcp-bridge) and the test harness read. "schema" is bumped
    // when a member changes meaning; members may be added without a bump.
    json versions = json::array();
    for (const std::string& version : Protocol::modern_versions())
        versions.push_back(version);
    for (const std::string& version : Protocol::legacy_versions())
        versions.push_back(version);
    json endpoint = json::object();
    endpoint["schema"] = 1;
    endpoint["url"] = current.url;
    endpoint["port"] = current.port;
    endpoint["token"] = token();
    endpoint["pid"] = Slic3r::get_current_pid();
    endpoint["protocolVersions"] = std::move(versions);
    endpoint["app"] = SLIC3R_APP_NAME;
    endpoint["appVersion"] = SoftFever_VERSION;
    const std::string text = endpoint.dump(2) + "\n";
    if (!write_private_file(endpoint_file(), text))
        BOOST_LOG_TRIVIAL(error) << "MCP: cannot write " << endpoint_file().string();
    if (!m_env_endpoint_file.empty() && !write_private_file(fs::path(m_env_endpoint_file), text))
        BOOST_LOG_TRIVIAL(error) << "MCP: cannot write " << m_env_endpoint_file;
}

void Service::remove_endpoint_files()
{
    boost::system::error_code ec;
    fs::remove(endpoint_file(), ec);
    if (!m_env_endpoint_file.empty())
        fs::remove(fs::path(m_env_endpoint_file), ec);
}

ServiceStatus Service::status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

std::string Service::claude_code_command()
{
    const ServiceStatus current = status();
    const int           port = current.running ? current.port : (m_running_port_setting > 0 ? m_running_port_setting : DEFAULT_PORT);
    return "claude mcp add --transport http orcaslicer http://127.0.0.1:" + std::to_string(port) + "/mcp --header \"Authorization: Bearer " + token() + "\"";
}

std::string Service::mcp_json_snippet()
{
    const ServiceStatus current = status();
    const int           port = current.running ? current.port : (m_running_port_setting > 0 ? m_running_port_setting : DEFAULT_PORT);
    json server = json::object();
    server["type"] = "http";
    server["url"] = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    server["headers"] = json::object({{"Authorization", "Bearer " + token()}});
    json servers = json::object();
    servers["orcaslicer"] = std::move(server);
    json root = json::object();
    root["mcpServers"] = std::move(servers);
    return root.dump(2);
}

void Service::record(const std::string& what, const std::string& outcome, int duration_ms)
{
    BOOST_LOG_TRIVIAL(info) << "MCP: " << what << " -> " << outcome << " (" << duration_ms << " ms)";
    std::lock_guard<std::mutex> lock(m_mutex);
    m_activity.push_back(ActivityRecord { now_hms(), what, outcome, duration_ms });
    while (m_activity.size() > MAX_ACTIVITY)
        m_activity.pop_front();
}

std::vector<ActivityRecord> Service::recent_activity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<ActivityRecord>(m_activity.begin(), m_activity.end());
}

}}} // namespace Slic3r::GUI::Mcp
