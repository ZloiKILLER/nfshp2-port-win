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
    if (w <= 0 || h <= 0) return 17778;
    // the resolution's own shape, width over height times 10000, from 16:10 up to 4:1 (21:9, 32:9 and all between); narrower: 4:3
    const double r = (double)w / h;
    if (r < 16.0 / 10.0 - 0.02) return 0;
    return (int)((r < 4.0 ? r : 4.0) * 10000.0 + 0.5);
}

int pc_aspect()
{
    return g_cfg.aspect ? pc_aspect_limit() : 0;
}

int g_pad_playstation;
int g_pad_nintendo;
int g_layout_playstation = 1;
int g_pad_driving_now = 0;

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
    default_custom(0);
    default_custom(1);
}

// The keys as they come (the user's table of 2026-11-25: "so that the two players' keys do not cross, and player 2's are nearer
// to their own"). No key is both players'. Player 2: W A S D and what lies around them.
void PcConfig::default_keys2()
{
    auto set = [&](int b, int k1, int k2) {
        key2[b][0] = k1;
        key2[b][1] = k2;
    };
    set(PCB_UP, '7', 0);                    // Cycle HUD
    set(PCB_DOWN, 0, 0);
    set(PCB_LEFT, 'A', 0);
    set(PCB_RIGHT, 'D', 0);
    set(PCB_CROSS, 'W', 0);                 // Accelerate
    set(PCB_SQUARE, 'S', 0);                // Brake / Reverse
    set(PCB_CIRCLE, 'G', 0);                // Horn / Siren
    set(PCB_TRIANGLE, 'Y', 0);              // Change View
    set(PCB_L1, 'T', 0);                    // Look Back
    set(PCB_R1, 'Q', 0);                    // Handbrake
    set(PCB_L2, '4', 0);                    // Shift Down
    set(PCB_R2, '5', 0);                    // Shift Up
    set(PCB_R3, 'F', 0);                    // Speed Boost
    set(PCB_START, 0, 0);
    set(PCB_SELECT, '6', 0);                // Reset Car
    memset(key2_zone, 0, sizeof key2_zone);
    key2_zone[0][0] = '8';                  // Zone Freeze; a cop's call for the helicopter
    key2_zone[1][0] = '9';                  // Zone Zoom; a cop's call for a road block
    // (no Zone Slow Motion: the slow motion is the first player's alone, and has no key here nor a row on the Keyboard page)
}

// Player 1: the arrows, and the left hand's keys. Driving, in the game's controller layout A: the buttons' jobs are in kActions
void PcConfig::default_keys()
{
    auto set = [&](int b, int k1, int k2) {
        key[b][0] = k1;
        key[b][1] = k2;
    };
    set(PCB_UP, 0x09, 0);                   // VK_TAB: Cycle HUD
    set(PCB_DOWN, 0, 0);
    set(PCB_LEFT, 0x25, 0);                 // VK_LEFT
    set(PCB_RIGHT, 0x27, 0);                // VK_RIGHT
    set(PCB_CROSS, 0x26, 0);                // VK_UP: Accelerate
    set(PCB_SQUARE, 0x28, 0);               // VK_DOWN: Brake / Reverse
    set(PCB_CIRCLE, 'H', 0);                // Horn / Siren
    set(PCB_TRIANGLE, 'C', 0);              // Change View
    set(PCB_L1, 'B', 0);                    // Look Back
    set(PCB_R1, 0x20, 0);                   // VK_SPACE: Handbrake
    set(PCB_L2, 'Z', 0);                    // Shift Down
    set(PCB_R2, 'X', 0);                    // Shift Up
    set(PCB_R3, 0xA0, 0);                   // VK_LSHIFT: Speed Boost
    set(PCB_START, 0, 0);                   // Esc pauses, always
    set(PCB_SELECT, 'R', 0);                // Reset Car
    key_zone[0][0] = '1', key_zone[0][1] = 0;       // Zone Freeze
    key_zone[1][0] = '2', key_zone[1][1] = 0;       // Zone Zoom
    key_zone[2][0] = '3', key_zone[2][1] = 0;       // Zone Slow Motion
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
    for (int p = 0; p < 2; p++) pad_dead[p] = 24, pad_steer[p] = 100, pad_vibration[p] = 100, pad_trigger_dead[p] = 0, pad_adaptive[p] = 1, pad_vibration_mode[p] = 1, pad_radio[p] = 1, pad_radio_volume[p] = 100;
}

