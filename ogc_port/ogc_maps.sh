#!/usr/bin/env bash
# Build the 42 map overlays beyond map0_s00 as symbol-prefixed static archives,
# the scheme of xbox360_port/build_maps.sh (from the Original Xbox and PS3
# ports): every src/maps/<name>/*.c plus its extracted-data TU, compiled with
# -DMAP<N>_S<M> -DSH_MAP_NAME=<name>, then every DEFINED extern renamed to
# <name>_<sym>. The shared AI/particle/player sources are #included into each
# map, so without the prefix a link of all 43 has 500+ duplicate symbols.
# map0_s00 stays unprefixed and is built by ogc_gate.sh (map_xbox.c's registry).
#
#   PLATFORM=wii ./ogc_maps.sh           # all maps
#   PLATFORM=wii ./ogc_maps.sh map1_s00  # just these
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/ogc_flags.sh" || exit 2

OUT="$SCRIPT_DIR/build/$PLATFORM/maps"
LOG="$SCRIPT_DIR/build/$PLATFORM/maps.log"
mkdir -p "$OUT"; : > "$LOG"

MAPS="${*:-
map0_s01 map0_s02
map1_s00 map1_s01 map1_s02 map1_s03 map1_s04 map1_s05 map1_s06
map2_s00 map2_s01 map2_s02 map2_s03 map2_s04
map3_s00 map3_s01 map3_s02 map3_s03 map3_s04 map3_s05 map3_s06
map4_s00 map4_s01 map4_s02 map4_s03 map4_s04 map4_s05 map4_s06
map5_s00 map5_s01 map5_s02 map5_s03
map6_s00 map6_s01 map6_s02 map6_s03 map6_s04 map6_s05
map7_s00 map7_s01 map7_s02 map7_s03
}"

JOBFILE="$OUT/jobs.txt"; : > "$JOBFILE"
for m in $MAPS; do
    mdir="$OUT/$m"; mkdir -p "$mdir/raw"
    rm -f "$mdir"/raw/*.o "$mdir"/raw/*.err "$mdir"/raw/*.warn "$mdir"/*.o
    def="-D$(echo "$m" | tr 'a-z' 'A-Z') -DSH_MAP_NAME=$m"
    for f in "$DECOMP/src/maps/$m"/*.c "$PCPORT/build_gen/extracted_data/${m}_extracted_data.c"; do
        [ -f "$f" ] || continue
        printf '%s\t%s\t%s\n' "$f" "$mdir/raw/$(basename "${f%.c}").o" "$def" >> "$JOBFILE"
    done
done
# shellcheck disable=SC2016
xargs -P "${JOBS:-$(nproc)}" -d '\n' -I{} bash -c 'IFS=$'"'"'\t'"'"' read -r f o e <<< "{}"; compile_one "$f" "$o" "$e"' < "$JOBFILE"

libs=0; bad=0
for m in $MAPS; do
    mdir="$OUT/$m"
    if ls "$mdir"/raw/*.err >/dev/null 2>&1; then
        cat "$mdir"/raw/*.err >> "$LOG"
        printf "  %-10s FAILED (%d files)\n" "$m" "$(ls "$mdir"/raw/*.err | wc -l)"
        bad=$((bad+1))
        continue
    fi
    ls "$mdir"/raw/*.o >/dev/null 2>&1 || { echo "  $m: no sources"; continue; }
    powerpc-eabi-nm --extern-only --defined-only "$mdir"/raw/*.o \
        | awk -v map="$m" 'NF >= 3 { print $3, map "_" $3 }' | sort -u > "$mdir/$m.syms"
    for o in "$mdir"/raw/*.o; do
        powerpc-eabi-objcopy --redefine-syms="$mdir/$m.syms" "$o" "$mdir/$(basename "$o")"
    done
    rm -f "$OUT/$m.a"
    powerpc-eabi-ar rcs "$OUT/$m.a" "$mdir"/*.o && libs=$((libs+1))
done

echo "=========================================="
echo " map overlays [$PLATFORM]: $libs archives, $bad failed"
echo "=========================================="
[ "$bad" -gt 0 ] && { echo "see $LOG"; exit 1; }
exit 0
