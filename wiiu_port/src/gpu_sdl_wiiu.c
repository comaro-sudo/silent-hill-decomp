/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_sdl_wiiu.c - Wii U backend behind the GpuNv2a_* interface
 * (xbox_port/include/gpu_nv2a.h), drawn through SDL2's GX2 renderer with
 * SDL_RenderGeometry.
 *
 * GX2 proper needs precompiled shaders; SDL's renderer already has them, and
 * SDL_RenderGeometry draws exactly what the shared PSX decoder produces:
 * screen-space triangles with a colour and texcoords per vertex (texel * colour,
 * as the D3D/GX backends do in stage 0). What it cannot do is mapped like this:
 *
 *   alpha test (kill 0x0000 texels)  -> every page is uploaded in two forms:
 *       "stp"    alpha 0 / 0x80 / 0xFF as decoded (ABR0 averaging), and
 *       "solid"  alpha 0 / 0xFF (any non-zero alpha -> opaque),
 *     and every mode draws BLENDed with src alpha, so alpha 0 texels add
 *     nothing. Opaque prims use "solid" with SRC_ALPHA/INV_SRC_ALPHA.
 *   ABR1 B+F      SRC_ALPHA, ONE, ADD            ("solid")
 *   ABR2 B-F      SRC_ALPHA, ONE, REV_SUBTRACT   ("solid")
 *   ABR3 B+F/4    SRC_ALPHA, ONE, ADD, vertex alpha 1/4 ("solid")
 *   fog term (spec, out = tex*col + spec) -> a second, untextured additive pass
 *     of the same triangles with colour = spec, for opaque prims only (the
 *     world geometry the fog is for).
 *
 * No W: SDL_Vertex is 2D, so texturing is affine -- the PSX look, and the
 * build has PGXP off (USE_PGXP=0) anyway.
 *
 * Output: 1920x1080 TV. Hor+ in game (16:9 of content), 4:3 screens at
 * 1440x1080 centred with black bars, never stretched.
 */
#include <string.h>
#include <stdlib.h>
#include <SDL.h>

#include "gpu_nv2a.h"
#include "sh_log.h"

int                g_Nv2aFrameCount;
int                g_Nv2aContentX = 0;
int                g_Nv2aContentW = 640;
int                g_Nv2aContentH = 480;
int                g_Nv2aViewW    = 640;
unsigned long long g_Nv2aDrawCycles;
extern int g_PcWidescreenMode;
extern int g_PcHorPlusEnabled;
extern unsigned PsxVram_DataGen(const void* data);
extern unsigned PsxVram_DecodeEpoch(void);
extern void Pgxp_FrameAdvance(void);

#define TV_W 1920
#define TV_H 1080
#define DRC_W 854
#define DRC_H 480

/* The TV and the GamePad are separate SDL windows (SDL_WINDOW_WIIU_TV_ONLY /
 * _GAMEPAD_ONLY), each with its own renderer. s_ren is whichever one the game
 * is drawn on: the TV normally, the GamePad in Off-TV play. Textures belong to
 * a renderer, so switching drops the page cache (it refills in a frame). */
SDL_Window*          g_WiiuWindow;
static SDL_Window*   s_drcWin;
static SDL_Renderer* s_tvRen;
static SDL_Renderer* s_drcRen;
static SDL_Renderer* s_ren;
static int           s_outW = TV_W, s_outH = TV_H;
static int           s_offTv;

extern void GamepadScreen_Init(SDL_Renderer* drc);
extern void GamepadScreen_Draw(SDL_Renderer* drc);
extern void GamepadScreen_DrawOverlay(SDL_Renderer* drc);
extern int  GamepadScreen_TakeOffTvToggle(void);
extern int  GamepadScreen_WantMapCapture(void);
extern void GamepadScreen_CaptureMap(SDL_Renderer* tv, const SDL_Rect* src);
static int           s_ready;

static int   s_vpX, s_vpW;
static float s_viewW = 640.0f, s_viewX = 0.0f, s_ppuX = 1.0f, s_ppuY = 1.0f;
static int   s_horPlus = -1;

#define GPU_POOL_VERTS 32768
static ShVertex   s_pool[GPU_POOL_VERTS];
static int        s_poolUsed, s_flushStart;
#define OUT_MAX (GPU_POOL_VERTS * 4)   /* room for PGXP subdivision */
static SDL_Vertex s_sv[OUT_MAX];
static SDL_Vertex s_fv[OUT_MAX];   /* fog-pass copy of s_sv */
static int        s_triOut[GPU_POOL_VERTS / 3 + 2];   /* input triangle -> first output vertex */

