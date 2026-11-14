/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_gx2.c - native GX2 backend behind the GpuNv2a_* interface
 * (xbox_port/include/gpu_nv2a.h), replacing gpu_sdl_wiiu.c (WIIU_RENDERER=sdl
 * still builds that one).
 *
 * Why: SDL_RenderGeometry can only modulate (tex * colour), so the fog term
 * (out = tex*col + spec) cost a second pass, and back-to-front order made that
 * one colour+fog pair per same-page run: ~2,500 draws a frame in town. SDL's
 * vertices are also 2D, so PGXP needed CPU subdivision.
 *
 * Here ONE pixel shader (CafeGLSL, compiled at boot) computes the same thing
 * ogc_port/src/gpu_gx.c builds from two TEV stages:
 *
 *   rgb = tex.rgb * col.rgb + spec.rgb
 *   a   = ABR0 (blend 1):  0 / 0.5 / 1 from the texel's alpha level
 *         other modes:     a constant (1, or 1/4 for ABR3) where tex.a > 0
 *         discard where tex.a == 0 (the PSX's transparent 0x0000 texel)
 *
 * and the blend unit does the PSX modes (the same table as the GX backend):
 *   0 opaque   no blend
 *   1 ABR0     SRC_ALPHA, INV_SRC_ALPHA        B/2 + F/2
 *   2 ABR1     ONE, ONE                        B + F
 *   3 ABR2     ONE, ONE, REV_SUBTRACT          B - F
 *   4 ABR3     SRC_ALPHA, ONE  (alpha 1/4)     B + F/4
 *
 * Perspective: vertices go to the GPU in clip space (x*w, y*w, w) with w from
 * PGXP (pos[3], 1.0 when PGXP is off), so the rasteriser's own perspective
 * division interpolates the UVs correctly. No CPU subdivision.
 *
 * Textures (see the README "Wii U renderer" section for the justification):
 *   - PSX pages are CI8: one R8 index atlas (4096x4096, 256 slots of 256x256)
 *     plus one palette texture (256 x 1024 RGBA, a row per CLUT). The pixel
 *     shader looks the index up in the palette row. A page is uploaded once
 *     (64 KB) and any CLUT can then be drawn with it with NO state change and
 *     no extra upload; the ARGB atlas of the SDL build needed a 256 KB
 *     converted copy per (page, CLUT) pair and three alpha variants of each.
 *   - non-paletted small textures: a 2048x2048 RGBA "direct" atlas (64 slots).
 *   - oversize textures (FMV frames, framebuffer grabs): a plain texture, one
 *     of three in rotation.
 *   Which of the four sources a vertex samples is a per-vertex attribute, so a
 *   batch is only ended by a blend-mode or scissor change, or by binding a new
 *   oversize texture.
 *
 * Vertex data: one big vertex buffer per frame (3 in rotation, guarded with
 * GX2 timestamps). Texture memory is written by the CPU while the GPU may still
 * be drawing earlier frames, so every cache entry remembers when it was last
 * drawn and an entry that may be in flight is never overwritten in place
 * (a changed page moves to a fresh slot; if none is free the GPU is drained).
 *
 * Output: both screens are GX2 now. The TV is 1920x1080 (Hor+ in game, 4:3
 * screens pillarboxed, never stretched) and the GamePad 854x480. The GamePad
 * UI (gamepad_screen.c) draws through the Gx2Ui_* calls at the bottom of this
 * file; there is no SDL video any more (SDL stays for audio and timers). One
 * GX2SwapScanBuffers per frame swaps both screens.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <SDL.h>

#include <coreinit/memdefaultheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <cafeglsl/CafeGLSLCompiler.h>

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

#define DRC_W 854
#define DRC_H 480

/* gamepad_screen.c */
extern void GamepadScreen_Init(SDL_Renderer* unused);
extern void GamepadScreen_Draw(SDL_Renderer* unused);
extern void GamepadScreen_DrawOverlay(SDL_Renderer* unused);
extern int  GamepadScreen_TakeOffTvToggle(void);
extern int  GamepadScreen_WantMapCapture(void);
extern void GamepadScreen_MapCaptured(void);

/* kept for symmetry with the SDL backend (nothing reads it on GX2) */
void* g_WiiuWindow;

/* ---- shaders ---------------------------------------------------------------- */

static const char kVS[] =
    "#version 450\n"
    "layout(location = 0) in vec4 aClip;\n"
    "layout(location = 1) in vec4 aTex;\n"
    "layout(location = 2) in vec4 aCol;\n"
    "layout(location = 3) in vec4 aSpec;\n"
    "layout(location = 0) out vec4 vTex;\n"
    "layout(location = 1) out vec4 vCol;\n"
    "layout(location = 2) out vec4 vSpec;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = aClip;\n"
    "    vTex  = aTex;\n"
    "    vCol  = aCol * (1.0 / 255.0);\n"
    "    vSpec = aSpec * (1.0 / 255.0);\n"
    "}\n";

/* vSpec.a carries the texture source (0 page+palette, 1 direct atlas, 2 white,
 * 3 oversize, as k/3); vTex = (u, v, palette row v, -); vCol.a = 0 for ABR0
 * (derive 0 / 0.5 / 1 from the texel), else the constant alpha. */
static const char kPS[] =
    "#version 450\n"
    "layout(binding = 0) uniform sampler2D uIdx;\n"
    "layout(binding = 1) uniform sampler2D uPal;\n"
    "layout(binding = 2) uniform sampler2D uDirect;\n"
    "layout(binding = 3) uniform sampler2D uBig;\n"
    "layout(location = 0) in vec4 vTex;\n"
    "layout(location = 1) in vec4 vCol;\n"
    "layout(location = 2) in vec4 vSpec;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    float m = vSpec.a * 3.0;\n"
    "    vec4 t;\n"
    "    if (m < 0.5) {\n"
    "        float idx = textureLod(uIdx, vTex.xy, 0.0).r;\n"
    "        t = textureLod(uPal, vec2((idx * 255.0 + 0.5) * (1.0 / 256.0), vTex.z), 0.0);\n"
    "    } else if (m < 1.5) {\n"
    "        t = textureLod(uDirect, vTex.xy, 0.0);\n"
    "    } else if (m < 2.5) {\n"
    "        t = vec4(1.0, 1.0, 1.0, 0.5);\n"
    "    } else {\n"
    "        t = textureLod(uBig, vTex.xy, 0.0);\n"
    "    }\n"
    "    if (t.a < 0.004) discard;\n"
    "    float a = vCol.a < 0.01 ? (t.a < 0.75 ? 0.5 : 1.0) : vCol.a;\n"
    "    oColor = vec4(t.rgb * vCol.rgb + vSpec.rgb, a);\n"
    "}\n";

typedef struct {
    float clip[4];       /* x*w, y*w, z*w, w */
    float uv[4];         /* u, v (atlas-normalised), palette row v, unused */
    float col[4];            /* 0..255 (the vertex shader scales to 0..1) */
    float spec[4];           /* rgb = fog 0..255, a = texture source * 85 */
} Vtx;                       /* 64 bytes */

enum { K_PAL = 0, K_DIRECT = 1, K_WHITE = 2, K_BIG = 3 };

static GX2VertexShader* s_vs;
static GX2PixelShader*  s_ps;
static GX2ShaderMode    s_shaderMode;
static GX2FetchShader   s_fs;
static void*            s_fsCode;
static int              s_ready;

/* ---- memory ----------------------------------------------------------------- */

static void* AllocMem(uint32_t size, uint32_t align)
{
    return MEMAllocFromDefaultHeapEx(size, align);
}

static void* AllocMem1(uint32_t size, uint32_t align)
{
    MEMHeapHandle h = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    void* p = h ? MEMAllocFromFrmHeapEx(h, size, (int)align) : NULL;
    return p ? p : AllocMem(size, align);
}

/* ---- targets ---------------------------------------------------------------- */

static GX2ColorBuffer  s_tvCB, s_drcCB;
static GX2ContextState* s_ctx;
static int s_tvW = 1920, s_tvH = 1080;
static GX2Texture s_mapTex;
static int s_mapOk;

static int InitColorBuffer(GX2ColorBuffer* cb, int w, int h, int tv)
{
    memset(cb, 0, sizeof(*cb));
    cb->surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    cb->surface.width     = w;
    cb->surface.height    = h;
    cb->surface.depth     = 1;
    cb->surface.mipLevels = 1;
    cb->surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    cb->surface.aa        = GX2_AA_MODE1X;
    cb->surface.use       = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    cb->surface.tileMode  = GX2_TILE_MODE_DEFAULT;
    cb->viewNumSlices     = 1;
    GX2CalcSurfaceSizeAndAlignment(&cb->surface);
    cb->surface.image = AllocMem1(cb->surface.imageSize, cb->surface.alignment);
    if (!cb->surface.image) return 0;
    GX2InitColorBufferRegs(cb);
    (void)tv;
    return 1;
}

static int InitDisplay(void)
{
    uint32_t size, unk;
    void *tvScan, *drcScan;
    GX2TVRenderMode mode;
    const GX2TVScanMode scan = GX2GetSystemTVScanMode();

    switch (scan) {
    case GX2_TV_SCAN_MODE_1080I:
    case GX2_TV_SCAN_MODE_1080P: s_tvW = 1920; s_tvH = 1080; mode = GX2_TV_RENDER_MODE_WIDE_1080P; break;
    case GX2_TV_SCAN_MODE_576I:
    case GX2_TV_SCAN_MODE_480I:
    case GX2_TV_SCAN_MODE_480P:  s_tvW = 854;  s_tvH = 480;  mode = GX2_TV_RENDER_MODE_WIDE_480P;  break;
    default:                     s_tvW = 1280; s_tvH = 720;  mode = GX2_TV_RENDER_MODE_WIDE_720P;  break;
    }
    SH_DBG("[GPU] TV scan mode %d -> %dx%d", (int)scan, s_tvW, s_tvH);

    GX2CalcTVSize(mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &size, &unk);
    tvScan = AllocMem(size, GX2_SCAN_BUFFER_ALIGNMENT);
    if (!tvScan) return 0;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tvScan, size);
    GX2SetTVBuffer(tvScan, size, mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);

    GX2CalcDRCSize(GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &size, &unk);
    drcScan = AllocMem(size, GX2_SCAN_BUFFER_ALIGNMENT);
    if (!drcScan) return 0;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drcScan, size);
    GX2SetDRCBuffer(drcScan, size, GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);

    if (!InitColorBuffer(&s_tvCB, s_tvW, s_tvH, 1) || !InitColorBuffer(&s_drcCB, DRC_W, DRC_H, 0)) return 0;

    s_ctx = (GX2ContextState*)AllocMem(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!s_ctx) return 0;
    GX2SetupContextStateEx(s_ctx, TRUE);
    GX2SetContextState(s_ctx);
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    GX2SetSwapInterval(1);

    /* 4:3-area copy of the TV image for the GamePad map (same format and tiling,
     * so GX2CopySurface is a plain GPU copy; no CPU readback of a tiled surface) */
    memset(&s_mapTex, 0, sizeof(s_mapTex));
    s_mapTex.surface = s_tvCB.surface;
    s_mapTex.surface.image = AllocMem(s_mapTex.surface.imageSize, s_mapTex.surface.alignment);
    if (s_mapTex.surface.image) {
        s_mapTex.viewNumMips = 1;
        s_mapTex.viewNumSlices = 1;
        s_mapTex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        GX2InitTextureRegs(&s_mapTex);
        s_mapOk = 1;
    }
    return 1;
}

