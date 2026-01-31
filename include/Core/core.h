#pragma once

#include <algorithm>
#include <atomic> // FIX: Added missing atomic include (Bug #3)
#include <bit>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex> // FIX: Added for thread safety fixes
#include <ranges>
#include <span>
#include <sstream> // FIX: Added for logging
#include <stack>
#include <string>
#include <thread>
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

#define LOG_ALLOCATOR(Level, Message)                                          \
  do {                                                                         \
    std::ostringstream oss;                                                    \
    oss << "[" << Level << "] " << Message << "\n";                            \
    std::lock_guard<std::mutex> lock(Allocator::g_LogMutex);                   \
    std::cout << oss.str() << std::flush;                                      \
  } while (0)
#else
#define LOG_ALLOCATOR(Level, Message) ((void)0)
#endif
#endif
