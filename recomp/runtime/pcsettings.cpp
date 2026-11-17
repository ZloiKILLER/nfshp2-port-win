// The port's settings as pages of the game's menus (pcmenu.h): Options > PC Settings, and
// PC Controls for the keyboard and gamepad bindings. Values live in g_cfg (hp2.ini).
#include <atomic>
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
static const int kKeyRows = PCB_COUNT + 2;
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

static void poll_capture()
{
    if (g_binding < 0) return;
    int code = 0, what = window_capture_poll(code);
    if (!what) return;
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

// The keyboard's keys, by action. Esc pauses, always.
static void binding_page(const char *name, const char *title, bool p2 = false)
{
    PcPage &p = pc_add_page(name, title);
    // (a key that a cop has for something else says so: the horn's key is the next suspect, the boost is the cop's alone, and the
    // zone cameras' two keys are the cop's calls for the helicopter and for a road block: Player::SetInputMode gives a cop the
    // events 128 and 129 on the buttons a racer has the zones on)
    auto add = [&p, p2](int row, const char *action, const char *help = "Select, then press the key for it. Esc cancels.") {
        PcRow r{action, help,
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
    p.rows.push_back({"Reset to Default", "Put these keys back as they were.", nullptr, [p2](int) {
                          if (p2) g_cfg.default_keys2();
                          else g_cfg.default_keys();
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

// the main menu is up: the profile's load is behind
void pc_profile_loaded()
{
    if (g_profile.empty() || g_fred_ready) return;
    snprintf(g_exit_save, sizeof g_exit_save, "%s/save.dat", mc_profile_dir(g_profile).c_str());
    g_fred_ready = true;
}

// the window's thread, as the window goes
void pc_save_on_exit()
{
    if (!g_fred_ready || pc_racing()) return;
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
    if (old == image) return;
    FILE *f = fopen(part.c_str(), "wb");
    if (!f) return;
    const bool whole = fwrite(image.data(), 1, kSaveSize, f) == kSaveSize;
    fclose(f);
    std::error_code ec;
    if (whole) std::filesystem::rename(part, file, ec);     // over the old one, at once
    if (!whole || ec) std::filesystem::remove(part, ec);
    fprintf(stderr, "profiles: the save is written as the game is left (%s)%c", whole && !ec ? "done" : "failed", 10);
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
    const bool pressed = window_delete_pressed();       // asked every frame: a press is one press
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

// ---- Graphics, Controls -----------------------------------------------------------------------
// ---- Extras > Cheat Menu ---------------------------------------------------------------------
// The game's cheats are button codes typed on the main menu (EasterEggs::JoyHandler, 0x24f748): a table of 78 entries of 56 bytes
// (gEasterEggs 0x31d7e8: +12 the table, +16 how many; an entry: eight buttons, +32 its kind (0 an easter egg, 1 a car, 2 a cop car
// for You're The Cop, 3 a track, 4 a track the other way), +36 which one, +44 the LanguageID of its name, +48 "entered"). A code that
// matches sets +48, and the car lists, the track lists and the rest ask the table (EasterEggs::IsEasterEggUnlocked, 0x24f9a0).
// Nothing of it is saved: a PS2 forgets them when it is switched off.
// With Cheat Menu on (Options > Extras), F8 on any menu before a race opens these pages: Effects, Cars, Cop Cars, Tracks, each
// with a row for all of its kind and a row for each entry, On or Off. On is the entry's +48 set, as its code sets it; what is on
// is kept in hp2.ini (cheats=) and set again every frame of the menus, so it holds from one run to the next. With Cheat Menu off
// nothing is set and F8 does nothing; the list in hp2.ini stays for when it is on again.
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
static void cheat_counts(int section, int &on, int &all)
{
    on = all = 0;
    for (int i = 0; i < cheat_count(); i++)
        if (cheat_section(i) == section) all++, on += g_cfg.cheats[i] != 0;
}
static const char *const kCheatPages[4] = {"PC Cheats Effects", "PC Cheats Cars", "PC Cheats Cop Cars", "PC Cheats Tracks"};

static void cheat_rows(PcPage &pg, int section)
{
    pg.rows.clear();
    PcRow every{"All", "Every one in this list, on or off.",
                [section] {
                    int on, all;
                    cheat_counts(section, on, all);
                    return std::string(on == 0 ? "Off" : on == all ? "On" : "Some");
                },
                [section](int) {
                    int on, all;
                    cheat_counts(section, on, all);
                    for (int i = 0; i < cheat_count(); i++)
                        if (cheat_section(i) == section) cheat_set(i, on != all);
                    pc_config_save();
                }};
    pg.rows.push_back(every);
    for (int i = 0; i < cheat_count(); i++) {
        if (cheat_section(i) != section) continue;
        pg.rows.push_back({cheat_name(i), "As if its code had been entered.", [i] { return onoff(g_cfg.cheats[i]); },
                           [i](int) {
                               cheat_set(i, !g_cfg.cheats[i]);
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
    pc_on_frame(profiles_frame);

    // the languages: Select switches the game at once (pclang.h)
    PcPage &langs = pc_add_page("PC Languages", "Languages");
    for (size_t i = 0; i < pc_languages().size(); i++) {
        PcRow r = {pc_languages()[i].name, "The game's texts and speech in this language.",
                   [i] { return std::string((int)i == pc_language_index() ? "Selected" : ""); },
                   [i](int) {
                       if ((int)i == pc_language_index()) return;
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
    v.rows.push_back({"Level of Detail", "Maximum: scenery and cars always at their most detailed models. Original: as on the PS2.",
                      [] { return std::string(g_cfg.max_detail ? "Maximum" : "Original"); },
                      [](int) {
                          g_cfg.max_detail ^= 1;
                          pc_config_save();
                      }});
    v.rows.push_back({"Reflections", "The reflections on the cars. Advanced: new every frame. High: new every frame, at twice the resolution.",
                      [] { return std::string(g_cfg.reflections == 2 ? "High" : g_cfg.reflections ? "Advanced" : "PS2"); },
                      [](int d) {
                          g_cfg.reflections = (g_cfg.reflections + (d < 0 ? 2 : 1)) % 3;
                          pc_config_save();
                      }});
    v.rows.push_back({"Shadows", "The cars' shadows. High: the body's own outline, lying on whatever is under it. Maximum: finer still.",
                      [] { return std::string(g_cfg.shadows >= 3 ? "Ultra" : g_cfg.shadows == 2 ? "Maximum" : g_cfg.shadows ? "High" : "PS2"); },
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
                window_relaunch(g_profile);
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
    // the screen the game is shown on: where the window is (it is not moved), or one of the screens by its number
    // (a page of its own for an hour on 2026-11-17, a list as the graphics cards'; back to this row at the user's word, till the
    // work on several screens at once)
    res.rows.push_back({"Monitor", "The screen the game is shown on.",
                      [] {
                          const int n = window_monitor_count();
                          if (g_cfg.monitor < 1 || g_cfg.monitor > n) return std::string("Automatic");
                          int w, h;
                          window_monitor_size(g_cfg.monitor, w, h);
                          return std::to_string(g_cfg.monitor) + ": " + std::to_string(w) + "x" + std::to_string(h);
                      },
                      [](int d) {
                          const int n = window_monitor_count();
                          int at = g_cfg.monitor < 1 || g_cfg.monitor > n ? 0 : g_cfg.monitor;
                          at = (at + (d < 0 ? n : 1)) % (n + 1);
                          window_set_monitor(at);
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
    q.rows.push_back({"Dithering", "Smooth gradients without colour steps.", [] { return onoff(g_cfg.deband); },
                      [](int) { g_cfg.deband ^= 1; pc_config_save(); }});
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

    PcPage &p = pc_add_page("PC Controls", "Controls");
    p.rows.push_back({"Keyboard", "The keys for driving. In menus: arrows, Enter, Esc.", nullptr,
                      [](int) { pc_open_page("PC Keyboard"); }});
    p.rows.push_back({"Gamepad", "The buttons for driving and the stick.", nullptr,
                      [](int) { pc_open_page("PC Gamepad"); }});

    p.rows.push_back({"Player 2", "What the second player drives with in a two-player race.", nullptr,
                      [](int) { pc_open_page("PC Player 2"); }});

    binding_page("PC Keyboard", "Keyboard");

    PcPage &two = pc_add_page("PC Player 2", "Player 2");
    two.rows.push_back({"Gamepad", "Second: the second gamepad or half the keyboard. First: the first one.",
                        [] { return std::string(g_cfg.player2 ? "First" : "Second"); },
                        [](int) {
                            g_cfg.player2 ^= 1;
                            pc_config_save();
                        }});
    two.rows.push_back({"Keyboard", "Player 2's keys, when the keyboard is shared.", nullptr,
                        [](int) { pc_open_page("PC Keyboard P2"); }});
    binding_page("PC Keyboard P2", "Player 2 Keyboard", true);

    PcPage &g = pc_add_page("PC Gamepad", "Gamepad");
    g.rows.push_back({"Layouts", "The gamepad's layouts, A to D: what each button does in a race.", nullptr,
                      [](int) { pc_open_page("Joystick PreConfig"); }});
    g.rows.push_back({"Stick Dead Zone", "How far the left stick moves before the car steers.",
                      [] { return std::to_string(g_cfg.pad_deadzone) + "%"; },
                      [](int d) {
                          int x = g_cfg.pad_deadzone + d * 2;
                          g_cfg.pad_deadzone = x < 0 ? 0 : x > 50 ? 50 : x;
                          pc_config_save();
                      }});
    g.rows.push_back({"Steering", "Above 100%, the car turns sharper for a small move of the stick.",
                      [] { return std::to_string(g_cfg.pad_steering) + "%"; },
                      [](int d) {
                          int x = g_cfg.pad_steering + d * 10;
                          g_cfg.pad_steering = x < 50 ? 50 : x > 200 ? 200 : x;
                          pc_config_save();
                      }});
    g.rows.push_back({"Button Icons", "Which buttons the game shows, and the menus' layout that goes with them.",
                      [] { return std::string(g_cfg.pad_icons == 1 ? "Xbox" : g_cfg.pad_icons == 2 ? "PlayStation" : "Auto"); },
                      [](int d) {
                          g_cfg.pad_icons = (g_cfg.pad_icons + (d < 0 ? 2 : 1)) % 3;
                          pc_config_save();
                      }});
    g.rows.push_back({"Defaults", "Put the gamepad back as it was.", nullptr, [](int) {
                          g_cfg.default_pad();
                          pc_config_save();
                          pc_set_blurb("The gamepad is back to the defaults.");
                          pc_refresh_page();
                      }});
}
