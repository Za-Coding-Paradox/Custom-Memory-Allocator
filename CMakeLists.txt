cmake_minimum_required(VERSION 3.14)
project(Custom_Memory_Allocator CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# 1. SETUP GOOGLETEST (SYSTEM PACKAGE)
# ==============================================================================
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ==============================================================================
# 2. YOUR LIBRARY (The Allocator)
# ==============================================================================
include_directories(include)

# Gather source files
file(GLOB_RECURSE SOURCES "src/modules/*.cpp" "src/utilities/*.cpp")

add_library(allocator ${SOURCES})
target_include_directories(allocator PUBLIC src/include)

# ==============================================================================
# 3. YOUR EXECUTABLE
# ==============================================================================
add_executable(allocator_test 
   tests/tests.cpp 
)

# LINKING:
# Link against the imported targets provided by find_package
target_link_libraries(allocator_test 
    PRIVATE 
    allocator 
    GTest::GTest 
    GTest::Main 
    pthread
)
