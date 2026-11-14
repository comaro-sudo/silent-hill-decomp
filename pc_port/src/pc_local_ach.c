/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local achievements: see pc_local_ach.h. No network code here.
 * Console port: the toast and the list are drawn with the game's own text and
 * primitives (no PC overlay, no GL/SDL), so GX and the Wii U SDL renderer both show them. */
#include "pc_local_ach.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/item_screens.h" /* GameEndingFlag_* */
#include "bodyprog/items.h"
#include "bodyprog/ranking.h"      /* func_8009146C: kill counts */
#include "bodyprog/savegame.h" /* event flags come in through game.h */
#include "bodyprog/screen/screen_data.h" /* g_OtTags0, g_ActiveBufferIdx, g_VBlanks */
#include "bodyprog/text/text_draw.h"
#include "pc_modern.h"
#include "fs_xbox360.h"                   /* Sh360Fs_DataRoot, Sh360Fs_Sync */

typedef struct
{
    const char* title;
    const char* desc;
    unsigned    points;
    const char* badgeFile; /* unused on consoles (no PNG decoder in the build) */
} s_LocalAchDef;

static const s_LocalAchDef ACH_DEFS[] = {
#include "pc_local_ach_set.inc"
};
#define ACH_COUNT ((int)(sizeof(ACH_DEFS) / sizeof(ACH_DEFS[0])))

#define ACH_FILE      "achievements_local.txt"
#define ACH_FILE_TMP  "achievements_local.txt.tmp"
#define ACH_TOTAL_POINTS 666

/* Which achievements have a check. The rest need event flags that are still
 * unnamed in the decomp (they show as locked, "not tracked yet"). */
static const unsigned char TRACKED[ACH_COUNT] = {
    /*  0 */ 1, 1, 1, 1, 1, 1, 0, 1, 1, 0,
    /* 10 */ 0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
    /* 20 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 30 */ 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    /* 50 */ 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
    /* 60 */ 0, 1, 1, 1, 1, 1,
};

static long long s_Unlocked[ACH_COUNT]; /* unix time, 0 = locked */
static int       s_Loaded = 0;

/* Session state (RAM only). */
static int s_WeaponKills[4];     /* rock drill, katana, chainsaw, hyper blaster */
static u8  s_EndingsAtSession;   /* clearGameEndings when the session began */
static int s_DaggerSeen;
static s32 s_BossPrevHealth[NPC_COUNT_MAX];
static s8  s_BossPrevChara[NPC_COUNT_MAX];

/* Modern counts as Easy for every achievement. */
static s32 Ach_Difficulty(void)
{
    return Pc_Modern_IsActive() ? GameDifficulty_Easy : g_SavegamePtr->gameDifficulty;
}

static void Ach_Path(char* out, size_t size, const char* name)
{
    const char* root = Sh360Fs_DataRoot();

    snprintf(out, size, "%s%s", root ? root : "", name);
}

/* Written to a temp file, synced, then renamed over the real one. */
static void Ach_Save(void)
{
    char  tmp[256], dst[256];
    FILE* f;
    int   i;

    Ach_Path(tmp, sizeof(tmp), ACH_FILE_TMP);
    Ach_Path(dst, sizeof(dst), ACH_FILE);
    f = fopen(tmp, "w");
    if (!f)
    {
        return;
    }
    fprintf(f, "# Silent Hill local achievements: index unlock-time (unix seconds)\n");
    for (i = 0; i < ACH_COUNT; i++)
    {
        if (s_Unlocked[i])
        {
            fprintf(f, "%d %lld\n", i, s_Unlocked[i]);
        }
    }
    Sh360Fs_Sync(f);
    fclose(f);
    remove(dst); /* FAT rename() won't replace */
    rename(tmp, dst);
}

void Pc_LocalAch_Init(void)
{
    FILE* f;
    char  line[64], path[256];

    if (s_Loaded)
    {
        return;
    }
    s_Loaded = 1;
    memset(s_Unlocked, 0, sizeof(s_Unlocked));

    Ach_Path(path, sizeof(path), ACH_FILE);
    f = fopen(path, "r");
    if (!f)
    {
        return;
    }
    while (fgets(line, sizeof(line), f))
    {
        int       idx;
        long long t;

        if (line[0] != '#' && sscanf(line, "%d %lld", &idx, &t) == 2 && idx >= 0 && idx < ACH_COUNT)
        {
            s_Unlocked[idx] = t ? t : 1;
        }
    }
    fclose(f);
}

