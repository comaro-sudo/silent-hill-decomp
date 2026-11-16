/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * autoplay.c - scripted pad input for unattended emulator test runs, shared by
 * the GameCube/Wii (pad_ogc.c) and Wii U (pad_wiiu.c) pads. Clears PSX button
 * bits (active-low) in the word the pad code is about to publish.
 */
#include <stdio.h>
#include <string.h>

#include "sh_log.h"

#define PSX_SELECT   0
#define PSX_L3       1
#define PSX_R3       2
#define PSX_START    3
#define PSX_UP       4
#define PSX_RIGHT    5
#define PSX_DOWN     6
#define PSX_LEFT     7
#define PSX_L2       8
#define PSX_R2       9
#define PSX_L1      10
#define PSX_R1      11
#define PSX_TRIANGLE 12
#define PSX_CIRCLE  13
#define PSX_CROSS   14
#define PSX_SQUARE  15

/* Scripted input for unattended emulator test runs (as the 360's
 * pad_xinput.cpp): <data root>/autoplay.txt, one press per line,
 *   <frame> <BUTTON> [holdFrames]      e.g.  "1500 START 10"
 *   every <first> <last> <period> <BUTTON> [hold]   repeat a press
 * BUTTON: CROSS CIRCLE SQUARE TRIANGLE START SELECT UP DOWN LEFT RIGHT
 * L1 R1 L2 R2, or QSAVE / QLOAD: queue the Options menu's Quick Save / Quick
 * Load (opens the real save/load screen once in gameplay), so a test run can
 * reach the memory-card code without walking to a save point. Frames count
 * Pad_Poll calls. Absent file = no effect. */
#define AUTO_QSAVE 100
#define AUTO_QLOAD 101
#define AUTO_TOAST 102   /* show achievement 0's pop-up, unlocking nothing */
#define AUTO_OFFTV 103   /* Wii U: toggle Off-TV play, as tapping PAD/TV does */
#define AUTO_FLAG  104   /* "<frame> FLAG <n>": set savegame event flag n */
#define AUTO_GIVE  105   /* "<frame> GIVE <id>": add item <id> to the inventory */
#define AUTO_GLINT 106   /* "<frame> GLINTALL": every key item shines (Modern) */
#define AUTO_WARPK 107   /* "<frame> WARPKEY <n>": Harry next to key-item spot n */
#define AUTO_BOSS  108   /* "<frame> BOSSSIM": Modern autosave sees a boss in this room */
#define AUTO_EQUIP 110   /* "<frame> EQUIP <id>": equip owned weapon <id> */
#define AUTO_WARPP 109   /* "<frame> WARPPOI <n>": Harry inside map point n, facing it */
#define AUTO_MAX 512
typedef struct { int first, last, period, bit, hold; } AutoKey;
static AutoKey s_auto[AUTO_MAX];
static int     s_autoN = -1;
static int     s_autoFrame;

static int Auto_Bit(const char* n)
{
    static const struct { const char* n; int b; } k[] = {
        {"SELECT",PSX_SELECT},{"START",PSX_START},{"UP",PSX_UP},{"RIGHT",PSX_RIGHT},
        {"DOWN",PSX_DOWN},{"LEFT",PSX_LEFT},{"L2",PSX_L2},{"R2",PSX_R2},{"L1",PSX_L1},
        {"R1",PSX_R1},{"TRIANGLE",PSX_TRIANGLE},{"CIRCLE",PSX_CIRCLE},{"CROSS",PSX_CROSS},
        {"SQUARE",PSX_SQUARE},{"QSAVE",AUTO_QSAVE},{"QLOAD",AUTO_QLOAD},{"TOAST",AUTO_TOAST},{"OFFTV",AUTO_OFFTV},{"FLAG",AUTO_FLAG},{"GIVE",AUTO_GIVE},{"GLINTALL",AUTO_GLINT},{"WARPKEY",AUTO_WARPK},{"WARPPOI",AUTO_WARPP},{"EQUIP",AUTO_EQUIP},{"BOSSSIM",AUTO_BOSS},
    };
    unsigned i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strcmp(n, k[i].n) == 0) return k[i].b;
    return -1;
}

