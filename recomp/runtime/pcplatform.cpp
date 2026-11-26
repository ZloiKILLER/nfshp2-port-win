// What the player sees of the PS2, as a PC game has it (recomp/docs/frontend-menus.md):
//  - the strings: FE_ENGLISH's table is patched as it loads, so every reader gets the new text;
//  - the button icons: BUTTONSOUT's sheet is repainted with the keys or gamepad buttons bound,
//    in the game's own FONT_A, whenever the bindings or the device in use change.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pclang.h"
#include "pcmenu.h"
#include "hw.h"
#include "pad.h"
#include "pcconfig.h"
#include "phase.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// ---- strings -----------------------------------------------------------------------------
// The high bytes are the game's help characters: 0x80 START, 0x81 SELECT, 0x82..0x85 the D-pad,
// 0x86 Cross, 0x87 Triangle, 0x88 Square, 0x89 Circle, 0x8a L1, 0x8b R1. They draw as the icons below.
static const struct { int id; const char *text; } kText[] = {
    {10000, "PRESS ENTER"},
    {100004, "HUD"},
    {100021, "Keyboard and gamepad."},
    {100022, "Window, resolution, detail."},
    {100023, "Set what you want to see."},
    {100024, "Choose, add or delete a profile."},
    {100025, "Set your player names."},
    {100026, "See who made this game."},
    {150004, "Saved Game"},
    {151001, "Loading game settings."},
    {151003, "No saved game settings found."},
    {151004, "Saving game settings.@Do not close the game."},
    {151006, "The save folder cannot be opened.@ @Would you like to retry?"},
    {151007, "The save folder is damaged. Would you like to create it again?"},
    {151008, "Creating the save folder..."},
    {151009, "The save folder was created."},
    {151010, "The save folder cannot be created.@ @Do you want to try again?"},
    {151011, "There is not enough disk space to save the game. Do you want to try again?"},
    {151012, "The game could not be saved.@Do you want to try again?"},
    {151013, "Looking for saved games..."},
    {151014, "A new save folder was created."},
    {151015, "The save folder cannot be opened.@ Would you like to retry loading?"},
    {151016, "The save folder cannot be used."},
    {151017, "The save folder cannot be opened."},
    {151018, "The saved game could not be loaded.@Would you like to try again?"},
    {151020, "Overwrite the@saved game?"},
    {151022, "Current settings will be overwritten by the saved game. Continue?"},
    {151023, "Create a saved game?"},
    {151024, "The saved game could not be loaded."},
    {151025, "The save folder cannot be opened."},
    {151027, "The save folder cannot be opened. Do you want to try again?"},
    {151101, "The save folder is damaged."},
    {152001, "Creating the save folder..."},
    {300350, "Press \x81" " to reset your car."},
    {800023, "\x82 \x83 Tilt car"},
    {800024, "\x84 \x85 Rotate car"},
    {800026, "\x8b \x8d Zoom camera"},
    {800070, "\x82 \x83 Select save option"},
    {800071, "\x86 \x80 Use save option"},
    {802004, "\x82 \x83 Tilt car"},
    {802005, "\x84 \x85 Rotate car"},
    {802006, "\x82 \x83 \x84 \x85 Rotate and tilt car"},
    {802007, "\x8b \x8d Zoom camera"},
    {802010, "\x82 \x83 Tilt car"},
    {802011, "\x84 \x85 Rotate car"},
    {802012, "\x82 \x83 \x84 \x85 Rotate and tilt car"},
    {802013, "\x8b \x8d Zoom camera"},
    {900005, "Please connect@a gamepad for@player %d."},
    {900011, "Please connect a gamepad."},
    {940001, "Press \x80" " to enter this event."},
};

static u32 g_press_text;                    // the title's line, "PRESS ENTER" or "PRESS START"

// the LanguageDescription (payload of the chunk): NumHash +76, NumID +80, pHash +88, pID +92,
// pText +96. A table entry is {key, offset into pText}; the English hash table lists the same
// offsets, so a replaced string's entries there follow it.
static void patch_language(u32 desc)
{
    // Cross and START do the same in the menus and Enter is both: "Cross START Enter game mode"
    // says it once. In place, in the pool: the string only gets shorter
    u32 size = LW(desc + 84), text = LW(desc + 96);
    u8 *pool_text = host(text);
    for (u32 i = 0; i + 3 < size; i++)
        if (pool_text[i] == 0x86 && pool_text[i + 1] == ' ' && pool_text[i + 2] == 0x80) {
            u32 end = i + 3;
            while (end < size && pool_text[end]) end++;
            memmove(pool_text + i + 1, pool_text + i + 3, end - (i + 3));
            memset(pool_text + end - 2, 0, 2);
        }
    // The port's texts: in English the code's (kText), then for any language its file's #id lines (pclang.h). The title's line (10000)
    // gets room of its own: the device in use rewrites it (pc_paint_buttons).
    const int slot = (int)LW(desc + 72);
    std::vector<std::pair<int, std::string>> texts;
    if (slot == 0)
        for (auto &t : kText) texts.push_back({t.id, t.text});
    for (auto &o : pc_language_overrides(slot)) {
        bool had = false;
        for (auto &t : texts)
            if (t.first == o.first) t.second = o.second, had = true;
        if (!had) texts.push_back(o);
    }
    bool press = false;
    for (auto &t : texts) press |= t.first == 10000;
    if (!press) texts.push_back({10000, tr("PRESS ENTER")});
    u32 size_all = 0;
    for (auto &t : texts) size_all += t.first == 10000 ? 64 : (u32)t.second.size() + 1;
    u32 p = pc_alloc(size_all);             // a language's own: a change of language makes another (a few kilobytes)
    u32 nhash = LW(desc + 76), nid = LW(desc + 80), phash = LW(desc + 88), pid = LW(desc + 92), ptext = LW(desc + 96);
    for (auto &t : texts) {
        const u32 room = t.first == 10000 ? 64 : (u32)t.second.size() + 1;
        memcpy(host(p), t.second.c_str(), std::min<size_t>(t.second.size() + 1, room - 1));
        if (t.first == 10000) g_press_text = p;
        for (u32 i = 0; i < nid; i++) {
            if ((s32)LW(pid + i * 8) != t.first) continue;
            u32 was = LW(pid + i * 8 + 4), now = p - ptext;
            SW(pid + i * 8 + 4, now);
            for (u32 k = 0; k < nhash; k++)
                if (LW(phash + k * 8 + 4) == was) SW(phash + k * 8 + 4, now);
            break;
        }
        p += room;
    }
}

void HOOK_Loader__15LanguageManagerP6bChunk(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 chunk = c.r[5].w[0];
    orig(c);
    if (c.r[2].w[0] != 1) return;
    u32 ret = c.r[2].w[0];
    patch_language(chunk + 8);              // LanguageDescription +72: the e_Languages it is
    c.r[2].d[0] = ret;
}

// ---- the sheet ----------------------------------------------------------------------------
// The keyboard and the gamepad's layouts. A port's layout is the word at +0xc8 of its joystick state (0x2f9530, 0xcc a port; 0x155020
// reads it): the configuration's id, 0x1e for the DualShock 2's first (A), and the game looks every button up by it each time. The
// layouts are the gamepad's (eight of them with layouts_patch.h, some with the D-pad for other things than steering). The keyboard
// has nothing to do with them: its keys are actions, pressed as the buttons that have those actions in layout A, and while the
// keyboard is what a player drives with, that player's port is given layout A. The layout chosen for the gamepad is kept and put
// back as soon as the gamepad is used, or the race is left: the game's screens and its save see only the chosen one.
static const u32 kKeyboardLayout = 0x1e;
static u32 g_layout_chosen[2];
static bool g_layout_keys[2];
// The game does not look the buttons up by that word each time: a handler is tied to its buttons when it is added
// (Joystick::ConnectEventNodeConfig, 0x153c50: FindScannerConfig by the event, the pad's type and this word), and tied again only when
// the pad's type changes. The race's handlers are added by Player::SetInputMode, which comes while the screen before it is still up
// and the word is the gamepad's; the keyboard's word came a pad read later and changed nothing, so the keys went by the gamepad's
// layout (the throttle's key braking) until the type happened to be read anew, seconds into the race. So when the word is changed
// here the port's handlers are tied again, by the game's own function, before the next events are made (HOOK_GenerateJoyEvents).
static bool g_layout_changed[2];
// A layout of the player's own made anew (pc_custom_changed, below) is not a change of the word: only the handlers of the events
// that layout answers itself (custom::mine, the race's) are tied again. Tying them all was a bug of the page of that layout
// (the user, 2026-11-25: "one press of Left or Right turns a selector several times"): a tie puts a handler back to "never
// read" (+24 = 0, +28 = 1), and the scanner of the menus' directions (0x154a90, events 3..6: a press, then repeats after 25
// and every 6 ticks) takes a button that is down then for a new press. So a change made by Left or Right tied the menus'
// handlers, the button still held was pressed again the next frame, that changed the row and tied them again, and so on
// until the button was let go.
static bool g_custom_rebuilt[2];
namespace custom { bool mine(u32 event); }

void pc_keyboard_layout(int port, bool keys)
{
    static const bool off = getenv("HP2_NOKEYLAYOUT") != nullptr;      // a test: the keys under the gamepad's layout, as they were
    static const char *force = getenv("HP2_PADLAYOUT");                // a test: the gamepad's layout, as if chosen (hex id)
    port &= 1;
    const u32 at = 0x2f9530 + (u32)port * 0xcc + 0xc8;
    if (force && !g_layout_keys[port]) SW(at, (u32)strtoul(force, nullptr, 16));
    if (off) return;
    if (keys) {
        if (!g_layout_keys[port]) g_layout_chosen[port] = LW(at), g_layout_keys[port] = true;
        if (LW(at) != kKeyboardLayout) SW(at, kKeyboardLayout), g_layout_changed[port] = true;
    } else if (g_layout_keys[port]) {
        if (LW(at) != g_layout_chosen[port]) g_layout_changed[port] = true;
        SW(at, g_layout_chosen[port]);
        g_layout_keys[port] = false;
    }
}

// GetJoystickButtonChar (0x155270: the port, an event): the help character of the button the port's layout gives the event. The
// objective's lines fill a button in with it: a cop's "%c target/untarget speeders" is event 126, the horn / siren (PreRaceBlurb's
// HandleEvent 0x1e34a8, PauseRaceObjective's 0x171a28). The keyboard's keys are layout A's buttons (pc_keyboard_layout), so with the
// keyboard in use the character is asked of layout A, whatever the gamepad's layout is: a gamepad on layout I has the horn on R1,
// and the keyboard's icon was R1's key where the target is the horn's (the user, 2026-11-26). The word is put back at once.
void HOOK_GetJoystickButtonChar(Ctx &c, void (*orig)(Ctx &))
{
    const u32 at = 0x2f9530 + (c.r[4].w[0] & 1) * 0xcc + 0xc8;
    const u32 was = LW(at);
    if (window_input_kind() == 1 || was == kKeyboardLayout) {
        orig(c);
        return;
    }
    SW(at, kKeyboardLayout);
    orig(c);
    SW(at, was);
}

// ---- a DualSense's adaptive triggers ---------------------------------------------------------------------------------
// The user, 2026-11-25: "let us first try to make the triggers adaptive for the DualSense". The pedal's trigger resists the
// finger: the brake's harder than the throttle's. Which trigger is which pedal is the layout's to say, whichever layout the
// player has (the game's, the modern one, the player's own): the game's own table of buttons is asked, by its own function
// (FindScannerConfig, 0x155138: the event, the pad's type, the layout chosen), for the throttle's button (the events 114 its
// pressure and 113 the button) and the brake's (117, 116), and a button is a trigger by its picture in the entry (+24: 140
// L2, 141 R2, the left and the right trigger of a PC gamepad). A pedal on another button leaves the triggers loose, and so
// does everything that is not driving: the menus, the pause, a replay, a wheel on the port (its type is not a gamepad's).
// The setting, the gamepad's kind and whose it is are window.cpp's (window_triggers); the packet is pad.cpp's.
// What the triggers go by besides the layout (the user, 2026-11-25: the kerbs, a blown tyre, the brake by the speed; and "the
// triggers are heavy all the time now, let us make them lighter"). The car's own numbers, read where the game reads them
// for the gamepad's motors: Car::MaybeRumble (0x134378), once a world step for each car a player drives, never in a replay
// or a demo. The car's player is at +24 and the player's port at +8; the function goes no further with no player, or with
// a driver (+28) whose word at +18 is set, and neither is taken here. Of the car:
//  - the speed, +964 (what the game compares with 5 and 13 for a kerb's one bump or two);
//  - the ground under each wheel, four words from +868: 30 and 31 are the kinds the game gives its road bumps for, the
//    kerbs and the rumble strips;
//  - blown tyres, the byte at +2125 (more than 0), and when, +2128, by the world's timer (0x330c78, 1/4000 s): the game's own
//    shake of them fades in 7 s (0.00025 * 1/7 a tick).
struct TriggerCar {
    unsigned long long seen = 0;    // the frame it was read in (g_trigger_frame)
    float speed = 0, blown_fresh = 0;       // (1 as the tyre goes, 0 from 7 s after)
    float rpm = 0, redline = 0, rpm_top = 0;        // the engine's speed, its red line and its top (0: not known)
    int gear = -1;                          // the gear the car is in (-1: not known)
    bool kerb = false, blown = false;
};
static TriggerCar g_trigger_car[2];
static unsigned long long g_trigger_frame = 8;

void HOOK_MaybeRumble__3Car(Ctx &c, void (*orig)(Ctx &))
{
    const u32 car = c.r[4].w[0], player = LW(car + 24), driver = LW(car + 28);
    if (player && !(driver && (LH(driver + 18) & 0xffff))) {
        const u32 port = LW(player + 8);
        if (port < 2) {
            TriggerCar &t = g_trigger_car[port];
            const auto real = [](u32 at) {
                const u32 w = LW(at);
                float f;
                memcpy(&f, &w, sizeof f);
                return f;
            };
            t.seen = g_trigger_frame;
            t.speed = fabsf(real(car + 964));
            t.kerb = false;
            for (u32 wheel = 0; wheel < 4; wheel++) t.kerb = t.kerb || LW(car + 868 + wheel * 4) - 30 < 2;
            t.blown = (s8)LB(car + 2125) > 0;
            const u32 since = LW(car + 2128);
            const float fresh = since && since != 0x7fffffff ? 1.0f - (float)(s32)(LW(c.r[28].w[0] - 19576) - since) * 0.00025f / 7.0f : 0.0f;
            t.blown_fresh = fresh < 0 ? 0 : fresh > 1 ? 1 : fresh;
            // The engine's speed as the car's sound has it, +980 (PlayerEngineSound::UpdateToRedLineRPM, 0x210880, writes it
            // there each frame), and the red line the same function reads for a car of kind 1 (+36): +48, its +68 the
            // DriveTrain, its +20 the Engine, its +16 the EngineInfo {idle, red line, top}.
            // The gear: the DriveTrain's +48 (GetCurrentGear, 0x18c128; +44 is the gear wanted, +52 the one before, +56 the
            // clutch). The same function counts 0 and 1 as no gear to rev out (reverse and neutral), 2 the first.
            t.rpm = real(car + 980);
            t.redline = t.rpm_top = 0;
            t.gear = -1;
            if (LW(car + 36) == 1)
                if (const u32 body = LW(car + 48))
                    if (const u32 train = LW(body + 68)) {
                        t.gear = (int)LW(train + 48);
                        if (const u32 engine = LW(train + 20))
                            if (const u32 info = LW(engine + 16)) t.redline = real(info + 4), t.rpm_top = real(info + 8);
                    }
        }
    }
    orig(c);
}

