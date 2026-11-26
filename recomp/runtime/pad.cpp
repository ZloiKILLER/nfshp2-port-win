// The gamepads through SDL3 (pad.h). Only its gamepad subsystem: the window is still the port's own, so
// SDL is told to listen without one having the focus; window_pad decides whether the game hears it.
// PAD_DEVICES slots: a pad that comes keeps its slot until it goes, so a player does not change pads.
//
// All of SDL runs on a thread of its own, and the game reads what that thread last saw. Finding a pad that was just plugged in and opening
// it are calls into Windows and into the device that can take seconds (DirectInput's enumeration, a pad's handshake over Bluetooth, a driver
// being installed, a pad that comes and goes): on the game's thread they stopped the game for as long, which a tester saw as the game
// hanging when a pad was connected while it ran. Here they stop only this thread; the game sees no pad a moment longer.
#include "pad.h"

#include "pcconfig.h"
#include "phase.h"
#include "cores.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

// a pad as SDL gives it, before the settings' dead zone and steering
struct Raw {
    bool ok = false, playstation = false, dualsense = false;
    int speaker = 0;        // pad_speaker
    int kind = 0;           // pad_kind
    int mask = 0;
    int lt = 0, rt = 0;
    int axis[4] = {};       // RX RY LX LY
    char name[48] = {};
};

std::mutex g_lock;          // g_seen
Raw g_seen[PAD_DEVICES];
std::atomic<int> g_count{0};
std::atomic<int> g_want_rumble[PAD_DEVICES];    // what the game asked for last: small << 8 | large, -1 before any
std::atomic<int> g_done_rumble[PAD_DEVICES];    // the request the pad's motors were last set by
std::atomic<bool> g_rumble_quiet;       // the window is not the one in front: no motor runs
std::atomic<bool> g_rumble_off[PAD_DEVICES];    // this pad is not the one its player is using
long long g_sent_rumble[PAD_DEVICES] = {-1, -1, -1, -1};    // what the pad was told last, and when (the pad's thread only)
Uint64 g_sent_rumble_at[PAD_DEVICES];
Uint64 g_rumble_retry[PAD_DEVICES];     // a command that did not go is said again from then on
// A DualSense's triggers (pad_triggers): what is wanted of them, the left one in the low half and the right one in the high, and
// what the pad was told last. The pad's thread's own: whether a slot's pad is a DualSense, whether its firmware has the newer way
// of the motors (SDL's enhanced_rumble: an Edge, firmware 2.24 on, or unknown over Bluetooth), and when to try again after a
// packet that did not go.
std::atomic<unsigned long long> g_want_triggers[PAD_DEVICES], g_sent_triggers[PAD_DEVICES];
bool g_dualsense[PAD_DEVICES], g_dualsense_new[PAD_DEVICES];
Uint64 g_triggers_retry[PAD_DEVICES];
// A pad's speaker (pad_speaker): its kind, the pad's thread's own; the pad's path, under g_lock; and for a DualSense whether its
// speaker is wanted on the sound's right channel (1) or not (0), and what it was told last (-1: nothing yet).
int g_speaker[PAD_DEVICES];
std::string g_speaker_path[PAD_DEVICES];
std::atomic<int> g_want_speaker[PAD_DEVICES], g_sent_speaker[PAD_DEVICES];
Uint64 g_speaker_retry[PAD_DEVICES];
std::atomic<int> g_virtual_press[2];    // HP2_VPADS: bit 0 south, bit 1 the right trigger
std::atomic<bool> g_started{false};

// the thread's own
SDL_Gamepad *g_pads[PAD_DEVICES];
SDL_JoystickID g_virtual[2];

