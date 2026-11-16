/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Modern difficulty: when Harry seems stuck, the door toward the next story
 * goal shines (docs/plans/modern-door-guidance-plan.md).
 *
 * - Goals: pc_story_goals.inc, in walkthrough order. The first goal not done
 *   is the one guided to; optional tracks (the Unknown Liquid, Kaufmann's
 *   vial, Next Fear extras) are guided alongside with a dimmer shine.
 * - Between maps: pc_door_maps.inc (generated), searched breadth-first for
 *   the next map on the way.
 * - Inside the loaded map: the map's own event list, read the way
 *   Event_Update reads it (same flag rules; the first active event at a
 *   point wins, so a "locked" message hides its door), grouped into rooms
 *   with the map's own room lookup.
 * - Blocked: if the goal can't be reached through open doors, the locked
 *   spot that takes an item Harry carries is guided to instead (keys,
 *   medallions, plates, the Five Rites door), since that's what unblocks it.
 * - Stuck: 3 minutes on a goal, or passing the right door on two visits, or
 *   four doors off the route. Once lit, a track stays lit until its goal is
 *   done.
 * - Only real doors and gates shine (button-opened). Inside the goal's room
 *   the goal spot gets the dim shine.
 */
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/items.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "pc_config.h"
#include "pc_modern.h"
#include "sh_log.h"

extern int  Pc_KeyGlint_Project(const VECTOR3* p, s32 dyUp, s16* sx, s16* sy, s32* otz);
extern int  Pc_KeyGlint_WallBetween(const VECTOR3* p);
extern void PsyX_SetNextPrimSzExact(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3);

enum { TRACK_MAIN, TRACK_LIQUID, TRACK_KAUFMANN, TRACK_EXTRAS, TRACK_COUNT };

#define GOAL_NO_SPOT  (1 << 0)  /* reaching the room is the goal; nothing to point at */
#define GOAL_ANY_ROOM (1 << 1)  /* the whole map is the goal */

typedef struct
{
    u8     track;
    s16    done;
    s16    required;
    s16    expires;
    s8     mapIdx;
    q19_12 x, z;
    u8     flags;
    u8     item;
} s_StoryGoal;

typedef struct
{
    s8  from, to;
    s16 required, disabled;
} s_MapDoor;

static const s_StoryGoal GOALS[] = {
#include "pc_story_goals.inc"
};
#define GOAL_COUNT ((s32)(sizeof(GOALS) / sizeof(GOALS[0])))

static const s_MapDoor MAP_DOORS[] = {
#include "pc_door_maps.inc"
};
#define MAP_DOOR_COUNT ((s32)(sizeof(MAP_DOORS) / sizeof(MAP_DOORS[0])))

#define MAP_COUNT       45
#define EVENTS_MAX      256
#define DOORS_MAX       160
#define ROOMS_MAX       256
#define STUCK_TIME      Q12(180.0f)
#define STUCK_PASSES    2
#define STUCK_WRONG     4
#define SHINE_RANGE     Q12(20.0f)
#define ENEMY_NEAR      Q12(4.0f)

/* A way out of a room in the loaded map. */
typedef struct
{
    u8  fromRoom;
    s16 toRoom;   /* room in this map, or -1 - mapIdx for a map exit */
    u8  point;
    u8  isDoor;   /* button-opened: a real door or gate, the only kind that shines */
} s_RoomDoor;

typedef struct
{
    s32    goal;      /* GOALS index being guided to, -1 = none */
    s32    lit;       /* stuck: shining */
    q19_12 time;
    s32    passes;
    s32    wrong;
    s32    door;      /* s_Doors index to shine, -1 = none */
    s32    expectRoom;
    s32    doorRoom;
    s32    spot;      /* 1 = Harry is in the goal room: point at the goal spot */
    VECTOR3 at;       /* where the shine goes */
} s_Track;

static s_RoomDoor s_Doors[DOORS_MAX];
static s32        s_DoorCount;
static s32        s_DoorsMap = -1;
static s_Track    s_Tracks[TRACK_COUNT];
static s32        s_PrevRoom = -1;
static s32        s_PrevMap  = -1;
static u32        s_Frame;

/* ---- event and flag rules (as Event_Update) ------------------------------------------ */

static int Guide_EventActive(const s_EventData* e)
{
    if (e->requiredEventFlag != EventFlag_None && !Savegame_EventFlagGet(e->requiredEventFlag))
    {
        return 0;
    }
    if (e->disabledEventFlag != EventFlag_None && Savegame_EventFlagGet(e->disabledEventFlag) &&
        (e->disabledEventFlag < 867 || e->activationType == TriggerActivationType_Exclusive ||
         e->sysState == SysState_EventSetFlag))
    {
        return 0;
    }
    return 1;
}