// The effects, by the car (nothing of this was felt by me; the numbers are the user's to set by his hand):
//  - ordinary driving: the triggers are loose, both of them;
//  - a blown tyre: both pedals' triggers resist, the brake's harder, for as long as the tyre is blown;
//  - a gear change: a short kick in both pedals' triggers (below);
// That is all there is now. The car's speed, its engine's speed and the kerbs under its wheels are still read above and not
// used: builds of the same day had the brake by the speed, a shake of the throttle at high revs and a drumming on kerbs, and
// the user took each of them out.
static void adaptive_triggers(Ctx &c)
{
    static const bool off = getenv("HP2_NOTRIGGERS") != nullptr;
    unsigned want[2][2] = {};
    g_pc_ctx = &c;
    for (int port = 0; port < 2 && !off; port++) {
        if (!g_cfg.pad_adaptive[port] || !window_pad_dualsense(port)) continue;
        if (!pc_car_input_port(port) || pc_replay_playing()) continue;
        if (LB(0x330350 + (u32)port) != 0x79) continue;                 // CurrentJoystickTypes: a DualShock 2 in analog mode
        const u32 layout = LW(0x2f9530 + (u32)port * 0xcc + 0xc8);
        const auto trigger_of = [layout](u32 pressure, u32 button) {    // 0 the left trigger, 1 the right, -1 neither
            for (u32 event : {pressure, button})
                if (const u32 entry = pc_call(0x155138, event, 0x79, layout)) {
                    const u32 picture = LW(entry + 24);
                    if (picture == 140 || picture == 141) return (int)(picture - 140);
                }
            return -1;
        };
        const int throttle = trigger_of(114, 113), brake = trigger_of(117, 116);
        static int told[2] = {-2, -2};
        if (told[port] != throttle * 4 + brake) {
            told[port] = throttle * 4 + brake;
            fprintf(stderr, "triggers: port %d, layout %x: the throttle is %s, the brake is %s\n", port, layout,
                    throttle < 0 ? "no trigger" : throttle ? "the right trigger" : "the left trigger",
                    brake < 0 ? "no trigger" : brake ? "the right trigger" : "the left trigger");
        }
        // the car, if the game has just read it (a step or two may pass with none: the picture's frames between the world's)
        const TriggerCar &car = g_trigger_car[port];
        const bool known = g_trigger_frame - car.seen <= 3;
        // The user's words of 2026-11-25, build by build: a resistance in ordinary driving was "heavy all the time" (70 / 150 of
        // 255, then 28 / 40..110); "take the triggers' resistance out of the ordinary driving; let there be a resistance only
        // when a tyre is punctured"; then, after a build with a light shake of the throttle at high revs and a drumming on the
        // kerbs: "take all the effects off the triggers but the resistance with a punctured tyre, and make that resistance
        // 40 percent less if that can be done" (it was 90 and 120).
        unsigned gas = 0, stop = 0;
        if (known && car.blown) gas = pad_trigger_resistance(24, 54), stop = pad_trigger_resistance(24, 72);
        // A gear change (the user, 2026-11-26: "a light click on the adaptive triggers when the gears change"; of my questions:
        // the throttle's trigger, at every change, up and down, by hand or by the automatic box, "a short kick"). The trigger's
        // own click, the "weapon" effect, is a stiff stretch of the travel that gives way as the finger goes through it: it
        // answers the finger's move, and a gear changes with the finger still. So the kick is made of time: for 80 ms the
        // trigger resists over all of its travel, wherever the finger holds it, and then is as it was. A gear is one that
        // drives (reverse 0, the first 2 and up); into neutral is no kick, so a change that passes through it is one kick
        // and not two, and no two kicks are nearer than 150 ms.
        static int gear_was[2] = {-1, -1};
        static unsigned long long kick_until[2], kick_last[2];
        const unsigned long long now = GetTickCount64();
        if (known && car.gear >= 0) {
            if (gear_was[port] >= 0 && car.gear != gear_was[port] && car.gear != 1 && now - kick_last[port] >= 150)
                kick_until[port] = now + 80, kick_last[port] = now;
            gear_was[port] = car.gear;
        } else {
            gear_was[port] = -1;
        }
        // (Three builds of 2026-11-26 had the throttle's trigger grow heavy near the engine's top: from the red line, 30 to 110;
        // over the last 20 % of the revs, 70 to 253; over the last 10 %, 50 to 100. The user took it out: "its effect and the
        // gear change lie on top of one another; better leave only the gear change". The engine's speed and its top are
        // still read above and not used.)
        // The kick, by the user's hand, build by build: 130 for 60 ms, 70, 40 for 40 ms, 30 for 60 ms, 20, 15 ("too little"), 20
        // on both pedals' triggers, 18, and now 20 for 80 ms. It does not lighten a trigger that resists more already.
        if (now < kick_until[port]) {
            const unsigned kick = pad_trigger_resistance(0, 20);
            if ((gas >> 16 & 255) < 20) gas = kick;
            if ((stop >> 16 & 255) < 20) stop = kick;
        }
        if (throttle >= 0) want[port][throttle] = gas;
        if (brake >= 0 && brake != throttle) want[port][brake] = stop;
    }
    g_trigger_frame++;
    window_triggers(want);
}

// GenerateJoyEvents (0x153e48), once a frame: the handlers of a port whose layout word the port has changed, tied to their buttons anew
// (the list is the Joystick's own: head at +0, each node's next at +0, the Joystick itself its end). HP2_NOLAYOUTTIE=1: as it was.
void HOOK_GenerateJoyEvents__Fv(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NOLAYOUTTIE") != nullptr;
    for (int port = 0; port < 2; port++) {
        if (!g_layout_changed[port] && !g_custom_rebuilt[port]) continue;
        const bool all = g_layout_changed[port];        // the word is another: every handler; else the own layout's events alone
        g_layout_changed[port] = g_custom_rebuilt[port] = false;
        if (off) continue;
        g_pc_ctx = &c;
        const u32 joy = 0x2f9530 + (u32)port * 0xcc;
        int n = 0, tied = 0;
        for (u32 node = LW(joy); node && node != joy && n < 256; node = LW(node), n++)
            if (all || custom::mine(LW(node + 8))) pc_call(0x153c50, joy, node), tied++;       // (+8: the handler's event)
        static int told;
        if (told < 8) told++, fprintf(stderr, "layout: port %d has configuration %x now, %d handlers of %d tied to it\n", port, LW(joy + 0xc8), tied, n);
    }
    adaptive_triggers(c);
    window_radio_frame();
    orig(c);
}

// ---- the gamepad's motors: as soon as the game has set them, and not over the pause menu ------------------------------
// The user, 2026-11-25: "why is the vibration late, and why does it play when the pause menu is open already?"
//
// The game works a pad's two motors out once a frame (UpdatePortRumblers, 0x155d40, from MainLoop after the world's step: two
// tRumblers a port, 20 bytes each at 0x2fa7c8 {envelope, time, scale, value, value before}, each playing its envelope by the
// real time step), and SetRumble (0x11d3d8) only notes the two numbers: six bytes a port at 0x2de608 and a "to be sent" word a
// port at gp-23784. They go to the pad from PrepareToGetJoystickData (0x11ca78), which is the next frame's: a frame late.
// Here they go as soon as they are worked out, 1/60 s sooner. The game sends them again a frame later, the same numbers,
// which the pad's thread takes as nothing new.
//
// And an envelope goes on by the real time while the race is paused: a hard hit is up to 1.6 s long (0.8 by twice the
// length), the road's noise and the suspension's sway up to 1.8 s, so the pad went on shaking over the pause menu. When the
// pause comes (the RaceCoordinator's state 6: IsGameFlowPaused, 0x1680b8) the rumblers are emptied and the motors are told
// to stop, as the game's own stop of a port does (0x155e40). What starts in the pause plays as it did: the Controller
// screen's test of the vibration. HP2_RUMBLEASIS=1: the game's own way, for a comparison.
// ---- the motors by the car itself ("Responsive") --------------------------------------------------------------------
// The user, 2026-11-25: "something is still wrong with the vibration ... it does not answer the game's behaviour".
// What the game does with what the car tells it (PortRumble, 0x155e88: the port, a number x of the car's, four numbers
// {threshold, top, scale at the threshold, scale at the top}, an envelope of (time, value) points, and whether it is a hit):
// the heavy motor has ONE player of envelopes a port, the scale stretches an envelope in strength and in time alike, a
// hit takes the player over whatever it plays, and the two that go on (the suspension's sway, the road's noise) are let
// in only if their scale is no less than the one playing. So what was strong a moment ago plays on by itself, 1.2 to 1.8 s
// of fading, and what the car does meanwhile is not heard; a hard crash is 1.6 s; a kerb's bump cuts a crash's fading
// off. On a DualShock 2's slow motor that passed for inertia; a modern pad plays every bit of it.
//
// Here the same calls are taken as they come and worked out anew, with the game's own numbers:
//  - the sway, the noise and a blown tyre's shake are levels: each is what the game's envelope starts at for the car's x now
//    (scale times the first point: for the two the game writes that point itself, 45 or 60 to 200), at once when it rises, and
//    falling to what it is now, or to nothing when the game says no more of it, in 0.15 s of a full motor;
//  - a hit (a crash, the suspension's bottom, a kerb's bump) is the game's envelope played by itself: the strength by the
//    scale, the time by it too but not beyond the envelope's own length when that is half a second or more (a crash: 0.8 s);
//  - all of them together are the largest of them, so one does not cut another off;
//  - the light motor is the game's: 0.15 s of it when a hit's x is over the top.
// The game's own conditions hold: only in a race or its pause, only with the player's Vibration on. The game's player of
// envelopes runs on beside this and its two numbers are not taken (window_rumble_owner). Calibration > Vibration Mode;
// "Original" is the game's own way, with what the block below does for it.
namespace rumble {

const u32 kSway = 0x2b1310, kNoise = 0x2b1320, kBlown = 0x2b1340;      // pRumbleDebugName (gp-21840): which of the car's it is
struct Pulse {
    u32 points = 0, name = 0;
    int count = 0;
    float t = 0, strength = 0, length = 1;
};
struct Port {
    float level[3] = {}, target[3] = {};    // the sway, the noise, a blown tyre
    int quiet[3] = {3, 3, 3};               // frames since the game last said anything of it
    Pulse pulse[4];
    float small = 0;                        // seconds of the light motor left
};
Port g_port[2];

float real(u32 at)
{
    const u32 w = LW(at);
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}
bool responsive(u32 port)
{
    static const bool as_is = getenv("HP2_RUMBLEASIS") != nullptr;
    return !as_is && port < 2 && g_cfg.pad_vibration_mode[port] != 0;
}
// an envelope's value at a time: straight lines between its points, nothing outside them
float value(const Pulse &p, float at)
{
    for (int i = 0; i + 1 < p.count; i++) {
        const float t0 = real(p.points + (u32)i * 8), t1 = real(p.points + (u32)i * 8 + 8);
        if (at < t0 || at >= t1 || t1 <= t0) continue;
        const float v0 = real(p.points + (u32)i * 8 + 4), v1 = real(p.points + (u32)i * 8 + 12);
        return v0 + (v1 - v0) * (at - t0) / (t1 - t0);
    }
    return -1;
}

}   // namespace rumble

void HOOK_PortRumble__FifPCfP9tEnvelopeb(Ctx &c, void (*orig)(Ctx &))
{
    using namespace rumble;
    const u32 port = c.r[4].w[0], coeffs = c.r[5].w[0], envelope = c.r[6].w[0], gp = c.r[28].w[0];
    const bool hit = c.r[7].w[0] != 0;
    const float x = c.f[12].f;
    const u32 name = LW(gp - 21840);
    orig(c);
    if (!responsive(port)) return;
    if (LW(gp - 21424)) {                       // InGameFlag: then only the race itself (3) and its pause (6)
        const u32 coordinator = LW(gp - 22216), state = coordinator ? LW(coordinator) : 0;
        if (state != 3 && state != 6) return;
    }
    bool on;                                    // the player's Vibration: the race's player, or the menus' settings (Fred)
    if (LW(gp - 21420) != 3) {
        const u32 player = LW(gp - 20936 + (port ? 4u : 0u));
        on = player && LB(player + 33) != 0;
    } else {
        on = LB(0x2fc050 + (port ? 1744u : 0u) + 0x10738 + 1605) != 0;
    }
    Port &p = g_port[port];
    const int channel = name == kSway ? 0 : name == kNoise ? 1 : name == kBlown ? 2 : -1;
    const float c0 = real(coeffs), c1 = real(coeffs + 4), c2 = real(coeffs + 8), c3 = real(coeffs + 12);
    const u32 points = LW(envelope);
    const int count = (int)LW(envelope + 4);
    if (!on || x < c0 || c1 <= c0 || !points || count < 2 || count > 16) {
        if (channel >= 0) p.target[channel] = 0, p.quiet[channel] = 0;
        return;
    }
    const float scale = ((x > c1 ? c1 : x) - c0) / (c1 - c0) * (c3 - c2) + c2;
    if (hit && x > c1) p.small = 0.15f;
    if (channel >= 0) {
        const float v = scale * real(points + 4);
        p.target[channel] = v > 255 ? 255 : v < 0 ? 0 : v;
        p.quiet[channel] = 0;
        return;
    }
    Pulse *slot = &p.pulse[0];                  // the one of this name, else a free one, else the one furthest through
    for (Pulse &q : p.pulse) {
        if (q.points && q.name == name) {
            slot = &q;
            break;
        }
        if (!q.points ? slot->points != 0 : slot->points && q.t / q.length > slot->t / slot->length) slot = &q;
    }
    slot->points = points, slot->count = count, slot->name = name;
    slot->t = 0, slot->strength = scale;
    slot->length = real(points + (u32)(count - 1) * 8) >= 0.5f && scale > 1.0f ? 1.0f : scale;
    if (slot->length < 0.05f) slot->length = 0.05f;
}

