// A steering wheel, the game's side (pcwheel.h): a wheel Windows has is put into the game's own WheelDevice objects.
#include "pcwheel.h"

#include "hw.h"
#include "pcconfig.h"
#include "pcmenu.h"
#include "ps2.h"

#include <cstdio>
#include <cstring>

extern FILE *g_hle_log;

namespace {

// the game's (work/input_rework/docs/wheel_module.cpp)
const u32 kWheels = 0x2ef6c0, kStride = 1092;       // WheelDevice LogitechWheels[2]
const u32 W_PORT = 4, W_DEVICE = 8, W_TYPE = 12, W_POSITION = 36, W_NUMBER = 52, W_RACE = 88, W_QUIET = 568, W_GAIN = 1048, W_SWEEP = 1052,
          W_RESTART = 1068, W_SHAKE = 1072, W_CRASH = 1076, W_GAIN_DIRTY = 1080;
const u32 kNumLogitechDevices = 0x32fd88;           // gp-23400
const u32 kTrackSpecificLoaded = 0x330540;
const u32 kHatToPadMask = 0x2de5f8;                 // u16[8]: [1], up and right, is Up+Left on the disc (a slip): put right here

bool g_mine;                                        // the wheels in the game's objects are the port's

void read_effect(u32 at, PcWheelEffect &e)
{
    e.type = LB(at);
    e.duration = (s32)LW(at + 4);
    for (u32 k = 0; k < 12; k++) e.p[k] = (s32)LW(at + 12 + k * 4);
}

// a pedal's axis as the game's byte: 255 is down. Logitech's pedals rest at the top of their axis
u8 pedal(const PcWheelInput &in, int axis)
{
    if (axis < 0 || axis > 7) return 0;
    int v = in.axis[axis];
    if (g_cfg.wheel_pedals_inverted) v = 65535 - v;
    v = v < 0 ? 0 : v > 65535 ? 65535 : v;
    return (u8)(v >> 8);
}

// The wheel's angle as the game's byte, 128 its middle. The game's wheels turned 200 degrees lock to lock; hp2.ini says how far this
// one turns in all (wheel_range, what the maker's software is set to: 900 for a G920) and how much of that is the game's full lock
// (wheel_lock).
// How far the wheel turns in all, in degrees: what it was set to through Logitech's software, or what that says it is
// (wheel_win.cpp), else hp2.ini's word
int range_now(int i)
{
    // (Logitech's word is the first wheel's alone: wheel_win.cpp asks and sets that one)
    const int set = i == 0 ? pc_wheel_range_set() : 0, known = i == 0 ? pc_wheel_range_known() : 0;
    return set ? set : known ? known : g_cfg.wheel_range > 0 ? g_cfg.wheel_range : 900;
}

double g_turn[2];                                   // each wheel's turn, -1 to 1 of all its range (the calibration screen's picture)
int g_shown = -1;                                   // the wheel that picture is of where the screen's player has none: the first there is

// Whose a wheel is (the user, 2026-11-24: the wheels' screen "in the likeness of the gamepads'", with a Select Device for each
// player). A player's choice is hp2.ini's wheel1 / wheel2: nothing is Automatic, "none", or a wheel's name. A named wheel that is
// connected is its player's (player 1's when both name it); then Automatic takes the first wheel nobody has, player 1 before
// player 2: one wheel and nothing chosen is player 1's, as it was before there was a choice.
std::string wheel_title(int i)
{
    std::string name = pc_wheel_name(i);
    if (name.empty()) return name;
    if (i == 1 && name == pc_wheel_name(0)) name += " #2";      // two of a kind
    if (name.size() > 120) name.resize(120);
    return name;
}
int g_of[2] = {-1, -1};                             // the wheel each player has now: 0, 1, or -1
bool g_any;                                         // a wheel is connected
void wheels_of(const bool have[2], int of[2])
{
    of[0] = of[1] = -1;
    for (int p = 0; p < 2; p++) {
        const std::string want = g_cfg.wheel_device[p];
        if (want.empty() || want == "none") continue;
        for (int i = 0; i < 2; i++)
            if (have[i] && of[0] != i && want == wheel_title(i)) of[p] = i;
    }
    for (int p = 0; p < 2; p++) {
        if (g_cfg.wheel_device[p][0]) continue;
        for (int i = 0; i < 2 && of[p] < 0; i++)
            if (have[i] && of[0] != i && of[1] != i) of[p] = i;
    }
}

// The wheel and the gamepad of one player (the user, 2026-11-24: "let both work at once, the wheel and the gamepad, for player 2 as
// well: a wheel is an extra device all the same"). The game has one device a port: a wheel there makes the port a wheel's (its own
// layout, its steering table, the forces), and a gamepad on the same port is then heard for its buttons only. So the port is
// given to whichever of the two is in use. The wheel has it while it is being used: turned by a hundredth of its range, a pedal
// down, a button; left alone for half a second, it gives the port up when the gamepad is touched, and takes it back at once when
// it is used again. Without a gamepad in use the wheel keeps the port. hp2.ini's wheel_shared=0: the wheel always has it.
bool wheel_has_port(int i, int port, const PcWheelInput &in)
{
    static bool mine[2] = {true, true};
    static double stood[2];
    static int quiet[2];
    const std::string screen = pc_screen();
    // The wheels' own screens are the wheel's: the game's calibration screen (it asks the port's device whether it can be calibrated
    // at every step), and the Controller screen opened as the wheels', from which that one is opened. The calibration screen is
    // made for the device the port has as it is opened: with the port given to the gamepad that had just pressed Select on the
    // row, it was made for a gamepad, which has nothing to calibrate, and nothing came up (the user, 2026-11-24, three builds
    // running: "the wheels' Calibration still does not open". It did open before the wheel and the gamepad shared a port.)
    // (JoystickCalibrationScreen's tick, 0x23f3a0, asks the port's type and whether that can be calibrated, and with a gamepad's
    // leaves at once. The half second of quiet starts anew here, so the port stays the wheel's through the change of screens.)
    if (screen.rfind("Joystick Calibration", 0) == 0 || (screen == "Joystick PreConfig" && pc_controller_wheels())) {
        quiet[i] = 0;
        return mine[i] = true;
    }
    // The gamepads' own screens are the gamepad's, a wheel on the same port or not: a player's layouts, and the Controller screen
    // opened as the gamepads'. With a wheel on the port the game showed the wheel's layouts there, of which there is one (the
    // user, 2026-11-24, with the pretend wheel as player 1's: "the first gamepad's layouts cannot be chosen, only A").
    if (screen.rfind("Joystick Config", 0) == 0 || (screen == "Joystick PreConfig" && !pc_controller_wheels())) return false;
    if (!g_cfg.wheel_shared) return true;
    const double turn = (in.axis[0] - 32767.5) / 32767.5;
    bool busy = in.buttons != 0 || in.hat >= 0 || pedal(in, g_cfg.wheel_gas) > 12 || pedal(in, g_cfg.wheel_brake) > 12;
    if (turn - stood[i] > 0.01 || stood[i] - turn > 0.01) busy = true, stood[i] = turn;
    quiet[i] = busy ? 0 : quiet[i] < 1000000 ? quiet[i] + 1 : quiet[i];
    if (busy) mine[i] = true;
    else if (mine[i] && quiet[i] > 30 && window_pad_busy(port)) mine[i] = false;
    return mine[i];
}

u8 steer(const PcWheelInput &in, int i, int player)
{
    double t = (in.axis[0] - 32767.5) / 32767.5;
    // (where the wheel's rotation was set to the lock itself through Logitech's software, wheel_win.cpp, the whole axis is the lock)
    const int set = i == 0 ? pc_wheel_range_set() : 0;
    const int range = range_now(i), lock = set ? set : g_cfg.wheel_lock[player] > 0 ? g_cfg.wheel_lock[player] : range;
    t *= (double)range / (double)lock;
    if (g_cfg.wheel_invert[player]) t = -t;
    t = t < -1 ? -1 : t > 1 ? 1 : t;
    const int v = (int)(128.0 + t * 127.0 + (t >= 0 ? 0.5 : -0.5));
    return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// The wheel's buttons as the game's "device 18" has them (Device18ButtonToPadBit): 0 Cross, 1 Square, 2 Circle, 3 Triangle, 4 Start,
// 5 Select, 6 L1 (shift down), 7 R1 (shift up), 8 L2, 9 R2. A Logitech G920's, as DirectInput numbers them: A B X Y, the right and
// the left paddle, Menu, View, the right and the left stalk button.
u16 buttons(const PcWheelInput &in)
{
    static const int kMap[10] = {0, 2, 1, 3, 7, 6, 4, 5, 9, 8};
    u16 b = 0;
    for (int i = 0; i < 10; i++)
        if (in.buttons >> i & 1) b |= (u16)(1u << kMap[i]);
    return b;
}

void let_go()
{
    for (u32 i = 0; i < 2; i++) {
        const u32 w = kWheels + i * kStride;
        SW(w + W_PORT, (u32)-1);
        SW(w + W_DEVICE, (u32)-1);
        SW(w + W_NUMBER, 0);
    }
    SW(kNumLogitechDevices, 0);
    g_mine = false;
}

}   // namespace

std::string pc_wheel_value(int player) { return g_cfg.wheel_device[player & 1]; }
std::string pc_wheel_step(const std::string &from, int dir)
{
    std::string list[4] = {"", "none"};
    int n = 2;
    for (int i = 0; i < 2; i++)
        if (!wheel_title(i).empty()) list[n++] = wheel_title(i);
    int at = 0;                                         // a wheel chosen and not connected: from Automatic on
    for (int i = 0; i < n; i++)
        if (list[i] == from) at = i;
    return list[(at + (dir < 0 ? n - 1 : 1)) % n];
}
void pc_wheel_set(int player, const std::string &value)
{
    player &= 1;
    const std::string mine = g_cfg.wheel_device[player], theirs = g_cfg.wheel_device[1 - player];
    if (!value.empty() && value != "none" && value == theirs)
        snprintf(g_cfg.wheel_device[1 - player], sizeof g_cfg.wheel_device[0], "%s", mine.c_str());
    snprintf(g_cfg.wheel_device[player], sizeof g_cfg.wheel_device[0], "%s", value.c_str());
    pc_wheel_lock_changed();                // (the first wheel may be another player's now: its rotation by that player's settings)
}
int pc_wheel_of(int player) { return g_of[player & 1]; }
bool pc_wheel_any() { return g_any; }
int pc_wheel_owner(int index) { return g_of[1] == index ? 1 : 0; }

// UpdateLogitechWheels (0x12c080), the first thing of PrepareToGetJoystickData every frame.
void HOOK_UpdateLogitechWheels__Fv(Ctx &c, void (*orig)(Ctx &))
{
    PcWheelInput in[2];
    bool have[2] = {false, false};
    if (g_cfg.wheel)
        for (int i = 0; i < 2; i++) have[i] = pc_wheel_input(i, in[i]);
    g_shown = have[0] ? 0 : have[1] ? 1 : -1;
    for (int i = 0; i < 2; i++)
        if (have[i]) g_turn[i] = (in[i].axis[0] - 32767.5) / 32767.5 * (g_cfg.wheel_invert[pc_wheel_owner(i)] ? -1.0 : 1.0);
    int of[2];
    wheels_of(have, of);
    g_of[0] = of[0], g_of[1] = of[1];
    g_any = have[0] || have[1];
    if (!have[0] && !have[1]) {
        // no wheel: the game's own function, as it always ran (it finds none)
        if (g_mine) {
            let_go();
            fprintf(g_hle_log, "wheel: none any more\n");
        }
        orig(c);
        return;
    }
    if (!g_mine) {
        g_mine = true;
        SH(kHatToPadMask + 2, 0x30);
    }
    {   // whose each is, said when it changes
        static int told[2] = {-1, -1};
        for (int p = 0; p < 2; p++) {
            if (of[p] == told[p]) continue;
            told[p] = of[p];
            if (of[p] >= 0) fprintf(g_hle_log, "wheel: %s is the game's wheel now (player %d)\n", pc_wheel_name(of[p]), p + 1);
            else fprintf(g_hle_log, "wheel: player %d has none now\n", p + 1);
            fflush(g_hle_log);
        }
    }
    const u32 gp = c.r[28].w[0];
    const u32 flow = LW(gp - 22216);                                    // IsGameFlowPaused (0x1680b8)
    const bool quiet = (flow && LW(flow) == 6) || !LW(kTrackSpecificLoaded);
    u32 count = 0;
    for (int i = 0; i < 2; i++) {
        const u32 w = kWheels + (u32)i * kStride;
        if (!have[i]) {
            SW(w + W_PORT, (u32)-1);
            SW(w + W_DEVICE, (u32)-1);
            SW(w + W_NUMBER, 0);
            continue;
        }
        // whose it is (wheels_of): the wheel of a player is on that player's port
        const s32 port = of[0] == i ? 0 : of[1] == i ? 1 : -1;
        if (port < 0 || !wheel_has_port(i, port, in[i])) {
            // nobody's, or the player's gamepad has the port now: the wheel is not in the game's objects, and is let loose (no force)
            SW(w + W_PORT, (u32)-1);
            SW(w + W_DEVICE, (u32)-1);
            SW(w + W_NUMBER, 0);
            PcWheelForces loose = {};
            loose.gain = 0;
            loose.strength = 0;
            pc_wheel_forces(i, loose);
            continue;
        }
        const bool fresh = (s32)LW(w + W_DEVICE) == -1;
        SW(w + W_PORT, (u32)port);
        SW(w + W_DEVICE, (u32)(1 + i));
        SW(w + W_TYPE, 18);                                             // pad type 22, layout 0x33
        if (fresh) {
            SW(w + W_SWEEP, 4);                                         // the start-up sweep: done, never begun
            SW(w + W_RESTART, 1);
        }
        count++;
        // WheelDevice::UpdateForces (0x12b9e8), without DoRightLeftInit
        PcWheelForces f = {};
        f.gain = -1;
        f.strength = g_cfg.wheel_ffb[port] ? g_cfg.wheel_force[port] : 0;         // (the wheels' screen: this player's Force Feedback, On or Off)
        if (quiet) {
            for (u32 k : {0u, 1u, 3u, 4u, 6u, 2u}) {
                read_effect(w + W_QUIET + k * 60, f.fx[k]);
                f.set |= 1u << k;
            }
            f.gain = 255;
        } else {
            for (u32 k : {0u, 1u, 3u, 4u}) {
                read_effect(w + W_RACE + k * 60, f.fx[k]);
                f.set |= 1u << k;
            }
            if (LW(w + W_RESTART)) {
                f.start |= 1u << 0 | 1u << 1 | 1u << 3 | 1u << 4;
                SW(w + W_RESTART, 0);
            }
            if (LW(w + W_SHAKE)) {
                read_effect(w + W_RACE + 6 * 60, f.fx[6]);
                f.set |= 1u << 6;
                f.start |= 1u << 6;
                SW(w + W_SHAKE, 0);
            }
            if (LW(w + W_CRASH)) {
                read_effect(w + W_RACE + 2 * 60, f.fx[2]);
                f.set |= 1u << 2;
                f.start |= 1u << 2;
                SW(w + W_CRASH, 0);
            }
            // the gain: sent when it changed there; here every frame, the Windows side sends it on when it is another
            f.gain = (s32)LW(w + W_GAIN);
            SW(w + W_GAIN_DIRTY, 0);
        }
        pc_wheel_forces(i, f);
        // WheelDevice::ReadPort's read: the position (lgDevPosition). Its bytes become pad bytes 4..19: the two that are a pad's
        // left stick and the first one stay at their middle, so that no stick seems pushed
        u8 pos[16] = {};
        pos[0] = 128;
        pos[2] = 128;
        pos[3] = 128;
        pos[1] = steer(in[i], i, port);
        pos[7] = pedal(in[i], g_cfg.wheel_gas);
        pos[8] = pedal(in[i], g_cfg.wheel_brake);
        pos[9] = in[i].hat >= 0 && in[i].hat < 8 ? (u8)in[i].hat : 8;
        const u16 b = buttons(in[i]);
        memcpy(pos + 14, &b, 2);
        memcpy(host(w + W_POSITION), pos, 16);
    }
    SW(kNumLogitechDevices, count);
}

// JoystickCalibrationScreen::DrawSteeringWheel (0x23fde0): the wheel's picture on the calibration screen. The game turns it by the
// steering byte, (byte - 128) * 0.703125 degrees: a quarter turn each way at the most, which was near enough to its own wheels' 200
// degrees lock to lock. A wheel of Windows turns three or nine hundred, and a tester (2026-11-24) saw the picture stop long before
// his hands did. Here the picture turns as the wheel does, degree for degree: its place on its axis times half of what it turns in
// all (range_now: Logitech's software's word, else hp2.ini's wheel_range). The game's own call is made first (it shows or hides the
// picture, and turns it its way); then the angle is set again. With no wheel of the port's: the game's alone.
// (FlailerSetRotation, 0x144928, takes whole degrees; the object's name is the one the game's function gives.)
void HOOK_DrawSteeringWheel__25JoystickCalibrationScreenb(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0];
    const bool shown = c.r[5].w[0] != 0;
    orig(c);
    // the wheel of the screen's player (the screen's name says whose port it is), else the first there is
    int wheel = g_of[pc_screen().find("Port2") != std::string::npos ? 1 : 0];
    if (wheel < 0) wheel = g_shown;
    if (!shown || !g_mine || wheel < 0 || !g_cfg.wheel) return;
    double degrees = g_turn[wheel] * 0.5 * (double)range_now(wheel);
    degrees = degrees < -1260.0 ? -1260.0 : degrees > 1260.0 ? 1260.0 : degrees;
    c.r[4].d[0] = (u64)(s64)(s32)LW(self + 8);
    c.r[5].d[0] = (u64)(s64)(s32)0x0b2ad6dfu;
    c.f[12].f = (float)degrees;
    ps2_call(c, 0x144928);
}
