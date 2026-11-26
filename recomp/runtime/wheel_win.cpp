// The steering wheel's Windows side (pcwheel.h): DirectInput 8, a thread of its own. Not run by anybody yet on a real wheel
// (2026-11-24): written from the API's documentation, to be tried on a tester's Logitech G920.
#include "pcwheel.h"

#include "cores.h"
#include "host.h"
#include "hw.h"
#include "pcconfig.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

extern FILE *g_hle_log;

namespace {

struct Device {
    IDirectInputDevice8W *dev = nullptr;
    GUID guid = {};
    bool forces = false;
    IDirectInputEffect *fx[8] = {};
    PcWheelEffect sent[8] = {};
    int share_sent[8] = {};             // the percent each was last sent with (hp2.ini's wheel_spring ... wheel_shake)
    bool made[8] = {};
    int gain_sent = -1;
    int lost = 0;                       // reads failed in a row
    char name[128] = "";
};

IDirectInput8W *g_di;
Device g_dev[2];
std::mutex g_mx;                        // g_in, g_have, g_want
PcWheelInput g_in[2];
bool g_have[2];
PcWheelForces g_want[2];
bool g_want_new[2];
char g_name[2][128];
std::atomic<bool> g_started;
std::atomic<bool> g_look{true};         // the devices are to be looked through

void say(const char *what, const Device &d, HRESULT hr = S_OK)
{
    fprintf(g_hle_log, "wheel: %s: %s (%08lx)\n", d.name, what, (unsigned long)hr);
    fflush(g_hle_log);
}

void release(Device &d)
{
    for (IDirectInputEffect *&e : d.fx)
        if (e) e->Release(), e = nullptr;
    if (d.dev) {
        d.dev->Unacquire();
        d.dev->Release();
    }
    d = Device();
}

// 0..255 of the game's as DirectInput's 0..10000
LONG di(int v)
{
    const long long x = (long long)v * 10000 / 255;
    return (LONG)(x > 10000 ? 10000 : x < -10000 ? -10000 : x);
}

// A slot's block as a DirectInput effect. The slots' kinds are the game's (wheel_module.cpp): 0 a spring, 1 a damper, 2 a sine (the
// crash), 3 a sine and 4 a sawtooth (the road), 6 a square (the shake). What the twelve numbers are is known where the game's
// setters name them (the condition's centre, dead band, coefficients, saturations; a periodic's magnitude and the crash's side),
// and taken to be DirectInput's own fields in their order elsewhere: p2 the period in ms, p3 the phase in degrees, p6 and p8 the
// envelope's attack and fade levels, p7 and p5 their times. Those last are a reading, to be set right by how the wheel feels.
bool apply(Device &d, int slot, const PcWheelEffect &e, bool start)
{
    static const GUID *const kKind[8] = {&GUID_Spring, &GUID_Damper, &GUID_Sine, &GUID_Sine, &GUID_SawtoothUp, nullptr, &GUID_Square, nullptr};
    if (!d.forces || !kKind[slot]) return false;
    DWORD axes[1] = {DIJOFS_X};
    LONG direction[1] = {0};
    DICONDITION cond = {};
    DIPERIODIC per = {};
    DIENVELOPE env = {sizeof(DIENVELOPE)};
    DIEFFECT fx = {sizeof(DIEFFECT)};
    fx.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    fx.dwDuration = e.duration < 0 ? INFINITE : (DWORD)e.duration * 1000;
    // each kind's own share (Options, Controls, Steering Wheel): 0 the spring, 1 the damper, 2 a hit, 3 and 4 the road, 6 the shake
    // (the settings of the player who has this wheel: the wheels' screen, Advanced)
    const int who = pc_wheel_owner((int)(&d - g_dev));
    const int share = slot == 0 ? g_cfg.wheel_spring[who] : slot == 1 ? g_cfg.wheel_damper[who] : slot == 2 ? g_cfg.wheel_crash[who]
                    : slot == 3 || slot == 4 ? g_cfg.wheel_road[who] : slot == 6 ? g_cfg.wheel_shake[who] : 100;
    fx.dwGain = (DWORD)(DI_FFNOMINALMAX / 100 * (share < 0 ? 0 : share > 100 ? 100 : share));
    fx.dwTriggerButton = DIEB_NOTRIGGER;
    fx.cAxes = 1;
    fx.rgdwAxes = axes;
    fx.rglDirection = direction;
    DWORD what = DIEP_TYPESPECIFICPARAMS | DIEP_DURATION;
    if (slot < 2) {
        cond.lOffset = di(e.p[0]);
        cond.lDeadBand = di(e.p[1]);
        cond.lPositiveCoefficient = di(e.p[2]);
        cond.lNegativeCoefficient = di(e.p[3]);
        cond.dwPositiveSaturation = (DWORD)di(e.p[4] < 0 ? 0 : e.p[4]);
        cond.dwNegativeSaturation = (DWORD)di(e.p[5] < 0 ? 0 : e.p[5]);
        fx.cbTypeSpecificParams = sizeof cond;
        fx.lpvTypeSpecificParams = &cond;
    } else {
        per.dwMagnitude = (DWORD)di(e.p[0] < 0 ? -e.p[0] : e.p[0]);
        per.lOffset = slot == 2 ? di(e.p[4]) : 0;
        per.dwPhase = (DWORD)((e.p[3] % 360 + 360) % 360) * 100;
        per.dwPeriod = (DWORD)(e.p[2] > 0 ? e.p[2] : 50) * 1000;
        fx.cbTypeSpecificParams = sizeof per;
        fx.lpvTypeSpecificParams = &per;
        const long long length = e.duration < 0 ? 0 : (long long)e.duration;
        if ((e.p[7] > 0 || e.p[5] > 0) && length > 0 && (long long)e.p[7] + e.p[5] <= length) {
            env.dwAttackLevel = (DWORD)di(e.p[6] < 0 ? 0 : e.p[6]);
            env.dwAttackTime = (DWORD)(e.p[7] > 0 ? e.p[7] : 0) * 1000;
            env.dwFadeLevel = (DWORD)di(e.p[8] < 0 ? 0 : e.p[8]);
            env.dwFadeTime = (DWORD)(e.p[5] > 0 ? e.p[5] : 0) * 1000;
            fx.lpEnvelope = &env;
            what |= DIEP_ENVELOPE;
        }
    }
    HRESULT hr = S_OK;
    if (!d.fx[slot]) {
        if (d.made[slot]) return false;         // it could not be made: not tried again for this device
        d.made[slot] = true;
        hr = d.dev->CreateEffect(*kKind[slot], &fx, &d.fx[slot], nullptr);
        if (FAILED(hr)) {
            d.fx[slot] = nullptr;
            say(slot < 2 ? "a condition effect could not be made" : "a periodic effect could not be made", d, hr);
            return false;
        }
        d.sent[slot] = e;
        d.share_sent[slot] = share;
    } else if (memcmp(&d.sent[slot], &e, sizeof e) || d.share_sent[slot] != share) {
        hr = d.fx[slot]->SetParameters(&fx, what | DIEP_GAIN);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTEXCLUSIVEACQUIRED) return false;      // sent again once the device is back
        d.sent[slot] = e;
        d.share_sent[slot] = share;
    }
    if (start && d.fx[slot]) d.fx[slot]->Start(1, 0);
    return true;
}

