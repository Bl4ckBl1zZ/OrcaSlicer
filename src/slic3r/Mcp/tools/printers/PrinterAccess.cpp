#include "Printers.hpp"

#include <cstdint>
#include <cstdlib>
#include <random>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "../../McpError.hpp"
#include "../../McpJournal.hpp"
#include "../../McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Printers {

namespace {

bool pref(const char* key)
{
    const AppConfig* config = wxGetApp().app_config;
    return config != nullptr && config->get_bool(key);
}

void refuse(ToolContext& ctx, const char* key, const std::string& what, const std::string& label)
{
    json details = json::object({{"preference", key}, {"tool", ctx.tool().name}});
    audit(ctx, "refused", "", json::object({{"reason", "printer_access_disabled"}, {"preference", key}}));
    fail("printer_access_disabled",
         "The user has not allowed AI agents to " + what + ". This is off by default and only the user can switch it on.",
         "Ask the user to switch on \"" + label + "\" in Preferences, MCP server section, if they want this. Do not try to change it "
         "yourself: no tool can.",
         std::move(details));
}

// 64-bit FNV-1a. Not a cryptographic hash; the salt is what keeps the serial out of reach.
uint64_t fnv1a(const std::string& text)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

const std::string& salt()
{
    static std::string value;
    if (!value.empty())
        return value;
    namespace fs = boost::filesystem;
    const fs::path dir  = fs::path(data_dir()) / "mcp";
    const fs::path file = dir / "printer_salt";
    {
        boost::nowide::ifstream in(file.string());
        std::string             line;
        if (in && std::getline(in, line) && line.size() >= 16)
            value = line;
    }
    if (value.empty()) {
        std::random_device rd;
        std::ostringstream ss;
        ss << std::hex;
        for (int i = 0; i < 4; ++i)
            ss << static_cast<uint32_t>(rd());
        value = ss.str();
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        boost::nowide::ofstream out(file.string(), std::ios::trunc);
        if (out)
            out << value << "\n";
        else
            BOOST_LOG_TRIVIAL(warning) << "MCP: cannot store the printer id salt; printer ids change on restart";
    }
    return value;
}

} // namespace

bool read_allowed() { return pref(PREF_READ); }
bool send_allowed() { return pref(PREF_SEND); }

void require_read(ToolContext& ctx)
{
    if (!read_allowed())
        refuse(ctx, PREF_READ, "read printer status", "Allow AI agents to read printer status");
}

void require_send(ToolContext& ctx)
{
    if (!send_allowed())
        refuse(ctx, PREF_SEND, "send print jobs or control printers", "Allow AI agents to send print jobs");
}

std::string make_id(const std::string& prefix, const std::string& key)
{
    const uint64_t     h = fnv1a(salt() + "|" + prefix + "|" + key);
    std::ostringstream ss;
    ss << std::hex;
    ss.width(16);
    ss.fill('0');
    ss << h;
    return prefix + "_" + ss.str().substr(0, 10);
}

void audit(ToolContext& ctx, const std::string& action, const std::string& printer_id, json details)
{
    json scope = json::object({{"printer", printer_id.empty() ? json(nullptr) : json(printer_id)}});
    ctx.journal(std::move(scope), action, nullptr, std::move(details), {});
}

void audit_later(const std::string& tool, const std::string& action, const std::string& printer_id, json details)
{
    json scope = json::object({{"printer", printer_id.empty() ? json(nullptr) : json(printer_id)}});
    Journal::instance().add(tool, std::move(scope), action, nullptr, std::move(details), {});
}

double confirm_timeout_s()
{
#ifdef SLIC3R_MCP_TEST_PRINTERS
    if (const char* env = std::getenv("ORCA_MCP_CONFIRM_TIMEOUT_S"); env != nullptr && *env != '\0') {
        const double v = std::atof(env);
        if (v >= 1. && v <= 300.)
            return v;
    }
#endif
    return 300.;
}

}}}} // namespace Slic3r::GUI::Mcp::Printers