// ---- the gamepad's motors: as soon as the game has set them, and not over the pause menu ------------------------------
// (the comment above this block, of the morning of 2026-11-25, tells the game's own way: UpdatePortRumblers once a frame by the
// real time step, SetRumble's two numbers and its "to be sent" word, a frame late through PrepareToGetJoystickData.)
// For a player whose Vibration Mode is Original the game's numbers go to the pad as soon as they are worked out, and the
// game's rumblers are emptied when the pause comes. For a player whose mode is Responsive the motors are worked out here
// from what HOOK_PortRumble took, by the same real time step, and stop with the pause the same way.
void HOOK_UpdatePortRumblers__Fv(Ctx &c, void (*orig)(Ctx &))
{
    using namespace rumble;
    static const bool as_is = getenv("HP2_RUMBLEASIS") != nullptr;
    if (as_is) {
        window_rumble_owner(0, false);
        window_rumble_owner(1, false);
        orig(c);
        return;
    }
    const u32 gp = c.r[28].w[0], kRumblers = 0x2fa7c8, kMotors = 0x2de608, pending = gp - 23784;
    const u32 coordinator = LW(gp - 22216);
    const bool paused = coordinator && LW(coordinator) == 6;
    static bool was_paused;
    if (paused && !was_paused)
        for (u32 port = 0; port < 2; port++) {
            for (u32 r = kRumblers + port * 40; r < kRumblers + port * 40 + 40; r += 20) SW(r, 0), SW(r + 12, 0), SW(r + 16, 0);
            SB(kMotors + port * 6, 0);          // SetRumble(port, 0, 0)
            SB(kMotors + port * 6 + 1, 0);
            SW(pending + port * 4, 1);
            g_port[port] = Port();
        }
    was_paused = paused;
    orig(c);
    float dt = real(gp - 19596);                // RealTimeElapsed
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    for (u32 port = 0; port < 2; port++) {
        const bool own = responsive(port);
        window_rumble_owner((int)port, own);
        if (!own) {
            g_port[port] = Port();
            if (LW(pending + port * 4)) window_rumble_port((int)port, LB(kMotors + port * 6), LB(kMotors + port * 6 + 1));
            continue;
        }
        Port &p = g_port[port];
        float big = 0;
        for (int k = 0; k < 3; k++) {
            if (p.quiet[k] < 3) p.quiet[k]++;
            else p.target[k] = 0;               // the game says nothing of it any more
            const float down = p.level[k] - dt * (255.0f / 0.15f);
            p.level[k] = p.target[k] > down ? p.target[k] : down;
            if (p.level[k] > big) big = p.level[k];
        }
        for (Pulse &q : p.pulse) {
            if (!q.points) continue;
            const float v = value(q, q.t / q.length);
            if (v < 0) {
                q.points = 0;                   // played through
                continue;
            }
            if (v * q.strength > big) big = v * q.strength;
            q.t += dt;
        }
        const bool small = p.small > 0;
        if (small) p.small -= dt;
        const u32 type = LB(0x330350 + port);   // CurrentJoystickTypes: the motors are a DualShock's (CanRumble, 0x11d3b0)
        if (type != 0x73 && type != 0x79) big = 0;
        window_rumble_own((int)port, small && big >= 0 && (type == 0x73 || type == 0x79) ? 255 : 0, big > 255 ? 255 : (int)big);
    }
}

// ---- a layout of the player's own ("Custom") ---------------------------------------------------------------------------
// The user, 2026-11-25: "a Manual layout for every gamepad, so that the players set the controls themselves ... in
// Configuration, at the end, after all the layouts ... when the editing is done the buttons are shown on that layout".
//
// A layout of the game's is nothing but its number in the table of scanners (ScannerConfig, 36 bytes, 230 of them at 0x2dc500:
// work/input_rework/docs/joystick_module.md): each entry says "for these layouts this event is that button, read this way", and
// FindScannerConfig (0x155138: the event, the pad's type, the layout chosen) gives the first entry of an event that holds the
// layout. The table is full and has no room, so the two new layouts (0x3c player 1's, 0x3d player 2's: ps2.cpp puts them
// last in the DualShock 2's list, and the Configuration screen comes to them after the game's own) have their entries here,
// in memory of the port's, made from what the player chose (hp2.ini, pcconfig.h); FindScannerConfig is answered from them for
// the race's events, and for everything else (the menus, the replay's buttons) as layout A is. So the game's own scanners read
// the player's buttons, by the game's own rules: the handlers, the replays and the demos know no difference, and the
// Configuration screen and the hints draw the buttons of this layout as they draw any other's (the glyph is in the entry).
//
// What an entry is, by what the thing is (the scanners: Analog 0x154e48, DigitalUpOrDown 0x1547e8, DigitalDown 0x1546b0,
// DigitalSteer 0x154be8; the events' numbers are the game's):
//  - steering: the left stick, STEER_ANALOG (112) as layout A has it; or two buttons, STEER (111), the D-pad's scanner; or
//    both at once, as the game's own layouts have the stick and the D-pad;
//  - the right stick, by the page's Right Stick row: the pedals, the game's "alternate" throttle and brake (115, 118: its
//    upper and lower half); or the camera, which is no event of the game's at all (the stick camera, below); or nothing;
//  - throttle, brake: analog, one of the two triggers each, its pressure byte through the analog scanner (114, 117: how far
//    it is pressed), or, digital, any button, the button itself (113, 116, value 255). The page keeps analog pedals on
//    the triggers (pcsettings.cpp); a button with no pressure byte (R3, L3, Select) would be digital whatever is chosen;
//  - the handbrake: the button itself, all or nothing (121, value 255), whatever the pedals are (the user, 2026-11-25: "the
//    handbrake is always a button pressed all the way");
//  - held things (the gears 123 / 124, look back 94, the horn 126, reset 125) and presses (the view 93, boost 127, the HUD 130):
//    the button alone;
//  - the zone cameras (95 / 96, and a cop's calls 129 / 128 on the same): the game's way, as its layouts with the stick and the
//    D-pad both steering have it, Change View's button held and then a gear's button pressed; or a button each. The game's
//    way is the game's entries with the player's buttons: the zones have the view's button as a shift that must be held, the
//    gears have it as one that must be up, and the view changes when its button is let go, not when it goes down.
// A thing with no button has no entry: the game takes that as "this layout has no button for it". Pause (131) and SKIP (132)
// are not the layout's: Start and Cross, as in A.
namespace custom {

const u32 kAnalog = 0x154e48, kUpOrDown = 0x1547e8, kDown = 0x1546b0, kSteer = 0x154be8;
// by PcButton: the button's bit in the pad's data, its pressure byte's bit (0: it has none), its glyph in the game's font
const struct { u8 bit, pressure, glyph; } kButton[PCB_COUNT + 1] = {
    {20, 80, 130}, {22, 88, 131}, {23, 72, 132}, {21, 64, 133},         // Up Down Left Right
    {30, 112, 134}, {31, 120, 136}, {29, 104, 137}, {28, 96, 135},      // Cross Square Circle Triangle
    {26, 128, 138}, {27, 136, 139}, {24, 144, 140}, {25, 152, 141},     // L1 R1 L2 R2
    {18, 0, 143}, {19, 0, 128}, {16, 0, 129},                           // R3 Start Select
    {17, 0, 142}};                                                      // L3 (PC_CUSTOM_L3: window_pad hands it on for this layout)
const u32 kFirst = 93, kLast = 130;         // the events' span; of it the race's own are the layout's (mine, below)
// The race's events, for which a layout of the player's own answers itself: the cameras and the zones (93..96) and the car's
// (111..130). Between them are the slow motion and the warp, which no pad has (97, 98), and the replay screen's buttons
// (99..110), which are every layout's alike and are answered as layout A.
bool mine(u32 event) { return (event >= 93 && event <= 96) || (event >= 111 && event <= 130); }
u32 g_base[3];                              // a layout's entries (player 1's own, player 2's, the modern one), one for each event from kFirst on
bool g_has[3][kLast - kFirst + 1];

// What a layout is made from: a player's own choices (pcconfig.h, the page of the layout), or the modern layout's, which are
// nobody's to change: a player's own layout as it comes (PcConfig::default_custom, which says what it is; the user,
// 2026-11-25: "take layout I as the default of layout J").
struct Setup {
    int b[PCC_COUNT];
    int steer, pedals_digital, right_stick, zone_buttons;
};
Setup setup(int p)
{
    Setup s;
    if (p < 2) {
        memcpy(s.b, g_cfg.custom[p], sizeof s.b);
        s.steer = g_cfg.custom_steer[p], s.pedals_digital = g_cfg.custom_pedals_digital[p];
        s.right_stick = g_cfg.custom_right_stick[p], s.zone_buttons = g_cfg.custom_zone_buttons[p];
        return s;
    }
    static const PcConfig modern;           // (its constructor is default_custom for both players)
    memcpy(s.b, modern.custom[0], sizeof s.b);
    s.steer = modern.custom_steer[0], s.pedals_digital = modern.custom_pedals_digital[0];
    s.right_stick = modern.custom_right_stick[0], s.zone_buttons = modern.custom_zone_buttons[0];
    return s;
}

void entry(int p, u32 event, u32 scan, int polarity, int shift_polarity, int button, int shift, int value, int axis, int glyph, int shift_glyph)
{
    const u32 a = g_base[p] + (event - kFirst) * 36;
    for (u32 k = 0; k < 36; k += 4) SW(a + k, 0);
    SB(a, (u8)(PC_CUSTOM_LAYOUT + (u32)p));     // +0: the layouts it is for
    SW(a + 8, event);
    SW(a + 12, scan);
    SB(a + 16, (u8)polarity);
    SB(a + 17, (u8)shift_polarity);
    SB(a + 18, (u8)button);
    SB(a + 19, (u8)shift);
    SB(a + 20, (u8)value);
    SB(a + 21, (u8)axis);
    SW(a + 24, (u32)glyph);
    SW(a + 28, (u32)shift_glyph);
    g_has[p][event - kFirst] = true;
}

void build(int p)
{
    if (!g_base[p]) return;
    memset(g_has[p], 0, sizeof g_has[p]);
    for (u32 k = 0; k < (kLast - kFirst + 1) * 36; k += 4) SW(g_base[p] + k, 0);       // (an empty entry has no scanner: not read)
    const Setup s = setup(p);
    const auto has = [&s](int action) {
        const int b = s.b[action];
        return b >= 0 && b <= PC_CUSTOM_L3 && b != PCB_START ? b : -1;
    };
    if (s.steer != 1) entry(p, 112, kAnalog, 1, 0, 48, 0, 0, 1, 148, 0);     // the left stick's X, calibration 1
    if (s.steer != 0) {
        const int l = has(PCC_STEER_LEFT), r = has(PCC_STEER_RIGHT);
        const bool dpad = l == PCB_LEFT && r == PCB_RIGHT;      // (the D-pad's own glyph of the two)
        if (l >= 0 && r >= 0) entry(p, 111, kSteer, 1, 1, kButton[l].bit, kButton[r].bit, 0, 0, dpad ? 153 : kButton[l].glyph, dpad ? 0 : kButton[r].glyph);
    }
    const auto pedal = [&](int action, u32 analog, int axis, u32 digital) {
        const int b = has(action);
        if (b < 0) return;
        if (!s.pedals_digital && kButton[b].pressure) entry(p, analog, kAnalog, 1, 0, kButton[b].pressure, 0, 0, axis, kButton[b].glyph, 0);
        else entry(p, digital, kUpOrDown, 1, 0, kButton[b].bit, 0, 255, 0, kButton[b].glyph, 0);
    };
    pedal(PCC_ACCEL, 114, 3, 113);
    pedal(PCC_BRAKE, 117, 2, 116);
    if (const int b = has(PCC_HANDBRAKE); b >= 0) entry(p, 121, kUpOrDown, 1, 0, kButton[b].bit, 0, 255, 0, kButton[b].glyph, 0);
    if (s.right_stick == 1) {
        entry(p, 115, kAnalog, 1, 0, 40, 0, 1, 3, 150, 0);     // the right stick's Y: its upper half the throttle
        entry(p, 118, kAnalog, 1, 0, 40, 0, 2, 2, 151, 0);     // its lower half the brake
    }
    const auto button = [&](int action, u32 event, u32 scan) {
        const int b = has(action);
        if (b >= 0) entry(p, event, scan, 1, 0, kButton[b].bit, 0, 0, 0, kButton[b].glyph, 0);
    };
    button(PCC_LOOK_BACK, 94, kUpOrDown);
    button(PCC_HORN, 126, kUpOrDown);
    button(PCC_RESET, 125, kUpOrDown);
    button(PCC_BOOST, 127, kDown);
    button(PCC_HUD, 130, kDown);
    if (s.zone_buttons) {
        button(PCC_SHIFT_UP, 123, kUpOrDown);
        button(PCC_SHIFT_DOWN, 124, kUpOrDown);
        button(PCC_VIEW, 93, kDown);
        button(PCC_ZONE_FREEZE, 95, kDown);
        button(PCC_ZONE_FREEZE, 129, kDown);        // a cop's call for the helicopter
        button(PCC_ZONE_ZOOM, 96, kDown);
        button(PCC_ZONE_ZOOM, 128, kDown);          // and for a road block
    } else {
        // the game's way: the view's button is the shift
        const int view = has(PCC_VIEW), up = has(PCC_SHIFT_UP), down = has(PCC_SHIFT_DOWN);
        const int held = view >= 0 ? kButton[view].bit : 0, held_glyph = view >= 0 ? kButton[view].glyph : 0;
        if (view >= 0) entry(p, 93, kDown, 0, 0, kButton[view].bit, 0, 0, 0, kButton[view].glyph, 0);      // (polarity 0: as it is let go)
        if (up >= 0) entry(p, 123, kUpOrDown, 1, 0, kButton[up].bit, held, 0, 0, kButton[up].glyph, 0);    // (shift polarity 0: the view's must be up)
        if (down >= 0) entry(p, 124, kUpOrDown, 1, 0, kButton[down].bit, held, 0, 0, kButton[down].glyph, 0);
        if (view >= 0 && down >= 0) {
            entry(p, 95, kDown, 1, 1, kButton[down].bit, held, 0, 0, kButton[down].glyph, held_glyph);     // Zone Freeze
            entry(p, 129, kDown, 1, 1, kButton[down].bit, held, 0, 0, kButton[down].glyph, held_glyph);    // the helicopter
        }
        if (view >= 0 && up >= 0) {
            entry(p, 96, kDown, 1, 1, kButton[up].bit, held, 0, 0, kButton[up].glyph, held_glyph);         // Zone Zoom
            entry(p, 128, kDown, 1, 1, kButton[up].bit, held, 0, 0, kButton[up].glyph, held_glyph);        // a road block
        }
    }
}

}   // namespace custom

