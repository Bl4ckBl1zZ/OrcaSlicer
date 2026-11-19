#include "McpFiles.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <boost/filesystem.hpp>

#include <wx/stdpaths.h>
#include <wx/string.h>

#ifdef _WIN32
#include <wx/msw/wrapwin.h>
#else
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

std::string comparable(const fs::path& path, bool fold_case)
{
    std::string text = path.generic_string();
    while (text.size() > 1 && text.back() == '/')
        text.pop_back();
    if (fold_case)
        text = to_lower_ascii(text);
    return text;
}

// `path` is `root` or below it, by the text of both. With `fold_case`, ASCII letters compare equal
// in either case: only for the lexical pre-filter, never as the decision (a case-sensitive volume can
// hold both "Docs" and "docs").
bool is_inside_text(const fs::path& path, const fs::path& root, bool fold_case)
{
    const std::string p = comparable(path, fold_case);
    const std::string r = comparable(root, fold_case);
    if (r.empty())
        return false;
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && (r.back() == '/' || p[r.size()] == '/'));
}

// `path` (an existing file or folder, links resolved) is `root` or lies below it. Exact text first;
// otherwise `path` and each of its parents are compared with `root` by identity (fs::equivalent: the
// same volume and file id), which is right on case-insensitive volumes (NTFS, APFS and HFS+ by
// default, FAT, SMB; "C:/users/ME" is the folder "C:/Users/me"; also non-ASCII letters and Windows
// short names) and can never match a different folder on a case-sensitive one. Because `path` is
// resolved, its textual parents are its real parents.
bool is_inside(const fs::path& path, const fs::path& root)
{
    if (is_inside_text(path, root, false))
        return true;
    boost::system::error_code ec;
    if (root.empty() || path.empty() || !fs::is_directory(root, ec))
        return false;
    fs::path current = path;
    for (int depth = 0; depth < 4096; ++depth) {
        ec.clear();
        if (fs::equivalent(current, root, ec) && !ec)
            return true;
        // Stop at the root directory ("C:/", "/"): its parent ("C:", the current folder of drive C)
        // is something else.
        if (!current.has_relative_path())
            break;
        current = current.parent_path();
    }
    return false;
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

// The pre-filter, on the text alone (ASCII case folded: the resolved test decides).
bool inside_lexically(const fs::path& path, const std::vector<Root>& roots)
{
    return std::any_of(roots.begin(), roots.end(),
                       [&path](const Root& root) { return is_inside_text(path, root.lexical, true) || is_inside_text(path, root.resolved, true); });
}

// The decision: `path` must exist and be resolved (a file to read, or the folder of a file to write).
bool inside_resolved(const fs::path& path, const std::vector<Root>& roots)
{
    return std::any_of(roots.begin(), roots.end(), [&path](const Root& root) { return is_inside(path, root.resolved); });
}

// ---- Open handles: where a file really is, and how many names it has --------------------------

#ifdef _WIN32
using NativeHandle = HANDLE;
NativeHandle to_native(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }
std::intptr_t from_native(NativeHandle h) { return reinterpret_cast<std::intptr_t>(h); }
bool          valid(std::intptr_t h) { return h != -1 && h != 0; }
void          close_handle(std::intptr_t h) { ::CloseHandle(to_native(h)); }

// Opens an existing file. Reparse points are followed: OneDrive and other cloud files are reparse
// points too, and where the handle really ends up is checked through its final path anyway. `read`:
// read access, else attributes only. `pin`: share read and write but not delete, so neither the file
// nor a folder above it can be renamed, deleted or replaced while the handle is open.
std::intptr_t open_file(const fs::path& path, bool read, bool pin)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | (pin ? 0 : FILE_SHARE_DELETE);
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE h = ::CreateFileW(path.wstring().c_str(), read ? GENERIC_READ : FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, flags, nullptr);
    return h == INVALID_HANDLE_VALUE ? -1 : from_native(h);
}

struct HandleInfo
{
    bool          regular = false;
    unsigned long links = 0;
};

bool handle_info(std::intptr_t h, HandleInfo& info)
{
    BY_HANDLE_FILE_INFORMATION data;
    if (!::GetFileInformationByHandle(to_native(h), &data))
        return false;
    info.regular = (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) == 0;
    info.links = data.nNumberOfLinks;
    return true;
}

