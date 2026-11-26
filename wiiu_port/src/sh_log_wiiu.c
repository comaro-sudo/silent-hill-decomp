/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_log_wiiu.c - debug log and heap reporting for Wii U.
 * Writes <data root>/silenthill_NNN.log, keeping the last 20.
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#include "sh_log.h"

extern const char* Sh360Fs_WritableRoot(void);   /* fs_wiiu.c */

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* 1 = also log the high-volume per-frame tags gated off below. */
int g_XboxLogDiag = 0;

int Sh_LogAllow(const char* fmt)
{
    static const char* const GATED[] = {
        "[SH_AUDIO]", "[SH_BGM]", "[UIDIAG]", "[UPD]", "[UPD2]", "[POST]",
        "[FOGST]", "[WALLSTOP]",
        "[WALL-HIT]", "[RAIN]", "[FSQ]", "[SS]", "[FXDROP]", "[BATCH]", "[STORE]",
        "[BIGPRIM]", "[ZETA]", "[ITEMZ]", "[FLEX]", "[FONTDUMP]", "[TXTPG]", "[TXSPR]",
        "[WMAT]", "[MESHDRAW]", "[A900]", "[ALTCULL]", "[CAMCHAIN]", "[SHOT]",
    };
    int i;
    if (g_XboxLogDiag)          return 1;
    if (!fmt || fmt[0] != '[')  return 1;
    for (i = 0; i < (int)(sizeof(GATED) / sizeof(GATED[0])); i++) {
        const char* g = GATED[i];
        const char* f = fmt;
        while (*g && *g == *f) { g++; f++; }
        if (*g == '\0') return 0;
    }
    return 1;
}

static char s_logBuf[128 * 1024];

const char* g_ShLogPath = "(none)";

#define SH_LOG_KEEP 20

static int ShLog_NextIndex(const char* dir)
{
    char  path[160], buf[16];
    int   idx = 0;
    FILE* f;
    snprintf(path, sizeof(path), "%sshlog_idx.txt", dir);
    f = fopen(path, "r");
    if (f) { if (fgets(buf, sizeof(buf), f)) idx = atoi(buf); fclose(f); }
    if (idx < 0) idx = 0;
    f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", idx + 1); fclose(f); }
    if (idx >= SH_LOG_KEEP) {
        char oldp[160];
        snprintf(oldp, sizeof(oldp), "%ssilenthill_%03d.log", dir, idx - SH_LOG_KEEP);
        remove(oldp);
    }
    return idx;
}

void SH_DebugLogInit(void)
{
    static char s_pathStore[160];
    const char* dir;

    if (g_ShDebugLog)
        return;
    dir = Sh360Fs_WritableRoot();
    if (!dir || !dir[0])
        return;
    snprintf(s_pathStore, sizeof(s_pathStore), "%ssilenthill_%03d.log", dir, ShLog_NextIndex(dir));
    g_ShDebugLog = fopen(s_pathStore, "w");
    if (!g_ShDebugLog)
        return;
    g_ShLogPath = s_pathStore;
    setvbuf(g_ShDebugLog, s_logBuf, _IOFBF, sizeof(s_logBuf));
}

void SH_DebugLogFlush(void)
{
    if (!g_ShDebugLog)
        return;
    fflush(g_ShDebugLog);
    fsync(fileno(g_ShDebugLog));
}

unsigned Xbox_MemUsedKB(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned)(mi.uordblks / 1024);
}

/* wut hands newlib one big sbrk block carved from MEM2 at boot, and fordblks
 * only counts what the break has already passed. Probe the upper bound once. */
static char* s_sbrkTop;

static void WiiU_SbrkTopProbe(void)
{
    size_t lo = 0, hi = (size_t)1536 << 20;
    while (hi - lo > ((size_t)1 << 20))
    {
        size_t mid = lo + (hi - lo) / 2;
        void*  p   = malloc(mid);
        if (p)
        {
            char* end = (char*)p + mid;
            if (end > s_sbrkTop) s_sbrkTop = end;
            free(p);
            lo = mid;
        }
        else
            hi = mid;
    }
    if (!s_sbrkTop) s_sbrkTop = (char*)sbrk(0);
}

unsigned Xbox_MemFreeKB(void)
{
    struct mallinfo mi = mallinfo();
    char* brk;
    if (!s_sbrkTop) WiiU_SbrkTopProbe();
    brk = (char*)sbrk(0);
    return (unsigned)((mi.fordblks + (brk < s_sbrkTop ? (size_t)(s_sbrkTop - brk) : 0)) / 1024);
}

void Xbox_MemReport(const char* tag)
{
    struct mallinfo mi = mallinfo();
    SH_DBG("[MEM] %s: used=%uKB arena=%uKB", tag ? tag : "",
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024));
}
