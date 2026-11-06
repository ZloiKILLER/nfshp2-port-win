// The port's settings as pages of the game's menus (pcmenu.h): Options > PC Settings, and
// PC Controls for the keyboard and gamepad bindings. Values live in g_cfg (hp2.ini).
#include <cstdio>
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

// the key goes to the button; a button that had it gets the key the button had (no key twice), the other player's too: in a race on one keyboard a key is one player's
static void bind_key(int (*keys)[2], int (*other)[2], int b, int vk)
{
    int old = keys[b][0];
    for (int o = 0; o < PCB_COUNT; o++)
        for (int s = 0; s < 2; s++) {
            if (o != b && keys[o][s] == vk) keys[o][s] = old;
            if (other[o][s] == vk) other[o][s] = old;
        }
    if (keys[b][1] == vk) keys[b][1] = 0;
    keys[b][0] = vk;
}

static void bind_pad(int b, int mask)
{
    int old = g_cfg.pad[b];
    for (int o = 0; o < PCB_COUNT; o++)
        if (o != b && (g_cfg.pad[o] & mask)) g_cfg.pad[o] = (g_cfg.pad[o] & ~mask) | old;
    g_cfg.pad[b] = mask;
}

static void poll_capture()
{
    if (g_binding < 0) return;
    int code = 0, what = window_capture_poll(code);
    if (!what) return;
    if (what == 1) bind_key(g_binding_p2 ? g_cfg.key2 : g_cfg.key, g_binding_p2 ? g_cfg.key : g_cfg.key2, g_binding, code);
    else if (what == 2) bind_pad(g_binding, code);
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

static void binding_page(const char *name, const char *title, bool pad, bool p2 = false)
{
    PcPage &p = pc_add_page(name, title);
    for (int b : kDriving) {
        if (!pad && b == PCB_START) continue;       // the keyboard pauses with Esc, always
        PcRow row{pc_button_action(b),
                  pad ? "Select, then press the gamepad button. Esc cancels." : "Select, then press the key. Esc cancels.",
                  [b, pad, p2] {
                      if (g_binding == b && g_binding_p2 == p2) return std::string("Press...");
                      return pad ? pc_pad_names(g_cfg.pad[b]) : key_text(p2 ? g_cfg.key2 : g_cfg.key, b);
                  },
                  [b, pad, p2](int) {
                      g_binding = b;
                      g_binding_p2 = p2;
                      window_capture_begin(pad ? 2 : 1);
                      pc_set_blurb(pad ? "Press a gamepad button. Esc cancels." : "Press a key. Esc cancels.");
                  }};
        row.select_only = true;
        p.rows.push_back(row);
    }
    p.rows.push_back({"Reset to Default", pad ? "Put the gamepad's buttons back as they were." : "Put these keys back as they were.", nullptr, [pad, p2](int) {
                          if (pad) {
                              PcConfig fresh;
                              memcpy(g_cfg.pad, fresh.pad, sizeof g_cfg.pad);
                          } else if (p2) {
                              g_cfg.default_keys2();
                          } else {
                              g_cfg.default_keys();
                          }
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
static std::string g_profile;           // the one in use; empty until one is chosen
static bool g_profiles_off, g_open_profiles, g_back_to_load, g_rebuild, g_typing, g_deleting;
static std::string g_typed, g_armed;

static void use_profile(const std::string &name)
{
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

// "Profile 2": for someone who has only a gamepad to name it with
static std::string free_name()
{
    const auto names = mc_profiles();
    for (int i = (int)names.size() + 1;; i++) {
        std::string n = "Profile " + std::to_string(i);
        if (std::find(names.begin(), names.end(), n) == names.end()) return n;
    }
}

static void choose_profile(const std::string &name)
{
    if (g_profile.empty()) {
        use_profile(name);
        g_back_to_load = true;
    } else if (name != g_profile) {
        pc_config_save();
        window_relaunch(name);
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
    choose_profile(name);
}

static void profile_rows(PcPage &pg)
{
    pg.rows.clear();
    std::vector<std::string> names = mc_profiles();
    // the one played last comes first
    auto last = std::find(names.begin(), names.end(), std::string(g_cfg.last_profile));
    if (last != names.end()) std::rotate(names.begin(), last, last + 1);
    pg.hide_back = g_profile.empty();
    for (const std::string &n : names) {
        PcRow r{n, g_deleting ? "Select it twice to delete this profile, its save and its controls." : "Play as this profile.",
                [n] {
                    if (g_armed == n) return std::string("Delete?");
                    return n == g_profile ? std::string("Current") : std::string();
                },
                [n](int) {
                    if (!g_deleting) {
                        if (n == g_profile) pc_set_blurb("This is the profile in use.");
                        else choose_profile(n);
                        return;
                    }
                    if (n == g_profile) {
                        pc_set_blurb("The profile in use cannot be deleted.");
                    } else if (g_armed != n) {
                        g_armed = n;
                        pc_set_blurb("Select it again to delete it for good.");
                        pc_refresh_page();
                    } else {
                        g_armed.clear();
                        mc_profile_delete(n);
                        g_deleting = false;
                        g_rebuild = true;
                    }
                }};
        r.select_only = true;
        pg.rows.push_back(r);
    }
    if (g_deleting) {
        pg.rows.push_back({"Cancel", "Delete nothing.", nullptr, [](int) {
                               g_deleting = false;
                               g_armed.clear();
                               g_rebuild = true;
                           }});
        return;
    }
    PcRow add{"New Profile", "Select, then type its name. With a gamepad it gets a number.",
              [] { return g_typing ? g_typed + pc_text_cursor(0xa9bfda04) : std::string(); },      // the font of the pages' values
              [](int) {
                  if (window_input_kind() == 1) {
                      make_profile(free_name());
                      return;
                  }
                  g_typing = true;
                  g_typed.clear();
                  window_text_begin();
                  pc_set_blurb("Type the name. Enter makes it, Esc leaves it.");
                  pc_refresh_page();
              }};
    add.select_only = true;
    pg.rows.push_back(add);
    if (!names.empty() && !g_profile.empty())
        pg.rows.push_back({"Delete Profile", "Choose one to delete.", nullptr, [](int) {
                               g_deleting = true;
                               g_armed.clear();
                               g_rebuild = true;
                           }});
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
    if (g_typing) {
        bool changed = false;
        while (const int ch = window_text_next()) {
            changed = true;
            if (ch == 13 || ch == 27) {
                g_typing = false;
                window_text_end();
                if (ch == 13) make_profile(g_typed);
                else pc_set_blurb("Select, then type its name. With a gamepad it gets a number.");
                break;
            }
            if (ch == 8) {
                if (!g_typed.empty()) g_typed.pop_back();
            } else if (g_typed.size() < 15 && (ch != ' ' || !g_typed.empty())) {
                g_typed += (char)ch;
            }
        }
        if (changed || pc_text_cursor_changed()) pc_refresh_page();
    }
    if (g_rebuild) {
        g_rebuild = false;
        for (PcPage *pg : {pc_find_page(kProfilesPage)})
            if (pg) {
                profile_rows(*pg);
                pg->top = 0;
            }
        pc_reshape_page();
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

    PcPage &profiles = pc_add_page(kProfilesPage, "Profiles");
    for (int i = 0; i < 8; i++) profiles.rows.push_back({"Profile", "", nullptr, nullptr});
    profiles.on_open = profile_rows;
    pc_profiles_page(kProfilesPage);

    PcPage &v = pc_add_page("PC Graphics", "Graphics");
    v.rows.push_back({"Resolution", "The window, the screen's resolution and the render scale.", nullptr,
                      [](int) { pc_open_page("PC Resolution"); }});
    v.rows.push_back({"Frame Rate", "The race's frame rate. The PS2's was 30.",
                      [] { return std::to_string(g_cfg.frame_rate) + " FPS"; },
                      [](int) {
                          g_cfg.frame_rate = g_cfg.frame_rate == 60 ? 30 : 60;
                          pc_config_save();
                      }});
    v.rows.push_back({"Draw Distance", "How far away scenery still shows. 100% is the PS2's.",
                      [] { return std::to_string(g_cfg.draw_distance) + "%"; },
                      [](int d) {
                          static const int kSteps[] = {100, 150, 200, 300, 400};
                          int at = 2;
                          for (int i = 0; i < 5; i++)
                              if (kSteps[i] == g_cfg.draw_distance) at = i;
                          g_cfg.draw_distance = kSteps[(at + (d < 0 ? 4 : 1)) % 5];
                          pc_config_save();
                      }});
    v.rows.push_back({"Level of Detail", "Maximum: scenery and cars always at their most detailed models. Original: as on the PS2.",
                      [] { return std::string(g_cfg.max_detail ? "Maximum" : "Original"); },
                      [](int) {
                          g_cfg.max_detail ^= 1;
                          pc_config_save();
                      }});
    v.rows.push_back({"Show FPS", "Frames per second in the corner of the picture.",
                      [] { return onoff(g_cfg.show_fps); }, [](int) { g_cfg.show_fps ^= 1; pc_config_save(); }});
    v.rows.push_back({"Advanced", "Anti-aliasing, texture filtering, brightness, contrast and gamma.", nullptr,
                      [](int) { pc_open_page("PC Advanced"); }});

    PcPage &res = pc_add_page("PC Resolution", "Resolution");
    res.rows.push_back({"Window", "Window or borderless fullscreen. Alt+Enter switches too.",
                      [] { return std::string(g_cfg.fullscreen ? "Fullscreen" : "Windowed"); },
                      [](int) { window_set_fullscreen(!g_cfg.fullscreen); }});
    res.rows.push_back({"Resolution", "The size of the window. Fullscreen fills the monitor.",
                      [] { return std::to_string(g_cfg.win_w) + "x" + std::to_string(g_cfg.win_h); },
                      [](int d) {
                          static const int kSizes[][2] = {{800, 600}, {960, 720}, {1024, 768}, {1280, 720}, {1280, 800}, {1280, 960},
                                                          {1366, 768}, {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080},
                                                          {1920, 1200}, {2560, 1440}, {2560, 1600}, {3840, 2160}};
                          int n = (int)(sizeof kSizes / sizeof kSizes[0]), at = 1;
                          for (int i = 0; i < n; i++)
                              if (kSizes[i][0] == g_cfg.win_w && kSizes[i][1] == g_cfg.win_h) at = i;
                          at = (at + (d < 0 ? n - 1 : 1)) % n;
                          window_set_size(kSizes[at][0], kSizes[at][1]);
                      }});
    res.rows.push_back({"Aspect Ratio", "The race's picture; the menus stay 4:3.",
                      [] { return std::string(g_cfg.aspect == 2 ? "16:9" : g_cfg.aspect == 1 ? "16:10" : "4:3"); },
                      [](int d) { g_cfg.aspect = (g_cfg.aspect + (d < 0 ? 2 : 1)) % 3; pc_config_save(); }});
    res.rows.push_back({"Render Scale", "The picture's detail, times the PS2's 512 x 448.",
                        [] { return std::to_string(g_cfg.scale) + "x"; },
                        [](int d) { g_cfg.scale = (g_cfg.scale - 1 + (d < 0 ? 7 : 1)) % 8 + 1; pc_config_save(); }});
    res.rows.push_back({"Scaling", "How the picture is scaled to the window.",
                      [] { return std::string(g_cfg.smooth ? "Smooth" : "Sharp"); },
                      [](int) { g_cfg.smooth ^= 1; pc_config_save(); }});
    PcPage &q = pc_add_page("PC Advanced", "Advanced");
    q.rows.push_back({"Anti-Aliasing", "Smoother edges.",
                      [] { return g_cfg.aa ? std::to_string(g_cfg.aa) + "x MSAA" : std::string("Off"); },
                      [](int d) { g_cfg.aa = pc_next_power(g_cfg.aa, 8, d); pc_config_save(); }});
    q.rows.push_back({"Anisotropic Filtering", "Sharper textures far away.",
                      [] { return g_cfg.aniso ? std::to_string(g_cfg.aniso) + "x" : std::string("Off"); },
                      [](int d) { g_cfg.aniso = pc_next_power(g_cfg.aniso, 16, d); pc_config_save(); }});

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

    PcPage &p = pc_add_page("PC Controls", "Controls");
    p.rows.push_back({"Keyboard", "The keys for driving. In menus: arrows, Enter, Esc.", nullptr,
                      [](int) { pc_open_page("PC Keyboard"); }});
    p.rows.push_back({"Gamepad", "The buttons for driving, the stick and vibration.", nullptr,
                      [](int) { pc_open_page("PC Gamepad"); }});

    p.rows.push_back({"Player 2", "What the second player drives with in a two-player race.", nullptr,
                      [](int) { pc_open_page("PC Player 2"); }});

    binding_page("PC Keyboard", "Keyboard", false);

    PcPage &two = pc_add_page("PC Player 2", "Player 2");
    two.rows.push_back({"Gamepad", "Second: the second gamepad or half the keyboard. First: the first one.",
                        [] { return std::string(g_cfg.player2 ? "First" : "Second"); },
                        [](int) {
                            g_cfg.player2 ^= 1;
                            pc_config_save();
                        }});
    two.rows.push_back({"Keyboard", "Player 2's keys, when the keyboard is shared.", nullptr,
                        [](int) { pc_open_page("PC Keyboard P2"); }});
    binding_page("PC Keyboard P2", "Player 2 Keyboard", false, true);

    PcPage &g = pc_add_page("PC Gamepad", "Gamepad");
    g.rows.push_back({"Layouts", "The game's own layouts: what each of the PS2's buttons does in a race.", nullptr,
                      [](int) { pc_open_page("Joystick PreConfig"); }});
    g.rows.push_back({"Buttons", "Which of the pad's buttons is which of the PS2's. In menus: A, B back, Y help.", nullptr,
                      [](int) { pc_open_page("PC Gamepad Buttons"); }});
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
    g.rows.push_back({"Vibration", "Vibration of the gamepad.",
                      [] { return onoff(g_cfg.pad_vibration); },
                      [](int) {
                          g_cfg.pad_vibration ^= 1;
                          if (!g_cfg.pad_vibration) window_rumble(0, 0);
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
    binding_page("PC Gamepad Buttons", "Gamepad Buttons", true);
}
