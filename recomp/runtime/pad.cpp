// The gamepads through SDL3 (pad.h). Only its gamepad subsystem: the window is still the port's own, so
// SDL is told to listen without one having the focus; window_pad decides whether the game hears it.
// Two slots: a pad that comes keeps its slot until it goes, so a player does not change pads.
//
// All of SDL runs on a thread of its own, and the game reads what that thread last saw. Finding a pad that was just plugged in and opening
// it are calls into Windows and into the device that can take seconds (DirectInput's enumeration, a pad's handshake over Bluetooth, a driver
// being installed, a pad that comes and goes): on the game's thread they stopped the game for as long, which a tester saw as the game
// hanging when a pad was connected while it ran. Here they stop only this thread; the game sees no pad a moment longer.
#include "pad.h"

#include "pcconfig.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

// a pad as SDL gives it, before the settings' dead zone and steering
struct Raw {
    bool ok = false, playstation = false;
    int mask = 0;
    int lt = 0, rt = 0;
    int axis[4] = {};       // RX RY LX LY
};

std::mutex g_lock;          // g_seen
Raw g_seen[2];
std::atomic<int> g_count{0};
std::atomic<int> g_want_rumble[2];      // what the game asked for last: small << 8 | large, -1 before any
std::atomic<int> g_done_rumble[2];      // what the pad was told last
std::atomic<int> g_virtual_press[2];    // HP2_VPADS: bit 0 south, bit 1 the right trigger
std::atomic<bool> g_started{false};

// the thread's own
SDL_Gamepad *g_pads[2];
SDL_JoystickID g_virtual[2];

const struct { SDL_GamepadButton b; int mask; } kButtons[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2},   {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8},   {SDL_GAMEPAD_BUTTON_START, 0x10},      {SDL_GAMEPAD_BUTTON_BACK, 0x20},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200}, {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
    {SDL_GAMEPAD_BUTTON_WEST, 0x4000},      {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};

void attach_virtual(int n)
{
    for (int i = 0; i < n && i < 2; i++) {
        if (g_virtual[i]) continue;
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "test pad";
        g_virtual[i] = SDL_AttachVirtualJoystick(&d);
    }
}

// who is there now: a pad that went leaves its slot, one that came takes a free one. A step that took long is written down: it is what
// used to stop the game
void refresh()
{
    const Uint64 t0 = SDL_GetTicks();
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);   // nobody reads the queue; plugging a pad fills it
    const Uint64 pumped = SDL_GetTicks() - t0;
    if (pumped >= 100) fprintf(stderr, "pad: looking for devices took %llu ms\n", (unsigned long long)pumped);
    for (int slot = 0; slot < 2; slot++) {
        SDL_Gamepad *&p = g_pads[slot];
        if (p && !SDL_GamepadConnected(p)) {
            fprintf(stderr, "pad: slot %d disconnected\n", slot);
            SDL_CloseGamepad(p);
            p = nullptr;
            g_done_rumble[slot] = -1;
        }
    }
    if (g_pads[0] && g_pads[1]) return;
    // a pad that could not be opened is not tried again at once: a second later, so that one that never opens costs nothing
    static Uint64 failed_at;
    if (failed_at && SDL_GetTicks() - failed_at < 1000) return;
    failed_at = 0;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        bool have = false;
        for (SDL_Gamepad *p : g_pads) have = have || (p && SDL_GetGamepadID(p) == ids[i]);
        if (have) continue;
        for (int slot = 0; slot < 2; slot++)
            if (!g_pads[slot]) {
                const Uint64 o0 = SDL_GetTicks();
                g_pads[slot] = SDL_OpenGamepad(ids[i]);
                const char *name = g_pads[slot] ? SDL_GetGamepadName(g_pads[slot]) : nullptr;
                fprintf(stderr, "pad: slot %d %s: %s, %llu ms\n", slot, g_pads[slot] ? "connected" : "could not be opened", name ? name : SDL_GetError(),
                        (unsigned long long)(SDL_GetTicks() - o0));
                if (!g_pads[slot]) failed_at = SDL_GetTicks();
                break;
            }
    }
    SDL_free(ids);
}

bool is_playstation(SDL_Gamepad *p)
{
    switch (SDL_GetGamepadType(p)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return true;
    default:
        return false;
    }
}

