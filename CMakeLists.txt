cmake_minimum_required(VERSION 3.20)

# --- 1. FORCE CLANG COMPILER (Must be before project defined) ---
set(CMAKE_C_COMPILER   "clang")
set(CMAKE_CXX_COMPILER "clang++")

project(CustomAllocator VERSION 1.0 LANGUAGES CXX)

# --- 2. Standard Configuration ---
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 3. Compiler Flags ---
add_compile_options(-Wall -Wextra -Wpedantic -Werror)
add_compile_definitions(ALLOCATOR_ENABLE_LOGGING)

# --- 4. AUTOMATIC FILE DISCOVERY (The Magic Part) ---
# This finds ALL .cpp files in src/ and its subfolders automatically.
# You don't need to list them manually anymore.
file(GLOB_RECURSE ALLOCATOR_SOURCES "src/*.cpp")

# --- 5. Define Library ---
add_library(allocator STATIC ${ALLOCATOR_SOURCES})

# --- 6. Global Include Paths ---
# This ensures you can include files relative to 'src' or 'include'
# from anywhere in the project.
target_include_directories(allocator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# --- 7. Executable ---
add_executable(allocator_test main/main.cpp)
target_link_libraries(allocator_test PRIVATE allocator)

# --- 8. Platform Linking ---
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(allocator PRIVATE Threads::Threads)
endif()

message(STATUS "---------------------------------------------------")
message(STATUS " [AUTO] Sources Found: ${ALLOCATOR_SOURCES}")
message(STATUS " [CONF] Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "---------------------------------------------------")
