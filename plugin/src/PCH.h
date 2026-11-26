#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#	define NOMINMAX
#endif

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
