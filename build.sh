#!/bin/sh
# hibiki - build script
#
# Uses pkg-config when available, falls back to Homebrew paths on Apple Silicon.

set -eu

CXX="${CXX:-clang++}"
STD="-std=c++17"
WARN="-Wall -Wextra -Wno-unused-parameter"
OPT="${OPT:--O2}"

if pkg-config --exists libusb-1.0 2>/dev/null; then
    CFLAGS="$(pkg-config --cflags libusb-1.0)"
    LDFLAGS="$(pkg-config --libs libusb-1.0)"
else
    # Homebrew fallback (Apple Silicon default prefix)
    BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"
    LIBUSB_DIR=""
    if [ -d "${BREW_PREFIX}/opt/libusb" ]; then
        LIBUSB_DIR="${BREW_PREFIX}/opt/libusb"
    elif ls -d "${BREW_PREFIX}/Cellar/libusb"/* >/dev/null 2>&1; then
        LIBUSB_DIR="$(ls -d "${BREW_PREFIX}/Cellar/libusb"/* | head -n 1)"
    fi
    if [ -z "${LIBUSB_DIR}" ]; then
        echo "libusb not found. brew install libusb   or   set BREW_PREFIX." >&2
        exit 1
    fi
    CFLAGS="-I${LIBUSB_DIR}/include/libusb-1.0"
    LDFLAGS="-L${LIBUSB_DIR}/lib -lusb-1.0"
fi

set -x
${CXX} ${STD} ${WARN} ${OPT} hibiki.cpp -o hibiki ${CFLAGS} ${LDFLAGS}