#define PAGE 256
/* ---- texture atlases -------------------------------------------------------
 * Every 256x256 page lives in a slot of a 4096x4096 atlas, so binding another
 * page only changes the UVs of the vertices that follow: it no longer ends the
 * batch. A batch ends on a blend-mode, scissor or oversize-texture change.
 * That took the frame from ~2,500 SDL_RenderGeometry calls to a few hundred
 * (goal: under 1,000 a frame on a real Wii U).
 *
 * Three atlases, one per alpha variant, same slot layout in each:
 *   STP   alpha as decoded (0 / 0x80 STP / 0xFF), for ABR0 averaging
 *   SOLID any visible texel opaque, for every other mode
 *   MASK  white with the texel's visibility, for the additive fog pass
 * Slot 0 is solid white in all three: untextured prims draw from it too, so
 * they don't break batches either. */
#define ATLAS     4096
#define ACOLS     (ATLAS / PAGE)          /* 16 */
#define SLOTS     (ACOLS * ACOLS)         /* 256, slot 0 = white */
enum { V_STP, V_SOLID, V_MASK, V_N };
typedef struct {
    const void* page; const void* pal;
    int w, h;
    unsigned pageGen, palGen, epoch, lastUse;
    unsigned char ok[V_N];                /* variant uploaded and current */
} Slot;
static Slot         s_slot[SLOTS];
static SDL_Texture* s_atlas[V_N];
static unsigned     s_conv[PAGE * PAGE];
static SDL_Texture* s_big[2];
static int          s_bigW[2], s_bigH[2], s_bigNext;

/* current binding: an atlas slot, or an oversize texture (FMV frames) */
static int          s_curSlot;            /* 0 = white */
static float        s_curScale;           /* tex -> texels: 256 paletted, 1 direct, 0 white */
static SDL_Texture* s_bigTex;             /* non-NULL = oversize texture bound */
static float        s_bigUS, s_bigVS;
static const void*  s_texAddr; static const void* s_texPal; static int s_texW, s_texH;
static int          s_blendMode;
static int          s_scFull = 1, s_scX, s_scY, s_scW, s_scH;
static SDL_BlendMode s_bmOpaque, s_bmHalf, s_bmAdd, s_bmSub;
static unsigned     s_frame, s_draws, s_verts, s_uploads, s_maxDraws;

/* UV segments of the pending batch: vertices from `start` on use `slot` */
typedef struct { int start, slot; float scale; } Seg;
#define SEG_MAX 8192
static Seg s_seg[SEG_MAX];
static int s_segN;

static void FlushBatch(void);
static void SegReset(void);
static int  AtlasesCreate(void);


static void SelectView(void)
{
    const int want = (g_PcWidescreenMode == 1 && g_PcHorPlusEnabled) ? 1 : 0;
    if (want == s_horPlus) return;
    s_horPlus = want;
    if (want) {
        s_vpX = 0; s_vpW = s_outW;
        s_viewW = (float)g_Nv2aContentH * 16.0f / 9.0f;
        s_viewX = (s_viewW - (float)g_Nv2aContentW) * 0.5f;
    } else {
        s_vpW = s_outH * 4 / 3; s_vpX = (s_outW - s_vpW) / 2;
        s_viewW = (float)g_Nv2aContentW; s_viewX = 0.0f;
    }
    s_ppuX = (float)s_vpW / s_viewW;
    s_ppuY = (float)s_outH / (float)g_Nv2aContentH;
    g_Nv2aViewW = (int)(s_viewW + 0.5f);
    SH_DBG("[GPU] view: %s (%d px at %d, content %.1f wide)",
           want ? "Hor+ 16:9" : "4:3 pillarbox", s_vpW, s_vpX, s_viewW);
}