// The path the system resolved the handle to ("\\?\C:\..." -> "C:\...", "\\?\UNC\srv\share" ->
// "\\srv\share"); empty when unknown.
fs::path final_path(std::intptr_t h)
{
    std::wstring buffer(MAX_PATH + 1, L'\0');
    DWORD        n = ::GetFinalPathNameByHandleW(to_native(h), buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buffer.size()) {
        buffer.assign(size_t(n) + 1, L'\0');
        n = ::GetFinalPathNameByHandleW(to_native(h), buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (n == 0 || n >= buffer.size())
        return {};
    buffer.resize(n);
    if (buffer.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        buffer = L"\\\\" + buffer.substr(8);
    else if (buffer.compare(0, 4, L"\\\\?\\") == 0)
        buffer = buffer.substr(4);
    return fs::path(buffer);
}
#else
bool valid(std::intptr_t h) { return h >= 0; }
void close_handle(std::intptr_t h) { ::close(static_cast<int>(h)); }

// O_NOFOLLOW: the last name must not be a link (callers pass resolved paths, so one there now was
// put there after the check). O_NONBLOCK: a FIFO swapped in must not hang the main thread. POSIX
// cannot pin a name, so `read` and `pin` change nothing.
std::intptr_t open_file(const fs::path& path, bool /*read*/, bool /*pin*/)
{
    return ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
}

struct HandleInfo
{
    bool          regular = false;
    unsigned long links = 0;
};

bool handle_info(std::intptr_t h, HandleInfo& info)
{
    struct stat st;
    if (::fstat(static_cast<int>(h), &st) != 0)
        return false;
    info.regular = S_ISREG(st.st_mode);
    info.links = static_cast<unsigned long>(st.st_nlink);
    return true;
}

// Where the descriptor really is: F_GETPATH on macOS, /proc/self/fd on Linux; empty when unknown.
fs::path final_path(std::intptr_t h)
{
#if defined(__APPLE__) && defined(F_GETPATH)
    char buffer[PATH_MAX + 1] = {};
    if (::fcntl(static_cast<int>(h), F_GETPATH, buffer) == 0)
        return fs::path(buffer);
    return {};
#else
    const std::string link = "/proc/self/fd/" + std::to_string(h);
    char              buffer[PATH_MAX + 1] = {};
    const ssize_t     n = ::readlink(link.c_str(), buffer, PATH_MAX);
    if (n <= 0)
        return {};
    std::string text(buffer, static_cast<size_t>(n));
    // A file deleted while open reads "/path (deleted)": not a path to trust.
    if (text.empty() || text[0] != '/' || text.find(" (deleted)") != std::string::npos)
        return {};
    return fs::path(text);
#endif
}
#endif

// Closes the handle when it goes out of scope (unless moved into a Pin).
struct ScopedHandle
{
    std::intptr_t h = -1;
    explicit ScopedHandle(std::intptr_t handle) : h(handle) {}
    ~ScopedHandle()
    {
        if (valid(h))
            close_handle(h);
    }
    ScopedHandle(const ScopedHandle&)            = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    std::intptr_t release()
    {
        const std::intptr_t out = h;
        h                       = -1;
        return out;
    }
};

// The number of names (hard links) of the file at `path`, without following a link; 0 when unknown.
unsigned long link_count(const fs::path& path)
{
#ifdef _WIN32
    ScopedHandle handle(open_file(path, false, false));
    HandleInfo   info;
    return valid(handle.h) && handle_info(handle.h, info) ? info.links : 0;
#else
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 ? static_cast<unsigned long>(st.st_nlink) : 0;
#endif
}

[[noreturn]] void hard_link_refused(const fs::path& path, const char* what, unsigned long links)
{
    fail("path_denied",
         std::string("`") + what + "`: " + path.generic_string() + " has " + std::to_string(links) +
             " hard links (other names for the same file, possibly outside the allowed folders); the agent may not write it.",
         "Choose another file name, or ask the user to replace the file with a plain copy.", json::object({{"reason", "hard_link"}}));
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

Pin::~Pin() { reset(); }

Pin::Pin(Pin&& other) noexcept : handle(other.handle) { other.handle = -1; }

Pin& Pin::operator=(Pin&& other) noexcept
{
    if (this != &other) {
        reset();
        handle       = other.handle;
        other.handle = -1;
    }
    return *this;
}

void Pin::reset()
{
    if (valid(handle))
        close_handle(handle);
    handle = -1;
}

namespace {

// Never the application's own folders, whatever the configured roots say. (The workspace is exempt:
// with a portable data folder it sits inside the installation.)
void check_not_app_folder(const fs::path& resolved, const std::vector<std::string>& allowed, const char* what)
{
    if (is_inside(resolved, canonical_path(fs::path(workspace_dir()))))
        return;
    if (is_inside(resolved, canonical_path(fs::path(Slic3r::data_dir()))))
        outside(resolved, allowed, what, "write into the application's data folder:");
    if (is_inside(resolved, canonical_path(fs::path(Slic3r::resources_dir()).parent_path())))
        outside(resolved, allowed, what, "write into the application's installation folder:");
}

[[noreturn]] void moved(const fs::path& checked, const fs::path& real, const std::vector<std::string>& allowed, const char* what, const char* verb)
{
    fail("path_denied",
         std::string("`") + what + "`: " + checked.generic_string() + " changed while it was being " + verb + ": it now leads to " +
             (real.empty() ? std::string("a place that cannot be determined") : real.generic_string()) +
             ", outside the allowed folders (a file or folder was replaced by a link after the check).",
         "Ask the user to check the folder.", json::object({{"reason", "changed_after_check"}, {"allowed", allowed}}));
}

} // namespace

fs::path check_read(const std::string& path, const std::vector<std::string>& extensions, const char* what, Pin* pin)
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
    // 4. Open it, then ask the system where the open file really is: a folder or the file swapped
    //    for a link since step 3 shows here. With a pin the handle stays open while the tool loads
    //    the file (on Windows nobody can rename or replace it meanwhile).
    ScopedHandle handle(open_file(resolved, true, pin != nullptr));
    if (!valid(handle.h) && pin != nullptr)
        handle.h = open_file(resolved, true, false); // Another program holds it with delete access: unpinned.
    if (!valid(handle.h))
        fail("not_found", std::string("`") + what + "`: " + resolved.generic_string() + " cannot be opened.",
             "Check that the file exists and is readable (and, on Windows, not being deleted or moved).");
    HandleInfo info;
    if (!handle_info(handle.h, info) || !info.regular)
        fail("not_found", std::string("`") + what + "`: " + resolved.generic_string() + " is not a regular file.", "Give the path of a file.");
    const fs::path real = final_path(handle.h);
    // Unknown on systems without F_GETPATH or /proc: then only the checks above apply.
    if (!real.empty() && !inside_resolved(real, roots))
        moved(resolved, real, allowed, what, "opened");
    if (pin != nullptr) {
        pin->reset();
        pin->handle = handle.release();
    }
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
    if (!inside_resolved(folder, roots))
        outside(resolved, allowed, what, "write");
    check_not_app_folder(folder, allowed, what);
    const fs::file_status status = fs::symlink_status(resolved, type_ec);
    if (fs::is_symlink(status))
        fail("path_denied", std::string("`") + what + "`: " + resolved.generic_string() + " is a symbolic link; the agent may not write through links.",
             "Choose another file name, or ask the user to remove the link.");
    if (fs::is_directory(status))
        fail("path_denied", std::string("`") + what + "`: " + resolved.generic_string() + " is a folder.", "Give a file name.");
    if (!overwrite && fs::exists(status))
        fail("exists", std::string("`") + what + "`: " + resolved.generic_string() + " already exists.", "Pass overwrite: true to replace it, or choose another name.");
    // A file with other names (hard links): writing it in place would change them too, and they can
    // be anywhere on the volume.
    if (fs::exists(status))
        if (const unsigned long links = link_count(resolved); links > 1)
            hard_link_refused(resolved, what, links);
    return resolved;
}

void verify_written(const fs::path& path, const char* what)
{
    const std::vector<Root>        roots = root_list("mcp_write_roots", false);
    const std::vector<std::string> allowed = resolved_texts(roots);
    boost::system::error_code      ec;
    if (fs::is_symlink(fs::symlink_status(path, ec)))
        moved(path, fs::read_symlink(path, ec), allowed, what, "written");
    ScopedHandle handle(open_file(path, false, false));
    if (!valid(handle.h))
        return; // Nothing there (the writer failed or removed it): the caller reports the failure.
    HandleInfo info;
    if (handle_info(handle.h, info) && info.links > 1)
        hard_link_refused(path, what, info.links);
    fs::path real = final_path(handle.h);
    if (real.empty()) {
        // No final path from the system: the folder, resolved again, must still be inside.
        real = fs::canonical(path.parent_path(), ec);
        if (ec)
            moved(path, fs::path(), allowed, what, "written");
        real /= path.filename();
    }
    if (!inside_resolved(real, roots))
        moved(path, real, allowed, what, "written");
    check_not_app_folder(real, allowed, what);
}

} // namespace Files

}}} // namespace Slic3r::GUI::Mcp