/* ---- textures --------------------------------------------------------------- */

#define PAGE 256
#define IDX_DIM   4096                         /* index atlas: 16 x 16 slots */
#define IDX_COLS  (IDX_DIM / PAGE)
#define IDX_SLOTS (IDX_COLS * IDX_COLS)        /* 256 */
#define DIR_DIM   2048                         /* direct atlas: 8 x 8 slots */
#define DIR_COLS  (DIR_DIM / PAGE)
#define DIR_SLOTS (DIR_COLS * DIR_COLS)        /* 64 */
#define PAL_ROWS  1024
#define NBIG 3

static GX2Texture s_idxTex, s_palTex, s_dirTex, s_bigTex[NBIG];
#define LIVE_W 320                             /* paper-map image size */
#define LIVE_H 480
static GX2Texture s_liveTex;                   /* GamePad live map, CPU-composed */
static int        s_liveOk;
static GX2Sampler s_sampPoint, s_sampLinear;
static unsigned char* s_idxMem; static int s_idxPitch;
static unsigned*      s_palMem; static int s_palPitch;
static unsigned*      s_dirMem; static int s_dirPitch;
static int            s_bigW[NBIG], s_bigH[NBIG], s_bigNext, s_bigCur;
static unsigned       s_uploads, s_syncs;

