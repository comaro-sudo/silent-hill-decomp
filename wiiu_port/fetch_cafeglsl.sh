#!/usr/bin/env bash
# Download CafeGLSL (Exzap, MIT; runtime GLSL -> GX2 shader compiler) from its
# official GitHub release into wiiu_port/cafeglsl/ (include/, lib/libcafeglsl.a).
# The .a is 21 MB and not committed. Needed by WIIU_RENDERER=gx2.
set -eu
VER="${CAFEGLSL_VER:-1.0.0-rc1}"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cafeglsl"
URL="https://github.com/Exzap/CafeGLSL/releases/download/v$VER/cafeglsl-$VER-wiiu-ppc.tar.xz"
mkdir -p "$DEST"
curl -fL "$URL" | tar -xJ -C "$DEST" --strip-components=1
ls -l "$DEST/lib/libcafeglsl.a" "$DEST/include/cafeglsl/CafeGLSLCompiler.h"
