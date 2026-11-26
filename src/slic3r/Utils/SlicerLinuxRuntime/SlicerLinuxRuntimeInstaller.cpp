#include "SlicerLinuxRuntimeInstaller.hpp"
#include "SlicerLinuxRuntimeConfig.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#if defined(_WIN32)
#include <boost/process/windows.hpp>
#else
#include <unistd.h>
#endif

namespace Slic3r::SlicerLinuxRuntime {

namespace fs = boost::filesystem;
namespace bp = boost::process;

namespace {

void set_reason(std::string* reason, std::string value)
{
    if (reason)
        *reason = std::move(value);
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    if (!stream)
        return {};
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

// Same content is assumed when size and modification time agree; the package files are only
// ever replaced by a new build, which changes at least one of the two.
bool needs_copy(const fs::path& src, const fs::path& dst)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(dst, ec))
        return true;
    const auto src_size = fs::file_size(src, ec);
    if (ec)
        return true;
    const auto dst_size = fs::file_size(dst, ec);
    if (ec || src_size != dst_size)
        return true;
    const auto src_time = fs::last_write_time(src, ec);
    if (ec)
        return true;
    const auto dst_time = fs::last_write_time(dst, ec);
    return ec || src_time != dst_time;
}

bool is_bambu_component_file(const std::string& name)
{
    for (const char* bambu_name : {"libbambu_networking.so", "libBambuSource.so", "liblive555.so",
                                   "libagora_rtc_sdk.so", "libagora-fdkaac.so", "network_plugins.json"})
        if (name == bambu_name)
            return true;
    return name == linux_component_manifest_file_name();
}

} // namespace

const char* runtime_package_folder_name()
{
    return "linux_runtime";
}

fs::path runtime_package_dir()
{
    boost::system::error_code ec;
    const fs::path exe = boost::dll::program_location(ec);
    if (ec || exe.empty())
        return {};
#if defined(__APPLE__)
    // OrcaSlicer.app/Contents/MacOS/OrcaSlicer -> OrcaSlicer.app/Contents/Resources: Contents/MacOS is for code only.
    const fs::path dir = exe.parent_path().parent_path() / "Resources" / runtime_package_folder_name();
#else
    const fs::path dir = exe.parent_path() / runtime_package_folder_name();
#endif
    return fs::is_directory(dir, ec) ? dir : fs::path();
}

bool sync_runtime_package(const fs::path& component_dir, std::string* reason)
{
    if (!enabled()) {
        set_reason(reason, "Linux runtime disabled");
        return true;
    }

    const fs::path package_dir = runtime_package_dir();
    if (package_dir.empty()) {
        set_reason(reason, std::string("no ") + runtime_package_folder_name() + " folder next to the executable");
        return false;
    }

    boost::system::error_code ec;
    fs::create_directories(component_dir, ec);

    bool        all_ok = true;
    std::string first_error;
    int         copied = 0;
    for (fs::directory_iterator it(package_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->status()))
            continue;
        const fs::path    src  = it->path();
        const std::string name = src.filename().string();
        // The package is produced by this project's build; anything of Bambu's that a developer
        // left in there must not overwrite what the application downloaded.
        if (is_bambu_component_file(name))
            continue;

        const fs::path dst = component_dir / name;
        if (!needs_copy(src, dst))
            continue;

        boost::system::error_code copy_ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            // A module that is still loaded cannot be overwritten, but it can be renamed aside.
            fs::path aside = dst;
            aside += ".old";
            boost::system::error_code aside_ec;
            fs::remove(aside, aside_ec);
            fs::rename(dst, aside, aside_ec);
            copy_ec.clear();
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, copy_ec);
        }
        if (copy_ec) {
            all_ok = false;
            if (first_error.empty())
                first_error = name + ": " + copy_ec.message();
            BOOST_LOG_TRIVIAL(error) << "linux_runtime: copying " << src.string() << " -> " << dst.string()
                                     << " failed: " << copy_ec.message();
            continue;
        }
        boost::system::error_code time_ec;
        const auto src_time = fs::last_write_time(src, time_ec);
        if (!time_ec)
            fs::last_write_time(dst, src_time, time_ec);
        ++copied;
    }

    if (copied > 0)
        BOOST_LOG_TRIVIAL(info) << "linux_runtime: updated " << copied << " runtime package file(s) in " << component_dir.string();
    set_reason(reason, all_ok ? "ok" : first_error);
    return all_ok;
}

