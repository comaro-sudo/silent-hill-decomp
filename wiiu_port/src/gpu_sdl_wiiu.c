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

SDL_Window*          g_WiiuWindow;
static SDL_Renderer* s_ren;
static int           s_ready;

static int   s_vpX, s_vpW;
static float s_viewW = 640.0f, s_viewX = 0.0f, s_ppuX = 1.0f, s_ppuY = 1.0f;
static int   s_horPlus = -1;

#define GPU_POOL_VERTS 32768
static ShVertex   s_pool[GPU_POOL_VERTS];
static int        s_poolUsed, s_flushStart;
static SDL_Vertex s_sv[GPU_POOL_VERTS];

#define PAGE 256
#define TEX_CACHE_N 192
typedef struct {
    const void* page; const void* pal;
    unsigned pageGen, palGen, epoch, lastUse;
    int solid;
    SDL_Texture* tex;
} TexEnt;
static TexEnt       s_cache[TEX_CACHE_N];
static unsigned     s_conv[PAGE * PAGE];
static SDL_Texture* s_big[2];
static int          s_bigW[2], s_bigH[2], s_bigSolid[2], s_bigNext;

static SDL_Texture* s_curTex;          /* NULL = untextured */
static float        s_uScale, s_vScale;
static const void*  s_texAddr; static const void* s_texPal; static int s_texW, s_texH;
static int          s_texSolid = -1;   /* variant the current bind was made for */
static int          s_blendMode;
static int          s_scFull = 1, s_scX, s_scY, s_scW, s_scH;
static SDL_BlendMode s_bmOpaque, s_bmHalf, s_bmAdd, s_bmSub;
static unsigned     s_frame, s_draws, s_verts, s_uploads;

static void FlushBatch(void);
static void Rebind(void);

static void SelectView(void)
{
    const int want = (g_PcWidescreenMode == 1 && g_PcHorPlusEnabled) ? 1 : 0;
    if (want == s_horPlus) return;
    s_horPlus = want;
    if (want) {
        s_vpX = 0; s_vpW = TV_W;
        s_viewW = (float)g_Nv2aContentH * 16.0f / 9.0f;
        s_viewX = (s_viewW - (float)g_Nv2aContentW) * 0.5f;
    } else {
        s_vpW = TV_H * 4 / 3; s_vpX = (TV_W - s_vpW) / 2;
        s_viewW = (float)g_Nv2aContentW; s_viewX = 0.0f;
    }
    s_ppuX = (float)s_vpW / s_viewW;
    s_ppuY = (float)TV_H / (float)g_Nv2aContentH;
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
    g_WiiuWindow = SDL_CreateWindow("Silent Hill", 0, 0, TV_W, TV_H, 0);
    s_ren = g_WiiuWindow ? SDL_CreateRenderer(g_WiiuWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!s_ren) {
        SH_DBG("[GPU] SDL renderer failed: %s", SDL_GetError());
        return;
    }
    s_bmOpaque = SDL_BLENDMODE_BLEND;
    s_bmHalf   = SDL_BLENDMODE_BLEND;
    s_bmAdd    = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
                                            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    s_bmSub    = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT,
                                            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
    SelectView();
    s_ready = 1;
    {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(s_ren, &ri);
        SH_DBG("[GPU] SDL renderer '%s' up: %dx%d, %d-vertex pool, %d cached pages",
               ri.name, TV_W, TV_H, GPU_POOL_VERTS, TEX_CACHE_N);
    }
}

/* ---- textures ------------------------------------------------------------- */

/* Host-order ARGB8888 is SDL_PIXELFORMAT_ARGB8888 on any endianness. */
static void Convert(const unsigned* src, const unsigned* pal, int w, int h, int solid)
{
    const unsigned char* idx = (const unsigned char*)src;
    int i, n = w * h;
    for (i = 0; i < n; i++) {
        unsigned c = pal ? pal[idx[i]] : src[i];
        if (solid && (c >> 24)) c |= 0xFF000000u;
        s_conv[i] = c;
    }
}

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

