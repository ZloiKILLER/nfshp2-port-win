// The Win32 window, the keyboard and the gamepads. The renderer draws to the window itself (native_vk.cpp).
#include "hw.h"
#include "pcconfig.h"
#include "host.h"
#include "pcmenu.h"
#include "pad.h"
#include "native.h"
#include <array>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

static HWND g_hwnd;
static bool g_fullscreen;               // borderless, on the window's monitor
static RECT g_windowed_rect;
static std::atomic<bool> g_want_fullscreen;
static const UINT WM_SETFULLSCREEN = WM_APP + 1;
static const UINT WM_SETWINDOWSIZE = WM_APP + 2;
static const UINT WM_CURSORSTATE = WM_APP + 3;
// set when a setting changed from the keyboard (Alt+Enter): the menu shows the new value
std::atomic<bool> g_pc_ui_dirty;

// borderless fullscreen on the monitor the window is on, and back; on the window's thread
static void set_fullscreen_now(bool fs)
{
    if (!g_hwnd || fs == g_fullscreen) return;
    if (fs) {
        GetWindowRect(g_hwnd, &g_windowed_rect);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, g_windowed_rect.left, g_windowed_rect.top,
                     g_windowed_rect.right - g_windowed_rect.left, g_windowed_rect.bottom - g_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_fullscreen = fs;
}

// from any thread; the setting is saved, and the menu told
void window_set_fullscreen(bool fs)
{
    g_cfg.fullscreen = fs;
    pc_config_save();
    g_pc_ui_dirty = true;
    if (g_hwnd) PostMessageA(g_hwnd, WM_SETFULLSCREEN, fs, 0);
}

void window_set_size(int w, int h)
{
    g_cfg.win_w = w;
    g_cfg.win_h = h;
    pc_config_save();
    if (g_hwnd) PostMessageA(g_hwnd, WM_SETWINDOWSIZE, 0, 0);
}

void *window_handle() { return g_hwnd; }

void window_screen_size(int &w, int &h)
{
    MONITORINFO mi = {sizeof mi};
    HMONITOR mon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoA(mon, &mi)) {
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
}

void window_view_size(int &w, int &h)
{
    if (g_cfg.fullscreen) {
        window_screen_size(w, h);
    } else {
        w = g_cfg.win_w;
        h = g_cfg.win_h;
    }
}

void window_relaunch(const std::string &profile)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    SetEnvironmentVariableA("HP2_PROFILE", profile.c_str());
    // The new one starts as from the desktop: no standard handles (this one's are the log's pipe, whose numbers mean nothing there), and
    // speed.log free for it to open.
    host_log_close();
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    SetEnvironmentVariableA("HP2_PROFILE", nullptr);
}

void window_quit()
{
    if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    else ExitProcess(0);
}

// the pointer shows in the menus; data\ui\cursor.cur or cursor.ani (beside the program, or under HP2_DATA) replaces the arrow
static std::atomic<bool> g_cursor_on{false};    // not till a screen asks for it and the mouse moves (window_pad)
static HCURSOR menu_cursor()
{
    static HCURSOR c = [] {
        char path[MAX_PATH];
        DWORD n;
        if (const char *data = getenv("HP2_DATA")) {
            n = (DWORD)snprintf(path, MAX_PATH, "%s\\", data);
        } else {
            n = GetModuleFileNameA(nullptr, path, MAX_PATH);
            while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        }
        for (const char *name : {"data\\ui\\cursor.cur", "data\\ui\\cursor.ani"}) {
            snprintf(path + n, MAX_PATH - n, "%s", name);
            if (HCURSOR own = LoadCursorFromFileA(path)) return own;
        }
        return LoadCursor(nullptr, IDC_ARROW);
    }();
    return c;
}

// Hidden over all of the window, the title bar too, while it is in front; SetCursor alone covers the client area and only from the next message.
static void apply_cursor(HWND h)
{
    static bool hidden;
    const bool hide = !g_cursor_on && GetForegroundWindow() == h;
    if (hide != hidden) {
        const int count = ShowCursor(hide ? FALSE : TRUE);
        hidden = hide;
        if (getenv("HP2_NATIVELOG")) fprintf(stderr, "cursor: %s, count %d\n", hide ? "hidden" : "shown", count);
    }
    SetCursor(g_cursor_on ? menu_cursor() : nullptr);
}

// ---- typing a name ------------------------------------------------------------------------
static std::atomic<bool> g_text_on, g_text_script_fresh;
// Enter and Esc count once let go since the typing began. The game sees a key by its state, the window by its message, which can come after:
// the Enter that opened the typing would otherwise end it at once, with the name as it was.
static std::atomic<bool> g_text_enter_free, g_text_esc_free;
static std::atomic<unsigned> g_text_in, g_text_out;
static char g_text_ring[64];

