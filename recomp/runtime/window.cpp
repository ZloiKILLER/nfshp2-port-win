// The Win32 window, the keyboard and the gamepads. The renderer draws to the window itself (native_vk.cpp).
#include "hw.h"
#include "pcconfig.h"
#include "pcwheel.h"
#include "phase.h"
#include "host.h"
#include "pcmenu.h"
#include "pad.h"
#include "spu2.h"
#include "audio_out.h"
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
static const UINT WM_SETMONITOR = WM_APP + 4;
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

// ---- the screen the game is shown on (Options > Graphics > Resolution > Monitor; the user, 2026-11-17) ----
// The screens in the order of Windows' own numbers (\\.\DISPLAY1 is 1), 1 and up. 0 in hp2.ini is "never chosen": the window is where
// Windows puts it and nothing is moved. The menu shows the screen the window is on, by its number and its name (2026-11-18: no
// "Automatic" and no size there).
struct Screen {
    RECT r;
    char dev[40];                           // \\.\DISPLAY1
    int number;
};
static BOOL CALLBACK monitor_each(HMONITOR m, HDC, LPRECT r, LPARAM to)
{
    Screen s = {*r, {}, 0};
    MONITORINFOEXA mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(m, (MONITORINFO *)&mi)) snprintf(s.dev, sizeof s.dev, "%s", mi.szDevice);
    size_t n = strlen(s.dev);
    while (n && s.dev[n - 1] >= '0' && s.dev[n - 1] <= '9') n--;
    s.number = atoi(s.dev + n);
    ((std::vector<Screen> *)to)->push_back(s);
    return TRUE;
}
static std::vector<Screen> monitors()
{
    std::vector<Screen> all;
    EnumDisplayMonitors(nullptr, nullptr, monitor_each, (LPARAM)&all);
    for (size_t i = 1; i < all.size(); i++)             // by the number, as they came where it is the same
        for (size_t k = i; k && all[k].number < all[k - 1].number; k--) std::swap(all[k], all[k - 1]);
    return all;
}
int window_monitor_count() { return (int)monitors().size(); }
void window_monitor_size(int index, int &w, int &h)
{
    const std::vector<Screen> all = monitors();
    w = h = 0;
    if (index >= 1 && index <= (int)all.size()) w = all[(size_t)index - 1].r.right - all[(size_t)index - 1].r.left, h = all[(size_t)index - 1].r.bottom - all[(size_t)index - 1].r.top;
}
// Windows' number of a screen (what its display settings call it); the place in the list where the name has none
int window_monitor_number(int index)
{
    const std::vector<Screen> all = monitors();
    if (index < 1 || index > (int)all.size()) return index;
    return all[(size_t)index - 1].number > 0 ? all[(size_t)index - 1].number : index;
}
int window_monitor_now()
{
    if (!g_hwnd) return 0;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi)) return 0;
    const std::vector<Screen> all = monitors();
    for (size_t i = 0; i < all.size(); i++)
        if (!strcmp(all[i].dev, mi.szDevice)) return (int)i + 1;
    return 0;
}
// the screen's name as its maker gave it: Windows has it by the path from the card's output to the screen
static std::string screen_name(const char *device)
{
    wchar_t dev[40];
    if (!MultiByteToWideChar(CP_ACP, 0, device, -1, dev, 40)) return {};
    UINT32 paths_n = 0, modes_n = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_n, &modes_n) != ERROR_SUCCESS || !paths_n) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(paths_n);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_n ? modes_n : 1);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_n, paths.data(), &modes_n, modes.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 i = 0; i < paths_n; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME from = {};
        from.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        from.header.size = sizeof from;
        from.header.adapterId = paths[i].sourceInfo.adapterId;
        from.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&from.header) != ERROR_SUCCESS || wcscmp(from.viewGdiDeviceName, dev)) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME to = {};
        to.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        to.header.size = sizeof to;
        to.header.adapterId = paths[i].targetInfo.adapterId;
        to.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&to.header) != ERROR_SUCCESS) return {};
        std::string name;                   // the menus' letters: plain ones, and no more than the row's value has room for
        for (const wchar_t *c = to.monitorFriendlyDeviceName; *c && name.size() < 18; c++)
            if (*c >= 32 && *c < 127) name += (char)*c;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        return name;
    }
    return {};
}
// (asked every frame the row is on the screen: Windows is asked again once in two seconds)
std::string window_monitor_name(int index)
{
    const std::vector<Screen> all = monitors();
    if (index < 1 || index > (int)all.size()) return {};
    static std::string dev, name;
    static ULONGLONG at;
    const ULONGLONG now = GetTickCount64();
    if (dev != all[(size_t)index - 1].dev || now - at > 2000) {
        dev = all[(size_t)index - 1].dev;
        name = screen_name(dev.c_str());
        at = now;
    }
    return name;
}
// on the window's thread: the window to the screen the setting names, in its middle; a fullscreen window over all of it
static void to_monitor_now()
{
    const std::vector<Screen> all = monitors();
    if (!g_hwnd || g_cfg.monitor < 1 || g_cfg.monitor > (int)all.size()) return;
    const RECT m = all[(size_t)g_cfg.monitor - 1].r;
    RECT r = {0, 0, g_cfg.win_w, g_cfg.win_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int spare_x = ((int)(m.right - m.left) - w) / 2, spare_y = ((int)(m.bottom - m.top) - h) / 2;
    const int x = m.left + (spare_x > 0 ? spare_x : 0), y = m.top + (spare_y > 0 ? spare_y : 0);
    if (g_fullscreen) {
        g_windowed_rect = {x, y, x + w, y + h};            // where it goes when it is a window again
        SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
    }
}
void window_set_monitor(int index)
{
    g_cfg.monitor = index;
    pc_config_save();
    g_pc_ui_dirty = true;
    if (g_hwnd) PostMessageA(g_hwnd, WM_SETMONITOR, 0, 0);
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
        // the monitor's, or the size chosen for fullscreen where the monitor has room for it
        window_screen_size(w, h);
        if (g_cfg.fs_w > 0 && g_cfg.fs_h > 0 && g_cfg.fs_w <= w && g_cfg.fs_h <= h) w = g_cfg.fs_w, h = g_cfg.fs_h;
    } else {
        w = g_cfg.win_w;
        h = g_cfg.win_h;
    }
}