/* Toast queue (RAM only), drawn by Ach_ToastDraw. */
#define TOAST_QUEUE_MAX 8
#define TOAST_TICKS     240 /* vblanks: ~4 s */
static int s_ToastQueue[TOAST_QUEUE_MAX];
static int s_ToastCount;
static int s_ToastTicks; /* > 0 while the head of the queue is on screen */

static void Ach_Unlock(int idx)
{
    if (idx < 0 || idx >= ACH_COUNT || s_Unlocked[idx])
    {
        return;
    }
    /* The title's attract demo plays a real savegame: it earns nothing. */
    if (g_SysWork.sysFlags & SysFlag_DemoActive)
    {
        return;
    }
    s_Unlocked[idx] = (long long)time(NULL);
    Ach_Save();

    if (s_ToastCount < TOAST_QUEUE_MAX)
    {
        s_ToastQueue[s_ToastCount++] = idx;
    }
}

/* ---- Drawing helpers: game text + TILE prims, no renderer-specific code ----
 * Text coordinates: Gfx_StringSetPosition(x, y) subtracts (160, 112), so a TILE
 * covering the same spot sits at (x - 160, y - 112). Hi-res glyph mode doubles
 * Y for text, so the tiles double Y too. */
static void Ach_Box(int x, int y, int w, int h, int r, int g, int b, int semiTrans, int layer)
{
    TILE* tile = (TILE*)GsOUT_PACKET_P;
    GsOT* ot   = (GsOT*)&g_OtTags0[g_ActiveBufferIdx][layer];
    int   k    = g_SysWork.enableHighResGlyphs ? 2 : 1;

    setTile(tile);
    if (semiTrans)
    {
        setcode(tile, PRIM_RECT | RECT_BLEND);
    }
    setRGB0(tile, r, g, b);
    setXY0(tile, x - 160, (y - 112) * k);
    setWH(tile, w, h * k);
    addPrim(ot, tile);
    GsOUT_PACKET_P = (u8*)tile + sizeof(TILE);
}

static void Ach_Text(int x, int y, int color, const char* str)
{
    char buf[48];

    char* p;

    snprintf(buf, sizeof(buf), "%s", str);
    for (p = buf; *p; p++)
    {
        if (*p == ' ')
        {
            *p = '_'; /* the game's string drawer spells a space '_' */
        }
    }
    Gfx_StringSetPosition(x, y);
    Gfx_StringSetColor(color);
    Gfx_StringDraw(buf, (s32)strlen(buf));
}

/* Copy at most `max` characters; longer strings end in "..". */
static void Ach_Fit(char* out, const char* in, int max)
{
    int len = (int)strlen(in);

    if (len <= max)
    {
        strcpy(out, in);
        return;
    }
    memcpy(out, in, (size_t)max);
    out[max - 2] = '.';
    out[max - 1] = '.';
    out[max]     = '\0';
}

/* Unlock toast: a box near the top with a heading and the title. Held back
 * while a movie plays; each toast shows for ~4 s, queued ones follow. */
static void Ach_ToastDraw(void)
{
    char title[32];
    int  idx, tick;

    if (s_ToastCount == 0)
    {
        return;
    }
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState == SysState_Fmv)
    {
        return;
    }

    if (s_ToastTicks <= 0)
    {
        s_ToastTicks = TOAST_TICKS;
    }
    idx = s_ToastQueue[0];

    Ach_Fit(title, ACH_DEFS[idx].title, 26);
    Ach_Box(26, 38, 268, 56, 8, 8, 8, 0, 7);          /* backing */
    Ach_Box(26, 38, 268, 2, 200, 170, 60, 0, 6);      /* gold edge */
    Ach_Box(26, 92, 268, 2, 200, 170, 60, 0, 6);
    Ach_Text(40, 46, StringColorId_Gold, "ACHIEVEMENT UNLOCKED");
    Ach_Text(40, 64, StringColorId_White, title);

    tick = (g_VBlanks > 0) ? g_VBlanks : 1;
    s_ToastTicks -= tick;
    if (s_ToastTicks <= 0)
    {
        memmove(&s_ToastQueue[0], &s_ToastQueue[1], (size_t)(s_ToastCount - 1) * sizeof(int));
        s_ToastCount--;
        s_ToastTicks = 0;
    }
}

