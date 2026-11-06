// A gamepad, whatever it is (SDL3: Xbox, DualSense, DualShock 4 ...), as the mask window_pad reads.
// Masks are XInput's bits, in hp2.ini too; the triggers are made-up bits above the real ones.
#pragma once
#include <cstdint>

struct PadState {
    bool ok = false;
    int mask = 0;                                   // buttons | 0x10000 left trigger | 0x20000 right trigger
    uint8_t lt = 0, rt = 0;
    uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};   // RX RY LX LY as a DualShock's: 0..255, up 0
};

// slot 0 or 1. From the game's thread
PadState pad_read(int slot = 0);
int pad_count();                                    // how many are connected
bool pad_is_playstation(int slot = 0);              // that pad has PlayStation's buttons
void pad_rumble(int slot, int small_on, int large); // large 0..255
void pad_virtual(int slot, bool south, bool right_trigger);     // HP2_VPADS: presses on a test pad