static TexEnt* CacheLookup(const void* page, const void* pal, int w, int h, int solid)
{
    const unsigned ep = PsxVram_DecodeEpoch();
    TexEnt* e = NULL;
    int i;
    for (i = 0; i < TEX_CACHE_N; i++)
        if (s_cache[i].page == page && s_cache[i].pal == pal && s_cache[i].solid == solid) { e = &s_cache[i]; break; }
    if (!(e && e->epoch == ep)) {
        unsigned pg = PsxVram_DataGen(page), pl = pal ? PsxVram_DataGen(pal) : 0;
        if (e && pg && (!pal || pl) && e->pageGen == pg && e->palGen == pl) {
            e->epoch = ep;
        } else {
            if (!e) {
                TexEnt* best = NULL;
                for (i = 0; i < TEX_CACHE_N; i++) {
                    TexEnt* c = &s_cache[i];
                    if (!c->page) { best = c; break; }
                    if (!best || c->lastUse < best->lastUse) best = c;
                }
                e = best;
                if (e->tex == s_curTex) FlushBatch();
            }
            if (!e->tex) e->tex = NewTex(PAGE, PAGE);
            if (!e->tex) return NULL;
            e->page = page; e->pal = pal; e->solid = solid; e->pageGen = pg; e->palGen = pl;
            e->epoch = (pg && (!pal || pl)) ? ep : ep - 1u;
            Convert((const unsigned*)page, (const unsigned*)pal, w, h, solid);
            {
                SDL_Rect r = { 0, 0, w, h };
                SDL_UpdateTexture(e->tex, &r, s_conv, w * 4);
            }
            s_uploads++;
        }
    }
    e->lastUse = (unsigned)g_Nv2aFrameCount;
    return e;
}

static void Use(SDL_Texture* t, float us, float vs)
{
    if (t == s_curTex && us == s_uScale && vs == s_vScale) return;
    FlushBatch();
    s_curTex = t; s_uScale = us; s_vScale = vs;
}

static int WantSolid(void) { return s_blendMode != 1; }

static void BindBig(const void* addr, int w, int h, int solid)
{
    int i = s_bigNext++ & 1;
    unsigned* buf;
    FlushBatch();
    if (!s_big[i] || s_bigW[i] != w || s_bigH[i] != h) {
        if (s_big[i]) SDL_DestroyTexture(s_big[i]);
        s_big[i] = NewTex(w, h);
        s_bigW[i] = w; s_bigH[i] = h;
        if (!s_big[i]) { Use(NULL, 0, 0); return; }
    }
    buf = (unsigned*)malloc((size_t)w * h * 4);
    if (buf) {
        int k, n = w * h;
        for (k = 0; k < n; k++) {
            unsigned c = ((const unsigned*)addr)[k];
            buf[k] = (solid && (c >> 24)) ? (c | 0xFF000000u) : c;
        }
        SDL_UpdateTexture(s_big[i], NULL, buf, w * 4);
        free(buf);
    }
    s_bigSolid[i] = solid;
    s_uploads++;
    Use(s_big[i], 1.0f / (float)w, 1.0f / (float)h);
}

static void Rebind(void)
{
    const int solid = WantSolid();
    TexEnt* e;
    s_texSolid = solid;
    if (!s_ready) return;
    if (!s_texAddr) { Use(NULL, 0, 0); return; }
    if (s_texPal) {
        e = CacheLookup(s_texAddr, s_texPal, PAGE, PAGE, solid);
        Use(e ? e->tex : NULL, 1.0f, 1.0f);           /* UVs arrive /256 */
    } else if (s_texW > PAGE || s_texH > PAGE) {
        BindBig(s_texAddr, s_texW, s_texH, solid);
    } else {
        e = CacheLookup(s_texAddr, NULL, s_texW, s_texH, solid);
        Use(e ? e->tex : NULL, 1.0f / (float)PAGE, 1.0f / (float)PAGE);
    }
}

void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    if (!page || !palette) { GpuNv2a_BindWhite(); return; }
    if (page == s_texAddr && palette == s_texPal && s_texSolid == WantSolid()) return;
    s_texAddr = page; s_texPal = palette; s_texW = s_texH = PAGE;
    Rebind();
}

void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    if (addr == s_texAddr && !s_texPal && w == s_texW && h == s_texH && s_texSolid == WantSolid()) return;
    s_texAddr = (addr && w > 0 && h > 0) ? addr : NULL; s_texPal = NULL; s_texW = w; s_texH = h;
    Rebind();
}

void GpuNv2a_BindWhite(void)
{
    s_texAddr = NULL; s_texPal = NULL;
    Use(NULL, 0, 0);
}

void* GpuNv2a_AllocTexMem(int bytes)
{
    return bytes > 0 ? malloc((size_t)bytes) : NULL;
}

