/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Modern difficulty: every third enemy killed drops an item on the floor, and
 * full health when a boss dies. See pc_modern.h.
 *
 * Drops use the same world models as the maps' own pickups (the common pickup
 * model names in D_800A99E4[2..7]), drawn with WorldGfx_ObjectAdd just before
 * the world objects are submitted, so they look exactly like placed items.
 * A drop whose model the current map can't resolve goes straight into the
 * inventory instead, so nothing extra is loaded.
 *
 * Persistence: drops stay per map for the session (leaving and coming back
 * keeps them). They are cleared on a new game and on loading a save; the
 * per-save side file from the plan is not written yet. */
#include "pc_modern.h"

#include <string.h>

#include "game.h"
#include "bodyprog/sys/joy.h" /* g_Controller0 */

#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* D_800A99E4: common pickup model names */
#include "bodyprog/gfx/world_object.h"
#include "bodyprog/item_screens.h"                  /* Inventory_AddSpecialItem */
#include "bodyprog/items.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sound/sound_system.h"
#include "main/rng.h"

#define DROP_COUNT_MAX     32
#define DROP_PER_MAP_MAX   8
#define DROP_PICKUP_RADIUS Q12(0.9f)
#define KILLS_PER_DROP     3

typedef struct
{
    u8       used;
    u8       pickupId; /* e_CommonPickupItemId */
    u8       mapIdx;
    u8       order;    /* spawn order within the map, oldest = lowest */
    VECTOR3  pos;
    SVECTOR3 rot;
} s_ModernDrop;

static s_ModernDrop       s_Drops[DROP_COUNT_MAX];
static u8                 s_NextOrder = 0;
static s_WorldObjectModel s_Models[6];
static s32                s_ModelsMapIdx = -1;

/* Pickup id -> inventory item and pickup count (before Modern's x2). */
static const u8 PICKUP_ITEM[6]  = { InvItemId_FirstAidKit, InvItemId_HealthDrink, InvItemId_Ampoule,
                                    InvItemId_HandgunBullets, InvItemId_RifleShells, InvItemId_ShotgunShells };
static const u8 PICKUP_COUNT[6] = { DEFAULT_PICKUP_ITEM_COUNT, DEFAULT_PICKUP_ITEM_COUNT, DEFAULT_PICKUP_ITEM_COUNT,
                                    HANDGUN_AMMO_PICKUP_ITEM_COUNT, RIFLE_AMMO_PICKUP_ITEM_COUNT, SHOTGUN_AMMO_PICKUP_ITEM_COUNT };
/* Weights from the plan: bullets 3, shells 2, rifle 2, drink 3, kit 1, ampoule 1. */
static const u8 PICKUP_WEIGHT[6] = { 1, 3, 1, 3, 2, 2 };

/* Bosses never count as kills; their death restores health instead. */
static int Modern_IsBoss(s32 charaId)
{
    switch (charaId)
    {
        case Chara_SplitHead:
        case Chara_Floatstinger:
        case Chara_Twinfeeler:
        case Chara_Bloodsucker:
        case Chara_MonsterCybil:
        case Chara_Incubus:
        case Chara_Incubator:
            return 1;
    }
    return 0;
}

static int Modern_HasItem(u8 itemId)
{
    s32 i;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == itemId)
        {
            return 1;
        }
    }
    return 0;
}

static int Modern_PickupAllowed(s32 pickupId)
{
    switch (pickupId)
    {
        case CommonPickupItemId_HandgunBullets: return Modern_HasItem(InvItemId_Handgun);
        case CommonPickupItemId_RifleShells:    return Modern_HasItem(InvItemId_HuntingRifle);
        case CommonPickupItemId_ShotgunShells:  return Modern_HasItem(InvItemId_Shotgun);
    }
    return 1;
}

/* The model cache (metadata.lmIdx) is per map, so rebuild it on map change. */
static void Modern_ModelsSync(void)
{
    s32 i;

    if (s_ModelsMapIdx == g_SavegamePtr->mapIdx)
    {
        return;
    }

    memset(s_Models, 0, sizeof(s_Models));
    for (i = 0; i < 6; i++)
    {
        WorldObject_ModelNameSet(&s_Models[i], D_800A99E4[2 + i]);
    }
    s_ModelsMapIdx = g_SavegamePtr->mapIdx;
}

static void Modern_GiveDirect(s32 pickupId)
{
    Inventory_AddSpecialItem(PICKUP_ITEM[pickupId], PICKUP_COUNT[pickupId] *
                             ((pickupId >= CommonPickupItemId_HandgunBullets) ? Pc_Modern_AmmoMultMin(g_GameWork.config.extraBulletAdjust + 1)
                                                                              : Pc_Modern_HealthItemCount(1)));
}

