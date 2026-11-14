/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_gx.c - GX (Flipper/Hollywood) backend behind the GpuNv2a_* interface
 * (xbox_port/include/gpu_nv2a.h), for GameCube and Wii.
 *
 * The OT walker and PSX primitive decoder (xbox_port/src/gpu_xbox.c) and the
 * VRAM page decoder (psx_vram.c) are shared and unchanged; they hand this file
 * screen-space triangles (ShVertex: position in content pixels plus W, colour,
 * texcoord, fog "specular") and decoded 256x256 pages. Modelled on the 360's
 * gpu_d3d360.cpp, which is the backend that renders the game correctly; the
 * mapping of its two shaders onto fixed-function GX is:
 *
 *   vertex shader   clip.xy = (pos.xy * scale + offset) * pos.w, clip.w = pos.w
 *     -> a GX PERSPECTIVE projection fed X = x*w, Y = y*w, Z = -w. GX's
 *        perspective matrix makes clip.w = -Z = w and clip.x = p00*X + p02*Z,
 *        so p00 = scale, p02 = -offset reproduces it exactly, and texture
 *        interpolation is perspective-correct whenever PGXP supplies a real W
 *        (affine when W = 1, which is the PSX look).
 *   pixel shader    rgb = tex.rgb * col.rgb + spec.rgb
 *                   a   = tex.a * blend.x + blend.y,   clip(tex.a - 0.5/255)
 *     -> TEV stage 0: tex * COLOR0 (vertex colour); stage 1: + COLOR1 (the
 *        fog term, a second vertex colour channel). Alpha: ABR0 passes tex.a
 *        (0x80 for STP texels, 0xFF opaque); every other mode outputs a konst
 *        (1 or 1/4) where tex.a > 0 and 0 elsewhere, using a TEV compare op.
 *        The alpha test (> 0) then kills exactly the transparent 0x0000
 *        texels, as the shader's clip() did.
 *   blend           ABR0 B/2+F/2 = SRCALPHA/INVSRCALPHA (alpha 0x80)
 *                   ABR1 B+F     = ONE/ONE add
 *                   ABR2 B-F     = GX_BM_SUBTRACT (dst - src)
 *                   ABR3 B+F/4   = SRCALPHA/ONE with konst alpha 1/4
 *
 * Textures: each decoded page is converted once into GX's tiled RGBA8 layout
 * and cached (keyed and invalidated exactly like the D3D atlas, via
 * PsxVram_DataGen/PsxVram_DecodeEpoch). Larger-than-a-page uploads (FMV frames,
 * full-screen images) go to a two-buffer ring.
 *
 * Output: 640x480 EFB, copied to a double-buffered XFB. 16:9 on Wii follows the
 * console setting (CONF_GetAspectRatio) and is anamorphic: the Hor+ view is
 * 853 content units wide squeezed into 640 pixels; 2D screens stay 4:3 and are
 * pillarboxed into the middle 480 pixels, never stretched. GameCube is 4:3.
 */
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/cache.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "ppc/timebase.h"

/* ---- shared state the decoder reads (same names as the D3D backend) ------- */
int                g_Nv2aFrameCount;
int                g_Nv2aContentX = 0;
int                g_Nv2aContentW = 640;
int                g_Nv2aContentH = 480;
int                g_Nv2aViewW    = 640;
unsigned long long g_Nv2aDrawCycles;
extern int g_PcWidescreenMode;          /* xbox_pcfeature_stubs.c: cfg widescreen_mode */
extern int g_PcHorPlusEnabled;          /* xbox_compat_globals.c, set by game_main.c */
extern int g_PsxPresentLastFrame;
extern unsigned PsxVram_DataGen(const void* data);
extern unsigned PsxVram_DecodeEpoch(void);
extern void Pgxp_FrameAdvance(void);

/* ---- video ---------------------------------------------------------------- */
#define FIFO_SIZE (256 * 1024)
static GXRModeObj* s_rmode;
static void*       s_xfb[2];
static int         s_xfbIdx;
static void*       s_fifo;
static int         s_ready;
static int         s_tv169;             /* console set to 16:9 (Wii only) */
static int         s_efbW = 640, s_efbH = 480;

/* view: the viewport rect in EFB pixels and the content width it shows */
static int   s_vpX, s_vpW;
static float s_viewW = 640.0f, s_viewX = 0.0f;
static int   s_horPlus = -1;

/* ---- vertex pool ---------------------------------------------------------- */
#if defined(SH_GC_PORT)
#define GPU_POOL_VERTS 8192
#define TEX_CACHE_N    3                /* direct-colour pages only (CI8 does the rest) */
#else
#define GPU_POOL_VERTS 24576
#define TEX_CACHE_N    16               /* direct-colour pages only (CI8 does the rest) */
#endif
static ShVertex s_pool[GPU_POOL_VERTS];
static int      s_poolUsed, s_flushStart;

