/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pad_ogc.c - GameCube pad (both consoles), and on Wii the Classic Controller
 * and Wii Remote + Nunchuk, mapped onto the PSX digital pad + right stick.
 * Same output contract as xbox360_port/xdk/src/pad_xinput.cpp.
 *
 * Layout is positional (the button in the PlayStation's place), so on-screen
 * prompts and muscle memory both work:
 *
 *   PSX        GameCube pad          Classic Controller   Remote + Nunchuk
 *   Cross      A                     b                    A
 *   Circle     B                     a                    2
 *   Square     X                     y                    C (Nunchuk)
 *   Triangle   Y                     x                    1
 *   L1 / R1    L / R (full click)    L / R                Z / B
 *   L2 / R2    L / R (half press)    ZL / ZR              - / -
 *   Select     Z                     -                    -
 *   Start      Start                 +                    +
 *   d-pad      d-pad + main stick    d-pad + left stick   d-pad + Nunchuk stick
 *   R stick    C stick               right stick          (none)
 */
#include <string.h>
#include <stdio.h>
#include <gccore.h>
#if defined(SH_WII_PORT)
#include <wiiuse/wpad.h>
#endif

#include "sh_log.h"

#define PSX_SELECT   0
#define PSX_L3       1
#define PSX_R3       2
#define PSX_START    3
#define PSX_UP       4
#define PSX_RIGHT    5
#define PSX_DOWN     6
#define PSX_LEFT     7
#define PSX_L2       8
#define PSX_R2       9
#define PSX_L1      10
#define PSX_R1      11
#define PSX_TRIANGLE 12
#define PSX_CIRCLE  13
#define PSX_CROSS   14
#define PSX_SQUARE  15

#define PadStateStable 6

static unsigned char* s_padBuf;
static unsigned short s_lastButtons = 0xFFFF;
static int            s_stickArmed;

/* ~40% of travel, as on the other console ports: less let stick drift toggle
 * the d-pad bit and the menus read it as repeated presses. GC sticks give
 * about +-100, Classic Controller about +-32 (both normalised to +-128 here). */
#define STICK_DZ 50

void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00; b[1] = 0x41;
    b[2] = 0xFF; b[3] = 0xFF;
    b[4] = 0x80; b[5] = 0x80;
    b[6] = 0x80; b[7] = 0x80;
}

void Pad_XboxInit(void)
{
    PAD_Init();
#if defined(SH_WII_PORT)
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
#endif
    SH_DBG("[PAD] GameCube pad%s",
#if defined(SH_WII_PORT)
           " + Wii Remote / Classic Controller"
#else
           ""
#endif
           );
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
}

static int Clamp8(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }

/* Scripted input for unattended emulator test runs (as the 360's
 * pad_xinput.cpp): <data root>/autoplay.txt, one press per line,
 *   <frame> <BUTTON> [holdFrames]      e.g.  "1500 START 10"
 *   every <first> <last> <period> <BUTTON> [hold]   repeat a press
 * BUTTON: CROSS CIRCLE SQUARE TRIANGLE START SELECT UP DOWN LEFT RIGHT
 * L1 R1 L2 R2. Frames count Pad_Poll calls. Absent file = no effect. */
#define AUTO_MAX 512
typedef struct { int first, last, period, bit, hold; } AutoKey;
static AutoKey s_auto[AUTO_MAX];
static int     s_autoN = -1;
static int     s_autoFrame;

static int Auto_Bit(const char* n)
{
    static const struct { const char* n; int b; } k[] = {
        {"SELECT",PSX_SELECT},{"START",PSX_START},{"UP",PSX_UP},{"RIGHT",PSX_RIGHT},
        {"DOWN",PSX_DOWN},{"LEFT",PSX_LEFT},{"L2",PSX_L2},{"R2",PSX_R2},{"L1",PSX_L1},
        {"R1",PSX_R1},{"TRIANGLE",PSX_TRIANGLE},{"CIRCLE",PSX_CIRCLE},{"CROSS",PSX_CROSS},
        {"SQUARE",PSX_SQUARE},
    };
    unsigned i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strcmp(n, k[i].n) == 0) return k[i].b;
    return -1;
}

static void Auto_Load(void)
{
    extern const char* Sh360Fs_DataRoot(void);
    char path[192], line[128], name[32];
    FILE* f;
    s_autoN = 0;
    snprintf(path, sizeof(path), "%sautoplay.txt", Sh360Fs_DataRoot());
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f) && s_autoN < AUTO_MAX) {
        AutoKey k = { 0, 0, 0, -1, 8 };
        if (sscanf(line, "every %d %d %d %31s %d", &k.first, &k.last, &k.period, name, &k.hold) >= 4) {
        } else if (sscanf(line, "%d %31s %d", &k.first, name, &k.hold) >= 2) {
            k.last = k.first; k.period = 1;
        } else {
            continue;
        }
        k.bit = Auto_Bit(name);
        if (k.bit >= 0 && k.period > 0) s_auto[s_autoN++] = k;
    }
    fclose(f);
    SH_DBG("[PAD] autoplay: %d scripted entries from %s", s_autoN, path);
}