static int TexCreate(GX2Texture* t, int w, int h, GX2SurfaceFormat fmt, uint32_t comp)
{
    memset(t, 0, sizeof(*t));
    t->surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    t->surface.width     = w;
    t->surface.height    = h;
    t->surface.depth     = 1;
    t->surface.mipLevels = 1;
    t->surface.format    = fmt;
    t->surface.aa        = GX2_AA_MODE1X;
    t->surface.use       = GX2_SURFACE_USE_TEXTURE;
    /* linear: the CPU writes rows straight in, no tiling code */
    t->surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
    t->viewNumMips       = 1;
    t->viewNumSlices     = 1;
    t->compMap           = comp;
    GX2CalcSurfaceSizeAndAlignment(&t->surface);
    t->surface.image = AllocMem(t->surface.imageSize, t->surface.alignment);
    if (!t->surface.image) return 0;
    memset(t->surface.image, 0, t->surface.imageSize);
    GX2InitTextureRegs(t);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, t->surface.image, t->surface.imageSize);
    return 1;
}

static void TexDestroy(GX2Texture* t)
{
    if (t->surface.image) MEMFreeToDefaultHeap(t->surface.image);
    memset(t, 0, sizeof(*t));
}

#define COMP_RGBA GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A)

static int TexturesCreate(void)
{
    int i;
    if (!TexCreate(&s_idxTex, IDX_DIM, IDX_DIM, GX2_SURFACE_FORMAT_UNORM_R8,
                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1))) return 0;
    if (!TexCreate(&s_palTex, PAGE, PAL_ROWS, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, COMP_RGBA)) return 0;
    if (!TexCreate(&s_dirTex, DIR_DIM, DIR_DIM, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, COMP_RGBA)) return 0;
    s_liveOk = TexCreate(&s_liveTex, LIVE_W, LIVE_H, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, COMP_RGBA);
    for (i = 0; i < NBIG; i++) {
        if (!TexCreate(&s_bigTex[i], 4, 4, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, COMP_RGBA)) return 0;
        s_bigW[i] = s_bigH[i] = 4;
    }
    s_idxMem = (unsigned char*)s_idxTex.surface.image; s_idxPitch = s_idxTex.surface.pitch;
    s_palMem = (unsigned*)s_palTex.surface.image;      s_palPitch = s_palTex.surface.pitch;
    s_dirMem = (unsigned*)s_dirTex.surface.image;      s_dirPitch = s_dirTex.surface.pitch;
    GX2InitSampler(&s_sampPoint,  GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSampler(&s_sampLinear, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    SH_DBG("[GPU] textures: index atlas %dx%d (pitch %d), palettes %dx%d, direct atlas %dx%d",
           IDX_DIM, IDX_DIM, s_idxPitch, PAGE, PAL_ROWS, DIR_DIM, DIR_DIM);
    return 1;
}

/* ARGB8888 (host order) -> the byte order GX2 reads as R,G,B,A */
static inline unsigned ToRGBA(unsigned c) { return (c << 8) | (c >> 24); }

/* ---- cache of what the GPU copy of the textures holds ----------------------- */

typedef struct {
    const void* key;
    int w, h;
    unsigned gen, epoch, lastUse;
} Ent;
typedef struct { Ent* e; int n; } Cache;
static Ent s_idxEnt[IDX_SLOTS], s_palEnt[PAL_ROWS], s_dirEnt[DIR_SLOTS];
static Cache s_ci = { s_idxEnt, IDX_SLOTS }, s_cp = { s_palEnt, PAL_ROWS }, s_cd = { s_dirEnt, DIR_SLOTS };
static int s_syncFrame = -1;       /* frame in which the GPU was last drained */
static void FlushBatch(void);
static void SegReset(void);

/* May the GPU still read this entry? Anything drawn this frame (queued, maybe
 * not even submitted), or in one of the two frames before it unless the GPU
 * was drained since. */
static int InFlight(const Ent* e)
{
    const unsigned now = (unsigned)g_Nv2aFrameCount;
    if (!e->key) return 0;
    if (e->lastUse == now) return 1;
    return (e->lastUse + 2 >= now) && (s_syncFrame != (int)now);
}

static void GpuDrain(void)
{
    FlushBatch();
    GX2Flush();
    GX2DrawDone();
    s_syncFrame = g_Nv2aFrameCount;
    s_syncs++;
}

/* A slot nothing can still be reading: empty, else least recently used. */
static int Victim(Cache* c)
{
    int i, best = -1;
    for (i = 0; i < c->n; i++) {
        if (!c->e[i].key) { best = i; break; }
        if (best < 0 || c->e[i].lastUse < c->e[best].lastUse) best = i;
    }
    if (InFlight(&c->e[best])) {
        /* every slot is recent: wait for the GPU, then reuse the oldest */
        GpuDrain();
        if (c->e[best].lastUse == (unsigned)g_Nv2aFrameCount) {
            /* in this very frame: still queued after the drain */
            static int s_told;
            if (!s_told++) SH_DBG("[GPU] texture cache thrash: all %d slots used this frame", c->n);
        }
    }
    return best;
}

/* Entry for (key, w, h); *upload = its contents must be (re)written. The
 * validity rule is the other backends': PsxVram_DataGen re-verified whenever
 * the decode epoch moves. */
static int CacheGet(Cache* c, const void* key, int w, int h, int* upload)
{
    const unsigned ep = PsxVram_DecodeEpoch();
    const unsigned now = (unsigned)g_Nv2aFrameCount;
    int i, k = -1;
    *upload = 0;
    for (i = 0; i < c->n; i++)
        if (c->e[i].key == key && c->e[i].w == w && c->e[i].h == h) { k = i; break; }
    if (k >= 0) {
        Ent* e = &c->e[k];
        if (e->epoch != ep) {
            const unsigned g = PsxVram_DataGen(key);
            const int same = (g && g == e->gen);
            e->gen = g;
            e->epoch = g ? ep : ep - 1u;
            if (!same) {
                if (InFlight(e)) {
                    /* its old contents are still wanted by queued/in-flight
                     * draws: write the new ones to another slot */
                    const int j = Victim(c);
                    Ent keep = *e;
                    e->key = NULL;
                    k = j; e = &c->e[k];
                    *e = keep;
                }
                *upload = 1;
            }
        }
    } else {
        Ent* e;
        k = Victim(c);
        e = &c->e[k];
        memset(e, 0, sizeof(*e));
        e->key = key; e->w = w; e->h = h;
        e->gen = PsxVram_DataGen(key);
        e->epoch = e->gen ? ep : ep - 1u;
        *upload = 1;
    }
    c->e[k].lastUse = now;
    return k;
}

static void UploadIdx(int k, const void* src)
{
    const unsigned char* s = (const unsigned char*)src;
    unsigned char* d = s_idxMem + (size_t)(k / IDX_COLS) * PAGE * s_idxPitch + (size_t)(k % IDX_COLS) * PAGE;
    int y;
    for (y = 0; y < PAGE; y++) memcpy(d + (size_t)y * s_idxPitch, s + y * PAGE, PAGE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, d, (uint32_t)((PAGE - 1) * (size_t)s_idxPitch + PAGE));
    s_uploads++;
}

static void UploadPal(int k, const void* src)
{
    const unsigned* s = (const unsigned*)src;
    unsigned* d = s_palMem + (size_t)k * s_palPitch;
    int i;
    for (i = 0; i < 256; i++) d[i] = ToRGBA(s[i]);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, d, 256 * 4);
    s_uploads++;
}

