#!/usr/bin/env bash
# flags.sh - shared compiler setup for the Wii U port, sourced by
# gate.sh, maps.sh and build.sh so game TUs and map TUs always match.
#   needs: SCRIPT_DIR (wiiu_port/)
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"
CC=powerpc-eabi-gcc
CXX=powerpc-eabi-g++

MACHDEP="-mcpu=750 -meabi -mhard-float -D__WIIU__ -D__WUT__"
PDEF="-DSH_WIIU_PORT"

# Renderer: gx2 (native GX2 + CafeGLSL shaders, wiiu_port/src/gpu_gx2.c)
# or sdl (SDL_RenderGeometry fallback, wiiu_port/src/gpu_sdl_wiiu.c).
# Run wiiu_port/fetch_cafeglsl.sh once to download CafeGLSL into wiiu_port/cafeglsl.
CAFEGLSL="${CAFEGLSL:-$SCRIPT_DIR/cafeglsl}"
if [ -z "${WIIU_RENDERER:-}" ]; then
    if [ -f "$CAFEGLSL/lib/libcafeglsl.a" ]; then WIIU_RENDERER=gx2; else WIIU_RENDERER=sdl; fi
fi
if [ "$WIIU_RENDERER" = gx2 ] && [ ! -f "$CAFEGLSL/lib/libcafeglsl.a" ]; then
    echo "WIIU_RENDERER=gx2 needs CafeGLSL: run wiiu_port/fetch_cafeglsl.sh (looked in $CAFEGLSL)"
    exit 2
fi

SYSINC="-I$DEVKITPRO/wut/include -I$DEVKITPRO/portlibs/wiiu/include
        -I$DEVKITPRO/portlibs/wiiu/include/SDL2 -I$DEVKITPRO/portlibs/ppc/include"
RDEF=""
if [ "$WIIU_RENDERER" = gx2 ]; then
    SYSINC="$SYSINC -I$CAFEGLSL/include"
    RDEF="-DSH_WIIU_GX2"
fi

DEFS="$RDEF $PDEF -DSH_XBOX360_PORT -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0 -DRENDERER_OGL"

# wiiu_port/include first (ppc/timebase.h, wiiu_prelude.h), then 360/Xbox/PC headers.
INCS="-I$SCRIPT_DIR/include -I$DECOMP/xbox360_port/include
      -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include
      -I$DECOMP/xbox_port/include
      $SYSINC"

WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign
      -Wno-builtin-declaration-mismatch -Wno-discarded-qualifiers"

CFLAGS_C="-std=gnu11 -fpermissive -fgnu89-inline -Dstatic_assert=_Static_assert"
PRELUDE="-include $SCRIPT_DIR/include/wiiu_prelude.h"
OPT="${SH_OPT:--O2 -fno-strict-aliasing -fwrapv}"
FLAGS="$MACHDEP -fsigned-char -ffunction-sections -fdata-sections $OPT"

compile_one() {
    local f="$1" obj="$2" extra="$3" tool std err
    case "$f" in
        *.cpp) tool="$CXX"; std="-std=gnu++17 -fno-exceptions -fno-rtti" ;;
        *)     tool="$CC";  std="$CFLAGS_C" ;;
    esac
    case "$f" in
        # Game-side Wii U files read decomp state: compiled like game code
        */wiiu_port/src/*_game.c) std="$std $PRELUDE" ;;
        # Real SDL2 headers must beat the game's SDL_*.h shims in xbox_port/include
        */wiiu_port/src/*) std="$std -I$DEVKITPRO/portlibs/wiiu/include/SDL2" ;;
        *.c) std="$std $PRELUDE" ;;
    esac
    if err=$("$tool" $FLAGS $std $DEFS $INCS $WARN $extra -c "$f" -o "$obj" 2>&1); then
        [ -n "$err" ] && { echo "########## $f"; echo "$err"; } > "$obj.warn"
        return 0
    fi
    { echo "########## $f"; echo "$err"; } > "$obj.err"
    rm -f "$obj"
}
export -f compile_one
export CC CXX FLAGS CFLAGS_C PRELUDE DEFS INCS WARN