void window_relaunch(const std::string &profile, bool no_intro)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    SetEnvironmentVariableA("HP2_PROFILE", profile.c_str());
    // The page to come back to, and where the window stands: the new one is to look as if nothing but the switch had happened
    SetEnvironmentVariableA("HP2_NOINTRO", no_intro ? "1" : nullptr);
    SetEnvironmentVariableA("HP2_WINPOS", nullptr);
    if (g_hwnd && !g_fullscreen && no_intro) {
        RECT r;
        if (GetWindowRect(g_hwnd, &r)) {
            char at[48];
            snprintf(at, sizeof at, "%ld,%ld", r.left, r.top);
            SetEnvironmentVariableA("HP2_WINPOS", at);
        }
    }
    // The new one starts as from the desktop: no standard handles (this one's are the log's pipe, whose numbers mean nothing there), and
    // speed.log free for it to open.
    host_log_close();
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);       // its window takes this one's place in front
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    SetEnvironmentVariableA("HP2_PROFILE", nullptr);
    SetEnvironmentVariableA("HP2_NOINTRO", nullptr);
    SetEnvironmentVariableA("HP2_WINPOS", nullptr);
}

void pc_save_on_exit();
void pc_wheel_quit();

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

// After an answer (typing ended, a key bound) the game sees nothing until what gave it is let go, or the key just bound would also
// move the menu (window_pad): 1 asked, all that is down at that moment (typing ended); 2 asked, the one key or the buttons named
// here (a capture's answer); 3 waiting for them. Never longer than a second and a half: what stays down for good is given up.
static std::atomic<int> g_release;
static int g_release_vk, g_release_pad;

void window_text_end()
{
    g_text_on = false;
    g_release = 1;
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
// The mouse moved, and not by a tremble (WM_MOUSEMOVE: six pixels from where it last counted). In the menus that says the player
// has gone to the keyboard and the mouse, as a click or a key does (the user, 2026-11-25: "when the mouse is what is used, let
// the game take it as the keyboard and switch to the keyboard"). window_pad reads it, where it is known whether the car is
// being driven: in a race the mouse does nothing, and a mouse nudged there must not hand a pad's race to the keyboard's layout.
static std::atomic<bool> g_mouse_moved;

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
        static int from_x, from_y;
        const int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        if (lp != last) {
            if (last != -1) {
                mouse_used();
                if (abs(mx - from_x) + abs(my - from_y) >= 6) g_mouse_moved = true, from_x = mx, from_y = my;
            } else {
                from_x = mx, from_y = my;
            }
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
    case WM_SETMONITOR:
        to_monitor_now();
        return 0;
    case WM_CURSORSTATE:
        apply_cursor(h);
        return 0;
    case WM_DEVICECHANGE:
        pc_wheel_devices_changed();         // a steering wheel plugged in or taken out (pcwheel.h)
        break;
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
        for (int slot = 0; slot < PAD_DEVICES; slot++) pad_triggers(slot, 0, 0), pad_speaker_route(slot, false);    // and a DualSense's triggers and its speaker's routing stay as told
        pad_settle(150);                // the pads' thread tells them
        pc_wheel_quit();                // a wheel's rotation the game set goes back to what it was
        pc_save_on_exit();              // the profile's save, with what was chosen in the menus since the game's last one
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
    // HP2_KEYTIME=1: the script's count goes by sixtieths of a second, not by pad reads: in a race at 30 steps a second a read is
    // worth two. Without it the same script presses its keys at other moments of a race at 30 than at 60 (the car starts later
    // and is driven twice as long), and two runs that are meant to differ in the frame rate alone differ in the driving too.
    static const bool by_time = getenv("HP2_KEYTIME") != nullptr;
    static long t = -1;
    t += by_time && pc_racing() && pc_world_rate() == 30 ? 2 : 1;
    memset(g_script_key, 0, sizeof g_script_key);
    for (auto &s : steps)
        if (t >= s.t && t < s.t + s.hold) g_script_key[s.vk & 255] = true;
}

static bool g_shift_with_win[4];          // VK_SHIFT 0x10, VK_LSHIFT 0xA0, VK_RSHIFT 0xA1 by their low two bits: 0, 0, 1
static bool key(int vk)
{
    if (g_key_script_on) return vk && g_script_key[vk & 255];
    if (!vk || !(GetAsyncKeyState(vk) & 0x8000)) {
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shift_with_win[vk & 3] = false;
        return false;
    }
    // A Shift pressed while a Windows key is down is Windows' shortcut, not a key of the game's: some keyboards' own keys send
    // such a shortcut (the user's Logitech K380, 2026-11-27: its screenshot key, Win+Shift+S, gave the speed boost, which is Left
    // Shift). It stays no key until it is let go, though the Windows key goes up first.
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        bool &latched = g_shift_with_win[vk & 3];
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) latched = true;
        if (latched) return false;
    }
    return true;
}

bool window_key_down(int vk)
{
    if (!g_key_script_on && (!g_hwnd || GetForegroundWindow() != g_hwnd)) return false;
    return key(vk);
}

