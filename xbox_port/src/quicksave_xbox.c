/* Quick Save / Quick Load on Xbox.
 *
 * Same behaviour as the PC port's pc_quicksave.c — including the boss-fight
 * guard — but driven by the pad and the options menu instead of SDL keys, and
 * opening the ORIGINAL save/load screens through the exact sequence the in-game
 * save point (game_sys_states.c) and the inventory save path (item_screens_2.c
 * case 24) use, so the screen assets are loaded before the state switch.
 *
 * Two triggers:
 *   - BLACK / WHITE buttons, when bw_quick_save is on. Those normally map to
 *     L1/R1 = step left / step right; the option swaps that mapping (pad_xbox.c
 *     stops emitting L1/R1 for them so a quick save cannot also sidestep).
 *   - The "Quick Save" / "Quick Load" rows on Xbox Options page 2, which queue a
 *     request and leave the menu. The request is consumed here rather than acted
 *     on in the menu because this is the state the PC port proved: gameplay only,
 *     with the fs queue idle. It fires as soon as gameplay resumes.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/item_screens.h"
#include "main/fsqueue.h"
#include "sh_log.h"
#include "pc_config.h"

/* Queued by the options-menu rows (options.c). */
static int s_pendingSave;
static int s_pendingLoad;

void Xbox_QuickSaveRequest(void) { s_pendingSave = 1; s_pendingLoad = 0; }
void Xbox_QuickLoadRequest(void) { s_pendingLoad = 1; s_pendingSave = 0; }

/* Test harness (autoplay GIVE <id>): put an item in the first free slot. */
void Xbox_TestItemGive(int id)
{
    s32 i;
    if (g_SavegamePtr == NULL) return;
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == 0 || (u8)g_SavegamePtr->items[i].id_0 == 0xFF)
        {
            g_SavegamePtr->items[i].id_0    = (u8)id;
            g_SavegamePtr->items[i].count_1 = 1;
            SH_DBG("[TEST] item %d given (slot %d)", id, (int)i);
            return;
        }
    }
}

/* Test harness (autoplay FLAG <n>): set a savegame event flag in gameplay. */
void Xbox_TestEventFlagSet(int flag)
{
    if (g_SavegamePtr == NULL || flag < 0 || flag >= 52 * 32) return;
    Savegame_EventFlagSet(flag);
    SH_DBG("[TEST] event flag %d set", flag);
}

/* As equipping from the inventory does (pc_combat.c Pc_EquipWeapon, which the
 * console builds leave out). */
void Xbox_TestEquip(int id)
{
    extern void GameFs_WeaponInfoUpdate(void);
    s32 i, slot = -1, ammo = -1;
    if (g_SavegamePtr == NULL) return;
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == (u8)id)                         slot = i;
        if (g_SavegamePtr->items[i].id_0 == (u8)(id + InvItemId_HealthDrink)) ammo = i;
    }
    if (slot < 0) { SH_DBG("[TEST] equip %d: not owned", id); return; }
    g_Inventory_EquippedItem                  = (u8)id;
    g_SavegamePtr->equippedWeapon             = (u8)id;
    g_SysWork.playerCombat.weaponAttack       = id + InvItemId_KitchenKnife;
    g_SysWork.playerCombat.weaponInventoryIdx = slot;
    g_SysWork.playerCombat.currentWeaponAmmo  = g_SavegamePtr->items[slot].count_1;
    g_SysWork.playerCombat.totalWeaponAmmo    =
        (INV_ITEM_GROUP(id) == InvItemGroup_GunWeapons && ammo >= 0) ? (s8)g_SavegamePtr->items[ammo].count_1 : 0;
    GameFs_WeaponInfoUpdate();
    SH_DBG("[TEST] equipped %d (slot %d)", id, (int)slot);
}

/* Map points mark doors and triggers; a "touch, facing" trigger needs Harry
 * inside its box and turned toward it, which scripted stick input can't aim. */
