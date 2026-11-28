// The port's settings as pages of the game's menus (pcmenu.h): Options > PC Settings, and
// PC Controls for the keyboard and gamepad bindings. Values live in g_cfg (hp2.ini).
#include <atomic>
#include <chrono>
#include <cstdio>
#include "native.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "hw.h"
#include "pcconfig.h"
#include "pcmenu.h"
#include "pclang.h"

static std::string onoff(int v) { return v ? "On" : "Off"; }

// ---- Keyboard, Gamepad: one row per thing to do while driving -------------------------------
// Select waits for a key (or a pad button). The menus' keys are fixed.
static int g_binding = -1;      // the button being bound, -1: none
static bool g_binding_p2;       // player 2's keys

static std::string key_text(int (*keys)[2], int b)
{
    std::string s = pc_key_name(keys[b][0]);
    if (keys[b][1]) s += "/" + pc_key_name(keys[b][1]);
    return s;
}

// A row of the keyboard's page: an action's key (the first of the pair the file has room for; the second is not used). The rows are the PS2 buttons of layout A, by what each does, and after them the
// two zone cameras.
static const int kKeyRows = PCB_COUNT + 3;
static int *key_row(bool p2, int row)
{
    if (row < PCB_COUNT) return (p2 ? g_cfg.key2 : g_cfg.key)[row];
    return (p2 ? g_cfg.key2_zone : g_cfg.key_zone)[row - PCB_COUNT];
}

// An action has one key, and a key one action and one player: it leaves whatever had it.
static void bind_key(bool p2, int row, int vk)
{
    for (int who = 0; who < 2; who++)
        for (int r = 0; r < kKeyRows; r++)
            for (int k = 0; k < 2; k++)
                if (key_row(who != 0, r)[k] == vk) key_row(who != 0, r)[k] = 0;
    int *mine = key_row(p2, row);
    mine[0] = vk;
    mine[1] = 0;
}

// A number set in a row, digit by digit (the wheel's rotation; the user, 2026-11-24: "let it be given in digits, by hand", and
// then "the field must take a gamepad's input too"). Select on the row opens it: three digits, the one being set blinking.
// Left and Right go from digit to digit, Up and Down turn the digit, a digit key of the keyboard writes it and goes on to the
// next; Select takes the number, Back leaves the row as it was. All of it by the menu's own buttons (pcmenu.cpp, pc_row_edit),
// so a gamepad sets it as the keyboard does. (The first build took the keys through the bindings' capture, which keeps the
// game from seeing any input: nothing was written and the keyboard was gone.)
static bool g_typing;
static int g_typing_player;             // whose wheel's rotation is being set
static int g_digit[3], g_digit_at;
void pc_wheel_lock_changed();           // wheel_win.cpp: the wheel is set again
static const char *const kRotationBlurb = "Degrees from lock to lock. Select, then set the number.";

static bool typing_blink()
{
    using namespace std::chrono;
    return (duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 300) & 1;
}

static std::string typing_shown()
{
    std::string out;
    for (int i = 0; i < 3; i++) out += i == g_digit_at && typing_blink() ? '-' : (char)('0' + g_digit[i]);
    return out;
}

static bool typing_button(int msg)
{
    if (msg == 6 || msg == 7) g_digit_at = (g_digit_at + (msg == 6 ? 2 : 1)) % 3;
    else if (msg == 8 || msg == 9) g_digit[g_digit_at] = (g_digit[g_digit_at] + (msg == 8 ? 1 : 9)) % 10;
    else if (msg == 17) {
        g_typing = false;
        pc_set_blurb("Cancelled.");
        return false;
    } else if (msg == 1) {
        const int n = g_digit[0] * 100 + g_digit[1] * 10 + g_digit[2];
        if (n < 40 || n > 900) {
            pc_set_blurb("The rotation is 40 to 900 degrees.");
            return true;
        }
        g_typing = false;
        g_cfg.wheel_lock[g_typing_player] = n;
        pc_config_save();
        pc_wheel_lock_changed();
        pc_set_blurb(kRotationBlurb);
        return false;
    }
    return true;
}

// each frame of the menus: the keyboard's digits, and the blinking
static void typing_frame()
{
    static bool was[20], blink;
    if (!g_typing || !pc_row_editing()) {
        g_typing = false;
        memset(was, 1, sizeof was);         // a digit held as the row opens is not a press
        return;
    }
    bool changed = false;
    for (int k = 0; k < 20; k++) {
        const bool now = window_key_down(k < 10 ? 0x30 + k : 0x60 + k - 10);       // 0 to 9 of the row and of the number pad
        if (now && !was[k]) {
            g_digit[g_digit_at] = k % 10;
            if (g_digit_at < 2) g_digit_at++;
            changed = true;
        }
        was[k] = now;
    }
    const bool phase = typing_blink();
    if (changed || phase != blink) pc_refresh_page();
    blink = phase;
}

// A gamepad layout of the player's own (pcplatform.cpp): the thing a button is being chosen for, and whose layout it is
static int g_custom_binding = -1, g_custom_player;

// A button to a thing of a player's layout. One button, one thing: what had the button gets the button this thing had (or none).
static void custom_take(int player, int action, int button)
{
    int *b = g_cfg.custom[player];
    const int was = b[action];
    for (int a = 0; a < PCC_COUNT; a++)
        if (a != action && b[a] == button) b[a] = was;
    b[action] = button;
}

// The pedals by the triggers' travel: the throttle and the brake have a trigger each. A trigger one of them has already is kept;
// the other one, or both (the right one the throttle), are taken from what had them, which gets the pedal's button for it.
static void custom_pedal_triggers(int player)
{
    int *b = g_cfg.custom[player];
    const auto trigger = [](int x) { return x == PCB_L2 || x == PCB_R2; };
    const bool gas = trigger(b[PCC_ACCEL]), brake = trigger(b[PCC_BRAKE]);
    if (gas && brake && b[PCC_ACCEL] != b[PCC_BRAKE]) return;
    if (gas) custom_take(player, PCC_BRAKE, b[PCC_ACCEL] == PCB_R2 ? PCB_L2 : PCB_R2);
    else if (brake) custom_take(player, PCC_ACCEL, b[PCC_BRAKE] == PCB_R2 ? PCB_L2 : PCB_R2);
    else custom_take(player, PCC_ACCEL, PCB_R2), custom_take(player, PCC_BRAKE, PCB_L2);
}

// What a change of a layout left with no button, said in the help line: a button taken from a thing that gets none back, or
// the D-pad taken for the steering, would otherwise go unnoticed until the car has no handbrake (the user's picture of
// 2026-11-25: Handbrake and Zone Zoom with no button after two changes). The things' names, as the page's rows have them:
static const char *const kCustomLabel[PCC_COUNT] = {"Steer Left", "Steer Right", "Accelerate", "Brake / Reverse", "Handbrake", "Speed Boost", "Shift Up",
                                                     "Shift Down", "Look Back", "Change View", "Horn / Siren", "Reset Car", "Cycle HUD",
                                                     "Zone Freeze / Helicopter", "Zone Zoom / Road Block"};
static bool custom_lost(int player, const int *before)
{
    std::string lost;
    for (int a = 0; a < PCC_COUNT; a++) {
        if (before[a] < 0 || g_cfg.custom[player][a] >= 0) continue;
        if ((a == PCC_STEER_LEFT || a == PCC_STEER_RIGHT) && g_cfg.custom_steer[player] == 0) continue;     // (the stick steers: not wanted)
        if ((a == PCC_ZONE_FREEZE || a == PCC_ZONE_ZOOM) && !g_cfg.custom_zone_buttons[player]) continue;   // (the game's way: not wanted)
        lost += (lost.empty() ? " " : ", ") + tr(kCustomLabel[a]);
    }
    if (lost.empty()) return false;
    pc_set_blurb(tr("Now without a button:") + lost);
    return true;
}

static void custom_bound(int what, int code)
{
    const int player = g_custom_player, action = g_custom_binding;
    g_custom_binding = -1;
    if (what == 2) {
        int button = code & 0x40 ? PC_CUSTOM_L3 : -1;       // (0x40: the left stick's press)
        for (int b = 0; b < PCB_COUNT; b++)
            if (g_cfg.pad[b] & code) button = b;
        if (button == PCB_START) {
            pc_set_blurb("Start is always Pause.");
        } else if (button < 0) {
            pc_set_blurb("This button cannot be used.");
        } else if (!g_cfg.custom_pedals_digital[player] && (button == PCB_L2 || button == PCB_R2)) {
            // (the pedals are the two triggers, by their travel: another thing with one would leave a pedal a plain button)
            pc_set_blurb("The triggers are the pedals while Accelerate/Brake is Analog.");
        } else {
            int before[PCC_COUNT];
            memcpy(before, g_cfg.custom[player], sizeof before);
            custom_take(player, action, button);
            pc_config_save();
            pc_custom_changed(player);
            if (!custom_lost(player, before)) pc_set_blurb("Done. Select a row to bind it again.");
        }
    } else {
        pc_set_blurb("Cancelled.");
    }
    pc_refresh_page();
}

static void poll_capture()
{
    if (g_binding < 0 && g_custom_binding < 0) return;
    window_capture_frame();                 // the capture's step: it runs by this page's frames (window.cpp)
    int code = 0, what = window_capture_poll(code);
    if (!what) return;
    if (g_custom_binding >= 0) {
        custom_bound(what, code);
        return;
    }
    if (what == 1) bind_key(g_binding_p2, g_binding, code);
    g_binding = -1;
    if (what != 3) pc_config_save();
    pc_set_blurb(what == 3 ? "Cancelled." : "Done. Select a row to bind it again.");
    pc_refresh_page();
}

static const int kDriving[] = {PCB_CROSS, PCB_SQUARE, PCB_LEFT, PCB_RIGHT, PCB_R1, PCB_R3, PCB_L1, PCB_TRIANGLE,
                               PCB_CIRCLE, PCB_R2, PCB_L2, PCB_SELECT, PCB_UP, PCB_START};

// the pages of bindings: "<base>", "<base> 2"...; the keyboard pauses with Esc, always
// Off, 2, 4 ... top, round and round
static int pc_next_power(int now, int top, int d)
{
    if (d < 0) return now == 0 ? top : now == 2 ? 0 : now / 2;
    return now == 0 ? 2 : now >= top ? 0 : now * 2;
}

// the most the graphics card has: 0 of multisampling, 1 of anisotropy; all of the setting's range until the renderer knows
static int gpu_limit(int what)
{
    int aa = 8, aniso = 16;
    native_vk_limits(aa, aniso);
    return what ? aniso : aa;
}

static std::string keys_text(const int *k) { return pc_key_name(k[0]); }

