cmake_minimum_required(VERSION 3.14)
project(Custom_Memory_Allocator CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# 1. SETUP GOOGLETEST (SYSTEM PACKAGE)
# ==============================================================================
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# ==============================================================================
# 2. YOUR LIBRARY (The Allocator)
# ==============================================================================
# Assuming your headers are in the 'include' directory
include_directories(include)

# Gather all source files for the allocator
file(GLOB_RECURSE SOURCES "src/modules/*.cpp" "src/utilities/*.cpp")

add_library(allocator ${SOURCES})
target_include_directories(allocator PUBLIC include)

# ==============================================================================
# 3. EXECUTABLES
# ==============================================================================

# Executable 1: The Core Tests
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cpp")
    add_executable(allocator_test tests/tests.cpp)
    target_link_libraries(allocator_test 
        PRIVATE 
        allocator 
        GTest::GTest 
        GTest::Main 
        pthread
    )
endif()

# Executable 2: The Benchmarks
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks.cpp")
    add_executable(allocator_benchmark tests/benchmarks.cpp)
    target_link_libraries(allocator_benchmark 
        PRIVATE 
        allocator 
        GTest::GTest 
        GTest::Main 
        pthread
    )
endif()
