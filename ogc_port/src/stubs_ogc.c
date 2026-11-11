/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * stubs_ogc.c - the subsystems the GameCube/Wii build does without, and the one
 * place they may be absent. Same set as xbox360_port/xdk/src/stubs_xdk.c.
 * Each stub is inert, never a fake success a caller would act on.
 */
#include <stdlib.h>

/* RetroAchievements (ra_xbox.c / ra_badge_xbox.c): no network client here. */
void Pc_Ra_Update(void)         { }
void Pc_Ra_StatusToast(void)    { }
void RaBadge_RenderDirect(void) { }

/* On-screen debug console (dbg_overlay_xbox.c). The log file has the lines. */
void DbgOverlay_XboxRender(void) { }

/* events_main.c test hook (SH_XBOX360_PORT): "dispatch map event n". Only the
 * 360's scripted Xenia runs ever set it; -1 = off. */
int g_DebugForceEventIdx = -1;

/* Quit from the game's menu: back to the loader (Homebrew Channel on Wii,
 * Swiss / the IPL on GameCube). exit() runs libogc's return-to-loader stub. */
void Xbox_QuitToDashboard(void)
{
    extern void SH_DebugLogFlush(void);
    SH_DebugLogFlush();
    exit(0);
}
