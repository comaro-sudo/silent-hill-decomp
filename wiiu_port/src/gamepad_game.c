/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gamepad_game.c - the game-side half of the GamePad screen: reads the few game
 * values the GamePad shows. Compiled like game code (decomp headers, the
 * ogc_prelude bool fix); the SDL side (gamepad_screen.c) only sees plain ints.
 * The *_game.c suffix is what ogc_flags.sh keys that on.
 */
#include "game.h"

/* Q12 health, 100.0 = full (g_SysWork.playerWork.player.health) */
int WiiU_PlayerHealthQ12(void) { return (int)g_SysWork.playerWork.player.health; }

/* e_GameState: GameState_InGame (11), GameState_PaperMapScreen (15), ... */
int WiiU_GameState(void) { return (int)g_GameWork.gameState; }

/* e_SysState inside InGame: SysState_Gameplay (0), SysState_MapScreen (3), ... */
int WiiU_SysState(void) { return (int)g_SysWork.sysState; }

/* What the GamePad should be doing this frame. */
int WiiU_ShowHealth(void)
{
    if (g_GameWork.gameState != GameState_InGame)
        return 0;
    switch (g_SysWork.sysState) {
    case SysState_Gameplay:
    case SysState_StatusMenu:
    case SysState_MapScreen:
    case SysState_ReadMessage:
    case SysState_GamePaused:
        return 1;
    default:
        return 0;                         /* FMV, loads, game over, save */
    }
}

int WiiU_InMapScreen(void)
{
    return g_GameWork.gameState == GameState_PaperMapScreen ||
           (g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_MapScreen);
}

/* thresholds of Gfx_Inventory_HealthStatusDraw (item_screens_3.c) */
int WiiU_HealthStage(void)
{
    const s32 h = g_SysWork.playerWork.player.health;
    if (h < Q12(10.0f)) return 3;         /* Danger */
    if (h < Q12(50.0f)) return 2;         /* Caution */
    return 1;                             /* Fine */
}
