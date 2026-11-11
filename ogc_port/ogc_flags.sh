#!/usr/bin/env bash
# ogc_flags.sh - the ONE compiler setup for the GameCube/Wii build, sourced by
# ogc_gate.sh, ogc_maps.sh and build_ogc.sh so a game TU and a map TU can never
# be compiled two different ways ("gate green, link red").
#   needs: SCRIPT_DIR (ogc_port/), PLATFORM (wii|cube)
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
PLATFORM="${PLATFORM:-wii}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"
CC=powerpc-eabi-gcc
CXX=powerpc-eabi-g++

case "$PLATFORM" in
    wii)  MACHDEP="-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float"; PDEF="-DSH_WII_PORT"; LIBDIR=wii ;;
    cube) MACHDEP="-DGEKKO -mogc -mcpu=750 -meabi -mhard-float"; PDEF="-DSH_GC_PORT";  LIBDIR=cube ;;
    *) echo "PLATFORM must be wii or cube"; exit 2 ;;
esac


DEFS="-DSH_OGC_PORT $PDEF -DSH_XBOX360_PORT -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"
# PsyX_render.h picks a renderer from the host OS macros; xenon-gcc gets
# RENDERER_OGL through __linux__ and MSVC through _WIN32. devkitPPC defines
# neither. Without USE_GLAD this only selects the TextureID/ShaderID typedefs.
DEFS="$DEFS -DRENDERER_OGL"

# ogc_port/include first (fs_xbox360.h forwarder etc.), then the 360 and Xbox
# shim headers, then the PC port, which shadows include/.
INCS="-I$SCRIPT_DIR/include -I$DECOMP/xbox360_port/include
      -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include
      -I$DECOMP/xbox_port/include
      -I$DEVKITPRO/libogc/include"

WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign
      -Wno-builtin-declaration-mismatch -Wno-discarded-qualifiers"
# gcc 14+ made several of the decomp's K&R-isms hard errors; -fpermissive
# turns them back into warnings for C.
CFLAGS_C="-std=gnu11 -fpermissive -fgnu89-inline -Dstatic_assert=_Static_assert"
# Game TUs only (not ogc_port/src): see ogc_port/include/ogc_prelude.h.
PRELUDE="-include $SCRIPT_DIR/include/ogc_prelude.h"
# -O2 needs these two, exactly as on the Original Xbox: the decomp type-puns
# constantly and relies on wrapping signed overflow.
OPT="${SH_OPT:--O2 -fno-strict-aliasing -fwrapv}"
FLAGS="$MACHDEP -ffunction-sections -fdata-sections $OPT"
compile_one() {
    local f="$1" obj="$2" extra="$3" tool std err
    case "$f" in
        *.cpp) tool="$CXX"; std="-std=gnu++17 -fno-exceptions -fno-rtti" ;;
        *)     tool="$CC";  std="$CFLAGS_C" ;;
    esac
    case "$f" in
        */ogc_port/src/*) ;;
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
