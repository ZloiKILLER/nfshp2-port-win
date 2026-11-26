// The keyboard as a device of its own (the user, 2026-11-23): its driving keys are the game's own actions, not the buttons of a
// PS2 pad. The gamepads are not touched: they go on through the game's pad reader, its layouts and its scanners.
//
// How the game takes input (work/input_rework/docs/joystick_module.md, receivers.md; all from our own listing): once a frame
// GenerateJoyEvents (0x153e48) empties the queue of events and, for every pad sample and both ports, calls
// Joystick::GenerateEventsFromScanners (0x153a98). That walks the port's list of handlers (EventNode: next +0, event +8, handler
// +16, context +20; the Joystick at 0x2f9530 + 204 * port is the list's end, its port at +184) and asks each node's scanner what
// its button says; an answer goes into the queue (QueuedJoystickEvents::AddEvent, 0x153a00: event | value << 8). The queue is then
// written to the attract demos' log and to the race's replay, and each entry is handed to its handler. Black Box's own PC games
// (Underground 1 and 2) kept this module and put a PC device under the scanners; the plugin that gives them XInput feeds at this
// same level (work/input_rework/docs/pc_ports_input.md).
//
// So here: after the game has scanned a port's pad, the same list is walked once more and each handler that a key stands for is
// answered from the keyboard. What is queued is recorded and played back like a pad's events; a handler that is not registered
// gets nothing, as with a pad (the list is the gate: a cop has the calls where a racer has the zone cameras).
//
// Which event a key is, and its value (the pad's own scanners' rules, joystick_module.md "The nine scanners"):
//  - steering: STEER (111), 0 left, 255 right, 128 neither or both: the D-pad's event and values, so the wheel turns at the game's
//    own rate exactly as it did. The pad's stick is STEER_ANALOG (112), another byte of the driver's: neither undoes the other.
//  - throttle, brake: THROTTLE (113), BRAKE (116), the digital events, which the driver keeps apart from the pad's analog ones
//    (114, 117) and prefers while not 0 (RealDriver: "(digital ? digital : max(analog, alternate)) / 255"). The value is 253, not
//    255: what a full press of the key gave through the pad's pressure (the low bit flipped, then the graph), so the car is as
//    fast as it was. Released: 0 (through the pad it read 1 of 255).
//  - handbrake: EBRAKE (121), 250: what the pad's EBRAKE_ANALOG came to (253 doubled into a byte).
//  - held things, 1 while down and 0 when let go: look back (94), horn (126), reset (125), the gears (123, 124).
//  - presses, 1 once as the key goes down: the view (93), the HUD (130), boost (127), pause (131), skip (132), the zone cameras
//    (95 Freeze, 96 Zoom, 97 the slow motion no pad has a button for) and the cop's calls on the same two keys (129, 128).
//  - the slow motion (97) is Extras' Slow Motion: with that off its key sends nothing; with it on the key sends the press, and so
//    do a gamepad's two sticks pressed together (window_pad_slomo), which is the one thing here that a gamepad sends. A second
//    press ends the slow motion (pcmenu.cpp, HOOK_RacingJoyHandler).
//  - the gears' keys do nothing with the automatic gearbox (the user: a keyboard has keys enough). The game itself turns Shift
//    Up / Down into the zone cameras there (RealDriver's handlers, by its transmission at +588: Player::RerouteJoyEvent), so the
//    events are simply not sent then; a gamepad still has it.
//
// When a handler comes back (after a zone camera, a pause) the pad's scanners send their state at the first scan. Here a held key
// is sent then, and a key let go meanwhile sends its release if the last thing this keyboard sent for it was not that. Whenever
// the keys stop being actions with handlers still there (the window loses the focus, a menu comes up), everything held is let go.
// While a replay plays back nothing is sent: the game does not scan a pad's car controls then either.
//
// hp2.ini [keyboard] actions=0: the old way, the keys press the buttons of a PS2 pad (window_pad). It stays until the new one is
// proven (the user, 2026-11-23).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pclang.h"
#include "pcmenu.h"
#include "hw.h"
#include "pcconfig.h"

#include <cstdio>
#include <cstring>