static void UploadDir(int k, const void* src, int w, int h)
{
    const unsigned* s = (const unsigned*)src;
    unsigned* d = s_dirMem + (size_t)(k / DIR_COLS) * PAGE * s_dirPitch + (size_t)(k % DIR_COLS) * PAGE;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) d[(size_t)y * s_dirPitch + x] = ToRGBA(s[y * w + x]);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, d, (uint32_t)((h > 0 ? h - 1 : 0) * (size_t)s_dirPitch * 4 + w * 4));
    s_uploads++;
}

/* ---- pool, segments, current binding ---------------------------------------- */

#define GPU_POOL_VERTS 65536
static ShVertex s_pool[GPU_POOL_VERTS];
static int      s_poolUsed, s_flushStart;

typedef struct { int start, kind; float ox, oy, scale, palV, maxU, maxV, invW, invH; } Seg;
#define SEG_MAX 65536
static Seg s_seg[SEG_MAX];
static int s_segN;

static Seg    s_cur;              /* the binding vertices are tagged with now */
static const void* s_texAddr; static const void* s_texPal; static int s_texW, s_texH;
static int    s_blendMode;
static int    s_scFull = 1, s_scX, s_scY, s_scW, s_scH;
static unsigned s_frame, s_draws, s_verts, s_maxDraws, s_dropped;

/* ---- vertex buffer ---------------------------------------------------------- */

#define NVB 3
#define VB_VERTS 262144
static Vtx*   s_vb[NVB];
static OSTime s_vbTs[NVB];
static int    s_vbCur;
static int    s_vbUsed;

/* ---- view ------------------------------------------------------------------- */

static int   s_outW = 1920, s_outH = 1080;
static int   s_offTv;
static int   s_vpX, s_vpW;
static float s_viewW = 640.0f, s_viewX = 0.0f, s_ppuX = 1.0f, s_ppuY = 1.0f;
static int   s_horPlus = -1;

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

/* ---- GX2 state --------------------------------------------------------------- */

static GX2ColorBuffer* s_target;
static int s_hwBlend = -2, s_hwScX, s_hwScY, s_hwScW, s_hwScH, s_hwBig = -1, s_hwBigLinear;
static int s_frameOpen;

static void SetBlendHw(int mode)
{
    if (mode == s_hwBlend) return;
    s_hwBlend = mode;
    switch (mode) {
    case 1:
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD,
                           TRUE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_ADD);
        break;
    case 2:
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_ADD,
                           TRUE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_ADD);
        break;
    case 3:
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_REV_SUB,
                           TRUE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_ADD);
        break;
    case 4:
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_ADD,
                           TRUE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_COMBINE_MODE_ADD);
        break;
    default: break;
    }
    GX2SetColorControl(GX2_LOGIC_OP_COPY, mode > 0 ? 1 : 0, FALSE, TRUE);
}

static void SetScissorHw(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_outW) w = s_outW - x;
    if (y + h > s_outH) h = s_outH - y;
    if (w < 1 || h < 1) { x = 0; y = 0; w = 1; h = 1; }
    if (x == s_hwScX && y == s_hwScY && w == s_hwScW && h == s_hwScH) return;
    s_hwScX = x; s_hwScY = y; s_hwScW = w; s_hwScH = h;
    GX2SetScissor((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

static void SetGameScissor(void)
{
    if (s_scFull) {
        SetScissorHw(s_vpX, 0, s_vpW, s_outH);
    } else {
        SetScissorHw(s_vpX + (int)((s_viewX + (float)s_scX) * s_ppuX), (int)((float)s_scY * s_ppuY),
                     (int)((float)s_scW * s_ppuX), (int)((float)s_scH * s_ppuY));
    }
}

static void SetBigHw(int idx, int linear)
{
    if (idx == s_hwBig && linear == s_hwBigLinear) return;
    s_hwBig = idx; s_hwBigLinear = linear;
    GX2SetPixelTexture(idx == -3 ? &s_liveTex : idx < 0 ? &s_mapTex : &s_bigTex[idx], 3);
    GX2SetPixelSampler(linear ? &s_sampLinear : &s_sampPoint, 3);
}

/* Everything a draw needs that GX2SetContextState wipes. */
static void SetPipeline(void)
{
    GX2SetFetchShader(&s_fs);
    GX2SetVertexShader(s_vs);
    GX2SetPixelShader(s_ps);
    GX2SetShaderMode(s_shaderMode);
    GX2SetAttribBuffer(0, VB_VERTS * (uint32_t)sizeof(Vtx), (uint32_t)sizeof(Vtx), s_vb[s_vbCur]);
    GX2SetPixelTexture(&s_idxTex, 0);  GX2SetPixelSampler(&s_sampPoint, 0);
    GX2SetPixelTexture(&s_palTex, 1);  GX2SetPixelSampler(&s_sampPoint, 1);
    GX2SetPixelTexture(&s_dirTex, 2);  GX2SetPixelSampler(&s_sampPoint, 2);
    s_hwBig = -2;
    SetBigHw(s_bigCur, 0);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    s_hwBlend = -2; s_hwScW = -1;
}

/* ---- vertex emission helpers ------------------------------------------------- */

static inline unsigned char U8(float f)
{
    return f <= 0.0f ? 0 : (f >= 1.0f ? 255 : (unsigned char)(f * 255.0f + 0.5f));
}

/* Reserve `n` vertices in this frame's vertex buffer (NULL when it is full). */
static Vtx* VbAlloc(int n, int* first)
{
    if (s_vbUsed + n > VB_VERTS) { s_dropped += (unsigned)n; return NULL; }
    *first = s_vbUsed;
    s_vbUsed += n;
    return s_vb[s_vbCur] + *first;
}

static void VbDraw(int first, int n)
{
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, s_vb[s_vbCur] + first, (uint32_t)n * (uint32_t)sizeof(Vtx));
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, (uint32_t)n, (uint32_t)first, 1);
    s_draws++;
}