/* ---- state ---------------------------------------------------------------- */

void GpuNv2a_SetBlendMode(int mode)
{
    const int wasSolid = WantSolid();
    if (mode == s_blendMode) return;
    FlushBatch();
    s_blendMode = mode;
    if (WantSolid() != wasSolid && s_texAddr)
        Rebind();                         /* switch the page's alpha variant */
}

static void ApplyClip(void)
{
    SDL_Rect r;
    if (s_scFull) {
        r.x = s_vpX; r.y = 0; r.w = s_vpW; r.h = TV_H;
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

static void FlushBatch(void)
{
    const int n = s_poolUsed - s_flushStart;
    int i, anySpec = 0;
    SDL_BlendMode bm;
    if (n < 3 || !s_ready) { s_flushStart = s_poolUsed; return; }

    switch (s_blendMode) {
    case 1:  bm = s_bmHalf; break;
    case 2:
    case 4:  bm = s_bmAdd;  break;
    case 3:  bm = s_bmSub;  break;
    default: bm = s_bmOpaque; break;
    }
    for (i = 0; i < n; i++) {
        const ShVertex* v = &s_pool[s_flushStart + i];
        SDL_Vertex*     o = &s_sv[i];
        o->position.x  = (float)s_vpX + (v->pos[0] + s_viewX) * s_ppuX;
        o->position.y  = v->pos[1] * s_ppuY;
        o->color.r = U8(v->col[0]); o->color.g = U8(v->col[1]); o->color.b = U8(v->col[2]);
        /* the texel alpha does the work; untextured ABR0 prims carry 0.5 */
        o->color.a = (s_blendMode == 4) ? 64 : ((s_curTex || s_blendMode == 1) ? U8(v->col[3]) : 255);
        if (!s_curTex && s_blendMode == 1) o->color.a = 128;
        o->tex_coord.x = v->tex[0] * s_uScale;
        o->tex_coord.y = v->tex[1] * s_vScale;
        if (v->spec[0] > 0.0f || v->spec[1] > 0.0f || v->spec[2] > 0.0f) anySpec = 1;
    }
    if (s_curTex) SDL_SetTextureBlendMode(s_curTex, bm);
    else          SDL_SetRenderDrawBlendMode(s_ren, bm);
    SDL_RenderGeometry(s_ren, s_curTex, s_sv, n - n % 3, NULL, 0);
    s_draws++;

    if (anySpec && s_blendMode == 0) {
        /* fog: + spec, untextured additive */
        for (i = 0; i < n; i++) {
            const ShVertex* v = &s_pool[s_flushStart + i];
            s_sv[i].color.r = U8(v->spec[0]); s_sv[i].color.g = U8(v->spec[1]);
            s_sv[i].color.b = U8(v->spec[2]); s_sv[i].color.a = 255;
        }
        SDL_SetRenderDrawBlendMode(s_ren, s_bmAdd);
        SDL_RenderGeometry(s_ren, NULL, s_sv, n - n % 3, NULL, 0);
        s_draws++;
    }
    s_flushStart = s_poolUsed;
}

void GpuNv2a_FlushBatch(void) { FlushBatch(); }

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;
    if (count <= 0 || count > GPU_POOL_VERTS) return NULL;
    if (s_poolUsed + count > GPU_POOL_VERTS) { FlushBatch(); s_poolUsed = s_flushStart = 0; }
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
    s_poolUsed = s_flushStart = 0;
    s_draws = s_verts = s_uploads = 0;
    if (!s_ready) return;
    SelectView();
    SDL_RenderSetClipRect(s_ren, NULL);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    c = s_clearOverrideOn ? s_clearOverride : GpuXbox_GetClearColor();
    r.x = s_vpX; r.y = 0; r.w = s_vpW; r.h = TV_H;
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(s_ren, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    SDL_RenderFillRect(s_ren, &r);
    s_scFull = 1;
    ApplyClip();
}

void GpuNv2a_FrameEnd(void)
{
    extern void WiiU_ProcPump(void);   /* main/HOME menu handling (pad_wiiu.c) */
    FlushBatch();
    if (s_ready) SDL_RenderPresent(s_ren);
    Pgxp_FrameAdvance();
    WiiU_ProcPump();
    if ((s_frame % 60) == 0)
        SH_DBG("[GPU] frame=%u verts=%u draws=%u uploads=%u", s_frame, s_verts, s_draws, s_uploads);
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
