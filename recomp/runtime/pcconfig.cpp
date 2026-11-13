// hp2.ini: the port's settings (pcconfig.h).
#include "pcconfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

PcConfig g_cfg;

void window_view_size(int &w, int &h);      // hw.h

int pc_aspect_limit()
{
    int w = 0, h = 0;
    window_view_size(w, h);
    if (w <= 0 || h <= 0) return 2;
    const double r = (double)w / h;
    return r >= 16.0 / 9.0 - 0.02 ? 2 : r >= 16.0 / 10.0 - 0.02 ? 1 : 0;
}

int pc_aspect()
{
    const int limit = pc_aspect_limit();
    return g_cfg.aspect < limit ? g_cfg.aspect : limit;
}

int g_pad_playstation;
int g_layout_playstation = 1;
int g_pad_steering_now = 100;

// XInput masks (xinput.h); the triggers are made-up bits above the real ones
enum {
    P_UP = 0x1, P_DOWN = 0x2, P_LEFT = 0x4, P_RIGHT = 0x8, P_START = 0x10, P_BACK = 0x20, P_L3 = 0x40, P_R3 = 0x80,
    P_LB = 0x100, P_RB = 0x200, P_A = 0x1000, P_B = 0x2000, P_X = 0x4000, P_Y = 0x8000, P_LT = 0x10000, P_RT = 0x20000
};

PcConfig::PcConfig()
{
    default_keys();
    default_keys2();
    default_pad();
}

// the left side of the keyboard: nothing here is in default_keys, so that the two can share it
void PcConfig::default_keys2()
{
    auto set = [&](int b, int k1, int k2) {
        key2[b][0] = k1;
        key2[b][1] = k2;
    };
    set(PCB_UP, 'T', 0);
    set(PCB_DOWN, 0, 0);
    set(PCB_LEFT, 'A', 0);
    set(PCB_RIGHT, 'D', 0);
    set(PCB_CROSS, 'W', 0);
    set(PCB_SQUARE, 'S', 0);
    set(PCB_CIRCLE, 'G', 0);
    set(PCB_TRIANGLE, 'V', 0);
    set(PCB_L1, 'E', 0);
    set(PCB_R1, 'F', 0);
    set(PCB_L2, '1', 0);
    set(PCB_R2, '2', 0);
    set(PCB_R3, 0xA2, 0);                   // VK_LCONTROL
    set(PCB_START, 0, 0);
    set(PCB_SELECT, 'B', 0);
    memset(key2_zone, 0, sizeof key2_zone);
}

// driving, in the game's controller layout A: the buttons' jobs are in kActions
void PcConfig::default_keys()
{
    auto set = [&](int b, int k1, int k2) {
        key[b][0] = k1;
        key[b][1] = k2;
    };
    set(PCB_UP, 0x09, 0);                   // VK_TAB
    set(PCB_DOWN, 0, 0);
    set(PCB_LEFT, 0x25, 0);                 // VK_LEFT
    set(PCB_RIGHT, 0x27, 0);                // VK_RIGHT
    set(PCB_CROSS, 0x26, 0);                // VK_UP
    set(PCB_SQUARE, 0x28, 0);               // VK_DOWN
    set(PCB_CIRCLE, 'H', 0);
    set(PCB_TRIANGLE, 'C', 0);
    set(PCB_L1, 'Q', 0);
    set(PCB_R1, 0x20, 0);                   // VK_SPACE
    set(PCB_L2, 'Z', 0);
    set(PCB_R2, 'X', 0);
    set(PCB_R3, 0xA0, 0);                   // VK_LSHIFT
    set(PCB_START, 0, 0);                   // Esc pauses, always
    set(PCB_SELECT, 'R', 0);
    key_zone[0][0] = 'F', key_zone[0][1] = 0;
    key_zone[1][0] = 'G', key_zone[1][1] = 0;
}