int         Pc_LocalAch_Count(void)          { return ACH_COUNT; }
const char* Pc_LocalAch_Title(int idx)       { return (idx >= 0 && idx < ACH_COUNT) ? ACH_DEFS[idx].title : ""; }
const char* Pc_LocalAch_Desc(int idx)        { return (idx >= 0 && idx < ACH_COUNT) ? ACH_DEFS[idx].desc : ""; }
unsigned    Pc_LocalAch_Points(int idx)      { return (idx >= 0 && idx < ACH_COUNT) ? ACH_DEFS[idx].points : 0; }
int         Pc_LocalAch_IsTracked(int idx)   { return (idx >= 0 && idx < ACH_COUNT) ? TRACKED[idx] : 0; }
int         Pc_LocalAch_IsUnlocked(int idx)
{
    Pc_LocalAch_Init();
    return (idx >= 0 && idx < ACH_COUNT) && s_Unlocked[idx] != 0;
}

void Pc_LocalAch_SessionReset(void)
{
    memset(s_WeaponKills, 0, sizeof(s_WeaponKills));
    s_EndingsAtSession = g_SavegamePtr ? g_SavegamePtr->clearGameEndings : 0;
    s_DaggerSeen       = g_SavegamePtr ? (Savegame_EventFlagGet(EventFlag_M7S02_PickupDaggerOfMelchior) != 0) : 0;
}

void Pc_LocalAch_OnKill(void* charaPtr)
{
    static const u8 WEAPONS[4] = { InvItemId_RockDrill, InvItemId_Katana, InvItemId_Chainsaw, InvItemId_HyperBlaster };
    static const u8 ACH[4]     = { 34, 35, 36, 37 };
    int             i;

    (void)charaPtr;
    if (g_SavegamePtr == NULL || (g_SysWork.sysFlags & SysFlag_DemoActive))
    {
        return;
    }
    for (i = 0; i < 4; i++)
    {
        if (g_SavegamePtr->equippedWeapon == WEAPONS[i] && ++s_WeaponKills[i] >= 30)
        {
            Ach_Unlock(ACH[i]);
        }
    }
}

void Pc_LocalAch_OnGameClear(int rankScore)
{
    u8 newEndings;

    if (g_SavegamePtr == NULL)
    {
        return;
    }
    newEndings = g_SavegamePtr->clearGameEndings & ~s_EndingsAtSession;

    /* Harry the Swift: any non-UFO ending in under 1 h 30 min. */
    if ((newEndings & ~GameEndingFlag_Ufo) && g_SavegamePtr->gameplayTimer < Q12(90.0f * 60.0f))
    {
        Ach_Unlock(49);
    }
    if (g_SavegamePtr->savegameCount <= 2)
    {
        Ach_Unlock(50); /* Daring Harry */
    }

    /* Rank stars. Modern is Normal underneath, so the game's score is the
     * Normal one; the plan's "rank as if Easy" refinement is not done yet. */
    if (rankScore >= 30)  Ach_Unlock(51);
    if (rankScore >= 50)  Ach_Unlock(52);
    if (rankScore >= 80)  Ach_Unlock(53);
    if (rankScore >= 100) Ach_Unlock(54);
}

/* Bosses: a boss whose health drops to 0. Any difficulty, and Hard. */
static void Ach_BossWatch(void)
{
    s32 i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        int             any = -1, hard = -1;

        if (npc->model.charaId != s_BossPrevChara[i])
        {
            s_BossPrevChara[i]  = npc->model.charaId;
            s_BossPrevHealth[i] = npc->health;
            continue;
        }

        switch (npc->model.charaId)
        {
            case Chara_SplitHead:    any = 4;  hard = 22; break;
            case Chara_Twinfeeler:   any = 7;  hard = 23; break;
            case Chara_Floatstinger: any = 8;  hard = 24; break;
            case Chara_Incubus:      any = 20; hard = 25; break;
            case Chara_Incubator:    any = 21; hard = 26; break;
        }

        if (any >= 0 && s_BossPrevHealth[i] > Q12(0.0f) && npc->health <= Q12(0.0f))
        {
            Ach_Unlock(any);
            if (Ach_Difficulty() == GameDifficulty_Hard)
            {
                Ach_Unlock(hard);
            }
        }
        s_BossPrevHealth[i] = npc->health;
    }
}

