#!/bin/sh
# hibiki - build script (CMake wrapper).
#
# Configures + builds with CMake into build/, then copies the CLI to ./hibiki
# so the existing `./hibiki ...` workflow keeps working. For a pure CMake flow:
#   cmake -S . -B build && cmake --build build

set -eu

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" -j

# Keep the CLI at the repo root (matches the pre-CMake layout / .gitignore).
cp "${BUILD_DIR}/hibiki" ./hibiki
echo "built ./hibiki"