// each PcButton: its bit in the button word, and its pressure byte (-1: none)
static const struct { int bit, pressure; } kPadBit[PCB_COUNT] = {
    {4, 2}, {6, 3}, {7, 1}, {5, 0},         // up down left right
    {14, 6}, {15, 7}, {13, 5}, {12, 4},     // cross square circle triangle
    {10, 8}, {11, 9}, {8, 10}, {9, 11},     // L1 R1 L2 R2
    {2, -1}, {3, -1}, {0, -1}};             // R3 start select

// Which gamepad a player has (the game's Controller screen, Select Device; the user, 2026-11-23). hp2.ini [gamepad] gamepad1 /
// gamepad2: nothing is Automatic, the default (the user: "auto-detect no longer works, can it come back?" - a choice by name
// held a player to one gamepad, and another one plugged in was nobody's): player 1 has the first gamepad plugged in and player
// 2 the next, so with one gamepad player 2 has none, and a gamepad that comes is taken at once. "none": no gamepad, by choice.
// A name (pad_device_name): that gamepad, and none while it is not plugged in. A gamepad one player has by name is not the
// other's by Automatic. -1: none (pad.h).
static int device_by_name(const char *want)
{
    for (int slot = 0; slot < PAD_DEVICES; slot++)
        if (pad_device_name(slot) == want) return slot;
    return -1;
}
static int pad_slot(int port)
{
    port &= 1;
    const char *mine = g_cfg.pad_device[port], *theirs = g_cfg.pad_device[1 - port];
    if (!strcmp(mine, "none")) return -1;
    if (mine[0]) return device_by_name(mine);
    const bool they_named = theirs[0] && strcmp(theirs, "none");
    const int taken = they_named ? device_by_name(theirs) : -1;
    int skip = port == 1 && !theirs[0] ? 1 : 0;         // both Automatic: the second is player 2's
    for (int slot = 0; slot < PAD_DEVICES; slot++)
        if (slot != taken && pad_read(slot).ok && skip-- == 0) return slot;
    return -1;
}

// Select Device (pcmenu.cpp): a player's choice as hp2.ini has it ("" Automatic, "none", or a gamepad's name); the choice after
// one, left or right: Automatic, None, then each gamepad that is plugged in; and a choice made. One player's choice alone is
// changed, unless it is the gamepad the other has by name: the same one is never both players', so the other gets what this
// one had.
std::string window_pad_value(int port) { return g_cfg.pad_device[port & 1]; }
std::string window_pad_step(const std::string &from, int dir)
{
    std::string list[PAD_DEVICES + 2] = {"", "none"};
    int n = 2;
    for (int slot = 0; slot < PAD_DEVICES; slot++)
        if (pad_read(slot).ok) list[n++] = pad_device_name(slot);
    int at = 0;                                         // a gamepad chosen and not plugged in: from Automatic on
    for (int i = 0; i < n; i++)
        if (list[i] == from) at = i;
    return list[(at + (dir < 0 ? n - 1 : 1)) % n];
}
void window_pad_set(int port, const std::string &value)
{
    port &= 1;
    const std::string mine = g_cfg.pad_device[port], theirs = g_cfg.pad_device[1 - port];
    if (!value.empty() && value != "none" && value == theirs) snprintf(g_cfg.pad_device[1 - port], sizeof g_cfg.pad_device[0], "%s", mine.c_str());
    snprintf(g_cfg.pad_device[port], sizeof g_cfg.pad_device[0], "%s", value.c_str());
}

// The gamepad whose buttons the hints show (pc_paint_buttons: Xbox's or PlayStation's) and that last said "a gamepad is in use":
// the one pressed last. Player 1's, and in the menus player 2's too, which the menus hear as well: with the one gamepad given
// to player 2 the hints never left the keyboard's (the user, 2026-11-23: "the keyboard and the gamepad no longer switch").
static int g_hint_slot = -1;

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

// A player's gamepad is in use now: a button down, a trigger, a stick out of its dead zone (pcwheel.cpp: the wheel and the gamepad of
// one player, who has the car)
static bool g_pad_busy[2];
static bool pad_busy(const PadState &ps)
{
    return ps.ok && (ps.mask || ps.sticks[0] != 0x80 || ps.sticks[1] != 0x80 || ps.sticks[2] != 0x80 || ps.sticks[3] != 0x80);
}
bool window_pad_busy(int port) { return g_pad_busy[port & 1]; }

// The right stick of a player's gamepad as it is now, each way -1..1, right and up above 0 (past the pad's dead zone, as the
// game gets it); false and (0, 0) with no gamepad. For the camera of a layout of the player's own (pcplatform.cpp).
bool window_right_stick(int port, float &x, float &y)
{
    const PadState ps = pad_read(pad_slot(port & 1), port & 1);
    x = y = 0.0f;
    if (!ps.ok) return false;
    x = ((int)ps.sticks[0] - 0x80) / 127.0f;
    y = (0x80 - (int)ps.sticks[1]) / 127.0f;
    x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
    return true;
}

// (the player's own strength of it, pcconfig.h: the large motor's by the percent, the small one on or off)
static bool g_rumble_own[2];
void window_rumble_port(int port, int small, int large)
{
    if (g_rumble_own[port & 1]) return;             // (the port works the motors out itself for this player)
    const int percent = g_cfg.pad_vibration[port & 1];
    large = large * percent / 100;
    pad_rumble(pad_slot(port), percent && small ? 255 : 0, large > 255 ? 255 : large);
}
void window_rumble_owner(int port, bool own) { g_rumble_own[port & 1] = own; }
void window_rumble_own(int port, int small, int large)
{
    const int percent = g_cfg.pad_vibration[port & 1];
    large = large * percent / 100;
    small = small * percent / 100;
    pad_rumble(pad_slot(port), small > 255 ? 255 : small, large > 255 ? 255 : large);
}
void window_rumble(int small, int large)
{
    pad_rumble(0, small, large);
    pad_rumble(1, small, large);
}

