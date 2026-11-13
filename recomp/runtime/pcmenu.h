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
u32 pc_alloc(u32 size);         // zeroed guest memory that stays; early, as the game's own lasting blocks
bool pc_racing();               // the game flow is in a race (else the frontend or loading)
bool pc_skip_wanted();          // a race waits to be started or left: Enter is the button it waits for
bool pc_two_player_race();      // a race of two players
bool pc_two_player_screen();    // a screen where two players choose at once
bool pc_replay_playing();       // a replay is on screen, with its bar of buttons
// keys: this port's player drives with the keyboard now, which has its own layout whatever the gamepad's is; else the chosen one is back
void pc_keyboard_layout(int port, bool keys);
bool pc_car_input();            // the player has the car or a race's own controls (not a menu, the pause menu included)
bool pc_race_menu();
u32 pc_screen_hash();           // the screen whose menu is up, 0 with none
bool pc_cursor_wanted();
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
// rewrite the values of the page on screen (after a value changed by something else)
void pc_refresh_page();
// the page on screen got other rows
void pc_reshape_page();
// called once per frame while a frontend screen runs (key capture, values changed elsewhere)
void pc_on_frame(std::function<void()> fn);
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
// pcplatform.cpp: the button icons for the bindings and the device in use; once per menu frame
void pc_paint_buttons();
std::string pc_text_cursor(u32 font_hash);      // the blinking cursor of a text being typed, in that font
bool pc_text_cursor_changed();                  // it blinked since the last call
u32 pc_replay_bar_stamp();                      // the replay bar's keys changed
void pc_replay_bar_retouch(u32 *px, int stride);