// The DualShock 2's buttons where any pad has them: Cross is A, Square X, Circle B, Triangle Y, L1/R1 the bumpers, L2/R2 the triggers, R3 the right
// stick's press, Select Back. The game's own controller layout (Options > Controls > Gamepad Layout) says what each does; layout 1 is the PS2's default.
static const int kPadDefault[PCB_COUNT] = {P_UP, P_DOWN, P_LEFT, P_RIGHT, P_A, P_X, P_B, P_Y, P_LB, P_RB, P_LT, P_RT, P_R3, P_START, P_BACK};
// the port's own of before, which a file may still have
static const int kPadBefore[PCB_COUNT] = {P_UP, 0, P_LEFT, P_RIGHT, P_A | P_RT, P_X | P_LT, P_B, P_Y, P_LB, P_RB, P_L3, P_R3, P_DOWN, P_START, P_BACK};
static bool g_pad_layout_old;

void PcConfig::default_pad()
{
    memcpy(pad, kPadDefault, sizeof pad);
    pad_deadzone = 24;
    pad_steering = 100;
}

// the menus: the arrows, Enter and Esc (window_pad), a key for each of the game's other menu buttons
int pc_menu_key(int b)
{
    static const int kKey[PCB_COUNT] = {0x26, 0x28, 0x25, 0x27, 0x0D, 0x20, 0x70, PC_BACK_KEY,    // F1, help
                                        'Q', 'E', '1', '2', 0, 0x0D, 0x08};
    return kKey[b];
}

int pc_menu_pad(int b)
{
    // Circle (B) goes back and Triangle (Y) is the help, on every pad, as PC games have it. The game's own are Triangle back and Circle help.
    if (b == PCB_TRIANGLE) return P_B;
    if (b == PCB_CIRCLE) return P_Y;
    static const int kPad[PCB_COUNT] = {P_UP, P_DOWN, P_LEFT, P_RIGHT, P_A, P_X, 0, 0,
                                        P_LB, P_RB, P_LT, P_RT, 0, P_START, P_BACK};
    return kPad[b];
}

// The keyboard in two, on the screens where both players choose at once
int pc_menu_key_split(int player, int b)
{
    static const int kKey[2][PCB_COUNT] = {
        {0x26, 0x28, 0x25, 0x27, 0x0D, 0xA3, 0x70, PC_BACK_KEY, 0xBC, 0xBE, 0, 0, 0, 0x0D, 0x08},   // comma, period, Backspace
        {'W', 'S', 'A', 'D', 0x20, 'F', 0x71, 'C', 'Q', 'E', '1', '2', 0, 0x20, 'R'}};              // F2
    return kKey[player][b];
}

static const char *kButtons[PCB_COUNT] = {"Up", "Down", "Left", "Right", "Cross", "Square", "Circle", "Triangle",
                                          "L1", "R1", "L2", "R2", "R3", "Start", "Select"};
const char *pc_button_name(int b) { return b >= 0 && b < PCB_COUNT ? kButtons[b] : "?"; }

// what each does in the game's default controller layout (A)
static const char *kActions[PCB_COUNT] = {"Cycle HUD", "-", "Steer Left", "Steer Right", "Accelerate",
                                          "Brake / Reverse", "Horn / Siren", "Change View", "Look Back",
                                          "Handbrake", "Shift Down", "Shift Up", "Speed Boost",
                                          "Pause", "Reset Car"};
const char *pc_button_action(int b) { return b >= 0 && b < PCB_COUNT ? kActions[b] : "?"; }

std::string pc_key_name(int vk)
{
    if (!vk) return "-";
#ifdef _WIN32
    static const struct { int vk; const char *name; } special[] = {
        {0x08, "Backspace"}, {0x09, "Tab"}, {0x0D, "Enter"}, {0x10, "Shift"}, {0x11, "Ctrl"}, {0x12, "Alt"},
        {0x14, "Caps Lock"}, {0x1B, "Esc"}, {0x20, "Space"}, {0x21, "Page Up"}, {0x22, "Page Down"},
        {0x23, "End"}, {0x24, "Home"}, {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"}, {0x28, "Down"},
        {0x2D, "Insert"}, {0x2E, "Delete"}, {0xA0, "L Shift"}, {0xA1, "R Shift"}, {0xA2, "L Ctrl"},
        {0xA3, "R Ctrl"}, {0xA4, "L Alt"}, {0xA5, "R Alt"}};
    for (auto &s : special)
        if (s.vk == vk) return s.name;
    if (vk >= 0x60 && vk <= 0x69) return "Num " + std::to_string(vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F);
    char name[64] = {};
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextA((LONG)(sc << 16), name, sizeof name) > 0) return name;
#endif
    return "Key " + std::to_string(vk);
}

