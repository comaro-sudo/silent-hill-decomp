/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * audio_wiiu.c - software SPU (xbox_port/src/audio_xbox.c) -> SDL2 audio queue,
 * 48 kHz stereo s16 in host (big-endian) order, which SDL's Wii U backend
 * hands to AX. Same pump model as the other console builds: render on the main
 * thread once per frame until about 85 ms is queued.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <coreinit/cache.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>

#include "sh_log.h"

extern const char* Sh360Fs_DataRoot(void);
extern short* g_AudioRadioOut;
extern int    g_AudioRadioMode;
extern int    g_AudioRadioPeak;

extern void Audio_RenderInto(short* out, int frames);
extern void Xa_XboxPump(void);

#define CHUNK_FRAMES 256
#define TARGET_BYTES (CHUNK_FRAMES * 4 * 16)

static SDL_AudioDeviceID s_dev;

/* --- Pocket radio through the GamePad speaker ------------------------------
 * The software SPU keys the radio's interference / static loops on reserved
 * voices 22 and 23. audio_xbox.c can sum those two to a separate mono stream
 * (g_AudioRadioOut); here it is written into a ring that one looping AX voice
 * plays, routed to the DRC (GamePad) device only. SDL's own AX voices carry
 * the rest of the game to TV + GamePad as before.
 *
 * silenthill.cfg: radio_gamepad=0 (everything in the one mix), 1 (default:
 * GamePad only, like a handheld radio), 2 (GamePad and TV). */
#define RADIO_RING 8192          /* samples, mono s16; ~170 ms */
#define RADIO_LEAD 2048          /* keep this far ahead of the play head */

static AXVoice*  s_radioVoice;
static short*    s_radioRing;
static unsigned long long s_radioWr;   /* absolute samples written */
static int       s_radioMode;
static int       s_radioLogT;

static int RadioCfgMode(void)
{
    char path[512], line[256];
    int  mode = 1;
    FILE* f;
    snprintf(path, sizeof(path), "%ssilenthill.cfg", Sh360Fs_DataRoot());
    f = fopen(path, "rb");
    if (!f) return mode;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "radio_gamepad", 13) == 0) {
            const char* p = strchr(line, '=');
            if (p) mode = atoi(p + 1);
        }
    }
    fclose(f);
    return mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

static void RadioInit(void)
{
    AXVoiceOffsets    off;
    AXVoiceDeviceMixData mix;
    AXVoiceVeData     ve;

    s_radioMode = RadioCfgMode();
    SH_DBG("[AUD] radio_gamepad=%d", s_radioMode);
    if (!s_radioMode) return;

    if (!AXIsInit()) AXInit();   /* SDL normally did this already */
    s_radioRing = (short*)memalign(64, RADIO_RING * 2);
    if (!s_radioRing) return;
    memset(s_radioRing, 0, RADIO_RING * 2);
    DCFlushRange(s_radioRing, RADIO_RING * 2);

    s_radioVoice = AXAcquireVoice(31, NULL, NULL);
    if (!s_radioVoice) {
        SH_DBG("[AUD] radio: AXAcquireVoice failed, radio stays in the main mix");
        return;
    }
    AXVoiceBegin(s_radioVoice);
    AXSetVoiceType(s_radioVoice, 0);
    ve.volume = 0x8000;
    ve.delta  = 0;
    AXSetVoiceVe(s_radioVoice, &ve);
    off.dataType       = AX_VOICE_FORMAT_LPCM16;
    off.loopingEnabled = AX_VOICE_LOOP_ENABLED;
    off.loopOffset     = 0;
    off.endOffset      = RADIO_RING - 1;
    off.currentOffset  = 0;
    off.data           = s_radioRing;
    AXSetVoiceOffsets(s_radioVoice, &off);
    AXSetVoiceSrcType(s_radioVoice, AX_VOICE_SRC_TYPE_NONE);
    AXSetVoiceSrcRatio(s_radioVoice, 1.0f);
    memset(&mix, 0, sizeof(mix));
    AXSetVoiceDeviceMix(s_radioVoice, AX_DEVICE_TYPE_TV, 0, &mix);
    mix.bus[0].volume = 0x8000;   /* mono voice on both GamePad channels */
    mix.bus[1].volume = 0x8000;
    AXSetVoiceDeviceMix(s_radioVoice, AX_DEVICE_TYPE_DRC, 0, &mix);
    if (s_radioMode == 2) {
        AXSetVoiceDeviceMix(s_radioVoice, AX_DEVICE_TYPE_TV, 0, &mix);
    }
    AXSetVoiceState(s_radioVoice, AX_VOICE_STATE_PLAYING);
    AXVoiceEnd(s_radioVoice);
    s_radioWr = RADIO_LEAD;
    SH_DBG("[AUD] radio: AX voice up, DRC%s, ring %d samples",
           s_radioMode == 2 ? "+TV" : " only", RADIO_RING);
}