static void Auto_Apply(unsigned short* psx)
{
    int i;
    if (s_autoN < 0) Auto_Load();
    s_autoFrame++;
    for (i = 0; i < s_autoN; i++) {
        const AutoKey* k = &s_auto[i];
        int rel;
        if (s_autoFrame < k->first || s_autoFrame > k->last + k->hold) continue;
        rel = (s_autoFrame - k->first) % k->period;
        if (rel < k->hold) *psx &= (unsigned short)~(1u << k->bit);
    }
}

void Pad_Poll(void)
{
    unsigned short psx = 0xFFFF;
    int lx = 0, ly = 0, rx = 0, ry = 0;

    if (!s_padBuf)
        return;
#define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))

    PAD_ScanPads();
    {
        u16 b = PAD_ButtonsHeld(0);
        int lt = PAD_TriggerL(0), rt = PAD_TriggerR(0);
        if (b & PAD_BUTTON_UP)    PRESS(PSX_UP);
        if (b & PAD_BUTTON_DOWN)  PRESS(PSX_DOWN);
        if (b & PAD_BUTTON_LEFT)  PRESS(PSX_LEFT);
        if (b & PAD_BUTTON_RIGHT) PRESS(PSX_RIGHT);
        if (b & PAD_BUTTON_A)     PRESS(PSX_CROSS);
        if (b & PAD_BUTTON_B)     PRESS(PSX_CIRCLE);
        if (b & PAD_BUTTON_X)     PRESS(PSX_SQUARE);
        if (b & PAD_BUTTON_Y)     PRESS(PSX_TRIANGLE);
        if (b & PAD_TRIGGER_Z)    PRESS(PSX_SELECT);
        if (b & PAD_BUTTON_START) PRESS(PSX_START);
        /* The digital click at the end of travel is L1/R1; a firm half
         * press without the click is L2/R2. */
        if (b & PAD_TRIGGER_L) PRESS(PSX_L1); else if (lt > 0x70) PRESS(PSX_L2);
        if (b & PAD_TRIGGER_R) PRESS(PSX_R1); else if (rt > 0x70) PRESS(PSX_R2);
        lx = PAD_StickX(0) * 128 / 100;  ly = PAD_StickY(0) * 128 / 100;
        rx = PAD_SubStickX(0) * 128 / 100; ry = PAD_SubStickY(0) * 128 / 100;
    }

#if defined(SH_WII_PORT)
    WPAD_ScanPads();
    {
        WPADData* d = WPAD_Data(WPAD_CHAN_0);
        u32 b = WPAD_ButtonsHeld(WPAD_CHAN_0);
        if (d && d->exp.type == WPAD_EXP_CLASSIC) {
            if (b & WPAD_CLASSIC_BUTTON_UP)      PRESS(PSX_UP);
            if (b & WPAD_CLASSIC_BUTTON_DOWN)    PRESS(PSX_DOWN);
            if (b & WPAD_CLASSIC_BUTTON_LEFT)    PRESS(PSX_LEFT);
            if (b & WPAD_CLASSIC_BUTTON_RIGHT)   PRESS(PSX_RIGHT);
            if (b & WPAD_CLASSIC_BUTTON_B)       PRESS(PSX_CROSS);
            if (b & WPAD_CLASSIC_BUTTON_A)       PRESS(PSX_CIRCLE);
            if (b & WPAD_CLASSIC_BUTTON_Y)       PRESS(PSX_SQUARE);
            if (b & WPAD_CLASSIC_BUTTON_X)       PRESS(PSX_TRIANGLE);
            if (b & WPAD_CLASSIC_BUTTON_FULL_L)  PRESS(PSX_L1);
            if (b & WPAD_CLASSIC_BUTTON_FULL_R)  PRESS(PSX_R1);
            if (b & WPAD_CLASSIC_BUTTON_ZL)      PRESS(PSX_L2);
            if (b & WPAD_CLASSIC_BUTTON_ZR)      PRESS(PSX_R2);
            if (b & WPAD_CLASSIC_BUTTON_MINUS)   PRESS(PSX_SELECT);
            if (b & WPAD_CLASSIC_BUTTON_PLUS)    PRESS(PSX_START);
            {
                const joystick_t* l = &d->exp.classic.ljs;
                const joystick_t* r = &d->exp.classic.rjs;
                int v;
                v = ((int)l->pos.x - (int)l->center.x) * 128 / ((int)l->max.x - (int)l->center.x + 1);
                if (v * v > lx * lx) lx = v;
                v = ((int)l->pos.y - (int)l->center.y) * 128 / ((int)l->max.y - (int)l->center.y + 1);
                if (v * v > ly * ly) ly = v;
                v = ((int)r->pos.x - (int)r->center.x) * 128 / ((int)r->max.x - (int)r->center.x + 1);
                if (v * v > rx * rx) rx = v;
                v = ((int)r->pos.y - (int)r->center.y) * 128 / ((int)r->max.y - (int)r->center.y + 1);
                if (v * v > ry * ry) ry = v;
            }
        } else if (d) {
            if (b & WPAD_BUTTON_UP)    PRESS(PSX_UP);
            if (b & WPAD_BUTTON_DOWN)  PRESS(PSX_DOWN);
            if (b & WPAD_BUTTON_LEFT)  PRESS(PSX_LEFT);
            if (b & WPAD_BUTTON_RIGHT) PRESS(PSX_RIGHT);
            if (b & WPAD_BUTTON_A)     PRESS(PSX_CROSS);
            if (b & WPAD_BUTTON_2)     PRESS(PSX_CIRCLE);
            if (b & WPAD_BUTTON_1)     PRESS(PSX_TRIANGLE);
            if (b & WPAD_BUTTON_B)     PRESS(PSX_R1);
            if (b & WPAD_BUTTON_MINUS) PRESS(PSX_SELECT);
            if (b & WPAD_BUTTON_PLUS)  PRESS(PSX_START);
            if (d->exp.type == WPAD_EXP_NUNCHUK) {
                const joystick_t* n = &d->exp.nunchuk.js;
                int v;
                if (b & WPAD_NUNCHUK_BUTTON_C) PRESS(PSX_SQUARE);
                if (b & WPAD_NUNCHUK_BUTTON_Z) PRESS(PSX_L1);
                v = ((int)n->pos.x - (int)n->center.x) * 128 / ((int)n->max.x - (int)n->center.x + 1);
                if (v * v > lx * lx) lx = v;
                v = ((int)n->pos.y - (int)n->center.y) * 128 / ((int)n->max.y - (int)n->center.y + 1);
                if (v * v > ly * ly) ly = v;
            }
        }
    }
#endif

    /* A stick is not trusted until seen at rest once, so one that comes up
     * deflected cannot hold a direction from boot. */
    if (!s_stickArmed && lx > -STICK_DZ && lx < STICK_DZ && ly > -STICK_DZ && ly < STICK_DZ)
        s_stickArmed = 1;
    if (s_stickArmed) {
        if (ly >  STICK_DZ) PRESS(PSX_UP);
        if (ly < -STICK_DZ) PRESS(PSX_DOWN);
        if (lx < -STICK_DZ) PRESS(PSX_LEFT);
        if (lx >  STICK_DZ) PRESS(PSX_RIGHT);
    }
    if (rx > -STICK_DZ / 2 && rx < STICK_DZ / 2) rx = 0;
    if (ry > -STICK_DZ / 2 && ry < STICK_DZ / 2) ry = 0;
#undef PRESS
    Auto_Apply(&psx);

    s_padBuf[0] = 0x00;
    s_padBuf[1] = 0x41;
    /* digitalButtons is a u16 the game reads natively; big-endian puts the
     * HIGH byte first (the 360 learned this on hardware, pad_xinput.cpp). */
    s_padBuf[2] = (unsigned char)((psx >> 8) & 0xFF);
    s_padBuf[3] = (unsigned char)(psx & 0xFF);
    s_padBuf[4] = (unsigned char)(128 + Clamp8(rx));
    s_padBuf[5] = (unsigned char)(128 - Clamp8(ry));
    s_padBuf[6] = 0x80;
    s_padBuf[7] = 0x80;

    if (psx != s_lastButtons) {
        SH_DBG("[PAD] buttons=%04X", (unsigned)psx);
        s_lastButtons = psx;
    }
}

