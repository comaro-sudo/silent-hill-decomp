/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ppc/timebase.h - PowerPC time-base helper for Wii U (Espresso CPU).
 *
 * The shared code (sh_hwperf.h, libgs_stub.c) reads the PowerPC time base
 * through mftb()/PPC_TIMEBASE_FREQ under SH_XBOX360_PORT. On Wii U Espresso,
 * the time base runs at a quarter of the 248.625 MHz bus clock.
 */
#ifndef SH_WIIU_PPC_TIMEBASE_H
#define SH_WIIU_PPC_TIMEBASE_H

#define PPC_TIMEBASE_FREQ 62156250ULL   /* Espresso: 248.625 MHz bus / 4 */

static inline unsigned long long mftb(void)
{
    unsigned int hi, lo, hi2;
    do {
        __asm__ __volatile__("mftbu %0" : "=r"(hi));
        __asm__ __volatile__("mftb %0"  : "=r"(lo));
        __asm__ __volatile__("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((unsigned long long)hi << 32) | lo;
}

#endif
