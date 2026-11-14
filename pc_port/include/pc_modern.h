/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MODERN_H
#define PC_MODERN_H

/* Modern difficulty (docs/plans/modern-difficulty-plan.md in the workspace).
 *
 * Modern is Normal underneath: the savegame keeps gameDifficulty = Normal and
 * a separate flag in the spare save byte s_Savegame.unused_23E:
 *   bit 0     Modern on
 *   bits 1-2  kills since the last enemy drop (0-2)
 * Old saves read the byte as 0 = not Modern. Only the changes below check it:
 *   - twice the ammo and health items from pickups
 *   - health regen after 8 s without fighting, in every game state (hidden:
 *     nothing tells the player), cutscenes skip the wait
 *   - full health after each boss
 *   - every third enemy killed drops an item on the floor (pc_modern_drops.c)
 */

#ifdef __cplusplus
extern "C" {
#endif

int  Pc_Modern_IsActive(void);
void Pc_Modern_SetActive(int on);

/* Pickup scaling, used by Event_CommonItemTake. */
int  Pc_Modern_AmmoCount(int count);      /* ammo pickup count: 1.5x on Modern, rounded */
int  Pc_Modern_HealthItemCount(int count); /* health item count: as on Normal */
void Pc_Modern_OnDeath(void);              /* game over: track deaths for the Modern offer */
int  Pc_Modern_OfferUpdate(void);          /* game over screen: 1 while the offer holds it */

/* Once per frame in every game state (game_main.c). Regen and drops. */
void Pc_Modern_Update(void);

/* Call once when a boss fight is won. Full health, regen timer at the delay. */
void Pc_Modern_BossCleared(void);

/* Call when an enemy dies for good (Savegame_EnemyStateUpdate). `chara` is the
 * s_SubCharacter*. Counts kills and spawns a drop every third one. */
void Pc_Modern_EnemyKilled(void* chara);

/* Draw and pick-up handling for drops; called from the gameplay frame. */
void Pc_Modern_DropsUpdate(void);

/* Drop side file next to a save slot (pc_modern_drops.c). */
void Pc_Modern_DropsSave(int slot, int saveIdx);
void Pc_Modern_DropsLoad(int slot, int saveIdx);
void Pc_Modern_DropsClear(void);

#ifdef __cplusplus
}
#endif

#endif
