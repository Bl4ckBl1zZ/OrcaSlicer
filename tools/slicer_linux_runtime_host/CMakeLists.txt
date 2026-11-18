cmake_minimum_required(VERSION 3.16)
project(slicer_linux_runtime_host_standalone LANGUAGES C CXX)

# Standalone build of the Linux runtime host: the process that runs inside the WSL2 distro,
# loads Bambu's Linux libbambu_networking.so / libBambuSource.so and serves the Windows
# forwarder module (slicer_linux_runtime.dll) over stdio RPC.
#
#   cmake -S tools/slicer_linux_runtime_host -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target slicer_linux_runtime_host
#   tools/slicer_linux_runtime_host/package_linux_host_runtime.sh build
#
# Two variants are built from the same sources, one per libstdc++ string ABI, because the
# proprietary component is called through std::string/std::function by value. The wrapper
# script picks the one that works with the installed component.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

option(SLICER_LINUX_RUNTIME_WITH_CURL
    "Build the host's own HTTP transport (http.* RPC methods). Not used by the application." OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG QUIET)
if (SLICER_LINUX_RUNTIME_WITH_CURL)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
endif()

set(HOST_SOURCES
    "${REPO_ROOT}/tools/slicer_linux_runtime_host/main.cpp"
    "${REPO_ROOT}/tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp"
    "${REPO_ROOT}/src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.cpp"
    "${REPO_ROOT}/src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcProtocol.cpp"
)

function(add_host_variant target_name abi_value)
    add_executable(${target_name} ${HOST_SOURCES})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/slicer_linux_runtime_host"
        OUTPUT_NAME ${target_name}
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
    # The stand-in directory must come first: it replaces libslic3r/AppConfig.hpp, which
    # src/slic3r/Utils/bambu_networking.hpp includes, so the host does not need libslic3r.
    target_include_directories(${target_name} PRIVATE
        "${REPO_ROOT}/src/slic3r/Utils/SlicerLinuxRuntime/standalone"
        "${REPO_ROOT}"
        "${REPO_ROOT}/src"
        "${REPO_ROOT}/deps_src"
    )
    target_compile_definitions(${target_name} PRIVATE
        SLICER_LINUX_RUNTIME_STANDALONE_HOST=1
        SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS=1
        _GLIBCXX_USE_CXX11_ABI=${abi_value}
    )
    if (TARGET nlohmann_json::nlohmann_json)
        target_link_libraries(${target_name} PRIVATE nlohmann_json::nlohmann_json)
    endif()
    if (SLICER_LINUX_RUNTIME_WITH_CURL)
        target_compile_definitions(${target_name} PRIVATE SLICER_LINUX_RUNTIME_WITH_CURL=1)
        target_link_libraries(${target_name} PRIVATE CURL::libcurl ZLIB::ZLIB OpenSSL::SSL)
    endif()
    target_link_libraries(${target_name} PRIVATE
        OpenSSL::Crypto
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # The runtime rootfs is a bare distro image; do not depend on its libstdc++.
        target_link_options(${target_name} PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endfunction()

add_host_variant(slicer_linux_runtime_host_abi1 1)
add_host_variant(slicer_linux_runtime_host_abi0 0)
add_custom_target(slicer_linux_runtime_host DEPENDS slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0)