static int Guide_FlagSet(s32 flag)
{
    return flag != 0 && Savegame_EventFlagGet(flag);
}

static int Guide_HasItem(s32 item)
{
    s32 i;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == item && g_SavegamePtr->items[i].count_1 > 0)
        {
            return 1;
        }
    }
    return 0;
}

static s32 Guide_RoomAt(q19_12 x, q19_12 z)
{
    if (g_MapOverlayHdr.mapRoomIdxGet == NULL)
    {
        return 0;
    }
    return g_MapOverlayHdr.mapRoomIdxGet(x, z);
}

static int Guide_EventCount(void)
{
    s32 n;

    if (g_MapOverlayHdr.mapEvents == NULL)
    {
        return 0;
    }
    for (n = 0; n < EVENTS_MAX && g_MapOverlayHdr.mapEvents[n].triggerType != NO_VALUE; n++);
    return n;
}

/* The ways out of each room, as the flags stand now. Rebuilt every half
 * second: doors open and close with the story. */
static void Guide_DoorsBuild(void)
{
    const s_EventData* ev = g_MapOverlayHdr.mapEvents;
    s32                n  = Guide_EventCount();
    u8                 claimed[32];
    s32                i;

    s_DoorCount = 0;
    s_DoorsMap  = g_SavegamePtr->mapIdx;
    memset(claimed, 0, sizeof(claimed));

    for (i = 0; i < n && s_DoorCount < DOORS_MAX; i++)
    {
        const s_EventData* e = &ev[i];
        const s_MapPoint2d* p;
        s32 point = e->pointOfInterestIdx;
        s32 walk, toRoom;

        if (!Guide_EventActive(e) || e->triggerType == TriggerType_None)
        {
            continue;
        }
        /* The first active button event at a point wins: a "locked" message
         * before the door event hides the door. */
        walk = (e->activationType != TriggerActivationType_Button && e->activationType != TriggerActivationType_Item);
        if (!walk && e->activationType == TriggerActivationType_Button)
        {
            if (claimed[point >> 3] & (1 << (point & 7)))
            {
                continue;
            }
            claimed[point >> 3] |= 1 << (point & 7);
        }
        if (e->sysState != SysState_LoadRoom && e->sysState != SysState_LoadOverlay)
        {
            continue;
        }

        p = &g_MapOverlayHdr.mapPoints[point];
        if (e->sysState == SysState_LoadRoom)
        {
            const s_MapPoint2d* dst = &g_MapOverlayHdr.mapPoints[e->eventParam];

            toRoom = Guide_RoomAt(dst->positionX, dst->positionZ);
        }
        else
        {
            toRoom = -1 - (s32)e->mapIdx;
        }

        s_Doors[s_DoorCount].fromRoom = (u8)Guide_RoomAt(p->positionX, p->positionZ);
        s_Doors[s_DoorCount].toRoom   = (s16)toRoom;
        s_Doors[s_DoorCount].point    = (u8)point;
        s_Doors[s_DoorCount].isDoor   = (e->activationType == TriggerActivationType_Button &&
                                         (e->triggerType == TriggerType_TouchFacing ||
                                          e->triggerType == TriggerType_TouchObbFacing));
        s_DoorCount++;
    }
}

/* ---- routes ------------------------------------------------------------------------- */

/* Next map on the way from `from` to `to` through the doors open now, -1 if none. */
static s32 Guide_NextMap(s32 from, s32 to)
{
    s8  prev[MAP_COUNT];
    s8  queue[MAP_COUNT];
    s32 head = 0, tail = 0, i, m;

    if (from == to)
    {
        return to;
    }
    memset(prev, -1, sizeof(prev));
    prev[from]    = (s8)from;
    queue[tail++] = (s8)from;
    while (head < tail)
    {
        m = queue[head++];
        for (i = 0; i < MAP_DOOR_COUNT; i++)
        {
            const s_MapDoor* d = &MAP_DOORS[i];

            if (d->from != m || prev[(s32)d->to] >= 0)
            {
                continue;
            }
            if ((d->required != 0 && !Savegame_EventFlagGet(d->required)) ||
                (d->disabled != 0 && Savegame_EventFlagGet(d->disabled)))
            {
                continue;
            }
            prev[(s32)d->to] = (s8)m;
            if (d->to == to)
            {
                for (m = to; prev[m] != from; m = prev[m]);
                return m;
            }
            queue[tail++] = d->to;
        }
    }
    return -1;
}

