#include "McpFiles.hpp"

#include <algorithm>
#include <cctype>
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

bool is_separator(char ch) { return ch == '/' || ch == '\\'; }

[[noreturn]] void malformed(const char* what, const std::string& why, const char* reason)
{
    fail("path_denied", std::string("`") + what + "`: " + why,
         "Give an absolute path on a local drive, inside one of the allowed folders (app_status lists them).",
         json::object({{"reason", reason}}));
}

#ifdef _WIN32
// CON, PRN, AUX, NUL, COM0-9, LPT0-9 (also with superscript 1-3), CONIN$, CONOUT$: Windows opens the
// device whatever the extension ("nul.3mf") and trailing spaces before it ("CON .txt").
bool is_device_name(const std::string& part)
{
    std::string base = part.substr(0, part.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    base = to_lower_ascii(base);
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$")
        return true;
    const bool com_or_lpt = base.size() >= 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0);
    if (com_or_lpt && base.size() == 4 && base[3] >= '0' && base[3] <= '9')
        return true;
    // Superscript one, two, three in UTF-8: C2 B9, C2 B2, C2 B3.
    if (com_or_lpt && base.size() == 5 && static_cast<unsigned char>(base[3]) == 0xC2) {
        const unsigned char digit = static_cast<unsigned char>(base[4]);
        if (digit == 0xB9 || digit == 0xB2 || digit == 0xB3)
            return true;
    }
    return false;
}
#endif

// Checks on the text alone, before anything touches the file system: Windows contacts the server of
// a UNC path (\\host\share, outbound SMB) as soon as the path is looked at, and device names,
// alternate data streams and trailing dots or spaces name something else than they appear to.
void check_path_text(const std::string& text, const char* what)
{
    if (text.size() >= 2 && is_separator(text[0]) && is_separator(text[1]))
        malformed(what, "network (UNC) and device paths (\\\\server\\share, \\\\?\\..., \\\\.\\...) are not accepted.", "unc");
    for (char ch : text)
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7F)
            malformed(what, "the path contains a control character.", "control_character");
#ifdef _WIN32
    const bool absolute = text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':' && is_separator(text[2]);
#else
    const bool absolute = !text.empty() && text[0] == '/';
#endif
    if (!absolute)
        malformed(what, "the path must be absolute (for example C:\\Users\\me\\Documents\\part.3mf); relative paths are not accepted.", "relative");
#ifdef _WIN32
    if (text.find(':', 2) != std::string::npos)
        malformed(what, "':' may only follow the drive letter (alternate data streams are not accepted).", "stream");
    size_t start = 3;
    while (start <= text.size()) {
        size_t end = start;
        while (end < text.size() && !is_separator(text[end]))
            ++end;
        const std::string part = text.substr(start, end - start);
        if (!part.empty() && part != "." && part != "..") {
            if (part.back() == '.' || part.back() == ' ')
                malformed(what, "the name \"" + part + "\" ends with a dot or a space, which Windows silently drops.", "trailing_dot_or_space");
            if (is_device_name(part))
                malformed(what, "\"" + part + "\" is a reserved device name on Windows.", "device_name");
        }
        if (end >= text.size())
            break;
        start = end + 1;
    }
#endif
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

// An allowed folder as configured (made absolute, "." and ".." removed) and resolved (links followed).
// A path is first tested against either form without touching the disk, then, resolved, against the
// resolved form.
struct Root
{
    fs::path lexical;
    fs::path resolved;
};

void add_root(std::vector<Root>& roots, const std::string& folder)
{
    if (folder.empty())
        return;
    boost::system::error_code ec;
    fs::path                  lexical = fs::absolute(fs::path(folder), ec);
    if (ec)
        lexical = fs::path(folder);
    Root root { lexical.lexically_normal(), canonical_path(fs::path(folder)) };
    const std::string text = root.resolved.generic_string();
    if (std::none_of(roots.begin(), roots.end(), [&text](const Root& r) { return r.resolved.generic_string() == text; }))
        roots.push_back(std::move(root));
}