// The keyboard's keys, by action, both players' in one list (the user, 2026-11-23): the first player's, then the second
// player's, which are used when player 2 has half of the keyboard (Controls > Player 2), each with "P2" before its name (the
// user, 2026-11-24: "take the Player 2 divider away and just put P2 at the start of player 2's bindings"; the mark is not
// translated, pcmenu.cpp's tr_label). Esc pauses, always.
static void binding_page(const char *name, const char *title)
{
    PcPage &p = pc_add_page(name, title);
    // (a key that a cop has for something else says so: the horn's key is the next suspect, the boost is the cop's alone, and the
    // zone cameras' two keys are the cop's calls for the helicopter and for a road block: Player::SetInputMode gives a cop the
    // events 128 and 129 where a racer has the zones)
    for (int who = 0; who < 2; who++) {
        const bool p2 = who != 0;
        auto add = [&p, p2](int row, const char *action, const char *help = "Select, then press the key for it. Esc cancels.") {
            PcRow r{p2 ? std::string("P2 ") + action : std::string(action), help,
                    [row, p2] {
                        if (g_binding == row && g_binding_p2 == p2) return std::string("Press...");
                        return keys_text(key_row(p2, row));
                    },
                    [row, p2](int) {
                        g_binding = row;
                        g_binding_p2 = p2;
                        window_capture_begin(1);
                        pc_set_blurb("Press a key. Esc cancels.");
                    }};
            r.select_only = true;
            p.rows.push_back(r);
        };
        for (int b : kDriving)
            if (b == PCB_CIRCLE) add(b, pc_button_action(b), "Horn. Police car: siren. Cop: next suspect.");
            else if (b == PCB_R3) add(b, pc_button_action(b), "The cop's turbo: a few seconds faster.");
            else if (b != PCB_START) add(b, pc_button_action(b));
        add(PCB_COUNT + 0, "Zone Freeze / Helicopter", "Racer: Zone Freeze. Cop: call the helicopter.");
        add(PCB_COUNT + 1, "Zone Zoom / Road Block", "Racer: Zone Zoom. Cop: ask for a road block.");
        // (the first player's alone: the user, 2026-11-25, "player 2 does not need slow motion, it does not work there")
        if (!p2) add(PCB_COUNT + 2, "Zone Slow Motion", "The game's slow motion, on and off. Works with Slow Motion on in Extras.");
    }
    // One row for all the keys of the page, the last of the list (the user, 2026-11-25: "move the Reset to Default button to
    // the very bottom of the list"; there was one after each player's keys, the first in the middle of the list)
    p.rows.push_back({"Reset to Default", "Put these keys back as they were.", nullptr, [](int) {
                          g_cfg.default_keys();
                          g_cfg.default_keys2();
                          pc_config_save();
                          pc_set_blurb("Back to the defaults.");
                          pc_refresh_page();
                      }});
}

// ---- Profiles -----------------------------------------------------------------------------------
// Each profile is a folder of saves/: its save, its controls. The screen comes up at every start, in front of the game's load of the save, which then loads that
// profile's. Options' Profiles is the same screen; there another profile takes starting over, as the game's state is the save it started from.
bool mc_profiles_enabled();
std::string mc_profile_dir(const std::string &name);
std::vector<std::string> mc_profiles();
void mc_profile_use(const std::string &name);
bool mc_profile_delete(const std::string &name);

static const u32 kMoveBackScreen = 0x170320, kFred = 0x2fc050, kFredName1 = kFred + 0x10d90;
static const char *const kProfilesPage = "PC Profiles";
static const char *const kNewProfileBlurb = "Select, then type its name. A gamepad gets the letter grid.";
static std::string g_profile;           // the one in use; empty until one is chosen
static bool g_profiles_off, g_open_profiles, g_back_to_load, g_rebuild;
static bool g_made;                     // a profile was made: the rebuilt list says so

// ---- the profile's save, as the game is left ------------------------------------------------------
// The game writes a profile's save after a race that was finished and from its own Options, nowhere else. What was chosen in the menus
// since (the Challenge screen's Players and Race Type among the rest: DataChallengeScreen, Fred +8, put back there as the screen is left)
// lived until the program closed, and the next start had the old values again: "two players every time" (the user, 2026-11-14). So the
// save is also written as the game is left from its menus.
// A save is Fred's image, 0x114e0 bytes: its size in the first word and the sum of its first 17719 words in the last
// (SavegameData::SetupForSave, 0x245738; FrontendDatabase::CalculateChecksum, 0x1878b0). Not from a race: Fred then holds the race under
// way, which the game never saves. Not before the profile's own save is in Fred: until the main menu is up it holds the game's defaults,
// or the profile played before.
static std::atomic<bool> g_fred_ready;
static char g_exit_save[512];           // the save's file, for the window's thread

static void exit_save_off() { g_fred_ready = false; }

// A start without the opening films: one made by a switch of the menus that needs the game started anew (window_relaunch's
// no_intro, HP2_NOINTRO: Extras' Playable Traffic Cars, Select GPU; the same profile, HP2_PROFILE), and every start with Extras'
// No Intro Movies on (the user, 2026-11-24). The title screen plays no films when the game's own SkipMovies (gp-23036) is set as
// TitleScreen's constructor (0x16f000) runs: it goes straight to waiting for Start. Nothing else: no passing the title by, no
// way back to a page (that was there, and the user had it taken out: "the restart really restarts the game, only without the
// intro films").
//
// SkipMovies is set for the constructor's call alone. It is a setting of the game's (LoadConfigItems, SaveConfigItems), and
// FlailerScreenList::StartMovie (0x16ce30) asks it too: the films behind the menus' screens. Held from the boot screen to the
// title, as it first was, it took the film away from behind the profiles' screen, which comes between the two (the user,
// 2026-11-24: "with the No Intro switch there is no video on the profiles' screen").
//
// With SkipMovies set the constructor also starts the jukebox itself (0x16f0a4: sound::jukebox::Start(true)), as HandleTick does
// when the films have run to their end (0x16f2c0). A film stopped with a button does not (HandleEvent, 0x16eef8): the title is
// silent then and the music comes with the main menu, which is what a player who skips the films knows. So that start of the
// music is left out (the user, the same day: "the menu's music starts before Enter is pressed: not wanted").
static const bool g_no_intro = getenv("HP2_NOINTRO") != nullptr;
static const u32 kSkipMovies = 0x3358f0 - 23036;      // gp-23036
static bool g_title_making;                 // the title's constructor runs, made to skip its films
void HOOK_TitleScreenMake(Ctx &c, void (*orig)(Ctx &))
{
    static bool made;                       // this start's first title screen has been made
    const bool skip = g_cfg.no_intro || (g_no_intro && !made);
    if (skip && !made) fprintf(stderr, "menus: the title screen without the opening films\n");
    made = true;
    if (!skip) {
        orig(c);
        return;
    }
    const u32 was = LW(kSkipMovies);
    SW(kSkipMovies, 1);
    g_title_making = true;
    orig(c);
    g_title_making = false;
    SW(kSkipMovies, was);
}

void HOOK_JukeboxStart(Ctx &c, void (*orig)(Ctx &))
{
    if (g_title_making) return;
    orig(c);
}

// the main menu is up: the profile's load is behind
void pc_profile_loaded()
{
    if (g_profile.empty() || g_fred_ready) return;
    snprintf(g_exit_save, sizeof g_exit_save, "%s/save.dat", mc_profile_dir(g_profile).c_str());
    g_fred_ready = true;
}

// the window's thread, as the window goes
static bool profile_write(const char *why);
void pc_save_on_exit()
{
    if (!g_fred_ready || pc_racing()) return;
    profile_write("as the game is left");
}

// In place of the game's "Autosave" screen (pcmenu.cpp, HOOK_ChangeToScreen__17FlailerScreenList): the same save, written by the
// port at once. The game's own Autosave saves Fred also after a race (the progress), so this one is written in a race too.
bool pc_profile_save_ready() { return g_fred_ready; }
bool pc_save_profile_now() { return g_fred_ready && profile_write("in place of the game's Autosave"); }

static bool profile_write(const char *why)
{
    const u32 kSaveSize = 0x114e0;
    std::vector<u8> image(kSaveSize), old;
    memcpy(image.data(), host(kFred), kSaveSize);
    memcpy(image.data(), &kSaveSize, 4);
    u32 sum = 0;
    for (u32 at = 0; at < kSaveSize - 4; at += 4) {
        u32 word;
        memcpy(&word, &image[at], 4);
        sum += word;
    }
    memcpy(&image[kSaveSize - 4], &sum, 4);
    const std::string file = g_exit_save, part = file + ".part";
    if (FILE *f = fopen(file.c_str(), "rb")) {
        old.resize(kSaveSize + 1);
        old.resize(fread(old.data(), 1, old.size(), f));
        fclose(f);
    }
    if (old == image) return true;
    FILE *f = fopen(part.c_str(), "wb");
    if (!f) return false;
    const bool whole = fwrite(image.data(), 1, kSaveSize, f) == kSaveSize;
    fclose(f);
    std::error_code ec;
    if (whole) std::filesystem::rename(part, file, ec);     // over the old one, at once
    if (!whole || ec) std::filesystem::remove(part, ec);
    fprintf(stderr, "profiles: the save is written %s (%s)%c", why, whole && !ec ? "done" : "failed", 10);
    return whole && !ec;
}

static void use_profile(const std::string &name)
{
    exit_save_off();                    // Fred is not this profile's yet
    const std::string dir = mc_profile_dir(name);
    std::error_code ec;
    const bool has_save = std::filesystem::exists(dir + "/save.dat", ec);
    const bool fresh = !has_save && !std::filesystem::exists(dir + "/controls.ini", ec);
    mc_profile_use(name);
    snprintf(g_cfg.last_profile, sizeof g_cfg.last_profile, "%s", name.c_str());
    pc_config_use_profile(dir + "/controls.ini", fresh);
    g_profile = name;
    if (!has_save) {                    // the game's start: its first player has the profile's name, as far as the game's names go
        u8 *to = host(kFredName1);
        memset(to, 0, 11);
        memcpy(to, name.data(), name.size() < 7 ? name.size() : 7);
    }
}

// A new profile's gamepads on layout I, the modern one, for both players (the user, 2026-11-26). The layout a profile has is
// Fred's DesiredControllerConfig (+376, a word a player), which its save keeps and MemcardTester::OverwriteFred (0x245d30) hands to
// the joysticks (SetJoystickUserConfiguration, 0x155000). The game's default is 0 there, "the pad type's first": layout A.
// FrontendDatabase::InitWithDefaults (0x183830) is what makes Fred for a profile with no save: at the start (InitFred), here when
// such a profile is chosen (choose_profile), and the game's language screen of the first start. After it the layout is I, in
// Fred and in the joysticks (at the start no save comes over them, and nothing else sets the word). A profile with a save gets
// what its save says, as before: its 0 is A. On a PS2 a save with I in it is layout A again (ChooseJoystickConfigurationNumber
// takes the type's first for a layout its list does not have).
void HOOK_InitWithDefaults__16FrontendDatabase(Ctx &c, void (*orig)(Ctx &))
{
    const u32 fred = c.r[4].w[0];
    orig(c);
    if (fred != kFred) return;
    for (u32 port = 0; port < 2; port++) {
        SW(fred + 376 + port * 4, PC_MODERN_LAYOUT);
        pc_call(0x155000, port, PC_MODERN_LAYOUT);
    }
}

static void choose_profile(const std::string &name)
{
    if (g_profile.empty()) {
        use_profile(name);
        g_back_to_load = true;
    } else if (name != g_profile) {
        // Another profile while the game runs, as the game's own Load does it. Its state is Fred (FrontendDatabase, 0x2fc050), which a
        // save is an image of. A profile with a save: the loader of the start (the screen BootMemcardLoader) comes over this one, reads
        // it, checks it, puts it in Fred (MemcardTester::OverwriteFred, 0x245d30) and goes. A profile with none: Fred as the game starts
        // with it (FrontendDatabase::InitWithDefaults, 0x183830), put in place by the same OverwriteFred, which also hands the sound
        // and the players what the image says.
        pc_config_save();
        std::error_code ec;
        const bool has_save = std::filesystem::exists(mc_profile_dir(name) + "/save.dat", ec);
        if (!has_save) pc_call(0x183830, kFred);
        use_profile(name);
        if (has_save) {
            pc_call(0x14cc78, 0x2bef10, 1);         // ChangeToScreen("BootMemcardLoader", over this one)
            return;
        }
        const u32 kSaveSize = 0x114e0;
        const u32 image = pc_alloc(kSaveSize), tester = pc_alloc(32);
        if (image && tester) {
            memcpy(host(image), host(kFred), kSaveSize);
            SW(tester + 0x10, tester + 0x18);       // MemcardTester's SavegameData, whose first word is the image
            SW(tester + 0x18, image);
            pc_call(0x245d30, tester);
        }
        if (image) pc_call(0x257850, image);        // bFree
        if (tester) pc_call(0x257850, tester);
        g_rebuild = true;
    }
}

