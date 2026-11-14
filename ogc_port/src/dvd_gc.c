/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dvd_gc.c - the PS1 disc image read from the GameCube DVD (SH_GC_PORT only).
 *
 * Dolphin cannot emulate an SD2SP2 / SD Gecko, so a GameCube disc that carries
 * the game's .dol (boot DOL) plus the PS1 .bin as an ordinary file is the way to
 * run the port there (tools: ogc_port/tools/mkgcm.py builds that disc).
 *
 * A GameCube disc has no ISO9660: files are found through the FST, a table of
 * 12-byte entries pointed to by boot.bin (disc offset 0x424 = FST offset, 0x428
 * = FST size). Entry: u8 flags (1 = directory), u24 name offset into the string
 * table that follows the entries, u32 file offset / dir parent, u32 file size /
 * dir "next" index. Root entry's last word is the entry count. File offsets are
 * absolute disc offsets.
 *
 * The cd code (xbox360_port/src/cd_xbox360.c) wants a FILE* it can fseek/fread.
 * funopen() gives exactly that over DVD_ReadPrio: the callbacks align every read
 * to the drive's 32-byte granularity through a bounce buffer.
 */
#if defined(SH_GC_PORT)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <gccore.h>
#include <ogc/dvd.h>

#include "sh_log.h"

#define BOUNCE_BYTES (32 * 1024)   /* 24 MB machine: keep the staging area small */

static dvdcmdblk s_blk;
static int       s_dvdUp;
static char*     s_bounce;                 /* 32-byte aligned */
static long long s_base, s_size, s_pos;
char             g_GcDiscName[64] = "";

static int DvdRead(void* dst, u32 len, long long off)    /* aligned args only */
{
    return DVD_ReadPrio(&s_blk, dst, len, off, 2) == (s32)len;
}

/* Arbitrary (offset, len) -> aligned reads through the bounce buffer. */
static int DvdReadAny(void* dst, size_t len, long long off)
{
    unsigned char* out = (unsigned char*)dst;

    while (len) {
        long long a    = off & ~31LL;
        u32       skip = (u32)(off - a);
        u32       want = (u32)((len + skip + 31) & ~31u);
        u32       take;

        if (want > BOUNCE_BYTES)
            want = BOUNCE_BYTES;
        if (!DvdRead(s_bounce, want, a))
            return 0;
        take = want - skip;
        if (take > len)
            take = (u32)len;
        memcpy(out, s_bounce + skip, take);
        out += take; off += take; len -= take;
    }
    return 1;
}

static int GcRead(void* cookie, char* buf, int n)
{
    long long left = s_size - s_pos;
    (void)cookie;
    if (n <= 0 || left <= 0)
        return 0;
    if (n > left)
        n = (int)left;
    if (!DvdReadAny(buf, (size_t)n, s_base + s_pos))
        return -1;
    s_pos += n;
    return n;
}

static fpos_t GcSeek(void* cookie, fpos_t off, int whence)
{
    long long p;
    (void)cookie;
    switch (whence) {
    case SEEK_SET: p = off; break;
    case SEEK_CUR: p = s_pos + off; break;
    case SEEK_END: p = s_size + off; break;
    default: return -1;
    }
    if (p < 0)
        return -1;
    s_pos = p;
    return (fpos_t)p;
}

static int GcClose(void* cookie) { (void)cookie; return 0; }

static u32 Be32(const unsigned char* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int EndsWithBin(const char* s)
{
    size_t n = strlen(s);
    return n > 4 && s[n - 4] == '.' && tolower((unsigned char)s[n - 3]) == 'b' &&
           tolower((unsigned char)s[n - 2]) == 'i' && tolower((unsigned char)s[n - 1]) == 'n';
}

/* Largest *.bin in the FST -> s_base/s_size. */
static int FindBin(void)
{
    unsigned char* hdr = (unsigned char*)memalign(32, 0x460);
    unsigned char* fst;
    u32            fstOff, fstSize, count, i;
    const unsigned char* names;
    int            found = 0;

    if (!hdr)
        return 0;
    if (!DvdRead(hdr, 0x460, 0)) {
        free(hdr);
        SH_DBG("[GCDISC] cannot read boot.bin");
        return 0;
    }
    fstOff  = Be32(hdr + 0x424);
    fstSize = Be32(hdr + 0x428);
    SH_DBG("[GCDISC] id=%.6s fst@0x%X size=%u", (char*)hdr, fstOff, fstSize);
    free(hdr);
    if (!fstOff || fstSize < 12 || fstSize > (1u << 20))
        return 0;

    fst = (unsigned char*)memalign(32, (fstSize + 31) & ~31u);
    if (!fst)
        return 0;
    if (!DvdRead(fst, (fstSize + 31) & ~31u, fstOff)) {
        free(fst);
        SH_DBG("[GCDISC] cannot read FST");
        return 0;
    }
    count = Be32(fst + 8);
    names = fst + (size_t)count * 12;
    for (i = 1; i < count && (size_t)(i + 1) * 12 <= fstSize; i++) {
        const unsigned char* e = fst + (size_t)i * 12;
        const char*          nm = (const char*)names + (Be32(e) & 0xFFFFFF);
        if (e[0] == 0 && EndsWithBin(nm) && Be32(e + 8) > s_size) {
            s_base = Be32(e + 4);
            s_size = Be32(e + 8);
            snprintf(g_GcDiscName, sizeof(g_GcDiscName), "%s", nm);
            found = 1;
        }
    }
    free(fst);
    return found;
}

/* Returns a read/seek-only FILE* over the .bin on the DVD, or NULL. */
FILE* GcDisc_OpenBin(void)
{
    if (!s_dvdUp) {
        DVD_Init();
        s_dvdUp = 1;
        SH_DBG("[GCDISC] DVD_Mount...");
        SH_DBG("[GCDISC] DVD_Mount -> %d", (int)DVD_Mount());
    }
    if (!s_bounce)
        s_bounce = (char*)memalign(32, BOUNCE_BYTES);
    if (!s_bounce)
        return NULL;
    s_base = s_size = s_pos = 0;
    if (!FindBin()) {
        SH_DBG("[GCDISC] no .bin file in the disc FST");
        return NULL;
    }
    SH_DBG("[GCDISC] '%s' at 0x%llX size %lld", g_GcDiscName, s_base, s_size);
    return funopen(NULL, GcRead, NULL, GcSeek, GcClose);
}

#endif /* SH_GC_PORT */