void GpuNv2a_Init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SH_DBG("[GPU] SDL video init failed: %s", SDL_GetError());
        return;
    }
    g_WiiuWindow = SDL_CreateWindow("Silent Hill", 0, 0, TV_W, TV_H, SDL_WINDOW_WIIU_TV_ONLY);
    s_tvRen = g_WiiuWindow ? SDL_CreateRenderer(g_WiiuWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!s_tvRen) {
        SH_DBG("[GPU] SDL renderer failed: %s", SDL_GetError());
        return;
    }
    /* GamePad: no vsync of its own -- the main renderer paces the frame, and
     * two vsynced presents per frame would halve the frame rate. */
    s_drcWin = SDL_CreateWindow("Silent Hill (GamePad)", 0, 0, DRC_W, DRC_H,
                                SDL_WINDOW_WIIU_GAMEPAD_ONLY | SDL_WINDOW_WIIU_PREVENT_SWAP);
    s_drcRen = s_drcWin ? SDL_CreateRenderer(s_drcWin, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (s_drcRen)
        GamepadScreen_Init(s_drcRen);
    else
        SH_DBG("[GPU] GamePad window failed: %s - TV only", SDL_GetError());
    s_ren = s_tvRen;
    s_bmOpaque = SDL_BLENDMODE_BLEND;
    s_bmHalf   = SDL_BLENDMODE_BLEND;
    s_bmAdd    = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
                                            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    s_bmSub    = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT,
                                            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
    SelectView();
    s_ready = 1;
    if (!AtlasesCreate())
        SH_DBG("[GPU] atlas creation failed - textures will be missing");
    {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(s_ren, &ri);
        SH_DBG("[GPU] SDL renderer '%s' up: %dx%d, %d-vertex pool, %d atlas slots x %d variants",
               ri.name, TV_W, TV_H, GPU_POOL_VERTS, SLOTS, V_N);
    }
}

/* ---- textures ------------------------------------------------------------- */

static int CurVariant(void) { return s_blendMode == 1 ? V_STP : V_SOLID; }

static SDL_Texture* NewTex(int w, int h)
{
    SDL_Texture* t = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    } else {
        SH_DBG("[GPU] CreateTexture %dx%d failed: %s", w, h, SDL_GetError());
    }
    return t;
}

static void AtlasesDestroy(void)
{
    int i;
    for (i = 0; i < V_N; i++) { if (s_atlas[i]) SDL_DestroyTexture(s_atlas[i]); s_atlas[i] = NULL; }
    for (i = 0; i < 2; i++) { if (s_big[i]) SDL_DestroyTexture(s_big[i]); s_big[i] = NULL; s_bigW[i] = s_bigH[i] = 0; }
    memset(s_slot, 0, sizeof(s_slot));
    s_bigTex = NULL;
}

static int AtlasesCreate(void)
{
    int v, i;
    for (v = 0; v < V_N; v++) {
        if (s_atlas[v]) continue;
        s_atlas[v] = NewTex(ATLAS, ATLAS);
        if (!s_atlas[v]) return 0;
        /* slot 0: white, opaque in every variant */
        for (i = 0; i < PAGE * PAGE; i++) s_conv[i] = 0xFFFFFFFFu;
        {
            SDL_Rect r = { 0, 0, PAGE, PAGE };
            SDL_UpdateTexture(s_atlas[v], &r, s_conv, PAGE * 4);
        }
    }
    s_slot[0].ok[V_STP] = s_slot[0].ok[V_SOLID] = s_slot[0].ok[V_MASK] = 1;
    return 1;
}

/* Host-order ARGB8888 is SDL_PIXELFORMAT_ARGB8888 on any endianness. */
static void Convert(const unsigned* src, const unsigned* pal, int w, int h, int variant)
{
    const unsigned char* idx = (const unsigned char*)src;
    int i, n = w * h;
    for (i = 0; i < n; i++) {
        unsigned c = pal ? pal[idx[i]] : src[i];
        if (variant == V_MASK)       c = (c >> 24) ? 0xFFFFFFFFu : 0x00FFFFFFu;
        else if (variant == V_SOLID && (c >> 24)) c |= 0xFF000000u;
        s_conv[i] = c;
    }
}

static void SlotUpload(int k, int variant)
{
    Slot* e = &s_slot[k];
    SDL_Rect r;
    r.x = (k % ACOLS) * PAGE; r.y = (k / ACOLS) * PAGE; r.w = e->w; r.h = e->h;
    Convert((const unsigned*)e->page, (const unsigned*)e->pal, e->w, e->h, variant);
    SDL_UpdateTexture(s_atlas[variant], &r, s_conv, e->w * 4);
    e->ok[variant] = 1;
    s_uploads++;
}

/* Slot of a page (+palette), its `variant` (and, for opaque, the fog mask)
 * uploaded and current. Same validity rule as the other backends: re-verify by
 * PsxVram_DataGen whenever the decode epoch moves. */