static void make_profile(std::string name)
{
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) {
        pc_set_blurb("A profile needs a name.");
        return;
    }
    for (const std::string &n : mc_profiles())
        if (!_stricmp(n.c_str(), name.c_str())) {
            pc_set_blurb("There is a profile of that name.");
            return;
        }
    std::error_code ec;
    if (!std::filesystem::create_directories(mc_profile_dir(name), ec)) {
        pc_set_blurb("That name cannot be a folder.");
        return;
    }
    // made, not loaded: it is one more row, to be selected like the others
    g_rebuild = true;
    g_made = true;
}

static void profile_rows(PcPage &pg)
{
    pg.rows.clear();
    if (!g_profile.empty()) fprintf(stderr, "profiles: %s in use, player 1 is %s\n", g_profile.c_str(), pc_str(kFredName1).c_str());
    std::vector<std::string> names = mc_profiles();
    // the one played last comes first
    auto last = std::find(names.begin(), names.end(), std::string(g_cfg.last_profile));
    if (last != names.end()) std::rotate(names.begin(), last, last + 1);
    // the first screen, before a profile is chosen: there is nowhere to go back to, Back offers to leave the game
    pg.hide_back = false;
    pg.back_text = g_profile.empty() ? "Exit" : "Back";
    if (g_profile.empty()) pg.on_back = [] { pc_ask_exit(); };
    else pg.on_back = nullptr;
    for (const std::string &n : names) {
        PcRow r{n, "Play as this profile.", [n] { return n == g_profile ? std::string("Current") : std::string(); },
                [n](int) {
                    if (n == g_profile) pc_set_blurb("This is the profile in use.");
                    else choose_profile(n);
                }};
        r.select_only = true;
        pg.rows.push_back(r);
    }
    PcRow add{"New Profile", kNewProfileBlurb,
              nullptr,
              [](int) {
                  // a gamepad: the game's letter grid; the keyboard types into the same seven places
                  const bool pad = window_input_kind() == 1;
                  if (pc_name_grid([](const std::string &name) {
                          if (name.empty()) pc_set_blurb(kNewProfileBlurb);
                          else make_profile(name);
                      }, pad)) {
                  }
              }};
    add.select_only = true;
    pg.rows.push_back(add);
}

// Delete, or Square (X) on the gamepad, on a profile's row: the game's question, and on Yes the profile goes, its save and its controls.
// On the first screen as in Options. The profile in use stays: the game is running on its save.
static void delete_frame()
{
    const int row = pc_page_row(kProfilesPage);
    if (row >= 0 && !pc_asking()) {      // (a question hides the page under it, the hint too)
        // the hint only where the button does something: not on the profile in use, not on New Profile, not while a name is typed
        PcPage *page = pc_find_page(kProfilesPage);
        const bool can = page && row < (int)page->rows.size() - 1 && page->rows[(size_t)row].label != g_profile && !pc_name_grid_up();
        // with the grid up the same button (Square, X) changes its letters to capitals and back, as on the PS2
        if (pc_name_grid_mode() == 2) pc_delete_hint(kProfilesPage, true, true, "Capitals");
        else pc_delete_hint(kProfilesPage, window_input_kind() == 1, can);
    }
    const bool pressed = pc_delete_pressed();           // asked every frame: a press is one press (the key, the pad, or the pointer on the hint)
    if (!pressed || row < 0 || pc_asking() || pc_name_grid_up()) return;
    PcPage *pg = pc_find_page(kProfilesPage);
    if (!pg || row >= (int)pg->rows.size() - 1) return;        // the last row is New Profile
    const std::string name = pg->rows[(size_t)row].label;
    if (name == g_profile) {
        pc_set_blurb("The profile in use cannot be deleted.");
        return;
    }
    std::string sure = tr("Delete the profile %s? Its save and its controls will be gone.");
    if (size_t at = sure.find("%s"); at != std::string::npos) sure.replace(at, 2, name);
    pc_ask("Delete Profile", sure, [name](bool yes) {
        if (!yes) return;
        mc_profile_delete(name);
        g_rebuild = true;
    });
}

static void profiles_frame()
{
    if (!g_profile.empty()) SW(kFred + 4, 1);       // autosave: a profile always keeps its progress
    if (g_open_profiles) {
        g_open_profiles = false;
        pc_open_page(kProfilesPage);
    }
    if (g_back_to_load) {
        g_back_to_load = false;
        pc_call(kMoveBackScreen, 1);
    }
    delete_frame();
    if (g_rebuild) {
        g_rebuild = false;
        for (PcPage *pg : {pc_find_page(kProfilesPage)})
            if (pg) {
                profile_rows(*pg);
                pg->top = 0;
            }
        pc_reshape_page();
        if (g_made) pc_set_blurb("The profile is made. Select it to play as it.");
        g_made = false;
    }
}

// The load at the start waits for a profile. The screen stays up while it does; the loader comes when the choice brings the game back here.
void HOOK_Callback__14BootAutoLoader13ScreenMessageP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    if (g_profile.empty() && !g_profiles_off) {
        if (const char *forced = getenv("HP2_PROFILE")) {
            use_profile(forced);
        } else if (!mc_profiles_enabled()) {
            g_profiles_off = true;
        } else {
            if (c.r[4].w[0] == 11) g_open_profiles = true;
            return;
        }
    }
    orig(c);
}

// ---- Extras > Speed Boost for All Cars --------------------------------------------------------
// "Speed Boost for all cars", a patch for PCSX2 by Silent (the user gave its two .pnach files, 2026-11-24; the same addresses for
// the American and the European disc): 0x13C24C = li s1,1; 0x13C264 and 0x13C268 = nop. All three are in
// RealDriver::JoyHandler_NitroBoost (0x13c228), the handler of JOY_EVENT_NITRO_BOOST (127: R3 in the pad's layouts, the keyboard's
// Speed Boost key), which RealDriver::AddJoyHandlers gives every player in every mode. As the game has it, the press does
// something only when the car's kind (CarTypeInfo +1336) is 1, a cop car, and the race is one where a player is the cop (the
// word at 0x31ab0c); then, if NitroAvailable (0x13c378: no fly-by running, the last boost over: it lasts 4 s, gp-22448, with no
// wait after it, gp-22444), the driver's nitro word +552 is set to 1 and the time +632 to the clock's (gp-19576). The patch takes
// the two conditions out; so does this, with the switch on. Nothing else changes: the computer's drivers have their own boost.
// The author is not named in the row (the user: his patch is to be credited in the credits' technical part).
void HOOK_NitroBoostHandler(Ctx &c, void (*orig)(Ctx &))
{
    if (!g_cfg.boost_all) {
        orig(c);
        return;
    }
    g_pc_ctx = &c;
    const u32 value = c.r[6].w[0] & 0xff, driver = c.r[7].w[0];
    if (!value || !driver) return;
    if (pc_call(0x13c378, driver)) {
        SW(driver + 552, 1);
        SW(driver + 632, LW(0x3358f0 - 19576));
    }
}

// ---- Extras > Disable Catchup -------------------------------------------------------------------
// "Disable Catchup", a patch for PCSX2 by Silent (the user gave its two .pnach files, 2026-11-24; the same for both discs): four
// stores made to store zero, at 0x216D24, 0x2171E8, 0x217864 and 0x1ED66C. All four write the game's own Catch-up setting,
// RaceParameters +760 (0x31ab90): StartQuickRace (0x216c18), the two career mode screens' HandleEvent (CopsOnModeSelect,
// 0x2170b0; CopsOffModeSelect, 0x217768) and RaceParameters::InitWithDefaults (0x1ed5c8) set it to 1 there. Catch-up is a row
// of the race setup screen ("Set the catch-up boost on or off."), which writes the same word (SetupRaceParametersAfterRaceSetup,
// 0x18662c); in Quick Race and the career the game turns it on without asking. With it off the opponents get no boost by their
// distance from the player and their place (AICarController::CalculateDistanceAndPositionBoost, 0x192cd0); with two players
// it is the one behind who loses a multiplier (Car::CalculateNewCatchupMultipliers, 0x1351a0).
// Here, with the switch on, the same four places leave the word at 0: the two plain ones after they have run, the two screens'
// when the call did write the word (it is given a mark before the call; a word still marked was not written and gets its
// value back). A demo of the attract mode brings its own race parameters (DemoModeReplay::RestoreRaceParametersSnapshot), as on
// a patched PS2. The author is not named in the row (as with the speed boost: for the credits' technical part).
static const u32 kCatchup = 0x31ab90;           // TheRaceParameters +760

// ---- The car select's windows open on the last choice ---------------------------------------------
// The user, 2026-11-27: "can the game remember the last choice of the transmission and the handling, and the windows start from
// it?"; his choice: from the profile, and a quick race does not change it. The car select's two windows (CarSelector::SummonPopup,
// 0x196bb0): TRANSMISSION (Automatic, Manual, Semi-Auto) and HANDLING (Classic, Extreme). The row a window opens on is the
// PopupConfig's +52, which SummonPopup takes from the selector's +120 (the transmission) and +124 (the handling); the window's
// answers (ScreenCallback, 0x197390) write the row's number back there, 0..2 and 0..1. SelectCarScreen::SetSelectedCars (0x1986c8)
// then puts them in the player's FrontendPlayerInfo, +1607 and +1606, in the same numbers. The player infos are in Fred
// (FrontendDatabase, 0x2fc050) from +0x10738, 1744 bytes each, and the profile's save is the whole of Fred (its checksum,
// CalculateChecksum 0x1878b0, runs over 0x114dc bytes): the last choice of each player is in the profile already. The game only
// never reads it back: CarSelector's constructor (0x1950d0) sets +120 and +124 to 0 every time the screen opens, so the windows
// open on Automatic and Classic. Here the constructor's two zeros become the player's bytes. Its player: view_id (+28) is 1 for
// the first player and 2 for the second (SelectCarScreen::ActivateCarSelector, 0x198518). A profile from before has 0 there, as
// the windows had.
static const u32 kPlayerInfos = 0x2fc050 + 0x10738;
static u32 player_info(int player) { return kPlayerInfos + 1744u * (u32)player; }

// StartQuickRace (0x216c18) writes the first player's transmission byte too: Manual with L1 held at the main menu, otherwise
// Automatic. The race takes its copy of the player infos in StartRace (0x184f68, called by StartQuickRace), and the loading screen
// reads the byte for its Manual mark (tLoadingScreenAnimation::Tick, 0x16fcd8, only while loading). So the byte the player chose
// at the car select is kept here and given back when the race is on (TheRaceIsOn, from Race::SetRaceStage, before any save), or
// at the next car select if the race never came to that.
static int g_quick_tranny = -1;
static void give_back_quick_tranny()
{
    if (g_quick_tranny < 0) return;
    SB(player_info(0) + 1607, (u8)g_quick_tranny);
    g_quick_tranny = -1;
}

void HOOK___11CarSelector12JoystickPortiiUiR13VisibleCarSeti(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0];
    orig(c);
    give_back_quick_tranny();
    if (!self) return;
    const int player = (int)LW(self + 28) - 1;
    if (player < 0 || player > 1) return;
    const u32 info = player_info(player);
    const s8 tranny = (s8)LB(info + 1607), handling = (s8)LB(info + 1606);
    SW(self + 120, tranny >= 0 && tranny <= 2 ? (u32)tranny : 0);
    SW(self + 124, handling >= 0 && handling <= 1 ? (u32)handling : 0);
}

void HOOK_TheRaceIsOn__16FrontendDatabaseP4Race(Ctx &c, void (*orig)(Ctx &))
{
    give_back_quick_tranny();
    orig(c);
}