const char *pc_pad_name(int mask)
{
    static const struct { int m; const char *n; } names[] = {
        {P_A, "A"}, {P_B, "B"}, {P_X, "X"}, {P_Y, "Y"}, {P_LB, "LB"}, {P_RB, "RB"}, {P_LT, "LT"}, {P_RT, "RT"},
        {P_BACK, "Back"}, {P_START, "Start"}, {P_L3, "L Stick"}, {P_R3, "R Stick"}, {P_UP, "Pad Up"},
        {P_DOWN, "Pad Down"}, {P_LEFT, "Pad Left"}, {P_RIGHT, "Pad Right"}};
    static const struct { int m; const char *n; } ps[] = {
        {P_A, "Cross"}, {P_B, "Circle"}, {P_X, "Square"}, {P_Y, "Triangle"}, {P_LB, "L1"}, {P_RB, "R1"}, {P_LT, "L2"},
        {P_RT, "R2"}, {P_BACK, "Create"}, {P_START, "Options"}, {P_L3, "L3"}, {P_R3, "R3"}};
    if (g_pad_playstation)
        for (auto &n : ps)
            if (mask & n.m) return n.n;
    for (auto &n : names)
        if (mask & n.m) return n.n;
    return "-";
}

std::string pc_pad_names(int mask)
{
    std::string s;
    for (int bit = 1; bit <= 0x20000; bit <<= 1)
        if (mask & bit) s += (s.empty() ? "" : "/") + std::string(pc_pad_name(bit));
    return s.empty() ? "-" : s;
}

static std::string ini_path()
{
    if (const char *e = getenv("HP2_INI")) return e;
    std::string p = "hp2.ini";
#ifdef _WIN32
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        p = exe;
        p = p.substr(0, p.find_last_of("\\/") + 1) + "hp2.ini";
    }
#endif
    return p;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// log=on, off (or 1, 0, yes, no); anything else is on
static int log_value(const char *val)
{
    while (*val == ' ' || *val == '\t') val++;
    return !(!_strnicmp(val, "off", 3) || !_strnicmp(val, "no", 2) || *val == '0');
}

bool pc_config_log()
{
    FILE *f = fopen(ini_path().c_str(), "r");
    if (!f) return true;
    char line[256];
    bool on = true;
    while (fgets(line, sizeof line, f)) {
        if (char *c = strchr(line, ';')) *c = 0;
        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        if (!_strnicmp(key, "log", 3)) {
            const char *eq = key + 3;
            while (*eq == ' ' || *eq == '\t') eq++;
            if (*eq == '=') on = log_value(eq + 1) != 0;
        }
    }
    fclose(f);
    return on;
}

static std::string g_controls_path;

// the keyboard, player 2 and the gamepad: a section of hp2.ini, and the profile's own file
static void write_controls(FILE *f)
{
    fprintf(f, "[keyboard]       ; while driving: the virtual-key code of each action's key (0 = none)\n");
    fprintf(f, "controls=2\n");
    for (int b = 0; b < PCB_COUNT; b++) {
        std::string n = pc_button_name(b);
        for (char &ch : n) ch = (char)tolower((unsigned char)ch);
        fprintf(f, "key_%s=%d      ; %s | %s\n", n.c_str(), g_cfg.key[b][0], pc_button_action(b), pc_key_name(g_cfg.key[b][0]).c_str());
    }
    fprintf(f, "key_zone_freeze=%d      ; Zone Freeze\n", g_cfg.key_zone[0][0]);
    fprintf(f, "key_zone_zoom=%d      ; Zone Zoom\n", g_cfg.key_zone[1][0]);
    fprintf(f, "\n[player2]\n");
    fprintf(f, "player2=%d          ; 0 = the second gamepad, or half of the keyboard; 1 = the first gamepad, player 1 has the keyboard\n", g_cfg.player2);
    for (int b = 0; b < PCB_COUNT; b++) {
        std::string n = pc_button_name(b);
        for (char &ch : n) ch = (char)tolower((unsigned char)ch);
        fprintf(f, "key2_%s=%d      ; %s | %s\n", n.c_str(), g_cfg.key2[b][0], pc_button_action(b), pc_key_name(g_cfg.key2[b][0]).c_str());
    }
    fprintf(f, "key2_zone_freeze=%d\n", g_cfg.key2_zone[0][0]);
    fprintf(f, "key2_zone_zoom=%d\n", g_cfg.key2_zone[1][0]);
    fprintf(f, "\n[gamepad]        ; its buttons are the DualShock 2's where the pad has them; what each does is the game's layout (A to D)\n");
    fprintf(f, "deadzone=%d         ; the left stick, percent\n", g_cfg.pad_deadzone);
    fprintf(f, "steering=%d        ; percent, above 100 sharper near the center\n", g_cfg.pad_steering);
    fprintf(f, "icons=%d            ; 0 by the pad, 1 Xbox, 2 PlayStation\n", g_cfg.pad_icons);
}