static int SlotFor(const void* page, const void* pal, int w, int h, int variant)
{
    const unsigned ep  = PsxVram_DecodeEpoch();
    const unsigned now = (unsigned)g_Nv2aFrameCount;
    int i, k = -1;
    for (i = 1; i < SLOTS; i++)
        if (s_slot[i].page == page && s_slot[i].pal == pal) { k = i; break; }
    if (k > 0 && s_slot[k].epoch != ep) {
        const unsigned pg = PsxVram_DataGen(page), pl = pal ? PsxVram_DataGen(pal) : 0;
        if (!(pg && (!pal || pl) && s_slot[k].pageGen == pg && s_slot[k].palGen == pl)) {
            memset(s_slot[k].ok, 0, sizeof(s_slot[k].ok));     /* contents changed */
            s_slot[k].pageGen = pg; s_slot[k].palGen = pl;
        }
        s_slot[k].epoch = (pg && (!pal || pl)) ? ep : ep - 1u;
    }
    if (k < 0) {
        int best = -1;
        for (i = 1; i < SLOTS; i++) {
            if (!s_slot[i].page) { best = i; break; }
            if (best < 0 || s_slot[i].lastUse < s_slot[best].lastUse) best = i;
        }
        k = best;
        /* a slot drawn from this frame: let its queued draws go first */
        if (s_slot[k].page && s_slot[k].lastUse == now) FlushBatch();
        memset(&s_slot[k], 0, sizeof(s_slot[k]));
        s_slot[k].page = page; s_slot[k].pal = pal; s_slot[k].w = w; s_slot[k].h = h;
        s_slot[k].pageGen = PsxVram_DataGen(page);
        s_slot[k].palGen  = pal ? PsxVram_DataGen(pal) : 0;
        s_slot[k].epoch   = (s_slot[k].pageGen && (!pal || s_slot[k].palGen)) ? ep : ep - 1u;
    }
    if (!s_slot[k].ok[variant]) SlotUpload(k, variant);
    if (variant == V_SOLID && !s_slot[k].ok[V_MASK]) SlotUpload(k, V_MASK);
    s_slot[k].lastUse = now;
    return k;
}

/* Start a new UV segment at the next vertex (replacing an empty one). */
static void SegMark(int slot, float scale)
{
    Seg* last = s_segN ? &s_seg[s_segN - 1] : NULL;
    if (last && last->slot == slot && last->scale == scale) return;
    if (last && last->start == s_poolUsed) { last->slot = slot; last->scale = scale; return; }
    if (s_segN >= SEG_MAX) FlushBatch();     /* resets the list */
    s_seg[s_segN].start = s_poolUsed; s_seg[s_segN].slot = slot; s_seg[s_segN].scale = scale;
    s_segN++;
}

static void UseSlot(int slot, float scale)
{
    if (s_bigTex) { FlushBatch(); s_bigTex = NULL; }
    s_curSlot = slot; s_curScale = scale;
    SegMark(slot, scale);
}

static void BindBig(const void* addr, int w, int h, int variant)
{
    int i = s_bigNext++ & 1;
    unsigned* buf;
    FlushBatch();
    if (!s_big[i] || s_bigW[i] != w || s_bigH[i] != h) {
        if (s_big[i]) SDL_DestroyTexture(s_big[i]);
        s_big[i] = NewTex(w, h);
        s_bigW[i] = w; s_bigH[i] = h;
        if (!s_big[i]) { s_bigTex = NULL; UseSlot(0, 0.0f); return; }
    }
    buf = (unsigned*)malloc((size_t)w * h * 4);
    if (buf) {
        int k, n = w * h;
        for (k = 0; k < n; k++) {
            unsigned c = ((const unsigned*)addr)[k];
            buf[k] = (variant == V_SOLID && (c >> 24)) ? (c | 0xFF000000u) : c;
        }
        SDL_UpdateTexture(s_big[i], NULL, buf, w * 4);
        free(buf);
    }
    s_uploads++;
    s_bigTex = s_big[i]; s_bigUS = 1.0f / (float)w; s_bigVS = 1.0f / (float)h;
}

