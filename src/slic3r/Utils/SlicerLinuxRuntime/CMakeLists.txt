# Bambu "Linux runtime" forwarder for Windows.
#
# orca-slicer.exe loads slicer_linux_runtime.dll in place of Bambu's Windows bambu_networking.dll.
# The DLL exports the same bambu_network_* / ft_* / Bambu_* entry points and forwards every call
# over stdio RPC to a host process running inside a dedicated WSL2 distro, where Bambu's Linux
# libbambu_networking.so / libBambuSource.so are loaded (see tools/slicer_linux_runtime_host and
# tools/slicer_linux_runtime). SlicerLinuxRuntimeConfig.cpp is additionally compiled into
# libslic3r_gui for the application-side hooks.

# Windows x64 only, like SlicerLinuxRuntime::enabled(): Bambu's Linux plug-in is linux-x86_64.
# Everywhere else (macOS, Linux, Windows on ARM64) no target, no linux_runtime package and no
# data_dir folder is added, and the application uses the native plug-in as upstream does.
if (NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    return()
endif ()

set(SLICER_LINUX_RUNTIME_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeConfig.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeConfig.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeCompat.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcProtocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcProtocol.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcClient.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeRpcClient.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeEventPump.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeEventPump.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeForwarderState.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeForwarderState.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeForwarderExports.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeLauncher.hpp
    ${CMAKE_CURRENT_LIST_DIR}/SlicerLinuxRuntimeLauncher_win.cpp
)

# libslic3r_version.h is generated into the binary directory of the libslic3r target.
get_target_property(SLICER_LINUX_RUNTIME_LIBSLIC3R_BINARY_DIR libslic3r BINARY_DIR)

add_library(slicer_linux_runtime SHARED ${SLICER_LINUX_RUNTIME_SOURCES})

# The stand-in directory must come first: it replaces libslic3r/AppConfig.hpp, which
# Utils/bambu_networking.hpp includes, so the module does not depend on libslic3r.
target_include_directories(slicer_linux_runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/standalone
    ${SLICER_LINUX_RUNTIME_LIBSLIC3R_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps_src
)
target_compile_definitions(slicer_linux_runtime PRIVATE
    SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS=1
    BAMBU_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(slicer_linux_runtime PRIVATE
    boost_headeronly
    boost_libs
    OpenSSL::Crypto
    ws2_32
    Crypt32
)
set_target_properties(slicer_linux_runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# ---------------------------------------------------------------------------------------------
# Runtime package: everything the application copies from <app folder>/linux_runtime into <data_dir>/plugins
# at start-up (GUI_App::copy_network_if_available). Bambu's proprietary libraries are never part
# of it; the application downloads them from Bambu's servers.
# ---------------------------------------------------------------------------------------------
set(SLICER_LINUX_RUNTIME_TOOLS_DIR "${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime")
set(SLICER_LINUX_RUNTIME_HOST_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/tools/slicer_linux_runtime_host/runtime/linux-x86_64"
    CACHE PATH "Packaged Linux host runtime (output of tools/slicer_linux_runtime_host/package_linux_host_runtime.sh)")
set(SLICER_LINUX_RUNTIME_ROOTFS_TAR "${SLICER_LINUX_RUNTIME_TOOLS_DIR}/rootfs/windows-wsl2-rootfs.tar"
    CACHE FILEPATH "WSL2 rootfs tar (output of tools/slicer_linux_runtime/rootfs/build_windows_wsl_rootfs.sh)")

set(SLICER_LINUX_RUNTIME_SUPPORT_FILES
    "${SLICER_LINUX_RUNTIME_TOOLS_DIR}/wsl/slicer_linux_runtime_wsl_distro.txt"
    "${SLICER_LINUX_RUNTIME_TOOLS_DIR}/wsl/slicer_linux_runtime_wsl_run_host.sh"
    "${SLICER_LINUX_RUNTIME_TOOLS_DIR}/wsl/install_runtime.ps1"
    "${SLICER_LINUX_RUNTIME_TOOLS_DIR}/wsl/install_runtime.cmd"
    "${SLICER_LINUX_RUNTIME_TOOLS_DIR}/wsl/verify_runtime.ps1"
    "${CMAKE_SOURCE_DIR}/resources/cert/ca-certificates.crt"
    "${CMAKE_SOURCE_DIR}/resources/cert/slicer_base64.cer"
)

# Everything goes into one sub-folder of the application folder (SlicerLinuxRuntimeInstaller.cpp:
# runtime_package_folder_name()).
set(SLICER_LINUX_RUNTIME_PACKAGE_FOLDER "linux_runtime")
install(TARGETS slicer_linux_runtime RUNTIME DESTINATION "${SLICER_LINUX_RUNTIME_PACKAGE_FOLDER}")
install(FILES ${SLICER_LINUX_RUNTIME_SUPPORT_FILES} DESTINATION "${SLICER_LINUX_RUNTIME_PACKAGE_FOLDER}")

# The Linux side is built separately (inside WSL/Linux) and is picked up when present. It is
# resolved at install time, so a host built after configuring is still installed.
install(CODE "
    set(_slr_runtime_dir \"${SLICER_LINUX_RUNTIME_HOST_RUNTIME_DIR}\")
    set(_slr_rootfs \"${SLICER_LINUX_RUNTIME_ROOTFS_TAR}\")
    set(_slr_dest \"\${CMAKE_INSTALL_PREFIX}/${SLICER_LINUX_RUNTIME_PACKAGE_FOLDER}\")
    set(_slr_missing \"\")
    foreach(_slr_name slicer_linux_runtime_host slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0)
        if (NOT EXISTS \"\${_slr_runtime_dir}/\${_slr_name}\")
            list(APPEND _slr_missing \"\${_slr_runtime_dir}/\${_slr_name}\")
        endif ()
    endforeach()
    if (NOT EXISTS \"\${_slr_rootfs}\")
        list(APPEND _slr_missing \"\${_slr_rootfs}\")
    endif ()
    if (_slr_missing)
        message(WARNING \"Bambu Linux runtime: not installed, build the Linux side first. Missing: \${_slr_missing}\")
    else ()
        file(GLOB _slr_files LIST_DIRECTORIES false \"\${_slr_runtime_dir}/*\")
        foreach(_slr_file \${_slr_files})
            get_filename_component(_slr_name \"\${_slr_file}\" NAME)
            # Never ship Bambu's proprietary libraries, even if a developer left them there.
            if (NOT _slr_name MATCHES \"^(libbambu_networking|libBambuSource|liblive555|libagora_rtc_sdk|libagora-fdkaac)[.]so\$|^(linux_component_manifest|network_plugins)[.]json\$\")
                file(INSTALL \"\${_slr_file}\" DESTINATION \"\${_slr_dest}\")
            endif ()
        endforeach()
        file(INSTALL \"\${_slr_rootfs}\" DESTINATION \"\${_slr_dest}\")
    endif ()
")

# Keep this build's settings, plug-ins and login apart from a stock OrcaSlicer (or an older
# bridge build) on the same machine: an existing \"data_dir\" folder next to the executable makes
# GUI_App::init_app_config() use it instead of %APPDATA%\OrcaSlicer.
option(SLICER_LINUX_RUNTIME_PORTABLE_DATA_DIR "Create a data_dir folder next to the installed executable" ON)
if (SLICER_LINUX_RUNTIME_PORTABLE_DATA_DIR)
    install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/data_dir\")")
endif ()