static void Auto_Load(void)
{
    extern const char* Sh360Fs_DataRoot(void);
    char path[192], line[128], name[32];
    FILE* f;
    s_autoN = 0;
    snprintf(path, sizeof(path), "%sautoplay.txt", Sh360Fs_DataRoot());
    f = fopen(path, "r");
#if defined(SH_GC_PORT)
    if (!f) {   /* disc-only (Dolphin): autoplay.txt may sit in the disc root */
        extern int GcDisc_ReadSmallFile(const char*, char*, int);
        static char txt[8192];
        char* p = txt;
        if (!GcDisc_ReadSmallFile("autoplay.txt", txt, sizeof(txt))) return;
        while (*p && s_autoN < AUTO_MAX) {
            char* nl = strchr(p, 10);
            AutoKey k = { 0, 0, 0, -1, 8 };
            if (nl) *nl = 0;
            if (sscanf(p, "every %d %d %d %31s %d", &k.first, &k.last, &k.period, name, &k.hold) >= 4) {
            } else if (sscanf(p, "%d %31s %d", &k.first, name, &k.hold) >= 2) {
                k.last = k.first; k.period = 1;
            } else k.bit = -2;
            if (k.bit != -2) {
                k.bit = Auto_Bit(name);
                if (k.bit >= 0 && k.period > 0) s_auto[s_autoN++] = k;
            }
            if (!nl) break;
            p = nl + 1;
        }
        SH_DBG("[PAD] autoplay: %d entries from disc", s_autoN);
        return;
    }
#endif
    if (!f) return;
    while (fgets(line, sizeof(line), f) && s_autoN < AUTO_MAX) {
        AutoKey k = { 0, 0, 0, -1, 8 };
        if (sscanf(line, "every %d %d %d %31s %d", &k.first, &k.last, &k.period, name, &k.hold) >= 4) {
        } else if (sscanf(line, "%d %31s %d", &k.first, name, &k.hold) >= 2) {
            k.last = k.first; k.period = 1;
        } else {
            continue;
        }
        k.bit = Auto_Bit(name);
        if (k.bit >= 0 && k.period > 0) s_auto[s_autoN++] = k;
    }
    fclose(f);
    SH_DBG("[PAD] autoplay: %d scripted entries from %s", s_autoN, path);
}

void Autoplay_Apply(unsigned short* psx)
{
    int i;
    if (s_autoN < 0) Auto_Load();
    s_autoFrame++;
    for (i = 0; i < s_autoN; i++) {
        const AutoKey* k = &s_auto[i];
        int rel;
        if (s_autoFrame < k->first || s_autoFrame > k->last + k->hold) continue;
        rel = (s_autoFrame - k->first) % k->period;
        if (k->bit == AUTO_BOSS) {
            extern void Pc_Saves_TestBossSim(void);
            if (s_autoFrame == k->first) Pc_Saves_TestBossSim();
            continue;
        }
        if (k->bit == AUTO_EQUIP) {
            extern void Xbox_TestEquip(int id);
            if (s_autoFrame == k->first) Xbox_TestEquip(k->hold);
            continue;
        }
        if (k->bit == AUTO_WARPP) {
            extern void Xbox_TestWarpToPoint(int n);
            if (rel == 0 && s_autoFrame <= k->last) Xbox_TestWarpToPoint(k->hold);
            continue;
        }
        if (k->bit == AUTO_WARPK) {
            extern void Pc_KeyGlint_TestWarp(int n);
            if (rel == 0 && s_autoFrame <= k->last) Pc_KeyGlint_TestWarp(k->hold);
            continue;
        }
        if (k->bit == AUTO_FLAG || k->bit == AUTO_GIVE || k->bit == AUTO_GLINT) {
            extern void Xbox_TestEventFlagSet(int flag);
            extern void Xbox_TestItemGive(int id);
            extern void Pc_KeyGlint_TestAll(void);
            if (s_autoFrame == k->first) {
                if (k->bit == AUTO_FLAG)      Xbox_TestEventFlagSet(k->hold);
                else if (k->bit == AUTO_GIVE) Xbox_TestItemGive(k->hold);
                else                          Pc_KeyGlint_TestAll();
            }
            continue;
        }
        if (k->bit >= AUTO_QSAVE) {
            extern void Xbox_QuickSaveRequest(void);
            extern void Xbox_QuickLoadRequest(void);
            if (rel == 0 && s_autoFrame <= k->last) {
                extern void Pc_LocalAch_TestToast(int idx);
                SH_DBG("[PAD] autoplay: %s at frame %d",
                       k->bit == AUTO_QSAVE ? "QSAVE" : k->bit == AUTO_QLOAD ? "QLOAD" :
                       k->bit == AUTO_TOAST ? "TOAST" : "OFFTV", s_autoFrame);
                if (k->bit == AUTO_QSAVE)      Xbox_QuickSaveRequest();
                else if (k->bit == AUTO_QLOAD) Xbox_QuickLoadRequest();
                else if (k->bit == AUTO_TOAST) Pc_LocalAch_TestToast(0);
#if defined(SH_WIIU_PORT)
                else { extern void GamepadScreen_RequestOffTvToggle(void); GamepadScreen_RequestOffTvToggle(); }
#endif
            }
            continue;
        }
        if (rel == 0 && s_autoFrame <= k->last)
            SH_DBG("[PAD] autoplay: button %d at frame %d", k->bit, s_autoFrame);
        if (rel < k->hold) *psx &= (unsigned short)~(1u << k->bit);
    }
}