/* One untextured rectangle in output pixels. */
static void PutRect(Vtx* v, float x, float y, float w, float h, unsigned char r, unsigned char g,
                    unsigned char b, unsigned char a, int outW, int outH)
{
    const float x0 = x / (float)outW * 2.0f - 1.0f, x1 = (x + w) / (float)outW * 2.0f - 1.0f;
    const float y0 = 1.0f - y / (float)outH * 2.0f, y1 = 1.0f - (y + h) / (float)outH * 2.0f;
    const float xs[6] = { x0, x1, x0, x1, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
    int i;
    for (i = 0; i < 6; i++) {
        memset(&v[i], 0, sizeof(Vtx));
        v[i].clip[0] = xs[i]; v[i].clip[1] = ys[i]; v[i].clip[2] = 0.5f; v[i].clip[3] = 1.0f;
        v[i].col[0] = r; v[i].col[1] = g; v[i].col[2] = b; v[i].col[3] = a;
        v[i].spec[3] = (unsigned char)(K_WHITE * 85);
    }
}

/* ---- init ---------------------------------------------------------------------- */

extern const char* Sh360Fs_DataRoot(void);

/* Development aid: gx2_vs.glsl / gx2_ps.glsl next to the disc image replace the
 * built-in shaders (no rebuild needed to iterate on them). */
static char* LoadOverride(const char* name)
{
    char path[256];
    FILE* f;
    long n;
    char* buf;
    snprintf(path, sizeof(path), "%s%s", Sh360Fs_DataRoot(), name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char*)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) { buf[n] = 0; SH_DBG("[GPU] shader override %s (%ld bytes)", name, n); }
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static int CompileShaders(void)
{
    char log[2048];
    char* ovVs = LoadOverride("gx2_vs.glsl");
    char* ovPs = LoadOverride("gx2_ps.glsl");
    InitGLSLCompiler();
    log[0] = 0;
    s_shaderMode = CompileShaderPair(ovVs ? ovVs : kVS, ovPs ? ovPs : kPS, GLSL_COMPILE_AUTO, &s_vs, &s_ps, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
    if (s_shaderMode == GLSL_SHADER_MODE_ERROR || !s_vs || !s_ps) {
        SH_DBG("[GPU] CafeGLSL compile FAILED: %s", log);
        DestroyGLSLCompiler();
        return 0;
    }
    free(ovVs); free(ovPs);
    SH_DBG("[GPU] CafeGLSL %s: shaders compiled, uniform mode %d%s%s", GetGLSLCompilerVersion(), (int)s_shaderMode,
           log[0] ? ", log: " : "", log);
    DestroyGLSLCompiler();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, s_vs->program, s_vs->size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, s_ps->program, s_ps->size);
    return 1;
}

static int CreateFetchShader(void)
{
    GX2AttribStream a[4];
    static const struct { int off; GX2AttribFormat fmt; } kAttr[4] = {
        { 0,  GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32 },
        { 16, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32 },
        { 32, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32 },
        { 48, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32 },
    };
    int i;
    for (i = 0; i < 4; i++) {
        memset(&a[i], 0, sizeof(a[i]));
        a[i].location   = (uint32_t)i;
        a[i].buffer     = 0;
        a[i].offset     = (uint32_t)kAttr[i].off;
        a[i].format     = kAttr[i].fmt;
        a[i].type       = GX2_ATTRIB_INDEX_PER_VERTEX;
        a[i].aluDivisor = 0;
        a[i].mask       = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
        a[i].endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
    }
    {
        const uint32_t size = GX2CalcFetchShaderSizeEx(4, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
        s_fsCode = AllocMem(size, GX2_SHADER_PROGRAM_ALIGNMENT);
        if (!s_fsCode) return 0;
        GX2InitFetchShaderEx(&s_fs, (uint8_t*)s_fsCode, 4, a, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, s_fsCode, size);
    }
    return 1;
}

void GpuNv2a_Init(void)
{
    int i;
    GX2Init(NULL);
    if (!InitDisplay()) { SH_DBG("[GPU] GX2 display setup failed"); return; }
    s_outW = s_tvW; s_outH = s_tvH;
    if (!CompileShaders()) return;
    if (!CreateFetchShader()) { SH_DBG("[GPU] fetch shader allocation failed"); return; }
    if (!TexturesCreate()) { SH_DBG("[GPU] texture allocation failed"); return; }
    for (i = 0; i < NVB; i++) {
        s_vb[i] = (Vtx*)AllocMem(VB_VERTS * (uint32_t)sizeof(Vtx), 0x100);
        if (!s_vb[i]) { SH_DBG("[GPU] vertex buffer allocation failed"); return; }
    }
    GamepadScreen_Init(NULL);
    s_cur.kind = K_WHITE;
    SegReset();
    SelectView();
    s_ready = 1;
    SH_DBG("[GPU] GX2 up: %dx%d TV + %dx%d GamePad, %d-vertex pool, %d-vertex frame buffers x%d, "
           "%d page slots, %d palettes, %d direct slots",
           s_tvW, s_tvH, DRC_W, DRC_H, GPU_POOL_VERTS, VB_VERTS, NVB, IDX_SLOTS, PAL_ROWS, DIR_SLOTS);
}

/* ---- binding ------------------------------------------------------------------- */

static void SegReset(void)
{
    s_segN = 1;
    s_seg[0] = s_cur;
    s_seg[0].start = s_poolUsed;
}

/* Tag the vertices that follow with the binding `b` (replacing an empty tag). */
static void SegMark(const Seg* b)
{
    Seg* last = s_segN ? &s_seg[s_segN - 1] : NULL;
    if (last && last->kind == b->kind && last->ox == b->ox && last->oy == b->oy && last->scale == b->scale &&
        last->palV == b->palV && last->maxU == b->maxU && last->maxV == b->maxV && last->invW == b->invW &&
        last->invH == b->invH)
        return;
    if (last && last->start == s_poolUsed) { *last = *b; last->start = s_poolUsed; return; }
    if (s_segN >= SEG_MAX) FlushBatch();     /* resets the list */
    s_seg[s_segN] = *b; s_seg[s_segN].start = s_poolUsed;
    s_segN++;
}

static void UseBinding(const Seg* b)
{
    s_cur = *b;
    SegMark(b);
}

static void UseWhite(void)
{
    Seg b;
    memset(&b, 0, sizeof(b));
    b.kind = K_WHITE;
    UseBinding(&b);
}

static void BindBig(const void* addr, int w, int h)
{
    int i = s_bigNext++ % NBIG, x, y;
    const unsigned* src = (const unsigned*)addr;
    unsigned* dst;
    Seg b;
    FlushBatch();                       /* the batch so far keeps the old big texture */
    if (s_bigW[i] != w || s_bigH[i] != h) {
        GpuDrain();                     /* free memory the GPU may still read */
        TexDestroy(&s_bigTex[i]);
        if (!TexCreate(&s_bigTex[i], w, h, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, COMP_RGBA)) {
            SH_DBG("[GPU] oversize texture %dx%d: out of memory", w, h);
            TexCreate(&s_bigTex[i], 4, 4, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, COMP_RGBA);
            s_bigW[i] = s_bigH[i] = 4;
            UseWhite();
            return;
        }
        s_bigW[i] = w; s_bigH[i] = h;
        SH_DBG("[GPU] oversize texture %dx%d: buffer %d", w, h, i);
    }
    dst = (unsigned*)s_bigTex[i].surface.image;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) dst[(size_t)y * s_bigTex[i].surface.pitch + x] = ToRGBA(src[y * w + x]);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, dst, (uint32_t)s_bigTex[i].surface.imageSize);
    s_uploads++;
    s_bigCur = i;
    memset(&b, 0, sizeof(b));
    b.kind = K_BIG; b.scale = 1.0f; b.invW = 1.0f / (float)w; b.invH = 1.0f / (float)h;
    UseBinding(&b);
}