/* ---- texture binding ------------------------------------------------------ */
#define PAGE 256
typedef struct {
    const void* page;   /* key: decoded page (indices or ARGB); NULL = empty */
    const void* pal;    /* key: palette, NULL for ARGB pages */
    unsigned    pageGen, palGen, epoch, lastUse;
    int         w, h;
    void*       gx;     /* RGBA8 tiles */
} TexEnt;
static TexEnt    s_cache[TEX_CACHE_N];

/* Palette-indexed pages, the bulk of the PSX's textures, use GX's native CI8
 * format: the 8-bit index page is cached ONCE (a byte reshuffle into 8x4
 * tiles, 64 KB) and each CLUT separately as a 256-entry RGB5A3 TLUT (512 B).
 * Expanding every page x palette pair to RGBA8 instead (the first version)
 * converted ~800 pages a frame in town -- the PSX reuses one page with many
 * CLUTs -- and that conversion was the frame time (Dolphin, 2026-11-11). */
#if defined(SH_GC_PORT)
#define CI_N 12                          /* 768 KB */
#define TL_N 128
#else
#define CI_N 64                          /* 4 MB */
#define TL_N 512
#endif
typedef struct { const void* key; unsigned gen, epoch, lastUse; void* buf; } KeyEnt;
static KeyEnt    s_ci[CI_N];
static KeyEnt    s_tl[TL_N];
static GXTlutObj s_curTlut;
static void*     s_curTl;               /* TLUT buffer of the current bind, NULL = not CI */
static int       s_tlutDirty;
static unsigned  s_tlutSlot;
static void*     s_bigBuf[2];
static int       s_bigCap[2], s_bigNext;
static unsigned  s_bigUse[2];
static u8        s_whiteTex[4 * 4 * 4] ATTRIBUTE_ALIGN(32);
static GXTexObj  s_curObj;
static void*     s_curGx;          /* buffer s_curObj points at */
static int       s_curObjValid, s_curObjDirty = 1, s_texInvalidate;
static float     s_uScale = 0.0f, s_vScale = 0.0f;   /* tex -> normalised */
static const void* s_texAddr;
static const void* s_texPal;
static int       s_texW, s_texH;
static int       s_blendMode;
static int       s_scFull = 1, s_scX, s_scY, s_scW, s_scH;
static unsigned  s_clearOverride;
static int       s_clearOverrideOn;

/* per-frame counters for the once-a-second line */
static unsigned s_frame, s_draws, s_verts, s_binds, s_uploads;

static void FlushBatch(void);
static void ApplyBlend(void);
static void ApplyScissor(void);

/* ------------------------------------------------------------------------- */

static void SelectView(void)
{
    const int want = (s_tv169 && g_PcWidescreenMode == 1 && g_PcHorPlusEnabled) ? 1 : 0;
    if (want == s_horPlus)
        return;
    s_horPlus = want;
    if (want) {
        /* anamorphic: 16:9 of content in all 640 pixels */
        s_vpX = 0; s_vpW = s_efbW;
        s_viewW = (float)g_Nv2aContentH * 16.0f / 9.0f;
        s_viewX = (s_viewW - (float)g_Nv2aContentW) * 0.5f;
    } else if (s_tv169) {
        /* 4:3 on a 16:9 screen: the middle 3/4, black bars */
        s_vpW = s_efbW * 3 / 4; s_vpX = (s_efbW - s_vpW) / 2;
        s_viewW = (float)g_Nv2aContentW; s_viewX = 0.0f;
    } else {
        s_vpX = 0; s_vpW = s_efbW;
        s_viewW = (float)g_Nv2aContentW; s_viewX = 0.0f;
    }
    g_Nv2aViewW = (int)(s_viewW + 0.5f);
    SH_DBG("[GPU] view: %s (viewport %d px at %d, content %.1f wide)",
           want ? "Hor+ 16:9 anamorphic" : (s_tv169 ? "4:3 pillarbox" : "4:3"),
           s_vpW, s_vpX, s_viewW);
}

static void LoadProjection(void)
{
    Mtx44 p;
    const float sx = 2.0f / s_viewW;
    const float ox = -1.0f + 2.0f * s_viewX / s_viewW;
    const float sy = -2.0f / (float)g_Nv2aContentH;
    const float oy = 1.0f;
    memset(p, 0, sizeof(p));
    p[0][0] = sx;  p[0][2] = -ox;
    p[1][1] = sy;  p[1][2] = -oy;
    p[2][2] = 0.5f; p[2][3] = 0.0f;     /* constant depth -0.5; Z is off */
    p[3][2] = -1.0f;
    GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
    GX_SetViewport((f32)s_vpX, 0.0f, (f32)s_vpW, (f32)s_efbH, 0.0f, 1.0f);
}

