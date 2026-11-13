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
void window_screen_size(int &w, int &h);    // the monitor the window is on (the main one before there is a window), in pixels
void window_view_size(int &w, int &h);  // what the picture is shown in: the setting's window, or the monitor when fullscreen
// native_vk.cpp: the most multisampling and anisotropy the GPU in use has (0: none); false until there is a device
bool native_vk_limits(int &aa, int &aniso);
void window_quit();                     // from any thread; closes the window and ends the program
// the program again, for another profile: a save is the game's whole state, and a new profile needs the game's start
void window_relaunch(const std::string &profile);
// typing a name: the game sees no input; window_text_next gives a character, 8 Backspace, 13 Enter, 27 Esc, or 0 for nothing yet.
// By the key, not the keyboard's layout: names are in Latin letters whatever the layout is.
void window_text_begin(bool keep = false);    // keep: with the letters typed while armed
void window_text_arm(bool on);
void window_text_end();
int window_text_next();
bool window_delete_pressed();           // Delete, or Square / X on the gamepad: once a press
int window_input_kind();                // the device last used: 0 the keyboard, 1 a gamepad (g_pad_playstation: which)
// binding a key or a pad button from the menu: the game sees no input until the next press
void window_capture_begin(int what);    // 1 a key, 2 a pad button
void window_rumble_port(int port, int small, int large);
void window_rumble(int small, int large);   // the gamepad's motors: on/off and 0..255
int window_capture_poll(int &code);     // 0 waiting, 1 key (virtual key), 2 pad button (mask), 3 cancelled
extern std::atomic<bool> g_pc_ui_dirty; // a setting changed behind the menu's back (Alt+Enter)

// INTC causes raised by the hardware
enum {
    INTC_GS = 0,
    INTC_VBLANK_START = 2,
    INTC_VBLANK_END = 3,
    INTC_VIF1 = 5,
};
void raise_intc(int cause);