bool pc_custom_port(int port) { return pc_port_layout(LW(0x2f9530 + (u32)(port & 1) * 0xcc + 0xc8)); }

void pc_custom_changed(int player)
{
    player &= 1;
    custom::build(player);
    g_custom_rebuilt[player] = true;        // (HOOK_GenerateJoyEvents: an event that had no button may have one now, and the other way)
}

// FindScannerConfig (0x155138: the event, the pad's type, the layout chosen): for a DualShock 2 with a layout of the player's
// own, the port's entry of a race event, or none; anything else as layout A has it.
void HOOK_FindScannerConfigByEvent(Ctx &c, void (*orig)(Ctx &))
{
    const u32 event = c.r[4].w[0], type = c.r[5].w[0], chosen = c.r[6].w[0];
    if (type == 0x79 && pc_port_layout(chosen)) {
        if (custom::mine(event)) {
            const int p = (int)(chosen - PC_CUSTOM_LAYOUT);
            if (!custom::g_base[p]) {
                g_pc_ctx = &c;
                custom::g_base[p] = pc_keep((custom::kLast - custom::kFirst + 1) * 36);
                custom::build(p);
                if (p < 2) fprintf(stderr, "layout: player %d's own layout is made, at %08x\n", p + 1, custom::g_base[p]);
                else fprintf(stderr, "layout: the modern layout is made, at %08x\n", custom::g_base[p]);
            }
            const bool has = custom::g_base[p] && custom::g_has[p][event - custom::kFirst];
            c.r[2].d[0] = has ? (u64)(s64)(s32)(custom::g_base[p] + (event - custom::kFirst) * 36) : 0;
            return;
        }
        c.r[6].d[0] = kKeyboardLayout;      // 0x1e, layout A
    }
    orig(c);
}

// The same by a button's name (0x155360: the name, the type, the layout chosen): the names are the table's, so as layout A
void HOOK_FindScannerConfigByName(Ctx &c, void (*orig)(Ctx &))
{
    if (c.r[5].w[0] == 0x79 && pc_port_layout(c.r[6].w[0])) c.r[6].d[0] = kKeyboardLayout;
    orig(c);
}

// ---- the stick camera: the right stick turns the race's camera around the car -------------------------------------------
// The user, 2026-11-25: "the right stick as the camera, as in modern racing games; into the Custom layout, and the player
// chooses what the right stick is: the throttle and the brake, the camera, or nothing".
//
// The race's camera (DriveCameraMover, its Update at 0x15c880) already turns around the car: that is how it looks back.
// Update takes the car's matrix transposed and turns it about the up axis by the mover's look-back angle (+140, 65536 a full
// turn; 0x8000 is backward, and 0x4000 / 0xc000 to the right / the left are what CalculateLookbackAngle, 0x15c5a8, gives
// for a pursuer beside the car), with bRotateZ (0x11ba18: dst = src * the turn), at two places, one for each kind of view
// (the calls that come back to 0x15d2e0 and 0x15d978); the boom, the height, the walls it must not go through and the
// spring are worked out after that, for whatever angle it is. The angle itself is made inside Update from the look-back
// button's state (+124 the button, +128 how far the turn has got, eased by a cosine: one side only), so it cannot be set
// from outside; the stick's angle is ADDED to it in the call instead: Update's hook says how much, bRotateZ's hook adds it
// to the angle of those two calls and of nothing else (the function is the game's rotation for everything; the generated
// code leaves the return address in ra before a call). The look-back button goes on working, on top of the stick.
//
// Those two turns are made only with the player's look-back option (Player +44, the pause menu's display options) not 0;
// at 0 the game looks back another way and makes no turn. So with the option at 0 the word is 1 for the length of Update
// while the stick is out and the player is not looking back; while the button looks back there, the stick waits.
//
// The stick: where it points is where the camera looks, by how far it is pushed. Up is ahead (nothing), to a side a
// quarter turn at the stick's end, down half a turn: backward. Straight down is half a turn either way, so the side is the
// one the stick was last clearly on. It follows the stick and goes back when the stick is let go, by an exponent (a tenth
// of the way left after about a fifth of a second), the short way round only while the stick is far out (rolled past
// straight down), else back the way it came. One player's stick, one player's camera: both in a split screen.
// Only with a layout of the player's own in use (not with the keyboard driving: the port's layout is A then), in a race,
// not in a replay. The stick is read here, from the gamepad: it is no event of the game's, a replay has nothing of it.
static std::atomic<int> g_stick_yaw{0};     // while a race camera's Update runs: what its look-back turn gets added (0: nothing)

static int stick_camera(u32 player, float step)
{
    static float yaw[2];                    // a player's turn now, in turns: to the right above 0, -0.5..0.5
    static int side[2] = {1, 1};
    static bool told[2];
    const int p = (int)(LW(player + 8) & 1);
    float x = 0.0f, y = 0.0f, want = 0.0f, amount = 0.0f;
    // (whose stick is the camera: the modern layout's always, a player's own layout's by its Right Stick row)
    const u32 layout = LW(0x2f9530 + (u32)p * 0xcc + 0xc8);
    const bool camera = layout == PC_MODERN_LAYOUT || (pc_custom_layout(layout) && g_cfg.custom_right_stick[layout - PC_CUSTOM_LAYOUT] == 2);
    if (camera && pc_racing() && !pc_replay_playing() && window_right_stick(p, x, y)) {
        const float kDead = 0.25f, m = sqrtf(x * x + y * y);
        if (m > kDead) {
            amount = m >= 1.0f ? 1.0f : (m - kDead) / (1.0f - kDead);
            if (fabsf(x) > 0.2f * m) side[p] = x < 0.0f ? -1 : 1;
            want = atan2f(fabsf(x), y) / 6.2831853f * amount * (float)side[p];     // (0 up, a quarter to the side, a half down)
        }
    }
    if (yaw[p] == 0.0f && want == 0.0f) return 0;
    if (!(step > 0.0f)) step = 0.0f;
    if (step > 0.1f) step = 0.1f;
    float d = want - yaw[p];
    if (amount > 0.8f) d -= floorf(d + 0.5f);
    yaw[p] += d * (1.0f - expf(-12.0f * step));
    yaw[p] -= floorf(yaw[p] + 0.5f);
    if (want == 0.0f && fabsf(yaw[p]) < 0.0005f) yaw[p] = 0.0f;
    if (!told[p]) told[p] = true, fprintf(stderr, "camera: player %d's right stick turns the camera\n", p + 1);
    return (int)lroundf(yaw[p] * 65536.0f) & 0xffff;
}

void HOOK_DriveCameraUpdate(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 mover = c.r[4].w[0], player = mover ? LW(mover + 28) : 0;
    int yaw = 0;
    if (player) {
        const u32 bits = LW(c.r[28].w[0] - 19560);      // the step Update itself goes by
        float step;
        memcpy(&step, &bits, 4);
        yaw = stick_camera(player, step);
    }
    bool forced = false;
    if (yaw && !LW(player + 44)) {
        if ((LW(mover + 124) << 1) == 0 && (LW(mover + 128) << 1) == 0) SW(player + 44, 1), forced = true;     // (0.0 both: not looking back)
        else yaw = 0;
    }
    g_stick_yaw.store(yaw, std::memory_order_relaxed);
    orig(c);
    g_stick_yaw.store(0, std::memory_order_relaxed);
    if (forced) SW(player + 44, 0);
}

void HOOK_RotateZ(Ctx &c, void (*orig)(Ctx &))
{
    if (const int yaw = g_stick_yaw.load(std::memory_order_relaxed)) {
        const u32 back = c.r[31].w[0];
        if (back == 0x15d2e0 || back == 0x15d978) c.r[6].d[0] = (u64)((c.r[6].w[0] + (u32)yaw) & 0xffff);
    }
    orig(c);
}

namespace {

const u32 FindFont = 0x13fe18;              // FindFont(hash)
const u32 FontTexture = 0x1400f8;           // Font::GetTextureInfo(font)
const u32 UpdateAllText = 0x141648;         // FlailerUpdateLanguageText()

// A help character's glyph is the byte of this table (0x2f09c0), from 0x80 on
const char kGlyphOf[30 + 1] = "abdefgijhknolm3wpv\x92uqrtscx2yz1";

// 16 colours, RGBA with the GS's alpha (0x80 opaque)
const u8 kPal[16][4] = {
    {0, 0, 0, 0},         {0, 0, 0, 0x30},         {0x28, 0x28, 0x28, 0x80}, {0x50, 0x50, 0x50, 0x80},
    {0x80, 0x80, 0x80, 0x80}, {0xb8, 0xb8, 0xb8, 0x80}, {0xff, 0xff, 0xff, 0x80}, {0x30, 0xa0, 0x30, 0x80},
    {0xd0, 0x30, 0x30, 0x80}, {0x30, 0x68, 0xd8, 0x80}, {0xd8, 0xb0, 0x10, 0x80}, {0x98, 0xd0, 0x98, 0x80},
    {0xe8, 0x98, 0x98, 0x80}, {0x98, 0xb4, 0xec, 0x80}, {0xec, 0xd8, 0x88, 0x80}, {0, 0, 0, 0x80},
};

struct Rgba { float r, g, b, a; };
const Rgba WHITE = {255, 255, 255, 128}, CAP = {0x28, 0x28, 0x28, 128}, EDGE = {0xb8, 0xb8, 0xb8, 128};

// the nibble of (x, y) in a 4-bit texture stored for an upload as PSMCT32 (Is32BitSwizzelled)
u32 swizzled4(int w, int x, int y)
{
    int page_y = y >> 7, py = y & 127, block_y = py >> 4, cy = py & 15, col = cy >> 2;
    int swap = ((cy >> 1) & 1) ^ (col & 1);
    int page_x = x >> 7, px = x & 127, block_x = px >> 5, cx = px & 31;
    return (u32)(page_y * 128 * w + col * 8 * w + (cy & 1) * 4 * w + block_y * 64 + ((cy >> 1) & 1) + page_x * 512 +
                 block_x * 32 * w + ((cx >> 3) & 1) * 2 + ((cx >> 4) & 1) * 4 + (cx & 1) * 8 + ((cx >> 1) & 1) * 16 +
                 (((cx >> 2) & 1) ^ swap) * 32);
}

struct Sheet {
    u32 pixels = 0, palette = 0;
    int w = 0;
    bool load(u32 font)
    {
        u32 ti = font ? pc_call(FontTexture, font) : 0;
        if (!ti || LB(ti + 0x28) != 4 || !LW(ti + 0x90)) return false;
        pixels = LW(ti + 0x90);
        palette = LW(ti + 0x94);
        w = LH(ti + 0x24);
        return palette != 0;
    }
    int get(int x, int y) const
    {
        u32 n = swizzled4(w, x, y);
        u8 b = LB(pixels + n / 2);
        return n & 1 ? b >> 4 : b & 15;
    }
    void set(int x, int y, int v) const
    {
        u32 n = swizzled4(w, x, y), a = pixels + n / 2;
        u8 b = LB(a);
        SB(a, (u8)(n & 1 ? (b & 0x0f) | v << 4 : (b & 0xf0) | v));
    }
};

float glyph_f(u32 g, int off)
{
    u32 t = LW(g + off);
    float f;
    memcpy(&f, &t, 4);
    return f;
}

// a glyph record of a font: x +4, y +8, width +12, height +16, advance +20
u32 font_glyph(u32 font, unsigned char ch) { return LW(font + 8 + ch * 4u); }

// one icon, drawn here and then written with the nearest of the 16 colours
struct Cell {
    int w, h;
    std::vector<Rgba> px;
    Cell(int w_, int h_) : w(w_), h(h_), px((size_t)(w_ * h_), Rgba{0, 0, 0, 0}) {}

    void blend(int x, int y, const Rgba &c, float cov)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        Rgba &d = px[(size_t)(y * w + x)];
        float a = c.a / 128 * cov, da = d.a / 128, out = a + da * (1 - a);
        if (out <= 0) return;
        d.r = (c.r * a + d.r * da * (1 - a)) / out;
        d.g = (c.g * a + d.g * da * (1 - a)) / out;
        d.b = (c.b * a + d.b * da * (1 - a)) / out;
        d.a = out * 128;
    }

    // a shape by a point test, four samples a side
    void fill(const std::function<bool(float, float)> &inside, const Rgba &c)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int n = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        if (inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4)) n++;
                if (n) blend(x, y, c, n / 16.0f);
            }
    }

    void round_rect(float x0, float y0, float x1, float y1, float r, const Rgba &c)
    {
        fill([=](float x, float y) {
            float dx = std::fmax(std::fmax(x0 + r - x, x - (x1 - r)), 0.0f);
            float dy = std::fmax(std::fmax(y0 + r - y, y - (y1 - r)), 0.0f);
            return dx * dx + dy * dy <= r * r;
        }, c);
    }

    void triangle(float ax, float ay, float bx, float by, float cx, float cy, const Rgba &col)
    {
        fill([=](float x, float y) {
            float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
            float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
            float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
            return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
        }, col);
    }

    // text centered at (cx, cy) in the game's FONT_A, as large as fits the width
    void text(const Sheet &sheet, u32 font, const std::string &s, float cx, float cy, float max_w, float max_scale,
              const Rgba &c)
    {
        float total = 0;
        for (unsigned char ch : s)
            if (u32 g = font_glyph(font, ch)) total += glyph_f(g, 20);
        if (total <= 0) return;
        float scale = std::fmin(max_scale, max_w / total), pen = cx - total * scale / 2;
        for (unsigned char ch : s) {
            u32 g = font_glyph(font, ch);
            if (!g) continue;
            float gx = glyph_f(g, 4), gy = glyph_f(g, 8), gw = glyph_f(g, 12), gh = glyph_f(g, 16);
            float top = cy - gh * scale / 2;
            for (int y = (int)std::floor(top); y < std::ceil(top + gh * scale); y++)
                for (int x = (int)std::floor(pen); x < std::ceil(pen + gw * scale); x++) {
                    float sum = 0;
                    for (int sy = 0; sy < 3; sy++)
                        for (int sx = 0; sx < 3; sx++) {
                            int tx = (int)(gx + (x + (sx + 0.5f) / 3 - pen) / scale);
                            int ty = (int)(gy + (y + (sy + 0.5f) / 3 - top) / scale);
                            if (tx < gx || ty < gy || tx >= gx + gw || ty >= gy + gh) continue;
                            // the letter, not its black outline: light times alpha
                            u32 e = sheet.palette + (u32)sheet.get(tx, ty) * 4;
                            sum += LB(e + 3) / 128.0f * std::fmin(LB(e) / 222.0f, 1.0f);
                        }
                    if (sum > 0) blend(x, y, c, std::fmin(sum / 9, 1.0f));
                }
            pen += glyph_f(g, 20) * scale;
        }
    }

    void store(const Sheet &sheet, int x0, int y0, const u8 *pal = nullptr) const
    {
        const u8 (*kPal)[4] = pal ? (const u8 (*)[4])pal : ::kPal;       // another palette of 16: the disc's own
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const Rgba &p = px[(size_t)(y * w + x)];
                int best = 0;
                float best_d = 1e30f;
                for (int i = 0; i < 16; i++) {
                    float da = (p.a - kPal[i][3]) * 2, dr = p.r - kPal[i][0], dg = p.g - kPal[i][1], db = p.b - kPal[i][2];
                    // the colour of a nearly transparent pixel does not show
                    float k = std::fmin(p.a, (float)kPal[i][3]) / 128;
                    float d = da * da + k * (dr * dr + dg * dg + db * db);
                    if (d < best_d) best_d = d, best = i;
                }
                sheet.set(x0 + x, y0 + y, best);
            }
    }
};

