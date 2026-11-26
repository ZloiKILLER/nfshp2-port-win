#pragma once
#include <string>
// A steering wheel (2026-11-24). The game has its own support for two Logitech wheels (work/input_rework/docs/wheel_module.md): a
// wheel is a device type of its own (pad type 21 / 22) with its own layout, steering table, calibration screen and force feedback of
// eight effects. On the PS2 that layer talked to Logitech's driver module; in the port nothing answered, and the layer ran inert.
//
// Now a wheel that Windows knows (DirectInput: any make, with Logitech's G HUB or Gaming Software for the forces) is put into the
// game's own two WheelDevice objects, and the game's code above them does the rest as it always did: the wheel's position becomes
// its pad data, the type becomes "wheel", Car::CalcForceParameters and the rest set the effects' numbers in the objects' blocks.
// What the port does in place of the game's two functions that talked to the driver (pcwheel.cpp):
//   - UpdateLogitechWheels: which wheel is there, on which player's port;
//   - WheelDevice::ReadPort / UpdateForces: the position in, and the effects' blocks out, by the game's own rules of when (paused or
//     no race: the "quiet" blocks; a race: the race's, the crash and the shake on their flags, the gain when it changes).
// Not done as the game did: the start-up sweep (the game pushed the wheel to both stops three times to find its middle; a wheel
// of 900 degrees has no use for that), and which USB socket is which player (the first wheel is player 1's, or hp2.ini's).
// With no wheel connected the game's own function runs as before: nothing changes for anybody without one.
//
// wheel_win.cpp is the Windows side, a thread of its own: the device's state in, the effects out.

struct PcWheelInput {
    int axis[8];                // 0..65535 each: X Y Z Rx Ry Rz, the two sliders
    unsigned buttons;           // bit n: button n is down
    int hat;                    // 0..7 from up, clockwise; -1: centred
    bool forces;                // the device takes effects
};

// an effect as the game's lgDevForceEffect has it (wheel_module.cpp): the type, the duration in ms (-1: endless), twelve numbers
struct PcWheelEffect {
    int type, duration, p[12];
};

struct PcWheelForces {
    PcWheelEffect fx[8];        // the slots: 0 the spring, 1 the damper, 2 the crash, 3 4 the road, 6 the shake
    unsigned set;               // bit n: slot n's block is given here
    unsigned start;             // bit n: start slot n now
    int gain;                   // 0..255, -1: as it is
    int strength;               // the setting, percent
};

void pc_wheel_start();                              // the thread, once (the window is there)
void pc_wheel_devices_changed();                    // Windows says a device came or went: look for a wheel again
int pc_wheel_range_set();                           // degrees the wheel's rotation was set to through Logitech's software (wheel_win.cpp); 0: not set
int pc_wheel_range_known();                         // degrees Logitech's software says the wheel turns, where the game did not set it; 0: not known
void pc_wheel_quit();                               // the game goes: a rotation the game set is put back
void pc_wheel_lock_changed();                       // the menu changed wheel_lock or wheel_set_range: the wheel is set again
bool pc_wheel_input(int index, PcWheelInput &out);  // wheel 0 or 1: false when there is none
void pc_wheel_forces(int index, const PcWheelForces &f);
const char *pc_wheel_name(int index);               // "" when there is none
// Whose a wheel is (pcwheel.cpp; the Controller screen as the wheels', pcmenu.cpp): a player's choice as hp2.ini has it ("" Automatic,
// "none", or a wheel's name); the choice after one, left or right, through Automatic, None and each wheel connected; a choice
// made (never the wheel the other player has by name: the two change places); and the wheel a player has now (0, 1, or -1).
std::string pc_wheel_value(int player);
std::string pc_wheel_step(const std::string &from, int dir);
void pc_wheel_set(int player, const std::string &value);
int pc_wheel_of(int player);
bool pc_wheel_any();                                // a wheel is connected, anybody's or nobody's
int pc_wheel_owner(int index);                      // the player whose settings wheel 0 or 1 goes by: who has it, player 1 where nobody has