void Xbox_TestWarpToPoint(int n)
{
    const s_MapPoint2d* p;
    if (g_SavegamePtr == NULL || n < 0 || g_MapOverlayHdr.mapPoints == NULL) return;
    p = &g_MapOverlayHdr.mapPoints[n];
    g_SysWork.playerWork.player.position.vx = p->positionX + Q12(0.4f);
    g_SysWork.playerWork.player.position.vz = p->positionZ;
    g_SysWork.playerWork.player.rotation.vy = Q12_ANGLE(270.0f);   /* toward -X */
    SH_DBG("[TEST] warp to map point %d (%d, %d)", n, (int)p->positionX, (int)p->positionZ);
}

/* A boss fight is the one place a quick save can strand a run: the save records
 * the player mid-arena, and reloading it drops them back into a fight they may
 * have entered with no health or ammo, with no way back out. The original game
 * has no save point inside a boss chamber for the same reason.
 *
 * Keyed on a LIVE boss actor rather than the map/room, so it covers every boss
 * without a hand-maintained room table, and lifts by itself the moment the boss
 * dies — the walk back out of the chamber stays saveable. Quick LOAD is left
 * alone: getting out is never the problem. */
static int Xbox_QuickSave_BossActive(void)
{
    static const unsigned char s_bossCharas[] = {
        Chara_SplitHead,    /* school basement / sewer */
        Chara_Floatstinger, /* Central Silent Hill otherworld */
        Chara_Twinfeeler,   /* Old Silent Hill */
        Chara_Bloodsucker,  /* sewers */
        Chara_Incubus,      /* Nowhere, final */
        Chara_MonsterCybil, /* amusement park carousel */
        Chara_Incubator     /* Nowhere, final */
    };

    int i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        int                   b;

        if (npc->health <= Q12(0.0f))
        {
            continue;
        }

        for (b = 0; b < (int)(sizeof(s_bossCharas) / sizeof(s_bossCharas[0])); b++)
        {
            if (npc->model.charaId == s_bossCharas[b])
            {
                return 1;
            }
        }
    }

    return 0;
}

void Pc_QuickSaveLoadUpdate(void)
{
    extern int Pad_XboxBlackWhite(int* black, int* white);   /* pad_xbox.c */

    static int prevSave = 0;
    static int prevLoad = 0;

    int black = 0, white = 0;
    int curSave, curLoad;

    /* White = save, Black = load. Only read as quick-save buttons when the option
     * is on; otherwise they are still the step-left/right binds and pressing them
     * must do nothing here. */
    if (g_PcConfig.bwQuickSave)
        Pad_XboxBlackWhite(&black, &white);

    curSave = white || s_pendingSave;
    curLoad = black || s_pendingLoad;

    /* Gameplay only: not menus, not cutscenes, not map events. A queued menu
     * request survives until we get there — that is the point of queueing it —
     * but a stale one must not fire into a later session, so it is dropped the
     * moment the game leaves gameplay for anything but a menu we came from. */
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState != SysState_Gameplay)
    {
        prevSave = prevLoad = 0;
        return;
    }

    if (curSave && !prevSave)
    {
        s_pendingSave = 0;
        if (Xbox_QuickSave_BossActive())
        {
            SH_DBG("[QUICK] save refused: boss fight active");
        }
        else
        {
            SH_DBG("[QUICK] opening save screen");
            SysWork_SavegameUpdatePlayer();
            Screen_Refresh(320, 0);
            GameFs_SaveLoadBinLoad();
            Fs_QueueWaitForEmpty();
            Game_StateSetNext(GameState_SaveScreen);
        }
    }
    else if (curLoad && !prevLoad)
    {
        s_pendingLoad = 0;
        SH_DBG("[QUICK] opening load screen");
        Screen_Refresh(320, 0);
        GameFs_SaveLoadBinLoad();
        Fs_QueueWaitForEmpty();
        Game_StateSetNext(GameState_LoadSavegameScreen);
    }

    prevSave = curSave;
    prevLoad = curLoad;
}