static void Rebind(void)
{
    const int v = CurVariant();
    if (!s_ready || !AtlasesCreate()) return;
    if (!s_texAddr) { UseSlot(0, 0.0f); return; }
    if (s_texPal)
        UseSlot(SlotFor(s_texAddr, s_texPal, PAGE, PAGE, v), (float)PAGE);   /* UVs arrive /256 */
    else if (s_texW > PAGE || s_texH > PAGE)
        BindBig(s_texAddr, s_texW, s_texH, v);
    else
        UseSlot(SlotFor(s_texAddr, NULL, s_texW, s_texH, v), 1.0f);          /* texel UVs */
}

void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    if (!page || !palette) { GpuNv2a_BindWhite(); return; }
    if (page == s_texAddr && palette == s_texPal && !s_bigTex) return;
    s_texAddr = page; s_texPal = palette; s_texW = s_texH = PAGE;
    Rebind();
}

void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    if (addr && addr == s_texAddr && !s_texPal && w == s_texW && h == s_texH) return;
    s_texAddr = (addr && w > 0 && h > 0) ? addr : NULL; s_texPal = NULL; s_texW = w; s_texH = h;
    Rebind();
}

void GpuNv2a_BindWhite(void)
{
    s_texAddr = NULL; s_texPal = NULL;
    if (s_ready) UseSlot(0, 0.0f);
}

void* GpuNv2a_AllocTexMem(int bytes)
{
    return bytes > 0 ? malloc((size_t)bytes) : NULL;
}

/* ---- state ---------------------------------------------------------------- */

void GpuNv2a_SetBlendMode(int mode)
{
    const int was = CurVariant();
    if (mode == s_blendMode) return;
    FlushBatch();
    s_blendMode = mode;
    /* the batch now draws from another atlas: make sure the page is in it */
    if (s_texAddr && CurVariant() != was) Rebind();
}

static void ApplyClip(void)
{
    SDL_Rect r;
    if (s_scFull) {
        r.x = s_vpX; r.y = 0; r.w = s_vpW; r.h = s_outH;
    } else {
        r.x = s_vpX + (int)((s_viewX + (float)s_scX) * s_ppuX);
        r.y = (int)((float)s_scY * s_ppuY);
        r.w = (int)((float)s_scW * s_ppuX);
        r.h = (int)((float)s_scH * s_ppuY);
    }
    SDL_RenderSetClipRect(s_ren, &r);
}

void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    const int full = (w <= 0 || h <= 0);
    if (full) { x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH; }
    if (full == s_scFull && x == s_scX && y == s_scY && w == s_scW && h == s_scH) return;
    FlushBatch();
    s_scX = x; s_scY = y; s_scW = w; s_scH = h; s_scFull = full;
    if (s_ready) ApplyClip();
}

void GpuNv2a_SetDepthWrite(int enable) { (void)enable; }
void GpuNv2a_SetDepthTest(int enable)  { (void)enable; }
void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* ---- drawing -------------------------------------------------------------- */

static inline Uint8 U8(float f) { return f <= 0.0f ? 0 : (f >= 1.0f ? 255 : (Uint8)(f * 255.0f + 0.5f)); }

static void SegReset(void)
{
    s_segN = 1;
    s_seg[0].start = s_poolUsed; s_seg[0].slot = s_curSlot; s_seg[0].scale = s_curScale;
}

/* PGXP on SDL: SDL_Vertex is 2D, so the GPU interpolates UVs affinely. Where
 * PGXP supplied a real W (pos[3]) that varies across a textured triangle, the
 * triangle is split at its edge midpoints (1 or 2 levels, by screen size) and
 * the new corners get PERSPECTIVE-correct UVs (u/w and 1/w interpolated), so
 * the warping shrinks to that of a quarter/sixteenth-size triangle. Colour and
 * fog stay screen-linear, as the PSX did. Costs vertices, not draw calls. */
typedef struct { float x, y, w, u, v, c[4], s[3]; } TV;

static int   s_on;                 /* output vertices this flush */
static int   s_outSlot;            /* atlas slot of the triangle being emitted */
static int   s_outWhite;
static int   s_tessTris;           /* stats: triangles subdivided this frame */