// ---- How far a Logitech wheel turns: asked of Logitech's own software, and set there ---------------------------------------------
// (The user, 2026-11-24: "support G HUB too", and after a tester's ride, "build Logitech's library in, to settle this once".)
// Windows does not say how many degrees a wheel turns. Logitech's software does, and lets a game set it (Operating Range). Their
// Steering Wheel SDK (work/ref/logitech_sdk, read 2026-11-24) turned out to have no library of its own to build in: what it ships
// is a few kilobytes that look up the real one, installed with G HUB or the Gaming Software, and pass every call on; and one of
// its samples (SteeringWheelSDKIndependant) does the same two things with no library at all. So nothing of Logitech's is in this
// exe or beside it, and both of their ways are used, with the port's own DirectInput device:
//   1. the installed library, found as their wrapper finds it (the registry key below holds its path), by its two functions that
//      take a DirectInput device: LogiGetOperatingRangeDInput, LogiSetOperatingRangeDInput;
//   2. the software's own window for such questions: a registered message with the wheel's USB product number.
// What comes of it: with hp2.ini's wheel_set_range the wheel is set to turn wheel_lock degrees in all, so its own stops are the
// game's full lock (g_range_set), and is put back as it was when the game goes; without, or if the software says no, the range it
// has is at least known (g_range_known), and the steering and the calibration screen's picture go by the wheel's true degrees
// (pcwheel.cpp, range_now). A wheel of another maker, or Logitech's with their software not running: hp2.ini's wheel_range.
// Written from Logitech's header, their sample and their wrapper's own way of looking the library up; not tried on a wheel.
std::atomic<int> g_range_set{0};        // degrees the wheel was set to turn (it is the game's full lock then); 0: it was not
std::atomic<int> g_range_known{0};      // degrees Logitech's software says the wheel turns, where the game did not set it; 0: not known