static void SetupTev(void)
{
    GX_SetNumChans(2);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(2);

    /* stage 0: tex * vertex colour */
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    /* stage 1: + fog colour (second vertex colour channel) */
    /* stage 1 samples the texture too: ABR0 needs the texel alpha again */
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR1A1);
    GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_FALSE);           /* alpha test before the (unused) Z */
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
}

static void SetupVertexFormat(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GX_InvVtxCache();
}

void GpuNv2a_Init(void)
{
    Mtx ident;
    GXColor black = { 0, 0, 0, 0xFF };
    int i;

    VIDEO_Init();
    s_rmode = VIDEO_GetPreferredMode(NULL);
#if defined(SH_WII_PORT)
    /* 480p wherever the cable allows it, EURGB60 on PAL consoles (plan: one
     * 60 Hz 640x480 image for every region). */
    if (VIDEO_HaveComponentCable())
        s_rmode = &TVNtsc480Prog;
    else if (CONF_GetEuRGB60() > 0 && s_rmode->viTVMode >> 2 == VI_PAL)
        s_rmode = &TVEurgb60Hz480IntDf;
    s_tv169 = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);
    if (s_tv169) {
        s_rmode->viWidth = 678;          /* the usual widescreen overscan trim */
        s_rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - 678) / 2;
    }
