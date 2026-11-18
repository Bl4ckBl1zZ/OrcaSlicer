#pragma once

#include <functional>
#include <string>

#include <boost/filesystem/path.hpp>

// Application-side management of the Bambu "Linux runtime" on Windows: getting the runtime
// package (forwarder module, Linux host binaries, WSL scripts and rootfs) from the application
// folder into <data_dir>/plugins, and importing/verifying the WSL2 distro the host runs in.
// Compiled into libslic3r_gui; the forwarder module itself does not use it.

namespace Slic3r::SlicerLinuxRuntime {

// Sub-folder of the application folder the build installs the runtime package into.
const char* runtime_package_folder_name();

// <application folder>/linux_runtime, or an empty path when the build carries no package.
boost::filesystem::path runtime_package_dir();

// Copies new or changed runtime package files into component_dir (<data_dir>/plugins).
// Bambu's proprietary libraries are never part of the package and are never touched.
// Returns false, with a reason, when a file could not be copied.
bool sync_runtime_package(const boost::filesystem::path& component_dir, std::string* reason = nullptr);

struct RuntimeCommandResult
{
    int         exit_code{-1};
    bool        canceled{false};
    std::string output; // stderr followed by stdout, tail-limited

    bool ok() const { return !canceled && exit_code == 0; }
};

// Runs install_runtime.ps1 from component_dir: imports the WSL2 distro when it is missing or its
// rootfs changed, then verifies it. Blocks until the script ends; cancel_requested is polled.
RuntimeCommandResult install_wsl_runtime(const boost::filesystem::path& component_dir,
                                         const boost::filesystem::path& component_cache_dir,
                                         const std::function<bool()>& cancel_requested = {});

}
