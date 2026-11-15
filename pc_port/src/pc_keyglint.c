/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_keyglint.c - Modern: a key item the player has seen on two separate
 * visits to its room, without picking it up, starts to shine
 * (docs/plans/modern-glint-and-boss-autosave-plan.md, section 1).
 *
 * - Where items lie: pc_key_items.inc, generated from the map event data by
 *   pc_port/tools/gen_key_items.py (map, item, picked-up flag, X/Z).
 * - Height: the map's own world object for the item, recorded as the map adds
 *   it this frame (WorldGfx_ObjectAdd), else the floor plus a little.
 * - A sighting: on screen, within 12 m of Harry, and no wall between the
 *   camera and the item (Ray_TraceQuery). At most one per room visit.
 * - "Seen on an earlier visit" is one bit per key item, saved with the game in
 *   the card file's last sector (memcard.c, PC-only steps).
 * - The glint: an additive gouraud fan with four spikes, plain PSX prims, so
 *   every renderer draws it. Nothing else changes: no sound, no text.
 */
#include <math.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/collision/ray.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/view/vc_main.h"
#include "bodyprog/view/vw_calc.h"
#include "bodyprog/view/vw_system.h"
#include "map_registry.h"
#include "pc_config.h"
#include "pc_modern.h"
#include "sh_log.h"

extern long ReadGeomScreen(void);
extern void PsyX_SetNextPrimSzExact(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3);

typedef struct
{
    const char* map;
    u8          item;
    u16         flag;
    q19_12      x, z;
    u16         required;  /* the pickup exists only once this flag is set; 0 = always */
} s_KeyItemSpot;

static const s_KeyItemSpot KEY_ITEMS[] = {
#include "pc_key_items.inc"
};
#define KEY_ITEM_SPOTS ((s32)(sizeof(KEY_ITEMS) / sizeof(KEY_ITEMS[0])))

#define ITEM_BASE    64
#define SIGHT_RANGE  Q12(12.0f)

static s16 s_spotMap[KEY_ITEM_SPOTS];  /* map index, resolved once */
static int s_spotMapReady;

static u8  s_seenEarlier[8];   /* saved: bit (item - 64) */
static u8  s_seenVisit[8];     /* this room visit */
static u8  s_shining[8];       /* shining since its second sighting (this session) */
static u32 s_visit;            /* bumps on every room change */
static u32 s_firstVisit[64];   /* visit of the first sighting this session */
static s32 s_roomKey = -1;
static u32 s_frame;

#define BIT_GET(a, i) (((a)[(i) >> 3] >> ((i) & 7)) & 1)
#define BIT_SET(a, i) ((a)[(i) >> 3] |= (u8)(1 << ((i) & 7)))

/* ---- the map's own object positions, this frame ------------------------------ */

#define OBJ_MAX 64
static VECTOR3 s_objPos[OBJ_MAX];
static int     s_objCount;

void Pc_KeyGlint_ObjectAdded(const VECTOR3* pos)
{
    if (pos != NULL && s_objCount < OBJ_MAX) s_objPos[s_objCount++] = *pos;
}

/* ---- saved state ------------------------------------------------------------- */

void Pc_KeyGlint_BitsGet(u8 out[8])
{
    memcpy(out, s_seenEarlier, 8);
}

void Pc_KeyGlint_BitsSet(const u8 in[8])
{
    if (in != NULL) memcpy(s_seenEarlier, in, 8);
    else            memset(s_seenEarlier, 0, 8);
    memset(s_seenVisit, 0, sizeof(s_seenVisit));
    memset(s_shining, 0, sizeof(s_shining));
    s_visit++;
    s_roomKey = -1;
}

/* The card file's last sector holds one 11-byte record per save slot:
 * 0x5A, 8 bytes of bits, their byte sum, 0xA5. */
#define REC_SIZE 11

void Pc_KeyGlint_RecordWrite(u8* sector, s32 slot)
{
    u8* r = sector + slot * REC_SIZE;
    u8  sum = 0;
    s32 i;
    if (slot < 0 || slot * REC_SIZE + REC_SIZE > 128) return;
    r[0] = 0x5A;
    for (i = 0; i < 8; i++) { r[1 + i] = s_seenEarlier[i]; sum += s_seenEarlier[i]; }
    r[9]  = sum;
    r[10] = 0xA5;
}