#endif
    VIDEO_Configure(s_rmode);
    for (i = 0; i < 2; i++) {
        s_xfb[i] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
        VIDEO_ClearFrameBuffer(s_rmode, s_xfb[i], COLOR_BLACK);
    }
    VIDEO_SetNextFramebuffer(s_xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (s_rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    s_fifo = memalign(32, FIFO_SIZE);
    memset(s_fifo, 0, FIFO_SIZE);
    GX_Init(s_fifo, FIFO_SIZE);
    GX_SetCopyClear(black, GX_MAX_Z24);

    s_efbW = s_rmode->fbWidth;
    s_efbH = s_rmode->efbHeight;
    {
        f32 yscale = GX_GetYScaleFactor(s_rmode->efbHeight, s_rmode->xfbHeight);
        u32 xfbH   = GX_SetDispCopyYScale(yscale);
        GX_SetScissor(0, 0, s_efbW, s_efbH);
        GX_SetDispCopySrc(0, 0, s_efbW, s_efbH);
        GX_SetDispCopyDst(s_efbW, xfbH);
        GX_SetCopyFilter(s_rmode->aa, s_rmode->sample_pattern, GX_TRUE, s_rmode->vfilter);
        GX_SetFieldMode(s_rmode->field_rendering,
                        (s_rmode->viHeight == 2 * s_rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    }
    GX_SetPixelFmt(s_rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);

    SetupVertexFormat();
    SetupTev();
    guMtxIdentity(ident);
    GX_LoadPosMtxImm(ident, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_LoadTexMtxImm(ident, GX_IDENTITY, GX_MTX2x4);

    /* 4x4 white, alpha 0x80 like the D3D atlas' white slot (an untextured
     * ABR0 prim then blends at one half from the texture alpha). */
    for (i = 0; i < 16; i++) {
        s_whiteTex[i * 2]          = 0x80;   /* A */
        s_whiteTex[i * 2 + 1]      = 0xFF;   /* R */
        s_whiteTex[32 + i * 2]     = 0xFF;   /* G */
        s_whiteTex[32 + i * 2 + 1] = 0xFF;   /* B */
    }
    DCFlushRange(s_whiteTex, sizeof(s_whiteTex));

    /* CI8 pages and TLUTs are reserved here, at boot, as one block each:
     * allocating them on first use came too late -- by then the game had the
     * heap, the allocations failed, and those pages drew untextured (flat grey
     * sidewalks and buildings in Dolphin, 2026-11-11). */
    {
        u8* ci = (u8*)memalign(32, (size_t)CI_N * PAGE * PAGE);
        u8* tl = (u8*)memalign(32, (size_t)TL_N * 512);
        if (!ci || !tl)
            SH_DBG("[GPU] CI8 cache allocation FAILED (ci=%p tl=%p)", (void*)ci, (void*)tl);
        for (i = 0; i < CI_N; i++) s_ci[i].buf = ci ? ci + (size_t)i * PAGE * PAGE : NULL;
        for (i = 0; i < TL_N; i++) s_tl[i].buf = tl ? tl + (size_t)i * 512 : NULL;
    }
    for (i = 0; i < TEX_CACHE_N; i++) {
        s_cache[i].gx = memalign(32, PAGE * PAGE * 4);
        if (!s_cache[i].gx) {
            SH_DBG("[GPU] texture cache: only %d of %d pages allocated", i, TEX_CACHE_N);
            break;
        }
    }

    g_Nv2aContentX = 0;
    s_scW = g_Nv2aContentW; s_scH = g_Nv2aContentH;
    SelectView();
    LoadProjection();
    GpuNv2a_BindWhite();
    ApplyBlend();
    s_ready = 1;
    SH_DBG("[GPU] GX up: EFB %dx%d, XFB %dx%d, %s, TV %s, %d-vertex pool, %d RGBA8 pages, %d CI8 pages, %d TLUTs",
           s_efbW, s_efbH, s_rmode->fbWidth, s_rmode->xfbHeight,
           (s_rmode->viTVMode & VI_NON_INTERLACE) ? "progressive" : "interlaced",
           s_tv169 ? "16:9" : "4:3", GPU_POOL_VERTS, TEX_CACHE_N, CI_N, TL_N);
}

/* ---- texture conversion --------------------------------------------------- */

/* ARGB8888 (host-order u32, so bytes A R G B on this CPU) -> GX RGBA8 tiles:
 * 4x4 texel blocks of 64 bytes, the 16 AR pairs then the 16 GB pairs. w and h
 * must be multiples of 4 (every caller's are: pages are 256, FMV frames are
 * multiples of 16). */
static void ToRGBA8(u8* dst, const u32* src, const u32* pal, int w, int h)
{
    int bx, by, x, y;
    const u8* idx = (const u8*)src;
    for (by = 0; by < h; by += 4) {
        for (bx = 0; bx < w; bx += 4) {
            u8* ar = dst;
            u8* gb = dst + 32;
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    const int o = (by + y) * w + (bx + x);
                    const u32 c = pal ? pal[idx[o]] : src[o];
                    *ar++ = (u8)(c >> 24);
                    *ar++ = (u8)(c >> 16);
                    *gb++ = (u8)(c >> 8);
                    *gb++ = (u8)c;
                }
            }
            dst += 64;
        }
    }
}

static void UseTexture(void* gx, int w, int h, float us, float vs)
{
    if (s_curObjValid && s_curGx == gx && !s_curTl && s_uScale == us && s_vScale == vs)
        return;
    FlushBatch();
    s_curTl = NULL;                      /* direct colour, no TLUT */
    GX_InitTexObj(&s_curObj, gx, (u16)w, (u16)h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&s_curObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    s_curGx = gx;
    s_curObjValid = 1;
    s_curObjDirty = 1;
    s_uScale = us; s_vScale = vs;
}

static TexEnt* CacheFind(const void* page, const void* pal)
{
    int i;
    for (i = 0; i < TEX_CACHE_N; i++)
        if (s_cache[i].gx && s_cache[i].page == page && s_cache[i].pal == pal)
            return &s_cache[i];
    return NULL;
}

static TexEnt* CacheVictim(void)
{
    TexEnt* best = NULL;
    TexEnt* oldest = NULL;
    int i;
    for (i = 0; i < TEX_CACHE_N; i++) {
        TexEnt* e = &s_cache[i];
        if (!e->gx) continue;
        if (!e->page) return e;
        if (!oldest || e->lastUse < oldest->lastUse) oldest = e;
        if (e->lastUse == (unsigned)g_Nv2aFrameCount) continue;
        if (!best || e->lastUse < best->lastUse) best = e;
    }
    if (!best) {
        /* every page was used this frame: the GPU may still be reading the
         * victim from the FIFO, so let it drain before overwriting */
        static int s_told;
        if (!s_told++) SH_DBG("[GPU] texture cache exhausted within one frame (%d pages)", TEX_CACHE_N);
        FlushBatch();
        GX_DrawDone();
        best = oldest;
    }
    return best;
}

static TexEnt* CacheLookup(const void* page, const void* pal, int w, int h)
{
    const unsigned ep = PsxVram_DecodeEpoch();
    TexEnt*        e  = CacheFind(page, pal);

    if (!(e && e->epoch == ep)) {
        unsigned pg = PsxVram_DataGen(page);
        unsigned pl = pal ? PsxVram_DataGen(pal) : 0;
        if (e && pg && (!pal || pl) && e->pageGen == pg && e->palGen == pl) {
            e->epoch = ep;               /* re-verified, contents unchanged */
        } else {
            if (!e) e = CacheVictim();
            if (!e) return NULL;
            if (s_curObjValid && s_curGx == e->gx) {
                FlushBatch();
                s_curObjValid = 0;
            }
            e->page = page; e->pal = pal; e->pageGen = pg; e->palGen = pl;
            e->epoch = (pg && (!pal || pl)) ? ep : ep - 1u;
            e->w = w; e->h = h;
            ToRGBA8((u8*)e->gx, (const u32*)page, (const u32*)pal, w, h);
            DCFlushRange(e->gx, (u32)w * (u32)h * 4u);
            s_texInvalidate = 1;
            s_uploads++;
        }
    }
    e->lastUse = (unsigned)g_Nv2aFrameCount;
    return e;
}

/* Open-addressed cache keyed by the decoded data's pointer, re-verified by its
 * generation (PsxVram_DataGen) whenever the decode epoch moves -- the same
 * validity rule as CacheLookup. `fill` (re)builds buf from key. A slot used
 * this frame is only taken after the GPU has drained. */
#define PROBE 8
static KeyEnt* KeyLookup(KeyEnt* tab, int n, const void* key, int bytes,
                         void (*fill)(void* buf, const void* key), int* filled)
{
    const unsigned ep  = PsxVram_DecodeEpoch();
    const unsigned now = (unsigned)g_Nv2aFrameCount;
    unsigned h = (unsigned)(((uintptr_t)key >> 5) * 2654435761u) % (unsigned)n;
    KeyEnt* victim = NULL;
    int i;
    *filled = 0;
    for (i = 0; i < PROBE; i++) {
        KeyEnt* e = &tab[(h + i) % n];
        if (e->key == key && e->buf) {
            if (e->epoch != ep) {
                const unsigned g = PsxVram_DataGen(key);
                if (!(g && g == e->gen)) {
                    fill(e->buf, key);
                    DCFlushRange(e->buf, bytes);
                    e->gen = g;
                    *filled = 1;
                }
                e->epoch = g ? ep : ep - 1u;
            }
            e->lastUse = now;
            return e;
        }
        if (!e->key) { if (!victim || victim->key) victim = e; }
        else if (!victim || (victim->key && e->lastUse < victim->lastUse)) victim = e;
    }
    if (!victim->buf) {
        static int s_told;
        if (!s_told++) SH_DBG("[GPU] CI8/TLUT cache has no memory - drawing untextured");
        return NULL;
    }
    if (victim->key && victim->lastUse == now) {
        FlushBatch();
        GX_DrawDone();                   /* still queued this frame */
    }
    victim->key = key;
    fill(victim->buf, key);
    DCFlushRange(victim->buf, bytes);
    victim->gen     = PsxVram_DataGen(key);
    victim->epoch   = victim->gen ? ep : ep - 1u;
    victim->lastUse = now;
    *filled = 1;
    return victim;
}

/* 256x256 8-bit indices -> CI8 tiles (8x4 texels, 32 bytes, row-major). */
static void FillCI8(void* buf, const void* key)
{
    const u8* src = (const u8*)key;
    u8* dst = (u8*)buf;
    int bx, by, y;
    for (by = 0; by < PAGE; by += 4)
        for (bx = 0; bx < PAGE; bx += 8)
            for (y = 0; y < 4; y++, dst += 8)
                memcpy(dst, src + (by + y) * PAGE + bx, 8);
}

/* 256 ARGB8888 entries -> RGB5A3: opaque = 1:5:5:5; otherwise 0:3:4:4:4. The
 * three PSX alpha levels land on 0, 4/7 (STP) and opaque; ApplyAlphaPath
 * turns those back into exact 0 / 0.5 / 1 where it matters. */
static void FillTLUT(void* buf, const void* key)
{
    const u32* src = (const u32*)key;
    u16* dst = (u16*)buf;
    int i;
    for (i = 0; i < 256; i++) {
        const u32 c = src[i];
        const u32 a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        if (a == 0xFF)
            dst[i] = (u16)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        else
            dst[i] = (u16)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
    }
}

void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    KeyEnt *ci, *tl;
    int f1, f2;
    if (!page || !palette) { GpuNv2a_BindWhite(); return; }
    if (page == s_texAddr && palette == s_texPal)
        return;
    s_texAddr = page; s_texPal = palette; s_texW = s_texH = PAGE;
    s_binds++;
    if (!s_ready) return;
    ci = KeyLookup(s_ci, CI_N, page, PAGE * PAGE, FillCI8, &f1);
    tl = KeyLookup(s_tl, TL_N, palette, 256 * 2, FillTLUT, &f2);
    if (!ci || !tl) { GpuNv2a_BindWhite(); return; }
    if (f1) { s_texInvalidate = 1; s_uploads++; }
    if (s_curObjValid && s_curGx == ci->buf && s_curTl == tl->buf)
        return;
    FlushBatch();
    /* rotate the 16 TLUT slots so a reload never lands on one a queued draw
     * may still be reading */
    s_tlutSlot = (s_tlutSlot + 1) & 15;
    GX_InitTlutObj(&s_curTlut, tl->buf, GX_TL_RGB5A3, 256);
    GX_InitTexObjCI(&s_curObj, ci->buf, PAGE, PAGE, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE,
                    GX_TLUT0 + s_tlutSlot);
    GX_InitTexObjLOD(&s_curObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    s_curGx = ci->buf; s_curTl = tl->buf;
    s_curObjValid = 1; s_curObjDirty = 1; s_tlutDirty = 1;
    s_uScale = 1.0f; s_vScale = 1.0f;    /* paletted UVs arrive /256 already */
}

static void BindBig(const void* addr, int w, int h)
{
    const int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
    const int bytes = pw * ph * 4;
    int i = s_bigNext & 1;
    s_bigNext++;
    FlushBatch();
    if (s_bigUse[i] == (unsigned)g_Nv2aFrameCount)
        GX_DrawDone();                   /* this buffer is still queued this frame */
    if (s_bigCap[i] < bytes) {
        free(s_bigBuf[i]);
        s_bigBuf[i] = memalign(32, bytes);
        s_bigCap[i] = s_bigBuf[i] ? bytes : 0;
        if (!s_bigBuf[i]) { SH_DBG("[GPU] oversize texture %dx%d: out of memory", w, h); GpuNv2a_BindWhite(); return; }
        SH_DBG("[GPU] oversize texture %dx%d: buffer %d", w, h, i);
    }
    if (pw == w && ph == h) {
        ToRGBA8((u8*)s_bigBuf[i], (const u32*)addr, NULL, w, h);
    } else {
        /* pad to whole 4x4 blocks: stage through a temporary */
        u32* tmp = (u32*)malloc((size_t)pw * ph * 4);
        int y;
        if (!tmp) { GpuNv2a_BindWhite(); return; }
        memset(tmp, 0, (size_t)pw * ph * 4);
        for (y = 0; y < h; y++)
            memcpy(tmp + (size_t)y * pw, (const u32*)addr + (size_t)y * w, (size_t)w * 4);
        ToRGBA8((u8*)s_bigBuf[i], tmp, NULL, pw, ph);
        free(tmp);
    }
    DCFlushRange(s_bigBuf[i], bytes);
    s_texInvalidate = 1;
    s_uploads++;
    s_bigUse[i] = (unsigned)g_Nv2aFrameCount;
    s_curObjValid = 0;
    UseTexture(s_bigBuf[i], pw, ph, 1.0f / (float)pw, 1.0f / (float)ph);
}

void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    TexEnt* e;
    if (addr == s_texAddr && s_texPal == NULL && w == s_texW && h == s_texH)
        return;
    s_texAddr = addr; s_texPal = NULL; s_texW = w; s_texH = h;
    s_binds++;
    if (!s_ready) return;
    if (!addr || w <= 0 || h <= 0) { GpuNv2a_BindWhite(); return; }
    if (w > PAGE || h > PAGE || (w & 3) || (h & 3)) { BindBig(addr, w, h); return; }
    e = CacheLookup(addr, NULL, w, h);
    if (e) UseTexture(e->gx, w, h, 1.0f / (float)w, 1.0f / (float)h);
}

void GpuNv2a_BindWhite(void)
{
    s_texAddr = NULL; s_texPal = NULL;
    UseTexture(s_whiteTex, 4, 4, 0.0f, 0.0f);
}

void* GpuNv2a_AllocTexMem(int bytes)
{
    void* p;
    if (bytes <= 0) return NULL;
    p = memalign(32, (size_t)bytes);
    if (!p) SH_DBG("[GPU] AllocTexMem(%d) FAILED", bytes);
    return p;
}

/* ---- state ---------------------------------------------------------------- */

static void ApplyAlphaPath(void)
{
    if (s_blendMode == 1) {
        /* ABR0: exact 0 / 0.5 / 1 from the texel's alpha level, whatever
         * precision the texture format kept (RGB5A3 has STP at 4/7):
         *   stage 0: 0.5 where tex.a > 0
         *   stage 1: + itself where tex.a > 3/4  -> 1.0 for opaque texels */
        GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1_2);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_KONST, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_3_4);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_TEXA, GX_CA_KONST, GX_CA_APREV, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        return;
    }
    /* stage 1 passes alpha through in every other mode */
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    {
        /* konst where tex.a > 0, else 0 (the alpha test then kills it) */
        GX_SetTevKAlphaSel(GX_TEVSTAGE0, s_blendMode == 4 ? GX_TEV_KASEL_1_4 : GX_TEV_KASEL_1);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_KONST, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
}

static void ApplyBlend(void)
{
    switch (s_blendMode) {
    default:
    case 0: GX_SetBlendMode(GX_BM_NONE,     GX_BL_ONE,      GX_BL_ZERO,        GX_LO_CLEAR); break;
    case 1: GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
    case 2: GX_SetBlendMode(GX_BM_BLEND,    GX_BL_ONE,      GX_BL_ONE,         GX_LO_CLEAR); break;
    case 3: GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE,      GX_BL_ONE,         GX_LO_CLEAR); break;
    case 4: GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_ONE,         GX_LO_CLEAR); break;
    }
    ApplyAlphaPath();
}

