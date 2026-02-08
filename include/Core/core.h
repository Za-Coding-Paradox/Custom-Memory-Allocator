#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <ranges>
#include <source_location>
#include <span>
#include <sstream>
#include <stack>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// ============================================================================
// LOGGING SYSTEM REDESIGN
// ============================================================================

#ifndef LOG_ALLOCATOR
// If NDEBUG is defined (Standard for Release builds), erase all logging.
// This removes the stringstream and mutex overhead from the hot path.
#ifdef NDEBUG
#define LOG_ALLOCATOR(Level, Message) ((void)0)
#else
namespace Allocator {
inline std::mutex g_LogMutex;
}

#define LOG_ALLOCATOR(Level, Message)                            \
    do {                                                         \
        std::ostringstream oss;                                  \
        oss << "[" << Level << "] " << Message << "\n";          \
        std::lock_guard<std::mutex> lock(Allocator::g_LogMutex); \
        std::cout << oss.str() << std::flush;                    \
    } while (0)
#endif
#endif

// ============================================================================
// PERFORMANCE MACROS
// ============================================================================

// Use these to wrap expensive diagnostic tracking (like O(N) peak usage checks)
// so they only run during development/torture tests.
#ifndef NDEBUG
#define ALLOCATOR_DIAGNOSTIC(code) code
#else
#define ALLOCATOR_DIAGNOSTIC(code) code
// write ((void)0) for maximum efficiency.
// write code for optimal testing
#endif

#ifdef NDEBUG
// Release Mode: Zero overhead.
#define ALLOCATOR_ASSERT(condition, message) ((void)0)
#else
// Debug Mode: Detailed validation.
#define ALLOCATOR_ASSERT(condition, message) assert((condition) && message)
#endif
