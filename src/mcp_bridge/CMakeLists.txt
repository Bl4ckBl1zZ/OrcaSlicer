# orca-mcp-bridge: stdio <-> Streamable HTTP forwarder for OrcaSlicer's MCP server.
#
# Built two ways:
#  * as part of the OrcaSlicer tree (ORCA_MCP_BRIDGE, top-level CMakeLists.txt): links the tree's
#    boost_headeronly target and lands next to orca-slicer in the build and install trees;
#  * on its own, for quick iteration and portability checks:
#      cmake -S src/mcp_bridge -B build-bridge -DCMAKE_PREFIX_PATH=<deps prefix or system>
#      cmake --build build-bridge --config Release
#
# Header-only Boost (Asio, Beast, JSON in header-only mode); no other dependency.
cmake_minimum_required(VERSION 3.13)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(orca_mcp_bridge LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release)
    endif ()
    set(_orca_mcp_bridge_standalone ON)
    option(ORCA_MCP_BRIDGE_WERROR "Treat warnings as errors" OFF)
else ()
    set(_orca_mcp_bridge_standalone OFF)
endif ()

add_executable(orca-mcp-bridge
    BridgeCommon.hpp
    Bridge.cpp
    Endpoint.cpp
    JsonImpl.cpp
    McpHeaders.cpp
    Platform.cpp
    main.cpp
)

if (TARGET boost_headeronly)
    target_link_libraries(orca-mcp-bridge PRIVATE boost_headeronly)
else ()
    if (POLICY CMP0167)
        cmake_policy(SET CMP0167 NEW)
    endif ()
    find_package(Boost 1.75 REQUIRED CONFIG)
    target_link_libraries(orca-mcp-bridge PRIVATE Boost::headers)
endif ()

find_package(Threads REQUIRED)
target_link_libraries(orca-mcp-bridge PRIVATE Threads::Threads)

# Header-only Boost.System/JSON; no auto-linking of compiled Boost libraries on MSVC.
target_compile_definitions(orca-mcp-bridge PRIVATE BOOST_ALL_NO_LIB BOOST_JSON_NO_LIB BOOST_CONTAINER_NO_LIB)

if (WIN32)
    target_compile_definitions(orca-mcp-bridge PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(orca-mcp-bridge PRIVATE ws2_32 mswsock shell32 ole32)
endif ()

if (MSVC)
    # C4702 (unreachable code) is reported from inlined Asio code and cannot be silenced per header.
    target_compile_options(orca-mcp-bridge PRIVATE /bigobj /utf-8 /permissive- /wd4702)
    target_compile_definitions(orca-mcp-bridge PRIVATE _CRT_SECURE_NO_WARNINGS)
    if (_orca_mcp_bridge_standalone)
        string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        target_compile_options(orca-mcp-bridge PRIVATE /W4)
        if (ORCA_MCP_BRIDGE_WERROR)
            target_compile_options(orca-mcp-bridge PRIVATE /WX)
        endif ()
    endif ()
elseif (_orca_mcp_bridge_standalone)
    target_compile_options(orca-mcp-bridge PRIVATE -Wall -Wextra)
    if (ORCA_MCP_BRIDGE_WERROR)
        target_compile_options(orca-mcp-bridge PRIVATE -Werror)
    endif ()
endif ()

if (NOT _orca_mcp_bridge_standalone)
    # Next to orca-slicer in the build tree (the OrcaSlicer target is built in src/).
    set_target_properties(orca-mcp-bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
    if (APPLE AND CMAKE_MACOSX_BUNDLE AND TARGET OrcaSlicer)
        # macOS: inside OrcaSlicer.app/Contents/MacOS, so it ships (and installs) with the bundle.
        # Copied after the app is linked (its bundle exists then and nothing rewrites the copy), and
        # signed ad hoc in place: Apple Silicon does not run unsigned code, and a copy that lost the
        # linker's signature would be killed on launch. Release signing (CI) re-signs every Mach-O of
        # the bundle with the real identity afterwards.
        add_dependencies(orca-mcp-bridge OrcaSlicer)
        find_program(ORCA_CODESIGN codesign)
        set(_bridge_in_bundle "$<TARGET_FILE_DIR:OrcaSlicer>/$<TARGET_FILE_NAME:orca-mcp-bridge>")
        if (ORCA_CODESIGN)
            set(_bridge_sign COMMAND "${ORCA_CODESIGN}" --force --sign - --timestamp=none "${_bridge_in_bundle}")
        else ()
            set(_bridge_sign "")
        endif ()
        add_custom_command(TARGET orca-mcp-bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OrcaSlicer>"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:orca-mcp-bridge>" "$<TARGET_FILE_DIR:OrcaSlicer>/"
            ${_bridge_sign}
            VERBATIM)
    elseif (WIN32)
        install(TARGETS orca-mcp-bridge RUNTIME DESTINATION ".")
    else ()
        install(TARGETS orca-mcp-bridge RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif ()
endif ()
