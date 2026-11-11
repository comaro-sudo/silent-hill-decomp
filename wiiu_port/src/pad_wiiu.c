/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pad_wiiu.c - Wii U input and system loop.
 *
 * GamePad (VPAD) and Pro Controller / Classic Controller (KPAD) mapped onto the
 * PSX pad, positionally (the button in the PlayStation's place):
 *
 *   PSX        GamePad / Pro / Classic
 *   Cross      B          Circle   A        Square  Y      Triangle  X
 *   L1 / R1    L / R      L2 / R2  ZL / ZR  L3 / R3 stick clicks
 *   Select     -          Start    +        d-pad   d-pad + left stick
 *   right stick -> the shared TPS / free-look camera
 *
 * Also owns the ProcUI pump (WHBProcIsRunning) the console requires every
 * frame for the HOME menu and for leaving the app, and the boot hook.
 */
#include <string.h>
#include <stdlib.h>
#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <whb/proc.h>
#include <SDL.h>

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
#define STICK_DZ 0.4f

static unsigned char* s_padBuf;
static unsigned short s_lastButtons = 0xFFFF;
static int            s_stickArmed;

void WiiU_Boot(void)
{
    WHBProcInit();
    SDL_Init(SDL_INIT_TIMER);
}

void WiiU_ProcPump(void)
{
    if (!WHBProcIsRunning()) {
        extern void SH_DebugLogFlush(void);
        SH_DBG("[SYS] leaving (HOME menu / power)");
        SH_DebugLogFlush();
        SDL_Quit();
        WHBProcShutdown();
        exit(0);
    }
}

void Xbox_QuitToDashboard(void)
{
    extern void SH_DebugLogFlush(void);
    SH_DebugLogFlush();
    SDL_Quit();
    WHBProcShutdown();
    exit(0);
}

void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00; b[1] = 0x41;
    b[2] = 0xFF; b[3] = 0xFF;
    b[4] = 0x80; b[5] = 0x80;
    b[6] = 0x80; b[7] = 0x80;
}

void Pad_XboxInit(void)
{
    VPADInit();
    KPADInit();
    WPADEnableURCC(1);
    SH_DBG("[PAD] GamePad + Pro / Classic Controller");
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf) Pad_FillIdle(s_padBuf);
}

static float Bigger(float a, float b) { return (a * a >= b * b) ? a : b; }