static void Rebind(void)
{
    Seg b;
    int up;
    if (!s_ready) return;
    if (!s_texAddr) { UseWhite(); return; }
    memset(&b, 0, sizeof(b));
    if (s_texPal) {
        const int ki = CacheGet(&s_ci, s_texAddr, PAGE, PAGE, &up);
        if (up) UploadIdx(ki, s_texAddr);
        {
            const int kp = CacheGet(&s_cp, s_texPal, PAGE, 1, &up);
            if (up) UploadPal(kp, s_texPal);
            b.palV = ((float)kp + 0.5f) * (1.0f / (float)PAL_ROWS);
        }
        b.kind = K_PAL; b.scale = (float)PAGE;       /* UVs arrive /256 */
        b.ox = (float)((ki % IDX_COLS) * PAGE); b.oy = (float)((ki / IDX_COLS) * PAGE);
        b.invW = b.invH = 1.0f / (float)IDX_DIM;
        b.maxU = b.maxV = 255.99f;
        UseBinding(&b);
    } else if (s_texW > PAGE || s_texH > PAGE) {
        BindBig(s_texAddr, s_texW, s_texH);
    } else {
        const int kd = CacheGet(&s_cd, s_texAddr, s_texW, s_texH, &up);
        if (up) UploadDir(kd, s_texAddr, s_texW, s_texH);
        b.kind = K_DIRECT; b.scale = 1.0f;           /* texel UVs */
        b.ox = (float)((kd % DIR_COLS) * PAGE); b.oy = (float)((kd / DIR_COLS) * PAGE);
        b.invW = b.invH = 1.0f / (float)DIR_DIM;
        b.maxU = (float)s_texW - 0.01f; b.maxV = (float)s_texH - 0.01f;
        UseBinding(&b);
    }
}

void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    if (!page || !palette) { GpuNv2a_BindWhite(); return; }
    if (page == s_texAddr && palette == s_texPal) return;
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
    if (s_ready) UseWhite();
}

void* GpuNv2a_AllocTexMem(int bytes)
{
    return bytes > 0 ? malloc((size_t)bytes) : NULL;
}

/* ---- state ---------------------------------------------------------------------- */

void GpuNv2a_SetBlendMode(int mode)
{
    if (mode == s_blendMode) return;
    FlushBatch();
    s_blendMode = mode;
}

void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    const int full = (w <= 0 || h <= 0);
    if (full) { x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH; }
    if (full == s_scFull && x == s_scX && y == s_scY && w == s_scW && h == s_scH) return;
    FlushBatch();
    s_scX = x; s_scY = y; s_scW = w; s_scH = h; s_scFull = full;
}

void GpuNv2a_SetDepthWrite(int enable) { (void)enable; }
void GpuNv2a_SetDepthTest(int enable)  { (void)enable; }
void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* ---- drawing -------------------------------------------------------------------- */