static void EmitTV(const TV* t)
{
    SDL_Vertex* o = &s_sv[s_on];
    SDL_Vertex* f = &s_fv[s_on];
    float u = t->u, v = t->v;
    o->position.x = (float)s_vpX + (t->x + s_viewX) * s_ppuX;
    o->position.y = t->y * s_ppuY;
    o->color.r = U8(t->c[0]); o->color.g = U8(t->c[1]); o->color.b = U8(t->c[2]);
    o->color.a = (s_blendMode == 4) ? 64 : U8(t->c[3]);
    if (s_bigTex) {
        o->tex_coord.x = u * s_bigUS;
        o->tex_coord.y = v * s_bigVS;
    } else {
        if (s_outWhite) { u = v = 128.0f; }
        else {
            if (u < 0.0f) u = 0.0f; else if (u > 255.99f) u = 255.99f;
            if (v < 0.0f) v = 0.0f; else if (v > 255.99f) v = 255.99f;
        }
        o->tex_coord.x = ((float)((s_outSlot % ACOLS) * PAGE) + u) * (1.0f / ATLAS);
        o->tex_coord.y = ((float)((s_outSlot / ACOLS) * PAGE) + v) * (1.0f / ATLAS);
    }
    *f = *o;
    f->color.r = U8(t->s[0]); f->color.g = U8(t->s[1]); f->color.b = U8(t->s[2]); f->color.a = 255;
    s_on++;
}

static TV Mid(const TV* p, const TV* q)
{
    TV m;
    int i;
    const float ip = 1.0f / p->w, iq = 1.0f / q->w, im = 0.5f * (ip + iq);
    m.x = 0.5f * (p->x + q->x);
    m.y = 0.5f * (p->y + q->y);
    m.w = 1.0f / im;
    m.u = 0.5f * (p->u * ip + q->u * iq) / im;
    m.v = 0.5f * (p->v * ip + q->v * iq) / im;
    for (i = 0; i < 4; i++) m.c[i] = 0.5f * (p->c[i] + q->c[i]);
    for (i = 0; i < 3; i++) m.s[i] = 0.5f * (p->s[i] + q->s[i]);
    return m;
}

static void Tess(const TV* a, const TV* b, const TV* c, int depth)
{
    TV ab, bc, ca;
    if (depth <= 0) { EmitTV(a); EmitTV(b); EmitTV(c); return; }
    ab = Mid(a, b); bc = Mid(b, c); ca = Mid(c, a);
    Tess(a, &ab, &ca, depth - 1);
    Tess(&ab, b, &bc, depth - 1);
    Tess(&ca, &bc, c, depth - 1);
    Tess(&ab, &bc, &ca, depth - 1);
}

static int TessDepth(const TV* t)
{
    float wmin = t[0].w, wmax = t[0].w, l = 0.0f, d;
    int i;
    if (s_bigTex || s_outWhite) return 0;
    for (i = 1; i < 3; i++) {
        if (t[i].w < wmin) wmin = t[i].w;
        if (t[i].w > wmax) wmax = t[i].w;
    }
    if (wmin <= 0.0f || wmax < wmin * 1.15f) return 0;      /* no PGXP W, or near-flat */
    for (i = 0; i < 3; i++) {
        const TV* p = &t[i]; const TV* q = &t[(i + 1) % 3];
        d = (p->x - q->x) * (p->x - q->x) + (p->y - q->y) * (p->y - q->y);
        if (d > l) l = d;
    }
    /* Longest edge in content units (the PSX's 640x480 space). One level
     * only, and only big triangles: two levels on every 24-unit triangle cost
     * ~3,000 split triangles a frame and took Cemu from ~19 to ~7 fps. */
    if (l > 64.0f * 64.0f) return 1;
    return 0;
}