static void ApplyScissor(void)
{
    if (!s_scFull && g_Nv2aContentW > 0 && g_Nv2aContentH > 0) {
        const float ppu = (float)s_vpW / s_viewW;
        int l = s_vpX + (int)((s_viewX + (float)s_scX) * ppu);
        int t = s_scY * s_efbH / g_Nv2aContentH;
        int w = (int)((float)s_scW * ppu);
        int h = s_scH * s_efbH / g_Nv2aContentH;
        if (l < 0) { w += l; l = 0; }
        if (t < 0) { h += t; t = 0; }
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        GX_SetScissor((u32)l, (u32)t, (u32)w, (u32)h);
    } else {
        GX_SetScissor((u32)s_vpX, 0, (u32)s_vpW, (u32)s_efbH);
    }
}

void GpuNv2a_SetBlendMode(int mode)
{
    if (mode == s_blendMode) return;
    FlushBatch();
    s_blendMode = mode;
    ApplyBlend();
}

void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    const int full = (w <= 0 || h <= 0);
    if (full) { x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH; }
    if (full == s_scFull && x == s_scX && y == s_scY && w == s_scW && h == s_scH)
        return;
    FlushBatch();
    s_scX = x; s_scY = y; s_scW = w; s_scH = h; s_scFull = full;
    ApplyScissor();
}