/* Convert the pending ShVertex run to GPU vertices and draw it: one draw. */
static void FlushBatch(void)
{
    const int n = s_poolUsed - s_flushStart;
    const int cnt = n - n % 3;
    int i, sg = 0, first;
    Vtx* out;
    float alpha;
    if (cnt < 3 || !s_ready || !s_frameOpen) { s_flushStart = s_poolUsed; SegReset(); return; }
    out = VbAlloc(cnt, &first);
    if (!out) { s_flushStart = s_poolUsed; SegReset(); return; }

    switch (s_blendMode) {
    case 0:  alpha = 1.0f;  break;
    case 1:  alpha = 0.0f;  break;      /* ABR0: levels from the texel */
    case 4:  alpha = 0.25f; break;
    default: alpha = 1.0f;  break;
    }
    {
        const unsigned char a8 = U8(alpha);
        for (i = 0; i < cnt; i++) {
            const int vi = s_flushStart + i;
            const ShVertex* v = &s_pool[vi];
            const Seg* s;
            float w, u = v->tex[0], t = v->tex[1], nx, ny;
            Vtx* o = &out[i];
            while (sg + 1 < s_segN && s_seg[sg + 1].start <= vi) sg++;
            s = &s_seg[sg];
            w = v->pos[3] > 0.0f ? v->pos[3] : 1.0f;
            nx = (((float)s_vpX + (v->pos[0] + s_viewX) * s_ppuX) / (float)s_outW) * 2.0f - 1.0f;
            ny = 1.0f - (v->pos[1] * s_ppuY / (float)s_outH) * 2.0f;
            o->clip[0] = nx * w; o->clip[1] = ny * w; o->clip[2] = 0.5f * w; o->clip[3] = w;
            if (s->kind == K_WHITE) {
                u = t = 0.0f;
            } else if (s->kind == K_BIG) {
                u *= s->invW; t *= s->invH;
            } else {
                u *= s->scale; t *= s->scale;
                if (u < 0.0f) u = 0.0f; else if (u > s->maxU) u = s->maxU;
                if (t < 0.0f) t = 0.0f; else if (t > s->maxV) t = s->maxV;
                u = (s->ox + u) * s->invW; t = (s->oy + t) * s->invH;
            }
            o->uv[0] = u; o->uv[1] = t; o->uv[2] = s->palV; o->uv[3] = 0.0f;
            o->col[0] = U8(v->col[0]); o->col[1] = U8(v->col[1]); o->col[2] = U8(v->col[2]); o->col[3] = a8;
            o->spec[0] = U8(v->spec[0]); o->spec[1] = U8(v->spec[1]); o->spec[2] = U8(v->spec[2]);
            o->spec[3] = (unsigned char)(s->kind * 85);
        }
    }
    SetBlendHw(s_blendMode);
    SetGameScissor();
    SetBigHw(s_bigCur, 0);
    VbDraw(first, cnt);
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
void GpuXenos_DebugFbProbe(const char* tag) { SH_DBG("[FBPROBE] %s: no readback on Wii U GX2", tag); }

/* ---- frame ---------------------------------------------------------------------- */

/* Make `cb` the render target: state reset, clear to black, pipeline. */
static void BeginTarget(GX2ColorBuffer* cb, int w, int h, int clear)
{
    GX2SetContextState(s_ctx);
    GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
    GX2SetScissor(0, 0, (uint32_t)w, (uint32_t)h);
    if (clear) GX2ClearColor(cb, 0.0f, 0.0f, 0.0f, 1.0f);
    s_target = cb;
    s_outW = w; s_outH = h;
    SetPipeline();
}

void GpuNv2a_FrameBegin(void)
{
    unsigned c;
    int first;
    Vtx* q;
    g_Nv2aFrameCount++;
    if (s_draws > s_maxDraws) s_maxDraws = s_draws;
    s_poolUsed = s_flushStart = 0;
    s_draws = s_verts = s_uploads = 0;
    s_syncs = 0;
    s_dropped = 0;
    s_frameOpen = 0;
    SegReset();
    if (!s_ready) return;

    /* this frame's vertex buffer: wait until the GPU is done with the frame
     * that used it NVB frames ago */
    s_vbCur = (s_vbCur + 1) % NVB;
    if (s_vbTs[s_vbCur]) GX2WaitTimeStamp(s_vbTs[s_vbCur]);
    s_vbUsed = 0;

    s_outW = s_offTv ? DRC_W : s_tvW;
    s_outH = s_offTv ? DRC_H : s_tvH;
    SelectView();
    BeginTarget(s_offTv ? &s_drcCB : &s_tvCB, s_outW, s_outH, 1);
    s_frameOpen = 1;

    /* the PSX draw-env background (the fog colour in game) over the content
     * area; the pillarbox bars stay black */
    c = s_clearOverrideOn ? s_clearOverride : GpuXbox_GetClearColor();
    q = VbAlloc(6, &first);
    if (q) {
        PutRect(q, (float)s_vpX, 0.0f, (float)s_vpW, (float)s_outH, (unsigned char)(c >> 16), (unsigned char)(c >> 8),
                (unsigned char)c, 255, s_outW, s_outH);
        SetBlendHw(0);
        VbDraw(first, 6);
    }
    s_scFull = 1;
}

/* ---- GamePad UI (gamepad_screen.c draws through these) --------------------------- */

#define UI_MAX 49152
static Vtx s_ui[UI_MAX];
static int s_uiN;
static unsigned char s_uiR, s_uiG, s_uiB, s_uiA = 255;

static void UiFlush(void)
{
    int first;
    Vtx* o;
    if (s_uiN < 6) { s_uiN = 0; return; }
    o = VbAlloc(s_uiN, &first);
    if (o) {
        memcpy(o, s_ui, (size_t)s_uiN * sizeof(Vtx));
        SetBlendHw(1);
        VbDraw(first, s_uiN);
    }
    s_uiN = 0;
}

void Gx2Ui_Color(int r, int g, int b, int a) { s_uiR = (unsigned char)r; s_uiG = (unsigned char)g; s_uiB = (unsigned char)b; s_uiA = (unsigned char)a; }

void Gx2Ui_Fill(float x, float y, float w, float h)
{
    if (s_uiN + 6 > UI_MAX) UiFlush();
    PutRect(&s_ui[s_uiN], x, y, w, h, s_uiR, s_uiG, s_uiB, s_uiA ? s_uiA : 1, DRC_W, DRC_H);
    s_uiN += 6;
}

void Gx2Ui_Rect(int x, int y, int w, int h)   /* 1-pixel outline */
{
    Gx2Ui_Fill((float)x, (float)y, (float)w, 1.0f);
    Gx2Ui_Fill((float)x, (float)(y + h - 1), (float)w, 1.0f);
    Gx2Ui_Fill((float)x, (float)y, 1.0f, (float)h);
    Gx2Ui_Fill((float)(x + w - 1), (float)y, 1.0f, (float)h);
}

void Gx2Ui_Line(int x0, int y0, int x1, int y1)
{
    if (y0 == y1) {
        Gx2Ui_Fill((float)(x0 < x1 ? x0 : x1), (float)y0, (float)(x1 > x0 ? x1 - x0 : x0 - x1) + 1.0f, 1.0f);
    } else if (x0 == x1) {
        Gx2Ui_Fill((float)x0, (float)(y0 < y1 ? y0 : y1), 1.0f, (float)(y1 > y0 ? y1 - y0 : y0 - y1) + 1.0f);
    } else {
        /* a 1-pixel-wide parallelogram: the ECG segments are steep, so widen in x */
        const float ax = (float)x0 + 0.5f, ay = (float)y0 + 0.5f, bx = (float)x1 + 0.5f, by = (float)y1 + 0.5f;
        Vtx* v;
        int i;
        const float px[6] = { ax - 0.5f, ax + 0.5f, bx - 0.5f, ax + 0.5f, bx + 0.5f, bx - 0.5f };
        const float py[6] = { ay, ay, by, ay, by, by };
        if (s_uiN + 6 > UI_MAX) UiFlush();
        v = &s_ui[s_uiN];
        for (i = 0; i < 6; i++) {
            memset(&v[i], 0, sizeof(Vtx));
            v[i].clip[0] = px[i] / (float)DRC_W * 2.0f - 1.0f; v[i].clip[1] = 1.0f - py[i] / (float)DRC_H * 2.0f;
            v[i].clip[2] = 0.5f; v[i].clip[3] = 1.0f;
            v[i].col[0] = s_uiR; v[i].col[1] = s_uiG; v[i].col[2] = s_uiB; v[i].col[3] = s_uiA ? s_uiA : 1;
            v[i].spec[3] = (unsigned char)(K_WHITE * 85);
        }
        s_uiN += 6;
    }
}

/* The captured TV 4:3 area, drawn into dst (GamePad pixels). */
void Gx2Ui_Map(int dx, int dy, int dw, int dh)
{
    int first, i;
    Vtx* q;
    float u0, v0, u1, v1;
    if (!s_mapOk) return;
    UiFlush();
    q = VbAlloc(6, &first);
    if (!q) return;
    {
        const float sx = (float)((s_tvW - s_tvH * 4 / 3) / 2), sw = (float)(s_tvH * 4 / 3);
        u0 = sx / (float)s_tvW; u1 = (sx + sw) / (float)s_tvW; v0 = 0.0f; v1 = 1.0f;
    }
    PutRect(q, (float)dx, (float)dy, (float)dw, (float)dh, 255, 255, 255, 255, DRC_W, DRC_H);
    {
        /* PutRect order: (x0,y0) (x1,y0) (x0,y1) (x1,y0) (x1,y1) (x0,y1) */
        const float us[6] = { u0, u1, u0, u1, u1, u0 }, vs[6] = { v0, v0, v1, v0, v1, v1 };
        for (i = 0; i < 6; i++) { q[i].uv[0] = us[i]; q[i].uv[1] = vs[i]; q[i].spec[3] = (unsigned char)(K_BIG * 85); }
    }
    SetBigHw(-1, 1);
    SetBlendHw(0);
    VbDraw(first, 6);
    SetBigHw(s_bigCur, 0);
}

/* GamePad live map: a w x h ARGB image (the paper map with its markings,
 * composed by pc_minimap.c). Uploaded only when it changes, so the drain that
 * keeps the GPU off the old pixels is rare. */
int Gx2Ui_LiveMapUpload(const unsigned* argb, int w, int h)
{
    unsigned* dst;
    int x, y;
    if (!s_liveOk || w > LIVE_W || h > LIVE_H) return 0;
    UiFlush();
    GpuDrain();
    dst = (unsigned*)s_liveTex.surface.image;
    for (y = 0; y < LIVE_H; y++)
        for (x = 0; x < LIVE_W; x++)
            dst[(size_t)y * s_liveTex.surface.pitch + x] =
                (x < w && y < h) ? ToRGBA(argb[y * w + x]) : 0x000000FFu;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, dst, (uint32_t)s_liveTex.surface.imageSize);
    return 1;
}

