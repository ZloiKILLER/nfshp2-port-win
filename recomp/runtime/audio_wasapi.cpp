// audio_out.h on WASAPI: shared mode, event driven, 48 kHz stereo 16-bit (Windows
// converts to the mix format).
#include "audio_out.h"
#include "phase.h"
#include "cores.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cfgmgr32.h>
#include <initguid.h>       // (the two headers after it give their keys storage here)
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern FILE *g_hle_log;

static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

static IAudioClient *g_client;
static IAudioRenderClient *g_render;
static HANDLE g_event;
static UINT32 g_frames;

static bool open_device()
{
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                (void **)&en)))
        return false;
    HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr)) return false;
    hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&g_client);
    dev->Release();
    if (FAILED(hr)) return false;

    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = AUDIO_RATE * 4;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /* AUTOCONVERTPCM */ |
                  0x08000000 /* SRC_DEFAULT_QUALITY */;
    if (FAILED(g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000 /* 40 ms */, 0, &wf, nullptr)))
        return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_client->SetEventHandle(g_event);
    g_client->GetBufferSize(&g_frames);
    return SUCCEEDED(g_client->GetService(kIID_IAudioRenderClient, (void **)&g_render));
}

bool audio_out_start(AudioPull pull)
{
    bool ok = false;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread([pull, ready, &ok] {
        phase_thread("audio out");
        cores_thread(CORE_SOUND);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool opened = open_device();
        ok = opened;
        SetEvent(ready);
        if (!opened) return;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        g_client->Start();
        for (;;) {
            WaitForSingleObject(g_event, 100);
            UINT32 pad = 0;
            if (FAILED(g_client->GetCurrentPadding(&pad))) continue;
            UINT32 n = g_frames - pad;
            BYTE *buf;
            if (!n || FAILED(g_render->GetBuffer(n, &buf))) continue;
            pull((s16 *)buf, (int)n);
            g_render->ReleaseBuffer(n, 0);
        }
    }).detach();
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    fprintf(g_hle_log, "audio: WASAPI %s (%u frames buffer)\n", ok ? "open" : "unavailable", g_frames);
    return ok;
}

// ---- a second output: a gamepad's own speaker (audio_out.h) -------------------------------------------------------------
// Which endpoint is the gamepad's: the one with the same container (Windows' word for "one physical device") as the gamepad's HID
// interface. The interface's path gives its device, the device its container (cfgmgr32, by name: nothing new to link); each
// active render endpoint says its own. Two gamepads of one kind are told apart this way; an endpoint's name could not.
static bool radio_container(const std::string &hid_path, GUID &out)
{
    static const HMODULE cm = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cm) return false;
    const auto of_interface = (decltype(&CM_Get_Device_Interface_PropertyW))GetProcAddress(cm, "CM_Get_Device_Interface_PropertyW");
    const auto locate = (decltype(&CM_Locate_DevNodeW))GetProcAddress(cm, "CM_Locate_DevNodeW");
    const auto of_node = (decltype(&CM_Get_DevNode_PropertyW))GetProcAddress(cm, "CM_Get_DevNode_PropertyW");
    if (!of_interface || !locate || !of_node) return false;
    wchar_t path[600] = {};
    if (!MultiByteToWideChar(CP_UTF8, 0, hid_path.c_str(), -1, path, 599)) return false;
    wchar_t instance[MAX_DEVICE_ID_LEN + 1] = {};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof instance - sizeof(wchar_t);
    if (of_interface(path, &DEVPKEY_Device_InstanceId, &type, (PBYTE)instance, &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING) return false;
    DEVINST node = 0;
    if (locate(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    size = sizeof out;
    return of_node(node, &DEVPKEY_Device_ContainerId, &type, (PBYTE)&out, &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_GUID;
}

static IMMDevice *radio_endpoint(const std::string &hid_path, std::string &name)
{
    GUID want;
    if (!radio_container(hid_path, want)) return nullptr;
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en))) return nullptr;
    IMMDeviceCollection *all = nullptr;
    IMMDevice *found = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) {
        UINT n = 0;
        all->GetCount(&n);
        for (UINT i = 0; i < n && !found; i++) {
            IMMDevice *dev = nullptr;
            IPropertyStore *props = nullptr;
            if (FAILED(all->Item(i, &dev))) continue;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &v)) && v.vt == VT_CLSID && v.puuid && IsEqualGUID(*v.puuid, want)) found = dev;
                PropVariantClear(&v);
                if (found && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
                    char utf8[200] = {};
                    WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, utf8, sizeof utf8 - 1, nullptr, nullptr);
                    name = utf8;
                }
                PropVariantClear(&v);
                props->Release();
            }
            if (found != dev) dev->Release();
        }
        all->Release();
    }
    en->Release();
    return found;
}

