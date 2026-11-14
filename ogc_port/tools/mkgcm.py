#!/usr/bin/env python3
"""Build a GameCube disc image (GCM) that boots the port's .dol and carries the
PS1 disc image as a file, for Dolphin (which cannot emulate an SD adapter).

  python3 mkgcm.py --dol silenthill.dol --bin "Silent Hill (v1.1).bin" \
                   --apploader apploader.bin --out silenthill.gcm

No Nintendo code is used: the apploader is ogc_port/tools/apploader_stub.c.
Layout: boot.bin @0, bi2.bin @0x440, apploader @0x2440, then DOL, FST, the .bin.
"""
import argparse, os, struct, shutil

def align(x, a): return (x + a - 1) & ~(a - 1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dol', required=True)
    ap.add_argument('--bin', required=True)
    ap.add_argument('--apploader', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--id', default='GSHE69')
    ap.add_argument('--name', default='Silent Hill (PS1 port)')
    ap.add_argument('--filename', default='silenthill.bin')
    ap.add_argument('--extra', action='append', default=[], help='NAME=PATH small file added to the disc root (e.g. autoplay.txt=...)')
    a = ap.parse_args()

    dol = open(a.dol, 'rb').read()
    app = bytearray(open(a.apploader, 'rb').read())
    mark = struct.pack('>I', 0x444F4C4F)
    if app.count(mark) != 1:
        # little-endian build of the constant is never the case: PPC is BE
        raise SystemExit('DOL_MARK not found exactly once in apploader')
    binsz = os.path.getsize(a.bin)

    APP_OFF = 0x2440
    app_hdr = bytearray(0x20)
    app_hdr[0:10] = b'2002/04/11'            # build date string (free-form)
    entry = 0x81200000
    # entry is the address of `entry()`; the linker puts .text.entry first
    app_hdr[0x10:0x14] = struct.pack('>I', entry)
    app_hdr[0x14:0x18] = struct.pack('>I', len(app))
    app_hdr[0x18:0x1C] = struct.pack('>I', 0)

    dol_off = align(APP_OFF + 0x20 + len(app), 0x20)
    app[app.index(mark):app.index(mark) + 4] = struct.pack('>I', dol_off)

    fst_off = align(dol_off + len(dol), 0x20)
    files = [(a.filename, None, binsz)]          # (name, path, size); the .bin is written last
    for e in a.extra:
        n, p = e.split('=', 1)
        files.insert(len(files) - 1, (n, p, os.path.getsize(p)))
    count = 1 + len(files)
    names = b''
    noff = []
    for n, _, _ in files:
        noff.append(len(names)); names += n.encode() + b'\0'
    fst_hdr = 12 * count
    cur = align(fst_off + fst_hdr + len(names), 0x20)
    offs = []
    for i, (n, p, sz) in enumerate(files):
        if p is None:
            cur = align(cur, 0x8000)
        offs.append(cur); cur = align(cur + sz, 0x20)
    file_off = offs[-1]
    fst = struct.pack('>III', 0x01000000, 0, count)
    for i, (n, p, sz) in enumerate(files):
        fst += struct.pack('>III', noff[i], offs[i], sz)
    fst += names
    fst_size = len(fst)

    boot = bytearray(0x440)
    boot[0:6] = a.id.encode()
    boot[6] = 0; boot[7] = 0
    struct.pack_into('>I', boot, 0x1C, 0xC2339F3D)
    boot[0x20:0x20 + len(a.name)] = a.name.encode()
    struct.pack_into('>IIII', boot, 0x420, dol_off, fst_off, fst_size, fst_size)
    bi2 = bytearray(0x2000)
    struct.pack_into('>I', bi2, 0x4, 0x01800000)                 # simulated mem size 24 MB
    struct.pack_into('>I', bi2, 0x18, 1)                         # country: USA
    struct.pack_into('>I', bi2, 0x1C, 1)

    with open(a.out, 'wb') as f:
        f.write(boot); f.write(bi2)
        f.seek(APP_OFF); f.write(app_hdr); f.write(app)
        f.seek(dol_off); f.write(dol)
        f.seek(fst_off); f.write(fst)
        for i, (n, p, sz) in enumerate(files):
            if p:
                f.seek(offs[i]); f.write(open(p, 'rb').read())
        f.seek(file_off)
        with open(a.bin, 'rb') as b:
            shutil.copyfileobj(b, f, 1 << 20)
        end = f.tell()
        f.truncate(align(end, 0x8000))
    print('dol @0x%X (%d)  fst @0x%X (%d)  %s @0x%X (%d)  total %d' %
          (dol_off, len(dol), fst_off, fst_size, a.filename, file_off, binsz, align(end, 0x8000)))

main()