// the keys of the file, in the order they are written
void pc_config_save()
{
    if (!g_controls_path.empty())
        if (FILE *c = fopen(g_controls_path.c_str(), "w")) {
            fprintf(c, "; This profile's controls.\n\n");
            write_controls(c);
            fclose(c);
        }
    FILE *f = fopen(ini_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "; NFS Hot Pursuit 2 (recompiled): settings. The game's Options > PC Settings changes them.\n");
    fprintf(f, "; Environment variables (HP2_NOSOUND ...) win over this file.\n\n");
    fprintf(f, "[video]\n");
    fprintf(f, "scale=%d            ; the picture, times the PS2's: 1 to 8\n", g_cfg.scale);
    fprintf(f, "fullscreen=%d       ; 1 = borderless fullscreen (Alt+Enter)\n", g_cfg.fullscreen);
    fprintf(f, "window=%d,%d     ; the window's picture\n", g_cfg.win_w, g_cfg.win_h);
    fprintf(f, "smooth=%d           ; 1 = smooth picture scaling, 0 = sharp\n", g_cfg.smooth);
    fprintf(f, "aspect=%d           ; the race's picture: 0 = 4:3, 1 = 16:10, 2 = 16:9\n", g_cfg.aspect);
    fprintf(f, "aa_mode=%d          ; anti-aliasing: 0 = off, 1 = MSAA, 2 = SMAA\n", g_cfg.aa_mode);
    fprintf(f, "aa_level=%d         ; 2 or 4: MSAA samples, SMAA quality\n", g_cfg.aa_level);
    fprintf(f, "aniso=%d            ; anisotropic filtering: 0 = off, 2, 4, 8, 16\n", g_cfg.aniso);
    fprintf(f, "brightness=%d       ; percent, -50 to 50\n", g_cfg.brightness);
    fprintf(f, "contrast=%d        ; percent, 50 to 150\n", g_cfg.contrast);
    fprintf(f, "gamma=%d           ; percent, 50 to 200: above 100 lighter\n", g_cfg.gamma);
    fprintf(f, "frame_rate=%d       ; the race's, 30 or 60\n", g_cfg.frame_rate);
    fprintf(f, "vsync=%d            ; 1 = the picture waits for the screen (no tearing), 0 = shown at once\n", g_cfg.vsync);
    fprintf(f, "draw_distance=%d   ; percent, 100 to 400: 100 is the PS2's\n", g_cfg.draw_distance);
    fprintf(f, "profile=%s\n", g_cfg.last_profile);
    fprintf(f, "language=%s     ; ENGLISH, FRENCH, GERMAN, ITALIAN, SPANISH, SWEDISH, or one of data\\lang\\\n", g_cfg.language);
    fprintf(f, "show_fps=%d\n", g_cfg.show_fps);
    fprintf(f, "max_detail=%d       ; 1 = every object at its most detailed model, 0 = the PS2's levels of detail\n", g_cfg.max_detail);
    fprintf(f, "reflections=%d      ; the cars' reflections: 0 = the PS2's, 1 = advanced (new every frame), 2 = high (new every frame, twice the resolution)\n", g_cfg.reflections);
    fprintf(f, "\n");
    fprintf(f, "log=%s             ; on = the game writes speed.log (4 MB at most: its first and its last 2 MB), off = it writes no log at all\n", g_cfg.log ? "on" : "off");
    fprintf(f, "[audio]\nvolume=%d           ; percent\n\n", g_cfg.volume);
    write_controls(f);
    fclose(f);
}

