// HP2_PROFRIP=<seconds>,<start>,<file>: where the game's thread really is, sampled by the instruction pointer every 1 to 3 ms.
// Written every 10 seconds, so closing the game loses little, and apart for what the game is doing: <file> all of it, <file>.drive the race itself,
// <file>.replay30 and <file>.replay60 a replay at that frame rate, <file>.other the rest.
// Unlike HP2_PROFILE it sees the runtime, the driver and the waiting too. The file has "<hits> <module> <rva>" lines; tools/recomp/profrip.py names them
// from the linker's map. The game goes on running.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include <thread>

#include "pcconfig.h"

bool pc_racing();
bool pc_car_input();
bool pc_replay_playing();

static void write_hits(const std::string &path, const std::map<DWORD64, unsigned> &hits, unsigned samples)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "# samples %u\n", samples);
    for (auto &h : hits) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        DWORD64 rva = h.first;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)h.first, &m) && m) {
            GetModuleFileNameA(m, name, MAX_PATH);
            rva = h.first - (DWORD64)m;
        }
        const char *base = strrchr(name, '\\');
        fprintf(f, "%u %s %llx\n", h.second, base ? base + 1 : name, (unsigned long long)rva);
    }
    fclose(f);
}

void profile_start()
{
    const char *env = getenv("HP2_PROFRIP");
    if (!env) return;
    int seconds = 0, start = 0;
    char file[512] = {};
    if (sscanf(env, "%d,%d,%511[^\n]", &seconds, &start, file) != 3) return;
    HANDLE game;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game, THREAD_ALL_ACCESS, FALSE, 0);
    std::string path = file;
    std::thread([game, seconds, start, path]() mutable {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        // a negative start: that many seconds after the race has begun
        if (start < 0) {
            while (!pc_racing()) Sleep(100);
            start = -start;
        }
        Sleep((DWORD)start * 1000);
        // Not Sleep: the system's tick is 16 ms, the game's frame 33, and samples on the tick land in one phase of the frame. A spin on the clock
        // at random gaps of 1 to 3 ms takes a core of its own and has no such phase.
        static const char *const kPhase[5] = {"", ".drive", ".replay30", ".replay60", ".other"};
        std::map<DWORD64, unsigned> hits[5];
        unsigned counts[5] = {};
        // <file>.timeline: each second, its samples and the 12 places that had most of them, "t <seconds of the system's clock> n <samples> <phase> | <hits>@<rva> ..."
        FILE *timeline = fopen((path + ".timeline").c_str(), "w");
        std::map<DWORD64, unsigned> second;
        unsigned second_n = 0;
        int second_phase = 0;
        ULONGLONG second_at = GetTickCount64() / 1000;
        HMODULE exe = GetModuleHandleA(nullptr);
        LONGLONG next_write = 0;
        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        const LONGLONG end = now.QuadPart + freq.QuadPart * seconds;
        unsigned samples = 0, random = 12345;
        while (now.QuadPart < end) {
            random = random * 1664525u + 1013904223u;
            LONGLONG next = now.QuadPart + freq.QuadPart / 1000 + (LONGLONG)(random >> 8) * (freq.QuadPart / 500) / (1 << 24);
            while (now.QuadPart < next) QueryPerformanceCounter(&now);
            if (SuspendThread(game) != (DWORD)-1) {
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_CONTROL;
                const bool got = GetThreadContext(game, &c) != 0;
                ResumeThread(game);
                if (got) {
                    const int phase = pc_replay_playing() ? (g_cfg.frame_rate == 60 ? 3 : 2) : pc_car_input() ? 1 : 4;
                    hits[0][c.Rip]++;
                    second[c.Rip]++;
                    second_n++;
                    second_phase = phase;
                    hits[phase][c.Rip]++;
                    counts[0]++;
                    counts[phase]++;
                    samples++;
                }
            }
            if (timeline && GetTickCount64() / 1000 != second_at) {
                std::vector<std::pair<unsigned, DWORD64>> top;
                for (auto &h : second) top.push_back({h.second, h.first});
                std::sort(top.rbegin(), top.rend());
                fprintf(timeline, "t %llu n %u %s |", (unsigned long long)second_at, second_n, kPhase[second_phase][0] ? kPhase[second_phase] + 1 : "-");
                for (size_t i = 0; i < top.size() && i < 12; i++) {
                    HMODULE m = nullptr;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)top[i].second, &m);
                    if (m == exe) fprintf(timeline, " %u@%llx", top[i].first, (unsigned long long)(top[i].second - (DWORD64)exe));
                    else fprintf(timeline, " %u@dll", top[i].first);
                }
                fprintf(timeline, "\n");
                fflush(timeline);
                second.clear();
                second_n = 0;
                second_at = GetTickCount64() / 1000;
            }
            if (now.QuadPart >= next_write) {
                next_write = now.QuadPart + freq.QuadPart * 10;
                for (int i = 0; i < 5; i++)
                    if (counts[i]) write_hits(path + kPhase[i], hits[i], counts[i]);
            }
        }
        for (int i = 0; i < 5; i++)
            if (counts[i]) write_hits(path + kPhase[i], hits[i], counts[i]);
    }).detach();
}
#else
void profile_start() {}
#endif