/* Breadth-first over rooms from Harry's room. Fills dist and the first door
 * taken to reach each room (-1 = none / start). */
static void Guide_RoomSearch(s32 start, s16* dist, s16* first)
{
    s16 queue[ROOMS_MAX];
    s32 head = 0, tail = 0, i;

    for (i = 0; i < ROOMS_MAX; i++)
    {
        dist[i]  = -1;
        first[i] = -1;
    }
    dist[start]   = 0;
    queue[tail++] = (s16)start;
    while (head < tail)
    {
        s32 r = queue[head++];

        for (i = 0; i < s_DoorCount; i++)
        {
            s32 t = s_Doors[i].toRoom;

            if (s_Doors[i].fromRoom != r || t < 0 || t >= ROOMS_MAX || dist[t] >= 0)
            {
                continue;
            }
            dist[t]       = dist[r] + 1;
            first[t]      = (r == start) ? (s16)i : first[r];
            queue[tail++] = (s16)t;
        }
    }
}

static void Guide_PointPos(s32 point, VECTOR3* out)
{
    const s_MapPoint2d* p = &g_MapOverlayHdr.mapPoints[point];

    out->vx = p->positionX;
    out->vz = p->positionZ;
    out->vy = Collision_GroundHeightGet(out->vx, out->vz);
}

/* Where this track's shine goes now: a door on the way (t->door), or the goal
 * spot when Harry is in the goal's room (t->spot). */
static void Guide_Route(s_Track* t, const s_StoryGoal* g, s32 harryRoom, int allowItemDoors)
{
    s16 dist[ROOMS_MAX], first[ROOMS_MAX];
    s32 mapIdx = g_SavegamePtr->mapIdx;
    s32 best = -1, bestDist = 0x7FFF, i;

    t->door = -1;
    t->spot = 0;
    t->expectRoom = -1;
    Guide_RoomSearch(harryRoom, dist, first);

    if (g->mapIdx == mapIdx)
    {
        s32 goalRoom = (g->flags & GOAL_ANY_ROOM) ? harryRoom : Guide_RoomAt(g->x, g->z);

        if (goalRoom == harryRoom)
        {
            if (!(g->flags & (GOAL_NO_SPOT | GOAL_ANY_ROOM)))
            {
                t->spot = 1;
                t->at.vx = g->x;
                t->at.vz = g->z;
                t->at.vy = Collision_GroundHeightGet(g->x, g->z);
            }
            return;
        }
        if (goalRoom >= 0 && goalRoom < ROOMS_MAX && dist[goalRoom] > 0)
        {
            best = first[goalRoom];
        }
    }
    else
    {
        s32 next = Guide_NextMap(mapIdx, g->mapIdx);

        /* The nearest way out to the next map. */
        for (i = 0; next >= 0 && i < s_DoorCount; i++)
        {
            s32 d;

            if (s_Doors[i].toRoom != -1 - next || dist[s_Doors[i].fromRoom] < 0)
            {
                continue;
            }
            d = dist[s_Doors[i].fromRoom];
            if (d < bestDist)
            {
                bestDist = d;
                best     = (d == 0) ? i : first[s_Doors[i].fromRoom];
            }
        }
    }

    if (best < 0 && allowItemDoors)
    {
        /* Blocked: the nearest locked spot that takes an item Harry carries. */
        const s_EventData* ev = g_MapOverlayHdr.mapEvents;
        s32                n  = Guide_EventCount();

        for (i = 0; i < n; i++)
        {
            const s_EventData* e = &ev[i];
            const s_MapPoint2d* p;
            s32 room, d;

            if (e->activationType != TriggerActivationType_Item || !Guide_EventActive(e) ||
                !Guide_HasItem(e->requiredItemId))
            {
                continue;
            }
            p    = &g_MapOverlayHdr.mapPoints[e->pointOfInterestIdx];
            room = Guide_RoomAt(p->positionX, p->positionZ);
            d    = dist[room];
            if (d < 0 || d >= bestDist)
            {
                continue;
            }
            bestDist = d;
            if (d == 0)
            {
                t->spot = 1;
                Guide_PointPos(e->pointOfInterestIdx, &t->at);
                best = -1;
            }
            else
            {
                t->spot = 0;
                best    = first[room];
            }
        }
    }

    if (best >= 0)
    {
        t->door       = best;
        t->expectRoom = s_Doors[best].toRoom;
        t->doorRoom   = s_Doors[best].fromRoom;
        Guide_PointPos(s_Doors[best].point, &t->at);
    }
}

/* ---- goals and the stuck rule ------------------------------------------------------- */

