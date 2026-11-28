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
// The keyboard's driving keys as actions (pcactions.cpp): an action of a button's name is what that button does in the game's
// layout A (pc_button_action); after them the three zone cameras' keys, and Esc and Enter where the race has a use for them.
enum PcAction { PCA_ZONE_FREEZE = PCB_COUNT, PCA_ZONE_ZOOM, PCA_ZONE_SLOMO, PCA_PAUSE, PCA_SKIP, PCA_COUNT };
// What a button can be given in a gamepad layout of the player's own (the game's Configuration screen, "Custom": pcplatform.cpp).
// Pause is not among them: it is Start, always.
enum PcCustom {
    PCC_STEER_LEFT, PCC_STEER_RIGHT, PCC_ACCEL, PCC_BRAKE, PCC_HANDBRAKE, PCC_BOOST, PCC_SHIFT_UP, PCC_SHIFT_DOWN,
    PCC_LOOK_BACK, PCC_VIEW, PCC_HORN, PCC_RESET, PCC_HUD, PCC_ZONE_FREEZE, PCC_ZONE_ZOOM, PCC_COUNT
};
// (one button more than the PcButtons there: the left stick's press, which is nothing in the game's own layouts on a PC pad)
const int PC_CUSTOM_L3 = PCB_COUNT;