/* No depth buffer use: the PSX draws in OT order, as on the D3D backend. */
void GpuNv2a_SetDepthWrite(int enable) { (void)enable; }
void GpuNv2a_SetDepthTest(int enable)  { (void)enable; }
void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* ---- drawing -------------------------------------------------------------- */

static inline u8 U8(float f)
{
    return f <= 0.0f ? 0 : (f >= 1.0f ? 255 : (u8)(f * 255.0f + 0.5f));
}

static void FlushBatch(void)
{
    int count = s_poolUsed - s_flushStart;
    unsigned long long t0;

    if (count <= 0 || !s_ready) {
        s_flushStart = s_poolUsed;
        return;
    }
    t0 = mftb();
    if (s_texInvalidate) { GX_InvalidateTexAll(); s_texInvalidate = 0; s_curObjDirty = 1; }
    if (s_tlutDirty && s_curTl) { GX_LoadTlut(&s_curTlut, GX_TLUT0 + s_tlutSlot); s_tlutDirty = 0; }
    if (s_curObjDirty && s_curObjValid) { GX_LoadTexObj(&s_curObj, GX_TEXMAP0); s_curObjDirty = 0; }

    while (count >= 3) {
        int n = count > 65532 ? 65532 : count;
        int i;
        n -= n % 3;
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)n);
        for (i = 0; i < n; i++) {
            const ShVertex* v = &s_pool[s_flushStart + i];
            const float w = v->pos[3] > 0.0f ? v->pos[3] : 1.0f;
            GX_Position3f32(v->pos[0] * w, v->pos[1] * w, -w);
            GX_Color4u8(U8(v->col[0]), U8(v->col[1]), U8(v->col[2]), U8(v->col[3]));
            GX_Color4u8(U8(v->spec[0]), U8(v->spec[1]), U8(v->spec[2]), 0);
            GX_TexCoord2f32(v->tex[0] * s_uScale, v->tex[1] * s_vScale);
        }
        GX_End();
        s_draws++;
        s_flushStart += n;
        count -= n;
    }
    s_flushStart = s_poolUsed;
    g_Nv2aDrawCycles += mftb() - t0;
}