typedef bool (*LogiGetFn)(void *, int *);
typedef bool (*LogiSetFn)(void *, int);
LogiGetFn g_logi_get;
LogiSetFn g_logi_set;
int g_put_back;                         // what the game changed and puts back as it goes: 0 nothing, 1 by the library, 2 by the window
int g_put_back_range;
unsigned g_put_back_pid;

// (another maker's code, called with a device of ours: whatever goes wrong in there stays in there)
bool logi_get(LogiGetFn f, void *dev, int *out)
{
    __try {
        return f(dev, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool logi_set(LogiSetFn f, void *dev, int range)
{
    __try {
        return f(dev, range);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the installed library's two functions, once
void logi_installed()
{
    static bool looked;
    if (looked) return;
    looked = true;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{63BD165D-1584-4E75-AB56-08330350545F}\\ServerBinary", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    wchar_t path[520] = {};
    DWORD bytes = sizeof path - sizeof(wchar_t), type = 0;
    const LSTATUS got = RegQueryValueExW(key, nullptr, nullptr, &type, (BYTE *)path, &bytes);
    RegCloseKey(key);
    if (got != ERROR_SUCCESS || type != REG_SZ || !path[0]) return;
    HMODULE dll = LoadLibraryW(path);       // kept for as long as the game runs
    if (!dll) {
        fprintf(g_hle_log, "wheel: Logitech's installed library is named in the registry and could not be loaded (%lu)\n", (unsigned long)GetLastError());
        return;
    }
    g_logi_get = (LogiGetFn)GetProcAddress(dll, "LogiGetOperatingRangeDInput");
    g_logi_set = (LogiSetFn)GetProcAddress(dll, "LogiSetOperatingRangeDInput");
}

// Logitech's software's window: the question's name, the wheel's USB product number, a number. 0: no answer
int logi_window(const wchar_t *what, unsigned pid, int number)
{
    const HWND to = FindWindowW(L"LCore_MessageHandler_{C464822E-04D1-4447-B918-6D5EB33E0E5D}", nullptr);
    const UINT message = RegisterWindowMessageW(what);
    if (!to || !message) return 0;
    DWORD_PTR answer = 0;
    if (!SendMessageTimeoutW(to, message, (WPARAM)pid, (LPARAM)number, SMTO_ABORTIFHUNG, 500, &answer)) return 0;
    return (int)(answer & 0xffff);
}

bool degrees_ok(int range) { return range >= 40 && range <= 2520; }

// a rotation the game set goes back to what the wheel's owner had
void logi_put_back()
{
    const int how = g_put_back;
    g_put_back = 0;
    if (how == 2) logi_window(L"LGS_Msg_SetOperatingRange", g_put_back_pid, g_put_back_range);
    else if (how == 1 && g_logi_set && g_dev[0].dev) logi_set(g_logi_set, g_dev[0].dev, g_put_back_range);
}
std::atomic<bool> g_relock;             // the menu changed the lock or whether it is set: the wheel is asked and set again

void logi_device_range(Device &d)
{
    const int who = pc_wheel_owner((int)(&d - g_dev));      // whose wheel it is: that player's rotation
    DIPROPDWORD id = {};
    id.diph.dwSize = sizeof id;
    id.diph.dwHeaderSize = sizeof id.diph;
    id.diph.dwHow = DIPH_DEVICE;
    if (FAILED(d.dev->GetProperty(DIPROP_VIDPID, &id.diph))) return;
    const unsigned vid = LOWORD(id.dwData), pid = HIWORD(id.dwData);
    if (vid != 0x046d) {
        fprintf(g_hle_log, "wheel: %s is not Logitech's (USB %04x:%04x): how far it turns is hp2.ini's wheel_range, %d degrees\n", d.name, vid, pid, g_cfg.wheel_range);
        return;
    }
    const int want = g_cfg.wheel_lock[who] < 40 ? 40 : g_cfg.wheel_lock[who] > 900 ? 900 : g_cfg.wheel_lock[who];
    int was = 0, how = 0;
    bool set = false;
    logi_installed();
    if (!g_cfg.wheel_set_range[who]) logi_put_back();        // (switched off in the menu: as it was before the game)
    if (g_logi_get && logi_get(g_logi_get, d.dev, &was) && degrees_ok(was)) how = 1;
    else was = 0;
    if (!how) {
        was = logi_window(L"LGS_Msg_GetOperatingRange", pid, 0);
        if (degrees_ok(was)) how = 2;
        else was = 0;
    }
    if (how && g_cfg.wheel_set_range[who]) {
        if (was == want) {
            set = true;                     // as the game would have it already
        } else {
            int now = 0;
            if (how == 1) {
                if (g_logi_set && logi_set(g_logi_set, d.dev, want)) set = logi_get(g_logi_get, d.dev, &now) && now == want;
            } else {
                logi_window(L"LGS_Msg_SetOperatingRange", pid, want);
                set = logi_window(L"LGS_Msg_GetOperatingRange", pid, 0) == want;
            }
            if (set && !g_put_back) g_put_back = how, g_put_back_range = was, g_put_back_pid = pid;
        }
    }
    if (set) g_range_set = want, g_range_known = 0;
    else if (how) g_range_known = was, g_range_set = 0;
    fprintf(g_hle_log, "wheel: %s (USB %04x:%04x): %s; %s\n", d.name, vid, pid,
            how == 1 ? "Logitech's installed library says how far it turns" : how == 2 ? "Logitech's software's window says how far it turns"
                     : g_logi_get ? "Logitech's library is there and gave no rotation, nor did the software's window (is G HUB running?)"
                                  : "neither Logitech's library nor its software's window answered (is G HUB running?)",
            !how ? "hp2.ini's wheel_range stands" : set ? "set to the game's full lock" : g_cfg.wheel_set_range[who] ? "it could not be set, and is taken as it is" : "taken as it is");
    if (how) fprintf(g_hle_log, "wheel: the rotation was %d degrees, is %d; the game's full lock is %d\n", was, set ? want : was, set ? want : g_cfg.wheel_lock[who]);
    fflush(g_hle_log);
}

BOOL CALLBACK found(const DIDEVICEINSTANCEW *inst, void *)
{
    if ((inst->dwDevType & 0xff) != DI8DEVTYPE_DRIVING) return DIENUM_CONTINUE;
    for (Device &d : g_dev)
        if (d.dev && !memcmp(&d.guid, &inst->guidInstance, sizeof(GUID))) return DIENUM_CONTINUE;
    Device *d = !g_dev[0].dev ? &g_dev[0] : !g_dev[1].dev ? &g_dev[1] : nullptr;
    if (!d) return DIENUM_STOP;
    WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, d->name, sizeof d->name, nullptr, nullptr);
    HRESULT hr = g_di->CreateDevice(inst->guidInstance, &d->dev, nullptr);
    if (FAILED(hr)) {
        say("the device could not be opened", *d, hr);
        *d = Device();
        return DIENUM_CONTINUE;
    }
    d->guid = inst->guidInstance;
    d->dev->SetDataFormat(&c_dfDIJoystick2);
    // the forces need the device to ourselves; in the background too, the game is played with the window in front anyway
    hr = d->dev->SetCooperativeLevel((HWND)window_handle(), DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) hr = d->dev->SetCooperativeLevel((HWND)window_handle(), DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    DIPROPRANGE range = {};
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_DEVICE;
    range.lMin = 0;
    range.lMax = 65535;
    d->dev->SetProperty(DIPROP_RANGE, &range.diph);
    DIDEVCAPS caps = {sizeof caps};
    d->forces = SUCCEEDED(d->dev->GetCapabilities(&caps)) && (caps.dwFlags & DIDC_FORCEFEEDBACK);
    if (d->forces) {                    // the wheel's own centring off: the game's spring is the centring
        DIPROPDWORD off = {};
        off.diph.dwSize = sizeof off;
        off.diph.dwHeaderSize = sizeof off.diph;
        off.diph.dwHow = DIPH_DEVICE;
        off.dwData = DIPROPAUTOCENTER_OFF;
        d->dev->SetProperty(DIPROP_AUTOCENTER, &off.diph);
    }
    hr = d->dev->Acquire();
    fprintf(g_hle_log, "wheel: %s: found (%lu axes, %lu buttons, %s), acquire %08lx\n", d->name, (unsigned long)caps.dwAxes, (unsigned long)caps.dwButtons,
            d->forces ? "with force feedback" : "no force feedback: is the maker's software (Logitech G HUB) running?", (unsigned long)hr);
    fflush(g_hle_log);
    if (d == &g_dev[0]) logi_device_range(*d);     // the first wheel's: how far it turns
    return DIENUM_CONTINUE;
}

void wheel_thread()
{
    cores_thread(CORE_BACKGROUND);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void **)&g_di, nullptr)) || !g_di) {
        fprintf(g_hle_log, "wheel: no DirectInput\n");
        return;
    }
    for (;;) {
        // The devices are looked through once the window is there, and again only when Windows says they have changed (a wheel
        // plugged in: window.cpp, WM_DEVICECHANGE). Not on a clock: going through DirectInput's devices every few seconds is a
        // known cause of a stutter every few seconds.
        if (window_handle() && (!g_dev[0].dev || !g_dev[1].dev) && g_look.exchange(false)) {
            Sleep(300);                 // Windows says so several times as a device comes up
            g_look = false;
            g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, found, nullptr, DIEDFL_ATTACHEDONLY);
        }
        if (g_relock.exchange(false) && g_dev[0].dev) logi_device_range(g_dev[0]);
        for (int i = 0; i < 2; i++) {
            Device &d = g_dev[i];
            if (!d.dev && g_cfg.wheel_emulate > i) {
                // hp2.ini's wheel_emulate (the user, 2026-11-24: "let us emulate a connected wheel, just to see the settings"): with
                // no wheel there, a pretend one, so that the game shows what it shows for a wheel (its controller setup, the
                // calibration screen). The keyboard is its hands and feet while the game's window is in front: Home and End turn
                // it (it comes back to its middle when let go, as a wheel with a spring does), Page Up is the throttle, Page Down
                // the brake. It takes no forces. Its pedals rest where hp2.ini says a pedal rests.
                // wheel_emulate=2 (the user, 2026-11-24: "connect two virtual wheels, to check"): a second one for the other
                // player, on the number pad: 4 and 6 turn it, 8 is the throttle, 5 the brake.
                static double turns[2];         // -1 to 1 of all it turns
                static bool tolds[2];
                double &turn = turns[i];
                bool &told = tolds[i];
                const int kLeft = i ? VK_NUMPAD4 : VK_HOME, kRight = i ? VK_NUMPAD6 : VK_END, kGas = i ? VK_NUMPAD8 : VK_PRIOR, kBrake = i ? VK_NUMPAD5 : VK_NEXT;
                const bool front = window_handle() && GetForegroundWindow() == (HWND)window_handle();
                const bool left = front && (GetAsyncKeyState(kLeft) & 0x8000), right = front && (GetAsyncKeyState(kRight) & 0x8000);
                const double step = 0.004 / 1.2;        // lock to lock in 2.4 s, a pass of this loop being 4 ms
                if (left != right) turn += left ? -step : step;
                else if (turn > 0) turn = turn > step ? turn - step : 0;
                else if (turn < 0) turn = turn < -step ? turn + step : 0;
                turn = turn < -1 ? -1 : turn > 1 ? 1 : turn;
                PcWheelInput in = {};
                for (int k = 0; k < 8; k++) in.axis[k] = 32767;
                in.axis[0] = (int)(32767.5 + turn * 32767.5);
                const int rest = g_cfg.wheel_pedals_inverted ? 65535 : 0, down = 65535 - rest;
                if (g_cfg.wheel_gas >= 1 && g_cfg.wheel_gas <= 7) in.axis[g_cfg.wheel_gas] = front && (GetAsyncKeyState(kGas) & 0x8000) ? down : rest;
                if (g_cfg.wheel_brake >= 1 && g_cfg.wheel_brake <= 7) in.axis[g_cfg.wheel_brake] = front && (GetAsyncKeyState(kBrake) & 0x8000) ? down : rest;
                in.hat = -1;
                in.forces = false;
                {
                    std::lock_guard<std::mutex> hold(g_mx);
                    g_in[i] = in;
                    g_have[i] = true;
                    strcpy(g_name[i], i ? "Emulated wheel 2 (hp2.ini, wheel_emulate)" : "Emulated wheel (hp2.ini, wheel_emulate)");
                    g_want_new[i] = false;
                    g_want[i].start = 0;
                }
                if (!told) {
                    told = true;
                    if (i) fprintf(g_hle_log, "wheel: a second pretend one (hp2.ini, wheel_emulate=2): 4 and 6 of the number pad turn it, 8 the throttle, 5 the brake\n");
                    else fprintf(g_hle_log, "wheel: none connected, a pretend one in its place (hp2.ini, wheel_emulate): Home and End turn it, Page Up the throttle, Page Down the brake\n");
                    fflush(g_hle_log);
                }
                continue;
            }
            if (!d.dev) {
                std::lock_guard<std::mutex> hold(g_mx);
                g_have[i] = false;
                g_name[i][0] = 0;
                continue;
            }
            DIJOYSTATE2 st = {};
            HRESULT hr = d.dev->Poll();
            if (FAILED(hr)) hr = d.dev->Acquire();
            hr = d.dev->GetDeviceState(sizeof st, &st);
            if (FAILED(hr)) {
                if (++d.lost > 500) {           // two seconds without it: gone (unplugged), looked for again
                    say("lost", d, hr);
                    release(d);
                }
                continue;
            }
            d.lost = 0;
            PcWheelInput in = {};
            const LONG a[8] = {st.lX, st.lY, st.lZ, st.lRx, st.lRy, st.lRz, st.rglSlider[0], st.rglSlider[1]};
            for (int k = 0; k < 8; k++) in.axis[k] = (int)a[k];
            for (int k = 0; k < 32; k++)
                if (st.rgbButtons[k] & 0x80) in.buttons |= 1u << k;
            in.hat = (st.rgdwPOV[0] & 0xffff) == 0xffff ? -1 : (int)(((st.rgdwPOV[0] + 2250) % 36000) / 4500);
            in.forces = d.forces;
            PcWheelForces want;
            bool want_new;
            {
                std::lock_guard<std::mutex> hold(g_mx);
                g_in[i] = in;
                g_have[i] = true;
                memcpy(g_name[i], d.name, sizeof g_name[i]);
                want = g_want[i];
                want_new = g_want_new[i];
                g_want_new[i] = false;
                g_want[i].start = 0;
            }
            if (!want_new || !d.forces) continue;
            for (int slot = 0; slot < 8; slot++)
                if (want.set >> slot & 1) apply(d, slot, want.fx[slot], (want.start >> slot & 1) != 0);
            if (want.gain >= 0) {
                int gain = (int)((long long)di(want.gain) * (want.strength < 0 ? 0 : want.strength > 200 ? 200 : want.strength) / 100);
                if (gain > 10000) gain = 10000;
                if (gain != d.gain_sent) {
                    DIPROPDWORD g = {};
                    g.diph.dwSize = sizeof g;
                    g.diph.dwHeaderSize = sizeof g.diph;
                    g.diph.dwHow = DIPH_DEVICE;
                    g.dwData = (DWORD)gain;
                    if (SUCCEEDED(d.dev->SetProperty(DIPROP_FFGAIN, &g.diph))) d.gain_sent = gain;
                }
            }
        }
        Sleep(4);
    }
}

}   // namespace

