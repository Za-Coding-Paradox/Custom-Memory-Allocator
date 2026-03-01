#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# ==============================================================================
# PARSE ARGUMENTS
# ==============================================================================
# Default values: Build 'release' mode and 'all' targets
MODE="release"
TARGET="all"

# Override with user arguments if provided
if [ "$1" != "" ]; then MODE=$(echo "$1" | tr '[:upper:]' '[:lower:]'); fi
if [ "$2" != "" ]; then TARGET=$(echo "$2" | tr '[:upper:]' '[:lower:]'); fi

# Validate Mode
if [ "$MODE" == "debug" ]; then
    CMAKE_BUILD_TYPE="Debug"
    BUILD_DIR="build/debug"
elif [ "$MODE" == "release" ]; then
    CMAKE_BUILD_TYPE="Release"
    BUILD_DIR="build/release"
else
    echo "Error: Invalid mode '$MODE'. Use 'debug' or 'release'."
    exit 1
fi

# Validate Target
if [ "$TARGET" == "test" ]; then
    TARGET_NAME="allocator_test"
elif [ "$TARGET" == "benchmark" ]; then
    TARGET_NAME="allocator_benchmark"
elif [ "$TARGET" == "all" ]; then
    TARGET_NAME="all"
else
    echo "Error: Invalid target '$TARGET'. Use 'test', 'benchmark', or 'all'."
    exit 1
fi

# ==============================================================================
# BUILD PROCESS
# ==============================================================================
echo "============================================================"
echo " Building in $CMAKE_BUILD_TYPE mode..."
echo " Target(s): $TARGET_NAME"
echo " Directory: $BUILD_DIR"
echo "============================================================"

# 1. Create the specific build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# 2. Configure CMake
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"

# 3. Build target(s) using all available CPU cores
if [ "$TARGET_NAME" == "all" ]; then
    # Builds everything configured in CMakeLists.txt
    cmake --build "$BUILD_DIR" -j $(nproc)
else
    # Builds only the specific target
    cmake --build "$BUILD_DIR" --target "$TARGET_NAME" -j $(nproc)
fi

echo ""
echo "============================================================"
echo " Build Complete! Binaries are ready in $BUILD_DIR/"
echo "============================================================"