void Pc_LocalAch_Update(void)
{
    static int s_Frame;
    u32        ranged, melee;
    u8         endings;

    Pc_LocalAch_Init();
    Ach_ToastDraw();
    if (g_SavegamePtr == NULL || g_GameWork.gameState != GameState_InGame)
    {
        return;
    }

    Ach_BossWatch();

    /* The rest only changes slowly: check a few times a second. */
    if ((++s_Frame & 15) != 0)
    {
        return;
    }

    /* Story, puzzles and items with named event flags. */
    if (Savegame_EventFlagGet(EventFlag_M0S01_CafeCutsceneStarted))    Ach_Unlock(0);  /* meet Cybil */
    if (Savegame_EventFlagGet(EventFlag_M2S00_LockOfLionOpen) &&
        Savegame_EventFlagGet(EventFlag_M2S00_LockOfWoodmanOpen) &&
        Savegame_EventFlagGet(EventFlag_M2S00_LockOfScarecrowOpen))    Ach_Unlock(1);  /* Keys for Eclipse */
    if (Savegame_EventFlagGet(EventFlag_M1S01_PickupGoldMedallion))    Ach_Unlock(2);
    if (Savegame_EventFlagGet(EventFlag_M1S01_PickupSilverMedallion))  Ach_Unlock(3);
    if (Savegame_EventFlagGet(EventFlag_M3S03_PickupPlateOfCat) &&      /* approximation: all four plates */
        Savegame_EventFlagGet(EventFlag_M3S03_PickupPlateOfHatter) &&
        Savegame_EventFlagGet(EventFlag_M3S03_PickupPlateOfTurtle) &&
        Savegame_EventFlagGet(EventFlag_M3S04_PickupPlateOfQueen))     Ach_Unlock(5);
    if (Savegame_EventFlagGet(EventFlag_M3S01_PickupUnknownLiquid))    Ach_Unlock(11); /* the glass vial */
    if (Savegame_EventFlagGet(EventFlag_M7S01_PickupStoneOfTime))      Ach_Unlock(15);
    if (Savegame_EventFlagGet(EventFlag_M0S02_PickupChannelingStone))  Ach_Unlock(55);

    /* Dagger of Melchior picked up while the Ring of Contract isn't. */
    if (!s_DaggerSeen && Savegame_EventFlagGet(EventFlag_M7S02_PickupDaggerOfMelchior))
    {
        s_DaggerSeen = 1;
        if (!Savegame_EventFlagGet(EventFlag_M7S01_PickupRingOfContract))
        {
            Ach_Unlock(19);
        }
    }

    /* Kill counts (12-bit counters, read through the ranking accessor). */
    ranged = func_8009146C(1);
    melee  = func_8009146C(0);
    if (ranged >= 20) Ach_Unlock(27);
    if (ranged >= 50) Ach_Unlock(28);
    if (ranged >= 90) Ach_Unlock(29);
    if (melee >= 20)  Ach_Unlock(30);
    if (melee >= 50)  Ach_Unlock(31);
    if (melee >= 90)  Ach_Unlock(32);

    if (g_SavegamePtr->pickedUpItemCount >= 140) Ach_Unlock(33);

    /* Endings. */
    endings = g_SavegamePtr->clearGameEndings;
    if (endings & GameEndingFlag_GoodPlus) Ach_Unlock(61);
    if (endings & GameEndingFlag_Good)     Ach_Unlock(62);
    if (endings & GameEndingFlag_BadPlus)  Ach_Unlock(63);
    if (endings & GameEndingFlag_Bad)      Ach_Unlock(64);
    if (endings & GameEndingFlag_Ufo)      Ach_Unlock(65);
}

/* ---- Title-menu ACHIEVEMENTS list ---- */

/* The title menu runs interlaced (480 lines): there text y = screen y - 128
 * (the in-game toast runs 240-line, where text y = screen y - 8). */
#define LIST_ROWS      8
#define LIST_ROW_Y0    (-18)
#define LIST_ROW_STEP  20
#define LIST_TITLE_MAX 22
#define DESC_LINE_MAX  26

static int s_ListSel;
static int s_ListTop;

void Pc_LocalAchUi_Open(void)
{
    Pc_LocalAch_Init();
    s_ListSel = 0;
    s_ListTop = 0;
}

/* Up/Down move (held repeats), Left/Right page, cancel (Circle) backs out.
 * Returns 0 once the player has left the list. */
