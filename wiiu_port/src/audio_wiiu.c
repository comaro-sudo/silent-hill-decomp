/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * audio_wiiu.c - software SPU (xbox_port/src/audio_xbox.c) -> SDL2 audio queue,
 * 48 kHz stereo s16 in host (big-endian) order, which SDL's Wii U backend
 * hands to AX. Same pump model as the other console builds: render on the main
 * thread once per frame until about 85 ms is queued.
 */
#include <SDL.h>

#include "sh_log.h"

extern void Audio_RenderInto(short* out, int frames);
extern void Xa_XboxPump(void);

#define CHUNK_FRAMES 256
#define TARGET_BYTES (CHUNK_FRAMES * 4 * 16)

static SDL_AudioDeviceID s_dev;

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
        SDL_QueueAudio(s_dev, chunk, sizeof(chunk));
    }
}

void Audio360_DropQueued(void)
{
    if (s_dev) SDL_ClearQueuedAudio(s_dev);
}