std::string key_label(int vk)
{
    static const struct { int vk; const char *name; } kShort[] = {
        {0x08, "Bksp"}, {0x0D, "Ent"}, {0x14, "Caps"}, {0x20, "Spc"}, {0x21, "PgUp"}, {0x22, "PgDn"}, {0x2D, "Ins"},
        {0x2E, "Del"}, {0xA0, "Shft"}, {0xA1, "Shft"}, {0xA2, "Ctrl"}, {0xA3, "Ctrl"}, {0xA4, "Alt"}, {0xA5, "Alt"}};
    for (auto &k : kShort)
        if (k.vk == vk) return k.name;
    if (vk >= 0x60 && vk <= 0x69) return "N" + std::to_string(vk - 0x60);
    return pc_key_name(vk).substr(0, 4);
}

void arrow(Cell &c, int dir, float cx, float cy, float r, const Rgba &col)        // 0 up, 1 down, 2 left, 3 right
{
    if (dir == 0) c.triangle(cx, cy - r, cx - r, cy + r * 0.8f, cx + r, cy + r * 0.8f, col);
    if (dir == 1) c.triangle(cx, cy + r, cx - r, cy - r * 0.8f, cx + r, cy - r * 0.8f, col);
    if (dir == 2) c.triangle(cx - r, cy, cx + r * 0.8f, cy - r, cx + r * 0.8f, cy + r, col);
    if (dir == 3) c.triangle(cx + r, cy, cx - r * 0.8f, cy - r, cx - r * 0.8f, cy + r, col);
}

void key_cap(Cell &c)
{
    c.round_rect(1, 3, 25, 25, 4, EDGE);
    c.round_rect(2, 4, 24, 24, 3, CAP);
}

// a keyboard key: the cap, and an arrow or the key's name
void draw_key(Cell &c, const Sheet &ft, u32 font, int vk)
{
    key_cap(c);
    if (vk >= 0x25 && vk <= 0x28) {
        static const int dir[4] = {2, 0, 3, 1};     // VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN
        arrow(c, dir[vk - 0x25], 13, 14, 6, WHITE);
    } else {
        c.text(ft, font, vk ? key_label(vk) : "-", 13, 14, 20, 0.9f, WHITE);
    }
}

// a gamepad button, by its name in pcconfig.cpp
void draw_pad(Cell &c, const Sheet &ft, u32 font, const std::string &name)
{
    static const struct { const char *n; Rgba col; } kFace[] = {
        {"A", {0x30, 0xa0, 0x30, 128}}, {"B", {0xd0, 0x30, 0x30, 128}}, {"X", {0x30, 0x68, 0xd8, 128}}, {"Y", {0xd8, 0xb0, 0x10, 128}}};
    for (auto &f : kFace)
        if (name == f.n) {
            // (a Nintendo pad's four are all of one dark grey, its letters white)
            c.fill([](float x, float y) { return (x - 13) * (x - 13) + (y - 14) * (y - 14) <= 11.5f * 11.5f; }, g_pad_nintendo ? Rgba{0x48, 0x48, 0x4c, 128} : f.col);
            c.text(ft, font, name, 13, 14, 16, 0.9f, WHITE);
            return;
        }
    if (name == "Plus" || name == "Minus") {    // a Nintendo pad's + and -: small round buttons with the sign, drawn (the font may have neither)
        c.fill([](float x, float y) { return (x - 13) * (x - 13) + (y - 14) * (y - 14) <= 9.5f * 9.5f; }, EDGE);
        c.fill([](float x, float y) { return (x - 13) * (x - 13) + (y - 14) * (y - 14) <= 8.5f * 8.5f; }, CAP);
        c.fill([](float x, float y) { return x >= 8.5f && x <= 17.5f && y >= 13.0f && y <= 15.0f; }, WHITE);
        if (name == "Plus") c.fill([](float x, float y) { return x >= 12.0f && x <= 14.0f && y >= 9.5f && y <= 18.5f; }, WHITE);
        return;
    }
    if (name.rfind("Pad ", 0) == 0) {           // the D-pad, with the direction lit
        c.round_rect(9, 3, 17, 25, 2, EDGE);
        c.round_rect(2, 10, 24, 18, 2, EDGE);
        c.round_rect(10, 4, 16, 24, 1, CAP);
        c.round_rect(3, 11, 23, 17, 1, CAP);
        int dir = name == "Pad Up" ? 0 : name == "Pad Down" ? 1 : name == "Pad Left" ? 2 : 3;
        static const float at[4][2] = {{13, 7.5f}, {13, 20.5f}, {6.5f, 14}, {19.5f, 14}};
        arrow(c, dir, at[dir][0], at[dir][1], 3, WHITE);
        return;
    }
    std::string label = name == "L Stick" ? "LS" : name == "R Stick" ? "RS" : name == "Start" ? "Strt" : name;
    c.round_rect(1, 6, 25, 22, 7, EDGE);
    c.round_rect(2, 7, 24, 21, 6, CAP);
    c.text(ft, font, label, 13, 14, 19, 0.75f, WHITE);
}

// A race's objective on the screen (pc_objective_screen), painted for the keyboard: its L2 and R2 are a cop's calls for the
// helicopter and the road block, which the keyboard has on the zone cameras' two keys (pcactions.cpp: events 129 and 128), not on
// the gears' keys that L2 and R2 are while driving. The game's own lines put L2 and R2 there for nothing else.
bool g_objective_keys;

// A PS2 button as the device in use has it. In a race the strings name what a button does while
// driving, but what they ask to press to go on (Cross) and pause (Start) are the fixed ones.
void draw_button(Cell &c, const Sheet &ft, u32 font, int b, bool pad, bool driving)
{
    bool fixed = !driving || b == PCB_CROSS || b == PCB_START;
    if (pad) {
        draw_pad(c, ft, font, pc_pad_name(fixed ? pc_menu_pad(b) : g_cfg.pad[b]));
        return;
    }
    if (g_objective_keys && (b == PCB_L2 || b == PCB_R2)) {
        const int *zone = g_cfg.key_zone[b == PCB_L2 ? 0 : 1];
        draw_key(c, ft, font, zone[0] ? zone[0] : zone[1]);
        return;
    }
    int vk = b == PCB_START && driving ? PC_BACK_KEY : fixed ? pc_menu_key(b) : g_cfg.key[b][0] ? g_cfg.key[b][0] : g_cfg.key[b][1];
    draw_key(c, ft, font, vk);
}

// the disc's icon of a PlayStation button, by its name in pcconfig.cpp: 0 when it has none
unsigned char ps_glyph(const std::string &n)
{
    static const struct { const char *n; char g; } k[] = {
        {"Cross", 'i'}, {"Circle", 'k'}, {"Square", 'h'}, {"Triangle", 'j'}, {"L1", 'n'}, {"R1", 'o'}, {"L2", 'l'}, {"R2", 'm'},
        {"L3", '3'}, {"R3", 'w'}, {"Options", 'a'}, {"Create", 'b'}, {"Pad Up", 'd'}, {"Pad Down", 'e'}, {"Pad Left", 'f'}, {"Pad Right", 'g'}};
    for (auto &e : k)
        if (n == e.n) return (unsigned char)e.g;
    return 0;
}

// 0x90..0x98: the sticks. On the keyboard the left stick is the arrows
void draw_stick(Cell &c, const Sheet &ft, u32 font, bool left, bool pad)
{
    if (pad || !left) {
        draw_pad(c, ft, font, left ? "L Stick" : "R Stick");
        return;
    }
    key_cap(c);
    for (int d = 0; d < 4; d++) {
        static const float at[4][2] = {{13, 8.5f}, {13, 19.5f}, {7.5f, 14}, {18.5f, 14}};
        arrow(c, d, at[d][0], at[d][1], 3, WHITE);
    }
}

}   // namespace

// The cursor of a text being typed: half a second on, half off. The game's fonts are not all whole: the first of these the font has.
std::string pc_text_cursor(u32 font_hash)
{
    if ((GetTickCount64() / 500) & 1) return " ";
    const u32 font = font_hash ? pc_call(FindFont, font_hash) : 0;
    for (unsigned char ch : {'_', '|', '-'})
        if (font && font_glyph(font, ch)) return std::string(1, (char)ch);
    return "-";
}

bool pc_text_cursor_changed()
{
    static uint64_t last;
    const uint64_t now = GetTickCount64() / 500;
    const bool changed = now != last;
    last = now;
    return changed;
}

// ---- the replay's bar --------------------------------------------------------------------
// REPLAYBOTTOM_NEW has the PS2's button under each symbol. Rows 30..60 are a flat band: each button's columns get a copy of the bare column beside
// them and the icon of the same button from BUTTONSOUT, so the bar and the help's text always say the same.
static std::atomic<u32> g_bar_stamp{1};
static struct BarIcon { int w = 0, h = 0; std::vector<u8> rgba; } g_bar_icon[9];
static const struct { int x0, x1, button; } kBarKeys[9] = {
    {16, 39, PCB_L1}, {62, 89, PCB_LEFT}, {107, 131, PCB_SQUARE}, {153, 177, PCB_CROSS}, {196, 221, PCB_TRIANGLE},
    {241, 265, PCB_CIRCLE}, {281, 309, PCB_RIGHT}, {325, 354, PCB_SELECT}, {373, 399, PCB_START}};

u32 pc_replay_bar_stamp() { return g_bar_stamp.load(); }

void pc_replay_bar_retouch(u32 *px, int stride)
{
    for (int i = 0; i < 9; i++) {
        const auto &k = kBarKeys[i];
        const BarIcon &ic = g_bar_icon[i];
        for (int y = 29; y <= 60; y++)
            for (int x = k.x0 - 2; x <= k.x1 + 2; x++) px[(size_t)y * stride + x] = px[(size_t)y * stride + k.x0 - 3];
        if (!ic.w) continue;
        const int ox = (k.x0 + k.x1 + 1 - ic.w) / 2, oy = 45 - ic.h / 2;
        for (int y = 0; y < ic.h; y++)
            for (int x = 0; x < ic.w; x++) {
                const u8 *s = &ic.rgba[(size_t)(y * ic.w + x) * 4];
                u32 &d = px[(size_t)(oy + y) * stride + ox + x];
                const int a = s[3] > 128 ? 128 : s[3];
                u32 out = d & 0xff000000;
                for (int c = 0; c < 3; c++) {
                    const int was = (d >> (c * 8)) & 255;
                    out |= (u32)((s[c] * a + was * (128 - a) + 64) / 128) << (c * 8);
                }
                d = out;
            }
    }
}

// The layouts' screen's hints have cells of their own in the buttons' font (below): 1 the three spare ones of the sticks', 0
// Select's and L3's, where this font has no glyphs in the spare ones (the hints then stand as they stood before 2026-11-25,
// and a layout of the player's own shows those two buttons as the hints); -1 not known yet (the font is not there).
static int g_hint_cells = -1;
int pc_layout_hint_cells() { return g_hint_cells; }

