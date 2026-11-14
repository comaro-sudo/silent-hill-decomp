/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Modern difficulty: flag, pickup scaling, hidden health regen, boss restore.
 * See pc_modern.h for the overview. */
#include "pc_modern.h"
#include "sh_log.h"

#include "game.h"
#include "bodyprog/sound/sound_system.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_data.h" /* g_DeltaTime */

#define MODERN_FLAG_ON        (1 << 0)
#define MODERN_REGEN_DELAY    Q12(8.0f)   /* calm time before health comes back */
#define MODERN_REGEN_FULL_S   120         /* seconds for a full 0 -> 100 refill (~0.83 HP/s) */
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

static int s_OfferPending;

void Pc_Modern_SetActive(int on)
{
    s_OfferPending = 0;   /* a new game or explicit choice drops a pending offer */
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

/* Ammo pickups: 1.5x the Normal count (Bullet Adjust included), rounded. */
int Pc_Modern_AmmoCount(int count)
{
    return Pc_Modern_IsActive() ? (count * 3 + 1) / 2 : count;
}

/* Health items: the Normal count (revision 2026-11-13; was x2). */
int Pc_Modern_HealthItemCount(int count)
{
    return count;
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
        add = dt * 100 / MODERN_REGEN_FULL_S;

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


/* ---- Modern offer after repeated deaths ------------------------------------
 * Two deaths within 10 minutes (real time) on Normal or Hard: the game over
 * screen asks whether to switch to Modern. Yes = the save the player continues
 * or loads next becomes Modern (Normal + flag) from that point on, so their
 * progress is kept. Starting a new game instead drops the pending switch
 * (Pc_Modern_SetActive). Easy players aren't asked: Modern isn't clearly
 * easier than Easy. */
#define MODERN_OFFER_WINDOW_Q12 ((long long)600 << 12)   /* 10 minutes */

static long long s_LastDeathQ12 = -1;
static int       s_OfferShown;          /* offer on screen, waiting for an answer */
static int       s_OfferSel = 1;        /* 0 = YES, 1 = NO (the safe default) */
/* s_OfferPending (above Pc_Modern_SetActive): accepted, apply on the next in-game frame */

void Pc_Modern_OnDeath(void)
{
    const long long now = GsGetCumulativeQ12();
    const int eligible  = g_SavegamePtr != NULL && !Pc_Modern_IsActive() &&
                          (g_SavegamePtr->gameDifficulty == GameDifficulty_Normal ||
                           g_SavegamePtr->gameDifficulty == GameDifficulty_Hard);

    if (eligible && s_LastDeathQ12 >= 0 && now - s_LastDeathQ12 <= MODERN_OFFER_WINDOW_Q12)
    {
        s_OfferShown = 1;
        s_OfferSel   = 1;
        SH_DBG("[MODERN] second death within 10 min - offering Modern");
    }
    s_LastDeathQ12 = now;
}

/* Called every frame of the GAME OVER step. Returns 1 while the offer holds
 * the screen (the caller doesn't advance), 0 once answered or if none. */
int Pc_Modern_OfferUpdate(void)
{
    if (!s_OfferShown)
    {
        return 0;
    }

    if (g_Controller0->pulsedBtnFlags & (ControllerFlag_LStickLeft | ControllerFlag_LStickRight))
    {
        s_OfferSel ^= 1;
        Sd_PlaySfx(Sfx_MenuMove, 0, 64);
    }

    Gfx_StringSetPosition(SCREEN_POSITION_X(20.0f), SCREEN_POSITION_Y(60.0f));
    Gfx_StringDraw("TRY_MODERN_DIFFICULTY?", 40);
    Gfx_StringSetPosition(SCREEN_POSITION_X(26.0f), SCREEN_POSITION_Y(67.0f));
    Gfx_StringDraw("PROGRESS_IS_KEPT", 40);
    Gfx_StringSetPosition(SCREEN_POSITION_X(33.0f), SCREEN_POSITION_Y(76.0f));
    Gfx_StringDraw(s_OfferSel == 0 ? "[YES]___NO_" : "_YES___[NO]", 40);

    if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
    {
        s_OfferShown   = 0;
        s_OfferPending = (s_OfferSel == 0);
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        SH_DBG("[MODERN] offer answered: %s", s_OfferPending ? "yes" : "no");
        return 1;                       /* this click was the answer, not "skip" */
    }
    if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
    {
        s_OfferShown = 0;
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        return 1;
    }
    return 1;
}

static void Modern_OfferApply(void)
{
    if (!s_OfferPending || g_GameWork.gameState != GameState_InGame || g_SavegamePtr == NULL)
    {
        return;
    }
    s_OfferPending = 0;
    g_SavegamePtr->gameDifficulty = GameDifficulty_Normal;   /* Modern is Normal underneath */
    Pc_Modern_SetActive(1);
    s_LastDeathQ12 = -1;
    SH_DBG("[MODERN] switched this save to Modern (progress kept)");
}

void Pc_Modern_Update(void)
{
    Modern_OfferApply();
    Modern_RegenUpdate();
}
