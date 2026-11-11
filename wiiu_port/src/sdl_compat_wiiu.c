/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sdl_compat_wiiu.c - the bits of the console SDL shim the real SDL2 library
 * does not provide. SDL_GetTicks / SDL_Delay / the performance counter and the
 * mouse calls come from libSDL2 itself on this build (defining them here would
 * clash with it at link time).
 */
#include <SDL.h>

/* ms since boot, read by xa_xbox.c / fmv_xbox.c under SH_XBOX360_PORT */
unsigned int Sh360_TickMs(void) { return (unsigned int)SDL_GetTicks(); }

/* No keyboard: every g_sdlKeyboardState[SDL_SCANCODE_*] read is "not pressed". */
static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
const unsigned char* g_sdlKeyboardState = s_zeroKeys;