void Pc_KeyGlint_RecordRead(const u8* sector, s32 slot)
{
    const u8* r;
    u8        sum = 0;
    s32       i;

    if (sector == NULL || slot < 0 || slot * REC_SIZE + REC_SIZE > 128) { Pc_KeyGlint_BitsSet(NULL); return; }
    r = sector + slot * REC_SIZE;
    for (i = 0; i < 8; i++) sum += r[1 + i];
    if (r[0] != 0x5A || r[10] != 0xA5 || r[9] != sum)
    {
        Pc_KeyGlint_BitsSet(NULL);   /* a save from before this feature */
        return;
    }
    Pc_KeyGlint_BitsSet(r + 1);
    SH_DBG("[GLINT] sightings loaded for slot %d", (int)slot);
}

/* Test harness (autoplay GLINTALL): every key item counts as seen twice. */
void Pc_KeyGlint_TestAll(void)
{
    memset(s_seenEarlier, 0xFF, sizeof(s_seenEarlier));
    memset(s_shining, 0xFF, sizeof(s_shining));
    SH_DBG("[TEST] all key items shine");
}

/* Test harness (autoplay WARPKEY <n>): Harry 2.5 m from spot n, facing it. */
void Pc_KeyGlint_TestWarp(int n)
{
    const s_KeyItemSpot* s;
    if (n < 0 || n >= KEY_ITEM_SPOTS || g_SavegamePtr == NULL) return;
    s = &KEY_ITEMS[n];
    g_SysWork.playerWork.player.position.vx = s->x + Q12(2.5f);
    g_SysWork.playerWork.player.position.vz = s->z;
    g_SysWork.playerWork.player.rotation.vy = Q12_ANGLE(270.0f);   /* toward -X */
    SH_DBG("[TEST] warp to key item %d (item %d, %s)", n, (int)s->item, s->map);
}

/* ---- sightings ---------------------------------------------------------------- */

static int Glint_InInventory(s32 item)
{
    s32 i;
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if ((u8)g_SavegamePtr->items[i].id_0 == item && g_SavegamePtr->items[i].count_1 > 0) return 1;
    }
    return 0;
}

static q19_12 Glint_Isqrt(s64 v)
{
    s64 r = 0, b = (s64)1 << 46;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b != 0)
    {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return (q19_12)r;
}

/* The item's world position: the map's object for it if one was added this
 * frame within 1.5 m of the pickup point, else the floor there, raised. */
static void Glint_ItemPos(const s_KeyItemSpot* s, VECTOR3* out)
{
    s32    i, best = -1;
    s64    bestD = (s64)Q12(0.6f) * Q12(0.6f);
    q19_12 floorY = Collision_GroundHeightGet(s->x, s->z);

    /* Only objects where an item can lie: floor to 1.6 m up (Y grows down).
     * A lamp or a high shelf nearby is not the item. */
    for (i = 0; i < s_objCount; i++)
    {
        s64 dx = s_objPos[i].vx - s->x, dz = s_objPos[i].vz - s->z;
        s64 d  = dx * dx + dz * dz;
        if (s_objPos[i].vy > floorY + Q12(0.1f) || s_objPos[i].vy < floorY - Q12(1.3f)) continue;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best >= 0)
    {
        *out = s_objPos[best];
        out->vy -= Q12(0.1f);   /* just above the object's origin */
        return;
    }
    out->vx = s->x;
    out->vz = s->z;
    out->vy = floorY - Q12(0.8f);   /* table, desk and shelf height */
}

/* Projection in plain arithmetic from the camera, the way the GTE does it
 * (screen = H * view / depth), with the game's own H. (A first version used
 * RotTransPers and unpacked its screen XY with shifts, which swaps x and y on
 * big-endian -- the ports store it in memory order; see pc_decals.c. This
 * version needs no GTE state at all.) Screen units are the game's 2D
 * coordinates (centre 0,0); *otz is depth / 4 like RotTransPers. */
static int Glint_Project(const VECTOR3* p, s32 dyUp, s16* sx, s16* sy, s32* otz)
{
    VECTOR3 cam, watch;
    float   fx, fy, fz, len, rx, rz, ux, uy, uz, dx, dy, dz, vx, vy, vz, h;

    vcGetNowCamPos(&cam);
    vcGetNowWatchPos(&watch);
    fx = (float)(watch.vx - cam.vx); fy = (float)(watch.vy - cam.vy); fz = (float)(watch.vz - cam.vz);
    len = sqrtf(fx * fx + fy * fy + fz * fz);
    if (len < 1.0f) return 0;
    fx /= len; fy /= len; fz /= len;

    /* right = forward x down (Y grows down), up (screen) = right x forward */
    rx = -fz; rz = fx;
    len = sqrtf(rx * rx + rz * rz);
    if (len < 1e-4f) return 0;
    rx /= len; rz /= len;
    ux = -rz * fy;           /* (rx,0,rz) x (fx,fy,fz) */
    uy = rz * fx - rx * fz;
    uz = rx * fy;

    dx = (float)(p->vx - cam.vx);
    dy = (float)(p->vy - dyUp - cam.vy);
    dz = (float)(p->vz - cam.vz);
    vz = dx * fx + dy * fy + dz * fz;          /* depth */
    vx = dx * rx + dz * rz;
    vy = dx * ux + dy * uy + dz * uz;
    if (vz < 4096.0f * 0.3f) return 0;         /* behind or at the near plane */

    h   = (float)ReadGeomScreen();
    /* Signs checked against Harry on screen (feet below head, right of the
     * shelf he faces): both axes come out mirrored from the basis above. */
    *sx = (s16)(-h * vx / vz);
    *sy = (s16)(h * vy / vz);
    *otz = (s32)(vz / 16.0f / 4.0f);           /* Q12 -> Q8, then SZ / 4 */
    return ABS(*sx) < 2048 && ABS(*sy) < 2048;
}