static void Modern_DropSpawn(const VECTOR3* at)
{
    s32 weights[6];
    s32 total = 0;
    s32 i;
    s32 pick;
    s32 slot     = -1;
    s32 mapCount = 0;
    s32 oldest   = -1;

    for (i = 0; i < 6; i++)
    {
        weights[i] = Modern_PickupAllowed(i) ? PICKUP_WEIGHT[i] : 0;
        total     += weights[i];
    }
    if (total == 0)
    {
        return;
    }

    pick = Rng_Rand16() % total;
    for (i = 0; i < 6; i++)
    {
        if (pick < weights[i])
        {
            break;
        }
        pick -= weights[i];
    }

    /* At most DROP_PER_MAP_MAX per map: the oldest one goes into the inventory. */
    for (pick = 0; pick < DROP_COUNT_MAX; pick++)
    {
        if (!s_Drops[pick].used)
        {
            if (slot < 0)
            {
                slot = pick;
            }
            continue;
        }
        if (s_Drops[pick].mapIdx == g_SavegamePtr->mapIdx)
        {
            mapCount++;
            if (oldest < 0 || (u8)(s_Drops[pick].order - s_NextOrder) < (u8)(s_Drops[oldest].order - s_NextOrder))
            {
                oldest = pick;
            }
        }
    }
    if (mapCount >= DROP_PER_MAP_MAX && oldest >= 0)
    {
        Modern_GiveDirect(s_Drops[oldest].pickupId);
        s_Drops[oldest].used = 0;
        slot                 = oldest;
    }
    if (slot < 0)
    {
        Modern_GiveDirect(i); /* list full everywhere: never lose the item */
        return;
    }

    s_Drops[slot].used     = 1;
    s_Drops[slot].pickupId = i;
    s_Drops[slot].mapIdx   = g_SavegamePtr->mapIdx;
    s_Drops[slot].order    = s_NextOrder++;
    s_Drops[slot].pos.vx   = at->vx;
    s_Drops[slot].pos.vz   = at->vz;
    s_Drops[slot].pos.vy   = Collision_GroundHeightGet(at->vx, at->vz); /* flyers die in the air */
    s_Drops[slot].rot.vx   = 0;
    s_Drops[slot].rot.vy   = Rng_Rand16() & 0xFFF;
    s_Drops[slot].rot.vz   = 0;
}

void Pc_Modern_EnemyKilled(void* charaPtr)
{
    s_SubCharacter* chara = (s_SubCharacter*)charaPtr;
    s32             kills;

    if (!Pc_Modern_IsActive() || chara == NULL || Modern_IsBoss(chara->model.charaId))
    {
        return;
    }

    kills = ((g_SavegamePtr->unused_23E >> 1) & 0x3) + 1;
    if (kills >= KILLS_PER_DROP)
    {
        kills = 0;
        Modern_DropSpawn(&chara->position);
    }
    g_SavegamePtr->unused_23E = (g_SavegamePtr->unused_23E & 0x1) | (kills << 1);
}

/* Boss death watch: a boss whose health drops to 0 means the fight is won. */
static void Modern_BossWatch(void)
{
    static s32 s_PrevHealth[NPC_COUNT_MAX];
    static s8  s_PrevChara[NPC_COUNT_MAX];
    s32        i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];

        if (npc->model.charaId != s_PrevChara[i])
        {
            s_PrevChara[i]  = npc->model.charaId;
            s_PrevHealth[i] = npc->health;
            continue;
        }

        if (Modern_IsBoss(npc->model.charaId) &&
            npc->model.charaId != Chara_Incubus && npc->model.charaId != Chara_Incubator && /* ending follows */
            s_PrevHealth[i] > Q12(0.0f) && npc->health <= Q12(0.0f))
        {
            Pc_Modern_BossCleared();
        }
        s_PrevHealth[i] = npc->health;
    }
}

void Pc_Modern_DropsUpdate(void)
{
    s32             i;
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    int             canPick;

    if (!Pc_Modern_IsActive() || g_GameWork.gameState != GameState_InGame)
    {
        return;
    }

    Modern_BossWatch();
    Modern_ModelsSync();

    canPick = g_SysWork.sysState == SysState_Gameplay && !(g_SysWork.sysFlags & SysFlag_CutsceneActive) &&
              !g_Player_IsDead && (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter);

    for (i = 0; i < DROP_COUNT_MAX; i++)
    {
        s_ModernDrop* drop = &s_Drops[i];

        if (!drop->used || drop->mapIdx != g_SavegamePtr->mapIdx)
        {
            continue;
        }

        WorldGfx_ObjectAdd(&s_Models[drop->pickupId], &drop->pos, &drop->rot);

        /* No model for this item in this map: give it straight away. */
        if (s_Models[drop->pickupId].metadata.lmIdx == 0)
        {
            Modern_GiveDirect(drop->pickupId);
            drop->used = 0;
            continue;
        }

        if (canPick &&
            Math_Distance2dGet(&player->position, &drop->pos) < DROP_PICKUP_RADIUS &&
            ABS(Math_AngleNormalizeSigned(ratan2(drop->pos.vx - player->position.vx, drop->pos.vz - player->position.vz) -
                                          player->rotation.vy)) < Q12_ANGLE(60.0f))
        {
            Modern_GiveDirect(drop->pickupId);
            SD_Call(Sfx_MenuConfirm);
            drop->used = 0;
            canPick    = 0; /* one per press */
        }
    }
}

void Pc_Modern_DropsClear(void)
{
    memset(s_Drops, 0, sizeof(s_Drops));
    s_NextOrder    = 0;
    s_ModelsMapIdx = -1;
}

/* Side file per save slot: not written yet (see the file comment). */
void Pc_Modern_DropsSave(int slot, int saveIdx)
{
    (void)slot;
    (void)saveIdx;
}

void Pc_Modern_DropsLoad(int slot, int saveIdx)
{
    (void)slot;
    (void)saveIdx;
    Pc_Modern_DropsClear();
}
