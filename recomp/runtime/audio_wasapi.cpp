// audio_out.h on WASAPI: shared mode, event driven, 48 kHz stereo 16-bit (Windows
// converts to the mix format).
#include "audio_out.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <thread>

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
