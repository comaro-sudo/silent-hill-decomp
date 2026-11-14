/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_log_ogc.c - the debug log for GameCube/Wii, after sh_log_xbox360.c.
 * Writes <data root>/silenthill_NNN.log, keeping the last 20.
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#include "sh_log.h"

extern const char* Sh360Fs_WritableRoot(void);   /* fs_ogc.c */

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* 1 = also the high-volume tags the gate below drops. */
int g_XboxLogDiag = 0;

/* Same per-frame tags the 360 build gates off: at SD-card speeds they would
 * cost more than the frame. */
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

#if defined(SH_GC_PORT)
static char s_logBuf[32 * 1024];      /* 24 MB of RAM: keep it small */
#else
static char s_logBuf[128 * 1024];
#endif

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

#if defined(SH_GC_PORT)
#include <ogc/system.h>
static int ShLog_UartWrite(void* cookie, const char* buf, int n)
{
    int done = 0;
    (void)cookie;
    while (done < n) {
        int c = n - done > 200 ? 200 : n - done;
        SYS_Report("%.*s", c, buf + done);
        done += c;
    }
    return n;
}
#endif

void SH_DebugLogInit(void)
{
    static char s_pathStore[160];
    const char* dir;

    if (g_ShDebugLog)
        return;
    dir = Sh360Fs_WritableRoot();
    if (!dir || !dir[0]) {
#if defined(SH_GC_PORT)
        /* Disc-only boot (Dolphin): no writable card, so send the log to the
         * emulator's OSReport UART instead (SYS_Report). */
        g_ShDebugLog = funopen(NULL, NULL, ShLog_UartWrite, NULL, NULL);
        if (g_ShDebugLog) {
            g_ShLogPath = "(OSReport)";
            setvbuf(g_ShDebugLog, NULL, _IOLBF, 0);
        }
#endif
        return;                       /* no card: logging stays off */
    }
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

unsigned Xbox_MemFreeKB(void)
{
    return Xbox_MemUsedKB();          /* name kept for the shared callers */
}

void Xbox_MemReport(const char* tag)
{
    struct mallinfo mi = mallinfo();
    SH_DBG("[MEM] %s: used=%uKB arena=%uKB", tag ? tag : "",
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024));
}