const struct { SDL_GamepadButton b; int mask; } kButtons[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2},   {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8},   {SDL_GAMEPAD_BUTTON_START, 0x10},      {SDL_GAMEPAD_BUTTON_BACK, 0x20},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200}, {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
    {SDL_GAMEPAD_BUTTON_WEST, 0x4000},      {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};

int kind_of(SDL_Gamepad *p);    // (below)

// A pad that has just been opened: is it a DualSense that takes effects. By its type, by Sony's numbers of it (0ce6, and 0df2 the
// Edge), and by SDL having its own protocol for it (the light bar is its to set: the same condition as an effect's).
void dualsense_seen(int slot)
{
    SDL_Gamepad *p = g_pads[slot];
    const Uint16 vendor = SDL_GetGamepadVendor(p), product = SDL_GetGamepadProduct(p), firmware = SDL_GetGamepadFirmwareVersion(p);
    const bool is = SDL_GetGamepadType(p) == SDL_GAMEPAD_TYPE_PS5 && vendor == 0x054c && (product == 0x0ce6 || product == 0x0df2);
    const bool effects = SDL_GetBooleanProperty(SDL_GetGamepadProperties(p), SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
    g_dualsense[slot] = is && effects;
    g_dualsense_new[slot] = product == 0x0df2 || firmware == 0 || firmware >= 0x0224;
    g_sent_triggers[slot] = 0;
    g_triggers_retry[slot] = 0;
    // the speaker: a DualSense that takes effects, or a DualShock 4 (Sony's 05c4 and 09cc), on a cable
    const bool wired = SDL_GetGamepadConnectionState(p) == SDL_JOYSTICK_CONNECTION_WIRED;
    const bool ds4 = SDL_GetGamepadType(p) == SDL_GAMEPAD_TYPE_PS4 && vendor == 0x054c && (product == 0x05c4 || product == 0x09cc);
    g_speaker[slot] = !wired ? 0 : is && effects ? 1 : ds4 ? 2 : 0;
    g_sent_speaker[slot] = -1;
    g_speaker_retry[slot] = 0;
    {
        const char *path = SDL_GetGamepadPath(p);
        std::lock_guard<std::mutex> hold(g_lock);
        g_speaker_path[slot] = g_speaker[slot] && path ? path : "";
    }
    if (g_speaker[slot]) fprintf(stderr, "pad: slot %d has a speaker (%s on a cable)\n", slot, g_speaker[slot] == 1 ? "a DualSense" : "a DualShock 4");
    if (is)
        fprintf(stderr, "pad: slot %d is a DualSense (product %04x, firmware %04x, %s): adaptive triggers %s\n", slot, product, firmware,
                SDL_GetGamepadConnectionState(p) == SDL_JOYSTICK_CONNECTION_WIRED ? "wired" : "wireless",
                effects ? "can be set" : "cannot be set, the pad does not take effects");
}

// A DualSense's triggers told what they are to be: the pad's packet of effects (SDL's DS5EffectsState_t, 47 bytes: the valid bits at
// 0, the motors at 2 and 3, the right trigger's effect at 10 and the left one's at 21, eleven bytes each, more valid bits at 38).
// An effect: its number, then its numbers; 0x05 lets the trigger loose. The packet says the motors' state as well, as SDL's own
// packets do every time: one that left it out would stop a vibration that is on (SDL_hidapi_ps5.c, HIDAPI_DriverPS5_UpdateEffects).
// (the motors' state into a packet of the port's own: SDL's packets carry it every time, and one without it would stop a vibration)
void put_motors(Uint8 *e, int slot)
{
    const long long say = g_sent_rumble[slot];  // low << 16 | high, as the motors were last told
    if (say <= 0) return;
    const int big = (int)(say >> 24 & 255), small = (int)(say >> 8 & 255);
    if (g_dualsense_new[slot]) e[38] |= 0x04, e[3] = (Uint8)big, e[2] = (Uint8)small;
    else e[0] |= 0x01, e[3] = (Uint8)(big >> 1), e[2] = (Uint8)(small >> 1);
    e[0] |= 0x02;
}

bool send_triggers(SDL_Gamepad *pad, int slot, unsigned left, unsigned right)
{
    Uint8 e[47] = {};
    e[0] = 0x04 | 0x08;                         // the right trigger's effect and the left one's are in the packet
    const auto put = [&e](int at, unsigned effect) {
        e[at] = (Uint8)(effect & 255 ? effect & 255 : 0x05);
        e[at + 1] = (Uint8)(effect >> 8);
        e[at + 2] = (Uint8)(effect >> 16);
        e[at + 3] = (Uint8)(effect >> 24);
    };
    put(10, right);
    put(21, left);
    put_motors(e, slot);
    return SDL_SendGamepadEffect(pad, e, (int)sizeof e);
}

// A DualSense's speaker told where its sound comes from: the same packet's bits 0x20 (the speaker's loudness is in it, at 5) and
// 0x80 (the routing is in it, at 7: bits 4 and 5, 0 the headset alone, 2 the headset the left channel and the speaker the right
// one). The loudness is the most the pad has; how loud the radio is, is the port's (spu2_radio_sink).
bool send_speaker(SDL_Gamepad *pad, int slot, bool on)
{
    Uint8 e[47] = {};
    e[0] = 0x20 | 0x80;
    e[5] = 0x64;
    e[7] = on ? 0x20 : 0x00;
    put_motors(e, slot);
    return SDL_SendGamepadEffect(pad, e, (int)sizeof e);
}

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
    bool room = false;
    for (int slot = 0; slot < PAD_DEVICES; slot++) {
        SDL_Gamepad *&p = g_pads[slot];
        if (p && !SDL_GamepadConnected(p)) {
            fprintf(stderr, "pad: slot %d disconnected\n", slot);
            SDL_CloseGamepad(p);
            p = nullptr;
            g_done_rumble[slot] = -1;
            g_sent_rumble[slot] = -1;
            g_dualsense[slot] = false;
            g_sent_triggers[slot] = 0;
            g_speaker[slot] = 0;
            {
                std::lock_guard<std::mutex> hold(g_lock);
                g_speaker_path[slot].clear();
            }
            g_rumble_retry[slot] = 0;
        }
        room = room || !p;
    }
    if (!room) return;
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
        for (int slot = 0; slot < PAD_DEVICES; slot++)
            if (!g_pads[slot]) {
                const Uint64 o0 = SDL_GetTicks();
                g_pads[slot] = SDL_OpenGamepad(ids[i]);
                const char *name = g_pads[slot] ? SDL_GetGamepadName(g_pads[slot]) : nullptr;
                fprintf(stderr, "pad: slot %d %s: %s, %llu ms\n", slot, g_pads[slot] ? "connected" : "could not be opened", name ? name : SDL_GetError(),
                        (unsigned long long)(SDL_GetTicks() - o0));
                if (g_pads[slot]) {
                    static const char *const kKind[3] = {"Xbox's (or not known)", "PlayStation's", "Nintendo's"};
                    fprintf(stderr, "pad: slot %d has %s buttons (SDL's type %d)\n", slot, kKind[kind_of(g_pads[slot])], (int)SDL_GetGamepadType(g_pads[slot]));
                }
                if (!g_pads[slot]) failed_at = SDL_GetTicks();
                else dualsense_seen(slot);
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

// whose buttons a pad has (pad_kind), by what SDL says it is
int kind_of(SDL_Gamepad *p)
{
    switch (SDL_GetGamepadType(p)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return 1;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return 2;
    default:
        return 0;
    }
}

void pad_thread()
{
    phase_thread("pads");
    cores_thread(CORE_BACKGROUND);
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
        Raw now[PAD_DEVICES];
        for (int slot = 0; slot < PAD_DEVICES; slot++) {
            SDL_Gamepad *pad = g_pads[slot];
            if (!pad) continue;
            if (slot < 2 && g_virtual[slot]) {
                const int press = g_virtual_press[slot];
                SDL_Joystick *j = SDL_GetGamepadJoystick(pad);
                SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, (press & 1) != 0);
                SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, press & 2 ? 32767 : -32768);
                SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
            }
            Raw &r = now[slot];
            r.ok = true;
            r.playstation = is_playstation(pad);
            r.kind = kind_of(pad);
            r.dualsense = g_dualsense[slot];
            r.speaker = g_speaker[slot];
            // a DualSense's speaker: on the sound's right channel while the radio is its, told when that changes
            if (g_speaker[slot] == 1) {
                const int want_speaker = g_want_speaker[slot];
                const Uint64 at = SDL_GetTicks();
                if (want_speaker != g_sent_speaker[slot] && (want_speaker || g_sent_speaker[slot] > 0) && at >= g_speaker_retry[slot]) {
                    if (send_speaker(pad, slot, want_speaker != 0)) g_sent_speaker[slot] = want_speaker;
                    else g_speaker_retry[slot] = at + 1000;
                }
            }
            if (const char *name = SDL_GetGamepadName(pad)) SDL_strlcpy(r.name, name, sizeof r.name);
            for (auto &b : kButtons)
                if (SDL_GetGamepadButton(pad, b.b)) r.mask |= b.mask;
            r.lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            r.rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            r.axis[0] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
            r.axis[1] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
            r.axis[2] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            r.axis[3] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            // the motors: the game keeps asking while it wants them; a stop is a request like any other
            // The game's two numbers are a DualShock's: the big motor 0..255, the small one on or off; the big goes to the low frequency
            // as it is, the small to the high at full. SDL stops a rumble after the time given, so one that lasts is told again.
            const int want = g_want_rumble[slot];
            if (want >= 0) {
                int large = want & 255, small = want >> 8 & 255;
                if (g_rumble_quiet || g_rumble_off[slot]) large = small = 0;
                const Uint16 low = (Uint16)(large * 257), high = (Uint16)(small * 257);
                const long long say = (long long)low << 16 | high;
                const Uint64 at = SDL_GetTicks();
                // (a command SDL could not give the pad was taken as given: a stop lost that way left the motor running until
                // SDL's own time for it, 65 s. It is said again a quarter of a second later; a pad with no motors says no every time.)
                if ((say != g_sent_rumble[slot] || (say && at - g_sent_rumble_at[slot] > 30000)) && at >= g_rumble_retry[slot]) {
                    if (SDL_RumbleGamepad(pad, low, high, say ? 0xFFFF : 0)) {
                        g_sent_rumble[slot] = say;
                        g_sent_rumble_at[slot] = at;
                    } else {
                        g_rumble_retry[slot] = at + 250;
                    }
                }
                g_done_rumble[slot] = want;
            }
            // A DualSense's triggers: told when what is wanted of them is another thing, after the motors (the packet carries their
            // state as it is now). One that did not go is tried again a second later.
            if (g_dualsense[slot]) {
                unsigned long long triggers = g_want_triggers[slot];
                if (g_rumble_quiet || g_rumble_off[slot]) triggers = 0;
                const Uint64 at = SDL_GetTicks();
                if (triggers != g_sent_triggers[slot] && at >= g_triggers_retry[slot]) {
                    if (send_triggers(pad, slot, (unsigned)triggers, (unsigned)(triggers >> 32))) {
                        g_sent_triggers[slot] = triggers;
                    } else {
                        g_triggers_retry[slot] = at + 1000;
                        static int told;
                        if (told < 4) told++, fprintf(stderr, "pad: slot %d did not take the triggers' effect: %s\n", slot, SDL_GetError());
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> hold(g_lock);
            for (int slot = 0; slot < PAD_DEVICES; slot++) g_seen[slot] = now[slot];
        }
        int count = 0;
        for (const Raw &r : now) count += r.ok ? 1 : 0;
        g_count = count;
        SDL_Delay(2);
    }
}

void start()
{
    if (g_started.exchange(true)) return;
    for (int i = 0; i < PAD_DEVICES; i++) g_want_rumble[i] = g_done_rumble[i] = -1;
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
    if (slot < 0 || slot >= PAD_DEVICES) return Raw();
    std::lock_guard<std::mutex> hold(g_lock);
    return g_seen[slot];
}

}   // namespace

void pad_virtual(int slot, bool south, bool right_trigger) { g_virtual_press[slot] = (south ? 1 : 0) | (right_trigger ? 2 : 0); }

std::string pad_device_name(int slot)
{
    start();
    const Raw r = seen(slot);
    if (!r.ok) return "";
    std::string name = r.name[0] ? r.name : "Gamepad";
    int same = 1;
    for (int before = 0; before < slot; before++) {
        const Raw b = seen(before);
        if (b.ok && name == (b.name[0] ? b.name : "Gamepad")) same++;
    }
    return same > 1 ? name + " #" + std::to_string(same) : name;
}

int pad_count()
{
    start();
    return g_count;
}

PadState pad_read(int slot, int player)
{
    start();
    PadState ps;
    const Raw r = seen(slot);
    if (!r.ok) return ps;
    ps.ok = true;
    ps.mask = r.mask;
    // the player's own: the triggers' dead zone (what is left of the travel is the whole of it), the stick's, the steering
    const int who = player == 1 ? 1 : 0;
    const auto trigger = [&](int raw) {
        int v = (raw < 0 ? 0 : raw) * 255 / 32767;
        const int cut = (player < 0 ? 0 : g_cfg.pad_trigger_dead[who]) * 255 / 100;
        if (cut > 0) v = v <= cut ? 0 : (v - cut) * 255 / (255 - cut);
        return (uint8_t)(v > 255 ? 255 : v);
    };
    ps.lt = trigger(r.lt);
    ps.rt = trigger(r.rt);
    if (ps.lt > 30) ps.mask |= 0x10000;
    if (ps.rt > 30) ps.mask |= 0x20000;
    const int kRightDead = 8689;
    int dead = g_cfg.pad_dead[who] * 32768 / 100;
    ps.sticks[0] = axis(r.axis[0], kRightDead);
    ps.sticks[1] = axis(r.axis[1], kRightDead);
    ps.sticks[2] = axis(r.axis[2], dead, g_pad_driving_now && player >= 0 ? g_cfg.pad_steer[who] : 100);
    ps.sticks[3] = axis(r.axis[3], dead);
    return ps;
}

bool pad_is_playstation(int slot) { return seen(slot).playstation; }
int pad_kind(int slot) { return seen(slot).kind; }
bool pad_is_dualsense(int slot) { return seen(slot).dualsense; }
int pad_speaker(int slot) { return seen(slot).speaker; }
std::string pad_speaker_path(int slot)
{
    if (slot < 0 || slot >= PAD_DEVICES) return "";
    std::lock_guard<std::mutex> hold(g_lock);
    return g_speaker_path[slot];
}
void pad_speaker_route(int slot, bool on)
{
    if (slot >= 0 && slot < PAD_DEVICES) g_want_speaker[slot] = on ? 1 : 0;
}

void pad_triggers(int slot, unsigned left, unsigned right)
{
    if (slot >= 0 && slot < PAD_DEVICES) g_want_triggers[slot] = (unsigned long long)right << 32 | left;
}

void pad_rumble_quiet(bool quiet) { g_rumble_quiet = quiet; }
void pad_rumble_gate(int slot, bool on)
{
    if (slot >= 0 && slot < PAD_DEVICES) g_rumble_off[slot] = !on;
}

void pad_rumble(int slot, int small, int large)
{
    if (slot >= 0 && slot < PAD_DEVICES) g_want_rumble[slot] = (small & 255) << 8 | (large & 255);
}

void pad_settle(int ms)
{
    if (!g_started) return;
    for (int waited = 0; waited < ms; waited += 2) {
        bool done = true;
        for (int slot = 0; slot < PAD_DEVICES; slot++) {
            const Raw r = seen(slot);
            done = done && (g_want_rumble[slot] < 0 || g_done_rumble[slot] == g_want_rumble[slot] || !r.ok);
            done = done && (!r.dualsense || g_sent_triggers[slot] == g_want_triggers[slot]);
            done = done && (r.speaker != 1 || g_sent_speaker[slot] == g_want_speaker[slot] || (g_sent_speaker[slot] < 0 && !g_want_speaker[slot]));
        }
        if (done) return;
        SDL_Delay(2);
    }
}
