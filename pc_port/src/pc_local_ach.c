/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local achievements: see pc_local_ach.h. No network code here. */
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
#include "pc_ach_screen.h"
#include "pc_modern.h"
#include "pc_ra_toast.h"

typedef struct
{
    const char* title;
    const char* desc;
    unsigned    points;
    const char* badgeFile;
} s_LocalAchDef;

static const s_LocalAchDef ACH_DEFS[] = {
#include "pc_local_ach_set.inc"
};
#define ACH_COUNT ((int)(sizeof(ACH_DEFS) / sizeof(ACH_DEFS[0])))

#define ACH_FILE      "achievements_local.txt"
#define ACH_FILE_TMP  "achievements_local.txt.tmp"
#define ACH_BADGE_DIR "gamedata/achievements/"

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

static void Ach_Save(void)
{
    FILE* f = fopen(ACH_FILE_TMP, "w");
    int   i;

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
    fclose(f);
    remove(ACH_FILE); /* rename() won't replace on Windows */
    rename(ACH_FILE_TMP, ACH_FILE);
}

void Pc_LocalAch_Init(void)
{
    FILE* f;
    char  line[64];

    if (s_Loaded)
    {
        return;
    }
    s_Loaded = 1;
    memset(s_Unlocked, 0, sizeof(s_Unlocked));

    f = fopen(ACH_FILE, "r");
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

static void Ach_Unlock(int idx)
{
    char badge[16];

    if (idx < 0 || idx >= ACH_COUNT || s_Unlocked[idx])
    {
        return;
    }
    s_Unlocked[idx] = (long long)time(NULL);
    Ach_Save();

    snprintf(badge, sizeof(badge), PC_LOCAL_ACH_BADGE_PREFIX "%02d", idx);
    Pc_RaToast_Show(ACH_DEFS[idx].title, ACH_DEFS[idx].desc, badge, ACH_DEFS[idx].points);
    Pc_LocalAch_RequestBadge(badge);
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
    if (g_SavegamePtr == NULL)
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

int Pc_LocalAch_Snapshot(PcRaAch* out, int max)
{
    int i, n = 0;

    Pc_LocalAch_Init();
    for (i = 0; i < ACH_COUNT && n < max; i++, n++)
    {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].title, sizeof(out[n].title), "%s", ACH_DEFS[i].title);
        snprintf(out[n].desc, sizeof(out[n].desc), TRACKED[i] ? "%s" : "%s (not tracked yet)", ACH_DEFS[i].desc);
        snprintf(out[n].badge, sizeof(out[n].badge), PC_LOCAL_ACH_BADGE_PREFIX "%02d", i);
        out[n].points     = ACH_DEFS[i].points;
        out[n].unlocked   = s_Unlocked[i] != 0;
        out[n].unlockTime = s_Unlocked[i] > 1 ? s_Unlocked[i] : 0;
    }
    return n;
}

int Pc_LocalAch_RequestBadge(const char* badge)
{
    char           path[256];
    FILE*          f;
    long           len;
    unsigned char* buf;
    int            idx;

    if (badge == NULL || strncmp(badge, PC_LOCAL_ACH_BADGE_PREFIX, 3) != 0)
    {
        return 0;
    }
    idx = atoi(badge + 3);
    if (idx < 0 || idx >= ACH_COUNT)
    {
        return 1;
    }

    snprintf(path, sizeof(path), ACH_BADGE_DIR "%s", ACH_DEFS[idx].badgeFile);
    f = fopen(path, "rb");
    if (!f)
    {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (len > 0) ? (unsigned char*)malloc((size_t)len) : NULL;
    if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len)
    {
        Pc_AchScreen_ProvideBadge(badge, buf, (size_t)len);
        Pc_RaToast_ProvideBadge(badge, buf, (size_t)len);
    }
    free(buf);
    fclose(f);
    return 1;
}
