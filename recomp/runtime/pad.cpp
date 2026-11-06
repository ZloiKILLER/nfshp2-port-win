// The gamepads through SDL3 (pad.h). Only its gamepad subsystem: the window is still the port's own, so
// SDL is told to listen without one having the focus; window_pad decides whether the game hears it.
// Two slots: a pad that comes keeps its slot until it goes, so a player does not change pads.
#include "pad.h"

#include "pcconfig.h"

#include <SDL3/SDL.h>
#include <cstdlib>

static SDL_Gamepad *g_pads[2];
static bool g_up;
static SDL_JoystickID g_virtual[2];

static bool up()
{
    static bool tried;
    if (!tried) {
        tried = true;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        g_up = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        // HP2_VPADS=<1 or 2>: gamepads that are not there, for testing: pad_virtual presses them
        if (g_up)
            if (const char *n = getenv("HP2_VPADS"))
                for (int i = 0; i < atoi(n) && i < 2; i++) {
                    SDL_VirtualJoystickDesc d;
                    SDL_INIT_INTERFACE(&d);
                    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
                    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    d.name = "test pad";
                    g_virtual[i] = SDL_AttachVirtualJoystick(&d);
                }
    }
    return g_up;
}

void pad_virtual(int slot, bool south, bool right_trigger)
{
    if (!g_virtual[slot] || !g_pads[slot]) return;
    SDL_Joystick *j = SDL_GetGamepadJoystick(g_pads[slot]);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, south);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, right_trigger ? 32767 : -32768);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
}

// an SDL axis (-32768..32767) as a DualShock axis, with a dead zone
static uint8_t axis(int v, int dead, int sensitivity = 100)
{
    if (v > -dead && v < dead) return 0x80;
    int s = v < 0 ? -1 : 1, m = (v < 0 ? -v : v) - dead;
    float t = (float)m / (float)(32768 - dead);
    if (sensitivity != 100) t = SDL_powf(t, 100.0f / (float)sensitivity);
    int out = 0x80 + s * (int)(t * 128);
    return (uint8_t)(out < 0 ? 0 : out > 255 ? 255 : out);
}

static const struct { SDL_GamepadButton b; int mask; } kButtons[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2},   {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8},   {SDL_GAMEPAD_BUTTON_START, 0x10},      {SDL_GAMEPAD_BUTTON_BACK, 0x20},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200}, {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
    {SDL_GAMEPAD_BUTTON_WEST, 0x4000},      {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};

// who is there now
static void refresh()
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);   // nobody reads the queue; plugging a pad fills it
    for (SDL_Gamepad *&p : g_pads)
        if (p && !SDL_GamepadConnected(p)) {
            SDL_CloseGamepad(p);
            p = nullptr;
        }
    if (g_pads[0] && g_pads[1]) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        bool have = false;
        for (SDL_Gamepad *p : g_pads) have = have || (p && SDL_GetGamepadID(p) == ids[i]);
        if (have) continue;
        for (SDL_Gamepad *&p : g_pads)
            if (!p) {
                p = SDL_OpenGamepad(ids[i]);
                break;
            }
    }
    SDL_free(ids);
}

int pad_count()
{
    if (!up()) return 0;
    refresh();
    return (g_pads[0] ? 1 : 0) + (g_pads[1] ? 1 : 0);
}

PadState pad_read(int slot)
{
    PadState ps;
    if (!up()) return ps;
    refresh();
    SDL_Gamepad *pad = g_pads[slot];
    if (!pad) return ps;
    ps.ok = true;
    for (auto &b : kButtons)
        if (SDL_GetGamepadButton(pad, b.b)) ps.mask |= b.mask;
    int lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    ps.lt = (uint8_t)((lt < 0 ? 0 : lt) * 255 / 32767);
    ps.rt = (uint8_t)((rt < 0 ? 0 : rt) * 255 / 32767);
    if (ps.lt > 30) ps.mask |= 0x10000;
    if (ps.rt > 30) ps.mask |= 0x20000;
    const int kRightDead = 8689;
    int dead = g_cfg.pad_deadzone * 32768 / 100;
    ps.sticks[0] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), kRightDead);
    ps.sticks[1] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), kRightDead);
    ps.sticks[2] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), dead, g_pad_steering_now);
    ps.sticks[3] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), dead);
    return ps;
}

bool pad_is_playstation(int slot)
{
    if (!g_pads[slot]) return false;
    switch (SDL_GetGamepadType(g_pads[slot])) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return true;
    default:
        return false;
    }
}

void pad_rumble(int slot, int small_on, int large)
{
    static int last[2] = {-1, -1};
    int now = g_cfg.pad_vibration ? (small_on ? 0x100 : 0) | (large & 255) : 0;
    if (!g_pads[slot] || now == last[slot]) return;
    last[slot] = now;
    // the game keeps asking while it wants it; a stop is a request like any other
    SDL_RumbleGamepad(g_pads[slot], (Uint16)((now & 255) * 257), now & 0x100 ? 0xFFFF : 0, now ? 0xFFFF : 0);
}