void GpuNv2a_FlushBatch(void) { FlushBatch(); }

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;
    if (count <= 0 || count > GPU_POOL_VERTS)
        return NULL;
    if (s_poolUsed + count > GPU_POOL_VERTS) {
        FlushBatch();
        /* GX_Begin copies vertices into the FIFO, so the pool is free again */
        s_poolUsed = s_flushStart = 0;
    }
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

/* One untextured, unblended quad over the content area, in content units. */
static void FillContent(unsigned argb)
{
    ShVertex* v = GpuNv2a_BatchAlloc(6);
    float x0 = -s_viewX, x1 = s_viewW - s_viewX, y1 = (float)g_Nv2aContentH;
    int i;
    if (!v) return;
    for (i = 0; i < 6; i++) {
        const int r = (i == 1 || i == 2 || i == 4), b = (i == 2 || i == 4 || i == 5);
        memset(&v[i], 0, sizeof(v[i]));
        v[i].pos[0] = r ? x1 : x0;
        v[i].pos[1] = b ? y1 : 0.0f;
        v[i].pos[3] = 1.0f;
        v[i].col[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        v[i].col[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        v[i].col[2] = (float)(argb & 0xFF) / 255.0f;
        v[i].col[3] = 1.0f;
    }
}

void GpuXenos_DebugClearOverride(unsigned argb, int enable)
{
    s_clearOverride = argb;
    s_clearOverrideOn = enable;
}

void GpuXenos_DebugFbProbe(const char* tag)
{
    SH_DBG("[FBPROBE] %s: (no CPU readback on GX) frame=%u", tag, s_frame);
}

void GpuNv2a_FrameBegin(void)
{
    int saveBlend = s_blendMode;
    g_Nv2aFrameCount++;
    s_poolUsed = s_flushStart = 0;
    s_draws = s_verts = s_binds = s_uploads = 0;
    if (!s_ready) return;

    SelectView();
    LoadProjection();
    s_scFull = 1;
    ApplyScissor();
    /* the EFB copy cleared the frame to black; fill the 4:3 or Hor+ content
     * area with the PSX draw-env background (fog colour in game) */
    s_blendMode = -1;
    GpuNv2a_SetBlendMode(0);
    GpuNv2a_BindWhite();
    FillContent(s_clearOverrideOn ? s_clearOverride : GpuXbox_GetClearColor());
    FlushBatch();
    if (saveBlend != 0) GpuNv2a_SetBlendMode(saveBlend);
}

/* ---- freeze frame (pause / save backgrounds) ------------------------------ */
static void* s_freezeTex;
static int   s_freezeHave;

static void FreezeCaptureBeforePresent(void)
{
#if defined(SH_GC_PORT)
    return;                              /* 1.2 MB: not on 24 MB */
#else
    if (!g_PsxPresentLastFrame || s_freezeHave)
        return;
    if (!s_freezeTex)
        s_freezeTex = memalign(32, (size_t)s_efbW * s_efbH * 4);
    if (!s_freezeTex)
        return;
    GX_SetTexCopySrc(0, 0, (u16)s_efbW, (u16)s_efbH);
    GX_SetTexCopyDst((u16)s_efbW, (u16)s_efbH, GX_TF_RGBA8, GX_FALSE);
    DCInvalidateRange(s_freezeTex, (u32)s_efbW * s_efbH * 4);
    GX_CopyTex(s_freezeTex, GX_FALSE);
    GX_PixModeSync();
    s_freezeHave = 1;
    SH_DBG("[FREEZE] captured frame %u", s_frame);
#endif
}

int  GpuNv2a_FreezeCapture(void) { return s_freezeHave; }
void GpuNv2a_FreezeRelease(void) { s_freezeHave = 0; }
void GpuNv2a_FreezeReserve(void) { }

void GpuNv2a_FreezeBlit(void)
{
    ShVertex* v;
    int i;
    if (!s_freezeHave || !s_freezeTex || !s_ready)
        return;
    FlushBatch();
    UseTexture(s_freezeTex, s_efbW, s_efbH, 1.0f, 1.0f);
    s_texInvalidate = 1;
    {
        int saveBlend = s_blendMode;
        GpuNv2a_SetBlendMode(0);
        v = GpuNv2a_BatchAlloc(6);
        if (v) {
            const float x0 = -s_viewX, x1 = s_viewW - s_viewX, y1 = (float)g_Nv2aContentH;
            /* the capture is the whole EFB; sample the viewport part of it */
            const float u0 = (float)s_vpX / (float)s_efbW, u1 = (float)(s_vpX + s_vpW) / (float)s_efbW;
            for (i = 0; i < 6; i++) {
                const int r = (i == 1 || i == 2 || i == 4), b = (i == 2 || i == 4 || i == 5);
                memset(&v[i], 0, sizeof(v[i]));
                v[i].pos[0] = r ? x1 : x0;
                v[i].pos[1] = b ? y1 : 0.0f;
                v[i].pos[3] = 1.0f;
                v[i].col[0] = v[i].col[1] = v[i].col[2] = v[i].col[3] = 1.0f;
                v[i].tex[0] = r ? u1 : u0;
                v[i].tex[1] = b ? 1.0f : 0.0f;
            }
        }
        FlushBatch();
        if (saveBlend != 0) GpuNv2a_SetBlendMode(saveBlend);
    }
    s_curObjValid = 0;
    GpuNv2a_BindWhite();
}

void GpuNv2a_FrameEnd(void)
{
    FlushBatch();
    if (!s_ready) { s_frame++; return; }

    FreezeCaptureBeforePresent();
    GX_SetScissor(0, 0, (u32)s_efbW, (u32)s_efbH);
    GX_DrawDone();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(s_xfb[s_xfbIdx], GX_TRUE);   /* also clears the EFB to black */
    GX_Flush();
    VIDEO_SetNextFramebuffer(s_xfb[s_xfbIdx]);
    VIDEO_Flush();
    s_xfbIdx ^= 1;
    Pgxp_FrameAdvance();

    if ((s_frame % 60) == 0)
        SH_DBG("[GPU] frame=%u verts=%u draws=%u binds=%u uploads=%u",
               s_frame, s_verts, s_draws, s_binds, s_uploads);
    s_frame++;
}

void GpuNv2a_WaitVbl(void)
{
    VIDEO_WaitVSync();
}

void GpuNv2a_DrainGpu(void)
{
    FlushBatch();
    if (s_ready) GX_DrawDone();
}

/* No CPU-visible framebuffer here (same as the D3D backend); callers fall back. */
const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
{
    static int s_warned;
    (void)fromLastQueued;
    if (w) *w = 0;
    if (h) *h = 0;
    if (pitchBytes) *pitchBytes = 0;
    if (!s_warned++) SH_DBG("[GPU] ReadbackSurface unavailable on GX");
    return NULL;
}

int GpuNv2a_Ms(void) { return (int)(mftb() / (PPC_TIMEBASE_FREQ / 1000)); }