static void FlushBatch(void)
{
    const int n = s_poolUsed - s_flushStart;
    const int cnt = n - n % 3;
    int i, k, sg, anySpec = 0;
    SDL_BlendMode bm;
    SDL_Texture* tex;
    if (cnt < 3 || !s_ready) { s_flushStart = s_poolUsed; SegReset(); return; }

    switch (s_blendMode) {
    case 1:  bm = s_bmHalf; break;
    case 2:
    case 4:  bm = s_bmAdd;  break;
    case 3:  bm = s_bmSub;  break;
    default: bm = s_bmOpaque; break;
    }
    tex = s_bigTex ? s_bigTex : s_atlas[CurVariant()];

    s_on = 0;
    sg = 0;
    for (i = 0; i < cnt; i += 3) {
        const int vi = s_flushStart + i;
        TV t[3];
        int j, depth;
        while (sg + 1 < s_segN && s_seg[sg + 1].start <= vi) sg++;
        s_outSlot  = s_seg[sg].slot;
        s_outWhite = (s_seg[sg].scale == 0.0f);
        for (j = 0; j < 3; j++) {
            const ShVertex* v = &s_pool[vi + j];
            const float sc = s_bigTex ? 1.0f : s_seg[sg].scale;
            t[j].x = v->pos[0]; t[j].y = v->pos[1];
            t[j].w = v->pos[3] > 0.0f ? v->pos[3] : 1.0f;
            t[j].u = v->tex[0] * sc; t[j].v = v->tex[1] * sc;
            t[j].c[0] = v->col[0]; t[j].c[1] = v->col[1]; t[j].c[2] = v->col[2]; t[j].c[3] = v->col[3];
            t[j].s[0] = v->spec[0]; t[j].s[1] = v->spec[1]; t[j].s[2] = v->spec[2];
            if (v->spec[0] > 0.0f || v->spec[1] > 0.0f || v->spec[2] > 0.0f) anySpec = 1;
        }
        s_triOut[i / 3] = s_on;
        depth = TessDepth(t);
        /* vertex budget: fall back to the plain triangle when full */
        while (depth > 0 && s_on + 3 * (1 << (2 * depth)) > OUT_MAX) depth--;
        if (s_on + 3 > OUT_MAX) continue;            /* full: skip, keep the index map whole */
        if (depth) s_tessTris++;
        Tess(&t[0], &t[1], &t[2], depth);
    }
    s_triOut[cnt / 3] = s_on;

    if (!tex || s_on < 3) {
        /* nothing to draw with */
    } else if (!(anySpec && s_blendMode == 0 && !s_bigTex && s_atlas[V_MASK])) {
        /* no fog: the whole batch is one draw */
        SDL_SetTextureBlendMode(tex, bm);
        SDL_RenderGeometry(s_ren, tex, s_sv, s_on, NULL, 0);
        s_draws++;
    } else {
        /* Fog (+spec, additive) can't share the colour pass in SDL, and the
         * PSX draws back to front: colour-all-then-fog-all added the fog of
         * hidden far triangles over the near ones (washed-out white objects,
         * doubled edges). So fogged batches go colour, fog per same-page
         * segment -- the order the per-texture batches of the old renderer
         * had -- while the atlas still saves every variant/texture switch.
         * The fog pass reads the MASK atlas (white, the texel's visibility,
         * same UVs) so it lands only where the texture is visible. */
        SDL_SetTextureBlendMode(tex, bm);
        SDL_SetTextureBlendMode(s_atlas[V_MASK], s_bmAdd);
        for (k = 0; k < s_segN; k++) {
            int a = s_seg[k].start - s_flushStart;
            int b = (k + 1 < s_segN) ? s_seg[k + 1].start - s_flushStart : cnt;
            int oa, ob;
            if (a < 0) a = 0;
            if (b > cnt) b = cnt;
            a -= a % 3; b -= b % 3;
            if (b - a < 3) continue;
            oa = s_triOut[a / 3]; ob = s_triOut[b / 3];
            if (ob - oa < 3) continue;
            SDL_RenderGeometry(s_ren, tex, s_sv + oa, ob - oa, NULL, 0);
            SDL_RenderGeometry(s_ren, s_atlas[V_MASK], s_fv + oa, ob - oa, NULL, 0);
            s_draws += 2;
        }
    }
    s_flushStart = s_poolUsed;
    SegReset();
}

void GpuNv2a_FlushBatch(void) { FlushBatch(); }

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;
    if (count <= 0 || count > GPU_POOL_VERTS) return NULL;
    if (s_poolUsed + count > GPU_POOL_VERTS) { FlushBatch(); s_poolUsed = s_flushStart = 0; SegReset(); }
    p = &s_pool[s_poolUsed];
    s_poolUsed += count;
    s_verts += (unsigned)count;
    return p;
}

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    ShVertex* d;
    if (!verts || count <= 0) return;
    d = GpuNv2a_BatchAlloc(count);
    if (d) memcpy(d, verts, (size_t)count * sizeof(ShVertex));
}

static unsigned s_clearOverride; static int s_clearOverrideOn;
void GpuXenos_DebugClearOverride(unsigned argb, int enable) { s_clearOverride = argb; s_clearOverrideOn = enable; }
void GpuXenos_DebugFbProbe(const char* tag) { SH_DBG("[FBPROBE] %s: no readback on Wii U SDL", tag); }