static void text_push(int ch)
{
    const unsigned at = g_text_in;
    if (at - g_text_out >= sizeof g_text_ring) return;
    g_text_ring[at % sizeof g_text_ring] = (char)ch;
    g_text_in = at + 1;
}

// a key as a character of a name: the letters, digits, space and what tKeyboard has of the rest, ! . #
static int text_char(int vk, bool shift)
{
    if (vk >= 'A' && vk <= 'Z') return shift != ((GetKeyState(VK_CAPITAL) & 1) != 0) ? vk : vk + 32;
    if (vk == '1' && shift) return '!';
    if (vk == '3' && shift) return '#';
    if (vk >= '0' && vk <= '9') return shift ? 0 : vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + vk - VK_NUMPAD0;
    if (vk == VK_SPACE) return ' ';
    if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) return shift ? 0 : '.';
    if (vk == VK_BACK) return 8;
    if (vk == VK_DELETE) return 127;
    if (vk == VK_LEFT) return 1;                    // the cursor in the name
    if (vk == VK_RIGHT) return 2;
    if (vk == VK_HOME) return 3;
    if (vk == VK_END) return 4;
    if (vk == VK_RETURN) return 13;
    if (vk == VK_ESCAPE) return 27;
    return 0;
}

int window_text_next()
{
    const unsigned at = g_text_out;
    if (at == g_text_in) return 0;
    const int ch = (unsigned char)g_text_ring[at % sizeof g_text_ring];
    g_text_out = at + 1;
    return ch;
}

// While a name is picked on the letter grid the keyboard's letters are kept: the first one says the player is back at the keyboard, and
// is the first typed (window_text_begin(true)). Enter, Esc and the keys that move are not: they only bring the keyboard back.
static std::atomic<bool> g_text_armed;

void window_text_arm(bool on)
{
    if (on && !g_text_armed) g_text_out = g_text_in.load();
    g_text_armed = on;
}

void window_text_begin(bool keep)
{
    if (!keep) g_text_out = g_text_in.load();
    g_text_script_fresh = true;
    // only a key that is down now is the one that opened the typing: any other counts at its first press
    g_text_enter_free = !(GetAsyncKeyState(VK_RETURN) & 0x8000);
    g_text_esc_free = !(GetAsyncKeyState(VK_ESCAPE) & 0x8000);
    g_text_on = true;
}

static bool g_cap_release;              // after an answer: the game sees nothing until it is let go, or the key just bound also moves the menu

void window_text_end()
{
    g_text_on = false;
    g_cap_release = true;
}

static std::atomic<int> g_wheel;            // notches not yet taken, up positive

// one notch of the wheel at a time, for the menus
int window_wheel()
{
    int w = g_wheel;
    if (w > 0) g_wheel--;
    else if (w < 0) g_wheel++;
    return w;
}

static std::atomic<int> g_input_kind;       // 0 the keyboard and mouse, 1 a gamepad
int window_input_kind() { return g_input_kind; }

