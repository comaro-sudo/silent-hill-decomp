/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_LOCAL_ACH_H
#define PC_LOCAL_ACH_H

#include <stddef.h>

#include "pc_retroachievements.h" /* PcRaAch */

/* Local achievements (workspace docs/plans/achievements-plan.md).
 *
 * The 66 achievements of the RetroAchievements Silent Hill set (by tele, used
 * with permission), checked natively in C on named game variables. Nothing
 * goes over the network: unlocks are kept in achievements_local.txt next to
 * the config. Modern difficulty counts as Easy. The existing achievements
 * screen and unlock popup show them (badges come from local PNG files). */

#ifdef __cplusplus
extern "C" {
#endif

#define PC_LOCAL_ACH_BADGE_PREFIX "LOC"

void Pc_LocalAch_Init(void);
/* Once per frame (game_main.c). Cheap when nothing changes. */
void Pc_LocalAch_Update(void);
/* Kill hook (Savegame_EnemyStateUpdate) and the results screen rank score
 * (stars x 10, out of 100). */
void Pc_LocalAch_OnKill(void* chara);
void Pc_LocalAch_OnGameClear(int rankScore);
/* New game, loaded save, Next Fear: per-session counters start over. */
void Pc_LocalAch_SessionReset(void);

/* For the achievements screen: the list in PcRaAch form (badge = "LOCnn"). */
int  Pc_LocalAch_Snapshot(PcRaAch* out, int max);
/* Badge request for a "LOCnn" name: loads the PNG from disk and hands it to
 * the screen and the popup. Returns 1 if the name was a local badge. */
int  Pc_LocalAch_RequestBadge(const char* badge);

#ifdef __cplusplus
}
#endif

#endif
