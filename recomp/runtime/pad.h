// A gamepad, whatever it is (SDL3: Xbox, DualSense, DualShock 4 ...), as the mask window_pad reads.
// Masks are XInput's bits, in hp2.ini too; the triggers are made-up bits above the real ones.
#pragma once
#include <cstdint>
#include <string>

struct PadState {
    bool ok = false;
    int mask = 0;                                   // buttons | 0x10000 left trigger | 0x20000 right trigger
    uint8_t lt = 0, rt = 0;
    uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};   // RX RY LX LY as a DualShock's: 0..255, up 0
};

// A slot is a gamepad that is plugged in, 0 to PAD_DEVICES - 1: one that comes takes a free slot and keeps it until it goes.
// Which player has which is not this file's business (window.cpp, pad_slot: Controller > Select Device). A slot below 0 is "no
// gamepad": it reads as not there and its motors are nobody's.
// From any thread: what the gamepads' own thread (pad.cpp) saw last, never a call into SDL
enum { PAD_DEVICES = 4 };
// player: whose gamepad it is read as (0 or 1: that player's dead zones and steering, pcconfig.h); -1: nobody's, player 1's dead zone
PadState pad_read(int slot = 0, int player = -1);
int pad_count();                                    // how many are connected
// the gamepad in a slot by its name, "" with none; two of one name are told apart by a number after the second's ("... #2")
std::string pad_device_name(int slot);
bool pad_is_playstation(int slot = 0);              // that pad has PlayStation's buttons
// whose buttons that pad has: 0 Xbox's (and any pad not known), 1 PlayStation's, 2 Nintendo's (a Switch Pro Controller, Joy-Cons)
int pad_kind(int slot = 0);
void pad_rumble(int slot, int small, int large);    // each 0..255: the light motor (the high frequency) and the heavy one (the low)
void pad_rumble_quiet(bool quiet);                  // the window is not in front: the motors rest until it is
void pad_rumble_gate(int slot, bool on);            // off: this pad's player is not using it (the keyboard is)
// A DualSense (or a DualSense Edge) that takes its own effects: its triggers can be set. No other gamepad is one, and a DualSense
// that Steam hands on as an Xbox pad is not either.
bool pad_is_dualsense(int slot);
// A DualSense's two triggers. For each: 0 lets it loose; else an effect of the pad's own, its number and three numbers of it
// (pad_trigger_resistance and the like below). They stay as told until told otherwise; a pad that is not a DualSense takes
// nothing. The motors' gates are the triggers' too: none for a game behind another window, or a pad nobody holds.
void pad_triggers(int slot, unsigned left, unsigned right);
// a steady resistance from `start` of the trigger's travel on (0..255), of `force` (1..255)
inline unsigned pad_trigger_resistance(int start, int force) { return 0x01u | (unsigned)(start & 255) << 8 | (unsigned)(force & 255) << 16; }
// a vibration of the trigger from `start` of its travel on: `frequency` in Hz, `amplitude` 1..255
inline unsigned pad_trigger_vibration(int frequency, int amplitude, int start)
{
    return 0x06u | (unsigned)(frequency & 255) << 8 | (unsigned)(amplitude & 255) << 16 | (unsigned)(start & 255) << 24;
}
// A gamepad with a speaker that Windows has as a sound device: 0 none, 1 a DualSense on a cable, 2 a DualShock 4 on a cable (over
// Bluetooth neither is a sound device). Its HID interface's path, by which its sound output is found (audio_out.h). And a
// DualSense's speaker told to play the right channel of that output, or (off) nothing, as it comes: a DualShock 4's plays by itself.
int pad_speaker(int slot);
std::string pad_speaker_path(int slot);
void pad_speaker_route(int slot, bool on);
void pad_virtual(int slot, bool south, bool right_trigger);     // HP2_VPADS: presses on a test pad
void pad_settle(int ms);                            // waits, that long at most, until the pads' motors are as last asked: before the program ends
