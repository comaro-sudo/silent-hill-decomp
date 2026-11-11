/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sdl_compat_ogc.c - the thin SDL2 slice the decomp's SH_PC_PORT code uses, on
 * libogc. Same job as xbox360_port/src/sdl_compat_xbox360.c: timing from the
 * PPC time base, keyboard and mouse permanently idle so the shared input code
 * falls through to the pad path. See xbox_port/include/SDL_*.h.
 */
#include <stdint.h>
#include <ogc/lwp.h>
#include <unistd.h>

#include "ppc/timebase.h"
#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_scancode.h"

Uint32 SDL_GetTicks(void)
{
    return (Uint32)(mftb() / (PPC_TIMEBASE_FREQ / 1000));
}

Uint64 SDL_GetPerformanceCounter(void)   { return (Uint64)mftb(); }
Uint64 SDL_GetPerformanceFrequency(void) { return (Uint64)PPC_TIMEBASE_FREQ; }

void SDL_Delay(Uint32 ms)
{
    /* libogc has a scheduler, so sleep rather than spin: the audio DMA
     * callback and the pad threads keep running meanwhile. */
    if (ms)
        usleep(ms * 1000u);
    else
        LWP_YieldThread();
}

/* ms since boot, the clock xa_xbox.c / fmv_xbox.c read under SH_XBOX360_PORT. */
unsigned int Sh360_TickMs(void) { return (unsigned int)SDL_GetTicks(); }

static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
const unsigned char* g_sdlKeyboardState = s_zeroKeys;

Uint32 SDL_GetMouseState(int* x, int* y)          { if (x) *x = 0; if (y) *y = 0; return 0; }
Uint32 SDL_GetRelativeMouseState(int* x, int* y)  { if (x) *x = 0; if (y) *y = 0; return 0; }
int    SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