// A player's own layout as it comes: the port's modern layout, I (the user, 2026-11-25: "take layout I as the default of
// layout J, so that people change the controls starting from its like"; it was the game's layout A until then). And layout I
// itself is made from this very function (pcplatform.cpp, custom::setup), so the two cannot come apart. What it is (the
// user's choice of the two offered: "as in Forza", the D-pad for the zones and the HUD): the left stick steers and nothing
// else does, the triggers are the pedals by how far they are pressed, the right stick is the camera, Cross the handbrake,
// Square and Circle the gears down and up, Triangle the view, L1 looks back, R1 the horn, the right stick's press the boost,
// Select puts the car back, the D-pad's Up the HUD and its Left and Right the zone cameras (a cop's helicopter and road
// block). By PcButton, so by the buttons the player's pad has for them.
void PcConfig::default_custom(int player)
{
    int *b = custom[player & 1];
    for (int a = 0; a < PCC_COUNT; a++) b[a] = -1;
    b[PCC_ACCEL] = PCB_R2, b[PCC_BRAKE] = PCB_L2, b[PCC_HANDBRAKE] = PCB_CROSS, b[PCC_BOOST] = PCB_R3;
    b[PCC_SHIFT_DOWN] = PCB_SQUARE, b[PCC_SHIFT_UP] = PCB_CIRCLE, b[PCC_VIEW] = PCB_TRIANGLE, b[PCC_LOOK_BACK] = PCB_L1;
    b[PCC_HORN] = PCB_R1, b[PCC_RESET] = PCB_SELECT, b[PCC_HUD] = PCB_UP;
    b[PCC_ZONE_FREEZE] = PCB_LEFT, b[PCC_ZONE_ZOOM] = PCB_RIGHT;
    custom_steer[player & 1] = 0;
    custom_pedals_digital[player & 1] = 0;
    custom_zone_buttons[player & 1] = 1;
    custom_right_stick[player & 1] = 2;
}

static int clampi(int v, int lo, int hi);
// hp2.ini's names of what a player's own layout gives buttons to
static const char *const kCustomKeys[PCC_COUNT] = {"steer_left", "steer_right", "accelerate", "brake", "handbrake", "boost", "shift_up",
                                                   "shift_down", "look_back", "view", "horn", "reset", "hud", "zone_freeze", "zone_zoom"};