void HOOK_StartQuickRace(Ctx &c, void (*orig)(Ctx &))
{
    const int chosen = LB(player_info(0) + 1607);
    orig(c);
    g_quick_tranny = chosen;
    if (g_cfg.no_catchup) SW(kCatchup, 0);
}

void HOOK_RaceParametersDefaults(Ctx &c, void (*orig)(Ctx &))
{
    const u32 params = c.r[4].w[0];
    orig(c);
    if (g_cfg.no_catchup && params) SW(params + 760, 0);
}

static void mode_select_event(Ctx &c, void (*orig)(Ctx &))
{
    if (!g_cfg.no_catchup) {
        orig(c);
        return;
    }
    const u32 kMark = 0x5ca7c4b0, was = LW(kCatchup);
    SW(kCatchup, kMark);
    orig(c);
    SW(kCatchup, LW(kCatchup) == kMark ? was : 0);
}
void HOOK_CopsOnModeSelectEvent(Ctx &c, void (*orig)(Ctx &)) { mode_select_event(c, orig); }
void HOOK_CopsOffModeSelectEvent(Ctx &c, void (*orig)(Ctx &)) { mode_select_event(c, orig); }

// ---- Extras > Cinematic Replays -----------------------------------------------------------------
// The replay's controls are a Flailer screen of INGAMEB (0x60befaed: the time bar, the markers, the buttons' panel). The button that
// hides them (ReplayScreen::JoyHandler 0x1ecf58, its event 107) puts another screen in their place, 0xddce04a3: LETTERBOX_TOP and
// LETTERBOX_BOTTOM, their two lines and a small icon, the film bars of a replay with nothing on it. That one is named only by
// ReplayScreen: shown by that button alone, hidden by it and by the screen's destructor. With the switch on it is never shown, so
// the replay with its controls hidden is the picture alone; the button shows the controls again as before. Only what is drawn:
// SetFlailerScreenVisibility (0x14cbf8; a0 the screen's hash, a1 1 hidden, 0 shown) is told "hidden" where it was told "shown".
static const u32 kReplayLetterbox = 0xddce04a3;
void HOOK_SetFlailerScreenVisibility__FUi23FlailerScreenVisibility(Ctx &c, void (*orig)(Ctx &))
{
    if (g_cfg.cinematic_replays && c.r[4].w[0] == kReplayLetterbox && c.r[5].w[0] == 0) c.r[5].d[0] = 1;
    orig(c);
}

// ---- Extras > Cockpit View ------------------------------------------------------------------------
// The view from the driver's seat, which the game has whole and almost never shows (the user, 2026-11-27: "let's turn it on for
// fun"). Three cars have it on the disc: the CarTypeInfo of F50, F50HP, SPIDER, BARCHETTA and BARCHETTAHP has a fourth POV of
// type POV_INCAR (at the driver's head) and a DashGeometryFilename (CARS\<car>\DASHGEOM: <car>_DASH, _DASH_WHEEL, _SPEED_NEEDLE,
// _TACH_NEEDLE). SPIDERHP has the POV but no file (its body is the 360 Challenge's), so it is left out by the file's name.
// What makes it shown is the ride's HasDash (RideInfo +6): CarLoader::Load (0x1b0560) then loads the dash's file beside the body,
// CycleDriveCameraPOVIndex (0x1786d0) lets the camera key reach the INCAR POV, DriveCameraMover::Update puts the camera at the
// head and sets the Camera's dash flag (+448), RenderDashboards (0x1242a8, every frame from eDisplayFrame) draws the dash, the
// steering wheel and the two needles, and RenderCars leaves the body out as it does for the bumper view. The game sets HasDash
// in FrontendPlayerInfo::BuildRideInfo only when the car's row of the player's list has dash_available (+20), which the
// challenge cars' and most events' rows do not have: so it was never seen.
// With the switch on, the player's ride is given HasDash as the race's drivers are made: SetupCarsForRace makes the player's
// DriverInfo (its ctor, 0x188610: a2 the driver's type, 1 the player; the ride copied to +12) for every race, so the switch
// holds for races started after it. Off, the ride is the game's. The split screen's POVs have no INCAR, so there the view is
// not reached (the dash's file is loaded all the same).
static const u32 kCarTypeInfoArray = 0x3358f0 - 20616;     // gp-20616: the pointer to the 64 CarTypeInfo of 1376 bytes
void HOOK___10DriverInfoi14DriverTypeEnumP8RideInfoiii(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0], type = c.r[6].w[0];
    orig(c);
    if (!g_cfg.cockpit_view || type != 1 || !self) return;
    const u32 ride = self + 12, car = LW(ride), table = LW(kCarTypeInfoArray);
    if (car >= 64 || !table) return;
    if (*host(table + car * 1376 + 128)) *host(ride + 6) = 1;      // DashGeometryFilename not empty
}

// The cockpit's steering wheel by the player's input (the user, 2026-11-27: "it turns strangely"; his choice, "from the player's
// input"). CarRenderInfo::RenderDashboard (0x123570) turns <car>_DASH_WHEEL about its X by the car's steering, CarDriver +528
// (this +0 the Car, Car +44 its CarDriver), which is the front tyres' angle itself: RealDriver::DoSteering (0x13b630) makes it
// the input, moved at the game's steering speed, through its curve, times the most the tyres may turn at this speed (for a pad
// 45 degrees standing, 13 at the table's second step, 2.9 at its last; for a wheel 45 to 6) and the car's Steering, plus the
// counter-steer of a power slide. So the wheel turned 45 degrees standing and hardly at all at speed, and jerked by itself in a
// slide. Now, for the drawing of the dash alone, +528 holds the input instead: RealDriver +596, -1 full right to 1 full left (its
// JoyHandler_Steer, 0x13b5b0, from the D-pad's or the stick's byte), as a turn of the wheel of up to kCockpitWheelLock degrees
// each way, and the game's value is put back after. A key gives the input at once, all or nothing: the drawn wheel follows it at
// most kCockpitWheelRate a second (full left to full right in a third of a second), so a key turns it and a stick is not slowed
// noticeably. Only a RealDriver (its vtable at +556, 0x2aee30) has the input; any other driver keeps the game's angle.
static const u32 kRealDriverVtable = 0x2aee30;
static const float kCockpitWheelLock = 180.0f, kCockpitWheelRate = 6.0f;
void HOOK_RenderDashboard__13CarRenderInfoP5eViewP8bVector3P8bMatrix4(Ctx &c, void (*orig)(Ctx &))
{
    const u32 info = c.r[4].w[0], car = info ? LW(info) : 0, driver = car ? LW(car + 44) : 0;
    if (!g_cfg.cockpit_view || !driver || LW(driver + 556) != kRealDriverVtable) {
        orig(c);
        return;
    }
    static float shown;
    static std::chrono::steady_clock::time_point last;
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    float input;
    memcpy(&input, host(driver + 596), 4);
    if (!(input >= -1.0f)) input = -1.0f;          // (a NaN too)
    if (input > 1.0f) input = 1.0f;
    if (dt > 0.25f || dt < 0) shown = input;        // the first drawing, or after a pause: where the input is
    else {
        const float step = kCockpitWheelRate * dt;
        shown += std::clamp(input - shown, -step, step);
    }
    u16 *steer = (u16 *)host(driver + 528);
    const u16 was = *steer;
    *steer = (u16)(s16)lrintf(shown * kCockpitWheelLock * (65536.0f / 360.0f));
    orig(c);
    *steer = was;
}

// ---- Graphics, Controls -----------------------------------------------------------------------
// ---- Extras > Cheat Menu ---------------------------------------------------------------------
// The game's cheats are button codes typed on the main menu (EasterEggs::JoyHandler, 0x24f748): a table of 78 entries of 56 bytes
// (gEasterEggs 0x31d7e8: +12 the table, +16 how many; an entry: eight buttons, +32 its kind (0 an easter egg, 1 a car, 2 a cop car
// for You're The Cop, 3 a track, 4 a track the other way), +36 which one, +44 the LanguageID of its name, +48 "entered"). A code that
// matches sets +48, and the car lists, the track lists and the rest ask the table (EasterEggs::IsEasterEggUnlocked, 0x24f9a0).
// Nothing of it is saved: a PS2 forgets them when it is switched off.
// With Cheat Menu on (Options > Extras), F8 on any menu before a race opens these pages: Effects, Cars, Cop Cars, Tracks, each
// with a row for each entry, On or Off. On is the entry's +48 set, as its code sets it; what is on is kept in hp2.ini (cheats=)
// and set again every frame of the menus, so it holds from one run to the next. With Cheat Menu off nothing is set and F8 does
// nothing; the list in hp2.ini stays for when it is on again.
//
// Six of the effects are one choice (the user, 2026-11-24: "it turned out by the game's code that the cheats cannot all work at
// once: check"; then: "fix it, and take the All / Some switch at the head of the list away"). The game has it twice over. An
// entry's +40 is a group: entering a code first clears every entry of the same group (JoyHandler calls 0x24f9f8), and the five
// of group 1 are Motion Blur, Black & White, Heavy Fog, R/C Car and Video Mode. And as a track is loaded
// (EndGameFlowLoadingTrackSpecific, 0x16924c) one render mode is chosen, by the first of these that is on (0x24bf18): entry 0
// Motion Blur (mode 8), 1 Black & White (9), 2 Heavy Fog (10), 3 R/C Car (11), 4 Video Mode (9 again), 8 Chrome Cars (12).
// The pages set +48 themselves, past the codes, and several of the six could be On with only the first of them doing anything.
// So turning one of the six on turns the other five off; what hp2.ini has from before is brought to the first of them at the
// start. The rest are free of this: the helicopter (5), the two bonus events (6, 7), every car and track.
static const u32 kEggs = 0x31d7e8, kGetTranslatedString = 0x21e560;
static int cheat_count()
{
    const u32 n = LW(kEggs + 16);
    return LW(kEggs + 12) && n <= sizeof g_cfg.cheats ? (int)n : 0;
}
static u32 cheat_entry(int i) { return LW(kEggs + 12) + 56u * (u32)i; }
// 0 Effects, 1 Cars, 2 Cop Cars, 3 Tracks (both ways)
static int cheat_section(int i)
{
    const u32 kind = LW(cheat_entry(i) + 32);
    return kind >= 3 ? 3 : (int)kind;
}
static std::string cheat_name(int i)
{
    const u32 text = pc_call(kGetTranslatedString, LW(cheat_entry(i) + 44));
    std::string name = text ? pc_str(text) : std::string();
    return name.empty() ? "#" + std::to_string(i) : name;
}
static void cheat_set(int i, bool on)
{
    g_cfg.cheats[i] = on;
    SW(cheat_entry(i) + 48, on ? 1 : 0);
}
// how many of a section are on, and how many it has (the sections' page shows it)
static void cheat_counts(int section, int &on, int &all)
{
    on = all = 0;
    for (int i = 0; i < cheat_count(); i++)
        if (cheat_section(i) == section) all++, on += g_cfg.cheats[i] != 0;
}
// What a cheat does, for its row's help line (the user, 2026-11-24: "can the cheats' descriptions be in the running line?";
// a long one runs: pcmenu.cpp, HOOK_BlurbAnimate). From the game's code, not from hearsay:
//  - the six render modes are sets of the game's own variables that a mode overwrites while it is on (Stomper, 0x24bd20; the
//    lists by mode at 0x31d718): 8 Motion Blur calls RadialBlurOnOff(1, 0); 9 Black & White sets the word eAttachBlackAndWhite
//    asks (0x32fb00); 10 Heavy Fog the word WeatherManQuery asks (0x330db8); 11 R/C Car puts 0.5 on the diagonal of a matrix
//    (0x2e0270) and sets words that CarRenderInfo::Render, AnimatedDriver::Render and the animations ask; 12 Chrome Cars sets
//    five words of the cars' render state (0x32f70c on, AttachMicroCode). Video Mode is mode 9 again, and SetHudFeatures shows a
//    group of the HUD for it;
//  - Pet Helicopter: RaceCoordinator::InitCars makes a Chopper in a race that has no pursuit, and the Chopper's own word +960
//    is set; the two bonus events: EventInfo::GetStatus; the cars and tracks: IsCarUnlocked, IsTrackUnlocked.
static const char *cheat_words(int i)
{
    const u32 e = cheat_entry(i), kind = LW(e + 32), which = LW(e + 36);
    if (kind == 1) return "Opens this car in the car lists.";
    if (kind == 2) return "Opens this police car for You're The Cop.";
    if (kind == 3) return "Opens this track in the track lists.";
    if (kind == 4) return "Opens this track driven the other way.";
    switch (which) {
    case 0: return "A blur of speed over the whole picture.";
    case 1: return "The race in black and white.";
    case 2: return "Thick fog on every track.";
    case 3: return "The cars at half their size, like radio-controlled models.";
    case 4: return "Black and white, with a video camera's overlay in the HUD.";
    case 5: return "A helicopter of your own in every race.";
    case 6: return "Opens the bonus event of the Championship.";
    case 7: return "Opens the bonus event of Ultimate Racer.";
    case 8: return "Every car's body in chrome.";
    }
    return "As if its code had been entered.";
}
// one of the six that are the game's one render mode: the entry's kind is 0 and its number 0 to 4, or 8
static bool cheat_render_mode(int i)
{
    const u32 e = cheat_entry(i), which = LW(e + 36);
    return LW(e + 32) == 0 && (which <= 4 || which == 8);
}
static const char *const kCheatPages[4] = {"PC Cheats Effects", "PC Cheats Cars", "PC Cheats Cop Cars", "PC Cheats Tracks"};