// the keys, the device or the context changed: the sheet again
void pc_paint_buttons()
{
    static PcConfig last;
    static int last_state = -1;
    // the game's controller layouts (Joystick Config, b6657464) show what each button does in a race: the race's keys and buttons
    const bool layouts = pc_screen_hash() == 0xb6657464;
    // That screen is the gamepad's: it shows a gamepad's buttons whatever is in use, PlayStation's unless an Xbox pad is there or chosen.
    // Its hints (Back and Help at the bottom, and "press ... to edit" under a layout of the player's own) are the device's in use, as
    // on every screen: they have cells of their own there, since Triangle's, Circle's and Cross's are the layout's. The cells are
    // three of the sticks' that no layout of a gamepad shows ('p', 'v', 'u': the help characters 0x90, 0x91, 0x93). They were
    // Select's and L3's until 2026-11-25; a layout of the player's own can give those two buttons a thing, and shows them.
    const bool hand = window_input_kind() == 1;
    // a race's objective, with the keyboard in use: its lines are about driving, so the keys are the driving ones (the boost's
    // Shift was a "-" there, the menus having no key for R3; the user, 2026-11-26)
    const bool objective = !hand && !layouts && pc_objective_screen();
    g_objective_keys = objective;
    bool pad = hand || layouts, driving = (pc_racing() && !pc_race_menu()) || layouts || objective;
    bool ps = layouts ? g_layout_playstation != 0 : pad && g_pad_playstation;
    int state = (pad ? 1 : 0) | (driving ? 2 : 0) | (ps ? 4 : 0) | (layouts ? 8 : 0) | (hand ? 16 : 0) | (g_pad_nintendo ? 32 : 0) | (objective ? 64 : 0);
    if (state == last_state && !memcmp(last.key, g_cfg.key, sizeof last.key) && !memcmp(last.pad, g_cfg.pad, sizeof last.pad) &&
        !memcmp(last.key_zone, g_cfg.key_zone, sizeof last.key_zone))
        return;
    u32 buttons = pc_call(FindFont, pc_hash("BUTTONSOUT")), letters = pc_call(FindFont, pc_hash("FONT_A"));
    Sheet out, ft;
    if (!out.load(buttons) || !ft.load(letters)) return;
    const bool spare = font_glyph(buttons, 'p') && font_glyph(buttons, 'v') && font_glyph(buttons, 'u');
    g_hint_cells = spare ? 1 : 0;
    last_state = state;
    memcpy(last.key, g_cfg.key, sizeof last.key);
    memcpy(last.pad, g_cfg.pad, sizeof last.pad);
    memcpy(last.key_zone, g_cfg.key_zone, sizeof last.key_zone);

    // what the disc has, before the first paint: a PlayStation pad gets its own icons from it
    static std::vector<u8> disc_px;
    static u8 disc_pal[64];
    if (disc_px.empty()) {
        const int h = 256;                  // BUTTONSOUT is 128 x 256
        disc_px.resize((size_t)out.w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < out.w; x++) disc_px[(size_t)y * out.w + x] = (u8)out.get(x, y);
        memcpy(disc_pal, host(out.palette), 64);
    }

    static const int kBtn[16] = {PCB_START, PCB_SELECT, PCB_UP, PCB_DOWN, PCB_LEFT, PCB_RIGHT, PCB_CROSS, PCB_TRIANGLE,
                                 PCB_SQUARE, PCB_CIRCLE, PCB_L1, PCB_R1, PCB_L2, PCB_R2, -1, PCB_R3};
    for (int i = 0; i < 30; i++) {
        unsigned char ch = (unsigned char)kGlyphOf[i];
        u32 g = ch == 0x92 ? 0 : font_glyph(buttons, ch);
        if (!g) continue;
        int gx = (int)glyph_f(g, 4), gy = (int)glyph_f(g, 8), gw = (int)glyph_f(g, 12), gh = (int)glyph_f(g, 16);
        // Back, Help and "edit" on the layouts' screen: the menu's button of each, in a cell of its own
        const int hint = !layouts ? -1 : spare ? (i == 16 ? PCB_TRIANGLE : i == 17 ? PCB_CIRCLE : i == 19 ? PCB_SQUARE : -1)
                                               : (i == 1 ? PCB_TRIANGLE : i == 14 ? PCB_CIRCLE : -1);
        if (hint >= 0) {
            const int menu = hint;
            if (hand && ps) {
                u32 src = font_glyph(buttons, ps_glyph(pc_pad_name(pc_menu_pad(menu))));
                int sx = src ? (int)glyph_f(src, 4) : gx, sy = src ? (int)glyph_f(src, 8) : gy;
                for (int y = 0; y < gh; y++)
                    for (int x = 0; x < gw; x++) out.set(gx + x, gy + y, disc_px[(size_t)(sy + y) * out.w + sx + x]);
            } else {
                Cell cell(gw, gh);
                if (i == 19 && !hand) draw_key(cell, ft, letters, 0x2E);         // "edit" on the keyboard: the Delete key (window_delete_pressed)
                else draw_button(cell, ft, letters, menu, hand, false);
                cell.store(out, gx, gy, ps ? disc_pal : nullptr);        // a key's cap among the disc's icons: in their colours
            }
            continue;
        }
        if (ps) {
            // the button bound, in the icon of its help character; the sticks and the rest as they are
            unsigned char from = ch;
            if (i < 16 && kBtn[i] >= 0) {
                int b = kBtn[i];
                bool fixed = !driving || b == PCB_CROSS || b == PCB_START;
                if (unsigned char want = ps_glyph(pc_pad_name(fixed ? pc_menu_pad(b) : g_cfg.pad[b]))) from = want;
            }
            u32 src = font_glyph(buttons, from);
            int sx = src ? (int)glyph_f(src, 4) : gx, sy = src ? (int)glyph_f(src, 8) : gy;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) out.set(gx + x, gy + y, disc_px[(size_t)(sy + y) * out.w + sx + x]);
            continue;
        }
        Cell cell(gw, gh);
        if (i == 14 && layouts && spare) draw_pad(cell, ft, letters, "L Stick");     // the left stick's press, on the layouts' screen
        else if (i == 14) draw_key(cell, ft, letters, 0x2E);    // the font's L3, which no other screen shows: the Delete key, for the profiles' hint
        else if (i < 16 && kBtn[i] >= 0) draw_button(cell, ft, letters, kBtn[i], pad, driving);
        else if (i >= 16 && i < 21) draw_stick(cell, ft, letters, true, pad);
        else if (i >= 21 && i < 25) draw_stick(cell, ft, letters, false, pad);
        cell.store(out, gx, gy);
    }
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) SB(out.palette + (u32)i * 4 + (u32)k, ps ? disc_pal[i * 4 + k] : kPal[i][k]);
    for (int i = 0; i < 9; i++) {
        BarIcon &ic = g_bar_icon[i];
        ic.w = 0;
        for (int n = 0; n < 16; n++)
            if (kBtn[n] == kBarKeys[i].button)
                if (u32 g = font_glyph(buttons, (unsigned char)kGlyphOf[n])) {
                    const int gx = (int)glyph_f(g, 4), gy = (int)glyph_f(g, 8);
                    ic.w = (int)glyph_f(g, 12);
                    ic.h = (int)glyph_f(g, 16);
                    ic.rgba.resize((size_t)ic.w * ic.h * 4);
                    for (int y = 0; y < ic.h; y++)
                        for (int x = 0; x < ic.w; x++)
                            memcpy(&ic.rgba[(size_t)(y * ic.w + x) * 4], ps ? &disc_pal[out.get(gx + x, gy + y) * 4] : kPal[out.get(gx + x, gy + y)], 4);
                }
    }
    g_bar_stamp++;
    // too small as an icon in the title's line: the word, of the same length
    if (g_press_text && !pc_racing()) {     // the title's text: gone once the race has been loaded
        const std::string &s = tr(pad ? "PRESS START" : "PRESS ENTER");
        memcpy(host(g_press_text), s.c_str(), std::min<size_t>(s.size() + 1, 63));
        SB(g_press_text + 63, 0);
        pc_call(UpdateAllText);
    }
}

// ---- TEMP diagnostics (HP2_DIAG=1): the attract demo's idle timer and the speech library's own messages ----------------------------
extern FILE *g_hle_log;
static bool diag_on()
{
    static const bool on = getenv("HP2_DIAG") != nullptr;
    return on;
}

// ConsiderStartDemoMode(Timer &last_active_time): the demo starts when (RealTimer - last) * k > BeginDemoModeTimeout
void HOOK_ConsiderStartDemoMode__FR5Timer(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) {
        const u32 gp = c.r[28].w[0], timer = c.r[4].w[0];
        static u32 calls, last_value;
        const u32 now = LW(gp - 19608), last = LW(timer);
        float k, timeout;
        u32 kw = LW(gp - 30500), tw = LW(gp - 23100);
        memcpy(&k, &kw, 4);
        memcpy(&timeout, &tw, 4);
        if (last != last_value || ++calls % 120 == 0) {
            fprintf(g_hle_log, "demo: timer %08x now %u last %u%s idle %.2f s of %.2f, attract mgr %08x replay state %u %u\n", timer, now, last,
                    last != last_value ? " (reset)" : "", (float)(s32)(now - last) * k, timeout, LW(0x2f0000 - 32), LW(0x2f0000 + 2400 + 4),
                    LW(0x2f0000 + 2400 + 8));
            fflush(g_hle_log);
            last_value = last;
        }
    }
    orig(c);
}

void HOOK_StartDemoMode__Fv(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) fprintf(g_hle_log, "demo: StartDemoMode\n"), fflush(g_hle_log);
    orig(c);
    if (diag_on()) fprintf(g_hle_log, "demo: StartDemoMode returned\n"), fflush(g_hle_log);
}

static void spch_string(const char *what, u32 s)
{
    char buf[300];
    int i = 0;
    for (; i < 299 && s; i++) {
        char ch = (char)LB(s + (u32)i);
        if (!ch) break;
        buf[i] = ch == '\n' ? ' ' : ch;
    }
    buf[i] = 0;
    fprintf(g_hle_log, "spch: %s %s\n", what, buf);
    fflush(g_hle_log);
}

void HOOK_S_AbortMessage__9cSEDWedgePce(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) spch_string("abort", c.r[4].w[0]);
    orig(c);
}

void HOOK_SPCHLOG_SaveMessage(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) spch_string("message", c.r[4].w[0]);
    orig(c);
}

// SPCH_AddEvent(event *): the event's first u16 is its ID
void HOOK_SPCH_AddEvent(Ctx &c, void (*orig)(Ctx &))
{
    const u32 ev = c.r[4].w[0];
    const u32 id = diag_on() ? LH(ev) : 0, gp = c.r[28].w[0];
    orig(c);
    if (diag_on()) {
        float ms;
        u32 w = LW(gp - 16548);
        memcpy(&ms, &w, 4);
        fprintf(g_hle_log, "spch: AddEvent %u (%04x %04x %04x %04x) -> %d, clock %.0f ms, frames %u\n", id, LH(ev + 2), LH(ev + 4), LH(ev + 6), LH(ev + 8),
                (int)c.r[2].w[0], ms, LW(gp - 19600));
        fflush(g_hle_log);
    }
}

void HOOK_PlaySentence__9cSEDWedgeif(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0];
    const u32 parts = LW(self + 108), state = LW(self + 120);
    orig(c);
    if (diag_on()) fprintf(g_hle_log, "spch: PlaySentence parts %u state %u -> %u, played %d\n", parts, state, LW(self + 120), (int)c.r[2].w[0]), fflush(g_hle_log);
}

void HOOK_ChangeGameFlow__F13GameFlowStateT0(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) fprintf(g_hle_log, "flow: ChangeGameFlow %u %u (ra %08x)\n", c.r[4].w[0], c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    // After an attract demo the front end opens by how it ended (0x2fc1d0, RCStateReplayDemoModeCallback and RCStateAIDemoModeCallback
    // write it, LaunchFrontendMenus reads it): 2, a button or the recording left, straight to the main menu; 1, played out, the title
    // screen and its films again, the PS2's loop, which on a PC looks like the game starting over. Here a demo that played out goes back
    // to the main menu it was started from, as one that was stopped does. 7 is the race being left for the front end.
    if (c.r[4].w[0] == 7 && LW(0x2fc1d0) == 1) SW(0x2fc1d0, 2);
    orig(c);
}

