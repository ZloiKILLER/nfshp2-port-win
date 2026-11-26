// Hardware behind the EE: what the game still sees of the DMAC, VIF1 and the GS, from io_read/io_write.
#pragma once
#include <string>
#include "ps2.h"

#include <atomic>
#include <mutex>
#include <vector>

// DMAC (dma.cpp)
bool dma_io_read(u32 addr, u64 &value);
bool dma_io_write(u32 addr, u64 value);
extern volatile u32 g_dmac_pending;     // channels whose interrupt is waiting

// VIF1 (vif.cpp): the VIFcodes of a transfer, for the interrupts and MARK the game waits on
void vif1_scan(const u32 *words, u32 count);
bool vif_io_read(u32 addr, u64 &value);
bool vif_io_write(u32 addr, u64 value);

// the GS's privileged registers (gs_regs.cpp)
bool gs_io_read(u32 addr, u64 &value);
bool gs_io_write(u32 addr, u64 value);
void gs_vblank();

// window (window.cpp)
void window_start();
int window_dpi_aware();                 // before the window: the process takes the screen's real pixels; what it was (0 unaware, 1 system, 2 per monitor)
void window_dpi_thread();               // HP2_DPI, for tests: this thread as in a process Windows scales
void *window_handle();                  // the window to draw to (an HWND), null until it is there
void window_set_fullscreen(bool fs);    // from any thread; saves hp2.ini
void window_set_size(int w, int h);     // from any thread; saves hp2.ini; a fullscreen window keeps it for later
void window_cursor_frame();             // the pointer shown or hidden as the screen and the mouse have it: once a frame
int window_monitor_count();                // the screens there are
void window_monitor_size(int index, int &w, int &h);     // of screen 1 and up, in pixels; 0, 0: no such screen
int window_monitor_number(int index);      // Windows' own number of that screen
std::string window_monitor_name(int index);     // its own name ("DELL U2720Q"), plain letters; empty: not known
int window_monitor_now();                  // the screen the window is on, 1 and up; 0: none
void window_set_monitor(int index);         // from any thread; saves hp2.ini; 0: where the window is, 1 and up: moved to that screen
void window_screen_size(int &w, int &h);    // the monitor the window is on (the main one before there is a window), in pixels
void window_view_size(int &w, int &h);  // the resolution: the setting's window, or fullscreen the size chosen for it (the monitor's own as it comes)
// native_vk.cpp: the most multisampling and anisotropy the GPU in use has (0: none); false until there is a device
bool native_vk_limits(int &aa, int &aniso);
void window_quit();                     // from any thread; closes the window and ends the program
// the program again, for another profile: a save is the game's whole state, and a new profile needs the game's start
// The game started again as this profile; no_intro: the new one shows no opening films and its window stands where this one did
// (a switch of the menus that needs the game started anew: Extras' Playable Traffic Cars, Select GPU)
void window_relaunch(const std::string &profile, bool no_intro = false);
// typing a name: the game sees no input; window_text_next gives a character, 8 Backspace, 13 Enter, 27 Esc, or 0 for nothing yet.
// By the key, not the keyboard's layout: names are in Latin letters whatever the layout is.
void window_text_begin(bool keep = false);    // keep: with the letters typed while armed
void window_text_arm(bool on);
void window_text_end();
int window_text_next();
bool window_delete_pressed();           // Delete, or Square / X on the gamepad: once a press
// the keyboard's driving keys of a player as actions (PcAction, by index), as window_pad / window_pad2 read them this frame;
// false, and all up: the keys are no actions now (not in a race, a menu up, a replay, the window not in front, actions=0)
bool window_actions(int port, bool keys[]);
// the gamepad of this port has both its sticks pressed (Extras' Slow Motion); valid: a gamepad drives for this port now
bool window_pad_slomo(int port, bool &valid);
// the game's Controller screen, Select Device: a player's choice ("" Automatic, "none", or a gamepad's name), the choice next to
// one (left or right), and a choice made
std::string window_pad_value(int port);
std::string window_pad_step(const std::string &from, int dir);
void window_pad_set(int port, const std::string &value);
bool window_key_down(int vk);          // a key of the keyboard is down now, the game's window in front (or the key script has it)
int window_input_kind();                // the device last used: 0 the keyboard, 1 a gamepad (g_pad_playstation: which)
// binding a key or a pad button from the menu: the game sees no input until the next press
void window_capture_begin(int what);    // 1 a key, 2 a pad button
void window_rumble_port(int port, int small, int large);
// The motors by the port's own working out (pcplatform.cpp, "Responsive"): while a port is so, the game's own two numbers for it
// (window_rumble_port) are not taken. small and large 0..255.
void window_rumble_owner(int port, bool own);
void window_rumble_own(int port, int small, int large);
// A DualSense's adaptive triggers (pad.h): whether a player's gamepad is one, and what each player's two triggers are to be
// (pad_trigger_resistance and the like, 0 loose): [player][0 the left trigger, 1 the right]. A player with the setting off, or
// with another gamepad, gets none, and a gamepad that is nobody's is let loose.
bool window_pad_dualsense(int port);
// The police radio on a gamepad's speaker: whether a player's gamepad has one that can take it (a DualSense or a DualShock 4 on a
// cable), and, once a frame, the choice of the speaker: player 1's if they have one and the setting on, else player 2's.
bool window_pad_speaker(int port);
void window_radio_frame();
void window_triggers(const unsigned want[2][2]);
bool window_pad_busy(int port);                 // this player's gamepad is in use now (a button, a trigger, a stick)
bool window_right_stick(int port, float &x, float &y);     // its right stick now, -1..1, right and up above 0; false: no gamepad
void window_rumble(int small, int large);   // the gamepad's motors: on/off and 0..255
int window_capture_poll(int &code);     // 0 waiting, 1 key (virtual key), 2 pad button (mask), 3 cancelled
void window_capture_frame();            // a step of the capture: every frame of the page that asked, until the poll answers
extern std::atomic<bool> g_pc_ui_dirty; // a setting changed behind the menu's back (Alt+Enter)

// INTC causes raised by the hardware
enum {
    INTC_GS = 0,
    INTC_VBLANK_START = 2,
    INTC_VBLANK_END = 3,
    INTC_VIF1 = 5,
};
void raise_intc(int cause);