static void cheat_rows(PcPage &pg, int section)
{
    pg.rows.clear();
    for (int i = 0; i < cheat_count(); i++) {
        if (cheat_section(i) != section) continue;
        const bool mode = cheat_render_mode(i);
        // (put together in the language in use: the line is shown as it is then)
        pg.rows.push_back({cheat_name(i), mode ? tr(cheat_words(i)) + " " + tr("One of these effects at a time.") : std::string(cheat_words(i)),
                           [i] { return onoff(g_cfg.cheats[i]); },
                           [i, mode](int) {
                               const bool on = !g_cfg.cheats[i];
                               if (on && mode)          // the game draws one of them: the others off, as their codes would leave them
                                   for (int k = 0; k < cheat_count(); k++)
                                       if (k != i && cheat_render_mode(k) && g_cfg.cheats[k]) cheat_set(k, false);
                               cheat_set(i, on);
                               pc_config_save();
                           }});
    }
}

// every frame of the menus: what is on stays on, and F8 opens the pages
static void cheats_frame()
{
    {   // TEMP HP2_CHEATLOG=1: what the game's own counts say, once the menus have run a while
        static const bool on = getenv("HP2_CHEATLOG") != nullptr;
        static int frames;
        if (on && !pc_racing() && ++frames == 500)
            fprintf(stderr, "cheats: menu %d; the game counts %u cars open of kind 0, %u of kind 1, %u cop cars; track 61 forward open %u, backward %u%c", g_cfg.cheat_menu,
                    pc_call(0x24b2a0, 0), pc_call(0x24b2a0, 1), pc_call(0x24b380), pc_call(0x24b760, 61, 0), pc_call(0x24b760, 61, 1), 10);
        if (on && !pc_racing() && frames == 500) {
            // each track the career has open, forward and backward (IsTrackUnlocked, 0x24b760: no cheats in it)
            fprintf(stderr, "cheats: tracks the career has open (number forward/backward):");
            for (int t = 0; t < 48; t++) fprintf(stderr, " %d %u/%u", t, pc_call(0x24b760, (u32)t, 0), pc_call(0x24b760, (u32)t, 1));
            fprintf(stderr, "%c", 10);
        }
    }
    if (!g_cfg.cheat_menu) return;
    {   // several of the six render modes on in hp2.ini (the pages used to allow it): the first stays, which is the one that worked
        static bool once;
        if (!once && cheat_count()) {
            once = true;
            bool have = false;
            for (int i = 0; i < cheat_count(); i++)
                if (cheat_render_mode(i) && g_cfg.cheats[i]) {
                    if (have) cheat_set(i, false);
                    have = true;
                }
        }
    }
    for (int i = 0; i < cheat_count(); i++)
        if (g_cfg.cheats[i]) SW(cheat_entry(i) + 48, 1);
    static bool held;
    const bool down = window_key_down(0x77);            // F8
    const bool fresh = down && !held;
    held = down;
    if (!fresh || pc_racing() || !pc_cursor_wanted()) return;       // a menu of the front end is up, not a film or a loading screen
    if (pc_page_row("PC Cheats") >= 0) return;
    for (const char *name : kCheatPages)
        if (pc_page_row(name) >= 0) return;
    pc_open_page("PC Cheats");
}

