/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_saves.c - staircase saves and Modern's autosaves
 * (docs/plans/staircase-saves-plan.md, modern-glint-and-boss-autosave-plan.md).
 *
 * - Every playthrough gets a run id (s_Savegame.unused_A1[0], 1..255) at New
 *   Game. Each card file header tags its 11 slots with the run id of the save
 *   in them and an "autosave" bit, in the header's unused unk_88 bytes (two
 *   bytes per slot), which the header checksum covers.
 * - Save screen: the cursor starts on the card's free entry ("previous + 1")
 *   instead of the newest save, so a save never overwrites one by default.
 * - Modern: after a key item (inventory ids 64-127) is picked up, and when
 *   Harry walks into a room with a live boss, the game writes this
 *   playthrough's single Autosave entry in the background, with no save
 *   screen. Before a boss it writes the snapshot taken at the last quiet moment
 *   in the previous room, so Continue puts Harry back in the room before.
 *
 * The write goes through the game's own memcard processes (Save_5 then Save_3,
 * as SaveScreen_SaveGame does), so the card layout, checksums and the save
 * list stay exactly the game's.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/memcard.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_fade.h"
#include "bodyprog/text/text_draw.h"
#include "screens/saveload.h"
#include "pc_config.h"
#include "pc_modern.h"
#include "sh_log.h"

#define RUN_ID(sg)        ((u8)(sg)->unused_A1[0])
#define TAG_RUN(hdr, k)   ((hdr)->unk_88[(k) * 2])
#define TAG_FLAGS(hdr, k) ((hdr)->unk_88[(k) * 2 + 1])
#define TAG_AUTOSAVE      0x01

#define KEY_ITEM_FIRST 64
#define KEY_ITEM_LAST  127

/* ---- run id ------------------------------------------------------------- */

static u8 Saves_NewRunId(void)
{
    unsigned v = (unsigned)time(NULL) * 2654435761u ^ (unsigned)VSync(-1) * 40503u;
    return (u8)(1 + (v % 255));
}

void Pc_Saves_NewRun(void)
{
    if (g_SavegamePtr == NULL) return;
    g_SavegamePtr->unused_A1[0] = (s8)Saves_NewRunId();
    { extern void Pc_KeyGlint_BitsSet(const u8* in); Pc_KeyGlint_BitsSet(NULL); }
    SH_DBG("[SAVES] new playthrough, run id %d", RUN_ID(g_SavegamePtr));
}

/* ---- what gets written, and how the header is tagged --------------------- */

static s_Savegame s_writeSnap;      /* boss autosave source */
static int        s_writeFromSnap;
static int        s_writingAutosave;

s_Savegame* Pc_Saves_SaveSource(void)
{
    return s_writeFromSnap ? &s_writeSnap : g_SavegamePtr;
}

/* Called by MemCard_TotalSavegameCountUpdate just before the header checksum. */
void Pc_Saves_HeaderTag(s_MemCard_SaveHeader* hdr, s32 saveIdx)
{
    s_Savegame* src = Pc_Saves_SaveSource();
    if (hdr == NULL || saveIdx < 0 || saveIdx >= MEMCARD_SAVES_COUNT_MAX) return;
    TAG_RUN(hdr, saveIdx)   = (s8)(src ? RUN_ID(src) : 0);
    TAG_FLAGS(hdr, saveIdx) = (s8)(s_writingAutosave ? TAG_AUTOSAVE : 0);
}

int Pc_Saves_IsAutosaveEntry(s32 deviceId, s32 fileIdx, s32 saveIdx)
{
    const s_MemCard_SaveHeader* hdr;
    if (deviceId < 0 || deviceId >= MEMCARD_DEVICE_COUNT_MAX || fileIdx < 0 || fileIdx >= MEMCARD_FILE_COUNT_MAX ||
        saveIdx < 0 || saveIdx >= MEMCARD_SAVES_COUNT_MAX) return 0;
    hdr = &g_MemCard_SaveWork.devices[deviceId].saveHeader[fileIdx];
    return (TAG_FLAGS(hdr, saveIdx) & TAG_AUTOSAVE) != 0;
}

/* ---- save screen: start on the free entry --------------------------------- */

