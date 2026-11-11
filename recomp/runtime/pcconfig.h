// The port's settings, hp2.ini next to the exe (pcconfig.cpp). Created with defaults on the
// first run; the game's menus (pcsettings.cpp) change them and save. Environment variables
// the port already has (HP2_GSBACKEND, HP2_GSSCALE, HP2_NOSOUND ...) win over the file.
#pragma once
#include <string>

// The PS2 buttons, in this order (the pad word's bit order is in window.cpp). key and pad say which
// keys and gamepad buttons give them while driving; the menus have fixed ones (pc_menu_key, pc_menu_pad).
enum PcButton {
    PCB_UP, PCB_DOWN, PCB_LEFT, PCB_RIGHT, PCB_CROSS, PCB_SQUARE, PCB_CIRCLE, PCB_TRIANGLE,
    PCB_L1, PCB_R1, PCB_L2, PCB_R2, PCB_R3, PCB_START, PCB_SELECT, PCB_COUNT
};

struct PcConfig {
    int scale = 2;              // the picture: this many times the PS2's 512 x 448, 1 to 8
    int fullscreen = 0;         // borderless fullscreen
    int smooth = 1;             // the picture is scaled smooth (else sharp)
    int aspect = 0;             // the race's picture: 0 4:3, 1 16:10, 2 16:9. The menus stay 4:3
    int aa_mode = 0;            // anti-aliasing: 0 off, 1 MSAA (the scene in samples), 2 SMAA (the finished picture's edges)
    int aa_level = 4;           // 2 or 4: MSAA's samples, SMAA's quality
    int msaa() const { return aa_mode == 1 ? aa_level : 0; }
    int aniso = 0;              // anisotropic filtering: 0 off (the GS's own sampling), 2, 4, 8, 16
    int brightness = 0;         // percent added, -50..50
    int contrast = 100;         // percent, 50..150
    int gamma = 100;            // percent, 50..200: 100 changes nothing, above it lighter
    int frame_rate = 30;        // the race's, 30 or 60; the menus have 60
    int vsync = 1;              // the picture waits for the screen's refresh (no tearing); 0: shown at once
    int draw_distance = 200;    // percent, 100..400: 100 is the PS2's
    char last_profile[32] = {}; // listed first on the profiles' screen
    int show_fps = 0;
    char language[16] = "ENGLISH";  // a key of pc_languages() (pclang.h)
    bool ask_language = true;   // hp2.ini has no language yet: the very first start shows the game's own choice of language (pclang.cpp)
    int max_detail = 1;         // 1: scenery, cars and tyres at their best models whatever the size; 0: the game's choice by size
    int volume = 100;           // percent
    int key[PCB_COUNT][2] = {}; // virtual-key codes, 0 = none
    int key2[PCB_COUNT][2] = {};    // player 2's, when the keyboard is shared in two (window_pad)
    int player2 = 0;            // 0: the second gamepad, else half of the keyboard. 1: the first gamepad, player 1 has the keyboard
    int pad[PCB_COUNT] = {};    // XInput button masks, 0 = none
    int pad_deadzone = 24;      // the left stick's dead zone, percent
    int pad_steering = 100;     // steering sensitivity, percent: above 100 more of it near the center
    int pad_vibration = 1;
    int pad_icons = 0;          // 0 by the pad, 1 Xbox, 2 PlayStation: its menu layout goes with them
    int win_w = 960, win_h = 720;   // the window's picture, in pixels
    PcConfig();
    void default_keys();
    void default_keys2();
    void default_pad();
};

extern PcConfig g_cfg;
extern int g_pad_playstation;   // the pad in use has PlayStation's buttons (window_pad keeps it)
extern int g_pad_steering_now;  // the sensitivity now: pad_steering while driving, else 100
// The race's aspect as it is drawn: the setting's, but no wider than the window (fullscreen: the screen), where a wider picture would only
// get bars above and below. pc_aspect_limit is the widest that fits.
int pc_aspect();
int pc_aspect_limit();
void pc_config_init();          // read hp2.ini (HP2_INI=<path> names another file); the main program only
void pc_config_save();
// the profile's file of controls: its values, or with none the defaults (fresh) or what is set now; each save writes it
void pc_config_use_profile(const std::string &controls_path, bool fresh);
const char *pc_button_name(int b);          // "Cross", the name in hp2.ini
const char *pc_button_action(int b);        // "Accelerate", what it does while driving
std::string pc_key_name(int vk);            // "Space", "A"
const char *pc_pad_name(int mask);          // "A", "LB", "Cross": the first button of a mask
std::string pc_pad_names(int mask);         // "A/RT": all of them
int pc_menu_key(int b);                     // the fixed virtual key of a button in the menus, 0: none
int pc_menu_pad(int b);                     // and the XInput mask
int pc_menu_key_split(int player, int b);   // that key on a screen where two players choose at once
const int PC_BACK_KEY = 0x1B;               // VK_ESCAPE: Triangle in the menus, Start in a race
