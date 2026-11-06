// Host audio output (platform layer). The backend pulls 48 kHz stereo s16 frames
// from the callback on its own thread. Windows: audio_wasapi.cpp.
#pragma once
#include "ps2.h"

static const int AUDIO_RATE = 48000;

typedef void (*AudioPull)(s16 *out, int frames);

// false when no output device could be opened
bool audio_out_start(AudioPull pull);