namespace {

unsigned long current_process_id()
{
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// Runs an installer script with its output captured in temporary files, and logs the result.
RuntimeCommandResult run_install_script(const fs::path& exe, const std::vector<std::string>& arguments,
                                        const std::function<bool()>& cancel_requested)
{
    RuntimeCommandResult result;
    boost::system::error_code ec;
    const std::string suffix = std::to_string(current_process_id()) + "-" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const fs::path stdout_path = fs::temp_directory_path() / ("orca-linux-runtime-" + suffix + ".out");
    const fs::path stderr_path = fs::temp_directory_path() / ("orca-linux-runtime-" + suffix + ".err");
    fs::remove(stdout_path, ec);
    fs::remove(stderr_path, ec);

    try {
        bp::child child(exe.string(), bp::args(arguments),
                        bp::std_out > stdout_path.string(), bp::std_err > stderr_path.string(), bp::std_in < bp::null
#if defined(_WIN32)
                        , bp::windows::create_no_window, bp::windows::hide
#endif
        );
        // Importing the rootfs takes well under a minute on a healthy machine; creating the OrbStack machine
        // and installing QEMU downloads a few hundred MB.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        while (child.running()) {
            if ((cancel_requested && cancel_requested()) || std::chrono::steady_clock::now() > deadline) {
                result.canceled = true;
                child.terminate();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        child.wait();
        result.exit_code = result.canceled ? -1 : child.exit_code();
    } catch (const std::exception& e) {
        result.output = e.what();
    }

    const std::string stderr_text = read_text_file(stderr_path);
    const std::string stdout_text = read_text_file(stdout_path);
    for (const std::string* text : {&stderr_text, &stdout_text}) {
        if (text->empty())
            continue;
        if (!result.output.empty())
            result.output += "\n";
        result.output += *text;
    }
    if (result.output.size() > 8000)
        result.output.erase(0, result.output.size() - 8000);
    fs::remove(stdout_path, ec);
    fs::remove(stderr_path, ec);

    BOOST_LOG_TRIVIAL(info) << "linux_runtime: " << arguments.front() << " exit_code=" << result.exit_code
                            << ", canceled=" << result.canceled;
    if (!result.output.empty()) {
        if (result.ok())
            BOOST_LOG_TRIVIAL(info) << "linux_runtime: " << result.output;
        else
            BOOST_LOG_TRIVIAL(error) << "linux_runtime: " << result.output;
    }
    return result;
}

} // namespace

RuntimeCommandResult install_wsl_runtime(const fs::path& component_dir,
                                         const fs::path& component_cache_dir,
                                         const std::function<bool()>& cancel_requested)
{
    RuntimeCommandResult result;
#if defined(_WIN32) || defined(__APPLE__)
#if defined(_WIN32)
    const fs::path script = component_dir / windows_wsl_import_script_file_name();
    const fs::path exe    = bp::search_path("powershell.exe");
    const std::vector<std::string> arguments = {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", script.string(),
        "-PackageDir", component_dir.string(),
        "-PluginDir", component_dir.string(),
        "-PluginCacheDir", component_cache_dir.string(),
        "-SkipCopyToPluginDir"
    };
#else
    const fs::path script = component_dir / mac_runtime_install_script_file_name();
    const fs::path exe    = "/bin/sh";
    const std::vector<std::string> arguments = {script.string(), component_dir.string(), component_cache_dir.string()};
#endif
    boost::system::error_code ec;
    if (!fs::is_regular_file(script, ec)) {
        result.output = "missing " + script.string();
    } else if (exe.empty()) {
        result.output = "powershell.exe not found";
    } else {
        fs::create_directories(component_cache_dir, ec);
        result = run_install_script(exe, arguments, cancel_requested);
    }
#else
    (void) component_dir;
    (void) component_cache_dir;
    (void) cancel_requested;
    result.exit_code = 0;
#endif
    return result;
}

}