static s32 Guide_GoalFor(s32 track)
{
    s32 i;

    for (i = 0; i < GOAL_COUNT; i++)
    {
        const s_StoryGoal* g = &GOALS[i];

        if (g->track != track || Guide_FlagSet(g->done))
        {
            continue;
        }
        if (track == TRACK_MAIN)
        {
            return i;
        }
        if ((g->required != 0 && !Savegame_EventFlagGet(g->required)) || Guide_FlagSet(g->expires) ||
            (g->item != 0 && !Guide_HasItem(g->item)))
        {
            continue;
        }
        return i;
    }
    return -1;
}

static void Guide_RoomChanged(s_Track* t, s32 fromRoom, s32 toRoom, s32 mapChanged)
{
    if (t->goal < 0 || t->lit)
    {
        return;
    }
    if (t->door >= 0 && fromRoom == t->doorRoom)
    {
        s32 expected = t->expectRoom;
        s32 tookIt   = mapChanged ? (expected < 0) : (expected == toRoom);

        if (!tookIt)
        {
            t->passes++;
            t->wrong++;
        }
    }
    else if (t->door >= 0)
    {
        t->wrong++;
    }
}

static int Guide_EnemyNear(void)
{
    s32 i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];

        if (npc->model.charaId == Chara_None || npc->health <= 0)
        {
            continue;
        }
        if (Math_Distance2dGet(&npc->position, &g_SysWork.playerWork.player.position) < ENEMY_NEAR)
        {
            return 1;
        }
    }
    return 0;
}

/* ---- the shine ---------------------------------------------------------------------- */

/* A tall soft column over a door, or a small glow over a spot. Additive,
 * plain PSX prims, drawn with the key-item glint's pass. */
static void Guide_Draw(GsOT* ot, const VECTOR3* at, int door, int dim)
{
    POLY_G4*  p = (POLY_G4*)GsOUT_PACKET_P;
    DR_TPAGE* dt;
    VECTOR3   base = *at;
    s16       bx, by, tx, ty;
    s32       otz, otz2, bucket, sz, half, k;
    q19_12    period = dim ? 3 * 60 : 2 * 60;
    s32       pulse  = Q12(0.75f) + Q12_MULT(Q12(0.25f), Math_Sin((q19_12)(((s_Frame % period) * 4096) / period)));
    s32       level  = (dim ? 90 : 200) * pulse >> 12;
    q19_12    height = door ? (dim ? Q12(1.2f) : Q12(2.0f)) : Q12(0.6f);
    q19_12    lift   = door ? 0 : Q12(0.7f);

    base.vy -= lift;
    if (!Pc_KeyGlint_Project(&base, 0, &bx, &by, &otz) || !Pc_KeyGlint_Project(&base, height, &tx, &ty, &otz2))
    {
        return;
    }
    if (ABS(bx) > 230 || ABS(by) > 135)
    {
        return;
    }
    half = ABS(by - ty) / (door ? 5 : 2);
    if (half < 2)
    {
        half = 2;
    }

    bucket = (otz >> 3) - 2;
    if (bucket < 0) bucket = 0;
    if (bucket >= ORDERING_TABLE_SIZE) bucket = ORDERING_TABLE_SIZE - 1;
    sz = (otz << 2) - 160;
    if (sz < 1) sz = 1;

    /* Two halves, bright on the centre line, black at the edges and ends. */
    for (k = 0; k < 2; k++)
    {
        s32 s  = k ? half : -half;
        s32 my = (by + ty) / 2;
        s32 h;

        for (h = 0; h < 2; h++)
        {
            s32 y0 = h ? my : ty, y1 = h ? by : my;
            u8  c0 = (u8)(h ? level : 0), c1 = (u8)(h ? 0 : level);

            setPolyG4(p);
            setSemiTrans(p, 1);
            setXY4(p, bx, y0, bx + s, y0, bx, y1, bx + s, y1);
            setRGB0(p, c0, c0 * 15 / 16, c0 * 3 / 4);
            setRGB1(p, 0, 0, 0);
            setRGB2(p, c1, c1 * 15 / 16, c1 * 3 / 4);
            setRGB3(p, 0, 0, 0);
            PsyX_SetNextPrimSzExact((u16)sz, (u16)sz, (u16)sz, (u16)sz);
            addPrim(&ot->org[bucket], p);
            p++;
        }
    }

    dt = (DR_TPAGE*)p;
    setDrawTPage(dt, 0, 1, getTPage(0, 1, 0, 0));
    addPrim(&ot->org[bucket], dt);
    GsOUT_PACKET_P = (PACKET*)(dt + 1);
}

