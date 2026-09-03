#!/bin/bash
# Build script for pacmkr with GTK4 GUI support
# Requires: gtkmm-4.0 and libalpm

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-gui"

echo "=== Building pacmkr GTK GUI ==="
echo ""

# Check for required dependencies
echo "Checking dependencies..."

if ! command -v cmake &> /dev/null; then
    echo "ERROR: cmake not found. Please install cmake."
    exit 1
fi

if ! pkg-config --exists gtkmm-4.0 2>/dev/null; then
    echo "ERROR: gtkmm-4.0 not found. Please install the GTK4 C++ bindings."
    echo "  Arch: sudo pacman -S gtkmm-4.0"
    echo "  Debian/Ubuntu: sudo apt install libgtkmm-4.0-dev"
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
    -DPACMKR_BUILD_GTK_GUI=ON \
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
