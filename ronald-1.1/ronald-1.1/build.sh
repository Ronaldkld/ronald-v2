#!/usr/bin/env bash
# Convenience script to cross-compile the whole project (core DLL, GUI
# host, console example and the manual test) into Windows x64 binaries
# using MinGW-w64. Works on Linux/macOS with mingw-w64 installed, and
# is also safe to run on Windows with a MinGW toolchain in PATH.
#
# Usage:
#   ./build.sh              # Release build into ./build/bin
#   ./build.sh clean        # Remove the build directory first
set -euo pipefail

cd "$(dirname "$0")"

if [ "${1:-}" = "clean" ]; then
    rm -rf build
fi

TOOLCHAIN_ARGS=()
if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    TOOLCHAIN_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake")
fi

cmake -B build "${TOOLCHAIN_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Build finished. Binaries are in build/bin:"
ls -la build/bin