bool window_pad_dualsense(int port) { return pad_is_dualsense(pad_slot(port)); }

bool window_pad_speaker(int port) { return pad_speaker(pad_slot(port)) != 0; }
// The speech leaves the mix only while the speaker's own sound output is open and playing (audio_radio_open): with the output
// not there (switched off in Windows, the cable just pulled) the radio stays in the speakers.
void window_radio_frame()
{
    int target = -1, player = 0;
    for (int port = 0; port < 2 && target < 0; port++) {
        const int slot = pad_slot(port);
        if (g_cfg.pad_radio[port] && slot >= 0 && pad_speaker(slot)) target = slot, player = port;
    }
    static int was = -2;
    static std::string path;
    if (target != was) {
        was = target;
        path = target >= 0 ? pad_speaker_path(target) : std::string();
    }
    audio_radio_target(path, target >= 0 && pad_speaker(target) == 1, spu2_radio_read);
    spu2_radio_sink(target >= 0 && audio_radio_open(), g_cfg.pad_radio_volume[player]);
    for (int s = 0; s < PAD_DEVICES; s++) pad_speaker_route(s, s == target);
}
void window_triggers(const unsigned want[2][2])
{
    const int slot[2] = {pad_slot(0), pad_slot(1)};
    for (int s = 0; s < PAD_DEVICES; s++) {
        const int player = s == slot[0] ? 0 : s == slot[1] ? 1 : -1;
        const bool on = player >= 0 && g_cfg.pad_adaptive[player];
        pad_triggers(s, on ? want[player][0] : 0, on ? want[player][1] : 0);
    }
}

// The keyboard is shared when there are two players and not a gamepad for each: player 2 has key2 and a player's keys are only their own.
// Elsewhere player 1 has all of it, the same as with no second player in the world. Set once a frame by window_pad, read by window_pad2.
enum { KEYS_ALONE, KEYS_SHARED_DRIVING, KEYS_SHARED_MENU };
static int g_keys_shared;

// One keyboard for two players: in a race while either of them drives (a tester, 0.14: one player finished and the other "couldn't
// move"; with the first player's mode alone deciding, the second lost their half of the keyboard when the first finished first).
static int keys_shared(bool driving, bool driving2)
{
    // player 2 has their keys by choice (Controls > Player 2 Controls), or because they have no gamepad (Select Device)
    if (!g_cfg.player2_keyboard && pad_slot(1) >= 0) return KEYS_ALONE;
    if ((driving || driving2) && pc_two_player_race()) return KEYS_SHARED_DRIVING;
    // In the menus player 2's keys are the second pad wherever the game hears one (the user, 2026-11-23, after the listing: on
    // the PS2 the second pad moves through nearly every menu, has its own rows on the Controller screen, its own name, its own
    // half of a two-player car select). With player 2 on the keyboard by choice that is every menu outside a race; otherwise,
    // as before, the screens where both choose at once. The keyboard is then in two: pc_menu_key_split.
    if (!driving && (pc_two_player_screen() || (g_cfg.player2_keyboard && !pc_racing()))) return KEYS_SHARED_MENU;
    return KEYS_ALONE;
}

static bool is_key2(int vk)
{
    for (auto &k : g_cfg.key2)
        if (vk && (k[0] == vk || k[1] == vk)) return true;
    return false;
}

// ---- binding a key or a pad button from the menu ------------------------------------
// window_capture_begin(): the game sees no input, and the next key or pad button that goes down is the answer (Esc cancels).
// What is down as the capture begins is no answer until it has been let go and pressed again: the key that chose the row, and
// whatever reads as held for good. (Until 2026-11-24 the capture first waited for every key and every button of both gamepads to
// be up, and then took the first one down. Anything that never read as up, a button of some device Windows shows as a gamepad,
// a key a driver holds, left it waiting for ever with the game's input taken away: the user, "the binding does not change,
// nothing gets pressed and the game loses the keyboard". And it ran from the game's read of the pad alone.) Its steps are the
// menu's frames now (window_capture_frame, called by the page that asked), so it does not depend on the game reading a pad.
static std::atomic<int> g_cap;          // 0 off, 1 just begun, 2 waiting for a press, 3 done, 4 cancelled
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