int Pc_LocalAchUi_Update(void)
{
    u32 pulsed = g_Controller0->pulsedBtnFlags;

    if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
    {
        return 0;
    }

    if (pulsed & ControllerFlag_LStickUp)
    {
        s_ListSel = (s_ListSel > 0) ? s_ListSel - 1 : ACH_COUNT - 1;
    }
    if (pulsed & ControllerFlag_LStickDown)
    {
        s_ListSel = (s_ListSel < ACH_COUNT - 1) ? s_ListSel + 1 : 0;
    }
    if (pulsed & ControllerFlag_LStickLeft)
    {
        s_ListSel = (s_ListSel > LIST_ROWS) ? s_ListSel - LIST_ROWS : 0;
    }
    if (pulsed & ControllerFlag_LStickRight)
    {
        s_ListSel = (s_ListSel < ACH_COUNT - 1 - LIST_ROWS) ? s_ListSel + LIST_ROWS : ACH_COUNT - 1;
    }

    if (s_ListSel < s_ListTop)
    {
        s_ListTop = s_ListSel;
    }
    if (s_ListSel >= s_ListTop + LIST_ROWS)
    {
        s_ListTop = s_ListSel - LIST_ROWS + 1;
    }
    return 1;
}

/* Wrap `in` into lines of at most DESC_LINE_MAX characters, breaking at spaces. */
static const char* Ach_WrapLine(char* out, const char* in)
{
    int len, cut;

    while (*in == ' ')
    {
        in++;
    }
    len = (int)strlen(in);
    if (len <= DESC_LINE_MAX)
    {
        strcpy(out, in);
        return in + len;
    }
    cut = DESC_LINE_MAX;
    while (cut > 0 && in[cut] != ' ')
    {
        cut--;
    }
    if (cut == 0)
    {
        cut = DESC_LINE_MAX;
    }
    memcpy(out, in, (size_t)cut);
    out[cut] = '\0';
    return in + cut;
}

void Pc_LocalAchUi_Draw(void)
{
    char buf[48], line[48];
    int  i, row, unlockedCount = 0, points = 0;
    const char* rest;

    for (i = 0; i < ACH_COUNT; i++)
    {
        if (s_Unlocked[i])
        {
            unlockedCount++;
            points += (int)ACH_DEFS[i].points;
        }
    }

    Ach_Box(8, -88, 304, 402, 4, 4, 12, 0, 8); /* backing, behind the title art's text layer */

    Ach_Text(20, -68, StringColorId_Gold, "ACHIEVEMENTS");
    snprintf(buf, sizeof(buf), "%d/%d  %d/%d", unlockedCount, ACH_COUNT, points, ACH_TOTAL_POINTS);
    Ach_Text(150, -68, StringColorId_White, buf);

    for (row = 0; row < LIST_ROWS; row++)
    {
        int y, color;

        i = s_ListTop + row;
        if (i >= ACH_COUNT)
        {
            break;
        }
        y = LIST_ROW_Y0 + row * LIST_ROW_STEP;

        if (i == s_ListSel)
        {
            Ach_Box(14, y - 2, 292, 20, 40, 40, 80, 0, 7);
            color = s_Unlocked[i] ? StringColorId_Gold : StringColorId_LightGrey;
        }
        else
        {
            color = s_Unlocked[i] ? StringColorId_White : StringColorId_DarkGrey;
        }

        Ach_Fit(buf, ACH_DEFS[i].title, LIST_TITLE_MAX);
        Ach_Text(20, y, color, buf);

        if (s_Unlocked[i])
        {
            snprintf(buf, sizeof(buf), "%u", ACH_DEFS[i].points);
            Ach_Text(268, y, StringColorId_Green, buf);
        }
        else
        {
            Ach_Text(268, y, StringColorId_DarkGrey, "--");
        }
    }

    /* Detail for the selected entry. */
    rest = Ach_WrapLine(line, ACH_DEFS[s_ListSel].desc);
    Ach_Text(20, 172, StringColorId_White, line);
    Ach_WrapLine(line, rest);
    if (strlen(rest) > DESC_LINE_MAX) /* too long for two lines */
    {
        Ach_Fit(buf, line, DESC_LINE_MAX);
        strcpy(line, buf);
    }
    Ach_Text(20, 192, StringColorId_White, line);

    snprintf(buf, sizeof(buf), "%s  %u PTS", s_Unlocked[s_ListSel] ? "UNLOCKED" : "LOCKED", ACH_DEFS[s_ListSel].points);
    Ach_Text(20, 217, s_Unlocked[s_ListSel] ? StringColorId_Green : StringColorId_LightGrey, buf);
    if (!TRACKED[s_ListSel])
    {
        Ach_Text(190, 217, StringColorId_Red, "(not tracked yet)");
    }

    Ach_Text(20, 262, StringColorId_LightGrey, "UP/DOWN SELECT   CIRCLE BACK");
}