/* Draw the image window (u0,v0)-(u1,v1), in image pixels, into dst. */
void Gx2Ui_LiveMap(int dx, int dy, int dw, int dh, float u0, float v0, float u1, float v1)
{
    int first, i;
    Vtx* q;
    if (!s_liveOk) return;
    UiFlush();
    q = VbAlloc(6, &first);
    if (!q) return;
    u0 /= (float)LIVE_W; u1 /= (float)LIVE_W; v0 /= (float)LIVE_H; v1 /= (float)LIVE_H;
    PutRect(q, (float)dx, (float)dy, (float)dw, (float)dh, 255, 255, 255, 255, DRC_W, DRC_H);
    {
        const float us[6] = { u0, u1, u0, u1, u1, u0 }, vs[6] = { v0, v0, v1, v0, v1, v1 };
        for (i = 0; i < 6; i++) { q[i].uv[0] = us[i]; q[i].uv[1] = vs[i]; q[i].spec[3] = (unsigned char)(K_BIG * 85); }
    }
    SetBigHw(-3, 1);
    SetBlendHw(0);
    VbDraw(first, 6);
    SetBigHw(s_bigCur, 0);
}

/* One filled triangle in GamePad pixels (Harry's arrow). */
void Gx2Ui_Tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    const float px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
    Vtx* v;
    int i;
    if (s_uiN + 6 > UI_MAX) UiFlush();
    v = &s_ui[s_uiN];
    /* the batch draws 6-vertex groups: the second triangle repeats the first */
    for (i = 0; i < 6; i++) {
        const int k = i % 3;
        memset(&v[i], 0, sizeof(Vtx));
        v[i].clip[0] = px[k] / (float)DRC_W * 2.0f - 1.0f; v[i].clip[1] = 1.0f - py[k] / (float)DRC_H * 2.0f;
        v[i].clip[2] = 0.5f; v[i].clip[3] = 1.0f;
        v[i].col[0] = s_uiR; v[i].col[1] = s_uiG; v[i].col[2] = s_uiB; v[i].col[3] = s_uiA ? s_uiA : 1;
        v[i].spec[3] = (unsigned char)(K_WHITE * 85);
    }
    s_uiN += 6;
}

/* Off-TV: the TV shows black, but must still be written and swapped. */
static void ClearTv(void)
{
    GX2ClearColor(&s_tvCB, 0.0f, 0.0f, 0.0f, 1.0f);
}

void GpuNv2a_FrameEnd(void)
{
    extern void WiiU_ProcPump(void);   /* main/HOME menu handling (pad_wiiu.c) */
    FlushBatch();
    if (s_ready && s_frameOpen) {
        s_frameOpen = 0;
        if (!s_offTv && s_mapOk && GamepadScreen_WantMapCapture()) {
            /* the map screen is 2D, so it sits in the TV's 4:3 area */
            GX2CopySurface(&s_tvCB.surface, 0, 0, &s_mapTex.surface, 0, 0);
            GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, s_mapTex.surface.image, s_mapTex.surface.imageSize);
            GamepadScreen_MapCaptured();
        }
        /* GamePad: its UI (or, in Off-TV play, the button over the game that is
         * already in the GamePad buffer) */
        if (s_offTv) {
            s_outW = DRC_W; s_outH = DRC_H;
            GX2SetColorBuffer(&s_drcCB, GX2_RENDER_TARGET_0);
            GX2SetViewport(0.0f, 0.0f, (float)DRC_W, (float)DRC_H, 0.0f, 1.0f);
            s_hwScW = -1; SetScissorHw(0, 0, DRC_W, DRC_H);
            Gx2Ui_Color(255, 255, 255, 255);
            GamepadScreen_DrawOverlay(NULL);
            UiFlush();
            ClearTv();
        } else {
            BeginTarget(&s_drcCB, DRC_W, DRC_H, 1);
            GamepadScreen_Draw(NULL);
            UiFlush();
        }
        GX2CopyColorBufferToScanBuffer(&s_tvCB, GX2_SCAN_TARGET_TV);
        GX2CopyColorBufferToScanBuffer(&s_drcCB, GX2_SCAN_TARGET_DRC);
        /* One scan-buffer swap per frame: it swaps the TV and the GamePad. */
        GX2SwapScanBuffers();
        GX2Flush();
        s_vbTs[s_vbCur] = GX2GetLastSubmittedTimeStamp();
        GX2SetTVEnable(TRUE);
        GX2SetDRCEnable(TRUE);
        if (GamepadScreen_TakeOffTvToggle()) {
            s_offTv = !s_offTv;
            s_horPlus = -1;
            SH_DBG("[GPU] output: %s", s_offTv ? "GamePad (Off-TV)" : "TV");
        }
        GX2WaitForVsync();
    }
    Pgxp_FrameAdvance();
    WiiU_ProcPump();
    if ((s_frame % 60) == 0) {
        SH_DBG("[GPU] frame=%u verts=%u draws=%u uploads=%u syncs=%u maxDraws(last 60)=%u vb=%d/%d dropped=%u",
               s_frame, s_verts, s_draws, s_uploads, s_syncs, s_maxDraws, s_vbUsed, VB_VERTS, s_dropped);
        s_maxDraws = 0;
    }
    s_frame++;
}

/* The swap above is vsynced, so this has nothing to wait for. */
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