static std::mutex g_radio_lock;         // what is wanted
static std::string g_radio_want;
static bool g_radio_right;
static RadioPull g_radio_pull;
static std::atomic<bool> g_radio_playing{false};

static void radio_thread()
{
    phase_thread("radio out");
    cores_thread(CORE_SOUND);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string now;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 frames = 0;
    unsigned long long retry_at = 0;
    bool lost = false;
    std::vector<s16> mono;
    const auto close = [&] {
        g_radio_playing = false;
        if (client) client->Stop();
        if (render) render->Release();
        if (client) client->Release();
        render = nullptr, client = nullptr;
    };
    for (;;) {
        std::string want;
        bool right;
        RadioPull pull;
        {
            std::lock_guard<std::mutex> hold(g_radio_lock);
            want = g_radio_want, right = g_radio_right, pull = g_radio_pull;
        }
        if (want != now || lost) {
            if (client) fprintf(stderr, "radio: the gamepad's speaker is %s\n", lost ? "gone" : "let go");
            close();
            now = want;
            retry_at = 0;
            lost = false;
        }
        if (now.empty() || !pull) {
            Sleep(100);
            continue;
        }
        if (!client) {
            if (GetTickCount64() < retry_at) {
                Sleep(100);
                continue;
            }
            retry_at = GetTickCount64() + 2000;
            std::string name;
            IMMDevice *dev = radio_endpoint(now, name);
            static int told;
            if (!dev) {
                if (told < 3) told++, fprintf(stderr, "radio: no sound output of the gamepad's own is found (is its speaker enabled in Windows' sound devices?)\n");
                continue;
            }
            HRESULT hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&client);
            dev->Release();
            if (FAILED(hr)) {
                client = nullptr;
                continue;
            }
            WAVEFORMATEX wf = {};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = AUDIO_RATE;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = 4;
            wf.nAvgBytesPerSec = AUDIO_RATE * 4;
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /* AUTOCONVERTPCM */ | 0x08000000 /* SRC_DEFAULT_QUALITY */;
            if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000 /* 40 ms */, 0, &wf, nullptr)) ||
                FAILED(hr = client->SetEventHandle(event)) || FAILED(hr = client->GetBufferSize(&frames)) ||
                FAILED(hr = client->GetService(kIID_IAudioRenderClient, (void **)&render)) || FAILED(hr = client->Start())) {
                fprintf(stderr, "radio: the gamepad's sound output \"%s\" did not open (%08lx)\n", name.c_str(), (unsigned long)hr);
                close();
                continue;
            }
            fprintf(stderr, "radio: the police radio plays on \"%s\" (%s, %u frames)\n", name.c_str(), right ? "the right channel, a DualSense's speaker" : "both channels", frames);
            g_radio_playing = true;
        }
        WaitForSingleObject(event, 100);
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) {
            lost = true;                    // (the cable is out, or Windows took the device away)
            continue;
        }
        const UINT32 n = frames - pad;
        BYTE *buf = nullptr;
        if (!n) continue;
        if (FAILED(render->GetBuffer(n, &buf))) {
            lost = true;
            continue;
        }
        mono.resize(n);
        pull(mono.data(), (int)n);
        s16 *out = (s16 *)buf;
        for (UINT32 i = 0; i < n; i++) out[2 * i] = right ? 0 : mono[i], out[2 * i + 1] = mono[i];
        render->ReleaseBuffer(n, 0);
    }
}

void audio_radio_target(const std::string &hid_path, bool right_only, RadioPull pull)
{
    static std::atomic<bool> started{false};
    {
        std::lock_guard<std::mutex> hold(g_radio_lock);
        g_radio_want = hid_path, g_radio_right = right_only, g_radio_pull = pull;
    }
    if (!hid_path.empty() && !started.exchange(true)) std::thread(radio_thread).detach();
}

bool audio_radio_open() { return g_radio_playing; }
