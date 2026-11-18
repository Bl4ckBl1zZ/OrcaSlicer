// Platform-specific helpers: binary stdio, environment, paths, process liveness, logging.
#include "BridgeCommon.hpp"

#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shlobj.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace orca_mcp_bridge {

// ---------------------------------------------------------------------------------------------
// UTF-8 <-> UTF-16 (Windows only)
// ---------------------------------------------------------------------------------------------
#ifdef _WIN32
static std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

static FILE *open_file_utf8(const std::string &path, const char *mode)
{
#ifdef _WIN32
    return ::_wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------
namespace {
std::mutex g_log_mutex;
int        g_verbosity = 1;
FILE      *g_log_file  = nullptr;

std::string timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const long ms = long(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;
}

void emit(const char *level, const std::string &msg)
{
    std::string line = std::string("[") + BRIDGE_NAME + "] " + timestamp() + " " + level + ": " + msg + "\n";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    if (g_log_file) {
        std::fwrite(line.data(), 1, line.size(), g_log_file);
        std::fflush(g_log_file);
    }
}
} // namespace

void log_init(int verbosity, const std::string &log_file_utf8)
{
    g_verbosity = verbosity;
    if (!log_file_utf8.empty()) {
        g_log_file = open_file_utf8(log_file_utf8, "ab");
        if (!g_log_file)
            emit("error", "cannot open log file " + log_file_utf8);
    }
}

void log_error(const std::string &msg) { emit("error", msg); }
void log_info(const std::string &msg)
{
    if (g_verbosity >= 1)
        emit("info", msg);
}
void log_debug(const std::string &msg)
{
    if (g_verbosity >= 2)
        emit("debug", msg);
}
bool log_debug_enabled() { return g_verbosity >= 2; }

// ---------------------------------------------------------------------------------------------
// stdio
// ---------------------------------------------------------------------------------------------
void stdio_init_binary()
{
#ifdef _WIN32
    // No CRLF translation in either direction; we frame on '\n' ourselves.
    ::_setmode(::_fileno(stdin), _O_BINARY);
    ::_setmode(::_fileno(stdout), _O_BINARY);
#else
    // A closed stdout must surface as a write error, not kill the process.
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

long stdin_read(char *buf, std::size_t len)
{
#ifdef _WIN32
    const unsigned int n = len > 0x40000000u ? 0x40000000u : unsigned(len);
    const int r = ::_read(0, buf, n);
    return long(r);
#else
    for (;;) {
        const ssize_t r = ::read(0, buf, len);
        if (r < 0 && errno == EINTR)
            continue;
        return long(r);
    }
#endif
}

bool stdout_write_all(const char *data, std::size_t len)
{
    while (len > 0) {
#ifdef _WIN32
        const unsigned int chunk = len > 0x10000000u ? 0x10000000u : unsigned(len);
        const int w = ::_write(1, data, chunk);
        if (w <= 0)
            return false;
#else
        const ssize_t w = ::write(1, data, len);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
#endif
        data += w;
        len -= std::size_t(w);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Environment and files
// ---------------------------------------------------------------------------------------------
std::optional<std::string> get_env(const char *name)
{
#ifdef _WIN32
    const std::wstring wname = widen(name);
    const DWORD n = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0)
        return std::nullopt;
    std::wstring buf(n, L'\0');
    const DWORD m = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    buf.resize(m);
    return narrow(buf);
#else
    const char *v = std::getenv(name);
    if (!v)
        return std::nullopt;
    return std::string(v);
#endif
}

bool read_file_utf8_path(const std::string &path, std::string &out, std::string &err)
{
    FILE *f = open_file_utf8(path, "rb");
    if (!f) {
        err = std::string("cannot open ") + path + ": " + std::strerror(errno);
        return false;
    }
    out.clear();
    char buf[16384];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
        out.append(buf, n);
        if (n < sizeof(buf))
            break;
        if (out.size() > (16u << 20)) {
            std::fclose(f);
            err = "file too large: " + path;
            return false;
        }
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        err = "cannot read " + path;
        return false;
    }
    return true;
}

std::optional<std::string> file_stamp(const std::string &path)
{
#ifdef _WIN32
    struct _stat64 st{};
    if (::_wstat64(widen(path).c_str(), &st) != 0)
        return std::nullopt;
    return std::to_string(st.st_mtime) + ":" + std::to_string(st.st_size);
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return std::nullopt;
#ifdef __APPLE__
    const long long nsec = (long long) st.st_mtimespec.tv_nsec;
#else
    const long long nsec = (long long) st.st_mtim.tv_nsec;
#endif
    return std::to_string((long long) st.st_mtime) + "." + std::to_string(nsec) + ":" + std::to_string((long long) st.st_size);
#endif
}

bool file_exists(const std::string &path) { return file_stamp(path).has_value(); }

std::string join_path(const std::string &a, const std::string &b)
{
    if (a.empty())
        return b;
    const char last = a.back();
    if (last == '/' || last == '\\')
        return a + b;
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

static std::string parent_dir(const std::string &p)
{
    const auto pos = p.find_last_of("/\\");
    if (pos == std::string::npos)
        return std::string();
    return p.substr(0, pos);
}

std::string executable_dir()
{
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return parent_dir(narrow(buf));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return std::string();
    buf.resize(std::strlen(buf.c_str()));
    char resolved[PATH_MAX];
    if (::realpath(buf.c_str(), resolved))
        buf = resolved;
    return parent_dir(buf);
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    buf[n] = '\0';
    return parent_dir(buf);
#endif
}

// The data directories OrcaSlicer itself uses, in the order it picks them:
//  1. a "data_dir" folder beside the application (portable install; GUI_App::init_app_config),
//  2. the per-user default: Windows %APPDATA%\OrcaSlicer, macOS ~/Library/Application Support/
//     OrcaSlicer, Linux $XDG_CONFIG_HOME/OrcaSlicer or ~/.config/OrcaSlicer.
std::vector<std::string> default_data_dirs()
{
    std::vector<std::string> dirs;
    const char *app_key = "OrcaSlicer";
    std::string exe = executable_dir();
    if (!exe.empty()) {
#ifdef __APPLE__
        // <bundle>.app/Contents/MacOS -> folder holding the bundle
        std::string bundle_parent = parent_dir(parent_dir(parent_dir(exe)));
        if (!bundle_parent.empty())
            dirs.push_back(join_path(bundle_parent, "data_dir"));
#endif
        dirs.push_back(join_path(exe, "data_dir"));
    }
#ifdef _WIN32
    PWSTR path = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path) {
        dirs.push_back(join_path(narrow(path), app_key));
    } else if (auto appdata = get_env("APPDATA")) {
        dirs.push_back(join_path(*appdata, app_key));
    }
    if (path)
        ::CoTaskMemFree(path);
#elif defined(__APPLE__)
    if (auto home = get_env("HOME"))
        dirs.push_back(*home + "/Library/Application Support/" + app_key);
#else
    if (auto xdg = get_env("XDG_CONFIG_HOME"); xdg && !xdg->empty()) {
        dirs.push_back(*xdg + "/" + app_key);
    } else {
        std::string home;
        if (auto h = get_env("HOME"))
            home = *h;
        else if (const passwd *pw = ::getpwuid(::getuid()))
            home = pw->pw_dir;
        dirs.push_back(home + "/.config/" + app_key);
    }
#endif
    return dirs;
}

std::optional<bool> process_alive(long long pid)
{
    if (pid <= 0)
        return std::nullopt;
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) {
        if (::GetLastError() == ERROR_INVALID_PARAMETER)
            return false; // no such process
        return std::nullopt;
    }
    DWORD code = 0;
    const BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    if (!ok)
        return std::nullopt;
    return code == STILL_ACTIVE;
#else
    if (::kill(pid_t(pid), 0) == 0)
        return true;
    if (errno == ESRCH)
        return false;
    if (errno == EPERM)
        return true; // exists, owned by someone else
    return std::nullopt;
#endif
}

} // namespace orca_mcp_bridge
