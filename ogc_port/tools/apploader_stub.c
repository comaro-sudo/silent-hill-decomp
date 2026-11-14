/* SPDX-License-Identifier: CC0-1.0
 *
 * A from-scratch GameCube apploader. It is NOT Nintendo's: it only does what
 * the boot ROM contract needs -- hand the loader three callbacks, stream the
 * boot DOL's sections to their load addresses, return the DOL entry point.
 *
 * Contract (as run by real IPL and by Dolphin's BS2 emulation):
 *   entry(&init, &main, &close)   fills the three function pointers
 *   init(report)                  one-time setup
 *   main(&dst, &size, &offset)    returns 1 and describes one disc->RAM copy,
 *                                 or 0 when nothing more is to be loaded
 *   close()                       returns the program's entry point
 *
 * The DOL's disc offset is patched in by mkgcm.py over the DOL_MARK word.
 */
typedef unsigned int u32;

#define HDR ((volatile u32*)0x81210000)        /* DOL header staging (256 B) */

volatile u32 dol_off = 0x444F4C4Fu;            /* "DOLO": patched by mkgcm.py */
static int   state = 0, idx = 0;

static void a_init(void* report) { (void)report; }

static int a_main(u32* dst, u32* size, u32* off)
{
    if (state == 0) {                          /* fetch the 0x100-byte DOL header */
        *dst = (u32)HDR; *size = 0x100; *off = dol_off;
        state = 1;
        return 1;
    }
    while (idx < 18) {                         /* 7 text + 11 data sections */
        u32 o = HDR[idx], a = HDR[18 + idx], n = HDR[36 + idx];
        idx++;
        if (n) {
            *dst = a; *size = (n + 31) & ~31u; *off = dol_off + o;
            return 1;
        }
    }
    return 0;
}

static void* a_close(void) { return (void*)HDR[0xE0 / 4]; }

__attribute__((section(".text.entry")))
void entry(void (**init)(void*), int (**main)(u32*, u32*, u32*), void* (**close)(void))
{
    *init = a_init; *main = a_main; *close = a_close;
}
