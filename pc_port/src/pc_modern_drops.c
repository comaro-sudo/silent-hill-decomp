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
 * Placement: the drop waits until the body stops moving (bodies fall and
 * slide after the kill is counted), then lands beside it on the side facing
 * the camera, so the body never hides it (docs/plans/
 * modern-drop-placement-plan.md).
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
#include "bodyprog/collision/ray.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* D_800A99E4: common pickup model names */
#include "bodyprog/gfx/world_object.h"
#include "bodyprog/item_screens.h"                  /* Inventory_AddSpecialItem */
#include "bodyprog/items.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_data.h"             /* g_DeltaTime */
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/view/vc_main.h"
#include "main/rng.h"

#define DROP_COUNT_MAX     32
#define DROP_PER_MAP_MAX   8
#define DROP_PICKUP_RADIUS Q12(0.9f)
#define KILLS_PER_DROP     3
#define PENDING_MAX        4
#define SETTLE_STILL       Q12(0.5f)  /* seconds without moving */
#define SETTLE_MAX         Q12(2.0f)  /* seconds after the kill at most */
#define SETTLE_MOVE        Q12(0.05f)
#define DROP_SAME_FLOOR    Q12(0.4f)
#define DROP_APART         Q12(0.5f)

typedef struct
{
    u8       used;
    u8       pickupId; /* e_CommonPickupItemId */
    u8       mapIdx;
    u8       order;    /* spawn order within the map, oldest = lowest */
    VECTOR3  pos;
    SVECTOR3 rot;
} s_ModernDrop;

/* A kill counted, its drop waiting for the body to settle. */
typedef struct
{
    u8      used;
    u8      slot;
    s8      charaId;
    u8      mapIdx;
    q3_12   rotY;
    VECTOR3 last;
    q19_12  still;
    q19_12  age;
} s_PendingDrop;

static s_ModernDrop       s_Drops[DROP_COUNT_MAX];
static s_PendingDrop      s_Pending[PENDING_MAX];
static u8                 s_NextOrder = 0;
static s_WorldObjectModel s_Models[6];
static s32                s_ModelsMapIdx = -1;

/* Pickup id -> inventory item and pickup count (before Modern's x2). */
static const u8 PICKUP_ITEM[6]  = { InvItemId_FirstAidKit, InvItemId_HealthDrink, InvItemId_Ampoule,
                                    InvItemId_HandgunBullets, InvItemId_RifleShells, InvItemId_ShotgunShells };
static const u8 PICKUP_COUNT[6] = { DEFAULT_PICKUP_ITEM_COUNT, DEFAULT_PICKUP_ITEM_COUNT, DEFAULT_PICKUP_ITEM_COUNT,
                                    HANDGUN_AMMO_PICKUP_ITEM_COUNT, RIFLE_AMMO_PICKUP_ITEM_COUNT, SHOTGUN_AMMO_PICKUP_ITEM_COUNT };
/* Ammunition only (revision 2026-11-13): bullets 3, rifle 2, shells 2; no
 * health items. With no gun yet nothing qualifies and nothing drops. */
static const u8 PICKUP_WEIGHT[6] = { 0, 0, 0, 3, 2, 2 };

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
    Inventory_AddSpecialItem(PICKUP_ITEM[pickupId],
                             (pickupId >= CommonPickupItemId_HandgunBullets)
                                 ? Pc_Modern_AmmoCount(PICKUP_COUNT[pickupId] * (g_GameWork.config.extraBulletAdjust + 1))
                                 : Pc_Modern_HealthItemCount(PICKUP_COUNT[pickupId]));
}

static void Modern_DropSpawn(const VECTOR3* at, s32 mapIdx)
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
        if (s_Drops[pick].mapIdx == mapIdx)
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
    s_Drops[slot].mapIdx   = mapIdx;
    s_Drops[slot].order    = s_NextOrder++;
    s_Drops[slot].pos.vx   = at->vx;
    s_Drops[slot].pos.vz   = at->vz;
    s_Drops[slot].pos.vy   = at->vy;
    s_Drops[slot].rot.vx   = 0;
    s_Drops[slot].rot.vy   = Rng_Rand16() & 0xFFF;
    s_Drops[slot].rot.vz   = 0;
}

/* How far from the body's centre a drop lands: past the arms, legs and wings. */
static q19_12 Modern_DropDistance(s32 charaId)
{
    switch (charaId)
    {
        case Chara_Creeper:
        case Chara_LarvalStalker:
            return Q12(0.8f);
        case Chara_AirScreamer:
        case Chara_NightFlutter:
            return Q12(1.4f);
    }
    return Q12(1.1f);
}

/* A spot passes when it's on the same floor, nothing solid is between it and
 * the body, and no other drop lies there. */
static int Modern_DropSpotOk(const VECTOR3* body, q19_12 bodyGround, q19_12 x, q19_12 z, s32 mapIdx, VECTOR3* out)
{
    s_RayTrace tr;
    VECTOR3    from, to, spot;
    q19_12     ground = Collision_GroundHeightGet(x, z);
    s32        i;

    if (ABS(ground - bodyGround) > DROP_SAME_FLOOR)
    {
        return 0;
    }

    /* Waist height (Y grows down): low walls, fences and doors block it.
     * A character in the way (the body itself, Harry) doesn't. */
    from.vx = body->vx; from.vy = bodyGround - Q12(0.5f); from.vz = body->vz;
    to.vx   = x;        to.vy   = ground - Q12(0.5f);     to.vz   = z;
    memset(&tr, 0, sizeof(tr));
    if (Ray_TraceQuery(&tr, &from, &to) && tr.hasHit && tr.character == NULL)
    {
        return 0;
    }

    spot.vx = x; spot.vy = ground; spot.vz = z;
    for (i = 0; i < DROP_COUNT_MAX; i++)
    {
        if (s_Drops[i].used && s_Drops[i].mapIdx == mapIdx &&
            Math_Distance2dGet(&spot, &s_Drops[i].pos) < DROP_APART)
        {
            return 0;
        }
    }

    *out = spot;
    return 1;
}

