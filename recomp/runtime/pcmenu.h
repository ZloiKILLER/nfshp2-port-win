// The port's items in the game's menus (pcmenu.cpp), see recomp/docs/frontend-menus.md.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ps2.h"

// ---- host calls into the game -------------------------------------------------------
extern Ctx *g_pc_ctx;           // the EE context, set by the hooks
// call a game function by its retail address (integer and pointer arguments only)
u32 pc_call(u32 fn, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0, u32 t0 = 0);
std::string pc_str(u32 addr);   // a guest C string
u32 pc_hash(const char *s);     // bStringHash of the game
u32 pc_keep(u32 size);          // zeroed guest memory that stays, out of the port's own early block (not the game's heap)
u32 pc_alloc(u32 size);         // zeroed guest memory that stays; early, as the game's own lasting blocks
bool pc_racing();               // the game flow is in a race (else the frontend or loading)
bool pc_skip_wanted();          // a race waits to be started or left: Enter is the button it waits for
bool pc_two_player_race();      // a race of two players
bool pc_cop_mode();             // the race is one where a player is the cop (You're the Cop)
bool pc_two_player_screen();    // a screen where two players choose at once
bool pc_replay_playing();       // a replay is on screen, with its bar of buttons
// keys: this port's player drives with the keyboard now, which has its own layout whatever the gamepad's is; else the chosen one is back
void pc_keyboard_layout(int port, bool keys);
// A gamepad layout of the player's own (pcplatform.cpp): the game's number of player 1's (player 2's is the next), whether a
// number is one of the two, and "its buttons have changed" (the table the game reads is written anew, and the port's handlers
// are tied to it again)
const u32 PC_CUSTOM_LAYOUT = 0x3c;
inline bool pc_custom_layout(u32 id) { return id == PC_CUSTOM_LAYOUT || id == PC_CUSTOM_LAYOUT + 1; }
// The port's own ready layout, the list's ninth, "I" (the user, 2026-11-25: "a layout as for modern games, where the right
// stick is the camera"): made the same way as a player's own, from a table that is not the player's to change, and both
// players'. pc_port_layout: a number whose buttons the port's table answers, any of the three.
const u32 PC_MODERN_LAYOUT = 0x3e;
inline bool pc_port_layout(u32 id) { return id >= PC_CUSTOM_LAYOUT && id <= PC_MODERN_LAYOUT; }
void pc_custom_changed(int player);
bool pc_custom_port(int port);  // the port's layout now is one the port's table answers (a player's own, or the modern one)
int pc_layout_hint_cells();     // the layouts' screen's hints: 1 in cells of their own, 0 in Select's and L3's, -1 not known yet (pcplatform.cpp)
bool pc_car_input_port(int port);        // the same for player 1 (0) or player 2 (1) of a race of two
bool pc_car_input();            // the player has the car or a race's own controls (not a menu, the pause menu included)
bool pc_race_menu();
bool pc_objective_screen();     // a race's objective is on the screen, before the start or in the pause menu
u32 pc_input_mode();            // Player::SetInputMode's last: 2 is a car being driven
bool pc_pause_menu();           // the pause menu is up, whatever the player's mode is
u32 pc_player_car();           // the first player's Car in a race (Player +4), 0 outside one
bool pc_actions_port(int port); // this player's keys are the game's actions now (pcactions.cpp), not a pad's buttons
bool pc_debug_menu();           // the developers' Debug menu is up (Extras' DEBUG.not.pretty)
bool pc_popup_up();             // a question is over the screen (a tPopupMenu: restart, quit and the like)
bool pc_pause_subpage();        // the race is paused and a page under the pause menu is up (its options, the objective, the Debug menu)
u32 pc_screen_hash();           // the screen whose menu is up, 0 with none
bool pc_title_waiting();        // the title screen waits for a button (not playing its logos and films)
bool pc_wide_menu(u32 hash);    // a menu screen drawn wide by the main menu's rules (native.cpp, g_wide_menu_now)
bool pc_popup_screen(u32 hash); // a window over a screen (a question, a short list, the memory card's message)
bool pc_cursor_wanted();
bool pc_delete_pressed();       // Delete, Square / X on a gamepad, or the pointer on a page's "Delete" hint: once a press
int pc_mouse_buttons();         // PcButton bits the mouse holds down in the menus now
bool pc_hud_up();                       // the race's own HUD is what is on the screen: its panels keep to the screen's edges            // the race's own results and menus are up

// ---- pages: screens of rows made out of the game's own Options screen ----------------
// A page is a Flailer screen cloned from the game's Options screen, with a value text and
// the Display screen's arrows added per row, and handled here instead of by the game.
// Rows: a label at the left, an optional value in the middle (Left/Right change it,
// arrows around it show that it can), a one-line help under the menu. Seven rows show; a longer page scrolls under them.
struct PcRow {
    std::string label;
    std::string blurb;
    std::function<std::string()> value;     // null: an action row (no value, no arrows)
    std::function<void(int dir)> change;     // dir -1 / +1 for Left / Right, 0 for Select
    bool select_only = false;                // Left and Right do nothing and no arrows show: a binding, which shows a value but only Select starts
    std::function<bool()> disabled;          // true: the row shows greyed and cannot be chosen, by the pad or by the pointer (the game's own grey)
    // A row that is a binding in one state and a choice in another (a layout's pedals: any button, or one of the two triggers):
    std::function<bool()> select_only_if;    // select_only while it says so
    std::function<std::string()> blurb_if;   // the help line by the row's state, in place of blurb
};