static int Guide_Visible(const VECTOR3* at)
{
    VECTOR3 mid = *at;

    if (Math_Distance2dGet(at, &g_SysWork.playerWork.player.position) > SHINE_RANGE)
    {
        return 0;
    }
    mid.vy -= Q12(1.1f);
    return !Pc_KeyGlint_WallBetween(&mid);
}

/* Cybil, possessed on the merry-go-round: a dim glow over her while Harry
 * carries the Unknown Liquid that saves her. */
static void Guide_Cybil(GsOT* ot)
{
    s32 i;

    if (!Guide_HasItem(InvItemId_UnknownLiquid))
    {
        return;
    }
    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];

        if (npc->model.charaId == Chara_MonsterCybil && npc->health > 0)
        {
            VECTOR3 at = npc->position;

            at.vy = Collision_GroundHeightGet(at.vx, at.vz) - Q12(1.0f);
            Guide_Draw(ot, &at, 0, 1);
        }
    }
}

/* ---- per frame, from the world draw (after the key-item glint) ---------------------- */

void Pc_DoorGuide_Draw(GsOT* ot)
{
    s32 mapIdx, room, k;
    int quiet;

    s_Frame++;
    if (!g_PcConfig.modernDoorGuide || !Pc_Modern_IsActive() || g_SavegamePtr == NULL ||
        g_GameWork.gameState != GameState_InGame || (g_SysWork.sysFlags & SysFlag_DemoActive) ||
        g_MapOverlayHdr.mapEvents == NULL || g_MapOverlayHdr.mapPoints == NULL)
    {
        return;
    }

    mapIdx = g_SavegamePtr->mapIdx;
    room   = Guide_RoomAt(g_SysWork.playerWork.player.position.vx, g_SysWork.playerWork.player.position.vz);
    quiet  = g_SysWork.sysState == SysState_Gameplay && !(g_SysWork.sysFlags & SysFlag_CutsceneActive);

    if (mapIdx != s_DoorsMap || (s_Frame % 30) == 0)
    {
        Guide_DoorsBuild();
    }
    if (mapIdx != s_PrevMap || room != s_PrevRoom)
    {
        for (k = 0; k < TRACK_COUNT; k++)
        {
            Guide_RoomChanged(&s_Tracks[k], s_PrevRoom, room, mapIdx != s_PrevMap);
        }
        s_PrevMap  = mapIdx;
        s_PrevRoom = room;
    }

    for (k = 0; k < TRACK_COUNT; k++)
    {
        s_Track* t    = &s_Tracks[k];
        s32      goal = Guide_GoalFor(k);

        if (goal != t->goal)
        {
            memset(t, 0, sizeof(*t));
            t->goal = goal;
            t->door = -1;
            if (goal >= 0)
            {
                SH_DBG("[GUIDE] track %d: goal %d (map %d, done flag %d)", (int)k, (int)goal,
                       (int)GOALS[goal].mapIdx, (int)GOALS[goal].done);
            }
        }
        if (goal < 0)
        {
            continue;
        }

        if (quiet)
        {
            t->time += g_DeltaTime;
        }
        if (!t->lit && (t->time >= STUCK_TIME || t->passes >= STUCK_PASSES || t->wrong >= STUCK_WRONG))
        {
            t->lit = 1;
            SH_DBG("[GUIDE] track %d stuck on goal %d (time %d s, passes %d, wrong doors %d): shine",
                   (int)k, (int)goal, (int)(t->time >> 12), (int)t->passes, (int)t->wrong);
        }

        if (((s_Frame + k * 7) % 15) == 0)
        {
            Guide_Route(t, &GOALS[goal], room, k == TRACK_MAIN);
        }

        if (!t->lit || !quiet || Guide_EnemyNear())
        {
            continue;
        }
        if (t->spot)
        {
            if (Guide_Visible(&t->at))
            {
                Guide_Draw(ot, &t->at, 0, 1);
            }
        }
        else if (t->door >= 0 && t->door < s_DoorCount && s_Doors[t->door].isDoor && Guide_Visible(&t->at))
        {
            Guide_Draw(ot, &t->at, 1, k != TRACK_MAIN);
        }
    }

    if (quiet)
    {
        Guide_Cybil(ot);
    }
}

/* Test hook (autoplay GUIDE): light every track now and log the routes. */
void Pc_DoorGuide_TestLight(void)
{
    s32 k;

    for (k = 0; k < TRACK_COUNT; k++)
    {
        s_Tracks[k].lit = 1;
        SH_DBG("[GUIDE] test: track %d goal %d door %d spot %d", (int)k, (int)s_Tracks[k].goal,
               (int)s_Tracks[k].door, (int)s_Tracks[k].spot);
    }
}