void pc_settings_register()
{
    pc_options_pages("PC Controls", "PC Graphics");
    pc_on_frame(poll_capture);
    pc_on_frame(typing_frame);
    pc_on_frame(profiles_frame);

    // the languages: Select switches the game at once (pclang.h)
    PcPage &langs = pc_add_page("PC Languages", "Languages");
    for (size_t i = 0; i < pc_languages().size(); i++) {
        PcRow r = {pc_languages()[i].name, "The game's texts and speech in this language.",
                   [i] { return std::string((int)i == pc_language_index() ? "Selected" : ""); },
                   [i](int) {
                       if ((int)i == pc_language_index()) return;
                       if (pc_language_restarts((int)i)) {
                           // the Russian disc: Russian and the other languages are drawn with different fonts, loaded at the start
                           pc_ask("Languages", "The game will restart to change the language. Continue?", [i](bool yes) {
                               if (!yes) return;
                               pc_language_keep((int)i);
                               window_relaunch(g_profile, true);
                           });
                           return;
                       }
                       pc_language_choose((int)i);
                       pc_retranslate();
                   }};
        r.select_only = true;
        langs.rows.push_back(r);
    }

    // Extras: the game's bonus things, each a switch (the user, 2026-11-14). The first: the developers' Debug item of the pause menu.
    PcPage &extras = pc_add_page("PC Extras", "Extras");
    extras.rows.push_back({"DEBUG.not.pretty", "The developers' debug menu, in the pause menu.",
                           [] { return onoff(g_cfg.debug_menu); }, [](int) { g_cfg.debug_menu ^= 1; pc_config_save(); }});

    extras.rows.push_back({"Cheat Menu", "F8 in the menus opens the game's cheats.", [] { return onoff(g_cfg.cheat_menu); },
                           [](int) {
                               g_cfg.cheat_menu ^= 1;
                               if (!g_cfg.cheat_menu)          // off: the game as it was, the list is kept for later
                                   for (int i = 0; i < cheat_count(); i++) SW(cheat_entry(i) + 48, 0);
                               pc_config_save();
                           }});
    // "Playable traffic cars", the mod by 379Felipe (traffic.cpp), under its own name (the user, 2026-11-24). Turning it asks first:
    // the game is started again with the switch turned (window_relaunch, without the opening films), so the cars' table is the
    // mod's or the disc's from the start, before anything is made of it.
    extras.rows.push_back({"Playable Traffic Cars", "Traffic cars as racers, more skins, more traffic: the mod by 379Felipe. Switching it restarts the game.",
                           [] { return onoff(g_cfg.traffic_cars); },
                           [](int) {
                               if (!traffic_available()) {
                                   pc_set_blurb("This disc's cars are not the ones the mod is made for.");
                                   return;
                               }
                               // (the title: the popup's line has room for some eighteen letters, the mod's name was cut)
                               pc_ask("Extras", "The game will restart now to switch this mod. Continue?", [](bool yes) {
                                   if (!yes) return;
                                   g_cfg.traffic_cars ^= 1;
                                   pc_config_save();
                                   pc_save_on_exit();
                                   window_relaunch(g_profile, true);
                               });
                           }});
    // every title screen without the opening films, as at the start a switch makes (HOOK_TitleScreenMake)
    extras.rows.push_back({"No Intro Movies", "The game starts without its opening films.", [] { return onoff(g_cfg.no_intro); },
                           [](int) { g_cfg.no_intro ^= 1; pc_config_save(); }});
    // the boost a cop car has in You're the Cop, for every car in every mode (HOOK_NitroBoostHandler): at once, no restart
    extras.rows.push_back({"Speed Boost for All Cars", "The speed boost in every car and every mode.", [] { return onoff(g_cfg.boost_all); },
                           [](int) { g_cfg.boost_all ^= 1; pc_config_save(); }});
    // The game's slow motion zone, which no pad has a button for (pcactions.cpp): with this off its key does nothing (the binding
    // stays in Controls), with it on the key turns it on and off, and so do a gamepad's two sticks pressed together (the user,
    // 2026-11-24).
    extras.rows.push_back({"Slow Motion", "The slow motion key works. On a gamepad: both sticks pressed together.", [] { return onoff(g_cfg.slow_motion); },
                           [](int) { g_cfg.slow_motion ^= 1; pc_config_save(); }});
    // the game's own Catch-up left off where the game turns it on by itself (the hooks above): races started after the switch
    extras.rows.push_back({"Disable Catchup", "No catch-up boost for the opponents in Quick Race and the career.", [] { return onoff(g_cfg.no_catchup); },
                           [](int) { g_cfg.no_catchup ^= 1; pc_config_save(); }});
    // the replay's letterbox left out when its controls are hidden (HOOK_SetFlailerScreenVisibility below): the eighth row, the
    // page scrolls from here on
    extras.rows.push_back({"Cinematic Replays", "Hiding the replay's controls hides the black bars too.", [] { return onoff(g_cfg.cinematic_replays); },
                           [](int) { g_cfg.cinematic_replays ^= 1; pc_config_save(); }});
    // the game's own cockpit view of three Ferraris (HOOK___10DriverInfo... above): races started after the switch
    extras.rows.push_back({"Cockpit View",
                           "The view from the driver's seat, left unused in the game: Ferrari F50, 360 Spider and 550 Barchetta, and the "
                           "F50 and 550 Barchetta Hot Pursuit editions. The fourth camera; for races started after the switch.",
                           [] { return onoff(g_cfg.cockpit_view); },
                           [](int) { g_cfg.cockpit_view ^= 1; pc_config_save(); }});
    PcPage &cheats = pc_add_page("PC Cheats", "Cheats");
    static const char *const kSections[4][2] = {{"Effects", "The game's easter eggs."},
                                                 {"Cars", "Cars a code opens."},
                                                 {"Cop Cars", "Cop cars for You're The Cop."},
                                                 {"Tracks", "Tracks a code opens."}};
    for (int k = 0; k < 4; k++) {
        cheats.rows.push_back({kSections[k][0], kSections[k][1],
                               [k] {
                                   int on, all;
                                   cheat_counts(k, on, all);
                                   return std::to_string(on) + " / " + std::to_string(all);
                               },
                               [k](int) { pc_open_page(kCheatPages[k]); }});
        cheats.rows.back().select_only = true;
        PcPage &list = pc_add_page(kCheatPages[k], kSections[k][0]);
        for (int i = 0; i < 8; i++) list.rows.push_back({"-", "", nullptr, nullptr});       // seven slots and the arrows: the rows come as it opens
        list.on_open = [k](PcPage &pg) { cheat_rows(pg, k); };
    }
    pc_on_frame(cheats_frame);
    pc_on_frame(traffic_frame);

    PcPage &profiles = pc_add_page(kProfilesPage, "Profiles");
    for (int i = 0; i < 8; i++) profiles.rows.push_back({"Profile", "", nullptr, nullptr});
    profiles.on_open = profile_rows;
    pc_profiles_page(kProfilesPage);

    PcPage &v = pc_add_page("PC Graphics", "Graphics");
    v.rows.push_back({"Resolution", "The window, the screen's resolution and the render scale.", nullptr,
                      [](int) { pc_open_page("PC Resolution"); }});
    v.rows.push_back({"Frame Rate", "The PS2's was 30. Above 60 the game still runs at 60, with pictures drawn in between.",
                      [] { return pc_picture_rate() >= 1000 ? std::string("Unlimited") : std::to_string(pc_picture_rate()) + " FPS"; },
                      [](int d) {
                          // the same list on every screen (the user, 2026-11-17); 1000 is Unlimited: as many pictures as the card makes
                          static const int list[] = {30, 60, 120, 144, 180, 240, 300, 360, 500, 1000};
                          const int n = (int)(sizeof list / sizeof list[0]);
                          const int now = pc_picture_rate();
                          int at = 0;
                          for (int i = 0; i < n; i++)
                              if (list[i] <= now) at = i;
                          const int next = list[(at + (d < 0 ? n - 1 : 1)) % n];
                          g_cfg.frame_rate = next >= 60 ? 60 : 30;
                          g_cfg.pictures = next > 60 ? next : 0;
                          pc_config_save();
                      }});
    v.rows.push_back({"VSync", "On: each frame waits for the screen, no tearing. Off: shown at once.",
                      [] { return onoff(g_cfg.vsync); }, [](int) { g_cfg.vsync ^= 1; pc_config_save(); }});
    // Original, Advanced (200 % of the PS2's), High (400), Maximum (600): the user, 2026-11-17. Another number in hp2.ini is shown as it is.
    v.rows.push_back({"Draw Distance", "How far away scenery still shows. Original: as on the PS2. Advanced: twice as far. High: four times. Maximum: six times.",
                      [] {
                          const int n = g_cfg.draw_distance;
                          return n == 100 ? std::string("Original") : n == 200 ? std::string("Advanced") : n == 400 ? std::string("High")
                               : n == 600 ? std::string("Maximum") : std::to_string(n) + "%";
                      },
                      [](int d) {
                          static const int kSteps[] = {100, 200, 400, 600};
                          int at = 0;
                          for (int i = 0; i < 4; i++)
                              if (kSteps[i] <= g_cfg.draw_distance) at = i;
                          g_cfg.draw_distance = kSteps[(at + (d < 0 ? 3 : 1)) % 4];
                          pc_config_save();
                      }});
    v.rows.push_back({"Level of Detail", "Maximum: scenery and cars always at their most detailed models, textures sharpen smoothly as you come closer. Original: as on the PS2.",
                      [] { return std::string(g_cfg.max_detail ? "Maximum" : "Original"); },
                      [](int) {
                          g_cfg.max_detail ^= 1;
                          pc_config_save();
                      }});
    v.rows.push_back({"Reflections", "The reflections on the cars. Advanced: new every frame, your mirrors show the road behind. High: the same at twice the resolution.",
                      [] { return std::string(g_cfg.reflections == 2 ? "High" : g_cfg.reflections ? "Advanced" : "PS2"); },
                      [](int d) {
                          g_cfg.reflections = (g_cfg.reflections + (d < 0 ? 2 : 1)) % 3;
                          pc_config_save();
                      }});
    v.rows.push_back({"Shadows", "The cars' shadows. High: the body's own outline, lying on whatever is under it. Very High: finer, and the tunnels' lamps cast them too.",
                      [] { return std::string(g_cfg.shadows >= 3 ? "Ultra" : g_cfg.shadows == 2 ? "Very High" : g_cfg.shadows ? "High" : "PS2"); },
                      [](int d) {
                          g_cfg.shadows = (g_cfg.shadows + (d < 0 ? 3 : 1)) % 4;
                          pc_config_save();
                      }});
    v.rows.push_back({"Show FPS", "Frames per second in the corner of the picture.",
                      [] { return onoff(g_cfg.show_fps); }, [](int) { g_cfg.show_fps ^= 1; pc_config_save(); }});
    v.rows.push_back({"Advanced", "Anti-aliasing, texture filtering, brightness, contrast and gamma.", nullptr,
                      [](int) { pc_open_page("PC Advanced"); }});

    // The graphics cards, a list as the profiles are (the user, 2026-11-17): the game's own choice, then each card by its name.
    // Another one is taken by starting the game again, on the same profile. (Resolution > Select GPU: it was Graphics > Graphics
    // Card for an hour.)
    PcPage &gpus = pc_add_page("PC GPU", "Select GPU");
    for (int i = 0; i < 8; i++) gpus.rows.push_back({"Select GPU", "", nullptr, nullptr});
    gpus.on_open = [](PcPage &pg) {
        pg.rows.clear();
        std::vector<std::string> names;
        std::string now;
        native_vk_gpus(names, now);
        const auto take = [](const std::string &want) {
            if (want == g_cfg.gpu) {
                pc_set_blurb("This is the choice in use.");
                return;
            }
            pc_ask("Select GPU", "The game will restart to draw with another graphics card. Continue?", [want](bool yes) {
                if (!yes) return;
                snprintf(g_cfg.gpu, sizeof g_cfg.gpu, "%s", want.c_str());
                pc_config_save();
                pc_save_on_exit();
                window_relaunch(g_profile, true);
            });
        };
        PcRow own{"Automatic", "The game chooses: a separate graphics card before a built-in one.",
                  [] { return std::string(g_cfg.gpu[0] ? "" : "Current"); }, [take](int) { take(std::string()); }};
        own.select_only = true;
        pg.rows.push_back(own);
        for (const std::string &n : names) {
            PcRow r{n, "Draw the game with this graphics card.", [n, now] { return std::string(n == g_cfg.gpu ? "Current" : n == now ? "In Use" : ""); },
                    [take, n](int) { take(n); }};
            r.select_only = true;
            pg.rows.push_back(r);
        }
    };

    PcPage &res = pc_add_page("PC Resolution", "Resolution");
    res.rows.push_back({"Window", "Window or borderless fullscreen. Alt+Enter switches too.",
                      [] { return std::string(g_cfg.fullscreen ? "Fullscreen" : "Windowed"); },
                      [](int) { window_set_fullscreen(!g_cfg.fullscreen); }});
    // The resolution, of those the monitor has room for: the window's size, or fullscreen the size the picture is made at before it
    // is stretched to the monitor (the monitor's own as it comes). Each mode keeps its own.
    res.rows.push_back({"Resolution", "The picture's size. Fullscreen stretches it to the monitor.",
                      [] {
                          int w, h;
                          window_view_size(w, h);
                          return std::to_string(w) + "x" + std::to_string(h);
                      },
                      [](int d) {
                          // (with the ultrawide ones, 21:9 and 32:9, and small ones of those shapes that fit a 16:9 screen as a window)
                          static const int kSizes[][2] = {{800, 600},   {960, 720},   {1024, 768},  {1280, 540},  {1280, 720},  {1280, 800},  {1280, 960},
                                                          {1366, 768},  {1600, 900},  {1600, 1200}, {1680, 1050}, {1720, 720},  {1920, 540},  {1920, 800},
                                                          {1920, 1080}, {1920, 1200}, {2560, 720},  {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440},
                                                          {3840, 1080}, {3840, 1600}, {3840, 2160}, {5120, 1440}, {5120, 2160}};
                          int sw, sh;
                          window_screen_size(sw, sh);
                          std::vector<std::pair<int, int>> fits;
                          bool own = false;       // the monitor's own size is among them
                          for (const auto &k : kSizes)
                              if (k[0] <= sw && k[1] <= sh) fits.push_back({k[0], k[1]}), own |= k[0] == sw && k[1] == sh;
                          if (g_cfg.fullscreen && !own) fits.push_back({sw, sh});
                          if (fits.empty()) fits.push_back({800, 600});
                          const int n = (int)fits.size();
                          int w, h, at = -1;
                          window_view_size(w, h);
                          for (int i = 0; i < n; i++)
                              if (fits[(size_t)i].first == w && fits[(size_t)i].second == h) at = i;
                          at = at < 0 ? (d < 0 ? n - 1 : 0) : (at + (d < 0 ? n - 1 : 1)) % n;
                          w = fits[(size_t)at].first, h = fits[(size_t)at].second;
                          if (g_cfg.fullscreen) {
                              const bool monitors = w == sw && h == sh;
                              g_cfg.fs_w = monitors ? 0 : w;
                              g_cfg.fs_h = monitors ? 0 : h;
                              pc_config_save();
                          } else {
                              window_set_size(w, h);
                          }
                      }});
    // The screen the game is shown on: the one its window is on now, by Windows' number and the screen's own name (the user,
    // 2026-11-18: no "Automatic" and no size here). Left and right take the window to the next screen and keep that for the next
    // start; till a screen is chosen (hp2.ini's monitor=0) the window is where Windows puts it.
    res.rows.push_back({"Monitor", "The screen the game is shown on.",
                      [] {
                          const int at = window_monitor_now();
                          if (at < 1) return std::string("-");
                          const std::string number = std::to_string(window_monitor_number(at)), name = window_monitor_name(at);
                          return name.empty() ? number : number + ": " + name;
                      },
                      [](int d) {
                          const int n = window_monitor_count();
                          if (n < 2) {
                              pc_set_blurb("This is the only screen.");
                              return;
                          }
                          int at = window_monitor_now();
                          if (at < 1) at = 1;
                          window_set_monitor((at - 1 + (d < 0 ? n - 1 : 1)) % n + 1);
                      }});
    // The aspect as it is drawn (pc_aspect): 4:3, or the shape of the resolution. A 16:9 resolution has no 16:10 picture.
    res.rows.push_back({"Aspect Ratio", "The race's picture: as wide as the resolution, or 4:3. The menus stay 4:3.",
                      [] {
                          // the shape by its usual name, if it is within a twentieth of one
                          const int a = pc_aspect();
                          if (!a) return std::string("4:3");
                          static const struct { double r; const char *name; } kShapes[] = {{16.0 / 10, "16:10"}, {16.0 / 9, "16:9"}, {2.0, "18:9"},
                                                                                           {64.0 / 27, "21:9"}, {32.0 / 9, "32:9"}};
                          const double r = a * 0.0001;
                          const char *name = nullptr;
                          double least = 0.05;
                          for (const auto &k : kShapes)
                              if (fabs(r / k.r - 1.0) < least) least = fabs(r / k.r - 1.0), name = k.name;
                          if (name) return std::string(name);
                          char own[16];
                          snprintf(own, sizeof own, "%.2f:1", r);
                          return std::string(own);
                      },
                      [](int) {
                          if (!pc_aspect_limit()) {
                              pc_set_blurb("A 4:3 resolution shows the race in 4:3. A wider one has the wider picture.");
                              return;
                          }
                          g_cfg.aspect = g_cfg.aspect ? 0 : 1;
                          pc_config_save();
                      }});
    // How finely the picture is drawn, as a share of the resolution: 20 to 200 % in steps of 20, 100 as it comes (the user, 2026-11-14;
    // a slider for an hour the same day, then a row like the others at his word)
    res.rows.push_back({"Render Scale", "How finely the picture is drawn: 100% is the resolution itself.",
                      [] { return std::to_string(g_cfg.render_scale) + "%"; },
                      [](int d) {
                          const int at = std::max(1, std::min(10, (g_cfg.render_scale + 10) / 20));
                          g_cfg.render_scale = ((at - 1 + (d < 0 ? 9 : 1)) % 10 + 1) * 20;
                          pc_config_save();
                      }});
    // a row that opens a list: the graphics card that draws the game
    res.rows.push_back({"Select GPU", "Which graphics card draws the game. Changing it restarts the game.", nullptr,
                        [](int) { pc_open_page("PC GPU"); }});
    PcPage &q = pc_add_page("PC Advanced", "Advanced");
    // what the graphics card has, no more: a value it does not have would be shown and then not drawn
    // no value beside it: a row that opens a page, as Resolution and Advanced do, not one to step through
    q.rows.push_back({"Antialiasing", "Smoother edges: off, MSAA or SMAA.", nullptr, [](int) { pc_open_page("PC Antialiasing"); }});
    q.rows.push_back({"Particles", "Smoke, sparks and dust. Advanced: the same, with soft edges.",
                      [] { return std::string(g_cfg.particles ? "Advanced" : "Original"); },
                      [](int) {
                          g_cfg.particles = g_cfg.particles ? 0 : 1;
                          pc_config_save();
                      }});
    q.rows.push_back({"Anisotropic Filtering", "Sharper textures far away, on everything in the world.",
                      [] {
                          const int v = std::min(g_cfg.aniso, gpu_limit(1));
                          return v ? std::to_string(v) + "x" : std::string("Off");
                      },
                      [](int d) {
                          const int top = gpu_limit(1);
                          if (!top) pc_set_blurb("This graphics card has no anisotropic filtering.");
                          g_cfg.aniso = top ? pc_next_power(std::min(g_cfg.aniso, top), top, d) : 0;
                          pc_config_save();
                      }});

    q.rows.push_back({"Ambient Occlusion", "Soft shading where surfaces meet: under cars, by walls and kerbs.", [] { return onoff(g_cfg.ao); },
                      [](int) { g_cfg.ao ^= 1; pc_config_save(); }});
    // the picture's look: left and right step it, Select goes on up and round
    auto stepped = [](int &v, int d, int lo, int hi, int step) {
        int x = v + (d < 0 ? -step : step);
        v = x > hi ? (d < 0 ? hi : lo) : x < lo ? (d < 0 ? hi : lo) : x;
        pc_config_save();
    };
    q.rows.push_back({"Brightness", "Lighter or darker, all over.",
                      [] { return (g_cfg.brightness > 0 ? "+" : "") + std::to_string(g_cfg.brightness); },
                      [stepped](int d) { stepped(g_cfg.brightness, d, -50, 50, 5); }});
    q.rows.push_back({"Contrast", "The difference between light and dark.",
                      [] { return std::to_string(g_cfg.contrast) + "%"; },
                      [stepped](int d) { stepped(g_cfg.contrast, d, 50, 150, 5); }});
    q.rows.push_back({"Gamma", "Above 1.00 the dark parts get lighter.",
                      [] {
                          char s[16];
                          snprintf(s, sizeof s, "%d.%02d", g_cfg.gamma / 100, g_cfg.gamma % 100);
                          return std::string(s);
                      },
                      [stepped](int d) { stepped(g_cfg.gamma, d, 50, 200, 5); }});
    q.rows.push_back({"Defaults", "Brightness, contrast and gamma as they were.", nullptr, [](int) {
                          g_cfg.brightness = 0;
                          g_cfg.contrast = g_cfg.gamma = 100;
                          pc_config_save();
                          pc_set_blurb("The picture is back to the defaults.");
                          pc_refresh_page();
                      }});

    // Antialiasing: the way, and how much of it. MSAA draws the scene in samples (what the graphics card has of them); SMAA finds the
    // finished picture's edges and blends along them, 2x its HIGH preset and 4x its ULTRA.
    PcPage &aa = pc_add_page("PC Antialiasing", "Antialiasing");
    aa.rows.push_back({"Antialiasing", "Off, MSAA (the scene drawn in samples) or SMAA (the picture's edges smoothed).",
                       [] { return std::string(g_cfg.aa_mode == 1 ? "MSAA" : g_cfg.aa_mode == 2 ? "SMAA" : "Off"); },
                       [](int d) {
                           const bool msaa = gpu_limit(0) >= 2;
                           for (int i = 0; i < 3; i++) {
                               g_cfg.aa_mode = (g_cfg.aa_mode + (d < 0 ? 2 : 1)) % 3;
                               if (g_cfg.aa_mode != 1 || msaa) break;
                           }
                           if (g_cfg.aa_mode == 1 && gpu_limit(0) < g_cfg.aa_level) g_cfg.aa_level = 2;
                           pc_config_save();
                       }});
    {
        PcRow level{"Quality", "MSAA: samples a pixel. SMAA: how far it looks along an edge.",
                    [] { return std::to_string(g_cfg.aa_level) + "x"; },
                    [](int) {
                        g_cfg.aa_level = g_cfg.aa_level == 2 ? 4 : 2;
                        if (g_cfg.aa_mode == 1 && gpu_limit(0) < g_cfg.aa_level) {
                            g_cfg.aa_level = 2;
                            pc_set_blurb("This graphics card has no more samples for the game.");
                        }
                        pc_config_save();
                    }};
        level.disabled = [] { return g_cfg.aa_mode == 0; };
        aa.rows.push_back(level);
    }

    // Controls (the user, 2026-11-24): the keyboard; the gamepads, straight to the game's Controller screen, where a player's
    // Calibration row is that player's own page of the gamepad's settings (the pages "PC Pad 1" and "PC Pad 2" below; pcmenu.cpp,
    // HOOK_JoystickPreConfigHandleEvent); the steering wheel, a device beside the others, with its own page; what player 2
    // drives with; and the buttons the game shows, which are everybody's.
    PcPage &p = pc_add_page("PC Controls", "Controls");
    p.rows.push_back({"Keyboard", "The keys for driving, for both players.", nullptr,
                      [](int) { pc_open_page("PC Keyboard"); }});
    p.rows.push_back({"Gamepad", "The buttons for driving and the stick.", nullptr,
                      [](int) { pc_open_controller(false); }});
    // (the same screen as the wheels': each player's Settings, Select Device, Calibration and Force Feedback; pcmenu.cpp)
    p.rows.push_back({"Steering Wheel", "Force feedback and rotation of a steering wheel.", nullptr,
                      [](int) { pc_open_controller(true); }});

    // Player 2: a gamepad and a wheel, or the keyboard, chosen on this page itself (the user, 2026-11-23: "without opening a menu";
    // 2026-11-24: "Pad/Wheel: this opens the pad and the wheel at once"). Pad/Wheel: the gamepad player 2 has on the game's
    // Controller screen (Select Device; window.cpp, pad_slot) and a wheel that is theirs, and their keys after all if they have
    // neither. Keyboard: player 2's keys, the ones after the divider on the Keyboard page.
    p.rows.push_back({"Player 2 Controls", "What the second player drives with in a two-player race.",
                      [] { return std::string(g_cfg.player2_keyboard ? "Keyboard" : "Pad/Wheel"); },
                      [](int) {
                          g_cfg.player2_keyboard ^= 1;
                          pc_config_save();
                      }});
    p.rows.push_back({"Button Icons", "Which buttons the game shows, and the menus' layout that goes with them.",
                      [] { return std::string(g_cfg.pad_icons == 1 ? "Xbox" : g_cfg.pad_icons == 2 ? "PlayStation" : g_cfg.pad_icons == 3 ? "Nintendo" : "Auto"); },
                      [](int d) {
                          g_cfg.pad_icons = (g_cfg.pad_icons + (d < 0 ? 3 : 1)) % 4;
                          pc_config_save();
                      }});

    binding_page("PC Keyboard", "Keyboard");

    // A player's gamepad: what the Controller screen's Calibration row opens for it. Each player's own numbers (pcconfig.h).
    // The rows are made as the page opens, by the gamepad the player has then (the user, 2026-11-25: "I want to see the
    // Adaptive Triggers setting only if a DualSense is connected ... an Xbox pad: nothing"): what only a DualSense has is a
    // row only with a DualSense, and is no row at all with any other gamepad.
    for (int player = 0; player < 2; player++) {
        PcPage &g = pc_add_page(player ? "PC Pad 2" : "PC Pad 1", player ? "Gamepad: Player 2" : "Gamepad: Player 1");
        for (int i = 0; i < 8; i++) g.rows.push_back({"Gamepad", "", nullptr, nullptr});       // (the screen's slots: the rows come as it opens)
        g.on_open = [player](PcPage &g) {
            g.rows.clear();
            const auto number = [&g](const char *label, const char *blurb, int *value, int low, int top, int step) {
                g.rows.push_back({label, blurb, [value] { return std::to_string(*value) + "%"; },
                                  [value, low, top, step](int d) {
                                      const int x = *value + d * step;
                                      *value = x < low ? low : x > top ? top : x;
                                      pc_config_save();
                                  }});
            };
            number("Stick Dead Zone", "How far the left stick moves before the car steers.", &g_cfg.pad_dead[player], 0, 50, 2);
            number("Steering", "Above 100%, the car turns sharper for a small move of the stick.", &g_cfg.pad_steer[player], 50, 200, 10);
            number("Vibration Strength", "How strongly the gamepad shakes. 0% is off.", &g_cfg.pad_vibration[player], 0, 200, 10);
            g.rows.push_back({"Vibration Mode", "Original: as the game plays it. Responsive: it follows the car at once and does not linger.",
                              [player] { return std::string(g_cfg.pad_vibration_mode[player] ? "Responsive" : "Original"); },
                              [player](int) {
                                  g_cfg.pad_vibration_mode[player] ^= 1;
                                  pc_config_save();
                              }});
            number("Trigger Dead Zone", "How far a trigger moves before it counts.", &g_cfg.pad_trigger_dead[player], 0, 50, 2);
            if (window_pad_dualsense(player))
                g.rows.push_back({"Adaptive Triggers", "Both triggers kick at a gear change and stiffen when a tyre is blown.",
                                  [player] { return std::string(g_cfg.pad_adaptive[player] ? "On" : "Off"); },
                                  [player](int) {
                                      g_cfg.pad_adaptive[player] ^= 1;
                                      pc_config_save();
                                  }});
            // (a DualSense or a DualShock 4 on a cable; over Bluetooth, and with any other gamepad, no rows)
            if (window_pad_speaker(player)) {
                g.rows.push_back({"Police Radio Speaker", "The police radio plays from the gamepad's speaker.",
                                  [player] { return std::string(g_cfg.pad_radio[player] ? "On" : "Off"); },
                                  [player](int) {
                                      g_cfg.pad_radio[player] ^= 1;
                                      pc_config_save();
                                  }});
                number("Speaker Volume", "How loud the gamepad's speaker is.", &g_cfg.pad_radio_volume[player], 0, 100, 10);
            }
            g.rows.push_back({"Defaults", "Put this gamepad's settings back as they were.", nullptr, [player](int) {
                                  g_cfg.pad_dead[player] = 24, g_cfg.pad_steer[player] = 100;
                                  g_cfg.pad_vibration[player] = 100, g_cfg.pad_trigger_dead[player] = 0;
                                  g_cfg.pad_adaptive[player] = g_cfg.pad_vibration_mode[player] = 1;
                                  g_cfg.pad_radio[player] = 1, g_cfg.pad_radio_volume[player] = 100;
                                  pc_config_save();
                                  pc_set_blurb("The settings are back to the defaults.");
                                  pc_refresh_page();
                              }});
        };
    }

    // A gamepad layout of the player's own, each player's page of it (the user, 2026-11-25; the game's Configuration screen
    // has it as the last layout, "Custom", and Select there opens this page: pcmenu.cpp). A row is a thing of the car's and
    // the button that has it: Select, then the button on the gamepad. The steering is the left stick or two buttons (the two
    // rows are greyed with the stick), the pedals by how far a trigger is pressed or all at once. Start is Pause, always.
    for (int player = 0; player < 2; player++) {
        PcPage &l = pc_add_page(player ? "PC Layout 2" : "PC Layout 1", player ? "Custom Layout: Player 2" : "Custom Layout: Player 1");
        const auto button = [player](int action, const char *label) {
            PcRow r{label, "Select, then press the button for it. Esc cancels.",
                    [player, action] {
                        if (g_custom_binding == action && g_custom_player == player) return std::string("Press...");
                        const int b = g_cfg.custom[player][action];
                        return std::string(b == PC_CUSTOM_L3 ? pc_pad_name(0x40) : b >= 0 && b < PCB_COUNT ? pc_pad_name(g_cfg.pad[b]) : "None");
                    },
                    [player, action](int) {
                        g_custom_binding = action;
                        g_custom_player = player;
                        window_capture_begin(2);
                        pc_set_blurb("Press a button. Esc cancels.");
                    }};
            r.select_only = true;
            return r;
        };
        l.rows.push_back({"Steering", "Analog: the left stick. Digital: two buttons. Both: either.",
                          [player] { return std::string(g_cfg.custom_steer[player] == 2 ? "Both" : g_cfg.custom_steer[player] ? "Digital" : "Analog"); },
                          [player](int d) {
                              int *b = g_cfg.custom[player];
                              int before[PCC_COUNT];
                              memcpy(before, b, sizeof before);
                              g_cfg.custom_steer[player] = (g_cfg.custom_steer[player] + (d < 0 ? 2 : 1)) % 3;
                              if (g_cfg.custom_steer[player]) {
                                  // the D-pad's Left and Right, taken from what had them
                                  if (b[PCC_STEER_LEFT] < 0) custom_take(player, PCC_STEER_LEFT, PCB_LEFT);
                                  if (b[PCC_STEER_RIGHT] < 0) custom_take(player, PCC_STEER_RIGHT, PCB_RIGHT);
                              } else {
                                  b[PCC_STEER_LEFT] = b[PCC_STEER_RIGHT] = -1;    // their buttons are free for other things
                              }
                              pc_config_save();
                              pc_custom_changed(player);
                              custom_lost(player, before);
                          }});
        for (int side = 0; side < 2; side++) {
            PcRow r = button(side ? PCC_STEER_RIGHT : PCC_STEER_LEFT, side ? "Steer Right" : "Steer Left");
            r.disabled = [player] { return g_cfg.custom_steer[player] == 0; };
            l.rows.push_back(r);
        }
        // The throttle and the brake (the user, 2026-11-25: "Analog: let the player choose the right or the left trigger for
        // the throttle and the brake. Digital: any button, and the choice as for Analog must go"). Analog: the two rows are
        // a choice between the two triggers, one each (a change of either swaps them), and a trigger gives how far it is
        // pressed. Digital: the rows are bindings like the others, any button, all at once. The row of the choice stands
        // above them, as Steering does above its two. The handbrake is all or nothing always. The row was "Pedals" (the
        // user, 2026-11-25: "Accelerate/Brake").
        l.rows.push_back({"Accelerate/Brake", "Analog: a trigger, by how far it is pressed. Digital: any button, all at once.",
                          [player] { return std::string(g_cfg.custom_pedals_digital[player] ? "Digital" : "Analog"); },
                          [player](int) {
                              int before[PCC_COUNT];
                              memcpy(before, g_cfg.custom[player], sizeof before);
                              g_cfg.custom_pedals_digital[player] ^= 1;
                              if (!g_cfg.custom_pedals_digital[player]) custom_pedal_triggers(player);
                              pc_config_save();
                              pc_custom_changed(player);
                              custom_lost(player, before);
                          }});
        for (int brake = 0; brake < 2; brake++) {
            PcRow r = button(brake ? PCC_BRAKE : PCC_ACCEL, brake ? "Brake / Reverse" : "Accelerate");
            const std::function<void(int)> bind = r.change;
            const std::string bind_blurb = r.blurb;
            r.select_only = false;
            r.select_only_if = [player] { return g_cfg.custom_pedals_digital[player] != 0; };
            r.blurb_if = [player, bind_blurb] { return g_cfg.custom_pedals_digital[player] ? bind_blurb : std::string("Left, Right: which trigger it is."); };
            r.change = [player, bind](int d) {
                if (g_cfg.custom_pedals_digital[player]) {
                    bind(d);
                    return;
                }
                int *b = g_cfg.custom[player];
                const int other = b[PCC_ACCEL];     // one trigger each: the other pedal gets this one's
                b[PCC_ACCEL] = b[PCC_BRAKE];
                b[PCC_BRAKE] = other;
                pc_config_save();
                pc_custom_changed(player);
            };
            l.rows.push_back(r);
        }
        l.rows.push_back(button(PCC_HANDBRAKE, "Handbrake"));
        // The right stick (the user, 2026-11-25: "the right stick as the camera, as in modern racing games ... and the
        // player chooses: the throttle and the brake, the camera, or nothing"). It was "Stick Pedals", On / Off.
        l.rows.push_back({"Right Stick", "Camera: look around the car. Accelerate/Brake: up is the throttle, down the brake.",
                          [player] { return std::string(g_cfg.custom_right_stick[player] == 2 ? "Camera" : g_cfg.custom_right_stick[player] == 1 ? "Accelerate/Brake" : "None"); },
                          [player](int d) {
                              g_cfg.custom_right_stick[player] = (g_cfg.custom_right_stick[player] + (d < 0 ? 2 : 1)) % 3;
                              pc_config_save();
                              pc_custom_changed(player);
                          }});
        l.rows.push_back(button(PCC_BOOST, "Speed Boost"));
        l.rows.push_back(button(PCC_SHIFT_UP, "Shift Up"));
        l.rows.push_back(button(PCC_SHIFT_DOWN, "Shift Down"));
        l.rows.push_back(button(PCC_LOOK_BACK, "Look Back"));
        l.rows.push_back(button(PCC_VIEW, "Change View"));
        l.rows.push_back(button(PCC_HORN, "Horn / Siren"));
        l.rows.push_back(button(PCC_RESET, "Reset Car"));
        l.rows.push_back(button(PCC_HUD, "Cycle HUD"));
        // The zone cameras: the game's way (Change View held, then a gear's button) or a button each; the two rows are greyed
        // with the game's way. A button each: the D-pad's Left and Right, if nothing has them.
        l.rows.push_back({"Zone Cameras", "View + Shift: hold Change View, press Shift Down or Up. Own Buttons: a button each.",
                          [player] { return std::string(g_cfg.custom_zone_buttons[player] ? "Own Buttons" : "View + Shift"); },
                          [player](int) {
                              int *b = g_cfg.custom[player];
                              g_cfg.custom_zone_buttons[player] ^= 1;
                              if (g_cfg.custom_zone_buttons[player]) {
                                  const auto free = [b](int button) {
                                      for (int a = 0; a < PCC_COUNT; a++)
                                          if (b[a] == button) return false;
                                      return true;
                                  };
                                  if (b[PCC_ZONE_FREEZE] < 0 && free(PCB_LEFT)) b[PCC_ZONE_FREEZE] = PCB_LEFT;
                                  if (b[PCC_ZONE_ZOOM] < 0 && free(PCB_RIGHT)) b[PCC_ZONE_ZOOM] = PCB_RIGHT;
                              } else {
                                  b[PCC_ZONE_FREEZE] = b[PCC_ZONE_ZOOM] = -1;     // their buttons are free for other things
                              }
                              pc_config_save();
                              pc_custom_changed(player);
                          }});
        for (int zone = 0; zone < 2; zone++) {
            PcRow r = button(zone ? PCC_ZONE_ZOOM : PCC_ZONE_FREEZE, zone ? "Zone Zoom / Road Block" : "Zone Freeze / Helicopter");
            r.disabled = [player] { return !g_cfg.custom_zone_buttons[player]; };
            l.rows.push_back(r);
        }
        l.rows.push_back({"Reset to Default", "Put this layout back as it was.", nullptr, [player](int) {
                              g_cfg.default_custom(player);
                              pc_config_save();
                              pc_custom_changed(player);
                              pc_set_blurb("Back to the defaults.");
                              pc_refresh_page();
                          }});
    }

    // A steering wheel's own page, each player's (the user, 2026-11-25: "each wheel must have its own wheel settings ... in the
    // row Advanced of each player"; pcmenu.cpp opens them from the wheels' screen): the forces, each kind by its percent of
    // what the game asks for (wheel_win.cpp, apply), and how far the wheel turns for the game's full lock, set digit by digit.
    for (int player = 0; player < 2; player++) {
        PcPage &w = pc_add_page(player ? "PC Wheel 2" : "PC Wheel 1", player ? "Steering Wheel: Player 2" : "Steering Wheel: Player 1");
        const auto percent = [&w](const char *label, const char *blurb, int *value, int top) {
            w.rows.push_back({label, blurb, [value] { return std::to_string(*value) + "%"; },
                              [value, top](int d) {
                                  const int x = *value + d * 10;
                                  *value = x < 0 ? 0 : x > top ? top : x;
                                  pc_config_save();
                              }});
        };
        percent("Force Feedback", "The strength of all the wheel's forces. 0% is off.", &g_cfg.wheel_force[player], 200);
        percent("Centering Spring", "How strongly the wheel returns to its middle.", &g_cfg.wheel_spring[player], 100);
        percent("Damper", "How heavy the wheel is to turn.", &g_cfg.wheel_damper[player], 100);
        percent("Road Feel", "The road's surface in the wheel.", &g_cfg.wheel_road[player], 100);
        percent("Collisions", "The jolt of a hit.", &g_cfg.wheel_crash[player], 100);
        percent("Shake", "The wheel's shaking.", &g_cfg.wheel_shake[player], 100);
        {
            PcRow r{"Rotation", kRotationBlurb,
                    [player] { return g_typing && g_typing_player == player ? typing_shown() : std::to_string(g_cfg.wheel_lock[player]); },
                    [player](int) {
                        const int n = g_cfg.wheel_lock[player] < 0 ? 0 : g_cfg.wheel_lock[player] > 999 ? 999 : g_cfg.wheel_lock[player];
                        g_digit[0] = n / 100, g_digit[1] = n / 10 % 10, g_digit[2] = n % 10;
                        g_digit_at = 0;
                        g_typing = true;
                        g_typing_player = player;
                        pc_row_edit(typing_button);
                        pc_set_blurb("Left, Right: the digit. Up, Down: its value. Select takes it, Back cancels.");
                    }};
            r.select_only = true;
            w.rows.push_back(r);
        }
        w.rows.push_back({"Set Rotation", "On: a Logitech wheel is set to this rotation while the game runs.",
                          [player] { return std::string(g_cfg.wheel_set_range[player] ? "On" : "Off"); },
                          [player](int) {
                              g_cfg.wheel_set_range[player] ^= 1;
                              pc_config_save();
                              pc_wheel_lock_changed();
                          }});
        w.rows.push_back({"Invert Steering", "The wheel steers the other way.",
                          [player] { return std::string(g_cfg.wheel_invert[player] ? "On" : "Off"); },
                          [player](int) {
                              g_cfg.wheel_invert[player] ^= 1;
                              pc_config_save();
                          }});
        w.rows.push_back({"Defaults", "Put the wheel's settings back as they were.", nullptr, [player](int) {
                              g_cfg.wheel_force[player] = 100;
                              g_cfg.wheel_spring[player] = g_cfg.wheel_damper[player] = g_cfg.wheel_road[player] = 100;
                              g_cfg.wheel_crash[player] = g_cfg.wheel_shake[player] = 100;
                              g_cfg.wheel_lock[player] = 360, g_cfg.wheel_set_range[player] = 1, g_cfg.wheel_invert[player] = 0;
                              pc_config_save();
                              pc_wheel_lock_changed();
                              pc_set_blurb("The settings are back to the defaults.");
                              pc_refresh_page();
                          }});
    }
}
