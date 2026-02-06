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

#ifndef LOG_ALLOCATOR
#ifdef ALLOCATOR_ENABLE_LOGGING
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
#else
#define LOG_ALLOCATOR(Level, Message) ((void)0)
#endif
#endif
