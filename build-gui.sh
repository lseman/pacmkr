#!/bin/bash
# Build script for pacmkr with Slint GUI support
# Requires: Qt6, libalpm, internet connection (for CPM.cmake/Slint download)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-gui"

echo "=== Building pacmkr with Slint GUI ==="
echo ""

# Check for required dependencies
echo "Checking dependencies..."

if ! command -v cmake &> /dev/null; then
    echo "ERROR: cmake not found. Please install cmake."
    exit 1
fi

if ! pkg-config --exists Qt6Core 2>/dev/null; then
    echo "ERROR: Qt6 not found. Please install Qt6 development packages."
    echo "  Arch: sudo pacman -S qt6-base"
    echo "  Debian/Ubuntu: sudo apt install qt6-base-dev"
    exit 1
fi

if ! pkg-config --exists alpm 2>/dev/null; then
    echo "ERROR: libalpm not found. Please install libalpm."
    echo "  Arch: sudo pacman -S alpm"
    exit 1
fi

echo "✓ All dependencies found"
echo ""

# Clean and create build directory
echo "Setting up build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure with CMake (enables GUI)
echo "Configuring project with CMake..."
cmake .. \
    -DPACMKR_BUILD_GUI=ON \
    -DPACMKR_BUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_CXX_STANDARD=17"

echo ""
echo "Building pacmkr-gui..."
make -j$(nproc)

echo ""
echo "=== Build complete ==="
echo ""
echo "Run the GUI with:"
echo "  ${BUILD_DIR}/pacmkr-gui"
echo ""
echo "Or run tests with:"
echo "  cd ${BUILD_DIR} && ctest --output-on-failure"
