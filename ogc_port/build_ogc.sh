#!/usr/bin/env bash
# Build the GameCube, Wii or Wii U port -> ogc_port/bin/<platform>/silenthill.dol
# (silenthill.rpx on Wii U)
#
#   PLATFORM=wii  bash ogc_port/build_ogc.sh
#   PLATFORM=cube bash ogc_port/build_ogc.sh
#   SKIP_GATE=1 / SKIP_MAPS=1   relink without recompiling that part
#
# Runs in WSL with devkitPPC + libogc + libfat under /opt/devkitpro.
# Compilation is the gate's and the map script's, so the .dol is exactly what
# they verified.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/ogc_flags.sh" || exit 2

GATE="$SCRIPT_DIR/build/$PLATFORM/gate"
MAPS="$SCRIPT_DIR/build/$PLATFORM/maps"
OUT="$SCRIPT_DIR/bin/$PLATFORM"
mkdir -p "$OUT"

if [ -z "${SKIP_GATE:-}" ]; then
    echo "=== compile ==="
    bash "$SCRIPT_DIR/ogc_gate.sh" || exit 1
fi
if [ -z "${SKIP_MAPS:-}" ]; then
    echo; echo "=== map overlays ==="
    bash "$SCRIPT_DIR/ogc_maps.sh" || exit 1
fi
MAPLIBS=$(ls "$MAPS"/*.a 2>/dev/null | tr '\n' ' ')

case "$PLATFORM" in
    wii)  SPECS="$DEVKITPRO/libogc/share/rvl.specs"; SYSLIBS="-lfat -lwiiuse -lbte -logc -lm" ;;
    cube) SPECS="$DEVKITPRO/libogc/share/ogc.specs"; SYSLIBS="-lfat -logc -lm" ;;
    wiiu) SPECS="$DEVKITPRO/wut/share/wut.specs"
          SYSLIBS="-L$DEVKITPRO/portlibs/wiiu/lib -L$DEVKITPRO/portlibs/ppc/lib -L$DEVKITPRO/wut/lib -lSDL2 -lwut -lm"
          [ "$WIIU_RENDERER" = gx2 ] && SYSLIBS="-L$CAFEGLSL/lib -lcafeglsl $SYSLIBS" ;;
esac
# Wii U: the GX2 renderer links to silenthill_gx2.rpx so both builds can sit side by side
NAME=silenthill
[ "$PLATFORM" = wiiu ] && [ "$WIIU_RENDERER" = gx2 ] && NAME=silenthill_gx2
LIBPATH=""
[ -n "$LIBDIR" ] && LIBPATH="-L$DEVKITPRO/libogc/lib/$LIBDIR"
# GameCube/Wii: start the program at 0x80200000, above the PSX's 2 MB. The map
# loader (map_xbox.c MapXbox_SanitizeHeader) clears every overlay-header word in
# 0x80000000-0x801FFFFF as a leftover raw PSX address; at libogc's default
# 0x80004000 the real function pointers live there too and were all cleared
# (first Dolphin run: ISI at PC 0 from GameBoot_InGameInit -> particlesUpdate).
LOADADDR=""
[ -n "$LIBDIR" ] && LOADADDR="-Wl,--section-start,.init=0x80200000"

WRAP=""
[ -n "${OGC_ALLOC_TRACE:-}" ] && WRAP="-Wl,--wrap=malloc,--wrap=calloc,--wrap=memalign"
echo; echo "=== link ($(ls "$GATE"/*.o | wc -l) objects, $(echo $MAPLIBS | wc -w) map archives) ==="
# --whole-archive for the maps: some map objects are reached only through data
# tables and constructors, never by a symbol the linker is already chasing.
if ! $CXX $MACHDEP -specs="$SPECS" "$GATE"/*.o \
        -Wl,--whole-archive $MAPLIBS -Wl,--no-whole-archive \
        $LIBPATH $SYSLIBS $LOADADDR $WRAP \
        -Wl,-Map,"$OUT/$NAME.map" -o "$OUT/$NAME.elf" 2> "$SCRIPT_DIR/build/$PLATFORM/link.log"; then
    echo "LINK FAILED - undefined symbols by count:"
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/$PLATFORM/link.log" \
        | sort | uniq -c | sort -rn | head -50
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/$PLATFORM/link.log" \
        | sort -u > "$SCRIPT_DIR/build/$PLATFORM/undefined.txt"
    echo "distinct: $(wc -l < "$SCRIPT_DIR/build/$PLATFORM/undefined.txt")"
    grep -v "undefined reference\|^collect2\|In function" "$SCRIPT_DIR/build/$PLATFORM/link.log" | head -20
    exit 1
fi

if [ "$PLATFORM" = wiiu ]; then
    elf2rpl "$OUT/$NAME.elf" "$OUT/$NAME.rpx" || exit 1
    IMG="$OUT/$NAME.rpx"
else
    elf2dol "$OUT/$NAME.elf" "$OUT/$NAME.dol" || exit 1
    IMG="$OUT/$NAME.dol"
fi
echo
powerpc-eabi-size "$OUT/$NAME.elf"
ls -l "$IMG"
