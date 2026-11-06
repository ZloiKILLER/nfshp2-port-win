// The platform side of running as a game of its own (host.h).
#include "host.h"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static bool g_log_to_file;              // nobody sees stderr: the player gets a message box

std::string host_exe_dir()
{
#ifdef _WIN32
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        std::string p = exe;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }
#endif
    return "";
}

void host_log_start()
{
#ifdef _WIN32
    // a GUI program has no standard handles unless whoever started it redirected them
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) return;
    if (freopen((host_exe_dir() + "speed.log").c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
    g_log_to_file = true;
#endif
}

void host_fatal(const std::string &msg)
{
    fprintf(stderr, "%s\n", msg.c_str());
    fflush(stderr);
#ifdef _WIN32
    if (g_log_to_file)
        MessageBoxA(nullptr, (msg + "\n\nDetails are in speed.log next to the game.").c_str(),
                    "Need for Speed: Hot Pursuit 2", MB_OK | MB_ICONERROR | MB_TOPMOST);
#endif
}
