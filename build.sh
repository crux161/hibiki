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

# Keep the binaries at the repo root (matches the pre-CMake layout / .gitignore).
cp "${BUILD_DIR}/hibiki" ./hibiki
if [ -f "${BUILD_DIR}/hibiki_sdl" ]; then
  cp "${BUILD_DIR}/hibiki_sdl" ./hibiki_sdl
  echo "built ./hibiki ./hibiki_sdl"
else
  echo "built ./hibiki"
fi