void Pad_Poll(void)
{
    unsigned short psx = 0xFFFF;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    VPADStatus vs;
    VPADReadError ve;
    int ch;

    if (!s_padBuf) return;
#define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))

    if (VPADRead(VPAD_CHAN_0, &vs, 1, &ve) == 1 && ve == VPAD_READ_SUCCESS) {
        unsigned b = vs.hold;
        if (b & VPAD_BUTTON_UP)    PRESS(PSX_UP);
        if (b & VPAD_BUTTON_DOWN)  PRESS(PSX_DOWN);
        if (b & VPAD_BUTTON_LEFT)  PRESS(PSX_LEFT);
        if (b & VPAD_BUTTON_RIGHT) PRESS(PSX_RIGHT);
        if (b & VPAD_BUTTON_B)     PRESS(PSX_CROSS);
        if (b & VPAD_BUTTON_A)     PRESS(PSX_CIRCLE);
        if (b & VPAD_BUTTON_Y)     PRESS(PSX_SQUARE);
        if (b & VPAD_BUTTON_X)     PRESS(PSX_TRIANGLE);
        if (b & VPAD_BUTTON_L)     PRESS(PSX_L1);
        if (b & VPAD_BUTTON_R)     PRESS(PSX_R1);
        if (b & VPAD_BUTTON_ZL)    PRESS(PSX_L2);
        if (b & VPAD_BUTTON_ZR)    PRESS(PSX_R2);
        if (b & VPAD_BUTTON_STICK_L) PRESS(PSX_L3);
        if (b & VPAD_BUTTON_STICK_R) PRESS(PSX_R3);
        if (b & VPAD_BUTTON_MINUS) PRESS(PSX_SELECT);
        if (b & VPAD_BUTTON_PLUS)  PRESS(PSX_START);
        lx = vs.leftStick.x;  ly = vs.leftStick.y;
        rx = vs.rightStick.x; ry = vs.rightStick.y;
    }

    for (ch = 0; ch < 4; ch++) {
        KPADStatus ks;
        int err = 0;
        if (KPADReadEx((KPADChan)ch, &ks, 1, &err) <= 0 || err != KPAD_ERROR_OK)
            continue;
        if (ks.extensionType == WPAD_EXT_PRO_CONTROLLER) {
            unsigned b = ks.pro.hold;
            if (b & WPAD_PRO_BUTTON_UP)       PRESS(PSX_UP);
            if (b & WPAD_PRO_BUTTON_DOWN)     PRESS(PSX_DOWN);
            if (b & WPAD_PRO_BUTTON_LEFT)     PRESS(PSX_LEFT);
            if (b & WPAD_PRO_BUTTON_RIGHT)    PRESS(PSX_RIGHT);
            if (b & WPAD_PRO_BUTTON_B)        PRESS(PSX_CROSS);
            if (b & WPAD_PRO_BUTTON_A)        PRESS(PSX_CIRCLE);
            if (b & WPAD_PRO_BUTTON_Y)        PRESS(PSX_SQUARE);
            if (b & WPAD_PRO_BUTTON_X)        PRESS(PSX_TRIANGLE);
            if (b & WPAD_PRO_TRIGGER_L)       PRESS(PSX_L1);
            if (b & WPAD_PRO_TRIGGER_R)       PRESS(PSX_R1);
            if (b & WPAD_PRO_TRIGGER_ZL)      PRESS(PSX_L2);
            if (b & WPAD_PRO_TRIGGER_ZR)      PRESS(PSX_R2);
            if (b & WPAD_PRO_BUTTON_STICK_L)  PRESS(PSX_L3);
            if (b & WPAD_PRO_BUTTON_STICK_R)  PRESS(PSX_R3);
            if (b & WPAD_PRO_BUTTON_MINUS)    PRESS(PSX_SELECT);
            if (b & WPAD_PRO_BUTTON_PLUS)     PRESS(PSX_START);
            lx = Bigger(lx, ks.pro.leftStick.x);  ly = Bigger(ly, ks.pro.leftStick.y);
            rx = Bigger(rx, ks.pro.rightStick.x); ry = Bigger(ry, ks.pro.rightStick.y);
        } else if (ks.extensionType == WPAD_EXT_CLASSIC || ks.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
            unsigned b = ks.classic.hold;
            if (b & WPAD_CLASSIC_BUTTON_UP)    PRESS(PSX_UP);
            if (b & WPAD_CLASSIC_BUTTON_DOWN)  PRESS(PSX_DOWN);
            if (b & WPAD_CLASSIC_BUTTON_LEFT)  PRESS(PSX_LEFT);
            if (b & WPAD_CLASSIC_BUTTON_RIGHT) PRESS(PSX_RIGHT);
            if (b & WPAD_CLASSIC_BUTTON_B)     PRESS(PSX_CROSS);
            if (b & WPAD_CLASSIC_BUTTON_A)     PRESS(PSX_CIRCLE);
            if (b & WPAD_CLASSIC_BUTTON_Y)     PRESS(PSX_SQUARE);
            if (b & WPAD_CLASSIC_BUTTON_X)     PRESS(PSX_TRIANGLE);
            if (b & WPAD_CLASSIC_BUTTON_L)     PRESS(PSX_L1);
            if (b & WPAD_CLASSIC_BUTTON_R)     PRESS(PSX_R1);
            if (b & WPAD_CLASSIC_BUTTON_ZL)    PRESS(PSX_L2);
            if (b & WPAD_CLASSIC_BUTTON_ZR)    PRESS(PSX_R2);
            if (b & WPAD_CLASSIC_BUTTON_MINUS) PRESS(PSX_SELECT);
            if (b & WPAD_CLASSIC_BUTTON_PLUS)  PRESS(PSX_START);
            lx = Bigger(lx, ks.classic.leftStick.x);  ly = Bigger(ly, ks.classic.leftStick.y);
            rx = Bigger(rx, ks.classic.rightStick.x); ry = Bigger(ry, ks.classic.rightStick.y);
        }
    }

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
    if (rx > 1) rx = 1; if (rx < -1) rx = -1;
    if (ry > 1) ry = 1; if (ry < -1) ry = -1;
#undef PRESS

    s_padBuf[0] = 0x00;
    s_padBuf[1] = 0x41;
    s_padBuf[2] = (unsigned char)((psx >> 8) & 0xFF);   /* big-endian u16 */
    s_padBuf[3] = (unsigned char)(psx & 0xFF);
    s_padBuf[4] = (unsigned char)(128 + (int)(rx * 127.0f));
    s_padBuf[5] = (unsigned char)(128 - (int)(ry * 127.0f));
    s_padBuf[6] = 0x80;
    s_padBuf[7] = 0x80;

    if (psx != s_lastButtons) {
        SH_DBG("[PAD] buttons=%04X", (unsigned)psx);
        s_lastButtons = psx;
    }
}

int Pad_XboxBlackWhite(int* black, int* white) { if (black) *black = 0; if (white) *white = 0; return 0; }
unsigned short Pad_XboxButtons(void) { return s_lastButtons; }

int  PadChkVsync(void)                              { return 0; }
void PadStartCom(void)                              { }
int  PadGetState(int socket)                        { (void)socket; return PadStateStable; }
int  PadInfoMode(int socket, int term, int offs)    { (void)socket; (void)term; (void)offs; return 0; }
int  PadSetActAlign(int socket, unsigned char* a)   { (void)socket; (void)a; return 1; }
int  PadSetMainMode(int socket, int offs, int lock) { (void)socket; (void)offs; (void)lock; return 1; }

/* PSX actuator bytes -> GamePad rumble (a pattern of on/off bits). */
void PadSetAct(int socket, unsigned char* table, int len)
{
    static int s_on = -1;
    static uint8_t pattern[15];
    int on;
    (void)socket;
    if (!table || len < 2) return;
    on = (table[0] | table[1]) ? 1 : 0;
    if (on == s_on) return;
    s_on = on;
    if (on) {
        memset(pattern, 0xFF, sizeof(pattern));
        VPADControlMotor(VPAD_CHAN_0, pattern, 120);
    } else {
        VPADStopMotor(VPAD_CHAN_0);
    }
}