// a file of settings; false if it is not there. old_controls: it has no controls=2
static bool read_settings(const std::string &path, bool &old_controls)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[256];
    old_controls = true;
    g_pad_layout_old = true;
    while (fgets(line, sizeof line, f)) {
        if (char *c = strchr(line, ';')) *c = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); *--e = 0) {}
        while (*val == ' ' || *val == '\t') val++;
        int n = atoi(val);
        if (!strcmp(key, "scale")) g_cfg.scale = clampi(n, 1, 8);
        else if (!strcmp(key, "controls")) old_controls = n < 2;
        else if (!strcmp(key, "pad_layout")) g_pad_layout_old = n < 2;
        else if (!strcmp(key, "profile")) {
            snprintf(g_cfg.last_profile, sizeof g_cfg.last_profile, "%s", val);
            for (char *e = g_cfg.last_profile + strlen(g_cfg.last_profile); e > g_cfg.last_profile && (unsigned char)e[-1] <= ' '; *--e = 0) {}
        }
        else if (!strcmp(key, "language")) {
            snprintf(g_cfg.language, sizeof g_cfg.language, "%s", val);
            g_cfg.ask_language = false;
            for (char *e = g_cfg.language + strlen(g_cfg.language); e > g_cfg.language && (unsigned char)e[-1] <= ' '; *--e = 0) {}
            for (char *e = g_cfg.language; *e; e++) *e = (char)toupper((unsigned char)*e);
        }
        else if (!strcmp(key, "fullscreen")) g_cfg.fullscreen = n != 0;
        else if (!strcmp(key, "smooth")) g_cfg.smooth = n != 0;
        else if (!strcmp(key, "aspect")) g_cfg.aspect = clampi(n, 0, 2);
        else if (!strcmp(key, "aa")) {      // an older hp2.ini: multisampling, 0, 2, 4 or 8
            g_cfg.aa_mode = n >= 2;
            if (n >= 2) g_cfg.aa_level = n >= 4 ? 4 : 2;
        }
        else if (!strcmp(key, "aa_mode")) g_cfg.aa_mode = clampi(n, 0, 2);
        else if (!strcmp(key, "aa_level")) g_cfg.aa_level = n >= 4 ? 4 : 2;
        else if (!strcmp(key, "aniso")) g_cfg.aniso = n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 0;
        else if (!strcmp(key, "brightness")) g_cfg.brightness = clampi(n, -50, 50);
        else if (!strcmp(key, "contrast")) g_cfg.contrast = clampi(n, 50, 150);
        else if (!strcmp(key, "gamma")) g_cfg.gamma = clampi(n, 50, 200);
        else if (!strcmp(key, "frame_rate")) g_cfg.frame_rate = n == 60 ? 60 : 30;
        else if (!strcmp(key, "vsync")) g_cfg.vsync = n != 0;
        else if (!strcmp(key, "draw_distance")) g_cfg.draw_distance = clampi(n, 100, 400);
        else if (!strcmp(key, "show_fps")) g_cfg.show_fps = n != 0;
        else if (!strcmp(key, "max_detail")) g_cfg.max_detail = n != 0;
        else if (!strcmp(key, "reflections")) g_cfg.reflections = clampi(n, 0, 2);
        else if (!strcmp(key, "log")) g_cfg.log = log_value(val);
        else if (!strcmp(key, "volume")) g_cfg.volume = clampi(n, 0, 100);
        else if (!strcmp(key, "deadzone")) g_cfg.pad_deadzone = clampi(n, 0, 50);
        else if (!strcmp(key, "steering")) g_cfg.pad_steering = clampi(n, 50, 200);
        else if (!strcmp(key, "icons")) g_cfg.pad_icons = clampi(n, 0, 2);
        else if (!strcmp(key, "player2")) g_cfg.player2 = clampi(n, 0, 1);
        else if (!strcmp(key, "window")) sscanf(val, "%d,%d", &g_cfg.win_w, &g_cfg.win_h);
        else if (!strcmp(key, "key_zone_freeze")) sscanf(val, "%d,%d", &g_cfg.key_zone[0][0], &g_cfg.key_zone[0][1]);
        else if (!strcmp(key, "key_zone_zoom")) sscanf(val, "%d,%d", &g_cfg.key_zone[1][0], &g_cfg.key_zone[1][1]);
        else if (!strcmp(key, "key2_zone_freeze")) sscanf(val, "%d,%d", &g_cfg.key2_zone[0][0], &g_cfg.key2_zone[0][1]);
        else if (!strcmp(key, "key2_zone_zoom")) sscanf(val, "%d,%d", &g_cfg.key2_zone[1][0], &g_cfg.key2_zone[1][1]);
        else
            for (int b = 0; b < PCB_COUNT; b++) {
                std::string name = pc_button_name(b);
                for (char &ch : name) ch = (char)tolower((unsigned char)ch);
                if ("key_" + name == key) sscanf(val, "%d,%d", &g_cfg.key[b][0], &g_cfg.key[b][1]);
                if ("key2_" + name == key) sscanf(val, "%d,%d", &g_cfg.key2[b][0], &g_cfg.key2[b][1]);
            }
    }
    fclose(f);
    // one key an action: the second of an older file's pair goes (if it was the only one, it is the key)
    for (auto *keys : {g_cfg.key, g_cfg.key2})
        for (int b = 0; b < PCB_COUNT; b++) {
            if (!keys[b][0]) keys[b][0] = keys[b][1];
            keys[b][1] = 0;
        }
    for (auto *keys : {g_cfg.key_zone, g_cfg.key2_zone})
        for (int z = 0; z < 2; z++) {
            if (!keys[z][0]) keys[z][0] = keys[z][1];
            keys[z][1] = 0;
        }
    // the gamepad's buttons are not the port's to move any more: the game's layouts say what each does
    memcpy(g_cfg.pad, kPadDefault, sizeof g_cfg.pad);
    return true;
}

