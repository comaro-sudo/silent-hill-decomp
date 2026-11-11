#!/usr/bin/env bash
# GameCube / Wii compile gate: build every TU of the game tree with devkitPPC
# (powerpc-eabi-gcc, 32-bit big-endian, libogc) and report failures.
#
# Source list = the 360 XDK gate's (xbox360_port/xdk/gate.sh), which is the
# build that plays through the intro, with the XDK HAL swapped for ogc_port/src.
# The game-side big-endian work all rides on SH_XBOX360_PORT, so that define
# stays on: this is "a 32-bit big-endian console HAL", which is what it means.
#
#   PLATFORM=wii  ./ogc_gate.sh       # default
#   PLATFORM=cube ./ogc_gate.sh
#   ./ogc_gate.sh src/main            # restrict to a subtree
#   SH_OPT='-O0' ./ogc_gate.sh        # override optimisation (default -O2)
#   JOBS=8 ./ogc_gate.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/ogc_flags.sh" || exit 2

OUT="$SCRIPT_DIR/build/$PLATFORM/gate"
LOG="$SCRIPT_DIR/build/$PLATFORM/gate.log"
WARNLOG="$SCRIPT_DIR/build/$PLATFORM/gate_warnings.log"
mkdir -p "$OUT"
# Not incremental: a stale object whose source was renamed would keep linking.
rm -f "$OUT"/*.o "$OUT"/*.err "$OUT"/*.warn
: > "$LOG"; : > "$WARNLOG"
collect_srcs() {
    local root="${1:-}"
    if [ -n "$root" ]; then
        find "$DECOMP/$root" -name '*.c'
        return
    fi
    find "$DECOMP/src/main" -maxdepth 1 -name '*.c' ! -name 'main.c' ! -name 'memcpy.c'
    find "$DECOMP/src/bodyprog" -name '*.c' ! -name 'bodyprog_80032D1C.c' ! -name 'text_draw_jp.c'
    find "$DECOMP/src/screens" -name '*.c' ! -name 'hp_safe1.c' ! -name 's__safe2.c'
    local excl="main_pc pc_combat pc_console_cmd pc_crash pc_quicksave dbg_overlay
                warning_screen hires_override fs_pc dll_loader map_overlay_loader
                map_registry xa_player control_style pc_mouse_cursor combat_target
                miniz tex_pack map7_s03_boss_motion"
    local pat=""
    for f in $excl; do pat="$pat ! -name $f.c"; done
    find "$PCPORT/src" -maxdepth 1 -name '*.c' $pat
    find "$PCPORT/src/stubs" -name '*.c' ! -name 'map_overlay_stub.c'
    find "$DECOMP/src/maps/map0_s00" -name '*.c'
    ls "$PCPORT/build_gen/extracted_data/map0_s00_extracted_data.c" 2>/dev/null
    # Shared 360 HAL that is plain C (CD sector reads off the .bin, the GPU
    # self-test picture). The libXenon/XDK-bound ones are replaced by ogc_port/src.
    echo "$DECOMP/xbox360_port/src/cd_xbox360.c"
    echo "$DECOMP/xbox360_port/src/gpu_selftest.c"
    find "$SCRIPT_DIR/src" -name '*.c'
    for f in "$DECOMP"/xbox_port/src/*.c; do
        case "$(basename "$f")" in
            crash_xbox.c|dbg_overlay_xbox.c|dsound_bridge.c|dsound_xbox.c|\
            gpu_nv2a.c|net_xbox.c|pad_xbox.c|ra_badge_xbox.c|sdl_compat_xbox.c|\
            sh_log_xbox.c|cd_xbox.c|fs_xbox.c|main_xbox.c|\
            ra_xbox.c|msvc_compat.c) ;;
            *) echo "$f" ;;
        esac
    done
    echo "$PCPORT/src/fmv/mdec.c"
    echo "$PCPORT/src/fmv/str_demux.c"
    echo "$PSYCROSS/src/psx/inline_c.c"
    echo "$PSYCROSS/src/psx/libgte.c"
    echo "$PSYCROSS/src/psx/abs.c"
    echo "$PSYCROSS/src/gte/PsyX_GTE.cpp"
}


JOBFILE="$OUT/jobs.txt"; : > "$JOBFILE"
while IFS= read -r f; do
    [ -z "$f" ] && continue
    obj="$OUT/$(echo "${f#$DECOMP/}" | tr '/' '_' | sed 's/\.\(c\|cpp\)$/.o/')"
    case "$f" in
        */maps/map0_s00/*|*map0_s00_extracted_data.c) extra="-DMAP0_S00 -DSH_MAP_NAME=map0_s00" ;;
        *) extra="" ;;
    esac
    printf '%s\t%s\t%s\n' "$f" "$obj" "$extra" >> "$JOBFILE"
done < <(collect_srcs "${1:-}")

# shellcheck disable=SC2016
xargs -P "${JOBS:-$(nproc)}" -d '\n' -I{} bash -c 'IFS=$'"'"'\t'"'"' read -r f o e <<< "{}"; compile_one "$f" "$o" "$e"' < "$JOBFILE"

pass=$(ls "$OUT"/*.o 2>/dev/null | wc -l)
fail=$(ls "$OUT"/*.err 2>/dev/null | wc -l)
for e in "$OUT"/*.err;  do [ -f "$e" ] && cat "$e" >> "$LOG"; done
for e in "$OUT"/*.warn; do [ -f "$e" ] && cat "$e" >> "$WARNLOG"; done

echo "=========================================="
echo " GameCube/Wii compile gate  [$PLATFORM, $CC]"
echo "   passed : $pass"
echo "   failed : $fail"
echo "   total  : $((pass+fail))"
echo "=========================================="
if [ "$fail" -gt 0 ]; then
    echo
    echo "Top error kinds:"
    grep -h "error:" "$LOG" | sed 's/.*error: //' \
        | sed "s/'[^']*'/'X'/g; s/\"[^\"]*\"/\"X\"/g" | sort | uniq -c | sort -rn | head -20
    echo "Full log: $LOG"
    exit 1
fi