// custom1_accelerate=4 and the like: the PcButton's number (-1 none; 15 the left stick's press; Start, 13, is Pause and is not taken)
static bool custom_key(const char *key, int n)
{
    if (strncmp(key, "custom", 6) || (key[6] != '1' && key[6] != '2') || key[7] != '_') return false;
    const int p = key[6] - '1';
    const char *what = key + 8;
    if (!strcmp(what, "steering")) return g_cfg.custom_steer[p] = clampi(n, 0, 2), true;
    if (!strcmp(what, "pedals")) return g_cfg.custom_pedals_digital[p] = n != 0, true;
    if (!strcmp(what, "right_stick")) return g_cfg.custom_right_stick[p] = clampi(n, 0, 2), true;
    if (!strcmp(what, "stick_pedals")) return true;     // (an older file's On / Off of the pedals on the right stick: right_stick now)
    if (!strcmp(what, "zones")) return g_cfg.custom_zone_buttons[p] = n != 0, true;
    for (int a = 0; a < PCC_COUNT; a++)
        if (!strcmp(what, kCustomKeys[a])) return g_cfg.custom[p][a] = n >= 0 && n <= PC_CUSTOM_L3 && n != PCB_START ? n : -1, true;
    return false;
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
    // A Nintendo pad (the user, 2026-11-25: "add Nintendo's buttons to the gamepad's detection; it must work as perfectly as
    // Xbox and PS do", and, of an exchange of confirm and back that a build had for it: "by no means, the logic of the
    // controls stays the same, only the matching buttons of the gamepad are to be shown"). So nothing but the letters: a
    // mask's bits are places (P_A the bottom button, P_B the right one, P_X the left, P_Y the top), every pad does the same
    // with the same place, and on a Nintendo pad those places are lettered B, A, Y and X.
    static const struct { int m; const char *n; } nin[] = {
        {P_A, "B"}, {P_B, "A"}, {P_X, "Y"}, {P_Y, "X"}, {P_LB, "L"}, {P_RB, "R"}, {P_LT, "ZL"}, {P_RT, "ZR"}, {P_BACK, "Minus"}, {P_START, "Plus"}};
    if (g_pad_nintendo && !g_pad_playstation)
        for (auto &n : nin)
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

// A wheel's own settings, each player's: wheel_force1, wheel_force2 and so on. The same name with no number is the file of
// before 2026-11-25, when both wheels had one: it sets both.
static bool wheel_key(const char *key, int n)
{
    const struct { const char *name; int *v; int lo, hi; } k[] = {
        {"wheel_lock", g_cfg.wheel_lock, 40, 2520},     {"wheel_set_range", g_cfg.wheel_set_range, 0, 1},
        {"wheel_force", g_cfg.wheel_force, 0, 200},     {"wheel_spring", g_cfg.wheel_spring, 0, 100},
        {"wheel_damper", g_cfg.wheel_damper, 0, 100},   {"wheel_road", g_cfg.wheel_road, 0, 100},
        {"wheel_crash", g_cfg.wheel_crash, 0, 100},     {"wheel_shake", g_cfg.wheel_shake, 0, 100},
        {"wheel_invert", g_cfg.wheel_invert, 0, 1}};
    for (const auto &e : k) {
        const size_t len = strlen(e.name);
        if (strncmp(key, e.name, len)) continue;
        const char *tail = key + len;
        if (!*tail) {
            e.v[0] = e.v[1] = clampi(n, e.lo, e.hi);
            return true;
        }
        if ((tail[0] == '1' || tail[0] == '2') && !tail[1]) {
            e.v[tail[0] - '1'] = clampi(n, e.lo, e.hi);
            return true;
        }
    }
    return false;
}

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
    fprintf(f, "actions=%d          ; 1 = a key is the game's action itself; 0 = the old way, a key presses a PS2 pad's button\n", g_cfg.key_actions);
    for (int b = 0; b < PCB_COUNT; b++) {
        std::string n = pc_button_name(b);
        for (char &ch : n) ch = (char)tolower((unsigned char)ch);
        fprintf(f, "key_%s=%d      ; %s | %s\n", n.c_str(), g_cfg.key[b][0], pc_button_action(b), pc_key_name(g_cfg.key[b][0]).c_str());
    }
    fprintf(f, "mouse_direct=%d     ; menus: 1 = the pointer takes the row under it at once; 0 = the old way, a press at a time\n", g_cfg.mouse_direct);
    fprintf(f, "key_zone_freeze=%d      ; Zone Freeze; a cop's call for the helicopter\n", g_cfg.key_zone[0][0]);
    fprintf(f, "key_zone_zoom=%d      ; Zone Zoom; a cop's call for a road block\n", g_cfg.key_zone[1][0]);
    fprintf(f, "key_zone_slomo=%d      ; Zone Slow Motion (with actions=1 only: no pad has a button for it)\n", g_cfg.key_zone[2][0]);
    fprintf(f, "\n[player2]\n");
    fprintf(f, "player2_keyboard=%d ; what player 2 drives with: 0 = a gamepad (the second; the only one if there is one), 1 = the keyboard (the key2_ keys)\n", g_cfg.player2_keyboard);
    for (int b = 0; b < PCB_COUNT; b++) {
        std::string n = pc_button_name(b);
        for (char &ch : n) ch = (char)tolower((unsigned char)ch);
        fprintf(f, "key2_%s=%d      ; %s | %s\n", n.c_str(), g_cfg.key2[b][0], pc_button_action(b), pc_key_name(g_cfg.key2[b][0]).c_str());
    }
    fprintf(f, "key2_zone_freeze=%d\n", g_cfg.key2_zone[0][0]);
    fprintf(f, "key2_zone_zoom=%d\n", g_cfg.key2_zone[1][0]);
    fprintf(f, "key2_zone_slomo=%d\n", g_cfg.key2_zone[2][0]);
    fprintf(f, "\n[gamepad]        ; its buttons are the DualShock 2's where the pad has them; what each does is the game's layout (A to D)\n");
    for (int p = 0; p < 2; p++) {           // each player's gamepad (the Controller screen's Calibration)
        fprintf(f, "deadzone%d=%d        ; player %d's gamepad: the left stick's dead zone, percent\n", p + 1, g_cfg.pad_dead[p], p + 1);
        fprintf(f, "steering%d=%d       ; its steering, percent: above 100 sharper near the center\n", p + 1, g_cfg.pad_steer[p]);
        fprintf(f, "vibration%d=%d      ; its vibration's strength, percent: 0 = none\n", p + 1, g_cfg.pad_vibration[p]);
        fprintf(f, "vibration_mode%d=%d ; 0 = as the game plays it, 1 = it follows the car at once and does not linger\n", p + 1, g_cfg.pad_vibration_mode[p]);
        fprintf(f, "trigger_deadzone%d=%d ; its triggers' dead zone, percent\n", p + 1, g_cfg.pad_trigger_dead[p]);
        fprintf(f, "radio_speaker%d=%d  ; 1 = the police radio plays from the gamepad's speaker (a DualSense or a DualShock 4 on a cable), 0 = from the speakers\n", p + 1, g_cfg.pad_radio[p]);
        fprintf(f, "radio_volume%d=%d   ; how loud that speaker is, percent\n", p + 1, g_cfg.pad_radio_volume[p]);
        fprintf(f, "adaptive_triggers%d=%d ; a DualSense's adaptive triggers: 1 = resistance under the throttle and the brake, 0 = none\n", p + 1, g_cfg.pad_adaptive[p]);
    }
    fprintf(f, "custom_defaults=2   ; (the port's own mark: which layout the two below started from)\n");
    for (int p = 0; p < 2; p++) {           // each player's own layout (the Configuration screen's last one, Custom)
        fprintf(f, "custom%d_steering=%d   ; player %d's own layout: 0 = the left stick steers, 1 = two buttons do, 2 = both\n", p + 1, g_cfg.custom_steer[p], p + 1);
        fprintf(f, "custom%d_pedals=%d     ; 0 = the throttle and the brake are the two triggers, by how far each is pressed; 1 = any button, all or nothing\n", p + 1, g_cfg.custom_pedals_digital[p]);
        fprintf(f, "custom%d_right_stick=%d ; the right stick: 0 = nothing, 1 = the pedals (up the throttle, down the brake), 2 = the camera, turned around the car\n", p + 1, g_cfg.custom_right_stick[p]);
        fprintf(f, "custom%d_zones=%d      ; the zone cameras: 0 = Change View held and a gear's button, as in the game's layouts; 1 = a button each\n", p + 1, g_cfg.custom_zone_buttons[p]);
        for (int a = 0; a < PCC_COUNT; a++) {
            const int b = g_cfg.custom[p][a];
            fprintf(f, "custom%d_%s=%d      ; %s\n", p + 1, kCustomKeys[a], b, b == PC_CUSTOM_L3 ? "L3" : b >= 0 && b < PCB_COUNT ? pc_button_name(b) : "-");
        }
    }
    fprintf(f, "gamepad1=%s          ; player 1's gamepad: nothing = automatic (the first one plugged in), none, or a gamepad's name\n", g_cfg.pad_device[0]);
    fprintf(f, "gamepad2=%s          ; player 2's: automatic is the next one plugged in\n", g_cfg.pad_device[1]);
    fprintf(f, "icons=%d            ; 0 by the pad, 1 Xbox, 2 PlayStation, 3 Nintendo\n", g_cfg.pad_icons);
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
    fprintf(f, "fullscreen=%d       ; 1 = borderless fullscreen (Alt+Enter)\n", g_cfg.fullscreen);
    fprintf(f, "window=%d,%d     ; the window's picture\n", g_cfg.win_w, g_cfg.win_h);
    fprintf(f, "fullscreen_size=%d,%d ; the picture's size in fullscreen, stretched to the monitor; 0,0 = the monitor's own\n", g_cfg.fs_w, g_cfg.fs_h);
    fprintf(f, "render_scale=%d   ; percent of the resolution the picture is drawn at: 20 to 200 in steps of 20\n", g_cfg.render_scale);
    fprintf(f, "aspect=%d           ; the race's picture: 0 = 4:3, 1 = as wide as the resolution (16:10, 16:9, 21:9, 32:9 ...)\n", g_cfg.aspect ? 1 : 0);
    fprintf(f, "aa_mode=%d          ; anti-aliasing: 0 = off, 1 = MSAA, 2 = SMAA\n", g_cfg.aa_mode);
    fprintf(f, "aa_level=%d         ; 2 or 4: MSAA samples, SMAA quality\n", g_cfg.aa_level);
    fprintf(f, "aniso=%d            ; anisotropic filtering: 0 = off, 2, 4, 8, 16\n", g_cfg.aniso);
    fprintf(f, "dithering=%d        ; dithering against colour banding: 0 = off, 1 = on\n", g_cfg.deband);
    fprintf(f, "ambient_occlusion=%d ; soft shading where surfaces meet, in a race: 0 = off, 1 = on\n", g_cfg.ao);
    fprintf(f, "ambient_occlusion_strength=%d ; how dark it is, percent (10 to 300)\n", g_cfg.ao_strength);
    fprintf(f, "side_mirrors=%d     ; 1 = with Reflections at Advanced or High the mirrors of your own car show the rear-view mirror's picture (one player, with the HUD's mirror on); 0 = never\n", g_cfg.side_mirrors);
    fprintf(f, "hud_smooth=%d       ; above 60 FPS: 1 = the HUD moves between the game's frames too, 0 = as the game draws it, 60 times a second\n", g_cfg.hud_smooth);
    fprintf(f, "hud_steady=%d       ; on a wide picture: 1 = a part of the HUD that stands still keeps its place whatever passes by it, 0 = every part placed anew in each frame, as it was\n", g_cfg.hud_steady);
    fprintf(f, "hud_anchors=%d      ; on a wide picture: 1 = each part of the HUD is put by the edge or the middle its place in the game's layout says, 0 = by what is drawn near it in each frame, as it was\n", g_cfg.hud_anchors);
    fprintf(f, "shadow_softness=%d  ; the shadows' soft edge at High and up, percent (25 to 200)\n", g_cfg.shadow_softness);
    fprintf(f, "monitor=%d          ; the screen the game is shown on: 0 = where the window is, 1 and up = that screen\n", g_cfg.monitor);
    fprintf(f, "; the graphics card that draws the game, by its name as Options, Graphics, Resolution, Select GPU lists it; empty: the game chooses\n");
    fprintf(f, "gpu=%s\n", g_cfg.gpu);
    fprintf(f, "brightness=%d       ; percent, -50 to 50\n", g_cfg.brightness);
    fprintf(f, "contrast=%d        ; percent, 50 to 150\n", g_cfg.contrast);
    fprintf(f, "gamma=%d           ; percent, 50 to 200: above 100 lighter\n", g_cfg.gamma);
    fprintf(f, "frame_rate=%d       ; the race's: 30, 60, or more (120, 144 ... 500; 1000 = unlimited: the game's 60 with pictures drawn between its frames)\n",
            pc_picture_rate());
    fprintf(f, "vsync=%d            ; 1 = the picture waits for the screen (no tearing), 0 = shown at once\n", g_cfg.vsync);
    fprintf(f, "draw_distance=%d   ; percent of the PS2's: 100 = original, 200 = advanced, 400 = high, 600 = maximum\n", g_cfg.draw_distance);
    fprintf(f, "profile=%s\n", g_cfg.last_profile);
    fprintf(f, "language=%s     ; ENGLISH, FRENCH, GERMAN, ITALIAN, SPANISH, SWEDISH, or one of data\\lang\\\n", g_cfg.language);
    fprintf(f, "show_fps=%d\n", g_cfg.show_fps);
    fprintf(f, "max_detail=%d       ; 1 = every object at its most detailed model, 0 = the PS2's levels of detail\n", g_cfg.max_detail);
    fprintf(f, "particles=%d        ; smoke, sparks, dust: 0 = original (drawn as the game draws them), 1 = advanced (the same particles with soft edges)\n", g_cfg.particles);
    fprintf(f, "shadows=%d          ; the cars' shadows: 0 = the PS2's, 1 = high (the body's outline, lying on what is under it, in the mirror and far off too), 2 = very high (finer, and the cars cast shadows from the tunnels' lamps; 80 MB of the card's memory), 3 = ultra (finer again; 320 MB)\n", g_cfg.shadows);
    fprintf(f, "reflections=%d      ; the cars' reflections: 0 = the PS2's, 1 = advanced (new every frame), 2 = high (new every frame, twice the resolution)\n", g_cfg.reflections);
    fprintf(f, "\n");
    fprintf(f, "log=%s             ; on = the game writes speed.log (4 MB at most: its first and its last 2 MB), off = it writes no log at all\n", g_cfg.log ? "on" : "off");
    fprintf(f, "[audio]\nvolume=%d           ; percent\n\n", g_cfg.volume);
    fprintf(f, "[extras]\ndebug_menu=%d       ; 1 = the developers' Debug item in the pause menu (Options > Extras > DEBUG.not.pretty)\n", g_cfg.debug_menu);
    fprintf(f, "traffic_cars=%d     ; 1 = the mod \"Playable traffic cars\" by 379Felipe (Options > Extras > Playable Traffic Cars; the game restarts when it is switched there)\n", g_cfg.traffic_cars);
    fprintf(f, "no_catchup=%d       ; 1 = no catch-up for the opponents in Quick Race and the career (Options > Extras > Disable Catchup)\n", g_cfg.no_catchup);
    fprintf(f, "cinematic_replays=%d ; 1 = a replay with its controls hidden has no black bars either (Options > Extras > Cinematic Replays)\n", g_cfg.cinematic_replays);
    fprintf(f, "slow_motion=%d      ; 1 = the Zone Slow Motion key works, and both sticks of a gamepad pressed together (Options > Extras > Slow Motion)\n", g_cfg.slow_motion);
    fprintf(f, "wheel=%d            ; 1 = a steering wheel Windows has is the game's wheel (its own layout, calibration and force feedback), 0 = wheels are left alone\n", g_cfg.wheel);
    fprintf(f, "wheel1=%s            ; player 1's wheel: nothing = automatic (the first one connected), none, or a wheel's name\n", g_cfg.wheel_device[0]);
    fprintf(f, "wheel2=%s            ; player 2's: automatic is the next one connected\n", g_cfg.wheel_device[1]);
    fprintf(f, "wheel_ffb1=%d       ; 1 = player 1's wheel takes the forces, 0 = it is left loose\n", g_cfg.wheel_ffb[0] ? 1 : 0);
    fprintf(f, "wheel_ffb2=%d       ; the same for player 2's\n", g_cfg.wheel_ffb[1] ? 1 : 0);
    fprintf(f, "wheel_range=%d    ; how far the wheel turns lock to lock, in degrees, as its software has it; a Logitech wheel with G HUB running says so itself, and this is for the others\n", g_cfg.wheel_range);
    for (int p = 0; p < 2; p++) {           // each player's wheel (the wheels' screen, Advanced)
        fprintf(f, "wheel_lock%d=%d    ; player %d's wheel: how many of the degrees it turns are the game's full lock: smaller steers quicker\n", p + 1, g_cfg.wheel_lock[p], p + 1);
        fprintf(f, "wheel_set_range%d=%d ; 1 = a Logitech wheel is set to turn that many degrees in all while the game runs (G HUB must allow games to do so), 0 = it is left as G HUB has it\n", p + 1, g_cfg.wheel_set_range[p]);
        fprintf(f, "wheel_force%d=%d   ; its force feedback, percent: 0 = none\n", p + 1, g_cfg.wheel_force[p]);
        fprintf(f, "wheel_spring%d=%d  ; the centring spring, percent of what the game asks for (0 to 100)\n", p + 1, g_cfg.wheel_spring[p]);
        fprintf(f, "wheel_damper%d=%d  ; the damper\n", p + 1, g_cfg.wheel_damper[p]);
        fprintf(f, "wheel_road%d=%d    ; the road's surface\n", p + 1, g_cfg.wheel_road[p]);
        fprintf(f, "wheel_crash%d=%d   ; a hit\n", p + 1, g_cfg.wheel_crash[p]);
        fprintf(f, "wheel_shake%d=%d   ; the shake\n", p + 1, g_cfg.wheel_shake[p]);
        fprintf(f, "wheel_invert%d=%d    ; 1 = it steers the other way\n", p + 1, g_cfg.wheel_invert[p]);
    }
    fprintf(f, "wheel_emulate=%d    ; 1 = with no wheel connected, a pretend one, to look at the game's wheel screens: Home and End turn it, Page Up is the throttle, Page Down the brake; 2 = a second one as well: 4 and 6 of the number pad, 8 and 5\n", g_cfg.wheel_emulate);
    fprintf(f, "wheel_shared=%d     ; 1 = a player's wheel and gamepad both drive, whichever is in use; 0 = a connected wheel has its player to itself\n", g_cfg.wheel_shared ? 1 : 0);
    fprintf(f, "wheel_gas=%d        ; the throttle's axis: 0 X, 1 Y, 2 Z, 3 Rx, 4 Ry, 5 Rz, 6 and 7 the sliders\n", g_cfg.wheel_gas);
    fprintf(f, "wheel_brake=%d      ; the brake's axis\n", g_cfg.wheel_brake);
    fprintf(f, "wheel_pedals_inverted=%d ; 1 = a pedal at rest is at the top of its axis (Logitech's pedals)\n", g_cfg.wheel_pedals_inverted);
    fprintf(f, "speed_boost=%d      ; 1 = the speed boost in every car and every mode (Options > Extras > Speed Boost for All Cars)\n", g_cfg.boost_all);
    fprintf(f, "no_intro=%d         ; 1 = the game starts without its opening films (Options > Extras > No Intro Movies)\n", g_cfg.no_intro);
    fprintf(f, "cheat_menu=%d       ; 1 = F8 in the menus opens the game's cheats (Options > Extras > Cheat Menu)\n", g_cfg.cheat_menu);
    fprintf(f, "cheats=");          // the entries of the game's table of codes that are on (recomp/docs/cheats.md has their numbers)
    for (int i = 0, n = 0; i < (int)sizeof g_cfg.cheats; i++)
        if (g_cfg.cheats[i]) fprintf(f, "%s%d", n++ ? "," : "", i);
    fprintf(f, "\n\n");
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
    int custom_defaults = 1;                // which layout the players' own started from, by the file (below, after the lines)
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
        if (!strcmp(key, "controls")) old_controls = n < 2;
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
        else if (!strcmp(key, "aspect")) g_cfg.aspect = clampi(n, 0, 2);
        else if (!strcmp(key, "aa")) {      // an older hp2.ini: multisampling, 0, 2, 4 or 8
            g_cfg.aa_mode = n >= 2;
            if (n >= 2) g_cfg.aa_level = n >= 4 ? 4 : 2;
        }
        else if (!strcmp(key, "aa_mode")) g_cfg.aa_mode = clampi(n, 0, 2);
        else if (!strcmp(key, "aa_level")) g_cfg.aa_level = n >= 4 ? 4 : 2;
        else if (!strcmp(key, "dithering")) g_cfg.deband = n != 0;
        else if (!strcmp(key, "ambient_occlusion")) g_cfg.ao = n != 0;     // (2 was High for a day)
        else if (!strcmp(key, "ambient_occlusion_strength")) g_cfg.ao_strength = clampi(n, 10, 300);
        else if (!strcmp(key, "hud_smooth")) g_cfg.hud_smooth = n != 0;
        else if (!strcmp(key, "hud_steady")) g_cfg.hud_steady = n != 0;
        else if (!strcmp(key, "hud_anchors")) g_cfg.hud_anchors = n != 0;
        else if (!strcmp(key, "side_mirrors")) g_cfg.side_mirrors = n != 0;
        else if (!strcmp(key, "shadow_softness")) g_cfg.shadow_softness = clampi(n, 25, 200);
        else if (!strcmp(key, "monitor")) g_cfg.monitor = clampi(n, 0, 16);
        else if (!strcmp(key, "gpu")) {
            snprintf(g_cfg.gpu, sizeof g_cfg.gpu, "%s", val);
            for (char *e = g_cfg.gpu + strlen(g_cfg.gpu); e > g_cfg.gpu && (unsigned char)e[-1] <= ' '; *--e = 0) {}
        }
        else if (!strcmp(key, "aniso")) g_cfg.aniso = n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 0;
        else if (!strcmp(key, "brightness")) g_cfg.brightness = clampi(n, -50, 50);
        else if (!strcmp(key, "contrast")) g_cfg.contrast = clampi(n, 50, 150);
        else if (!strcmp(key, "gamma")) g_cfg.gamma = clampi(n, 50, 200);
        else if (!strcmp(key, "frame_rate")) g_cfg.frame_rate = n >= 60 ? 60 : 30, g_cfg.pictures = n > 60 ? clampi(n, 61, 1000) : 0;
        else if (!strcmp(key, "vsync")) g_cfg.vsync = n != 0;
        else if (!strcmp(key, "draw_distance")) g_cfg.draw_distance = clampi(n, 100, 600);
        else if (!strcmp(key, "show_fps")) g_cfg.show_fps = n != 0;
        else if (!strcmp(key, "max_detail")) g_cfg.max_detail = n != 0;
        else if (!strcmp(key, "reflections")) g_cfg.reflections = clampi(n, 0, 2);
        else if (!strcmp(key, "shadows")) g_cfg.shadows = clampi(n, 0, 3);
        else if (!strcmp(key, "particles")) g_cfg.particles = clampi(n, 0, 1);      // (2 was High till 2026-11-17: Advanced now)
        else if (!strcmp(key, "log")) g_cfg.log = log_value(val);
        else if (!strcmp(key, "volume")) g_cfg.volume = clampi(n, 0, 100);
        else if (!strcmp(key, "debug_menu")) g_cfg.debug_menu = n != 0;
        else if (!strcmp(key, "cheat_menu")) g_cfg.cheat_menu = n != 0;
        else if (!strcmp(key, "no_intro")) g_cfg.no_intro = n != 0;
        else if (!strcmp(key, "speed_boost")) g_cfg.boost_all = n != 0;
        else if (!strcmp(key, "slow_motion")) g_cfg.slow_motion = n != 0;
        else if (!strcmp(key, "wheel")) g_cfg.wheel = n != 0;
        else if (!strcmp(key, "wheel1") || !strcmp(key, "wheel2")) {
            char *to = g_cfg.wheel_device[key[5] - '1'];
            snprintf(to, sizeof g_cfg.wheel_device[0], "%s", val);
            for (char *e = to + strlen(to); e > to && (unsigned char)e[-1] <= ' '; *--e = 0) {}
        }
        else if (!strcmp(key, "wheel_ffb1") || !strcmp(key, "wheel_ffb2")) g_cfg.wheel_ffb[key[9] - '1'] = n != 0;
        else if (!strcmp(key, "wheel_range")) g_cfg.wheel_range = clampi(n, 40, 2520);
        else if (wheel_key(key, n)) {}
        else if (!strcmp(key, "custom_defaults")) custom_defaults = n;
        else if (custom_key(key, n)) {}
        else if (!strcmp(key, "wheel_emulate")) g_cfg.wheel_emulate = clampi(n, 0, 2);
        else if (!strcmp(key, "wheel_shared")) g_cfg.wheel_shared = n != 0;
        else if (!strcmp(key, "wheel_gas")) g_cfg.wheel_gas = clampi(n, 0, 7);
        else if (!strcmp(key, "wheel_brake")) g_cfg.wheel_brake = clampi(n, 0, 7);
        else if (!strcmp(key, "wheel_pedals_inverted")) g_cfg.wheel_pedals_inverted = n != 0;
        else if (!strcmp(key, "no_catchup")) g_cfg.no_catchup = n != 0;
        else if (!strcmp(key, "cinematic_replays")) g_cfg.cinematic_replays = n != 0;
        else if (!strcmp(key, "traffic_cars")) g_cfg.traffic_cars = n != 0;
        else if (!strcmp(key, "cheats")) {
            memset(g_cfg.cheats, 0, sizeof g_cfg.cheats);
            for (const char *at = val; at && *at;) {
                const int i = atoi(at);
                if (i >= 0 && i < (int)sizeof g_cfg.cheats && (*at >= '0' && *at <= '9')) g_cfg.cheats[i] = 1;
                at = strchr(at, ',');
                if (at) at++;
            }
        }
        // (deadzone and steering with no number are the files of before 2026-11-24: one for both players)
        else if (!strcmp(key, "deadzone")) g_cfg.pad_dead[0] = g_cfg.pad_dead[1] = clampi(n, 0, 50);
        else if (!strcmp(key, "steering")) g_cfg.pad_steer[0] = g_cfg.pad_steer[1] = clampi(n, 50, 200);
        else if (!strcmp(key, "deadzone1") || !strcmp(key, "deadzone2")) g_cfg.pad_dead[key[8] - '1'] = clampi(n, 0, 50);
        else if (!strcmp(key, "steering1") || !strcmp(key, "steering2")) g_cfg.pad_steer[key[8] - '1'] = clampi(n, 50, 200);
        else if (!strcmp(key, "vibration1") || !strcmp(key, "vibration2")) g_cfg.pad_vibration[key[9] - '1'] = clampi(n, 0, 200);
        else if (!strcmp(key, "vibration_mode1") || !strcmp(key, "vibration_mode2")) g_cfg.pad_vibration_mode[key[14] - '1'] = n != 0;
        else if (!strcmp(key, "radio_speaker1") || !strcmp(key, "radio_speaker2")) g_cfg.pad_radio[key[13] - '1'] = n != 0;
        else if (!strcmp(key, "radio_volume1") || !strcmp(key, "radio_volume2")) g_cfg.pad_radio_volume[key[12] - '1'] = clampi(n, 0, 100);
        else if (!strcmp(key, "adaptive_triggers1") || !strcmp(key, "adaptive_triggers2")) g_cfg.pad_adaptive[key[17] - '1'] = n != 0;
        else if (!strcmp(key, "trigger_deadzone1") || !strcmp(key, "trigger_deadzone2")) g_cfg.pad_trigger_dead[key[16] - '1'] = clampi(n, 0, 50);
        else if (!strcmp(key, "icons")) g_cfg.pad_icons = clampi(n, 0, 3);
        else if (!strcmp(key, "player2_keyboard")) g_cfg.player2_keyboard = clampi(n, 0, 1);
        else if (!strcmp(key, "gamepad1") || !strcmp(key, "gamepad2")) {
            char *to = g_cfg.pad_device[key[7] - '1'];
            snprintf(to, sizeof g_cfg.pad_device[0], "%s", val);
            for (char *e = to + strlen(to); e > to && (unsigned char)e[-1] <= ' '; *--e = 0) {}
        }
        else if (!strcmp(key, "window")) sscanf(val, "%d,%d", &g_cfg.win_w, &g_cfg.win_h);
        else if (!strcmp(key, "fullscreen_size")) sscanf(val, "%d,%d", &g_cfg.fs_w, &g_cfg.fs_h);
        else if (!strcmp(key, "render_scale")) g_cfg.render_scale = clampi((n + 10) / 20 * 20, 20, 200);
        else if (!strcmp(key, "key_zone_freeze")) sscanf(val, "%d,%d", &g_cfg.key_zone[0][0], &g_cfg.key_zone[0][1]);
        else if (!strcmp(key, "key_zone_zoom")) sscanf(val, "%d,%d", &g_cfg.key_zone[1][0], &g_cfg.key_zone[1][1]);
        else if (!strcmp(key, "key2_zone_freeze")) sscanf(val, "%d,%d", &g_cfg.key2_zone[0][0], &g_cfg.key2_zone[0][1]);
        else if (!strcmp(key, "key2_zone_zoom")) sscanf(val, "%d,%d", &g_cfg.key2_zone[1][0], &g_cfg.key2_zone[1][1]);
        else if (!strcmp(key, "key_zone_slomo")) g_cfg.key_zone[2][0] = n;
        else if (!strcmp(key, "key2_zone_slomo")) g_cfg.key2_zone[2][0] = 0;       // (a file of before: player 2 has no slow motion)
        else if (!strcmp(key, "actions")) g_cfg.key_actions = n != 0;
        else if (!strcmp(key, "mouse_direct")) g_cfg.mouse_direct = n != 0;
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
    // A player's own layout as it came until 2026-11-25 was the game's layout A, and the file has it written out like any
    // other. One that still is exactly that was never the player's doing: it becomes the layout as it comes now, the modern
    // one (the user: "why is J not the same mapping as I? I wanted them the same by default"). Once: the file written after
    // this says custom_defaults=2, and a layout A the player then makes by hand stays.
    if (custom_defaults < 2)
        for (int p = 0; p < 2; p++) {
            static const int kWas[PCC_COUNT] = {PCB_LEFT, PCB_RIGHT, PCB_CROSS, PCB_SQUARE, PCB_R1, PCB_R3, PCB_R2, PCB_L2,
                                                PCB_L1, PCB_TRIANGLE, PCB_CIRCLE, PCB_SELECT, PCB_UP, -1, -1};
            if (!memcmp(g_cfg.custom[p], kWas, sizeof kWas) && g_cfg.custom_steer[p] == 2 && g_cfg.custom_pedals_digital[p] == 1 &&
                g_cfg.custom_zone_buttons[p] == 0)
                g_cfg.default_custom(p);
        }
    // A layout's analog pedals are the two triggers, one each (2026-11-25). A file of before that, or one written by hand, with
    // other buttons for them: all or nothing, which is what those buttons gave.
    for (int p = 0; p < 2; p++) {
        const int gas = g_cfg.custom[p][PCC_ACCEL], brake = g_cfg.custom[p][PCC_BRAKE];
        if (!((gas == PCB_R2 && brake == PCB_L2) || (gas == PCB_L2 && brake == PCB_R2))) g_cfg.custom_pedals_digital[p] = 1;
    }
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
