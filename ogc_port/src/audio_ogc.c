/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * audio_ogc.c - output pump for the software SPU on GameCube/Wii.
 *
 * The PSX SPU itself (voices, ADSR, VAG, reverb) is xbox_port/src/audio_xbox.c,
 * shared with the Xbox and 360 builds; it renders 48 kHz stereo s16 through
 * Audio_RenderInto(). The Gekko/Broadway audio interface plays exactly that
 * natively (AI_SAMPLERATE_48KHZ, big-endian s16 interleaved -- host order on
 * this CPU), so there is no resample or swap stage.
 *
 * Same shape as the 360 pumps: the main loop renders into a ring once per frame
 * (Audio_XboxPump), and the AI's DMA-done interrupt drains it one block at a
 * time. Rendering stays on the main thread because the software SPU is not
 * reentrant; the interrupt only copies. An empty ring plays silence rather than
 * stalling, so a long blocking disc read costs a gap, not a hang.
 */
#include <string.h>
#include <stdint.h>
#include <gccore.h>
#include <ogc/audio.h>
#include <ogc/cache.h>

#include "sh_log.h"

extern void Audio_RenderInto(short* out, int frames);
extern void Xa_XboxPump(void);       /* xa_xbox.c: decode XA ahead of the mixer */

#define DMA_FRAMES    256                            /* 5.3 ms per DMA block */
#define DMA_BYTES     (DMA_FRAMES * 4)               /* stereo s16 */
#define RING_FRAMES   (DMA_FRAMES * 32)              /* ~170 ms */
#define TARGET_FRAMES (DMA_FRAMES * 16)              /* keep ~85 ms queued */

static short s_dma[2][DMA_FRAMES * 2] ATTRIBUTE_ALIGN(32);
static int   s_dmaIdx;
static short s_ring[RING_FRAMES * 2];
static volatile unsigned s_rd, s_wr;                 /* frame counters, wrap freely */
static int   s_ready;
static unsigned s_underruns;

static void DmaDone(void)
{
    short*   out   = s_dma[s_dmaIdx];
    unsigned avail = s_wr - s_rd;
    unsigned n     = avail < DMA_FRAMES ? avail : DMA_FRAMES;
    unsigned i;

    for (i = 0; i < n; i++) {
        unsigned p = ((s_rd + i) % RING_FRAMES) * 2;
        out[i * 2]     = s_ring[p];
        out[i * 2 + 1] = s_ring[p + 1];
    }
    if (n < DMA_FRAMES) {
        memset(out + n * 2, 0, (DMA_FRAMES - n) * 4);
        s_underruns++;
    }
    s_rd += n;
    DCFlushRange(out, DMA_BYTES);
    AUDIO_InitDMA((u32)out, DMA_BYTES);
    s_dmaIdx ^= 1;
}

void Audio360_Init(void)
{
    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    memset(s_dma, 0, sizeof(s_dma));
    DCFlushRange(s_dma, sizeof(s_dma));
    AUDIO_RegisterDMACallback(DmaDone);
    s_dmaIdx = 0;
    AUDIO_InitDMA((u32)s_dma[0], DMA_BYTES);
    AUDIO_StartDMA();
    s_ready = 1;
    SH_DBG("[AUD] AI DMA up: 48000 Hz stereo, %d-frame blocks, %d-frame ring", DMA_FRAMES, RING_FRAMES);
}

/* Once per frame (audio_xbox.c / the game loop call it under this name). */
void Audio_XboxPump(void)
{
    static short chunk[DMA_FRAMES * 2];
    int guard = 0;

    if (!s_ready)
        return;
    Xa_XboxPump();

    while ((s_wr - s_rd) < TARGET_FRAMES && guard++ < TARGET_FRAMES / DMA_FRAMES) {
        unsigned i, w = s_wr;
        Audio_RenderInto(chunk, DMA_FRAMES);
        for (i = 0; i < DMA_FRAMES; i++) {
            unsigned p = ((w + i) % RING_FRAMES) * 2;
            s_ring[p]     = chunk[i * 2];
            s_ring[p + 1] = chunk[i * 2 + 1];
        }
        s_wr = w + DMA_FRAMES;            /* publish after the copy */
    }

    {
        static unsigned s_frames, s_lastUnder;
        if ((++s_frames % 1800) == 0 && s_underruns != s_lastUnder) {
            SH_DBG("[AUD] underruns so far: %u", s_underruns);
            s_lastUnder = s_underruns;
        }
    }
}

/* XaPlayer_Stop: drop what is rendered but not heard yet, or the tail of the
 * stopped line plays over the next one. */
void Audio360_DropQueued(void)
{
    u32 level = IRQ_Disable();
    s_rd = s_wr;
    IRQ_Restore(level);
}