static s32 Saves_NewestOfRun(s32 column, u8 runId)
{
    s_SaveScreenElement* e = MemCard_ActiveSavegameEntryGet(column);
    s32 i, best = -1, bestCount = 0;

    for (i = 0; i < g_Savegame_ElementCount1[column]; i++, e++)
    {
        s_MemCard_SaveHeader* hdr;
        if (e->type != SavegameEntryType_Save || e->saveMetadata == NULL) continue;
        hdr = &g_MemCard_SaveWork.devices[(u8)e->deviceId].saveHeader[(u8)e->fileIdx];
        if ((u8)TAG_RUN(hdr, e->elementIdx) != runId) continue;
        if (e->saveMetadata->totalSavegameCount > bestCount)
        {
            bestCount = e->saveMetadata->totalSavegameCount;
            best      = i;
        }
    }
    return best;
}

static s32 Saves_FreeEntry(s32 column)
{
    s_SaveScreenElement* e = MemCard_ActiveSavegameEntryGet(column);
    s32 i;
    for (i = 0; i < g_Savegame_ElementCount1[column]; i++, e++)
    {
        if (e->type == SavegameEntryType_NewSave || e->type == SavegameEntryType_NewFile) return i;
    }
    return -1;
}

static int s_staircaseDone;

void Pc_Saves_ScreenOpened(void) { s_staircaseDone = 0; }

/* After the game picked its default (the newest save on each card). The list
 * holds every save plus at most one free entry per card (the first free slot,
 * or "new file"), and saves can't be deleted, so the entry after a
 * playthrough's last save that isn't taken is that card's free entry. */
void Pc_Saves_StaircaseDefault(void)
{
    s32 pref, col, other, freeIdx;
    u8  runId;

    if (!g_PcConfig.saveStaircase || g_SaveScreen_SaveScreenState != SaveScreenState_Save) return;

    runId = (g_SavegamePtr != NULL) ? RUN_ID(g_SavegamePtr) : 0;
    pref  = g_SelectedSaveSlotIdx;      /* retail: the card with the newest save */
    if (runId != 0)
    {
        s32 n0 = Saves_NewestOfRun(0, runId), n1 = Saves_NewestOfRun(1, runId);
        if (n0 >= 0 && n1 < 0) pref = 0;
        if (n1 >= 0 && n0 < 0) pref = 1;
    }

    for (col = 0; col < MEMCARD_SLOT_COUNT_MAX; col++)
    {
        freeIdx = Saves_FreeEntry(col);
        if (freeIdx >= 0) g_SlotElementSelectedIdx[col] = (u8)freeIdx;
    }

    other = 1 - pref;
    if (Saves_FreeEntry(pref) >= 0)       g_SelectedSaveSlotIdx = (s8)pref;
    else if (Saves_FreeEntry(other) >= 0) g_SelectedSaveSlotIdx = (s8)other;
    /* both cards full: the game's own default (overwrite asks to confirm) */
}

void Pc_Saves_StaircaseOnce(void)
{
    if (s_staircaseDone || g_MemCard_TotalElementsCount <= 0) return;
    s_staircaseDone = 1;
    Pc_Saves_StaircaseDefault();
}

/* ---- Modern autosave ------------------------------------------------------ */

typedef enum
{
    AS_Idle,
    AS_Scan,
    AS_Write5,
    AS_Write3,
} e_AutosaveState;

static e_AutosaveState s_asState;
static int             s_asTimer;
static int             s_asPendingKey;   /* a key item came in */
static int             s_asPendingBoss;  /* a boss room was entered: write s_prevRoomSnap */
static s32             s_asDev, s_asFile, s_asSlot;
static int             s_noticeTimer;    /* "card full" notice */
static int             s_savingTimer;    /* keeps "Saving..." up briefly after a fast write */

static int Saves_IsEnemy(s32 charaId)
{
    return (charaId >= Chara_AirScreamer && charaId <= Chara_MonsterCybil && charaId != Chara_Chicken) ||
           charaId == Chara_Incubator;
}

static int Saves_IsBoss(s32 charaId)
{
    switch (charaId)
    {
        case Chara_SplitHead:
        case Chara_Twinfeeler:
        case Chara_Floatstinger:
        case Chara_Bloodsucker:
        case Chara_MonsterCybil:
        case Chara_Incubus:
        case Chara_Incubator:
            return 1;
        default:
            return 0;
    }
}

/* Harry has control, nothing is fading, and no live enemy is within 12 m. */
static int Saves_QuietMoment(void)
{
    s32 i;

    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) return 0;
    if (g_SysWork.sysFlags & SysFlag_DemoActive) return 0;
    if ((g_Screen_FadeStatus & 0x7) > ScreenFadeState_None) return 0;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* n = &g_SysWork.npcs[i];
        q19_12 dx, dz;
        if (!Saves_IsEnemy(n->model.charaId) || n->health <= Q12(0.0f)) continue;
        dx = (n->position.vx - g_SysWork.playerWork.player.position.vx) >> 6;
        dz = (n->position.vz - g_SysWork.playerWork.player.position.vz) >> 6;
        if (dx * dx + dz * dz < (Q12(12.0f) >> 6) * (Q12(12.0f) >> 6)) return 0;
    }
    return 1;
}

