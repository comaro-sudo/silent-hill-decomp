#!/usr/bin/env bash
# Native Windows ARM64 (Snapdragon / CLANGARM64) build of the Silent Hill PC port.
# Run from an MSYS2 CLANGARM64 shell:
#   bash pc_port/build-arm64.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
mkdir -p build

echo "== configure =="
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSH_BUILD_MAP_DLLS=ON \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++

echo "== build =="
cmake --build build

echo "== done =="
ls -lh build/SilentHillPC.exe
