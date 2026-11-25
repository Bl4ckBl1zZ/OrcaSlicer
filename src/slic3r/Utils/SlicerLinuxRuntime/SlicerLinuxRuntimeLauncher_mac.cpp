// macOS launcher: the host runs in an arm64 OrbStack Linux machine, the x86-64 binary through qemu-user
// (Rosetta crashes in Bambu's plug-in). OrbStack shows the Mac's file system at the same paths inside the
// machine, so the component and cache folders are passed unchanged. install_runtime_macos.sh prepares the
// machine; this file only starts the host in it.
#include "SlicerLinuxRuntimeLauncher.hpp"
#include "SlicerLinuxRuntimeConfig.hpp"

#include <dlfcn.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>

namespace Slic3r::SlicerLinuxRuntime {

namespace {

constexpr const char* default_machine_name = "orca-bambu-bridge";
// Unpacked by install_runtime_macos.sh from the rootfs the Windows build imports into WSL.
constexpr const char* exec_prefix = "qemu-x86_64 -L /opt/x64root";

std::filesystem::path module_dir()
{
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(&build_default_launch_spec), &info) == 0 || info.dli_fname == nullptr)
        return {};
    std::error_code ec;
    const auto path = std::filesystem::canonical(std::filesystem::u8path(info.dli_fname), ec);
    return ec ? std::filesystem::u8path(info.dli_fname).parent_path() : path.parent_path();
}

std::string trim_ascii(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string env_value(const char* name)
{
    const char* value = std::getenv(name);
    return (value && *value) ? trim_ascii(value) : std::string();
}

std::string machine_name(const std::filesystem::path& component_dir)
{
    if (auto name = env_value("SLICER_LINUX_RUNTIME_ORBSTACK_MACHINE"); !name.empty())
        return name;
    std::ifstream in(component_dir / mac_orbstack_machine_file_name());
    std::string name = trim_ascii(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    return name.empty() ? default_machine_name : name;
}

std::filesystem::path component_cache_dir(const std::filesystem::path& component_dir)
{
    // The application exports the directory it actually uses; <data_dir>/plugins -> <data_dir>/ota/plugins otherwise.
    if (auto dir = env_value("SLICER_LINUX_RUNTIME_WINDOWS_COMPONENT_CACHE_DIR"); !dir.empty())
        return std::filesystem::u8path(dir);
    return component_dir.parent_path() / "ota" / "plugins";
}

std::string first_missing_runtime_file(const std::filesystem::path& component_dir)
{
    for (const std::string& name : {host_executable_file_name(), std::string("slicer_linux_runtime_host_abi1"),
                                    std::string("slicer_linux_runtime_host_abi0"), windows_wsl_bootstrap_script_file_name(),
                                    std::string("ca-certificates.crt"), std::string("slicer_base64.cer")}) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(component_dir / name, ec))
            return name;
    }
    return {};
}

LaunchSpec error_launch_spec(const std::string& message)
{
    LaunchSpec spec;
    spec.description = "macos runtime preflight error";
    spec.argv = {"/bin/sh", "-c", "printf '%s\\n' \"$1\" >&2; exit 127", "sh", message};
    return spec;
}

} // namespace

std::string host_executable_name()
{
    return host_executable_file_name();
}

std::string host_pipe_hint()
{
    return "stdio";
}

std::string launch_preflight_error()
{
    const std::filesystem::path component_dir = module_dir();
    if (component_dir.empty())
        return "runtime launcher could not resolve plugin directory";
    if (orbstack_cli_path().empty())
        return "OrbStack was not found. Install it from https://orbstack.dev and start it once";
    if (const auto missing = first_missing_runtime_file(component_dir); !missing.empty())
        return "required macOS runtime file missing: " + missing;
    return {};
}

LaunchSpec build_default_launch_spec()
{
    if (const auto error = launch_preflight_error(); !error.empty())
        return error_launch_spec(error);

    const std::filesystem::path component_dir = module_dir();
    LaunchSpec spec;
    spec.description = "macos via OrbStack machine with qemu-user";
    // A machine that is missing or stopped makes orb fail or start it; the host's exit is reported by the RPC
    // client either way. install_runtime_macos.sh creates the machine.
    // The slicer hands the plug-in files in its temporary folder (the print job's 3MF), which macOS names
    // /var/folders/...; /var is the Mac's /private/var, which OrbStack shares as /private but not as /var.
    spec.argv = {
        orbstack_cli_path(),
        "-m", machine_name(component_dir),
        "-u", "root",
        "env", std::string("SLICER_LINUX_RUNTIME_EXEC_PREFIX=") + exec_prefix,
        "sh", "-c", "[ -e /var/folders ] || ln -s /private/var/folders /var/folders; exec sh \"$0\" \"$@\"",
        (component_dir / windows_wsl_bootstrap_script_file_name()).string(),
        component_dir.string(), component_cache_dir(component_dir).string()
    };
    return spec;
}

}