static void Saves_PlayerIntoSave(s_Savegame* s)
{
    /* SysWork_SavegameUpdatePlayer without the location name: a save point
     * sets that from its own event parameter, stale during normal play. */
    s->playerPositionX = g_SysWork.playerWork.player.position.vx;
    s->playerPositionZ = g_SysWork.playerWork.player.position.vz;
    s->playerRotationY = g_SysWork.playerWork.player.rotation.vy;
    s->playerHealth    = g_SysWork.playerWork.player.health;
}

static int Saves_DeviceReady(s32 dev)
{
    return (MemCard_StatusGet(MemCard_AllMemCardsStatusGet(), dev)) == MemCardState_Available;
}

/* This playthrough's Autosave entry, else the first free slot (existing files
 * first, then a new file), on the first card that has one. */
static int Saves_PickTarget(u8 runId)
{
    s32 dev, file, slot;

    for (dev = 0; dev < MEMCARD_DEVICE_COUNT_MAX; dev++)
    {
        if (!Saves_DeviceReady(dev)) continue;
        for (file = 0; file < MEMCARD_FILE_COUNT_MAX; file++)
        {
            s_MemCard_SaveHeader* hdr;
            if (g_MemCard_SaveWork.devices[dev].fileState[file] != FileState_Used) continue;
            hdr = &g_MemCard_SaveWork.devices[dev].saveHeader[file];
            for (slot = 0; slot < MEMCARD_SAVES_COUNT_MAX; slot++)
            {
                if (hdr->saveMetadata[slot].totalSavegameCount > 0 && (u8)TAG_RUN(hdr, slot) == runId &&
                    (TAG_FLAGS(hdr, slot) & TAG_AUTOSAVE))
                {
                    s_asDev = dev; s_asFile = file; s_asSlot = slot;
                    return 1;
                }
            }
        }
    }

    for (dev = 0; dev < MEMCARD_DEVICE_COUNT_MAX; dev++)
    {
        if (!Saves_DeviceReady(dev)) continue;
        for (file = 0; file < MEMCARD_FILE_COUNT_MAX; file++)
        {
            s_MemCard_SaveHeader* hdr;
            if (g_MemCard_SaveWork.devices[dev].fileState[file] != FileState_Used) continue;
            hdr = &g_MemCard_SaveWork.devices[dev].saveHeader[file];
            for (slot = 0; slot < MEMCARD_SAVES_COUNT_MAX; slot++)
            {
                if (hdr->saveMetadata[slot].totalSavegameCount <= 0)
                {
                    s_asDev = dev; s_asFile = file; s_asSlot = slot;
                    return 1;
                }
            }
        }
        if (MemCard_FreeFilesCount(dev) > 0)
        {
            for (file = 0; file < MEMCARD_FILE_COUNT_MAX; file++)
            {
                if (g_MemCard_SaveWork.devices[dev].fileState[file] == FileState_Unused)
                {
                    s_asDev = dev; s_asFile = file; s_asSlot = 0;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void Saves_AutosaveFinish(const char* why)
{
    SH_DBG("[SAVES] autosave %s (dev %d file %d slot %d)", why, (int)s_asDev, (int)s_asFile, (int)s_asSlot);
    s_writeFromSnap    = 0;
    s_writingAutosave  = 0;
    s_asState          = AS_Idle;
    {
        /* The background scan restarts after the write and marks the device it
         * is reading "loading". Switching off mid-scan left that status stuck,
         * and the save screen waits on it forever (no input, 2026-11-15). Back
         * to "unknown": the next start rescans like the first one does. */
        s32 i;
        for (i = 0; i < MEMCARD_DEVICE_COUNT_MAX; i++) g_MemCard_SaveWork.devices[i].status = 0;
    }
    MemCard_SysDisable();
}

static void Saves_AutosaveStep(void)
{
    s32 result;

    switch (s_asState)
    {
        case AS_Idle:
            break;

        case AS_Scan:
        {
            s32 st0 = MemCard_StatusGet(MemCard_AllMemCardsStatusGet(), 0);
            s32 st1 = MemCard_StatusGet(MemCard_AllMemCardsStatusGet(), 4);
            int settled = (st0 != MemCardState_Null && st0 != MemCardState_Loading) &&
                          (st1 != MemCardState_Null && st1 != MemCardState_Loading);

            if (++s_asTimer > 600) { Saves_AutosaveFinish("gave up: card scan timed out"); break; }
            if (!settled || s_asTimer < 20 || g_MemCard_SaveWork.saveWork[0].processId != MemCardProcess_None) break;

            {
                s_Savegame*             src = Pc_Saves_SaveSource();
                s_MemCard_SaveMetadata* meta;

                if (RUN_ID(src) == 0) src->unused_A1[0] = g_SavegamePtr->unused_A1[0] = (s8)Saves_NewRunId();
                if (!Saves_PickTarget(RUN_ID(src)))
                {
                    s_noticeTimer = 150;
                    Saves_AutosaveFinish("skipped: no free space on any card");
                    break;
                }

                /* As SaveScreen_SaveGame case 0. */
                meta = MemCard_SaveMetadataGet(s_asDev, s_asFile, s_asSlot);
                src->savegameCount++;
                if (src != g_SavegamePtr) g_SavegamePtr->savegameCount = src->savegameCount;
                meta->savegameCount            = src->savegameCount;
                meta->locationId               = src->locationId;
                meta->gameplayTimer            = src->gameplayTimer;
                meta->isNextFearMode           = src->isNextFearMode;
                meta->add290Hours              = src->add290Hours;
                meta->pickedUpSpecialItemCount = src->pickedUpSpecialItemCount;

                s_writingAutosave = 1;
                if (MemCard_ProcessSet(MemCardProcess_Save_5, s_asDev, s_asFile, s_asSlot))
                {
                    s_asState = AS_Write5;
                    s_asTimer = 0;
                }
                else
                {
                    Saves_AutosaveFinish("gave up: card busy");
                }
            }
            break;
        }

        case AS_Write5:
        case AS_Write3:
            if (!g_MemCard_AvailibityStatus) { Saves_AutosaveFinish("aborted: card system closed"); break; }
            if (++s_asTimer > 900) { Saves_AutosaveFinish("gave up: write timed out"); break; }
            result = MemCard_LastMemCardResultGet();
            if (result == MemCardResult_Success) break;  /* still running */
            if (result != MemCardResult_FileIoComplete) { Saves_AutosaveFinish("FAILED"); break; }

            if (s_asState == AS_Write5)
            {
                if (MemCard_ProcessSet(MemCardProcess_Save_3, s_asDev, s_asFile, 0))
                {
                    s_asState = AS_Write3;
                    s_asTimer = 0;
                }
                else
                {
                    Saves_AutosaveFinish("written (options skipped)");
                }
            }
            else
            {
                /* The in-RAM Continue point, as after a manual save. */
                g_GameWork.autosave = *Pc_Saves_SaveSource();
                Saves_AutosaveFinish("written");
            }
            break;
    }
}

static void Saves_AutosaveStart(int fromSnap)
{
    s_writeFromSnap = fromSnap;
    s_asState       = AS_Scan;
    s_asTimer       = 0;
    s_savingTimer   = 60;
    MemCard_SysInit2();
    MemCard_InitStatus();
    SH_DBG("[SAVES] autosave starting (%s)", fromSnap ? "before boss room" : "key item");
}

/* ---- triggers ------------------------------------------------------------- */

static u32 s_keyHave[2];   /* bit (id - 64) */
static int s_keyValid;

static void Saves_KeyItemsScan(void)
{
    u32 now[2] = { 0, 0 };
    s32 i;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        s32 id = (u8)g_SavegamePtr->items[i].id_0;
        if (id >= KEY_ITEM_FIRST && id <= KEY_ITEM_LAST && g_SavegamePtr->items[i].count_1 > 0)
            now[(id - KEY_ITEM_FIRST) >> 5] |= 1u << ((id - KEY_ITEM_FIRST) & 31);
    }
    if (s_keyValid && ((now[0] & ~s_keyHave[0]) | (now[1] & ~s_keyHave[1])))
    {
        s_asPendingKey = 1;
        SH_DBG("[SAVES] key item picked up: autosave pending");
    }
    s_keyHave[0] = now[0];
    s_keyHave[1] = now[1];
    s_keyValid   = 1;
}

static s_Savegame s_roomSnap;      /* last quiet moment in the current room */
static s_Savegame s_prevRoomSnap;  /* ... and in the room before it */
static int        s_roomSnapValid, s_prevRoomSnapValid;
static s32        s_roomKey = -1;
static int        s_bossHandled;   /* this room's boss already caused an autosave */
static int        s_snapTick;

static int s_bossSim;   /* test harness (autoplay BOSSSIM): pretend a boss is in the room */
void Pc_Saves_TestBossSim(void) { s_bossSim = 1; SH_DBG("[TEST] boss simulated in this room"); }

static void Saves_BossScan(void)
{
    s32 key = (s32)g_SavegamePtr->mapIdx * 256 + (u8)g_SavegamePtr->mapRoomIdx;
    s32 i;

    if (key != s_roomKey)
    {
        if (s_roomSnapValid)
        {
            s_prevRoomSnap      = s_roomSnap;
            s_prevRoomSnapValid = 1;
        }
        s_roomSnapValid = 0;
        s_roomKey       = key;
        s_bossHandled   = 0;
        s_bossSim       = 0;
    }

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* n = &g_SysWork.npcs[i];
        if (Saves_IsBoss(n->model.charaId) && n->health > Q12(0.0f)) break;
    }
    if (i < NPC_COUNT_MAX || s_bossSim)
    {
        if (!s_bossHandled && !s_prevRoomSnapValid)
        {
            s_bossHandled = 1;
            SH_DBG("[SAVES] boss in room (chara %d) but no quiet moment in a previous room: no autosave",
                   i < NPC_COUNT_MAX ? (int)g_SysWork.npcs[i].model.charaId : -1);
        }
        if (!s_bossHandled && s_prevRoomSnapValid)
        {
            s_bossHandled   = 1;
            s_asPendingBoss = 1;
            s_writeSnap     = s_prevRoomSnap;
            SH_DBG("[SAVES] boss in room (chara %d): autosave of the room before pending",
                   i < NPC_COUNT_MAX ? (int)g_SysWork.npcs[i].model.charaId : -1);
        }
        return;
    }

    /* Snapshot quiet moments, about every second. */
    if (++s_snapTick >= 30 && Saves_QuietMoment())
    {
        s_snapTick = 0;
        s_roomSnap = *g_SavegamePtr;
        Saves_PlayerIntoSave(&s_roomSnap);
        s_roomSnapValid = 1;
    }
}

static void Saves_ResetSession(void)
{
    s_keyValid          = 0;
    s_roomSnapValid     = 0;
    s_prevRoomSnapValid = 0;
    s_roomKey           = -1;
    s_bossHandled       = 0;
    s_asPendingKey      = 0;
    s_asPendingBoss     = 0;
}

static void Saves_TextDraw(int x, int y, const char* s)
{
    char buf[40];
    char* p;
    snprintf(buf, sizeof(buf), "%s", s);
    for (p = buf; *p; p++)
        if (*p == ' ') *p = '_';   /* the game's string drawer spells a space '_' */
    Gfx_StringSetPosition(x, y);
    Gfx_StringSetColor(StringColorId_White);
    Gfx_StringDraw(buf, (s32)strlen(buf));
}

void Pc_Saves_Update(void)
{
    int modern;

    if (g_SavegamePtr == NULL) return;

    if (g_GameWork.gameState != GameState_InGame || (g_SysWork.sysFlags & SysFlag_DemoActive))
    {
        if (s_asState == AS_Idle) Saves_ResetSession();
    }
    else
    {
        modern = Pc_Modern_IsActive();
        if (g_SysWork.sysState == SysState_Gameplay)
        {
            if (modern && g_PcConfig.autosaveKeyItems)   Saves_KeyItemsScan();
            if (modern && g_PcConfig.autosaveBeforeBoss) Saves_BossScan();
        }

        if (s_asState == AS_Idle && modern)
        {
            if (s_asPendingBoss)
            {
                /* The snapshot is already the room before; write it now. */
                s_asPendingBoss = 0;
                Saves_AutosaveStart(1);
            }
            else if (s_asPendingKey && Saves_QuietMoment())
            {
                s_asPendingKey = 0;
                Saves_PlayerIntoSave(g_SavegamePtr);
                Saves_AutosaveStart(0);
            }
        }
    }

    Saves_AutosaveStep();

    if (s_asState != AS_Idle || s_savingTimer > 0)
    {
        if (s_asState == AS_Idle) s_savingTimer--;
        if (g_GameWork.gameState == GameState_InGame) Saves_TextDraw(16, 212, "Saving...");
    }
    else if (s_noticeTimer > 0)
    {
        s_noticeTimer--;
        if (g_GameWork.gameState == GameState_InGame) Saves_TextDraw(16, 212, "Memory card full");
    }
}