int Pad_XboxBlackWhite(int* black, int* white)
{
    if (black) *black = 0;
    if (white) *white = 0;
    return 0;
}

unsigned short Pad_XboxButtons(void) { return s_lastButtons; }

int  PadChkVsync(void)                              { return 0; }
void PadStartCom(void)                              { }
int  PadGetState(int socket)                        { (void)socket; return PadStateStable; }
int  PadInfoMode(int socket, int term, int offs)    { (void)socket; (void)term; (void)offs; return 0; }
int  PadSetActAlign(int socket, unsigned char* a)   { (void)socket; (void)a; return 1; }
int  PadSetMainMode(int socket, int offs, int lock) { (void)socket; (void)offs; (void)lock; return 1; }

/* PSX actuator bytes -> rumble. GC pads and Wii Remotes have one on/off motor:
 * on while either PSX motor is asked for anything. */
void PadSetAct(int socket, unsigned char* table, int len)
{
    static int s_on = -1;
    int on;
    (void)socket;
    if (!table || len < 2)
        return;
    on = (table[0] | table[1]) ? 1 : 0;
    if (on == s_on)
        return;
    s_on = on;
    PAD_ControlMotor(0, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
#if defined(SH_WII_PORT)
    WPAD_Rumble(WPAD_CHAN_0, on);
#endif
}