void window_capture_frame()
{
    static bool was[256], since[256];       // down at the step before; down ever since the capture began
    static int was_pad, since_pad;
    static ULONGLONG began;
    static bool told;
    if (g_cap != 1 && g_cap != 2) return;
    if (!g_hwnd || (!g_key_script_on && GetForegroundWindow() != g_hwnd)) return;       // another window's keys are no answers
    const PadState ps = pad_read(0), ps2 = pad_read(1);
    bool now[256] = {};
    // (the sided Shift, Ctrl and Alt are bindable, the generic ones are the same key twice; not the test script's two marks)
    for (int vk = 8; vk < 0xFE; vk++) now[vk] = vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && key(vk);
    const int pad_now = (ps.ok ? ps.mask : 0) | (ps2.ok ? ps2.mask : 0);
    if (g_cap == 1) {
        memcpy(was, now, sizeof was);
        memcpy(since, now, sizeof since);
        was_pad = since_pad = pad_now;
        began = GetTickCount64();
        told = false;
        g_cap = 2;
        return;
    }
    int pressed_key = 0, pressed_pad = pad_now & ~was_pad;
    for (int vk = 0xFD; vk >= 8; vk--)
        if (now[vk] && !was[vk]) pressed_key = vk;      // the lowest of those that went down
    if (now[VK_ESCAPE] && !was[VK_ESCAPE]) pressed_key = VK_ESCAPE;
    for (int vk = 8; vk < 0xFE; vk++) since[vk] = since[vk] && now[vk];
    since_pad &= pad_now;
    memcpy(was, now, sizeof was);
    was_pad = pad_now;
    // what has been down all along, said once (speed.log): it is no answer until it is let go
    if (!told && GetTickCount64() - began > 3000) {
        told = true;
        char held[256] = "";
        int at = 0;
        for (int vk = 8; vk < 0xFE && at < 200; vk++)
            if (since[vk]) at += snprintf(held + at, sizeof held - (size_t)at, " key 0x%02X", vk);
        if (since_pad && at < 200) at += snprintf(held + at, sizeof held - (size_t)at, " gamepad buttons 0x%X", since_pad);
        if (at) fprintf(stderr, "keys: waiting for a key to bind; down since before it was asked, and so not taken:%s\n", held);
    }
    // HP2_CAPTEST=<virtual key>: the test press, a few frames after the capture began
    static const char *test = getenv("HP2_CAPTEST");
    static int waited;
    if (test && ++waited > 30) pressed_key = atoi(test), waited = 0;
    if (pressed_key == VK_ESCAPE) {
        g_release_vk = VK_ESCAPE, g_release_pad = 0;
        g_release = 2;
        g_cap = 4;
        return;
    }
    if (g_cap_what == 2) pressed_key = 0;
    if (g_cap_what == 1) pressed_pad = 0;
    if (pressed_key) {
        g_cap_pad = 0;
        g_cap_code = pressed_key;
        g_release_vk = pressed_key, g_release_pad = 0;
    } else if (pressed_pad) {
        g_cap_pad = 1;
        g_cap_code = pressed_pad & -pressed_pad;      // the lowest button of those down
        g_release_vk = 0, g_release_pad = g_cap_code;
    } else {
        return;
    }
    g_release = 2;
    g_cap = 3;
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
// For a cop the same two buttons are the calls for the helicopter and for a road block (Player::RacingJoyHandler, events 129 and 128),
// and there the key is a short press, whatever the finger does: the game changes the camera's view when Triangle is let go, and
// keeps that from following a call for one second only (Player +152, the time of the call). A key held longer than that, as a
// single key easily is, changed the view on its way up. Six reads and both buttons are up again, well inside the second.
// The keyboard's driving keys as the game's actions (pcactions.cpp, pc_actions_port): read here, where the keys always were, and
// taken by the scan that follows the pad's in the same frame. Off, and so all up, on every way out of the pad reads that is not
// a player driving on the keys.
static bool g_act_on[2], g_act_key[2][PCA_COUNT];
static bool g_esc_down;                 // Esc went for the pause this frame (window_pad), for the player who has the keys
bool window_actions(int port, bool keys[])
{
    port &= 1;
    for (int a = 0; a < PCA_COUNT; a++) keys[a] = g_act_on[port] && g_act_key[port][a];
    return g_act_on[port];
}

// The pause menu and a gamepad's Back (B, Circle: 0x2000 of the pad's mask; the menus' Triangle, pc_menu_pad). The game's pause
// menu refuses Back (PauseMenu's callback: "Triangle refused silently") and is closed by Start alone, so on a gamepad the
// button every other menu goes back with did nothing there (the user, 2026-11-23: "the second gamepad does not leave the pause
// menu by Circle or its Xbox twin, only by Start"; the first one did not either). There it is Start: the pause is closed, as
// Esc closes it. Not with a question over the menu (restart? quit?), where Back is the question's No. The button that closed
// the pause is then not the race's button it also is (the horn) until it is let go.
// What a press of Back is, is settled as the button goes down (BackPress: that pad's, kept by the caller): only one that went
// down with the pause menu up is its Start. Back held from a page under the pause menu (the Debug menu, the options) took the
// player back to the pause menu and then, still down, closed it too (the user, 2026-11-24, of Esc on the Debug menu: "pressed
// twice, straight on to the race"); such a Back is nothing on the pause menu until it is let go. `starts`: press Start for it.
struct BackPress {
    bool was = false, closes = false;
};
static int pause_back_mask(int mask, BackPress &s, bool &starts)
{
    const bool on_pause = pc_pause_menu() && !pc_popup_up();
    starts = false;
    if (!(mask & 0x2000)) {
        s = BackPress();
        return mask;
    }
    if (!s.was) s.closes = on_pause;
    s.was = true;
    if (s.closes && on_pause) {
        starts = true;
        return mask;
    }
    // it closed the pause: not the race's button it also is (the horn); or it came down elsewhere: not the pause menu's
    return s.closes || on_pause ? mask & ~0x2000 : mask;
}

// Extras' Slow Motion on a gamepad: both sticks pressed together, in every layout (the user, 2026-11-24). No layout of the game
// has a button for the slow motion (JOY_EVENT_ZONE_SLOMO, 97), so the port sends the event itself for these two (pcactions.cpp
// asks window_pad_slomo). The left stick's press (0x40) is nothing in the port; the right one's (0x80) is the boost. So that
// the two together are not a boost as well: once both are down neither reaches the game until both are let go, and the right
// one alone is held back for 70 ms, in case the left one is on its way (a tap shorter than that is given once it is over).
// Only while this player drives and the switch is on: otherwise the buttons are as ever. And never in a race of two players:
// the game has no zones there (Player::RequestZone, 0x177dc8: two players, refused, before anything is granted; and
// World::SetTimeScale is called from RequestZone alone), so there the sticks are left alone too and the event is not sent
// (the user, 2026-11-24: "make sure it will not work on the split screen even with the switch on").
struct SlowCombo {
    bool combo = false, passed = false;
    ULONGLONG since = 0;
    int tap = 0;
};
static bool g_pad_slomo[2], g_pad_slomo_valid[2];
bool window_pad_slomo(int port, bool &valid)
{
    valid = g_pad_slomo_valid[port & 1];
    return g_pad_slomo[port & 1];
}
static int slow_combo(int mask, SlowCombo &s, int port, bool driving)
{
    g_pad_slomo[port] = false;
    g_pad_slomo_valid[port] = driving && g_cfg.slow_motion && !pc_two_player_race();
    if (!g_pad_slomo_valid[port]) {
        s = SlowCombo();
        return mask;
    }
    const bool l3 = (mask & 0x40) != 0, r3 = (mask & 0x80) != 0;
    if (s.combo) {
        if (!l3 && !r3) s.combo = false;
        g_pad_slomo[port] = l3 && r3;
        return mask & ~0xC0;
    }
    if (l3 && r3) {
        s = SlowCombo();
        s.combo = true;
        g_pad_slomo[port] = true;
        return mask & ~0xC0;
    }
    if (r3 && !l3) {
        if (s.passed) return mask;
        const ULONGLONG now = GetTickCount64();
        if (!s.since) s.since = now;
        if (now - s.since >= 70) {
            s.passed = true;
            return mask;
        }
        return mask & ~0x80;
    }
    if (s.since && !s.passed && !l3) s.tap = 3;     // a short press of the right stick that was held back: now
    s.since = 0;
    s.passed = false;
    if (s.tap > 0 && !l3) {
        s.tap--;
        return mask | 0x80;
    }
    s.tap = 0;
    return mask;
}

template <class Press> static void zone_keys(bool down, int which, int &held, Press &press)
{
    if (!down) {
        held = 0;
        return;
    }
    if (pc_cop_mode() && held >= 6) return;
    press(PCB_TRIANGLE, 0xFF);
    if (++held > 2) press(which ? PCB_R2 : PCB_L2, 0xFF);
}

int native_turntest_pad();     // native.cpp

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
    g_act_on[0] = false;
    g_pad_slomo[0] = g_pad_slomo_valid[0] = false;
    g_esc_down = false;
    // A pad shakes for the player who holds it: with one player, while the gamepad is the device used last (driving on the keyboard
    // leaves a pad on the desk still) or has something held on it now; with two, each has their own. (The device used last
    // alone was not enough: a key or a click in a race driven on the pad made it the keyboard's until the next new press, and a
    // trigger held and a stick moved a little are no new press: the pad was still, and came alive at the press of Start.)
    pad_rumble_gate(pad_slot(0), pc_two_player_race() || g_input_kind == 1 || g_pad_busy[0]);
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
    if (g_cap && getenv("HP2_CAPTEST")) return 0xFFFF;
    key_script_step();
    if (!g_hwnd || (!g_key_script_on && GetForegroundWindow() != g_hwnd)) return 0xFFFF;
    static bool esc_held;                  // from cancelling a capture: not a press yet
    if (g_cap) {                           // a key is being bound (window_capture_frame): nothing of it is the game's
        esc_held = true;
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
    if (g_release) {
        static bool waits[256];
        static int waits_pad;
        static ULONGLONG until;
        const PadState a = pad_read(0), b = pad_read(1);
        const int pad_now = (a.ok ? a.mask : 0) | (b.ok ? b.mask : 0);
        int asked = g_release;
        if (asked != 3 && g_release.compare_exchange_strong(asked, 3)) {
            // (not the test script's two "as if a gamepad" marks)
            for (int vk = 8; vk < 0xFE; vk++) waits[vk] = (asked == 1 || vk == g_release_vk) && key(vk);
            waits_pad = asked == 1 ? pad_now : pad_now & g_release_pad;
            until = GetTickCount64() + 1500;
        }
        bool held = false;
        for (int vk = 8; vk < 0xFE; vk++) held |= (waits[vk] = waits[vk] && key(vk));
        waits_pad &= pad_now;
        if ((held || waits_pad) && GetTickCount64() < until) return 0xFFFF;
        int waiting = 3;
        g_release.compare_exchange_strong(waiting, 0);
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
            g_cfg.pictures = 0;
            fprintf(stderr, "speedlog: F9, frame rate %d\n", g_cfg.frame_rate);
        }
        f9_held = f9;
    }
    // and F10 the draw distance, for comparing the same place
    static bool f10_held;
    if (frame_key) {
        const bool f10 = key(VK_F10);
        if (f10 && !f10_held) {
            g_cfg.draw_distance = g_cfg.draw_distance >= 600 ? 100 : g_cfg.draw_distance >= 400 ? 600 : g_cfg.draw_distance * 2;
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
    bool driving = pc_car_input_port(0);
    if (g_mouse_moved.exchange(false) && !driving) g_input_kind = 0;       // the mouse is in use in the menus: the keyboard's hints
    window_cursor_frame();
    auto key_on = [&](int vk) {
        if (!key(vk) || (alt && vk == VK_RETURN)) return false;
        if (g_key_script_on) g_input_kind = 0;      // the real keyboard says so in wndproc
        return true;
    };
    g_keys_shared = keys_shared(driving, pc_car_input_port(1));
    // The keys as the game's actions: nothing of the keyboard goes through the pad then. The layout is still the keyboard's (A)
    // while the keyboard is the device in use: the game picks the buttons it shows in its hints by the layout, and the port
    // paints each with the key of what that button does in A.
    const bool actions = driving && pc_actions_port(0);
    pc_keyboard_layout(0, driving && g_input_kind != 1);
    const bool shared_driving = g_keys_shared == KEYS_SHARED_DRIVING, shared_menu = g_keys_shared == KEYS_SHARED_MENU;
    auto drive_on = [&](int vk) { return !(shared_driving && is_key2(vk)) && key_on(vk); };
    auto menu_key = [&](int b) { return shared_menu ? pc_menu_key_split(0, b) : pc_menu_key(b); };
    bool enter = !shared_menu && key_on(VK_RETURN);
    if (actions) {
        g_act_on[0] = true;
        bool *a = g_act_key[0];
        memset(a, 0, sizeof g_act_key[0]);
        for (int b = 0; b < PCB_COUNT; b++) a[b] = drive_on(g_cfg.key[b][0]) || drive_on(g_cfg.key[b][1]);
        for (int z = 0; z < 3; z++) a[PCA_ZONE_FREEZE + z] = drive_on(g_cfg.key_zone[z][0]) || drive_on(g_cfg.key_zone[z][1]);
        a[PCA_SKIP] = enter && pc_skip_wanted();        // Enter where the game waits to go on (before the start)
    } else if (driving) {
        // a key is an action, pressed as the button that has it in the keyboard's own layout (pc_keyboard_layout)
        for (int b = 0; b < PCB_COUNT; b++)
            if (drive_on(g_cfg.key[b][0]) || drive_on(g_cfg.key[b][1])) press(b, 0xFF);
        static int zone_held[2];
        for (int z = 0; z < 2; z++) zone_keys(drive_on(g_cfg.key_zone[z][0]) || drive_on(g_cfg.key_zone[z][1]), z, zone_held[z], press);
        if (const int hands = native_turntest_pad()) {      // TEMP HP2_TURNTEST: the port's own hands
            if (hands & 1) press(PCB_CROSS, 0xFF);
            if (hands & 2) press(PCB_LEFT, 0xFF);
        }
        // Steering is the two keys' buttons alone (the D-pad's Left and Right, JOY_EVENT_STEER): the game's own digital steering, which
        // turns the wheel at its own rate. The stick is left in the middle: pushed to its end with the key, as it used to be, it was the
        // analog steering at full lock at once.
        // Enter is no key of the car's: in the race it does nothing (it was Cross, the throttle or the brake by the gamepad's layout).
        // Where the game waits to go on, before the start and after the finish, it is the button waited for.
        if (enter && pc_skip_wanted()) press(PCB_CROSS, 0xFF);
    } else {
        // (this player has finished and the other still drives on the same keyboard: the other's keys are no menu keys of this one's)
        auto menu_on = [&](int b) { return drive_on(menu_key(b)); };
        for (int b = 0; b < PCB_COUNT; b++)
            if (b != PCB_TRIANGLE && b != PCB_START && menu_on(b)) press(b, 0xFF);
        if (menu_on(PCB_LEFT)) sticks[2] = 0x00;
        if (menu_on(PCB_RIGHT)) sticks[2] = 0xFF;
        if (menu_on(PCB_UP)) sticks[3] = 0x00;
        if (menu_on(PCB_DOWN)) sticks[3] = 0xFF;
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
    // What a press of Esc is, is settled as the key goes down, and while it is held it is only what it was then and still is: a
    // key held is otherwise read anew on every screen that comes up under it. (The user, 2026-11-24: "the way out of the Debug
    // menu is pressed twice and the game goes straight on to the race: it should be the pause menu first". Esc there is Back; the
    // pause menu came up with Esc still down, and on the pause menu Esc is Start, which had just then gone down: the race.)
    static bool esc_was;
    static int esc_is;                      // bits: 1 the pause as the action, 2 Start, 4 Triangle
    const bool esc = !esc_held && key_on(VK_ESCAPE);
    if (esc) {
        // (the pause menu can be up with the mode of driving, opened while the zone camera was on: Start closes it there too)
        // Not on a page under the pause menu (the user, 2026-11-23: Esc there is a step back, not the race again). The menus take one
        // press a frame (FlailerScreenList::JoyHandler), and Start came before Triangle: on every pause page Start resumes the race
        // (message 13), on the developers' Debug menu it was a Select of the row. There Esc is Triangle alone, the page's own Back.
        g_esc_down = true;
        int now = 0;
        if (actions) now |= 1;              // the pause, as the action it is (JOY_EVENT_PAUSE)
        else if (pc_racing() && !pc_pause_subpage() && !pc_popup_up() && (!pc_race_menu() || pc_pause_menu())) now |= 2;
        if (!driving) now |= 4;
        if (!esc_was) esc_is = now;
        now &= esc_is;
        if (now & 1) g_act_key[0][PCA_PAUSE] = true;
        if (now & 2) press(PCB_START, 0xFF);
        if (now & 4) press(PCB_TRIANGLE, 0xFF);
    }
    esc_was = esc;
    g_pad_driving_now = driving ? 1 : 0;
    // HP2_VPADS: 0xC0, 0xC1 in the key script are A and RT on the second test pad, 0xC2, 0xC3 on the first
    if (g_key_script_on) {
        pad_virtual(1, g_script_key[0xC0], g_script_key[0xC1]);
        pad_virtual(0, g_script_key[0xC2], g_script_key[0xC3]);
    }
    PadState ps = pad_read(pad_slot(0), 0);
    g_pad_busy[0] = pad_busy(ps);
    if (!pad_read(g_hint_slot).ok) g_hint_slot = pad_slot(0);       // the gamepad last pressed has gone: player 1's
    g_pad_playstation = g_cfg.pad_icons ? g_cfg.pad_icons == 2 : pad_is_playstation(g_hint_slot);
    g_pad_nintendo = g_cfg.pad_icons ? g_cfg.pad_icons == 3 : pad_kind(g_hint_slot) == 2;
    g_layout_playstation = g_cfg.pad_icons ? g_cfg.pad_icons == 2 : !pad_read(g_hint_slot).ok || pad_is_playstation(g_hint_slot);
    if (ps.ok) {
        // a pad that is there when the game starts is the device, until a key says otherwise
        static bool first = true;
        if (first) g_input_kind = 1;
        first = false;
        // A new press or a stick pushed well over says the player has moved to the pad. One held says nothing: a drifting stick
        // or a pedal kept down must not hold the hints against the keyboard.
        static int pad_held;
        static bool stick_held;
        if (ps.mask & ~pad_held) g_input_kind = 1, g_mouse_at = 0, g_hint_slot = pad_slot(0);
        pad_held = ps.mask;
        static BackPress back;
        bool back_starts;
        static SlowCombo slow;
        const int mask = slow_combo(pause_back_mask(ps.mask, back, back_starts), slow, 0, driving);
        const bool back_closes = !driving && back_starts;
        for (int b = 0; b < PCB_COUNT; b++) {
            int m = (driving ? g_cfg.pad[b] : pc_menu_pad(b)) & mask;
            if (!m) continue;
            if (back_closes && b == PCB_TRIANGLE) {     // Back on the pause menu: Start, which closes it
                press(PCB_START, 0xFF);
                continue;
            }
            // a trigger alone gives its pressure (gas, brake); the other buttons are all or nothing
            int amount = 0xFF;
            if (m == 0x20000) amount = ps.rt;
            else if (m == 0x10000) amount = ps.lt;
            press(b, amount);
        }
        // (the left stick's press, 0x40: nothing in the game's own layouts on a PC pad, and the pad's L3 for a layout of the
        // player's own, which may give it a thing: pcplatform.cpp)
        if (driving && (mask & 0x40) && pc_custom_port(0)) buttons &= (u16)~(1u << 1);
        bool pushed = false;
        for (int i = 0; i < 4; i++)
            if (ps.sticks[i] != 0x80) {
                sticks[i] = ps.sticks[i];
                if (ps.sticks[i] < 0x40 || ps.sticks[i] > 0xC0) pushed = true;
            }
        if (pushed && !stick_held) g_input_kind = 1, g_mouse_at = 0, g_hint_slot = pad_slot(0);
        stick_held = pushed;
    } else if (g_input_kind == 1 && !pad_read(g_hint_slot).ok) {
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
    g_act_on[1] = false;
    g_pad_slomo[1] = g_pad_slomo_valid[1] = false;
    if (!g_hwnd || g_cap || (!g_key_script_on && GetForegroundWindow() != g_hwnd)) return 0xFFFF;
    u16 buttons = 0xFFFF;
    auto press = [&](int b, int amount) {
        buttons &= ~(1u << kPadBit[b].bit);
        int p = kPadBit[b].pressure;
        if (p >= 0 && amount > pressure[p]) pressure[p] = (u8)amount;
    };
    const bool driving = pc_car_input_port(1);
    const bool actions = driving && g_keys_shared == KEYS_SHARED_DRIVING && pc_actions_port(1);
    pc_keyboard_layout(1, driving && g_keys_shared == KEYS_SHARED_DRIVING);
    if (actions) {
        g_act_on[1] = true;
        bool *a = g_act_key[1];
        memset(a, 0, sizeof g_act_key[1]);
        for (int b = 0; b < PCB_COUNT; b++) a[b] = key(g_cfg.key2[b][0]) || key(g_cfg.key2[b][1]);
        for (int z = 0; z < 3; z++) a[PCA_ZONE_FREEZE + z] = key(g_cfg.key2_zone[z][0]) || key(g_cfg.key2_zone[z][1]);
        a[PCA_SKIP] = key(VK_RETURN) && !key(VK_MENU) && pc_skip_wanted();
        a[PCA_PAUSE] = g_esc_down && !g_act_on[0];      // Esc pauses for this player when the first one no longer drives
    } else if (g_keys_shared == KEYS_SHARED_DRIVING) {
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
    PadState ps = pad_read(pad_slot(1), 1);
    g_pad_busy[1] = pad_busy(ps);
    if (ps.ok) {
        // in the menus this gamepad is heard too: a new press on it says a gamepad is in use, and whose buttons to show
        static int held2;
        if ((ps.mask & ~held2) && !pc_racing()) g_input_kind = 1, g_hint_slot = pad_slot(1);
        held2 = ps.mask;
        static BackPress back;
        bool back_starts;
        static SlowCombo slow;
        const int mask = slow_combo(pause_back_mask(ps.mask, back, back_starts), slow, 1, driving);
        const bool back_closes = !driving && back_starts;
        for (int b = 0; b < PCB_COUNT; b++) {
            int m = (driving ? g_cfg.pad[b] : pc_menu_pad(b)) & mask;
            if (!m) continue;
            if (back_closes && b == PCB_TRIANGLE) {     // Back on the pause menu: Start, which closes it (this player's pause)
                press(PCB_START, 0xFF);
                continue;
            }
            int amount = 0xFF;
            if (m == 0x20000) amount = ps.rt;
            else if (m == 0x10000) amount = ps.lt;
            press(b, amount);
        }
        if (driving && (mask & 0x40) && pc_custom_port(1)) buttons &= (u16)~(1u << 1);       // (the left stick's press, as above)
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
        phase_thread("window");
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
        // started again by itself (window_relaunch): where the window was, and in front as it was
        if (const char *at = getenv("HP2_WINPOS")) {
            int x, y;
            if (g_cfg.monitor < 1 && sscanf(at, "%d,%d", &x, &y) == 2) SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        to_monitor_now();                   // the screen the setting names, if it does
        if (g_cfg.fullscreen) set_fullscreen_now(true);
        if (getenv("HP2_NOINTRO")) SetForegroundWindow(g_hwnd);
        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }).detach();
}