void GpuNv2a_FrameBegin(void)
{
    unsigned c;
    SDL_Rect r;
    g_Nv2aFrameCount++;
    if (s_draws > s_maxDraws) s_maxDraws = s_draws;
    s_poolUsed = s_flushStart = 0;
    s_draws = s_verts = s_uploads = 0;
    s_tessTris = 0;
    SegReset();
    if (!s_ready) return;
    SelectView();
    SDL_RenderSetClipRect(s_ren, NULL);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    c = s_clearOverrideOn ? s_clearOverride : GpuXbox_GetClearColor();
    r.x = s_vpX; r.y = 0; r.w = s_vpW; r.h = s_outH;
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(s_ren, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    SDL_RenderFillRect(s_ren, &r);
    s_scFull = 1;
    ApplyClip();
}

/* Move the game between the TV and the GamePad (Off-TV play). */
static void SetOffTv(int offTv)
{
    int i;
    if (!s_drcRen || offTv == s_offTv) return;
    FlushBatch();
    /* every texture belongs to the old renderer: the atlases are rebuilt on
     * the new one at the next bind */
    AtlasesDestroy();
    s_texAddr = NULL; s_texPal = NULL; s_curSlot = 0; s_curScale = 0.0f;
    (void)i;
    s_ren = offTv ? s_drcRen : s_tvRen;  /* the atlases belong to the new renderer */
    AtlasesCreate();
    /* vsync stays on the TV renderer: it presents last every frame (FrameEnd) */
    s_offTv = offTv;
    s_ren   = offTv ? s_drcRen : s_tvRen;
    s_outW  = offTv ? DRC_W : TV_W;
    s_outH  = offTv ? DRC_H : TV_H;
    s_horPlus = -1;
    SelectView();
    SH_DBG("[GPU] output: %s", offTv ? "GamePad (Off-TV)" : "TV");
}

void GpuNv2a_FrameEnd(void)
{
    extern void WiiU_ProcPump(void);   /* main/HOME menu handling (pad_wiiu.c) */
    FlushBatch();
    if (s_ready) {
        if (!s_offTv && s_drcRen && GamepadScreen_WantMapCapture()) {
            /* the map screen is 2D, so it sits in the TV's 4:3 area */
            SDL_Rect src = { (TV_W - TV_H * 4 / 3) / 2, 0, TV_H * 4 / 3, TV_H };
            GamepadScreen_CaptureMap(s_tvRen, &src);
        }
        /* One scan-buffer swap per frame. Presenting either SDL window swaps
         * the TV AND GamePad buffers, so two swaps a frame left the TV showing
         * a stale/cleared buffer every other frame (black flicker in Cemu,
         * 2026-11-11). The GamePad window is created with PREVENT_SWAP and is
         * presented FIRST; the TV present then does the single swap for both. */
        if (s_drcRen) {
            if (s_offTv) GamepadScreen_DrawOverlay(s_drcRen);
            else         GamepadScreen_Draw(s_drcRen);
            SDL_RenderPresent(s_drcRen);
        }
        if (s_offTv) {                   /* TV stays black, but must still swap */
            SDL_RenderSetClipRect(s_tvRen, NULL);
            SDL_SetRenderDrawColor(s_tvRen, 0, 0, 0, 255);
            SDL_RenderClear(s_tvRen);
        }
        SDL_RenderPresent(s_tvRen);
        if (GamepadScreen_TakeOffTvToggle())
            SetOffTv(!s_offTv);
    }
    Pgxp_FrameAdvance();
    WiiU_ProcPump();
    if ((s_frame % 60) == 0)
        { SH_DBG("[GPU] frame=%u verts=%u draws=%u uploads=%u maxDraws(last 60)=%u pgxpTess=%d", s_frame, s_verts, s_draws, s_uploads, s_maxDraws, s_tessTris); s_maxDraws = 0; }
    s_frame++;
}

/* Present is vsynced (SDL_RENDERER_PRESENTVSYNC), so this has nothing to wait for. */
void GpuNv2a_WaitVbl(void) { }
void GpuNv2a_DrainGpu(void) { FlushBatch(); }

int  GpuNv2a_FreezeCapture(void) { return 0; }
void GpuNv2a_FreezeRelease(void) { }
void GpuNv2a_FreezeReserve(void) { }
void GpuNv2a_FreezeBlit(void)    { }

const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
{
    (void)fromLastQueued;
    if (w) *w = 0;
    if (h) *h = 0;
    if (pitchBytes) *pitchBytes = 0;
    return NULL;
}

int GpuNv2a_Ms(void) { return (int)SDL_GetTicks(); }