struct PcConfig {
    int fullscreen = 0;         // borderless fullscreen
    int aspect = 0;             // the race's picture: 0 4:3, else as wide as the resolution (16:10 or 16:9: pc_aspect). The menus stay 4:3
    int fs_w = 0, fs_h = 0;     // fullscreen: the picture's size, stretched to the monitor; 0: the monitor's own
    int render_scale = 100;     // percent of the resolution the picture is drawn at, 20 to 200 in steps of 20
    int aa_mode = 0;            // anti-aliasing: 0 off, 1 MSAA (the scene in samples), 2 SMAA (the finished picture's edges)
    int aa_level = 4;           // 2 or 4: MSAA's samples, SMAA's quality
    int msaa() const { return aa_mode == 1 ? aa_level : 0; }
    int aniso = 0;              // anisotropic filtering: 0 off (the GS's own sampling), 2, 4, 8, 16
    // The port's own effects on a race's 3D picture; the PS2 had none of them, so off as they come
    // Ambient Occlusion in a race (native.cpp, ao_end): 0 off, 1 on (it reaches 7 m). Off / Low / High for a day; the user,
    // 2026-11-26: "back to On / Off, the current High our only one"
    int ao = 0;
    int ao_strength = 100;      // how dark, percent (hp2.ini only)
    // The side mirrors of the player's own car show a part of the rear-view mirror's picture (native.cpp, mirror_glass_now;
    // the user, 2026-11-24: "for the fun of it"). One player, and only while the HUD's mirror is on: it is that picture.
    // It goes by Reflections: never at the PS2's, on at the others (the user, the same day); this is hp2.ini's own switch
    // over that, for turning it off alone.
    int side_mirrors = 1;
    int hud_smooth = 1;         // above 60 pictures a second: the HUD's parts on their way between two frames (hp2.ini only)
    // On a wide picture a part of the HUD that stands still keeps the place it has, whatever is drawn around it (native.cpp,
    // resolve_wide_2d). 0: every part's place worked out anew in each frame from what lies near what, as it was (hp2.ini only)
    int hud_steady = 1;
    // On a wide picture the HUD's objects are put by the parts the layout's table gives them (native.cpp, g_hud_part). 0: all
    // of them by the lines of each frame, as before (hp2.ini only)
    int hud_anchors = 1;
    int shadow_softness = 100;  // the cars' shadows at High and up: how wide the soft edge gets, percent (hp2.ini only)
    int monitor = 0;            // the screen the game is shown on: 0 where the window is, 1 and up that screen (window.cpp)
    char gpu[128] = {};         // the graphics card that draws the game, by its name; empty: the game's choice (native_vk.cpp)
    int brightness = 0;         // percent added, -50..50
    int contrast = 100;         // percent, 50..150
    int gamma = 100;            // percent, 50..200: 100 changes nothing, above it lighter
    int frame_rate = 30;        // the race's, 30 or 60; the menus have 60
    int pictures = 0;           // above 60: with frame_rate 60, so many pictures a second, drawn between the race's frames (native.cpp)
    int vsync = 1;              // the picture waits for the screen's refresh (no tearing); 0: shown at once
    int draw_distance = 200;    // percent, 100..600: 100 is the PS2's (the menu: Original 100, Advanced 200, High 400, Maximum 600)
    char last_profile[32] = {}; // listed first on the profiles' screen
    int show_fps = 0;
    int traffic_cars = 0;       // Extras, Playable Traffic Cars: the mod by 379Felipe, laid over the game while it is on (traffic.cpp)
    int no_catchup = 0;         // Extras, Disable Catchup: Quick Race and the career start with the game's Catch-up off (pcsettings.cpp)
    int cinematic_replays = 0;  // Extras, Cinematic Replays: a replay with its controls hidden has no black bars either (pcsettings.cpp)
    int cockpit_view = 0;       // Extras, Cockpit View: the player's ride has the game's dash, for the cars that have one (pcsettings.cpp)
    // A steering wheel (pcwheel.h): hp2.ini only so far, no menu rows
    int wheel = 1;              // 1: a wheel Windows has is the game's wheel; 0: wheels are not looked for
    // Whose a wheel is (the Controller screen as the wheels', its Select Device; pcwheel.cpp): "" Automatic (player 1 has the first
    // wheel nobody named, player 2 the next), "none", or the wheel's name. hp2.ini's wheel1 and wheel2
    char wheel_device[2][128] = {};
    int wheel_ffb[2] = {1, 1};  // a player's wheel takes the forces: the same screen's Force Feedback, On or Off
    int wheel_range = 900;      // how far the wheel turns lock to lock, in degrees, as the maker's software has it (G HUB: 900)
    // A wheel's own settings, each player's (the user, 2026-11-25: "each wheel must have its own wheel settings"; the wheels'
    // screen, the row Advanced: pcsettings.cpp). hp2.ini's wheel_lock1, wheel_lock2 and so on
    int wheel_lock[2] = {360, 360};         // how much of what the wheel turns is the game's full lock, degrees
    int wheel_set_range[2] = {1, 1};        // 1: a Logitech wheel is set to turn wheel_lock degrees in all while the game runs (wheel_win.cpp)
    int wheel_force[2] = {100, 100};        // the force feedback's strength, percent (0: none)
    // each kind of force, percent of what the game asks for (0..100): the centring spring, the damper, the road, a hit, the shake
    int wheel_spring[2] = {100, 100}, wheel_damper[2] = {100, 100}, wheel_road[2] = {100, 100}, wheel_crash[2] = {100, 100}, wheel_shake[2] = {100, 100};
    int wheel_invert[2] = {0, 0};           // 1: the wheel's axis the other way
    int wheel_emulate = 0;      // 1: with no wheel connected, a pretend one driven by the keyboard, to look at the game's wheel screens (wheel_win.cpp)
    int wheel_shared = 1;       // 1: a player's wheel and gamepad both drive, whichever is in use; 0: a wheel has its player to itself (pcwheel.cpp)
    int wheel_gas = 1, wheel_brake = 5;     // the pedals' axes: 0 X, 1 Y, 2 Z, 3 Rx, 4 Ry, 5 Rz, 6 and 7 the sliders
    int wheel_pedals_inverted = 1;          // 1: a pedal rests at the top of its axis (Logitech's)
    int slow_motion = 0;        // Extras, Slow Motion: the Zone Slow Motion key works, and a gamepad's two sticks pressed together (pcactions.cpp)
    int boost_all = 0;          // Extras, Speed Boost for All Cars: the boost in every car and mode (pcsettings.cpp, the nitro handler's hook)
    int no_intro = 0;           // Extras, No Intro Movies: every start is without the opening films (pcsettings.cpp, no_intro_frame)
    int cheat_menu = 0;         // Extras, Cheat Menu: F8 in the menus opens the cheats' pages, and what is on there is on in the game
    unsigned char cheats[96] = {};      // by the entry of the game's table of codes (EasterEggs): 1 on
    int debug_menu = 0;         // Extras, DEBUG.not.pretty: the developers' Debug item shows in the pause menu (pcmenu.cpp)
    char language[16] = "ENGLISH";  // a key of pc_languages() (pclang.h)
    bool ask_language = true;   // hp2.ini has no language yet: the very first start shows the game's own choice of language (pclang.cpp)
    int log = 1;                // speed.log is written (1), or nothing is (0): hp2.ini has it as on / off
    int shadows = 0;            // the cars' shadows: 0 the PS2's (the game's polygons), 1 high (the body's own outline from the sun, laid by
                                // the picture's depth on whatever is under it; in the mirror and far off too), 2 maximum: that at twice
                                // the map's resolution (native.cpp, "Shadows")
    int particles = 0;          // the smoke, sparks and dust: 0 original (the game's own sprites, made by its own code as on the PS2),
                                // 1 advanced (the same particles, each sprite at its exact place and with soft edges): native.cpp
    int reflections = 0;        // the cars' reflections in a race: 0 the PS2's (views of 64 pixels, a few of the six a frame), 1 advanced:
                                // the PS2's resolution, all six views every frame; 2 high: twice the resolution, all six every frame
    int max_detail = 1;         // 1: scenery, cars and tyres at their best models whatever the size; 0: the game's choice by size
    int volume = 100;           // percent
    int key[PCB_COUNT][2] = {}; // virtual-key codes, 0 = none
    int key2[PCB_COUNT][2] = {};    // player 2's, when the keyboard is shared in two (window_pad)
    // The keys are actions, named by what the button does in the game's layout A (pc_button_action); in another layout of the gamepad's
    // the key keeps its action and presses the button that has it there (pc_layout_button). Two more, which on a pad are two buttons
    // together: the zone cameras, 0 Zone Freeze (Triangle + L2), 1 Zone Zoom (Triangle + R2).
    // And 2, the zone camera no pad has a button for: Zone Slow Motion (JOY_EVENT_ZONE_SLOMO, 97), a key of the actions' only.
    int key_zone[3][2] = {}, key2_zone[3][2] = {};
    // 1: the driving keys are the game's actions themselves (pcactions.cpp); 0: the keys press the buttons of a PS2 pad, the way
    // it was (window_pad). hp2.ini [keyboard] actions. The old way stays until the new one is proven (the user, 2026-11-23).
    int key_actions = 1;
    // 1: in the menus the pointer puts the highlight on the row under it at once and a click chooses it at once (pcmenu.cpp,
    // mouse_frame: the menu's own handler called directly); 0: the way it was, a pad's press at a time. hp2.ini [keyboard]
    // mouse_direct. The old way stays until the new one is proven, as the keys' does.
    int mouse_direct = 1;
    // What player 2 drives with (Controls > Player 2 Controls; the user, 2026-11-23: "a choice of gamepad or keyboard is enough").
    // 0 Gamepad: the one player 2 has (pad_device below: the Controller screen's Select Device); with none, player 2's keys
    // after all. 1 Keyboard: player 2's keys (key2), player 1 the rest.
    // (It was "which gamepad, first or second" till then: hp2.ini's player2, no longer read.)
    int player2_keyboard = 0;
    // Which gamepad each player has (the game's Controller screen, Select Device; the user, 2026-11-23): "" Automatic, the
    // default (the first gamepad is player 1's, the next player 2's: with one gamepad player 2 has none), "none" for none, or
    // the gamepad's name as pad_device_name gives it. The same gamepad is never both players'. hp2.ini [gamepad] gamepad1,
    // gamepad2 (window.cpp, pad_slot).
    char pad_device[2][64] = {};
    int pad[PCB_COUNT] = {};    // XInput button masks, 0 = none
    // A gamepad layout of the player's own, each player's (the user, 2026-11-25: "a Manual layout for every gamepad, so that
    // the players set the controls themselves"): the PcButton each thing has (or PC_CUSTOM_L3), -1 none. The steering is the
    // left stick (analog), two buttons (digital) or both at once; the throttle, the brake and the handbrake go by how far a
    // trigger is pressed (analog) or are all or nothing (digital). The right stick is the player's choice (the user,
    // 2026-11-25: "the right stick as the camera, as in modern racing games ... and the player chooses: the throttle and
    // the brake, the camera, or nothing"): the pedals as in the game's own layouts, or the camera turned around the car
    // (pcplatform.cpp, stick camera). hp2.ini [gamepad] custom1_..., custom2_...
    int custom[2][PCC_COUNT] = {};
    int custom_steer[2] = {0, 0};           // 0 the left stick, 1 two buttons, 2 both
    int custom_pedals_digital[2] = {0, 0};
    int custom_right_stick[2] = {2, 2};     // 0 nothing, 1 the pedals (up the throttle, down the brake), 2 the camera
    // the zone cameras (a cop's calls): 0 the game's way, Change View held and a gear's button; 1 a button each
    int custom_zone_buttons[2] = {1, 1};
    // A gamepad's own settings, each player's (the user, 2026-11-24): the game's Controller screen, the row Calibration (pcsettings.cpp)
    int pad_dead[2] = {24, 24};             // the left stick's dead zone, percent
    int pad_steer[2] = {100, 100};          // steering sensitivity, percent: above 100 more of it near the center
    int pad_vibration[2] = {100, 100};      // the vibration's strength, percent (0: none)
    int pad_trigger_dead[2] = {0, 0};       // a trigger's dead zone, percent
    int pad_vibration_mode[2] = {1, 1};     // 0 the game's own play of its envelopes, 1 by the car at once ("Responsive")
    int pad_radio[2] = {1, 1};              // the police radio on the gamepad's speaker (a DualSense or a DualShock 4 on a cable)
    int pad_radio_volume[2] = {100, 100};   // and how loud, percent
    int pad_adaptive[2] = {1, 1};           // a DualSense's adaptive triggers: resistance under the throttle and the brake
    int pad_icons = 0;          // 0 by the pad, 1 Xbox, 2 PlayStation, 3 Nintendo: its menu layout goes with them
    int win_w = 960, win_h = 720;   // the window's picture, in pixels
    PcConfig();
    void default_keys();
    void default_keys2();
    void default_pad();
    void default_custom(int player);
};

