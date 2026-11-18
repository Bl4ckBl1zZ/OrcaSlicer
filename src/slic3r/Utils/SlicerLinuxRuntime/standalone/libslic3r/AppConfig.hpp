#pragma once

// Stand-in for libslic3r/AppConfig.hpp for the two targets that share the plug-in ABI headers
// with the application without linking libslic3r: the runtime forwarder module
// (slicer_linux_runtime) and the standalone Linux host (tools/slicer_linux_runtime_host).
// src/slic3r/Utils/bambu_networking.hpp includes the real AppConfig.hpp for one macro only;
// both targets list this directory first on their include path so that include resolves here.
// Keep the values in step with libslic3r/AppConfig.hpp.

#ifndef BAMBU_NETWORK_AGENT_VERSION_LEGACY
#if defined(_MSC_VER) || defined(_WIN32)
#define BAMBU_NETWORK_AGENT_VERSION_LEGACY "01.10.01.09"
#else
#define BAMBU_NETWORK_AGENT_VERSION_LEGACY "01.10.01.01"
#endif
#endif