/* Append n mono samples (already rendered) to the ring, keeping the write head
 * RADIO_LEAD..RADIO_RING-n ahead of the voice's play head. */
static void RadioPush(const short* src, int n)
{
    AXVoiceOffsets off;
    unsigned long long playPos;
    static unsigned int lastOff;
    static unsigned long long laps;
    long long ahead;
    int i;

    AXGetVoiceOffsets(s_radioVoice, &off);
    if (off.currentOffset < lastOff) laps++;      /* the ring wrapped */
    lastOff = off.currentOffset;
    playPos = laps * RADIO_RING + off.currentOffset;
    ahead   = (long long)s_radioWr - (long long)playPos;
    if (ahead < 256 || ahead > RADIO_RING - 1024) {
        /* underrun, or the head ran away: resync with a small gap of silence */
        for (i = 0; i < RADIO_LEAD; i++)
            s_radioRing[(playPos + i) % RADIO_RING] = 0;
        s_radioWr = playPos + RADIO_LEAD;
    }
    for (i = 0; i < n; i++)
        s_radioRing[(s_radioWr + i) % RADIO_RING] = src[i];
    s_radioWr += n;
    DCFlushRange(s_radioRing, RADIO_RING * 2);
}

void Audio360_Init(void)
{
    SDL_AudioSpec want, have;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SH_DBG("[AUD] SDL audio init failed: %s", SDL_GetError());
        return;
    }
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_dev) {
        SH_DBG("[AUD] SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    RadioInit();
    if (s_radioVoice) {
        g_AudioRadioMode = s_radioMode;
        g_AudioRadioOut  = (short*)memalign(32, CHUNK_FRAMES * 2);
        if (!g_AudioRadioOut) { g_AudioRadioMode = 0; }
    }
    SH_DBG("[AUD] SDL audio up: %d Hz, %d ch, format 0x%x", have.freq, have.channels, (unsigned)have.format);
}

void Audio_XboxPump(void)
{
    static short chunk[CHUNK_FRAMES * 2];
    int guard = 0;
    if (!s_dev) return;
    Xa_XboxPump();
    while (SDL_GetQueuedAudioSize(s_dev) < TARGET_BYTES && guard++ < 16) {
        Audio_RenderInto(chunk, CHUNK_FRAMES);
        if (g_AudioRadioOut && s_radioVoice) {
            RadioPush(g_AudioRadioOut, CHUNK_FRAMES);
            if (g_AudioRadioPeak > 0 || (++s_radioLogT % 4096) == 0) {
                static int wasOn, burstMax;
                if (g_AudioRadioPeak > burstMax) burstMax = g_AudioRadioPeak;
                if ((g_AudioRadioPeak > 64) != wasOn) {
                    wasOn = g_AudioRadioPeak > 64;
                    /* OFF reports the loudest sample of the burst (of 32767) */
                    if (wasOn) SH_DBG("[AUD] radio static ON on GamePad");
                    else       SH_DBG("[AUD] radio static OFF on GamePad (burst peak %d)", burstMax);
                    burstMax = 0;
                }
            }
        }
        SDL_QueueAudio(s_dev, chunk, sizeof(chunk));
    }
}

void Audio360_DropQueued(void)
{
    if (s_dev) SDL_ClearQueuedAudio(s_dev);
}
