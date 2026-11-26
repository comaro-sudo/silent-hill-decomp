#!/usr/bin/env bash
# Build the Silent Hill Wii U port -> wiiu_port/bin/silenthill_gx2.rpx (or silenthill.rpx)
#
#   bash wiiu_port/build.sh                    # GX2 if CafeGLSL is present, else SDL2
#   WIIU_RENDERER=sdl bash wiiu_port/build.sh  # force SDL2 fallback renderer
#   SKIP_GATE=1 SKIP_MAPS=1 bash wiiu_port/build.sh  # relink only
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/flags.sh" || exit 2

GATE="$SCRIPT_DIR/build/gate"
MAPS="$SCRIPT_DIR/build/maps"
OUT="$SCRIPT_DIR/bin"
mkdir -p "$OUT"

if [ -z "${SKIP_GATE:-}" ]; then
    echo "=== compile ==="
    bash "$SCRIPT_DIR/gate.sh" || exit 1
fi
if [ -z "${SKIP_MAPS:-}" ]; then
    echo; echo "=== map overlays ==="
    bash "$SCRIPT_DIR/maps.sh" || exit 1
fi
MAPLIBS=$(ls "$MAPS"/*.a 2>/dev/null | tr '\n' ' ')

SPECS="$DEVKITPRO/wut/share/wut.specs"
SYSLIBS="-L$DEVKITPRO/portlibs/wiiu/lib -L$DEVKITPRO/portlibs/ppc/lib -L$DEVKITPRO/wut/lib -lSDL2 -lwut -lm"
NAME=silenthill
if [ "$WIIU_RENDERER" = gx2 ]; then
    SYSLIBS="-L$CAFEGLSL/lib -lcafeglsl $SYSLIBS"
    NAME=silenthill_gx2
fi

echo; echo "=== link ($(ls "$GATE"/*.o | wc -l) objects, $(echo $MAPLIBS | wc -w) map archives) ==="
if ! $CXX $MACHDEP -specs="$SPECS" "$GATE"/*.o \
        -Wl,--whole-archive $MAPLIBS -Wl,--no-whole-archive \
        $SYSLIBS \
        -Wl,-Map,"$OUT/$NAME.map" -o "$OUT/$NAME.elf" 2> "$SCRIPT_DIR/build/link.log"; then
    echo "LINK FAILED - undefined symbols by count:"
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" \
        | sort | uniq -c | sort -rn | head -50
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" \
        | sort -u > "$SCRIPT_DIR/build/undefined.txt"
    echo "distinct: $(wc -l < "$SCRIPT_DIR/build/undefined.txt")"
    grep -v "undefined reference\|^collect2\|In function" "$SCRIPT_DIR/build/link.log" | head -20
    exit 1
fi

elf2rpl "$OUT/$NAME.elf" "$OUT/$NAME.rpx" || exit 1
echo
powerpc-eabi-size "$OUT/$NAME.elf"
ls -l "$OUT/$NAME.rpx"
