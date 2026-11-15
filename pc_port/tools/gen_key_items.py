#!/usr/bin/env python3
"""Generate pc_port/src/pc_key_items.inc: every key-item pickup lying in a map.

Modern's key-item shine (pc_keyglint.c) needs, per map, where each key item
lies and which event flag says it has been picked up. That is already in the
decomp, spread over three files per map:

  mapX_sYY_events_data.c  MAP_EVENTS: disabledEventFlag (= picked up),
                          pointOfInterestIdx, eventParam
  mapX_sYY_header.c       g_MapEventFuncs[eventParam] -> handler name
  map_points.h            MAP_POINTS[pointOfInterestIdx] -> X/Z position
  handler body            the item it gives: Event_ItemTake(InvItemId_...),
                          Event_InvItemCmd(InvItemCmd_AddItem, InvItemId_...)
                          Gfx_PickupItemAnimate(InvItemId_...) or
                          func_80086470(3, InvItemId_..., ...)

Key items are inventory IDs 64-127 (include/bodyprog/items.h). Run from the
repo root: python3 pc_port/tools/gen_key_items.py
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def item_ids():
    ids = {}
    for m in re.finditer(r"\b(InvItemId_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", read(f"{ROOT}/include/bodyprog/items.h")):
        ids[m.group(1)] = int(m.group(2), 0)
    return ids


def braces(text, start):
    """Index just past the brace block that opens at or after start."""
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return i, j + 1
    raise ValueError("unbalanced")


def func_bodies(paths):
    bodies = {}
    for p in paths:
        t = read(p)
        for m in re.finditer(r"^\s*(?:static\s+)?void\s+(\w+)\s*\(\s*void\s*\)\s*(?://[^\n]*)?\n\s*\{", t, re.M):
            try:
                a, b = braces(t, m.end() - 1)
            except ValueError:
                continue
            bodies.setdefault(m.group(1), t[a:b])
    return bodies


def main():
    ids = item_ids()
    out = []
    skipped = []
    for d in sorted(glob.glob(f"{ROOT}/src/maps/map*_s*")):
        name = os.path.basename(d)
        data = f"{d}/{name}_events_data.c"
        header = f"{d}/{name}_header.c"
        points = f"{d}/map_points.h"
        if not (os.path.exists(data) and os.path.exists(header) and os.path.exists(points)):
            continue

        h = read(header)
        m = re.search(r"g_MapEventFuncs\[\][^=]*=\s*\{(.*?)\};", h, re.S)
        funcs = [f.strip() for f in m.group(1).split(",") if f.strip()] if m else []

        pts = []
        for e in re.finditer(r"\{(.*?)\}", read(points), re.S):
            x = re.search(r"positionX\s*=\s*Q12\(\s*(-?[\d.]+)f?\s*\)", e.group(1))
            z = re.search(r"positionZ\s*=\s*Q12\(\s*(-?[\d.]+)f?\s*\)", e.group(1))
            pts.append((float(x.group(1)) if x else None, float(z.group(1)) if z else None))

        srcs = glob.glob(f"{d}/*.c") + glob.glob(f"{d}/*.h") + glob.glob(f"{ROOT}/include/maps/shared/*.h")
        bodies = func_bodies(srcs)

        ev = read(data)
        for e in re.finditer(r"\{([^{}]*eventParam[^{}]*)\}", ev, re.S):
            body = e.group(1)
            flag = re.search(r"disabledEventFlag\s*=\s*(\w+)", body)
            req = re.search(r"requiredEventFlag\s*=\s*(\w+)", body)
            poi = re.search(r"pointOfInterestIdx\s*=\s*(\d+)", body)
            prm = re.search(r"eventParam\s*=\s*(\d+)", body)
            if not (poi and prm):
                continue
            idx = int(prm.group(1))
            if idx >= len(funcs):
                continue
            handler = funcs[idx]
            hb = bodies.get(handler, "")
            if not flag:
                # Some pickups (map7_s02's Dagger, Key of Aratron) have no
                # disabledEventFlag: the handler sets its own "Pickup" flag.
                flag = re.search(r"Savegame_EventFlagSet\(\s*(EventFlag_\w*Pickup\w*)", hb)
                if not flag:
                    continue
            seen = set()
            for t in re.finditer(r"(?:Event_ItemTake\(|InvItemCmd_AddItem\s*,|Gfx_PickupItemAnimate\(|func_80086470\(\s*3\s*,)\s*(InvItemId_\w+)", hb):
                if t.group(1) in seen:
                    continue
                seen.add(t.group(1))
                item = ids.get(t.group(1))
                if item is None or not (64 <= item <= 127):
                    continue
                p = int(poi.group(1))
                if p >= len(pts) or pts[p][0] is None:
                    skipped.append(f"{name} {handler}: no point {p}")
                    continue
                out.append((name, item, t.group(1), flag.group(1), pts[p][0], pts[p][1], handler,
                            req.group(1) if req else "0"))

    with open(f"{ROOT}/pc_port/src/pc_key_items.inc", "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Generated by pc_port/tools/gen_key_items.py - do not edit.\n")
        f.write(" * { map, item id, picked-up event flag, X, Z, required event flag (0 = none) }\n")
        f.write(" * (key items: ids 64-127; a pickup exists only while its required flag is set) */\n")
        for name, item, iname, flag, x, z, handler, req in out:
            f.write(f'{{ "{name}", {item:3d}, {flag}, Q12({x:.2f}f), Q12({z:.2f}f), {req} }}, /* {iname}, {handler} */\n')
    print(f"{len(out)} key-item pickups across {len(set(o[0] for o in out))} maps; "
          f"{len(set(o[1] for o in out))} distinct items")
    for s in skipped:
        print("skipped:", s)
    found = set(o[1] for o in out)
    missing = sorted((v, k) for k, v in ids.items() if 64 <= v <= 127 and v not in found)
    print("not found as a map pickup:", ", ".join(f"{k}({v})" for v, k in missing))


if __name__ == "__main__":
    sys.exit(main())
