#!/bin/sh
# hibiki - build script (CMake wrapper).
#
# Configures + builds with CMake into build/. Produces the CLI (build/hibiki)
# and, when SDL3/FFmpeg are present, the macOS app bundle build/Hibiki.app.
# For a pure CMake flow:  cmake -S . -B build && cmake --build build

set -eu

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" -j

# Keep the CLI at the repo root (matches the pre-CMake layout / .gitignore).
cp "${BUILD_DIR}/hibiki" ./hibiki

if [ -d "${BUILD_DIR}/Hibiki.app" ]; then
  rm -rf ./Hibiki.app
  cp -R "${BUILD_DIR}/Hibiki.app" ./Hibiki.app
  ln -sf Hibiki.app/Contents/MacOS/Hibiki ./hibiki-tv
  echo "built ./hibiki ./Hibiki.app ./hibiki-tv"
else
  echo "built ./hibiki (SDL front-end skipped)"
fi
