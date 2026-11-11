/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ppc/timebase.h - libXenon's time-base header, for libogc.
 *
 * The shared code (sh_hwperf.h, libgs_stub.c) reads the PowerPC time base
 * through libXenon's mftb()/PPC_TIMEBASE_FREQ under SH_XBOX360_PORT. The
 * instruction is the same on Gekko/Broadway; only the rate differs. The time
 * base runs at a quarter of the bus clock: 162 MHz / 4 on GameCube, 243 MHz / 4
 * on Wii (libogc's TB_TIMER_CLOCK says the same).
 */
#ifndef SH_OGC_PPC_TIMEBASE_H
#define SH_OGC_PPC_TIMEBASE_H

#if defined(SH_GC_PORT)
#define PPC_TIMEBASE_FREQ 40500000ULL
#else
#define PPC_TIMEBASE_FREQ 60750000ULL
#endif

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