static int Glint_WallBetween(const VECTOR3* item)
{
    VECTOR3    cam;
    s_RayTrace tr;
    s64        dx, dy, dz;
    q19_12     dist;

    vcGetNowCamPos(&cam);
    dx   = item->vx - cam.vx;
    dy   = item->vy - cam.vy;
    dz   = item->vz - cam.vz;
    dist = Glint_Isqrt(dx * dx + dy * dy + dz * dz);
    memset(&tr, 0, sizeof(tr));
    if (!Ray_TraceQuery(&tr, &cam, item) || !tr.hasHit) return 0;
    /* A hit within 0.5 m of the item is what it rests on. */
    return tr.hitDistance < dist - Q12(0.5f);
}

static void Glint_Sighting(s32 bit, s32 item)
{
    if (BIT_GET(s_seenVisit, bit)) return;
    BIT_SET(s_seenVisit, bit);

    if (!BIT_GET(s_seenEarlier, bit))
    {
        BIT_SET(s_seenEarlier, bit);
        s_firstVisit[bit] = s_visit;
        SH_DBG("[GLINT] item %d seen (first sighting)", (int)item);
    }
    else if (s_firstVisit[bit] != s_visit)
    {
        if (!BIT_GET(s_shining, bit)) SH_DBG("[GLINT] item %d seen again on a later visit: shines", (int)item);
        BIT_SET(s_shining, bit);
    }
}

/* ---- the glint -------------------------------------------------------------------- */

#define GLOW_SEGS 12

static void Glint_Draw(GsOT* ot, s16 cx, s16 cy, s32 otz, s32 radius)
{
    POLY_G3*  p = (POLY_G3*)GsOUT_PACKET_P;
    DR_TPAGE* dt;
    s32       pulse, i, bucket, sz, r, spike;
    u8        cr, cg, cb;

    /* Two-second pulse: 0.45..1.0 in Q12. */
    pulse = Q12(0.725f) + Q12_MULT(Q12(0.275f), Math_Sin((q19_12)((s_frame * 4096 / 60) & 0xFFF)));
    r     = (radius * pulse) >> 12;
    if (r < 3) r = 3;
    spike = r * 5 / 2;
    cr    = (u8)((200 * pulse) >> 12);
    cg    = (u8)((185 * pulse) >> 12);
    cb    = (u8)((140 * pulse) >> 12);

    bucket = (otz >> 3) - 2;
    if (bucket < 0) bucket = 0;
    if (bucket >= ORDERING_TABLE_SIZE) bucket = ORDERING_TABLE_SIZE - 1;
    sz = (otz << 2) - 160;
    if (sz < 1) sz = 1;

    /* Soft glow: bright centre, black rim (additive, so black adds nothing). */
    for (i = 0; i < GLOW_SEGS; i++)
    {
        q19_12 a0 = (q19_12)((4096 * i) / GLOW_SEGS), a1 = (q19_12)((4096 * (i + 1)) / GLOW_SEGS);
        setPolyG3(p);
        setSemiTrans(p, 1);
        setRGB0(p, cr, cg, cb);
        setRGB1(p, 0, 0, 0);
        setRGB2(p, 0, 0, 0);
        setXY3(p, cx, cy,
               cx + ((r * Math_Cos(a0)) >> 12), cy + ((r * Math_Sin(a0)) >> 12),
               cx + ((r * Math_Cos(a1)) >> 12), cy + ((r * Math_Sin(a1)) >> 12));
        PsyX_SetNextPrimSzExact((u16)sz, (u16)sz, (u16)sz, (u16)sz);
        addPrim(&ot->org[bucket], p);
        p++;
    }

    /* Four thin spikes. */
    for (i = 0; i < 4; i++)
    {
        s32 dx = (i == 0) ? spike : (i == 1) ? -spike : 0;
        s32 dy = (i == 2) ? spike : (i == 3) ? -spike : 0;
        s32 wx = (dy != 0) ? 1 : 0, wy = (dx != 0) ? 1 : 0;
        s32 k;
        for (k = 0; k < 2; k++)
        {
            s32 sgn = k ? -1 : 1;
            setPolyG3(p);
            setSemiTrans(p, 1);
            setRGB0(p, cr, cg, cb);
            setRGB1(p, 0, 0, 0);
            setRGB2(p, 0, 0, 0);
            setXY3(p, cx, cy, cx + dx, cy + dy, cx + sgn * wx * 2, cy + sgn * wy * 2);
            PsyX_SetNextPrimSzExact((u16)sz, (u16)sz, (u16)sz, (u16)sz);
            addPrim(&ot->org[bucket], p);
            p++;
        }
    }

    /* addPrim prepends: the draw mode goes in last so it runs first. abr 1 = add. */
    dt = (DR_TPAGE*)p;
    setDrawTPage(dt, 0, 1, getTPage(0, 1, 0, 0));
    addPrim(&ot->org[bucket], dt);
    GsOUT_PACKET_P = (PACKET*)(dt + 1);
}