extern PcConfig g_cfg;
extern int g_layout_playstation;    // the layouts' screen shows PlayStation's buttons: unless an Xbox pad is there or chosen
extern int g_pad_playstation;   // the pad in use has PlayStation's buttons (window_pad keeps it)
extern int g_pad_nintendo;      // or Nintendo's: the same buttons in the same places, lettered A right, B bottom, X top, Y left
extern int g_pad_driving_now;   // 1 while a car is driven: the players' steering sensitivities count then, else a stick is as it is
// The race's aspect as it is drawn: 4:3, or the shape of the resolution (window_view_size): a 16:9 resolution has 16:9 and 4:3, a
// 16:10 one 16:10 and 4:3, a 4:3 one 4:3 alone (the user, 2026-11-14: no 16:10 picture in 1920x1080). pc_aspect_limit is that shape.
// How many steps of the world a second the race is asked for: 30 as on the PS2, or 60 (Frame Rate 60, and 120, which is 60 with a
// picture drawn between each two). Everything that makes the game behave at more steps a second as it did at 30 (whose turn it
// is to think, the windows of the AI drivers' averages, the steering's scale, the animations between frames) goes by this number
// and by the world's clock, never by a setting's name: a new rate has them all without being listed anywhere (the user,
// 2026-11-14: "all the physics fixes for 60 must work at 120 and at whatever comes later").
inline int pc_world_rate() { return g_cfg.frame_rate >= 60 ? 60 : 30; }
// what Frame Rate shows: 30, 60, or the pictures a second above that
inline int pc_picture_rate() { return g_cfg.frame_rate >= 60 ? (g_cfg.pictures > 60 ? g_cfg.pictures : 60) : 30; }

int pc_aspect();
int pc_aspect_limit();
void pc_config_init();          // read hp2.ini (HP2_INI=<path> names another file); the main program only
void pc_config_save();
bool pc_config_log();           // hp2.ini's log key alone, before anything else is read: whether there is to be a log at all
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
