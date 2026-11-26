#pragma once

// Must be defined before CommonLib/SKSE headers transitively include Windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// SkyLine supports Skyrim SE and AE, not VR. CommonLibSSE-NG enables VR by
// default, which makes SKSEPlugin_Query reject non-VR runtimes when this macro
// leaks through the public CommonLib target definitions.
#if defined(ENABLE_SKYRIM_VR)
#	error "SkyLine is an SE/AE plugin. Build CommonLibSSE-NG with ENABLE_SKYRIM_VR=OFF."
#endif

#include <spdlog/sinks/basic_file_sink.h>

#include <Windows.h>
#include <psapi.h>
#include <windowsx.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <unordered_map>

#define DLLEXPORT __declspec(dllexport)

// A hook passing a call on jumps instead of calling, so a stack sampler never finds SkyLine above the real function.
#if defined(__clang__)
#	define TAIL_CALL [[clang::musttail]]
#else
#	define TAIL_CALL
#endif

namespace logger = SKSE::log;

using namespace std::literals;
