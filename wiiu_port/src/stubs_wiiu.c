/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * stubs_wiiu.c - subsystems the Wii U build does without.
 * Each stub is inert, never a fake success a caller would act on.
 */

/* RetroAchievements (ra_xbox.c / ra_badge_xbox.c): no network client here. */
void Pc_Ra_Update(void)         { }
void Pc_Ra_StatusToast(void)    { }
void RaBadge_RenderDirect(void) { }

/* On-screen debug console (dbg_overlay_xbox.c). The log file has the lines. */
void DbgOverlay_XboxRender(void) { }

/* events_main.c test hook (SH_XBOX360_PORT): -1 = off. */
int g_DebugForceEventIdx = -1;