void pad_thread()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "pad: SDL's gamepads did not start: %s\n", SDL_GetError());
        return;
    }
    // HP2_VPADS=<1 or 2>: gamepads that are not there, for testing: pad_virtual presses them. HP2_VPADS_AT=<ms>: they are plugged in
    // that long after the start, as a pad connected while the game runs
    const char *vn = getenv("HP2_VPADS"), *vat = getenv("HP2_VPADS_AT");
    const int virtual_pads = vn ? atoi(vn) : 0;
    const Uint64 virtual_at = vat ? (Uint64)atoll(vat) : 0, start = SDL_GetTicks();
    for (;;) {
        if (virtual_pads && SDL_GetTicks() - start >= virtual_at) attach_virtual(virtual_pads);
        refresh();
        Raw now[2];
        for (int slot = 0; slot < 2; slot++) {
            SDL_Gamepad *pad = g_pads[slot];
            if (!pad) continue;
            if (g_virtual[slot]) {
                const int press = g_virtual_press[slot];
                SDL_Joystick *j = SDL_GetGamepadJoystick(pad);
                SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, (press & 1) != 0);
                SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, press & 2 ? 32767 : -32768);
                SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
            }
            Raw &r = now[slot];
            r.ok = true;
            r.playstation = is_playstation(pad);
            for (auto &b : kButtons)
                if (SDL_GetGamepadButton(pad, b.b)) r.mask |= b.mask;
            r.lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            r.rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            r.axis[0] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
            r.axis[1] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
            r.axis[2] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            r.axis[3] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            // the motors: the game keeps asking while it wants them; a stop is a request like any other
            const int want = g_want_rumble[slot];
            if (want >= 0 && want != g_done_rumble[slot]) {
                SDL_RumbleGamepad(pad, (Uint16)((want & 255) * 257), want & 0x100 ? 0xFFFF : 0, want ? 0xFFFF : 0);
                g_done_rumble[slot] = want;
            }
        }
        {
            std::lock_guard<std::mutex> hold(g_lock);
            g_seen[0] = now[0];
            g_seen[1] = now[1];
        }
        g_count = (now[0].ok ? 1 : 0) + (now[1].ok ? 1 : 0);
        SDL_Delay(2);
    }
}

void start()
{
    if (g_started.exchange(true)) return;
    for (int i = 0; i < 2; i++) g_want_rumble[i] = g_done_rumble[i] = -1;
    std::thread(pad_thread).detach();
}

// an SDL axis (-32768..32767) as a DualShock axis, with a dead zone
uint8_t axis(int v, int dead, int sensitivity = 100)
{
    if (v > -dead && v < dead) return 0x80;
    int s = v < 0 ? -1 : 1, m = (v < 0 ? -v : v) - dead;
    float t = (float)m / (float)(32768 - dead);
    if (sensitivity != 100) t = SDL_powf(t, 100.0f / (float)sensitivity);
    int out = 0x80 + s * (int)(t * 128);
    return (uint8_t)(out < 0 ? 0 : out > 255 ? 255 : out);
}

Raw seen(int slot)
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_seen[slot];
}

}   // namespace

void pad_virtual(int slot, bool south, bool right_trigger) { g_virtual_press[slot] = (south ? 1 : 0) | (right_trigger ? 2 : 0); }

int pad_count()
{
    start();
    return g_count;
}

PadState pad_read(int slot)
{
    start();
    PadState ps;
    const Raw r = seen(slot);
    if (!r.ok) return ps;
    ps.ok = true;
    ps.mask = r.mask;
    ps.lt = (uint8_t)((r.lt < 0 ? 0 : r.lt) * 255 / 32767);
    ps.rt = (uint8_t)((r.rt < 0 ? 0 : r.rt) * 255 / 32767);
    if (ps.lt > 30) ps.mask |= 0x10000;
    if (ps.rt > 30) ps.mask |= 0x20000;
    const int kRightDead = 8689;
    int dead = g_cfg.pad_deadzone * 32768 / 100;
    ps.sticks[0] = axis(r.axis[0], kRightDead);
    ps.sticks[1] = axis(r.axis[1], kRightDead);
    ps.sticks[2] = axis(r.axis[2], dead, g_pad_steering_now);
    ps.sticks[3] = axis(r.axis[3], dead);
    return ps;
}

bool pad_is_playstation(int slot) { return seen(slot).playstation; }

void pad_rumble(int slot, int small_on, int large)
{
    g_want_rumble[slot] = g_cfg.pad_vibration ? (small_on ? 0x100 : 0) | (large & 255) : 0;
}

void pad_settle(int ms)
{
    if (!g_started) return;
    for (int waited = 0; waited < ms; waited += 2) {
        bool done = true;
        for (int slot = 0; slot < 2; slot++) done = done && (g_want_rumble[slot] < 0 || g_done_rumble[slot] == g_want_rumble[slot] || !seen(slot).ok);
        if (done) return;
        SDL_Delay(2);
    }
}
