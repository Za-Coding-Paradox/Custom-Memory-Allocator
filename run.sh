#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# ==============================================================================
# PARSE ARGUMENTS
# ==============================================================================
# Default values
MODE="release"
TARGET="test"

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
    EXE_NAME="allocator_test"
elif [ "$TARGET" == "benchmark" ]; then
    EXE_NAME="allocator_benchmark"
else
    echo "Error: Invalid target '$TARGET'. Use 'test' or 'benchmark'."
    exit 1
fi

# ==============================================================================
# BUILD AND RUN
# ==============================================================================
echo "============================================================"
echo " Building in $CMAKE_BUILD_TYPE mode..."
echo " Target: $EXE_NAME"
echo " Directory: $BUILD_DIR"
echo "============================================================"

# 1. Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# 2. Configure CMake (only configures what has changed)
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"

# 3. Build the specific target using all available CPU cores
cmake --build "$BUILD_DIR" --target "$EXE_NAME" -j $(nproc)

echo ""
echo "============================================================"
echo " Running $EXE_NAME..."
echo "============================================================"
echo ""

# 4. Execute the binary
"./$BUILD_DIR/$EXE_NAME"