// When the mouse last moved or clicked, by GetTickCount64; 0: not yet, or a key or a pad button since. The pointer shows for 3 seconds after
// it (window_pad), so it is there for whoever uses the mouse and gone for whoever does not.
static std::atomic<unsigned long long> g_mouse_at;
static void mouse_used() { g_mouse_at = GetTickCount64() | 1; }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    // Any new press on the keyboard or the mouse says the player has moved to them (a repeat says nothing, nor do the system's Alt and Win).
    case WM_KEYDOWN:
        if (!(lp & 1 << 30) && wp != VK_LWIN && wp != VK_RWIN) g_input_kind = 0, g_mouse_at = 0;
        if (g_text_on)
            if (const int ch = text_char((int)wp, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
                if ((ch != 13 || g_text_enter_free) && (ch != 27 || g_text_esc_free)) text_push(ch);
        if (!g_text_on && g_text_armed && !(lp & 1 << 30))
            if (const int ch = text_char((int)wp, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
                if (ch >= 32 && ch < 127) text_push(ch);
        break;
    case WM_KEYUP:
        if (wp == VK_RETURN) g_text_enter_free = true;
        if (wp == VK_ESCAPE) g_text_esc_free = true;
        break;
    case WM_MOUSEMOVE: {
        // Windows sends one when a window comes up under a pointer that stands still: only a new place is a move
        static LPARAM last = -1;
        if (lp != last) {
            if (last != -1) mouse_used();
            last = lp;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        g_input_kind = 0;
        mouse_used();
        g_wheel += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        if (g_wheel > 4) g_wheel = 4;
        if (g_wheel < -4) g_wheel = -4;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        g_input_kind = 0;
        mouse_used();
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && !(lp & 1 << 30)) {                   // Alt+Enter
            window_set_fullscreen(!g_cfg.fullscreen);
            return 0;
        }
        break;
    case WM_SETWINDOWSIZE:
        if (!g_cfg.fullscreen) {
            RECT r = {0, 0, g_cfg.win_w, g_cfg.win_h};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        return 0;
    case WM_SETFULLSCREEN:
        set_fullscreen_now(wp != 0);
        return 0;
    case WM_CURSORSTATE:
        apply_cursor(h);
        return 0;
    case WM_ACTIVATE:
        apply_cursor(h);
        pad_rumble_quiet(LOWORD(wp) == WA_INACTIVE);    // a pad does not shake for a game behind another window
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            apply_cursor(h);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        window_rumble(0, 0);            // a pad's motors outlive the program
        pad_settle(150);                // the pads' thread tells them
        host_log_flush();
        ExitProcess(0);
    }
    return DefWindowProcA(h, m, wp, lp);
}

// ---- the mouse ---------------------------------------------------------------------
// The menus' pointer, in the GS's 512 x 448 as the 2D is laid out. HP2_MOUSESCRIPT=<file>: lines "<frame> <x> <y> [1 held]", for runs nobody is at.
bool window_mouse(float &x, float &y, bool &left)
{
    static const std::vector<std::array<int, 4>> script = [] {
        std::vector<std::array<int, 4>> v;
        if (const char *path = getenv("HP2_MOUSESCRIPT"))
            if (FILE *f = fopen(path, "r")) {
                int t, a, b, h;
                char line[64];
                while (fgets(line, sizeof line, f)) {
                    h = 0;
                    if (sscanf(line, "%d %d %d %d", &t, &a, &b, &h) >= 3) v.push_back({t, a, b, h});
                }
                fclose(f);
            }
        return v;
    }();
    if (!script.empty()) {
        static int t;
        t++;
        bool any = false;
        for (auto &s : script)
            if (t >= s[0]) x = (float)s[1], y = (float)s[2], left = s[3] != 0 && t < s[0] + 3, any = true;
        return any;
    }
    left = false;
    if (!g_hwnd || GetForegroundWindow() != g_hwnd) return false;
    left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    POINT p;
    RECT r;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &r) || r.right <= 0 || r.bottom <= 0) return false;
    // the box the picture is shown in (native_vk.cpp, blit_to_window)
    float f = pc_racing() ? native_narrow() : 1.0f;
    float aspect = (4.0f / 3.0f) / f;
    float bw = (float)r.right, bh = (float)r.bottom;
    if (bw > bh * aspect) bw = bh * aspect;
    else bh = bw / aspect;
    float bx = (r.right - bw) / 2, by = (r.bottom - bh) / 2;
    float fx = (p.x - bx) / bw, fy = (p.y - by) / bh;
    x = 256 + (fx * 512 - 256) / f;
    y = fy * 444;
    return fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1;
}

// ---- keyboard as a DualShock 2 ---------------------------------------------------
// Active-low button word as scePadRead reports it, sticks (RX RY LX LY) and the
// twelve pressure bytes (right left up down triangle circle cross square L1 R1 L2 R2).
// The bindings are g_cfg's (hp2.ini, the game's Options > PC Settings).
// HP2_KEYSCRIPT=<file>: lines "<frame or +N> <virtual key, hex> [held frames]", frames being the
// game's pad reads: the keyboard for repeatable runs, whatever window has the focus.
static std::atomic<bool> g_key_script_on;
static bool g_script_key[256];

static void key_script_step()
{
    struct Step { long t, hold; int vk; };
    static const std::vector<Step> steps = [] {
        std::vector<Step> v;
        const char *path = getenv("HP2_KEYSCRIPT");
        FILE *f = path ? fopen(path, "r") : nullptr;
        if (!f) return v;
        char line[128], when[32];
        while (fgets(line, sizeof line, f)) {
            Step s = {0, 6, 0};
            if (sscanf(line, "%31s %x %ld", when, &s.vk, &s.hold) < 2 || when[0] == '#') continue;
            s.t = atol(when + (when[0] == '+')) + (when[0] == '+' && !v.empty() ? v.back().t : 0);
            v.push_back(s);
        }
        fclose(f);
        return v;
    }();
    if (steps.empty()) return;
    g_key_script_on = true;
    static long t = -1;
    t++;
    memset(g_script_key, 0, sizeof g_script_key);
    for (auto &s : steps)
        if (t >= s.t && t < s.t + s.hold) g_script_key[s.vk & 255] = true;
}

static bool key(int vk)
{
    if (g_key_script_on) return vk && g_script_key[vk & 255];
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// each PcButton: its bit in the button word, and its pressure byte (-1: none)
static const struct { int bit, pressure; } kPadBit[PCB_COUNT] = {
    {4, 2}, {6, 3}, {7, 1}, {5, 0},         // up down left right
    {14, 6}, {15, 7}, {13, 5}, {12, 4},     // cross square circle triangle
    {10, 8}, {11, 9}, {8, 10}, {9, 11},     // L1 R1 L2 R2
    {2, -1}, {3, -1}, {0, -1}};             // R3 start select

// which gamepad a player has
static int pad_slot(int port) { return g_cfg.player2 ? 1 - port : port; }

// Delete on the keyboard, Square (X on an Xbox pad) on the first gamepad: true once for each press, while the game's window has the input.
// Asked once a frame by the screen that deletes with it (the profiles).
bool window_delete_pressed()
{
    static bool was;
    bool now = false;
    if (g_hwnd && (g_key_script_on || GetForegroundWindow() == g_hwnd)) {
        const PadState a = pad_read(0), b = pad_read(1);      // either gamepad, as the menus take them
        now = key(VK_DELETE) || (a.ok && (a.mask & 0x4000)) || (b.ok && (b.mask & 0x4000));
    }
    const bool pressed = now && !was;
    was = now;
    return pressed;
}

void window_rumble_port(int port, int small, int large) { pad_rumble(pad_slot(port), small, large); }
void window_rumble(int small, int large)
{
    pad_rumble(0, small, large);
    pad_rumble(1, small, large);
}

// The keyboard is shared when there are two players and not a gamepad for each: player 2 has key2 and a player's keys are only their own.
// Elsewhere player 1 has all of it, the same as with no second player in the world. Set once a frame by window_pad, read by window_pad2.
enum { KEYS_ALONE, KEYS_SHARED_DRIVING, KEYS_SHARED_MENU };
static int g_keys_shared;

static int keys_shared(bool driving)
{
    if (g_cfg.player2 || pad_count() >= 2) return KEYS_ALONE;
    if (driving && pc_two_player_race()) return KEYS_SHARED_DRIVING;
    if (!driving && pc_two_player_screen()) return KEYS_SHARED_MENU;
    return KEYS_ALONE;
}

static bool is_key2(int vk)
{
    for (auto &k : g_cfg.key2)
        if (vk && (k[0] == vk || k[1] == vk)) return true;
    return false;
}

// ---- binding a key or a pad button from the menu ------------------------------------
// window_capture_begin(): the game sees no input; once everything is released the next key
// or pad button pressed is the answer (Esc cancels). Polled from the game's frame.
static std::atomic<int> g_cap;          // 0 off, 1 waiting for release, 2 waiting for a press, 3 done, 4 cancelled
static int g_cap_pad, g_cap_code, g_cap_what;

void window_capture_begin(int what)
{
    g_cap_what = what;
    g_cap = 1;
}
// 0: still waiting; 1: a key (code = virtual key); 2: a pad button (code = mask); 3: cancelled
int window_capture_poll(int &code)
{
    int st = g_cap;
    if (st == 3) {
        g_cap = 0;
        code = g_cap_code;
        return g_cap_pad ? 2 : 1;
    }
    if (st == 4) {
        g_cap = 0;
        return 3;
    }
    return 0;
}

static void capture_step()
{
    PadState ps = pad_read(0), ps2 = pad_read(1);
    int pressed_key = 0;
    for (int vk = 8; vk < 256; vk++) {
        // the sided Shift, Ctrl and Alt are bindable, the generic ones are the same key twice
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_SHIFT || vk == VK_CONTROL ||
            vk == VK_MENU)
            continue;
        if (key(vk)) {
            pressed_key = vk;
            break;
        }
    }
    int pressed_pad = (ps.ok ? ps.mask : 0) | (ps2.ok ? ps2.mask : 0);
    // HP2_CAPTEST=<virtual key>: the test press, a few frames after the capture began
    static const char *test = getenv("HP2_CAPTEST");
    static int waited;
    if (test && g_cap == 2 && ++waited > 30) pressed_key = atoi(test);
    if (g_cap == 1) {
        if (!pressed_key && !pressed_pad) g_cap = 2;
        return;
    }
    if (pressed_key == VK_ESCAPE) {
        g_cap = 4;
        g_cap_release = true;
        return;
    }
    if (g_cap_what == 2) pressed_key = 0;
    if (g_cap_what == 1) pressed_pad = 0;
    if (pressed_key) {
        g_cap_pad = 0;
        g_cap_code = pressed_key;
        g_cap = 3;
        g_cap_release = true;
    } else if (pressed_pad) {
        g_cap_pad = 1;
        g_cap_code = pressed_pad & -pressed_pad;      // the lowest button of those down
        g_cap = 3;
        g_cap_release = true;
    }
}

// The pointer: where a screen takes the mouse (pc_cursor_wanted), and only while the mouse is in use. Once for every frame drawn
// (native_end_frame) as well as for every read of the pad: the game does not read the pad all the time (through the logos and the opening
// film it does not), and a pointer left showing by the screen before would stay through them.
// The difference is taken as a signed number: the window's thread may stamp the mouse a millisecond after this one read the clock, and
// unsigned that is a huge time ago; the pointer went out for a frame at a time while the mouse moved.
void window_cursor_frame()
{
    if (!g_hwnd) return;
    const unsigned long long mouse_at = g_mouse_at;
    const bool cursor = pc_cursor_wanted() && mouse_at && (long long)(GetTickCount64() - mouse_at) < 3000;
    static const bool log = getenv("HP2_NATIVELOG") != nullptr;
    if (log) {                              // the wish itself, whether or not a mouse is there
        static int was = -1, flips;
        const int now = pc_cursor_wanted() ? 1 : 0;
        if (now != was && ++flips < 200) fprintf(stderr, "pointer: the screen %s one, replay %d, car input %d\n", now ? "takes" : "takes no", (int)pc_replay_playing(), (int)pc_car_input());
        was = now;
    }
    if (g_cursor_on != cursor) {
        g_cursor_on = cursor;
        PostMessageA(g_hwnd, WM_CURSORSTATE, 0, 0);
        if (log) fprintf(stderr, "pointer: %s at %.1f s\n", cursor ? "wanted" : "not wanted", GetTickCount64() % 1000000 / 1000.0);
    }
}

// A zone camera's key: on a pad it is Triangle held and L2 (Zone Freeze) or R2 (Zone Zoom) pressed with it, the same in every layout.
// The key holds Triangle and, two reads later, the second button with it, as fingers would.
template <class Press> static void zone_keys(bool down, int which, int &held, Press &press)
{
    if (!down) {
        held = 0;
        return;
    }
    press(PCB_TRIANGLE, 0xFF);
    if (++held > 2) press(which ? PCB_R2 : PCB_L2, 0xFF);
}

u16 window_pad(u8 sticks[4], u8 pressure[12])
{
    {   // HP2_NATIVELOG: when the game reads the pad, a line a second (through all of the start it may not)
        static const bool log = getenv("HP2_NATIVELOG") != nullptr;
        static unsigned long long last;
        static unsigned reads;
        reads++;
        if (log && GetTickCount64() / 1000 != last) {
            last = GetTickCount64() / 1000;
            fprintf(stderr, "pad: read %u at %.1f s\n", reads, GetTickCount64() % 1000000 / 1000.0);
        }
    }
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80;
    memset(pressure, 0, 12);
    // A pad shakes for the player who holds it: with one player, while the gamepad is the device used last (driving on the keyboard
    // leaves a pad on the desk still); with two, each has their own.
    pad_rumble_gate(pad_slot(0), pc_two_player_race() || g_input_kind == 1);
    pad_rumble_gate(pad_slot(1), true);
    // HP2_AUTOPRESS=<start s>,<interval s>: tap cross (and start) on a timer, for
    // walking through the menus with defaults without anyone at the keyboard
    static const char *autop = getenv("HP2_AUTOPRESS");
    // HP2_HOLD=<s>: from that second on, cross is held down (gas, for testing a race)
    static const char *hold = getenv("HP2_HOLD");
    if (hold) {
        static const DWORD t0 = GetTickCount();
        if ((GetTickCount() - t0) / 1000.0 >= atof(hold)) {
            pressure[6] = 0xFF;
            return (u16)~(1u << 14);
        }
    }
    if (autop) {
        static const DWORD t0 = GetTickCount();
        double start = atof(autop), every = strchr(autop, ',') ? atof(strchr(autop, ',') + 1) : 2.0;
        double t = (GetTickCount() - t0) / 1000.0;
        if (t >= start && fmod(t - start, every) < 0.25) {
            pressure[6] = 0xFF;
            // a third number: after that second press cross only (no START, so no pause)
            const char *c2 = strchr(autop, ',') ? strchr(strchr(autop, ',') + 1, ',') : nullptr;
            bool odd = (((int)((t - start) / every)) & 1) || (c2 && t >= atof(c2 + 1));
            return odd ? (u16)~(1u << 14) : (u16)~((1u << 14) | (1u << 3));   // cross, cross+start
        }
        return 0xFFFF;
    }
    if (g_cap && getenv("HP2_CAPTEST")) {
        capture_step();
        return 0xFFFF;
    }
    key_script_step();
    if (!g_hwnd || (!g_key_script_on && GetForegroundWindow() != g_hwnd)) return 0xFFFF;
    static bool esc_held;                  // from cancelling a capture: not a press yet
    if (g_cap) {
        esc_held = true;
        capture_step();
        return 0xFFFF;
    }
    if (g_text_on) {
        esc_held = true;
        // a gamepad's button says the player has moved to it: whoever types offers it its own way (the letter grid)
        {
            const PadState a = pad_read(0), b = pad_read(1);
            if ((a.ok && a.mask) || (b.ok && b.mask) || (g_key_script_on && g_script_key[0xFF])) g_input_kind = 1;
        }
        static const bool by_window = getenv("HP2_TYPEBYWINDOW") != nullptr;     // a test that posts the window's own messages
        if (g_key_script_on && !by_window) {    // the same keys from a test's script, by their edges
            static bool was[256];
            if (g_text_script_fresh) {
                for (int vk = 8; vk < 256; vk++) was[vk] = g_script_key[vk] != 0;
                g_text_script_fresh = false;
                g_text_enter_free = g_text_esc_free = true;     // the script's edges are its own
            }
            for (int vk = 8; vk < 256; vk++) {
                const bool now = g_script_key[vk] != 0;
                if (now && !was[vk])
                    if (const int ch = text_char(vk, false)) text_push(ch);
                was[vk] = now;
            }
        }
        return 0xFFFF;
    }
    if (g_cap_release) {
        bool held = false;
        for (int vk = 8; vk < 0xFE && !held; vk++) held = key(vk);     // not the test script's two "as if a gamepad" marks
        PadState a = pad_read(0), b = pad_read(1);
        if (held || (a.ok && a.mask) || (b.ok && b.mask)) return 0xFFFF;
        g_cap_release = false;
    }
    u16 buttons = 0xFFFF;
    auto press = [&](int b, int amount) {
        buttons &= ~(1u << kPadBit[b].bit);
        int p = kPadBit[b].pressure;
        if (p >= 0 && amount > pressure[p]) pressure[p] = (u8)amount;
    };
    // HP2_SPEEDLOG: F9 changes the frame rate where the menu cannot be reached, in a replay. Not saved
    static const bool frame_key = getenv("HP2_SPEEDLOG") != nullptr;
    static bool f9_held;
    if (frame_key) {
        const bool f9 = key(VK_F9);
        if (f9 && !f9_held) {
            g_cfg.frame_rate = g_cfg.frame_rate == 60 ? 30 : 60;
            fprintf(stderr, "speedlog: F9, frame rate %d\n", g_cfg.frame_rate);
        }
        f9_held = f9;
    }
    // and F10 the draw distance, for comparing the same place
    static bool f10_held;
    if (frame_key) {
        const bool f10 = key(VK_F10);
        if (f10 && !f10_held) {
            g_cfg.draw_distance = g_cfg.draw_distance >= 400 ? 100 : g_cfg.draw_distance * 2;
            fprintf(stderr, "speedlog: F10, draw distance %d\n", g_cfg.draw_distance);
        }
        f10_held = f10;
    }
    // TEMP: HP2_F11DUMP, F11 dumps the next frame's draws and pictures (native_dump_next)
    static const bool f11_key = getenv("HP2_F11DUMP") != nullptr;
    static bool f11_held;
    if (f11_key) {
        const bool f11 = key(VK_F11);
        if (f11 && !f11_held) native_dump_next();
        f11_held = f11;
    }
    bool alt = key(VK_MENU);                // Alt+Enter is the window's
    bool driving = pc_car_input();
    window_cursor_frame();
    auto key_on = [&](int vk) {
        if (!key(vk) || (alt && vk == VK_RETURN)) return false;
        if (g_key_script_on) g_input_kind = 0;      // the real keyboard says so in wndproc
        return true;
    };
    g_keys_shared = keys_shared(driving);
    pc_keyboard_layout(0, driving && g_input_kind != 1);
    const bool shared_driving = g_keys_shared == KEYS_SHARED_DRIVING, shared_menu = g_keys_shared == KEYS_SHARED_MENU;
    auto drive_on = [&](int vk) { return !(shared_driving && is_key2(vk)) && key_on(vk); };
    auto menu_key = [&](int b) { return shared_menu ? pc_menu_key_split(0, b) : pc_menu_key(b); };
    bool enter = !shared_menu && key_on(VK_RETURN);
    if (driving) {
        // a key is an action, pressed as the button that has it in the keyboard's own layout (pc_keyboard_layout)
        for (int b = 0; b < PCB_COUNT; b++)
            if (drive_on(g_cfg.key[b][0]) || drive_on(g_cfg.key[b][1])) press(b, 0xFF);
        static int zone_held[2];
        for (int z = 0; z < 2; z++) zone_keys(drive_on(g_cfg.key_zone[z][0]) || drive_on(g_cfg.key_zone[z][1]), z, zone_held[z], press);
        // Steering is the two keys' buttons alone (the D-pad's Left and Right, JOY_EVENT_STEER): the game's own digital steering, which
        // turns the wheel at its own rate. The stick is left in the middle: pushed to its end with the key, as it used to be, it was the
        // analog steering at full lock at once.
        // Enter is no key of the car's: in the race it does nothing (it was Cross, the throttle or the brake by the gamepad's layout).
        // Where the game waits to go on, before the start and after the finish, it is the button waited for.
        if (enter && pc_skip_wanted()) press(PCB_CROSS, 0xFF);
    } else {
        for (int b = 0; b < PCB_COUNT; b++)
            if (b != PCB_TRIANGLE && b != PCB_START && key_on(menu_key(b))) press(b, 0xFF);
        if (key_on(menu_key(PCB_LEFT))) sticks[2] = 0x00;
        if (key_on(menu_key(PCB_RIGHT))) sticks[2] = 0xFF;
        if (key_on(menu_key(PCB_UP))) sticks[3] = 0x00;
        if (key_on(menu_key(PCB_DOWN))) sticks[3] = 0xFF;
    }
    if (!driving || pc_replay_playing()) {      // a replay has the car's controls and a bar of buttons for the pointer
        int mouse = pc_mouse_buttons();
        for (int b = 0; b < PCB_COUNT; b++)
            if (mouse >> b & 1) {
                press(b, 0xFF);
                if (b == PCB_LEFT) sticks[2] = 0x00;
                if (b == PCB_RIGHT) sticks[2] = 0xFF;
                if (b == PCB_UP) sticks[3] = 0x00;
                if (b == PCB_DOWN) sticks[3] = 0xFF;
            }
    }
    // Esc: back in the menus. In a race it opens the pause menu; there Start closes it, and Back
    // says No to its questions
    if (!key(VK_ESCAPE)) esc_held = false;
    if (!esc_held && key_on(VK_ESCAPE)) {
        if (pc_racing() && !pc_race_menu()) press(PCB_START, 0xFF);
        if (!driving) press(PCB_TRIANGLE, 0xFF);
    }
    g_pad_steering_now = driving ? g_cfg.pad_steering : 100;
    // HP2_VPADS: 0xC0, 0xC1 in the key script are A and RT on the second test pad, 0xC2, 0xC3 on the first
    if (g_key_script_on) {
        pad_virtual(1, g_script_key[0xC0], g_script_key[0xC1]);
        pad_virtual(0, g_script_key[0xC2], g_script_key[0xC3]);
    }
    PadState ps = pad_read(pad_slot(0));
    g_pad_playstation = g_cfg.pad_icons ? g_cfg.pad_icons == 2 : pad_is_playstation(pad_slot(0));
    g_layout_playstation = g_cfg.pad_icons ? g_cfg.pad_icons == 2 : !ps.ok || pad_is_playstation(pad_slot(0));
    if (ps.ok) {
        // a pad that is there when the game starts is the device, until a key says otherwise
        static bool first = true;
        if (first) g_input_kind = 1;
        first = false;
        // A new press or a stick pushed well over says the player has moved to the pad. One held says nothing: a drifting stick
        // or a pedal kept down must not hold the hints against the keyboard.
        static int pad_held;
        static bool stick_held;
        if (ps.mask & ~pad_held) g_input_kind = 1, g_mouse_at = 0;
        pad_held = ps.mask;
        for (int b = 0; b < PCB_COUNT; b++) {
            int m = (driving ? g_cfg.pad[b] : pc_menu_pad(b)) & ps.mask;
            if (!m) continue;
            // a trigger alone gives its pressure (gas, brake); the other buttons are all or nothing
            int amount = 0xFF;
            if (m == 0x20000) amount = ps.rt;
            else if (m == 0x10000) amount = ps.lt;
            press(b, amount);
        }
        bool pushed = false;
        for (int i = 0; i < 4; i++)
            if (ps.sticks[i] != 0x80) {
                sticks[i] = ps.sticks[i];
                if (ps.sticks[i] < 0x40 || ps.sticks[i] > 0xC0) pushed = true;
            }
        if (pushed && !stick_held) g_input_kind = 1, g_mouse_at = 0;
        stick_held = pushed;
    } else if (g_input_kind == 1) {
        g_input_kind = 0;       // the pad is gone
    }
    if (g_key_script_on && g_script_key[0xFF]) g_input_kind = 1;    // the script's "as if a gamepad were in use"
    if (g_key_script_on && g_script_key[0xFE]) {                     // and "as if that gamepad were a PlayStation's"
        g_input_kind = 1;
        g_pad_playstation = 1;
    }
    return buttons;
}

// Player 2: their gamepad, and the keyboard's other half when it is shared
u16 window_pad2(u8 sticks[4], u8 pressure[12])
{
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80;
    memset(pressure, 0, 12);
    if (!g_hwnd || g_cap || (!g_key_script_on && GetForegroundWindow() != g_hwnd)) return 0xFFFF;
    u16 buttons = 0xFFFF;
    auto press = [&](int b, int amount) {
        buttons &= ~(1u << kPadBit[b].bit);
        int p = kPadBit[b].pressure;
        if (p >= 0 && amount > pressure[p]) pressure[p] = (u8)amount;
    };
    const bool driving = pc_car_input();
    pc_keyboard_layout(1, driving && g_keys_shared == KEYS_SHARED_DRIVING);
    if (g_keys_shared == KEYS_SHARED_DRIVING) {
        for (int b = 0; b < PCB_COUNT; b++)
            if (key(g_cfg.key2[b][0]) || key(g_cfg.key2[b][1])) press(b, 0xFF);
        static int zone_held[2];
        for (int z = 0; z < 2; z++) zone_keys(key(g_cfg.key2_zone[z][0]) || key(g_cfg.key2_zone[z][1]), z, zone_held[z], press);
        if (key(VK_RETURN) && !key(VK_MENU) && pc_skip_wanted()) press(PCB_CROSS, 0xFF);    // as player 1 has it
    } else if (g_keys_shared == KEYS_SHARED_MENU) {
        for (int b = 0; b < PCB_COUNT; b++)
            if (key(pc_menu_key_split(1, b))) press(b, 0xFF);
        if (key(pc_menu_key_split(1, PCB_LEFT))) sticks[2] = 0x00;
        if (key(pc_menu_key_split(1, PCB_RIGHT))) sticks[2] = 0xFF;
        if (key(pc_menu_key_split(1, PCB_UP))) sticks[3] = 0x00;
        if (key(pc_menu_key_split(1, PCB_DOWN))) sticks[3] = 0xFF;
    }
    PadState ps = pad_read(pad_slot(1));
    if (ps.ok) {
        for (int b = 0; b < PCB_COUNT; b++) {
            int m = (driving ? g_cfg.pad[b] : pc_menu_pad(b)) & ps.mask;
            if (!m) continue;
            int amount = 0xFF;
            if (m == 0x20000) amount = ps.rt;
            else if (m == 0x10000) amount = ps.lt;
            press(b, amount);
        }
        for (int i = 0; i < 4; i++)
            if (ps.sticks[i] != 0x80) sticks[i] = ps.sticks[i];
    }
    return buttons;
}

// The process takes the screen's real pixels, whatever the screen's scaling: the window's client size is then the swapchain's on every
// driver, and the fullscreen window is the monitor's own size. Said here and not left to what else is in the process (a driver, an overlay,
// the system's compatibility settings): a window Windows scales by itself is drawn at another size than it has, and each driver takes that
// its own way. The functions by name: they are Windows 10's.
int window_dpi_aware()
{
    HMODULE user = GetModuleHandleA("user32.dll");
    typedef void *(WINAPI *GetContext)();
    typedef int (WINAPI *FromContext)(void *);
    typedef BOOL (WINAPI *SetContext)(void *);
    const auto get = (GetContext)(void *)GetProcAddress(user, "GetThreadDpiAwarenessContext");
    const auto from = (FromContext)(void *)GetProcAddress(user, "GetAwarenessFromDpiAwarenessContext");
    const auto set = (SetContext)(void *)GetProcAddress(user, "SetProcessDpiAwarenessContext");
    const int was = get && from ? from(get()) : -1;
    if (!set || !set((void *)(INT_PTR)-4))                  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (was < 1) SetProcessDPIAware();
    return was;
}

// HP2_DPI=0 | 1, for tests: the window's thread and the renderer's as in a process Windows scales (unaware, or aware of the system's scaling
// only), which is what the port was on a scaled screen before it declared itself. Called at the start of each of the two threads.
void window_dpi_thread()
{
    const char *e = getenv("HP2_DPI");
    if (!e) return;
    typedef void *(WINAPI *SetThreadContext)(void *);
    if (const auto set = (SetThreadContext)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext"))
        set((void *)(INT_PTR)(atoi(e) == 1 ? -2 : -1));
}

void window_start()
{
    std::thread([] {
        window_dpi_thread();
        if (getenv("HP2_CURSORSTART")) g_cursor_on = true, mouse_used();    // for tests: as if the mouse had just been used on a screen before
        WNDCLASSA wc = {};
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "hp2recomp";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));    // speed.rc
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassA(&wc);
        RECT r = {0, 0, g_cfg.win_w, g_cfg.win_h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_hwnd = CreateWindowA("hp2recomp", "Need for Speed: Hot Pursuit 2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                               nullptr, wc.hInstance, nullptr);
        if (g_cfg.fullscreen) set_fullscreen_now(true);
        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }).detach();
}