struct PcPage {
    std::string name;                       // the screen name the game's ChangeToScreen takes
    std::string title;                      // the heading
    std::vector<PcRow> rows;
    u32 hash = 0;                           // the Flailer screen's name hash (filled at registration)
    u32 cur = 0;                            // the slot the highlight is on (1-based, 1..7)
    int top = 0;                            // the row in the first slot
    std::function<void(PcPage &)> on_open;  // the rows are made as the page opens: registered with at least 8 so that its screen has all it may need
    bool hide_back = false;                 // no Back at the bottom: there is nowhere to go
    std::function<void()> on_back;          // Back is the page's own (the first screen: Exit)
    std::string back_text;                  // and its word at the bottom, when not Back
};

// register a page; before the frontend is built (the page's screen is made as FRONTB loads)
PcPage &pc_add_page(const char *name, const char *title);
// the pages behind the Options screen's Controls and Graphics rows
void pc_options_pages(const char *controls, const char *graphics);
void pc_profiles_page(const char *name);    // Options' Profiles
// the hint of the button that deletes, on that page: Square's icon or the Delete key's, shown or not
void pc_delete_hint(const char *page, bool pad, bool show, const char *text = "Delete");
void pc_retranslate();                      // the language changed: the port's texts on the frontend's screens again
PcPage *pc_find_page(const char *name);
// go to a page now (from a row that opens another page)
void pc_open_page(const char *name);
// The game's Controller screen, as the gamepads' or as the steering wheels' (the same rows for each player: pcmenu.cpp)
void pc_open_controller(bool wheels);
bool pc_controller_wheels();    // it was last opened as the wheels'
// A row of the page that is up is edited: fn gets every button's message while it lasts (1 Select, 6 Left, 7 Right, 8 Up, 9 Down,
// 17 Back), a gamepad's and the keyboard's alike, and says whether the editing goes on
void pc_row_edit(std::function<bool(int msg)> fn);
bool pc_row_editing();
// rewrite the values of the page on screen (after a value changed by something else)
void pc_refresh_page();
// the page on screen got other rows
void pc_reshape_page();
// called once per frame while a frontend screen runs (key capture, values changed elsewhere)
void pc_on_frame(std::function<void()> fn);
void traffic_frame();           // traffic.cpp: the cars' table follows Extras > Playable Traffic Cars (the menus' frames)
bool traffic_available();       // false: this disc's cars' table is not one the mod is for
// the row of this page the highlight is on, from 0; -1 when the page is not the screen that is up
int pc_page_row(const char *name);
// The game's own question over the screen, with Yes and No (a tPopupMenu, as "quit the race?"): answer is called with the choice, No if
// it was left. One at a time; pc_asking while it is up.
void pc_ask(const std::string &title, const std::string &question, std::function<void(bool yes)> answer, const char *yes = nullptr,
            const char *no = nullptr);       // the two buttons' words, when not Yes and No
void pc_ask_exit();             // "Leave the game?", Exit and Cancel
bool pc_asking();
// The game's letter grid (the Player Names screen's) on the page that is up, for a name of up to 7 characters from a gamepad: the name is
// typed in the highlighted row; done gets it, empty if nothing was typed. False: no grid here. Without grid the keyboard types
// it (Enter takes it, Esc leaves it), shown the same way: the seven places, the bar under the one being typed.
bool pc_name_grid(std::function<void(const std::string &name)> done, bool grid = true);
bool pc_name_grid_up();
int pc_name_grid_mode();        // 0 no name is typed, 1 the keyboard types it, 2 the grid is up
// a row's blurb on screen now (e.g. "Press a key")
void pc_set_blurb(const std::string &text);
// registered by pcsettings.cpp
void pc_settings_register();
bool pc_on_title();             // the title screen is up: the logos, the opening film, "press start"
std::string pc_screen();        // the name of the screen that is up, "" with none
// a screen opened with the screens it is reached through put under it, so that Back leads through them (first: the one furthest back)
void pc_open_page_under(const std::vector<std::string> &under, const char *name);
bool pc_tap(int button);        // one press of a pad button (PCB_*) for the menus, as the pointer makes them; false: one is still going
void pc_profile_loaded();       // the main menu is up: the profile's save is in the game's memory
void pc_save_on_exit();         // the window's thread: the profile's save, as the game is left from its menus
bool pc_profile_save_ready();   // the profile's save is in Fred (the main menu has been up)
bool pc_save_profile_now();     // the profile's save written now (in place of the game's Autosave screen)
// pcplatform.cpp: the button icons for the bindings and the device in use; once per menu frame
void pc_paint_buttons();
std::string pc_text_cursor(u32 font_hash);      // the blinking cursor of a text being typed, in that font
bool pc_text_cursor_changed();                  // it blinked since the last call
u32 pc_replay_bar_stamp();                      // the replay bar's keys changed
void pc_replay_bar_retouch(u32 *px, int stride);