std::vector<Root> root_list(const char* config_key, bool with_user_folders)
{
    std::vector<Root> out;
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

std::vector<std::string> resolved_texts(const std::vector<Root>& roots)
{
    std::vector<std::string> out;
    for (const Root& root : roots)
        out.push_back(root.resolved.generic_string());
    return out;
}

bool inside_lexically(const fs::path& path, const std::vector<Root>& roots)
{
    return std::any_of(roots.begin(), roots.end(), [&path](const Root& root) { return is_inside(path, root.lexical) || is_inside(path, root.resolved); });
}

bool inside_resolved(const fs::path& path, const std::vector<Root>& roots)
{
    return std::any_of(roots.begin(), roots.end(), [&path](const Root& root) { return is_inside(path, root.resolved); });
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

std::vector<std::string> read_roots() { return resolved_texts(root_list("mcp_read_roots", true)); }
std::vector<std::string> write_roots() { return resolved_texts(root_list("mcp_write_roots", false)); }

const std::vector<std::string>& model_extensions()
{
    static const std::vector<std::string> extensions = {".stl", ".3mf", ".step", ".stp", ".obj", ".amf", ".drc", ".oltp", ".svg"};
    return extensions;
}

fs::path check_read(const std::string& path, const std::vector<std::string>& extensions, const char* what)
{
    if (path.empty())
        fail("invalid_argument", std::string("`") + what + "` is empty.");
    // 1. The text alone: no UNC, device or relative paths (nothing has touched the disk yet).
    check_path_text(path, what);
    const std::vector<Root>        roots = root_list("mcp_read_roots", true);
    const std::vector<std::string> allowed = resolved_texts(roots);
    // 2. Lexically inside an allowed folder ("..", "." resolved without the file system).
    const fs::path lexical = fs::path(path).lexically_normal();
    if (!inside_lexically(lexical, roots))
        outside(lexical, allowed, what, "read");
    check_extension(lexical, extensions, what);
    // 3. Resolved (symbolic links, junctions): the file must exist and still be inside.
    boost::system::error_code ec;
    const fs::path            resolved = fs::canonical(lexical, ec);
    boost::system::error_code type_ec;
    if (ec || !fs::is_regular_file(resolved, type_ec))
        fail("not_found", std::string("`") + what + "`: there is no file " + lexical.generic_string() + ".", "Check the path; it must be absolute.");
    if (!inside_resolved(resolved, roots))
        outside(resolved, allowed, what, "read");
    check_extension(resolved, extensions, what);
    return resolved;
}

fs::path check_write(const std::string& path, const std::vector<std::string>& extensions, bool overwrite, const char* what)
{
    if (path.empty())
        fail("invalid_argument", std::string("`") + what + "` is empty.");
    // 1. The text alone: no UNC, device or relative paths (nothing has touched the disk yet).
    check_path_text(path, what);
    const std::vector<Root>        roots = root_list("mcp_write_roots", false);
    const std::vector<std::string> allowed = resolved_texts(roots);
    // 2. Lexically inside an allowed folder.
    const fs::path lexical = fs::path(path).lexically_normal();
    const fs::path name = lexical.filename();
    if (name.empty() || name == "." || name == "..")
        fail("path_denied", std::string("`") + what + "`: " + lexical.generic_string() + " is a folder.", "Give a file name.");
    if (!inside_lexically(lexical, roots))
        outside(lexical, allowed, what, "write");
    check_extension(lexical, extensions, what);
    // 3. Resolved: the folder with its links followed, plus the file name. A link in place of the
    //    file itself is refused below, so the write cannot be redirected out of the folder.
    boost::system::error_code ec;
    const fs::path            folder = fs::canonical(lexical.parent_path(), ec);
    boost::system::error_code type_ec;
    if (ec || !fs::is_directory(folder, type_ec))
        fail("path_denied", std::string("`") + what + "`: the folder " + lexical.parent_path().generic_string() + " does not exist.", "Create the file in an existing folder.");
    const fs::path resolved = folder / name;
    if (!inside_resolved(resolved, roots))
        outside(resolved, allowed, what, "write");
    // Whatever the configured roots say: never the application's own folders.
    // (The workspace is exempt: with a portable data folder it sits inside the installation.)
    if (!is_inside(resolved, canonical_path(fs::path(workspace_dir())))) {
        if (is_inside(resolved, canonical_path(fs::path(Slic3r::data_dir()))))
            outside(resolved, allowed, what, "write into the application's data folder:");
        if (is_inside(resolved, canonical_path(fs::path(Slic3r::resources_dir()).parent_path())))
            outside(resolved, allowed, what, "write into the application's installation folder:");
    }
    const fs::file_status status = fs::symlink_status(resolved, type_ec);
    if (fs::is_symlink(status))
        fail("path_denied", std::string("`") + what + "`: " + resolved.generic_string() + " is a symbolic link; the agent may not write through links.",
             "Choose another file name, or ask the user to remove the link.");
    if (fs::is_directory(status))
        fail("path_denied", std::string("`") + what + "`: " + resolved.generic_string() + " is a folder.", "Give a file name.");
    if (!overwrite && fs::exists(status))
        fail("exists", std::string("`") + what + "`: " + resolved.generic_string() + " already exists.", "Pass overwrite: true to replace it, or choose another name.");
    return resolved;
}

} // namespace Files

}}} // namespace Slic3r::GUI::Mcp
