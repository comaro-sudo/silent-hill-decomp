/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gamepad_screen.c - what the Wii U GamePad shows while the game is on the TV
 * (docs/plans/wii-u-port-plan.md, "GamePad features").
 *
 *   +-------------------------------------------------- 854 ---+
 *   | FINE / CAUTION / DANGER   ~~~~/\~~~~ ECG sweep ~~~~  [TV] |  health strip
 *   |                                                           |
 *   |              last-viewed paper map (4:3)                  |
 *   |                                                           |
 *   +-----------------------------------------------------------+
 *
 * Health strip: Harry's condition, always visible during play, in the look of
 *   the inventory's status display (Gfx_Inventory_HealthStatusDraw): same
 *   thresholds (Danger < 10, Caution < 50), same colours (red, orange, yellow-
 *   green -> green), an ECG trace whose beat quickens as health drops. No
 *   number, as in the original. On by default; tap it to hide it (the original
 *   keeps health hidden on purpose) -- remembered with a `gamepad_nohealth`
 *   file next to the disc image.
 * Map: while the paper map screen is open on the TV, the TV image is copied
 *   down to the GamePad every half second, so the GamePad keeps the map as it
 *   was last seen (markings and Harry's marker included). It is a snapshot, not
 *   a live map: the game only draws the map inside the map screen.
 * [TV] (top right, tap): Off-TV play -- moves the game to the GamePad and
 *   blanks the TV; tap again to go back.
 *
 * Drawn with plain SDL rectangles and lines plus a built-in 5x7 font, so it
 * needs no assets.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

#include "sh_log.h"

extern int WiiU_ShowHealth(void);       /* gamepad_game.c */
extern int WiiU_HealthStage(void);
extern int WiiU_PlayerHealthQ12(void);
extern int WiiU_InMapScreen(void);
extern int WiiU_WeaponAmmo(int* loaded, int* capacity);
extern int WiiU_PadMapZoom(void);
extern const char* Sh360Fs_DataRoot(void);

#define DRC_W 854
#define DRC_H 480
#define STRIP_H 56
#define TVBTN_X (DRC_W - 92)
#define TVBTN_Y 8
#define TVBTN_W 84
#define TVBTN_H 44
#define MAP_W 480
#define MAP_H 360

#ifndef SH_WIIU_GX2
static SDL_Texture* s_mapTex;
#endif
static int          s_haveMap;
static int          s_hideHealth;
static int          s_offTvRequest;
static int          s_offTvNow;          /* which draw path ran last frame */
static unsigned     s_frame;
static float        s_phase;
static int          s_sweepX;
static short        s_trace[DRC_W];

/* ---- 5x7 font ------------------------------------------------------------- */
static const unsigned char kFont[][7] = {
    /* A */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* D */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* J */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* N */ {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    /* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* V */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* W */ {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* X */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* Y */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Z */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 2 */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 3 */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* - */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* . */ {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    /* / */ {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
};

static const unsigned char* Glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return kFont[c - 'A'];
    if (c >= '0' && c <= '9') return kFont[26 + c - '0'];
    if (c == '-') return kFont[36];
    if (c == '.') return kFont[37];
    if (c == '/') return kFont[38];
    return NULL;
}

/* ---- drawing back end -------------------------------------------------------
 * SDL build: the GamePad's own SDL renderer. GX2 build (SH_WIIU_GX2): the
 * Gx2Ui_* calls in gpu_gx2.c, which batch everything into one draw. */
#ifdef SH_WIIU_GX2
extern void Gx2Ui_Color(int r, int g, int b, int a);
extern void Gx2Ui_Fill(float x, float y, float w, float h);
extern void Gx2Ui_Rect(int x, int y, int w, int h);
extern void Gx2Ui_Line(int x0, int y0, int x1, int y1);
extern void Gx2Ui_Map(int x, int y, int w, int h);
extern int  Gx2Ui_LiveMapUpload(const unsigned* argb, int w, int h);
extern void Gx2Ui_LiveMap(int dx, int dy, int dw, int dh, float u0, float v0, float u1, float v1);
extern void Gx2Ui_Tri(float x0, float y0, float x1, float y1, float x2, float y2);
static void Dc_Color(SDL_Renderer* r, int cr, int cg, int cb, int ca) { (void)r; Gx2Ui_Color(cr, cg, cb, ca); }
static void Dc_Fill(SDL_Renderer* r, int x, int y, int w, int h) { (void)r; Gx2Ui_Fill((float)x, (float)y, (float)w, (float)h); }
static void Dc_Rect(SDL_Renderer* r, int x, int y, int w, int h) { (void)r; Gx2Ui_Rect(x, y, w, h); }
static void Dc_Line(SDL_Renderer* r, int x0, int y0, int x1, int y1) { (void)r; Gx2Ui_Line(x0, y0, x1, y1); }
#else
static void Dc_Color(SDL_Renderer* r, int cr, int cg, int cb, int ca) { SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)ca); }
static void Dc_Fill(SDL_Renderer* r, int x, int y, int w, int h) { SDL_Rect q = { x, y, w, h }; SDL_RenderFillRect(r, &q); }
static void Dc_Rect(SDL_Renderer* r, int x, int y, int w, int h) { SDL_Rect q = { x, y, w, h }; SDL_RenderDrawRect(r, &q); }
static void Dc_Line(SDL_Renderer* r, int x0, int y0, int x1, int y1) { SDL_RenderDrawLine(r, x0, y0, x1, y1); }
#endif

static void Text(SDL_Renderer* r, int x, int y, int scale, const char* s)
{
    for (; *s; s++, x += 6 * scale) {
        const unsigned char* g = Glyph(*s);
        int row, col;
        if (!g) continue;
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    Dc_Fill(r, x + col * scale, y + row * scale, scale, scale);
    }
}

static int TextW(const char* s, int scale) { return (int)strlen(s) * 6 * scale - scale; }

/* ---- persistent "hide health" -------------------------------------------- */
static void FlagPath(char* out, int n) { snprintf(out, n, "%sgamepad_nohealth", Sh360Fs_DataRoot()); }

static void LoadHide(void)
{
    char p[192];
    FILE* f;
    FlagPath(p, sizeof(p));
    f = fopen(p, "rb");
    s_hideHealth = f ? 1 : 0;
    if (f) fclose(f);
}

static void SaveHide(void)
{
    char p[192];
    FlagPath(p, sizeof(p));
    if (s_hideHealth) {
        FILE* f = fopen(p, "wb");
        if (f) fclose(f);
    } else {
        remove(p);
    }
}

/* ---- API used by gpu_sdl_wiiu.c and pad_wiiu.c ---------------------------- */

void GamepadScreen_Init(SDL_Renderer* drc)
{
#ifndef SH_WIIU_GX2
    s_mapTex = SDL_CreateTexture(drc, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, MAP_W, MAP_H);
    if (s_mapTex) SDL_SetTextureScaleMode(s_mapTex, SDL_ScaleModeLinear);
#else
    (void)drc;
#endif
    LoadHide();
    memset(s_trace, 0, sizeof(s_trace));
    SH_DBG("[DRC] GamePad screen up (health strip %s)", s_hideHealth ? "hidden" : "on");
}

/* Touch press in GamePad pixels (pad_wiiu.c, on the press edge). */
void GamepadScreen_Touch(int x, int y)
{
    if (x >= TVBTN_X && x < TVBTN_X + TVBTN_W && y >= TVBTN_Y && y < TVBTN_Y + TVBTN_H) {
        s_offTvRequest = 1;
        return;
    }
    if (y < STRIP_H) {
        s_hideHealth = !s_hideHealth;
        SaveHide();
        SH_DBG("[DRC] health strip %s", s_hideHealth ? "hidden" : "shown");
    }
}

int GamepadScreen_OffTv(void) { return s_offTvNow; }

int GamepadScreen_TakeOffTvToggle(void)
{
    const int r = s_offTvRequest;
    s_offTvRequest = 0;
    return r;
}

int GamepadScreen_WantMapCapture(void)
{
    return WiiU_InMapScreen() && (s_frame % 30) == 15;
}

#ifdef SH_WIIU_GX2
/* GX2: the backend copies the TV image to a texture on the GPU itself. */
void GamepadScreen_MapCaptured(void)
{
    if (!s_haveMap) SH_DBG("[DRC] first map snapshot");
    s_haveMap = 1;
}
#else
/* Copy the TV's 4:3 area down to MAP_W x MAP_H (called before the TV present). */
void GamepadScreen_CaptureMap(SDL_Renderer* tv, const SDL_Rect* src)
{
    static unsigned* s_big;
    static unsigned  s_small[MAP_W * MAP_H];
    int x, y;
    if (!s_mapTex || src->w <= 0 || src->h <= 0) return;
    if (!s_big) s_big = (unsigned*)malloc((size_t)src->w * src->h * 4);
    if (!s_big) return;
    if (SDL_RenderReadPixels(tv, src, SDL_PIXELFORMAT_ARGB8888, s_big, src->w * 4) != 0) {
        static int s_told;
        if (!s_told++) SH_DBG("[DRC] map capture: ReadPixels failed: %s", SDL_GetError());
        return;
    }
    for (y = 0; y < MAP_H; y++) {
        const unsigned* row = s_big + (size_t)(y * src->h / MAP_H) * src->w;
        for (x = 0; x < MAP_W; x++)
            s_small[y * MAP_W + x] = row[x * src->w / MAP_W] | 0xFF000000u;
    }
    SDL_UpdateTexture(s_mapTex, NULL, s_small, MAP_W * 4);
    if (!s_haveMap) SH_DBG("[DRC] first map snapshot");
    s_haveMap = 1;
}
#endif

/* One heartbeat, phase 0..1 -> -1..1 (P wave, QRS spike, T wave). */
static float Beat(float p)
{
    if (p < 0.08f) return 0.12f * SDL_sinf(p / 0.08f * 3.14159f);
    if (p < 0.12f) return 0.0f;
    if (p < 0.14f) return -0.15f;
    if (p < 0.17f) return 1.0f;
    if (p < 0.20f) return -0.35f;
    if (p < 0.32f) return 0.0f;
    if (p < 0.46f) return 0.22f * SDL_sinf((p - 0.32f) / 0.14f * 3.14159f);
    return 0.0f;
}

static void StageColour(int stage, int hQ12, Uint8* r, Uint8* g, Uint8* b)
{
    if (stage == 3)      { *r = 0xFF; *g = 0x00; *b = 0; }
    else if (stage == 2) { *r = 0xFF; *g = 0xA0; *b = 0; }
    else {
        /* 50 -> 100: A0FF00 -> 00FF00, the inventory's two green stops */
        int t = (hQ12 - 50 * 4096) * 255 / (50 * 4096);
        if (t < 0) t = 0;
        if (t > 255) t = 255;
        *r = (Uint8)(0xA0 * (255 - t) / 255); *g = 0xFF; *b = 0;
    }
}

static void Polyline(SDL_Renderer* r, const int* px, const int* py, int n, int dy)
{
    int i;
    for (i = 0; i + 1 < n; i++) Dc_Line(r, px[i], py[i] + dy, px[i + 1], py[i + 1] + dy);
}

static void DrawHealth(SDL_Renderer* r)
{
    /* No word when healthy; the status stays deliberately quiet (dim, thin). */
    static const char* const kLabel[4] = { "", "", "CAUTION", "DANGER" };
    /* beats per second and sweep speed per stage, quicker when hurt */
    static const float kBps[4]   = { 1.0f, 1.0f, 1.5f, 2.3f };
    const int stage = WiiU_HealthStage();
    const int hq    = WiiU_PlayerHealthQ12();
    const int x0 = 120, x1 = TVBTN_X - 150, mid = STRIP_H / 2, amp = 14;
    Uint8 cr, cg, cb;
    int i, steps;
    int px[DRC_W], py[DRC_W];
    int n = 0;

    StageColour(stage, hq, &cr, &cg, &cb);
    cr = (Uint8)(cr * 5 / 10); cg = (Uint8)(cg * 5 / 10); cb = (Uint8)(cb * 5 / 10);

    /* advance the sweep: 4 px per frame, sample the beat at each new column */
    for (steps = 0; steps < 4; steps++) {
        float v;
        s_phase += kBps[stage] / 60.0f / 4.0f;
        if (s_phase >= 1.0f) s_phase -= 1.0f;
        v = Beat(s_phase);
        if (stage == 3) v *= 0.6f + 0.4f * (float)(rand() % 100) / 100.0f;   /* weak, irregular */
        s_sweepX++;
        if (s_sweepX >= x1) s_sweepX = x0;
        s_trace[s_sweepX] = (short)(mid - v * amp);
    }

#ifndef SH_WIIU_GX2
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
#endif
    Dc_Color(r, 8, 8, 8, 255);
    Dc_Fill(r, 0, 0, DRC_W, STRIP_H);
    Dc_Color(r, cr / 6, cg / 6, cb / 6, 255);
    Dc_Line(r, x0, mid, x1, mid);

    Dc_Color(r, cr, cg, cb, 255);
    Text(r, 16, mid - 7, 2, kLabel[stage]);
    /* the trace, with a gap just ahead of the sweep like a monitor */
    for (i = x0; i < x1; i++) {
        if (i > s_sweepX && i < s_sweepX + 24) {
            Polyline(r, px, py, n, 0);
            n = 0;
            continue;
        }
        if (s_trace[i] == 0) s_trace[i] = (short)mid;
        px[n] = i; py[n] = s_trace[i]; n++;
    }
    Polyline(r, px, py, n, 0);

    /* loaded / magazine of the equipped gun, as quiet as the trace */
    {
        int loaded, cap;
        if (WiiU_WeaponAmmo(&loaded, &cap)) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d/%d", loaded, cap);
            Dc_Color(r, 150, 150, 150, 255);
            Text(r, TVBTN_X - 24 - TextW(buf, 3), mid - 10, 3, buf);
        }
    }
}

static void DrawTvButton(SDL_Renderer* r, int offTv)
{
#ifndef SH_WIIU_GX2
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
#endif
    Dc_Color(r, 20, 20, 20, 200);
    Dc_Fill(r, TVBTN_X, TVBTN_Y, TVBTN_W, TVBTN_H);
    Dc_Color(r, 160, 160, 160, 255);
    Dc_Rect(r, TVBTN_X, TVBTN_Y, TVBTN_W, TVBTN_H);
    Text(r, TVBTN_X + (TVBTN_W - TextW(offTv ? "TV" : "PAD", 3)) / 2, TVBTN_Y + 12, 3, offTv ? "TV" : "PAD");
}

#ifdef SH_WIIU_GX2
/* Live map: the area's paper map with its markings (pc_minimap.c composes it),
 * at 2x so its full width fits, scrolled to keep Harry in view, and his arrow
 * on it. Replaces the TV minimap, which is off by default on Wii U. */
extern int Pc_LiveMapState(int* x, int* y, int* ang, unsigned* ver);
extern int Pc_LiveMapCompose(unsigned* dst, int cap, int* outW, int* outH);

#define LIVE_CAP (320 * 480)
static unsigned s_liveBuf[LIVE_CAP];
static unsigned s_liveUploaded;
static int      s_liveUpOk, s_liveW, s_liveH;

static int DrawLiveMap(void)
{
    const int top = STRIP_H + 6, dh = DRC_H - top;
    int   hx, hy, ang, dw, dx, i;
    unsigned ver;
    float winH, v0, sx, sy, ca, sa;
    static const float lx[3] = { 0.0f, -3.0f, 3.0f }, ly[3] = { -5.0f, 3.0f, 3.0f };
    float vx[3], vy[3];

    if (!Pc_LiveMapState(&hx, &hy, &ang, &ver)) return 0;
    if (!s_liveUpOk || ver != s_liveUploaded) {
        int w = 0, h = 0;
        s_liveUpOk = Pc_LiveMapCompose(s_liveBuf, LIVE_CAP, &w, &h) && Gx2Ui_LiveMapUpload(s_liveBuf, w, h);
        if (!s_liveUpOk) return 0;
        if (!s_liveUploaded) SH_DBG("[DRC] live map up: %dx%d", w, h);
        s_liveUploaded = ver; s_liveW = w; s_liveH = h;
    }

    /* image pixels are 320x480 in map units whatever the decode size. The 480
     * rows are the map screen's interlaced lines of a 4:3 picture, so a row is
     * half as tall as a column is wide: 4x across, 2x down. The window follows
     * Harry on both axes and stops at the map's edges. */
    {
        /* zoom applies to the 4:3 picture: a row is half a column */
        int         zoom = WiiU_PadMapZoom();
        const float zx = (float)(zoom < 1 ? 1 : zoom), zy = zx / 2.0f;
        float u0, u1, v1, dyf, dhf;
        /* each axis: centre the map when it fits, else scroll with Harry */
        if (320.0f * zx <= (float)DRC_W) {
            u0 = 0.0f; u1 = 320.0f; dw = (int)(320.0f * zx); dx = (DRC_W - dw) / 2;
        } else {
            const float winW = (float)DRC_W / zx;
            u0 = (float)hx - winW / 2.0f;
            if (u0 > 320.0f - winW) u0 = 320.0f - winW;
            if (u0 < 0.0f) u0 = 0.0f;
            u1 = u0 + winW; dx = 0; dw = DRC_W;
        }
        if (480.0f * zy <= (float)dh) {
            v0 = 0.0f; v1 = 480.0f; dhf = 480.0f * zy; dyf = (float)top + ((float)dh - dhf) / 2.0f;
        } else {
            winH = (float)dh / zy;
            v0 = (float)hy - winH / 2.0f;
            if (v0 > 480.0f - winH) v0 = 480.0f - winH;
            if (v0 < 0.0f) v0 = 0.0f;
            v1 = v0 + winH; dhf = (float)dh; dyf = (float)top;
        }
        Gx2Ui_LiveMap(dx, (int)dyf, dw, (int)dhf, u0 * (float)s_liveW / 320.0f, v0 * (float)s_liveH / 480.0f,
                      u1 * (float)s_liveW / 320.0f, v1 * (float)s_liveH / 480.0f);
        sx = (float)dx + ((float)hx - u0) * zx;
        sy = dyf + ((float)hy - v0) * zy;
    }
    ca = SDL_cosf((float)ang * (6.2831853f / 4096.0f));
    sa = SDL_sinf((float)ang * (6.2831853f / 4096.0f));
    for (i = 0; i < 3; i++) {
        vx[i] = sx + (lx[i] * ca - ly[i] * sa) * 4.0f;
        vy[i] = sy + (lx[i] * sa + ly[i] * ca) * 4.0f;
    }
    /* black edge, then the minimap's yellow */
    Gx2Ui_Color(0, 0, 0, 255);
    Gx2Ui_Tri(vx[0] + (vx[0] - sx) * 0.35f, vy[0] + (vy[0] - sy) * 0.35f,
              vx[1] + (vx[1] - sx) * 0.35f, vy[1] + (vy[1] - sy) * 0.35f,
              vx[2] + (vx[2] - sx) * 0.35f, vy[2] + (vy[2] - sy) * 0.35f);
    Gx2Ui_Color(255, 216, 48, 255);
    Gx2Ui_Tri(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
    return 1;
}
#endif

/* Achievement unlock, shown here instead of on the TV (pc_local_ach.c keeps the
 * queue and the ~4 s timing). A band over the top of the map area. */
extern int         Pc_LocalAch_PadToast(void);
extern const char* Pc_LocalAch_Title(int idx);

static void DrawAchToast(SDL_Renderer* r)
{
    const int idx = Pc_LocalAch_PadToast();
    const int y = STRIP_H + 14, h = 74, x = 60, w = DRC_W - 120;
    const char* t;
    if (idx < 0) return;
    t = Pc_LocalAch_Title(idx);
#ifndef SH_WIIU_GX2
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
#endif
    Dc_Color(r, 8, 8, 8, 235);
    Dc_Fill(r, x, y, w, h);
    Dc_Color(r, 200, 170, 60, 255);
    Dc_Fill(r, x, y, w, 2);
    Dc_Fill(r, x, y + h - 2, w, 2);
    Text(r, x + 18, y + 14, 2, "ACHIEVEMENT UNLOCKED");
    Dc_Color(r, 230, 230, 230, 255);
    Text(r, x + 18, y + 40, 3, t);
}

/* Whole GamePad frame while the game is on the TV. */
void GamepadScreen_Draw(SDL_Renderer* r)
{
    s_frame++;
    s_offTvNow = 0;
#ifndef SH_WIIU_GX2
    SDL_RenderSetClipRect(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
#endif

    if (WiiU_ShowHealth() && !s_hideHealth)
        DrawHealth(r);
    else if (!WiiU_ShowHealth()) {
        Dc_Color(r, 90, 90, 90, 255);
        Text(r, (DRC_W - TextW("SILENT HILL", 3)) / 2, 18, 3, "SILENT HILL");
    }

#ifdef SH_WIIU_GX2
    if (DrawLiveMap()) {
    } else if (s_haveMap) {
        const int dh = DRC_H - STRIP_H - 6, dw = dh * 4 / 3;
        Gx2Ui_Map((DRC_W - dw) / 2, STRIP_H + 6, dw, dh);
    } else {
#else
    if (s_haveMap && s_mapTex) {
        SDL_Rect dst = { (DRC_W - MAP_W) / 2, STRIP_H + 6, MAP_W, DRC_H - STRIP_H - 6 };
        dst.w = dst.h * 4 / 3;
        dst.x = (DRC_W - dst.w) / 2;
        SDL_RenderCopy(r, s_mapTex, NULL, &dst);
    } else {
#endif
        Dc_Color(r, 70, 70, 70, 255);
        Text(r, (DRC_W - TextW("OPEN THE MAP TO SHOW IT HERE", 3)) / 2, 290, 3, "OPEN THE MAP TO SHOW IT HERE");
    }
    DrawAchToast(r);
    DrawTvButton(r, 0);
}

/* Off-TV: the game is on the GamePad; just the button over it. */
void GamepadScreen_DrawOverlay(SDL_Renderer* r)
{
    s_offTvNow = 1;
#ifndef SH_WIIU_GX2
    SDL_RenderSetClipRect(r, NULL);
#endif
    DrawTvButton(r, 1);
}
