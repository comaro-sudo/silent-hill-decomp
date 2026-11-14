/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_LOCAL_ACH_H
#define PC_LOCAL_ACH_H

/* Local achievements (workspace docs/plans/achievements-plan.md), console port.
 *
 * The 66 achievements of the RetroAchievements Silent Hill set (by tele, used
 * with permission), checked natively in C on named game variables. Nothing
 * goes over the network: unlocks are kept in achievements_local.txt in the
 * data root. Modern difficulty counts as Easy.
 *
 * Unlike the PC build there is no GL/SDL overlay: the unlock toast and the
 * title-menu list are drawn with the game's own text and primitives, so they
 * work on the GX (GameCube/Wii) and SDL (Wii U) renderers alike. */

#ifdef __cplusplus
extern "C" {
#endif

void Pc_LocalAch_Init(void);
/* Once per frame (game_main.c): checks, then draws the unlock toast. Cheap when nothing changes. */
void Pc_LocalAch_Update(void);
/* Kill hook (Savegame_EnemyStateUpdate) and the results screen rank score
 * (stars x 10, out of 100). */
void Pc_LocalAch_OnKill(void* chara);
void Pc_LocalAch_OnGameClear(int rankScore);
/* New game, loaded save, Next Fear: per-session counters start over. */
void Pc_LocalAch_SessionReset(void);

/* Accessors (list screen, and a possible Wii U GamePad list). */
int         Pc_LocalAch_Count(void);
const char* Pc_LocalAch_Title(int idx);
const char* Pc_LocalAch_Desc(int idx);
unsigned    Pc_LocalAch_Points(int idx);
int         Pc_LocalAch_IsUnlocked(int idx);
int         Pc_LocalAch_IsTracked(int idx);

/* Title-menu ACHIEVEMENTS list. Open() on selecting the entry; Update() each
 * frame with the pad (returns 0 once the player backs out); Draw() each frame. */
void Pc_LocalAchUi_Open(void);
int  Pc_LocalAchUi_Update(void);
void Pc_LocalAchUi_Draw(void);

#ifdef __cplusplus
}
#endif

#endif