namespace {

enum Kind { LEVEL, PRESS };

// what the keyboard last told the game for an event of a port, and the handler it told
struct Told {
    u32 node = 0, handler = 0, context = 0;     // the first handler of the event in the port's list
    unsigned scan = 0;          // the scan it was last seen in
    int sent = -1;              // LEVEL: the last value sent, -1 none yet
    bool down = false;          // PRESS: the key as it was
    int now = -1;               // what this scan sends for the event (-1 nothing): a second handler of it gets the same
};
Told g_told[2][256];
unsigned g_scan[2];

// the event's kind, its value from the keys, its value at rest; false: no key of the keyboard is this event
bool key_event(u32 event, const bool *k, u32 context, Kind &kind, int &value, int &rest)
{
    kind = LEVEL, rest = 0;
    switch (event) {
    case 111: rest = 128; value = k[PCB_LEFT] == k[PCB_RIGHT] ? 128 : k[PCB_LEFT] ? 0 : 255; return true;
    case 113: value = k[PCB_CROSS] ? 253 : 0; return true;
    case 116: value = k[PCB_SQUARE] ? 253 : 0; return true;
    case 121: value = k[PCB_R1] ? 250 : 0; return true;
    case 94: value = k[PCB_L1]; return true;
    case 126: value = k[PCB_CIRCLE]; return true;
    case 125: value = k[PCB_SELECT]; return true;
    case 123: case 124:
        // the driver's transmission: 0 automatic (the handlers' context is the RealDriver)
        value = context && LW(context + 588) != 0 && k[event == 123 ? PCB_R2 : PCB_L2];
        return true;
    }
    kind = PRESS;
    switch (event) {
    case 93: value = k[PCB_TRIANGLE]; return true;
    case 130: value = k[PCB_UP]; return true;
    case 127: value = k[PCB_R3]; return true;
    case 131: value = k[PCA_PAUSE]; return true;
    case 132: value = k[PCA_SKIP]; return true;
    case 95: case 129: value = k[PCA_ZONE_FREEZE]; return true;
    case 96: case 128: value = k[PCA_ZONE_ZOOM]; return true;
    case 97: value = k[PCA_ZONE_SLOMO]; return true;
    }
    return false;
}

int scan_keys(int port, u32 joy, u32 queue)
{
    bool keys[PCA_COUNT] = {};
    const bool on = g_cfg.key_actions && window_actions(port, keys);       // (false: no key is an action now, and all read as up)
    bool pad_drives = false;
    const bool sticks = window_pad_slomo(port, pad_drives);                // a gamepad's two sticks: the slow motion
    if (!g_cfg.key_actions && !pad_drives) return 0;
    const unsigned scan = ++g_scan[port];
    const u32 replay = LW(0x3358f0 - 22440);            // TheReplayManager; +0: a replay plays back
    if (replay && LW(replay) != 0) return 0;
    int queued = 0, n = 0;
    for (u32 node = LW(joy); node && node != joy && n < 256; node = LW(node), n++) {
        const u32 event = LW(node + 8) & 0xff, handler = LW(node + 16), context = LW(node + 20);
        Kind kind;
        int value, rest;
        if (!key_event(event, keys, context, kind, value, rest)) continue;
        bool live = on;                                 // the event is looked at on this scan
        if (event == 97) {
            value = g_cfg.slow_motion && !pc_two_player_race() && (value || sticks);     // (two players: the game has no zones)
            live = on || pad_drives;
        } else if (!g_cfg.key_actions) {
            continue;                                   // the keys go the old way: only the gamepad's slow motion is the port's
        }
        Told &t = g_told[port][event];
        int send = -1;
        if (t.scan == scan) {
            send = t.now;                               // the event has another handler in the list: the same answer
        } else {
            // the handler is new, or was away (a zone camera, a pause, another race): the pad's "first scan"
            const bool first = t.scan + 1 != scan || t.node != node || t.handler != handler || t.context != context;
            if (kind == LEVEL) {
                if (first) {
                    // a held key is told again (the handler may be a new driver's); a key let go meanwhile sends the release
                    // this keyboard owes; a key at rest that sent nothing says nothing (a pad's button may hold the same event)
                    if (value != rest) send = value;
                    else if (t.sent >= 0 && t.sent != rest) send = rest;
                } else if (value != t.sent) {
                    send = value;
                }
                if (send >= 0 || t.sent < 0) t.sent = value;
            } else {
                // A press is a key going down while the keys are actions. On a scan where they are not, the key is not looked
                // at, and the next scan where they are is this handler's first again. (The user, 2026-11-24: "Enter is pressed
                // twice as a race starts and I do not see the fly-by". The race's player and its mode 5, with the handler that
                // waits for SKIP, are there while the Enter that chose the race in the menu is still down; the keys' state had
                // been taken before the mode changed, so that scan read Enter as up, and the one after it as a new press.)
                if (!live) continue;
                if (value && !t.down && !first) send = 1;   // a key held as the handler comes is no press
                t.down = value != 0;
            }
            t.node = node, t.handler = handler, t.context = context, t.scan = scan, t.now = send;
        }
        if (send < 0) continue;
        pc_call(0x153a00, queue, (u32)port, event | (u32)send << 8, node);     // QueuedJoystickEvents::AddEvent
        queued++;
    }
    return queued;
}

} // namespace

// Joystick::GenerateEventsFromScanners(joy, type, now, before, queue): the pad first, then the keyboard's actions for the same
// port. Its answer is how many events it queued (anything moves the attract mode's idle clock): the keyboard's count too.
void HOOK_GenerateEventsFromScanners__8JoystickiPUcT2R20QueuedJoystickEvents(Ctx &c, void (*orig)(Ctx &))
{
    const u32 joy = c.r[4].w[0], queue = c.r[8].w[0];
    orig(c);
    if (!joy || !queue) return;
    g_pc_ctx = &c;
    const int port = (int)(LW(joy + 184) & 1);
    const int queued = scan_keys(port, joy, queue);
    if (queued) {
        c.r[2].d[0] = (s64)(s32)(c.r[2].w[0] + (u32)queued);
        static int told;
        if (told < 6) told++, fprintf(stderr, "input: the keyboard's actions: %d for port %d this frame\n", queued, port);
    }
}
