#!/usr/bin/env bash
# Build MapBaker on Linux as a Windows x64 PE (clang-cl + xwin + lld-link).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/linux-clang-cl}"
JOBS="${JOBS:-$(nproc)}"
XWIN_ROOT="${XWIN_ROOT:-/media/storage/toolchains/xwin}"
CCACHE_DIR="${CCACHE_DIR:-/media/storage/ccache}"

export CCACHE_DIR
mkdir -p "$CCACHE_DIR"

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/Tools/MapBaker/linux/toolchain-clang-cl-xwin.cmake" \
    -DXWIN_ROOT="$XWIN_ROOT" \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL \
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=OFF

cmake --build "$BUILD_DIR" --target MapBaker --parallel "$JOBS"

echo "MapBaker: $ROOT/bin/Win/MapBaker64.exe"
