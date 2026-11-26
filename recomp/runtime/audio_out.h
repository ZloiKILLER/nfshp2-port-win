// Host audio output (platform layer). The backend pulls 48 kHz stereo s16 frames
// from the callback on its own thread. Windows: audio_wasapi.cpp.
#pragma once
#include "ps2.h"

static const int AUDIO_RATE = 48000;

typedef void (*AudioPull)(s16 *out, int frames);

// false when no output device could be opened
bool audio_out_start(AudioPull pull);

// A second output: a gamepad's own speaker. A DualSense and a DualShock 4 on a cable are sound devices of Windows as well as
// gamepads; the render endpoint that belongs to the same device as the gamepad's HID interface (`hid_path`, as SDL gives it) is
// opened and fed mono 48 kHz frames from `pull`, to the right channel alone (`right_only`: a DualSense, whose speaker is told to
// play that channel) or to both. An empty path closes it. audio_radio_open: an endpoint is open and playing now.
#include <string>
typedef void (*RadioPull)(s16 *mono, int frames);
void audio_radio_target(const std::string &hid_path, bool right_only, RadioPull pull);
bool audio_radio_open();