void pc_wheel_start()
{
    if (!g_cfg.wheel || g_started.exchange(true)) return;
    std::thread(wheel_thread).detach();
}

void pc_wheel_devices_changed() { g_look = true; }
int pc_wheel_range_set() { return g_range_set.load(); }
int pc_wheel_range_known() { return g_range_known.load(); }

// The game goes (window.cpp, WM_DESTROY): the wheel's rotation back as its owner had it
void pc_wheel_quit()
{
    const int how = g_put_back;
    g_put_back = 0;
    if (how == 2) logi_window(L"LGS_Msg_SetOperatingRange", g_put_back_pid, g_put_back_range);
    else if (how == 1 && g_logi_set && g_dev[0].dev) logi_set(g_logi_set, g_dev[0].dev, g_put_back_range);
}
void pc_wheel_lock_changed() { g_relock = true; }

bool pc_wheel_input(int index, PcWheelInput &out)
{
    if (index < 0 || index > 1 || !g_started) return false;
    std::lock_guard<std::mutex> hold(g_mx);
    if (!g_have[index]) return false;
    out = g_in[index];
    return true;
}

void pc_wheel_forces(int index, const PcWheelForces &f)
{
    if (index < 0 || index > 1 || !g_started) return;
    std::lock_guard<std::mutex> hold(g_mx);
    const unsigned start = g_want[index].start | f.start;      // a start not taken yet is not lost
    g_want[index] = f;
    g_want[index].start = start;
    g_want_new[index] = true;
}

const char *pc_wheel_name(int index)
{
    static char name[2][128];
    if (index < 0 || index > 1) return "";
    std::lock_guard<std::mutex> hold(g_mx);
    memcpy(name[index], g_name[index], sizeof name[index]);
    return name[index];
}

#else
void pc_wheel_start() {}
void pc_wheel_devices_changed() {}
int pc_wheel_range_set() { return 0; }
int pc_wheel_range_known() { return 0; }
void pc_wheel_quit() {}
void pc_wheel_lock_changed() {}
bool pc_wheel_input(int, PcWheelInput &) { return false; }
void pc_wheel_forces(int, const PcWheelForces &) {}
const char *pc_wheel_name(int) { return ""; }
#endif
