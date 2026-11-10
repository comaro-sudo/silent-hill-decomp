/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Modern difficulty: flag, pickup scaling, hidden health regen, boss restore.
 * See pc_modern.h for the overview. */
#include "pc_modern.h"

#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_data.h" /* g_DeltaTime */

#define MODERN_FLAG_ON        (1 << 0)
#define MODERN_REGEN_DELAY    Q12(8.0f)   /* calm time before health comes back */
#define MODERN_REGEN_RATE     2           /* HP per second */
#define MODERN_HEALTH_CAP     Q12(100.0f)
#define MODERN_DT_MAX         Q12(0.5f)   /* ignore load hitches and pauses longer than this */

extern long long GsGetCumulativeQ12(void);

static long long s_PrevClock    = -1;
static q19_12    s_CalmTime     = 0;
static q19_12    s_PrevHealth   = 0;
static q20_12    s_PrevRunDist  = 0;
static int       s_HaveSnapshot = 0;

int Pc_Modern_IsActive(void)
{
    return g_SavegamePtr != NULL && (g_SavegamePtr->unused_23E & MODERN_FLAG_ON);
}

void Pc_Modern_SetActive(int on)
{
    if (g_SavegamePtr == NULL)
    {
        return;
    }

    if (on)
    {
        g_SavegamePtr->unused_23E = MODERN_FLAG_ON; /* kill counter starts at 0 */
    }
    else
    {
        g_SavegamePtr->unused_23E = 0;
    }

    s_HaveSnapshot = 0;
    s_CalmTime     = 0;
}

int Pc_Modern_AmmoMultMin(int mult)
{
    if (Pc_Modern_IsActive() && mult < 2)
    {
        return 2;
    }
    return mult;
}

int Pc_Modern_HealthItemCount(int count)
{
    return Pc_Modern_IsActive() ? count * 2 : count;
}

void Pc_Modern_BossCleared(void)
{
    if (!Pc_Modern_IsActive() || g_SysWork.playerWork.player.health <= Q12(0.0f))
    {
        return;
    }

    g_SysWork.playerWork.player.health = MODERN_HEALTH_CAP;
    s_CalmTime   = MODERN_REGEN_DELAY;
    s_PrevHealth = MODERN_HEALTH_CAP;
}

/* Health regen. Runs in every game state and is paced by the real clock, so
 * it keeps going in menus, the map and cutscenes (where g_DeltaTime is 0) and
 * runs at the same speed at any frame rate. */
static void Modern_RegenUpdate(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    long long       now    = GsGetCumulativeQ12();
    q19_12          dt;
    q19_12          add;

    dt          = (s_PrevClock < 0) ? 0 : (q19_12)(now - s_PrevClock);
    s_PrevClock = now;
    if (dt < 0 || dt > MODERN_DT_MAX)
    {
        dt = 0;
    }

    /* Only during a playthrough, and never while dead. */
    if (!Pc_Modern_IsActive() || g_GameWork.gameState != GameState_InGame ||
        g_Player_IsDead || player->health <= Q12(0.0f))
    {
        s_HaveSnapshot = 0;
        s_CalmTime     = 0;
        return;
    }

    if (!s_HaveSnapshot)
    {
        s_PrevHealth   = player->health;
        s_PrevRunDist  = g_SavegamePtr->runDistance;
        s_HaveSnapshot = 1;
    }

    /* Anything that counts as fighting resets the calm timer: damage, firing
     * or attacking, aiming, running. Walking doesn't. */
    if (player->health < s_PrevHealth ||
        (player->properties.player.flags & (PlayerFlag_Shooting | PlayerFlag_DamageReceived)) ||
        g_SysWork.playerCombat.isAiming ||
        g_SavegamePtr->runDistance > s_PrevRunDist)
    {
        s_CalmTime = 0;
    }
    else
    {
        s_CalmTime += dt;
    }

    /* Cutscenes skip the wait. */
    if ((g_SysWork.sysFlags & SysFlag_CutsceneActive) && s_CalmTime < MODERN_REGEN_DELAY)
    {
        s_CalmTime = MODERN_REGEN_DELAY;
    }
    if (s_CalmTime > MODERN_REGEN_DELAY)
    {
        s_CalmTime = MODERN_REGEN_DELAY; /* keep it bounded */
    }

    if (s_CalmTime >= MODERN_REGEN_DELAY && player->health < MODERN_HEALTH_CAP)
    {
        add = dt * MODERN_REGEN_RATE;

        /* Ampoules (healthSaturation) already add g_DeltaTime this frame in the
         * player update. Don't stack: only top up to the faster rate. */
        if (g_SavegamePtr->healthSaturation > Q12(0.0f))
        {
            add -= g_DeltaTime;
        }

        if (add > 0)
        {
            player->health += add;
            if (player->health > MODERN_HEALTH_CAP)
            {
                player->health = MODERN_HEALTH_CAP;
            }
        }
    }

    s_PrevHealth  = player->health;
    s_PrevRunDist = g_SavegamePtr->runDistance;
}

void Pc_Modern_Update(void)
{
    Modern_RegenUpdate();
}
