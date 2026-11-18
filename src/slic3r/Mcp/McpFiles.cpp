#include "McpFiles.hpp"

#include <algorithm>
#include <cstdlib>

#include <boost/filesystem.hpp>

#include <wx/stdpaths.h>
#include <wx/string.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "McpError.hpp"
#include "McpJson.hpp"
#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Files {

namespace fs = boost::filesystem;

namespace {

std::string utf8(const wxString& text) { return std::string(text.utf8_str().data()); }

fs::path canonical_path(const fs::path& path)
{
    boost::system::error_code ec;
    fs::path                  absolute = fs::absolute(path, ec);
    if (ec)
        absolute = path;
    fs::path resolved = fs::weakly_canonical(absolute, ec);
    if (ec)
        resolved = absolute.lexically_normal();
    return resolved;
}

std::string comparable(const fs::path& path)
{
    std::string text = path.generic_string();
    while (text.size() > 1 && text.back() == '/')
        text.pop_back();
#ifdef _WIN32
    text = to_lower_ascii(text);
#endif
    return text;
}

bool is_inside(const fs::path& path, const fs::path& root)
{
    const std::string p = comparable(path);
    const std::string r = comparable(root);
    if (r.empty())
        return false;
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && (r.back() == '/' || p[r.size()] == '/'));
}

std::vector<std::string> split_list(const std::string& text)
{
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(';', start);
        const std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty())
            out.push_back(item);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

void add_root(std::vector<std::string>& roots, const std::string& folder)
{
    if (folder.empty())
        return;
    const std::string text = canonical_path(fs::path(folder)).generic_string();
    if (std::find(roots.begin(), roots.end(), text) == roots.end())
        roots.push_back(text);
}

std::vector<std::string> roots(const char* config_key, bool with_user_folders)
{
    std::vector<std::string> out;
    const AppConfig*         config = wxGetApp().app_config;
    const std::string        configured = config != nullptr ? config->get(config_key) : std::string();
    if (!configured.empty()) {
        for (const std::string& folder : split_list(configured))
            add_root(out, folder);
    } else if (with_user_folders) {
        const wxStandardPaths& paths = wxStandardPaths::Get();
        add_root(out, utf8(paths.GetUserDir(wxStandardPaths::Dir_Documents)));
        add_root(out, utf8(paths.GetUserDir(wxStandardPaths::Dir_Downloads)));
        add_root(out, utf8(paths.GetUserDir(wxStandardPaths::Dir_Desktop)));
    }
    add_root(out, workspace_dir());
    if (const char* extra = std::getenv("ORCA_MCP_ROOTS"); extra != nullptr)
        for (const std::string& folder : split_list(extra))
            add_root(out, folder);
    return out;
}

std::string extension_of(const fs::path& path)
{
    // ".gcode.3mf" counts as its own extension.
    const std::string name = to_lower_ascii(path.filename().string());
    static const std::string double_ext = ".gcode.3mf";
    if (name.size() > double_ext.size() && name.compare(name.size() - double_ext.size(), double_ext.size(), double_ext) == 0)
        return double_ext;
    return to_lower_ascii(path.extension().string());
}

void check_extension(const fs::path& path, const std::vector<std::string>& extensions, const char* what)
{
    if (extensions.empty())
        return;
    const std::string ext = extension_of(path);
    if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
        return;
    std::string accepted;
    for (const std::string& e : extensions)
        accepted += (accepted.empty() ? "" : ", ") + e;
    fail("path_denied", std::string("`") + what + "`: files of type \"" + ext + "\" are not accepted here.", "Accepted: " + accepted + ".",
         json::object({{"extensions", extensions}}));
}

[[noreturn]] void outside(const fs::path& path, const std::vector<std::string>& allowed, const char* what, const char* verb)
{
    fail("path_denied", std::string("`") + what + "`: the agent may not " + verb + " " + path.generic_string() + ".",
         std::string("Use a file inside one of the allowed folders (app_status lists them). The user can add folders in Preferences > MCP server."),
         json::object({{"allowed", allowed}}));
}

} // namespace

std::string workspace_dir()
{
    const fs::path            dir = fs::path(Slic3r::data_dir()) / "mcp" / "workspace";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir.generic_string();
}

std::vector<std::string> read_roots() { return roots("mcp_read_roots", true); }
std::vector<std::string> write_roots() { return roots("mcp_write_roots", false); }

const std::vector<std::string>& model_extensions()
{
    static const std::vector<std::string> extensions = {".stl", ".3mf", ".step", ".stp", ".obj", ".amf", ".drc", ".oltp", ".svg"};
    return extensions;
}

fs::path check_read(const std::string& path, const std::vector<std::string>& extensions, const char* what)
{
    if (path.empty())
        fail("invalid_argument", std::string("`") + what + "` is empty.");
    const fs::path                 resolved = canonical_path(fs::path(path));
    const std::vector<std::string> allowed = read_roots();
    const bool inside = std::any_of(allowed.begin(), allowed.end(), [&resolved](const std::string& root) { return is_inside(resolved, fs::path(root)); });
    if (!inside)
        outside(resolved, allowed, what, "read");
    check_extension(resolved, extensions, what);
    boost::system::error_code ec;
    if (!fs::is_regular_file(resolved, ec))
        fail("not_found", std::string("`") + what + "`: there is no file " + resolved.generic_string() + ".", "Check the path; it must be absolute or relative to nothing.");
    return resolved;
}

fs::path check_write(const std::string& path, const std::vector<std::string>& extensions, bool overwrite, const char* what)
{
    if (path.empty())
        fail("invalid_argument", std::string("`") + what + "` is empty.");
    const fs::path                 resolved = canonical_path(fs::path(path));
    const std::vector<std::string> allowed = write_roots();
    const bool inside = std::any_of(allowed.begin(), allowed.end(), [&resolved](const std::string& root) { return is_inside(resolved, fs::path(root)); });
    if (!inside)
        outside(resolved, allowed, what, "write");
    // Whatever the configured roots say: never the application's own folders.
    // (The workspace is exempt: with a portable data folder it sits inside the installation.)
    if (!is_inside(resolved, canonical_path(fs::path(workspace_dir())))) {
        if (is_inside(resolved, canonical_path(fs::path(Slic3r::data_dir()))))
            outside(resolved, allowed, what, "write into the application's data folder:");
        if (is_inside(resolved, canonical_path(fs::path(Slic3r::resources_dir()).parent_path())))
            outside(resolved, allowed, what, "write into the application's installation folder:");
    }
    check_extension(resolved, extensions, what);
    boost::system::error_code ec;
    if (fs::is_directory(resolved, ec))
        fail("path_denied", std::string("`") + what + "`: " + resolved.generic_string() + " is a folder.", "Give a file name.");
    if (!overwrite && fs::exists(resolved, ec))
        fail("exists", std::string("`") + what + "`: " + resolved.generic_string() + " already exists.", "Pass overwrite: true to replace it, or choose another name.");
    if (!fs::is_directory(resolved.parent_path(), ec))
        fail("path_denied", std::string("`") + what + "`: the folder " + resolved.parent_path().generic_string() + " does not exist.", "Create the file in an existing folder.");
    return resolved;
}

} // namespace Files

}}} // namespace Slic3r::GUI::Mcp