/* Beside the body: across it (bodies lie along their facing) on the camera's
 * side, then the other side, then toward Harry, then all round, each at full,
 * three-quarter and half distance. Nothing fits: the body's own spot, as
 * before, so the item is never lost. */
static void Modern_DropPlace(const VECTOR3* body, q3_12 rotY, s32 charaId, s32 mapIdx)
{
    VECTOR3 cam;
    VECTOR3 out;
    q3_12   dirs[11];
    s32     dirCount   = 0;
    q19_12  bodyGround = Collision_GroundHeightGet(body->vx, body->vz);
    q19_12  full       = Modern_DropDistance(charaId);
    q3_12   side       = rotY + Q12_ANGLE(90.0f);
    s32     d, i;

    vcGetNowCamPos(&cam);
    if ((s64)Math_Sin(side) * (cam.vx - body->vx) + (s64)Math_Cos(side) * (cam.vz - body->vz) < 0)
    {
        side += Q12_ANGLE(180.0f);
    }
    dirs[dirCount++] = side;
    dirs[dirCount++] = side + Q12_ANGLE(180.0f);
    dirs[dirCount++] = ratan2(g_SysWork.playerWork.player.position.vx - body->vx,
                              g_SysWork.playerWork.player.position.vz - body->vz);
    for (i = 0; i < 8; i++)
    {
        dirs[dirCount++] = side + i * Q12_ANGLE(45.0f);
    }

    for (d = 4; d >= 2; d--)
    {
        q19_12 dist = full * d / 4;

        for (i = 0; i < dirCount; i++)
        {
            q19_12 x = body->vx + (q19_12)(((s64)Math_Sin(dirs[i]) * dist) >> 12);
            q19_12 z = body->vz + (q19_12)(((s64)Math_Cos(dirs[i]) * dist) >> 12);

            if (Modern_DropSpotOk(body, bodyGround, x, z, mapIdx, &out))
            {
                Modern_DropSpawn(&out, mapIdx);
                return;
            }
        }
    }

    out.vx = body->vx;
    out.vy = bodyGround;
    out.vz = body->vz;
    Modern_DropSpawn(&out, mapIdx);
}

/* Bodies fall and slide after the kill is counted: wait until this one has
 * stopped (or 2 s at most), then place the drop beside where it lies. */
static void Modern_PendingUpdate(void)
{
    s32 i;

    for (i = 0; i < PENDING_MAX; i++)
    {
        s_PendingDrop*  p = &s_Pending[i];
        s_SubCharacter* chara;

        if (!p->used)
        {
            continue;
        }

        chara = &g_SysWork.npcs[p->slot];
        if (p->mapIdx == g_SavegamePtr->mapIdx && chara->model.charaId == p->charaId)
        {
            if (Math_Distance2dGet(&chara->position, &p->last) > SETTLE_MOVE)
            {
                p->still = 0;
            }
            else
            {
                p->still += g_DeltaTime;
            }
            p->last = chara->position;
            p->rotY = chara->rotation.vy;
            p->age += g_DeltaTime;
            if (p->still < SETTLE_STILL && p->age < SETTLE_MAX)
            {
                continue;
            }
        }

        /* Settled, timed out, the body's slot reused, or the map changed. */
        p->used = 0;
        if (p->mapIdx == g_SavegamePtr->mapIdx)
        {
            Modern_DropPlace(&p->last, p->rotY, p->charaId, p->mapIdx);
        }
        else
        {
            VECTOR3 at = p->last;

            at.vy = Collision_GroundHeightGet(at.vx, at.vz);
            Modern_DropSpawn(&at, p->mapIdx);
        }
    }
}

static void Modern_DropQueue(s_SubCharacter* chara)
{
    s32 slot = chara - g_SysWork.npcs;
    s32 i;

    if (slot >= 0 && slot < NPC_COUNT_MAX)
    {
        for (i = 0; i < PENDING_MAX; i++)
        {
            if (!s_Pending[i].used)
            {
                s_Pending[i].used    = 1;
                s_Pending[i].slot    = (u8)slot;
                s_Pending[i].charaId = chara->model.charaId;
                s_Pending[i].mapIdx  = (u8)g_SavegamePtr->mapIdx;
                s_Pending[i].rotY    = chara->rotation.vy;
                s_Pending[i].last    = chara->position;
                s_Pending[i].still   = 0;
                s_Pending[i].age     = 0;
                return;
            }
        }
    }

    /* Not an npc slot, or four drops already waiting: place it now. */
    Modern_DropPlace(&chara->position, chara->rotation.vy, chara->model.charaId, g_SavegamePtr->mapIdx);
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
        Modern_DropQueue(chara);
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
    Modern_PendingUpdate();

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
    memset(s_Pending, 0, sizeof(s_Pending));
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