// a key is one player's, whatever the file says: player 1 keeps it
static bool one_player_a_key()
{
    bool twice = false;
    for (auto &p2 : g_cfg.key2)
        for (int &k : p2)
            for (auto &p1 : g_cfg.key)
                if (k && (k == p1[0] || k == p1[1])) {
                    k = 0;
                    twice = true;
                }
    return twice;
}

void pc_config_use_profile(const std::string &controls_path, bool fresh)
{
    g_controls_path = controls_path;
    bool old = false;
    if (!read_settings(controls_path, old) && fresh) {
        g_cfg.default_keys();
        g_cfg.default_keys2();
        g_cfg.default_pad();
    }
    one_player_a_key();
    pc_config_save();
}

void pc_config_init()
{
    std::string path = ini_path();
    bool old_controls = true;
    if (!read_settings(path, old_controls)) {
        pc_config_save();
        return;
    }
    if (old_controls) {
        // A file from before player 1 had the arrows alone and player 2 the left side: the old defaults go, what was changed by hand stays.
        static const struct { int b, arrow, letter; } kMove[] = {{PCB_LEFT, 0x25, 'A'}, {PCB_RIGHT, 0x27, 'D'}, {PCB_CROSS, 0x26, 'W'}, {PCB_SQUARE, 0x28, 'S'}};
        PcConfig fresh;
        bool p2_old = true;
        for (auto &m : kMove) {
            if (g_cfg.key[m.b][0] == m.arrow && g_cfg.key[m.b][1] == m.letter) g_cfg.key[m.b][1] = 0;
            if (g_cfg.key2[m.b][0] != m.arrow || g_cfg.key2[m.b][1]) p2_old = false;
        }
        if (p2_old) memcpy(g_cfg.key2, fresh.key2, sizeof g_cfg.key2);
        pc_config_save();
    }
    if (one_player_a_key()) pc_config_save();
}