void HOOK_SetEndReason__15DemoDiscManager17DemoDiscEndReason(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) fprintf(g_hle_log, "demo: SetEndReason %u (ra %08x)\n", c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

void HOOK_RCStateNoneCallback__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on() && c.r[5].w[0] != 3) fprintf(g_hle_log, "rc: None msg %u param %u (ra %08x)\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

void HOOK_RCStateAIDemoModeCallback__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on() && c.r[5].w[0] != 3) fprintf(g_hle_log, "rc: AIDemo msg %u param %u (ra %08x)\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

// HP2_REPLAYKEEPON=1: a replay that has left its recording plays on, as the port had it before its floats were the EE's (for looking at
// where the world goes once it differs). Without it the game does what it does on a PS2: it skips the rest (VerifyDebugChecksums).
static bool replay_keeps_on()
{
    static const bool on = getenv("HP2_REPLAYKEEPON") != nullptr;
    return on;
}

static u32 g_replay_steps, g_replay_diffs;
static bool g_replay_left_told;             // this replay's leaving its recording is in the log

// RC_SKIP_REPLAY from VerifyDebugChecksums (see there). An attract demo that leaves its recording is ended, as on a PS2: the next one
// starts. A replay of the player's own race is another matter: the game closes it on the spot and the player, who asked to see the race,
// is back in it with no word (and what the port knows to have made such replays differ is mended: the racers' turns, the road blocks'
// gaps). So one of the player's that still differs plays on, as the port had it before its floats were the EE's, and the log says so.
void HOOK_PostMessage__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    extern bool g_in_verify;
    const u32 rc = LW(c.r[28].w[0] - 22216);
    const bool skip = c.r[5].w[0] == 11 && g_in_verify, demo = rc && LW(rc) == 9;     // RCSTATE_REPLAY_DEMO_MODE
    if (diag_on() && !skip) fprintf(g_hle_log, "rc: PostMessage %u %u (ra %08x)\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0]), fflush(g_hle_log);
    if (skip && (replay_keeps_on() || !demo)) {
        if (!g_replay_left_told) {
            g_replay_left_told = true;
            fprintf(g_hle_log, "replay: left its recording at step %u (the world's timer %u); it plays on\n", g_replay_steps + 1, LW(c.r[28].w[0] - 0x4c78));
            fflush(g_hle_log);
        }
        return;
    }
    orig(c);
}

ULONG64 g_replay_cycles_at;
void HOOK_ChangeState__15RaceCoordinator7RCState(Ctx &c, void (*orig)(Ctx &))
{
    const u32 state = c.r[5].w[0];
    // TEMP (HP2_DIAG): the game thread's cycles over a replay, the measure of its work that the clock's speed and the waits do not change
    ULONG64 &cycles_at = g_replay_cycles_at;
    static u32 rc_state = 0;
    if (diag_on() && (rc_state == 9 || rc_state == 4 || rc_state == 5) && state != rc_state) {
        ULONG64 now = 0;
        QueryThreadCycleTime(GetCurrentThread(), &now);
        fprintf(g_hle_log, "replay: done, %u steps, %.1f million cycles of the game's thread, %.3f million a step\n", g_replay_steps,
                (now - cycles_at) / 1e6, g_replay_steps ? (now - cycles_at) / 1e6 / g_replay_steps : 0.0);
        // HP2_DEMOQUIT: a checking run ends with its replay, not by the clock (work/nshots/t5/demos5.sh)
        static const bool quit = getenv("HP2_DEMOQUIT") != nullptr;
        if (quit) {
            fprintf(g_hle_log, "replay: %u steps in all, %u differ\n", g_replay_steps, g_replay_diffs);
            fflush(g_hle_log), fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    if (state == 4 || state == 5 || state == 9) {
        g_replay_steps = g_replay_diffs = 0;                // a replay starts
        g_replay_left_told = false;
        // HP2_QUIETCORE=<logical processor, an even one>: for measuring on a machine that does other work. The game's thread stays
        // on that processor above the others' priority, and a thread of ours that only pauses holds the processor's other half
        // (the next one), so nobody else's work shares the core; what a pausing neighbour costs is the same in every run.
        static const char *quiet = getenv("HP2_QUIETCORE");
        static bool quiet_set;
        if (quiet && !quiet_set) {
            quiet_set = true;
            const int cpu = atoi(quiet) & ~1;
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << (int)(intptr_t)p);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                for (;;) YieldProcessor();
            }, (LPVOID)(intptr_t)(cpu + 1), 0, nullptr));
            fprintf(g_hle_log, "replay: the game's thread on processor %d, its other half held\n", cpu);
        }
        QueryThreadCycleTime(GetCurrentThread(), &cycles_at);
    }
    rc_state = state;
    if (diag_on()) fprintf(g_hle_log, "rc: ChangeState %u (ra %08x)\n", c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

// ---- replays: the world's checksum -------------------------------------------------------------------------------------------------
// A replay (the attract demo's REPLAY\ files, the instant and auto replays) stores, each world step, a byte of a checksum of the world:
// WorldTimer and each car's (x + y) * 65536 (World::CalculateDebugChecksum). VerifyDebugChecksums compares it while replaying and on any
// difference posts RC_SKIP_REPLAY: the game does not show a replay that has left its recording, it goes on to the next. The port used to
// drop that message, when its floats were not the EE's to the last bit and every replay differed at its first step. They are now
// (eefloat.h), and a replay that differs is left to the game, as on a PS2: of the five attract recordings one does, T43GreenM5 at its
// step 2745, with every float operation checked against the model (-DEE_CHECK). HP2_REPLAYKEEPON=1 plays on regardless.
// Each replay's steps are counted, and the first that differs goes to the log: how far the PC's world keeps to the PS2's (HP2_DIAG).
bool g_in_verify;
static u32 g_recorded = ~0u;                // TEMP: the recording's byte VerifyDebugChecksums has just read (HP2_CHKLOG)

void HOOK_GetData__13ReplayManagerii(Ctx &c, void (*orig)(Ctx &))
{
    orig(c);
    if (g_in_verify) g_recorded = c.r[2].w[0] & 0xff;
}

// TEMP: HP2_WATCH=<timer>,<hex address>: from the replay step of that timer on, every write to that word of the game's memory and the
// code that made it (a debug register of the game's thread; the place as an offset into speed.exe, for speed.map)
static u32 g_watch_addr, g_watch_timer;
static LONG CALLBACK watch_hit(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    static int n;
    if (n++ < 600)
        fprintf(stderr, "watch: %08x = %08x at +%llx after the step of timer %u\n", g_watch_addr, LW(g_watch_addr),
                (unsigned long long)(e->ContextRecord->Rip - (DWORD64)GetModuleHandleW(nullptr)), g_watch_timer);
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_step(u32 timer)
{
    static const char *const want = getenv("HP2_WATCH");
    static bool armed;
    g_watch_timer = timer;
    if (!want || armed || (u32)strtoul(want, nullptr, 10) != timer) return;
    const char *comma = strchr(want, ',');
    if (!comma) return;
    armed = true;
    g_watch_addr = (u32)strtoul(comma + 1, nullptr, 16);
    AddVectoredExceptionHandler(1, watch_hit);
    HANDLE me = nullptr;                            // a thread's registers are set from another, while it is stopped
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &me, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE helper = CreateThread(nullptr, 0, [](void *h) -> DWORD {
        SuspendThread((HANDLE)h);
        CONTEXT cx = {};
        cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        cx.Dr0 = (DWORD64)host(g_watch_addr);
        cx.Dr7 = 1 | (1 << 16) | (3 << 18);         // the first register, on a write, four bytes
        const BOOL ok = SetThreadContext((HANDLE)h, &cx);
        ResumeThread((HANDLE)h);
        return ok;
    }, me, 0, nullptr);
    WaitForSingleObject(helper, INFINITE);
    CloseHandle(helper), CloseHandle(me);
    fprintf(stderr, "watch: armed on %08x at timer %u\n", g_watch_addr, timer);
}

void HOOK_VerifyDebugChecksums__5World(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rm = LW(gp - 22440);      // pReplayManager: +0 playing, +4 recording
    const u32 flag = rm ? LW(rm + 12) : 0;
    const bool playing = rm && LW(rm) && !LW(rm + 4);
    const u32 world = c.r[4].w[0];
    g_in_verify = true;
    orig(c);
    g_in_verify = false;
    if (!playing) return;
    g_replay_steps++;
    watch_step(LW(gp - 0x4c78));
    // TEMP: HP2_MEMPATCH=<timer>,<file>: the file's pairs of words, an address and what to put there, after the step of that timer
    static const char *const mp = getenv("HP2_MEMPATCH");
    if (mp)
        if ((u32)strtoul(mp, nullptr, 10) == LW(gp - 0x4c78) && strchr(mp, ','))
            if (FILE *f = fopen(strchr(mp, ',') + 1, "rb")) {
                u32 pair[2], n = 0;
                while (fread(pair, 4, 2, f) == 2) SW(pair[0], pair[1]), n++;
                fclose(f);
                fprintf(stderr, "mempatch: %u words\n", n);
            }
    // TEMP: HP2_POKE=<timer>,<hex address>=<hex word>[,<address>=<word>...]: words put into the game's memory after the step of that timer
    static const char *const poke = getenv("HP2_POKE");
    if (poke)
        if ((u32)strtoul(poke, nullptr, 10) == LW(gp - 0x4c78))
            for (const char *p = strchr(poke, ','); p; p = strchr(p + 1, ',')) {
                char *eq = nullptr;
                const u32 a = (u32)strtoul(p + 1, &eq, 16);
                if (eq && *eq == '=') {
                    fprintf(stderr, "poke: %08x was %08x", a, LW(a));
                    SW(a, (u32)strtoul(eq + 1, nullptr, 16));
                    fprintf(stderr, ", now %08x\n", LW(a));
                }
            }
    // TEMP: HP2_MEMDUMP=<prefix> with HP2_MEMDUMPT=<timer>[,<timer>...]: the game's whole memory as the step of that timer leaves it
    static const char *const pre = getenv("HP2_MEMDUMP"), *const list = getenv("HP2_MEMDUMPT");
    if (pre) {
        char want[16], name[400];
        snprintf(want, sizeof want, "%u", LW(gp - 0x4c78));
        if (list && strstr(list, want)) {
            snprintf(name, sizeof name, "%s_%s.bin", pre, want);
            if (FILE *f = fopen(name, "wb")) fwrite(g_ram, 1, 0x02000000, f), fclose(f);
        }
    }
    // TEMP: HP2_CHKLOG=<file>, the parts of the checksum each step of a replay: the timer, each car's (x + y) * 65536 as the sum takes it
    // (World +0x1c cars at +0x2c; a car's place at +0x60), the word the object at World +0x18 adds (+0x380), and whether the step matched
    static FILE *chk = getenv("HP2_CHKLOG") ? fopen(getenv("HP2_CHKLOG"), "w") : nullptr;
    if (chk) {
        const u32 n = LW(world + 0x1c), extra = LW(world + 0x18);
        fprintf(chk, "%u t %u %s rec %02x cars %u extra %08x:%08x seed %08x |", g_replay_steps, LW(gp - 0x4c78), LW(rm + 12) != flag ? "DIFF" : "ok",
                g_recorded, n, extra, extra ? LW(extra + 0x380) : 0, LW(world + 0x178));
        for (u32 i = 0; i < n && i < 40; i++) {
            const u32 car = LW(world + 0x2c + i * 4);
            float x, y, z;
            const u32 wx = LW(car + 0x60), wy = LW(car + 0x64), wz = LW(car + 0x68);
            memcpy(&x, &wx, 4), memcpy(&y, &wy, 4), memcpy(&z, &wz, 4);
            fprintf(chk, " %08x(%.3f %.3f %.3f)[%08x %08x]", car, x, y, z, wx, wy);
        }
        fprintf(chk, "\n");
        if (extra && g_replay_steps >= 2738 && g_replay_steps <= 2752) {      // the object's own words about the one that is summed
            fprintf(chk, "  obj");
            for (u32 a = 0x2c0; a < 0x3c0; a += 4) fprintf(chk, " %03x:%08x", a, LW(extra + a));
            fprintf(chk, "\n");
        }
        if (g_replay_steps % 64 == 0) fflush(chk);
        static const u32 quit_at = getenv("HP2_REPLAYQUIT") ? (u32)strtoul(getenv("HP2_REPLAYQUIT"), nullptr, 10) : 0;   // TEMP: the run ends there
        if (quit_at && LW(gp - 0x4c78) == quit_at) fflush(chk), fflush(stderr), TerminateProcess(GetCurrentProcess(), 0);
    }
    if (g_replay_steps == 1500 && diag_on()) {             // TEMP: the work of a replay's first 1500 steps, in the game thread's cycles
        extern ULONG64 g_replay_cycles_at;
        ULONG64 now = 0;
        QueryThreadCycleTime(GetCurrentThread(), &now);
        fprintf(g_hle_log, "replay: 1500 steps took %.1f million cycles of the game's thread\n", (now - g_replay_cycles_at) / 1e6);
        fflush(g_hle_log);
    }
    static ULONGLONG began;                                // when the replay's first step was
    if (g_replay_steps == 1) began = GetTickCount64();
    // TEMP: HP2_SHOTSTEPS=<step>[,<step>...]: the renderer's picture (HP2_NATIVESHOT, as F11 takes it, two frames on) at those steps of a replay
    static const char *const steps = getenv("HP2_SHOTSTEPS");
    if (steps) {
        extern void native_dump_next();
        char want[16];
        snprintf(want, sizeof want, ",%u,", g_replay_steps);
        static const std::string list = std::string(",") + steps + ",";
        if (list.find(want) != std::string::npos) native_dump_next();
    }
    if (LW(rm + 12) != flag) {                             // a difference: the game set this and posted the skip
        if (replay_keeps_on()) SW(rm + 12, flag);
        if (!g_replay_diffs++ && diag_on())
            fprintf(g_hle_log, "replay: first difference at step %u (world loop %u, timer %u), %.2f s after its first step\n", g_replay_steps,
                    LW(gp - 19552), LW(gp - 19576), (GetTickCount64() - began) / 1000.0),
                fflush(g_hle_log);
    }
    if (g_replay_steps % 300 == 0 && diag_on())
        fprintf(g_hle_log, "replay: %u steps, %u differ%s\n", g_replay_steps, g_replay_diffs, g_replay_diffs ? "" : " (in step with the recording)"),
            fflush(g_hle_log);
}


// TEMP (HP2_DIAG): where a frame's CPU goes in a replay: GetTimestep (which waits for real time while a replay plays) and the world's step
static double g_t_gettimestep, g_t_dotimestep;
static u32 g_t_steps;
static double now_ms()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
// ServiceResourceLoading (0x199788): a step of the game's loading. Each call lets the queue of files read one piece (128 KB at the
// most, ServiceQueuedFiles) and turns one file that has come whole into what it is (LoadResourceIfFileTransferFinished); it returns
// whether nothing is left (gp-19944, the files still wanted, is 0). The main loop calls it once a frame, which paced the loading to
// a PS2's drive: a piece a frame, 4 to 8 MB a second. The disk here reads a piece in microseconds, so the step is repeated while
// files are wanted, for up to 8 ms of a frame: the loading's screen keeps its frames, the loading takes what the work takes. The
// order of the reads and of the files' callbacks is the game's own. HP2_PS2LOADING=1: a step a frame, as it was.
void HOOK_ServiceResourceLoading__Fv(Ctx &c, void (*orig)(Ctx &))
{
    orig(c);
    static const bool paced = getenv("HP2_PS2LOADING") != nullptr;
    if (paced) return;
    // Busy: resource files still wanted (gp-19944), or reads of the file layer on their way (its list of AsyncEntry at gp-14696 is
    // not empty: the list's head points at itself when it is). A read is a state machine made for a drive (AsyncRealCDRead,
    // 0x256130): the step that sends it says "in progress", and the step that finds it done comes with the next service, a frame
    // later on the PS2. Here the read is done when it is sent, so the next service can follow at once. It has to be a service from
    // here, the main loop: finishing the read inside the call that starts it hung the game (bReadAsync's callers, the films' and the
    // music's ring buffer among them, count on the callback coming after they have returned).
    const u32 gp = c.r[28].w[0], list = gp - 14696;
    auto busy = [&] { return LW(gp - 19944) != 0 || LW(list) != list; };
    if (!busy()) return;
    const double until = now_ms() + 8.0;
    for (int idle = 0; idle < 3;) {
        const u32 head = LW(list), state = head != list ? LW(head + 12) : 0, left = head != list ? LW(head + 24) : 0, wanted = LW(gp - 19944);
        orig(c);
        if (!busy() || now_ms() >= until) break;
        const u32 head2 = LW(list);
        const bool moved = head2 != head || LW(gp - 19944) != wanted || (head2 != list && (LW(head2 + 12) != state || LW(head2 + 24) != left));
        idle = moved ? 0 : idle + 1;
    }
}

// QueuedFile::BeginRead (0x1ad760): the next piece of a queued file, min(the piece's size at +72, what is left of the file), read
// straight into the file's own buffer (+8) at its place. The piece is 1 MB as the constructor has it and 128 KB as the loaders set
// it (ChangeQueuedFileParams): the pace of a PS2's drive, nothing the data needs. Here the piece is the whole file: one read a file.
// HP2_PS2LOADING=1: the game's pieces.
void HOOK_BeginRead__10QueuedFile(Ctx &c, void (*orig)(Ctx &))
{
    static const bool paced = getenv("HP2_PS2LOADING") != nullptr;
    if (!paced) SW(c.r[4].w[0] + 72, 0x7fffffff);
    orig(c);
}

static bool g_in_gettimestep;
void HOOK_GetTimestep__5World(Ctx &c, void (*orig)(Ctx &))
{
    const double t0 = now_ms();
    Phase timed(PH_WAIT_TICK);
    g_in_gettimestep = true;
    orig(c);
    g_in_gettimestep = false;
    g_t_gettimestep += now_ms() - t0;
}
// HP2_REPLAYNOWAIT: a replay does not wait for real time (GetTimestep spins on the ticker until its step is due): the demo as fast as it goes,
// its steps the same, for timing and profiles
void HOOK_bGetTickerDifference__FUiUi(Ctx &c, void (*orig)(Ctx &))
{
    static const bool nowait = getenv("HP2_REPLAYNOWAIT") != nullptr;
    orig(c);
    if (nowait && g_in_gettimestep) c.f[0].f = 1e9f;
    // The game spins here, a core kept busy, till the step is due: GetTimestep's loop is while (difference * $f22 < $f20), both registers
    // its own and still set in this call. While more than 2.5 ms are left the thread sleeps a millisecond (the timer's resolution is 1 ms,
    // iop_kernel.cpp), and the loop asks again; the end of the wait is the game's own spin, as exact as before. HP2_SPINWAIT=1: as it was.
    static const bool spin = getenv("HP2_SPINWAIT") != nullptr;
    if (g_in_gettimestep && !spin && !nowait) {
        const float left = c.f[20].f - c.f[0].f * c.f[22].f;
        if (left > 0.0025f && left < 1.0f) Sleep(1);
    }
}
#ifdef EE_COUNT
uint64_t g_ee_count[8];
// where the divisions and multiplications outside the world's step are made: return addresses, the most frequent to the log
static bool g_in_step;
static struct Site { void *at; uint64_t n; } g_sites[2][65536];
__attribute__((noinline)) void ee_count_site(int kind)
{
    if (g_in_step) return;
    void *ra = __builtin_return_address(0);
    Site &e = g_sites[kind][((uintptr_t)ra * 0x9E3779B97F4A7C15ull) >> 48];
    if (e.at != ra) e.at = ra, e.n = 0;
    e.n++;
}
static void ee_sites_log()
{
    for (int kind = 0; kind < 2; kind++)
        for (int k = 0; k < 12; k++) {
            Site *best = nullptr;
            for (Site &e : g_sites[kind])
                if (e.n && (!best || e.n > best->n)) best = &e;
            if (!best) break;
            fprintf(g_hle_log, "site: %s at image + %llx: %llu\n", kind ? "mul" : "div", (unsigned long long)((uintptr_t)best->at - (uintptr_t)GetModuleHandleA(nullptr)), (unsigned long long)best->n);
            best->n = 0;
        }
    memset(g_sites, 0, sizeof g_sites);
}
#endif
void HOOK_DoTimestep__5Worldf(Ctx &c, void (*orig)(Ctx &))
{
    const double t0 = now_ms();
    Phase timed(PH_WORLD_REST);
#ifdef EE_COUNT
    uint64_t at_entry[8];
    memcpy(at_entry, g_ee_count, sizeof at_entry);
    g_in_step = true;
#endif
    orig(c);
    g_t_dotimestep += now_ms() - t0;
#ifdef EE_COUNT
    g_in_step = false;
    if ((g_t_steps + 1) % 900 == 0) ee_sites_log();
    {
        static uint64_t in_step[8], before[8];
        for (int i = 0; i < 8; i++) in_step[i] += g_ee_count[i] - at_entry[i];
        if ((g_t_steps + 1) % 300 == 0) {
            fprintf(g_hle_log, "count: a step: add %.0f mul %.0f div %.0f sqrt %.0f add4 %.0f mul4 %.0f recurrences %.0f; outside the step, a step: add %.0f mul %.0f div %.0f sqrt %.0f add4 %.0f mul4 %.0f\n",
                    in_step[0] / 300.0, in_step[1] / 300.0, in_step[2] / 300.0, in_step[3] / 300.0, in_step[4] / 300.0, in_step[5] / 300.0, in_step[6] / 300.0,
                    (g_ee_count[0] - before[0] - in_step[0]) / 300.0, (g_ee_count[1] - before[1] - in_step[1]) / 300.0, (g_ee_count[2] - before[2] - in_step[2]) / 300.0,
                    (g_ee_count[3] - before[3] - in_step[3]) / 300.0, (g_ee_count[4] - before[4] - in_step[4]) / 300.0, (g_ee_count[5] - before[5] - in_step[5]) / 300.0);
            for (int i = 0; i < 8; i++) before[i] = g_ee_count[i], in_step[i] = 0;
        }
    }
#endif
    if (++g_t_steps % 300 == 0 && diag_on()) {
        fprintf(g_hle_log, "timing: %u steps: GetTimestep %.2f ms a step, DoTimestep %.2f ms a step\n", g_t_steps, g_t_gettimestep / 300, g_t_dotimestep / 300);
        fflush(g_hle_log);
        g_t_gettimestep = g_t_dotimestep = 0;
    }
}

// TEMP: HP2_DEMOWHOLE=1, the attract demo does not jump from scene to scene: the scene it starts with has no end (the snapshot's +0x34,
// which ServiceDemoMode waits for), so the recording plays on through what lies between the scenes. For telling a world that has left
// its recording from a scene that does not start as the recording was.
void HOOK_AdvanceSnapshot__14DemoModeReplayb(Ctx &c, void (*orig)(Ctx &))
{
    static const bool whole = getenv("HP2_DEMOWHOLE") != nullptr;
    const u32 self = c.r[4].w[0];
    orig(c);
    if (!whole) return;
    if (const u32 snap = LW(self + 0x20)) {
        fprintf(stderr, "demo: scene %u, snapshot %08x: times %u..%u, end %u, now endless\n", LW(self + 0x1c), snap, LW(snap + 0x28), LW(snap + 0x2c), LW(snap + 0x34));
        SW(snap + 0x34, 0x7fffffff);
    }
}

// What a recording's snapshot leaves out. A scene of an attract demo starts from a snapshot in the recording, and the game's snapshots do
// not hold everything. A road block (tCopManager, gp-15620: +316 of them from +320, 120 bytes each) comes back with its place (+68) but
// without its gap (+92, the point AIActionAvoidRoadBlock, 0x1c98e0, steers its car through): CalculateRoadBlockParameters (0x226478) made
// that with the world's random numbers when the block was called, the snapshot does not carry it and nothing makes it again, so the car
// steers by what fresh memory holds (MemoryPool's 0xaa), leaves the recording, and VerifyDebugChecksums ends the demo. That is the green
// BMW's demo (JUL23\T43GreenM5, the fourth), three steps into its second scene, and by the game's own code the PS2's too. The gap below
// is the recording's own: what that road block holds when the recording is played through without the jump (HP2_DEMOWHOLE), with which
// every step of the scene matches the recorded checksums. HP2_NOREPLAYFIX=1 leaves the recording as it is.
struct ReplayFix {
    u32 demo, timer;            // which recording (DemoModeReplay +36), and the world's timer the snapshot brings
    float cx, cy;               // the road block, by its place
    u32 gap_x, gap_y;           // its gap, as the words of the two floats
};
static const ReplayFix kReplayFixes[] = {{4, 1328074, -211.068f, 2533.61f, 0xc35c400bu, 0x451efe57u}};

// The same for a race played here. The game takes a snapshot by writing it and reading it back at once (TakeSnapshot, 0x13e9f0, calls
// RestoreSnapshot), so that the race goes on from what a replay will start from; and the road blocks come out of that without their gaps
// too, in the race itself. Their gaps are noted before a snapshot is taken and put back after it, and kept by the world's timer: a replay
// that starts from that snapshot gets the same ones. HP2_NOREPLAYFIX=1: none of it.
struct BlockGap {
    u32 cx, cy, gap_x, gap_y;   // the block's place and its gap, as the floats' words
};
struct SnapshotGaps {
    u32 timer;
    std::vector<BlockGap> blocks;
    long long think_tick;       // the racers' turns: the count they go on from (native.cpp, HOOK_Think)
};
static std::vector<SnapshotGaps> g_snapshot_gaps;       // the last 4096 snapshots (two a second in a race)
long long native_think_state();
void native_think_restore(long long tick);
static bool g_taking_snapshot;

static std::vector<BlockGap> road_block_gaps(u32 gp)
{
    std::vector<BlockGap> out;
    const u32 cops = LW(gp - 15620);
    if (!cops) return out;
    const u32 n = LW(cops + 316);
    for (u32 i = 0; i < n && i < 64; i++) {
        const u32 block = cops + 320 + i * 120;
        out.push_back({LW(block + 68), LW(block + 72), LW(block + 92), LW(block + 96)});
    }
    return out;
}

static u32 road_block_gaps_back(u32 gp, const std::vector<BlockGap> &gaps)
{
    const u32 cops = LW(gp - 15620);
    if (!cops) return 0;
    const u32 n = LW(cops + 316);
    u32 put = 0;
    for (u32 i = 0; i < n && i < 64; i++) {
        const u32 block = cops + 320 + i * 120;
        for (const BlockGap &g : gaps)
            if (g.cx == LW(block + 68) && g.cy == LW(block + 72)) {
                if (LW(block + 92) != g.gap_x || LW(block + 96) != g.gap_y) put++;
                SW(block + 92, g.gap_x);
                SW(block + 96, g.gap_y);
                break;
            }
    }
    return put;
}

void HOOK_TakeSnapshot__13ReplayManagerPCc(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NOREPLAYFIX") != nullptr;
    const u32 gp = c.r[28].w[0];
    if (off) {
        orig(c);
        return;
    }
    const std::vector<BlockGap> gaps = road_block_gaps(gp);
    const long long think_tick = native_think_state();
    const bool was = g_taking_snapshot;
    g_taking_snapshot = true;
    orig(c);
    g_taking_snapshot = was;
    const u32 put = road_block_gaps_back(gp, gaps);
    const u32 timer = LW(gp - 0x4c78);
    for (auto it = g_snapshot_gaps.begin(); it != g_snapshot_gaps.end();)
        it = it->timer == timer ? g_snapshot_gaps.erase(it) : it + 1;
    if (g_snapshot_gaps.size() >= 4096) g_snapshot_gaps.erase(g_snapshot_gaps.begin());
    g_snapshot_gaps.push_back({timer, gaps, think_tick});
    if (diag_on()) fprintf(g_hle_log, "replay: snapshot taken at timer %u with %zu road blocks, %u gaps put back\n", timer, gaps.size(), put), fflush(g_hle_log);
}

void HOOK_RestoreSnapshot__13ReplayManagerP14ReplaySnapshot(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0];
    orig(c);
    static const bool off = getenv("HP2_NOREPLAYFIX") != nullptr;
    const u32 demo = LW(0x2f0960 + 36), timer = LW(gp - 0x4c78), cops = LW(gp - 15620);
    if (diag_on() && !g_taking_snapshot) fprintf(g_hle_log, "replay: snapshot restored, demo %u, timer %u\n", demo, timer), fflush(g_hle_log);
    if (off) return;
    if (!g_taking_snapshot)                 // a replay starts from a snapshot of this race: the gaps its road blocks had
        for (const SnapshotGaps &s : g_snapshot_gaps)
            if (s.timer == timer) {
                native_think_restore(s.think_tick);
                const u32 put = road_block_gaps_back(gp, s.blocks);
                if (diag_on()) fprintf(g_hle_log, "replay: %zu road blocks of that snapshot, %u gaps put back\n", s.blocks.size(), put), fflush(g_hle_log);
            }
    if (!cops) return;
    for (const ReplayFix &f : kReplayFixes) {
        if (f.demo != demo || f.timer != timer) continue;
        const u32 n = LW(cops + 316);
        for (u32 i = 0; i < n && i < 64; i++) {
            const u32 block = cops + 320 + i * 120, wx = LW(block + 68), wy = LW(block + 72);
            float x, y;
            memcpy(&x, &wx, 4), memcpy(&y, &wy, 4);
            if (std::fabs(x - f.cx) > 0.01f || std::fabs(y - f.cy) > 0.01f) continue;
            if (diag_on())
                fprintf(g_hle_log, "replay: road block %u gets its gap, %08x %08x for %08x %08x\n", i, f.gap_x, f.gap_y, LW(block + 92), LW(block + 96)),
                    fflush(g_hle_log);
            SW(block + 92, f.gap_x);
            SW(block + 96, f.gap_y);
        }
    }
}

// TEMP: HP2_DEMOREPLAY=<1..5>, the attract demo plays that recording (DemoModeReplay +36, which ConsiderLaunching takes over its random one)
void HOOK_ConsiderLaunching__14DemoModeReplay(Ctx &c, void (*orig)(Ctx &))
{
    static const int which = getenv("HP2_DEMOREPLAY") ? atoi(getenv("HP2_DEMOREPLAY")) : -1;
    if (which >= 1 && which <= 5) SW(c.r[4].w[0] + 36, (u32)which);
    orig(c);
}

// ---- HP2_HEAPLOG=<file>: what the game's heap holds ---------------------------------------------------
// The heap is the PS2's: one MemoryPool from the program's end to the stack, under 29 MB. A block's header (8 bytes before what bMalloc
// returns: flags, padding, the mark 0x2222, the block's bytes) has no name, but nearly every allocation is asked for with one
// (bMalloc(size, name, line, flags), which goes on to bMalloc(size, flags)). With HP2_HEAPLOG the live blocks are kept here by that
// name, or by the caller's return address where there is none, and every 900 frames the file gets the sum by name: bytes, blocks, the
// largest. Without it the hooks only pass on.
#include <algorithm>
#include <map>
#include <vector>
#include <unordered_map>
namespace {
struct HeapBlock {
    std::string name;
    u32 bytes;
};
const char *const g_heap_log = getenv("HP2_HEAPLOG");
std::unordered_map<u32, HeapBlock> g_heap_live;
std::string g_heap_name;
u64 g_heap_allocs, g_heap_frees;
u32 g_heap_peak;
}   // namespace

void HOOK_bMalloc__FiPCcii(Ctx &c, void (*orig)(Ctx &))
{
    if (g_heap_log) g_heap_name = pc_str(c.r[5].w[0]);
    orig(c);
    g_heap_name.clear();
}

void HOOK_bMalloc__Fii(Ctx &c, void (*orig)(Ctx &))
{
    if (!g_heap_log) {
        orig(c);
        return;
    }
    char by[24];
    snprintf(by, sizeof by, "@%08x", c.r[31].w[0]);
    const std::string name = g_heap_name.empty() ? by : g_heap_name;
    orig(c);
    const u32 p = c.r[2].w[0];
    if (!p) return;
    g_heap_live[p] = {name, LW(p - 4) + LB(p - 7)};
    g_heap_allocs++;
}

void HOOK_bFree__FPv(Ctx &c, void (*orig)(Ctx &))
{
    if (g_heap_log && g_heap_live.erase(c.r[4].w[0])) g_heap_frees++;
    orig(c);
}

void HOOK_bResizeMalloc__FPvi(Ctx &c, void (*orig)(Ctx &))
{
    const u32 p = c.r[4].w[0];
    orig(c);
    if (!g_heap_log || !p) return;
    auto it = g_heap_live.find(p);
    if (it != g_heap_live.end()) it->second.bytes = LW(p - 4) + LB(p - 7);
}

// once a frame, from the renderer's end of frame
void pc_heap_report(u32 frame)
{
    if (!g_heap_log) return;
    u32 live = 0;
    for (auto &b : g_heap_live) live += b.second.bytes;
    if (live > g_heap_peak) g_heap_peak = live;
    if (frame % 900) return;
    struct Sum {
        u64 bytes = 0;
        u32 blocks = 0, largest = 0;
    };
    std::map<std::string, Sum> by;
    for (auto &b : g_heap_live) {
        Sum &s = by[b.second.name];
        s.bytes += b.second.bytes, s.blocks++;
        if (b.second.bytes > s.largest) s.largest = b.second.bytes;
    }
    std::vector<std::pair<u64, std::string>> order;
    for (auto &s : by) order.push_back({s.second.bytes, s.first});
    std::sort(order.rbegin(), order.rend());
    static FILE *f = fopen(g_heap_log, "w");
    if (!f) return;
    fprintf(f, "---- frame %u %s: live %u bytes in %zu blocks (peak %u), free %u, largest free %u; %llu allocations, %llu frees so far\n", frame,
            pc_racing() ? "race" : "menus", live, g_heap_live.size(), g_heap_peak, pc_call(0x257988, 0), pc_call(0x2579b0, 0),
            (unsigned long long)g_heap_allocs, (unsigned long long)g_heap_frees);
    for (auto &o : order) {
        const Sum &s = by[o.second];
        fprintf(f, "%10llu %6u %9u  %s\n", (unsigned long long)s.bytes, s.blocks, s.largest, o.second.c_str());
    }
    fflush(f);
}