/* ---- per frame, from the world draw ------------------------------------------------- */

void Pc_KeyGlint_Draw(GsOT* ot)
{
    s32 i, roomKey, mapIdx;

    s_frame++;

    if (!g_PcConfig.modernKeyGlint || !Pc_Modern_IsActive() || g_SavegamePtr == NULL ||
        g_GameWork.gameState != GameState_InGame || (g_SysWork.sysFlags & SysFlag_DemoActive))
    {
        s_objCount = 0;
        return;
    }

    if (!s_spotMapReady)
    {
        for (i = 0; i < KEY_ITEM_SPOTS; i++) s_spotMap[i] = (s16)MapRegistry_FindByName(KEY_ITEMS[i].map);
        s_spotMapReady = 1;
    }

    mapIdx  = g_SavegamePtr->mapIdx;
    roomKey = mapIdx * 256 + (u8)g_SavegamePtr->mapRoomIdx;
    if (roomKey != s_roomKey)
    {
        s_roomKey = roomKey;
        s_visit++;
        memset(s_seenVisit, 0, sizeof(s_seenVisit));
    }

    for (i = 0; i < KEY_ITEM_SPOTS; i++)
    {
        const s_KeyItemSpot* s = &KEY_ITEMS[i];
        s32     bit = s->item - ITEM_BASE;
        VECTOR3 pos;
        s16     sx, sy, sx2, sy2;
        s32     otz, otz2;
        s64     dx, dz;

        if (s_spotMap[i] != mapIdx || bit < 0 || bit >= 64) continue;
        /* Some items have two pickup points side by side: one glint. */
        if (i > 0 && KEY_ITEMS[i - 1].flag == s->flag && s_spotMap[i - 1] == s_spotMap[i]) continue;
        if (Savegame_EventFlagGet(s->flag) || Glint_InInventory(s->item)) continue;
        if (s->required != 0 && !Savegame_EventFlagGet(s->required)) continue;

        dx = s->x - g_SysWork.playerWork.player.position.vx;
        dz = s->z - g_SysWork.playerWork.player.position.vz;
        if (dx * dx + dz * dz > (s64)SIGHT_RANGE * SIGHT_RANGE) continue;

        Glint_ItemPos(s, &pos);
        if (!Glint_Project(&pos, 0, &sx, &sy, &otz)) continue;
        if (ABS(sx) > 230 || ABS(sy) > 135) continue;   /* off screen (16:9 Hor+ width) */

        if (g_SysWork.sysState == SysState_Gameplay && !BIT_GET(s_seenVisit, bit) &&
            ((s_frame + i) & 7) == 0 && !Glint_WallBetween(&pos))
        {
            Glint_Sighting(bit, s->item);
        }

        if (BIT_GET(s_shining, bit) && Glint_Project(&pos, Q12(0.12f), &sx2, &sy2, &otz2))
        {
            s32 radius = ABS(sy2 - sy) * 3;
            static u8 s_logged[8];
            if (!BIT_GET(s_logged, bit)) { BIT_SET(s_logged, bit); SH_DBG("[GLINT] item %d shining at screen (%d,%d) r=%d", (int)s->item, sx, sy, (int)radius); }
            if (radius > 40) radius = 40;
            Glint_Draw(ot, sx, sy, otz, radius);
        }
    }
    s_objCount = 0;
}
