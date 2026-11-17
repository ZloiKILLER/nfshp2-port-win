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
#include "stall.h"
#include <atomic>

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

// ---- the stall catcher (stall.h) ---------------------------------------------------------------------------------
static std::atomic<unsigned> g_stall_frames;
static HANDLE g_stall_game, g_stall_render;

static long long stall_us()
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000000 / freq.QuadPart;
}

StallTimer::StallTimer(std::atomic<int> &t) : to(t), from(stall_us()) {}
StallTimer::~StallTimer() { to += (int)(stall_us() - from); }

void stall_frame() { g_stall_frames++; }
void stall_render_thread() { DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_stall_render, THREAD_ALL_ACCESS, FALSE, 0); }

void stall_test()
{
    static const char *env = getenv("HP2_STALLTEST");
    if (!env) return;
    static int frames;
    static bool done;
    if (!pc_racing()) frames = 0;
    else if (++frames == 600 && !done) done = true, Sleep((DWORD)atoi(env));
}

// one thread's place: "<module>+<offset>", and the return addresses into the program up its stack
static void stall_sample(HANDLE thread, const char *who, unsigned long long stood)
{
    if (!thread || SuspendThread(thread) == (DWORD)-1) return;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    const bool got = GetThreadContext(thread, &c) != 0;
    DWORD64 stack[160] = {};
    SIZE_T read = 0;
    if (got) ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c.Rsp, stack, sizeof stack, &read);
    ResumeThread(thread);
    if (!got) return;
    HMODULE exe = GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    const DWORD64 code = nt->OptionalHeader.BaseOfCode + nt->OptionalHeader.SizeOfCode;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    DWORD64 rva = c.Rip;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        rva = c.Rip - (DWORD64)m;
    }
    const char *base = strrchr(name, '\\');
    char line[512];
    int at = snprintf(line, sizeof line, "stall: %5llu ms, the %s thread at %s+%llx, called from", stood, who, base ? base + 1 : name, (unsigned long long)rva);
    int found = 0;
    for (SIZE_T i = 0; i < read / 8 && found < 8 && at < (int)sizeof line - 24; i++) {
        const DWORD64 v = stack[i];
        if (v - (DWORD64)exe - 0x1000 >= code - 0x1000) continue;
        const unsigned char *b = (const unsigned char *)v;
        const auto call_reg = [&](int back) { return b[-back] == 0xff && (b[-back + 1] >> 3 & 7) == 2; };
        if (b[-5] != 0xe8 && !call_reg(2) && !call_reg(3) && !call_reg(6) && !call_reg(7)) continue;
        at += snprintf(line + at, sizeof line - (size_t)at, " %llx", (unsigned long long)(v - (DWORD64)exe));
        found++;
    }
    fprintf(stderr, "%s\n", line);
}

void stall_watch_start()
{
    static const bool off = getenv("HP2_NOSTALLWATCH") != nullptr;
    if (off || g_stall_game) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_stall_game, THREAD_ALL_ACCESS, FALSE, 0);
    HMODULE exe = GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    fprintf(stderr, "stall: watching for a picture that stands still in a race (this exe's stamp is %08lx)\n", (unsigned long)nt->FileHeader.TimeDateStamp);
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        unsigned last = g_stall_frames, flowing = 0, samples = 0, lines = 0;
        ULONGLONG last_at = GetTickCount64();
        bool standing = false;
        // the world's clock: its bits when they last changed, when that was and which frame, and how the stand is going
        unsigned world = 0, world_frame = 0, world_samples = 0;
        ULONGLONG world_at = GetTickCount64();
        bool world_stands = false;
        for (;;) {
            Sleep(25);
            const unsigned frames = g_stall_frames;
            const ULONGLONG now = GetTickCount64();
            {
                unsigned bits = 0, paused = 0, slow = 0;
                const bool runs = stall_world(bits, paused, slow);
                if (!runs || bits != world) {
                    if (world_stands)
                        fprintf(stderr, "stall: over, the world stood for %llu ms while %u frames were shown\n", (unsigned long long)(now - world_at), frames - world_frame), fflush(stderr);
                    world = bits, world_at = now, world_frame = frames, world_samples = 0, world_stands = false;
                } else {
                    const ULONGLONG stood = now - world_at;
                    if (stood >= 150 && frames - world_frame >= 4 && flowing >= 40 && lines < 800 && world_samples < 12 && stood >= 150 + world_samples * 60ull) {
                        if (!world_stands) fprintf(stderr, "stall: the world stands though frames go on (paused %u, slow %u)\n", paused, slow);
                        world_stands = true, world_samples++, lines += 2;
                        stall_sample(g_stall_game, "game's", stood);
                        stall_sample(g_stall_render, "renderer's", stood);
                        fflush(stderr);
                    }
                }
            }
            if (frames != last) {
                if (standing) fprintf(stderr, "stall: over, the picture stood for %llu ms (frame %u)\n", (unsigned long long)(now - last_at), frames), fflush(stderr);
                // frames coming at a race's pace: the catcher is for a picture that stops among those, not for a screen that loads
                flowing = now - last_at < 70 ? std::min(flowing + 1, 1000u) : 0;
                last = frames, last_at = now, samples = 0, standing = false;
                continue;
            }
            const ULONGLONG stood = now - last_at;
            if (stood < 150 || (!standing && flowing < 40) || lines > 800 || samples >= 12 || stood < 150 + samples * 60ull) continue;
            if (!standing && !pc_racing()) continue;
            standing = true, samples++, lines += 2;
            stall_sample(g_stall_game, "game's", stood);
            stall_sample(g_stall_render, "renderer's", stood);
            fflush(stderr);
        }
    }).detach();
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
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
        const DWORD64 exe_size = nt->OptionalHeader.BaseOfCode + nt->OptionalHeader.SizeOfCode;       // the code's end
        std::map<std::pair<DWORD64, DWORD64>, unsigned> callers;
        // <file>.callers: "<hits> <rva of the place> <rva of its caller's return address>" (tools/recomp/profcallers.py)
        const auto write_callers = [&] {
            if (FILE *f = fopen((path + ".callers").c_str(), "w")) {
                for (auto &k : callers) fprintf(f, "%u %llx %llx\n", k.second, (unsigned long long)(k.first.first - (DWORD64)exe), (unsigned long long)(k.first.second - (DWORD64)exe));
                fclose(f);
            }
        };
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
                // who called: the first word up the stack that is a return address into the program (a call just before it). Right for
                // the small helpers (the EE's division, the vectors), whose own frames hold no other such word; a guess for the rest
                DWORD64 stack[48] = {};
                SIZE_T read = 0;
                if (got) ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c.Rsp, stack, sizeof stack, &read);
                ResumeThread(game);
                if (got && c.Rip - (DWORD64)exe < exe_size)
                    for (SIZE_T i = 0; i < read / 8; i++) {
                        const DWORD64 v = stack[i];
                        if (v - (DWORD64)exe - 0x1000 >= exe_size - 0x1000) continue;
                        const unsigned char *b = (const unsigned char *)v;
                        const auto call_reg = [&](int back) { return b[-back] == 0xff && (b[-back + 1] >> 3 & 7) == 2; };
                        if (b[-5] == 0xe8 || call_reg(2) || call_reg(3) || call_reg(6) || call_reg(7)) {
                            callers[{c.Rip, v}]++;
                            break;
                        }
                    }
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
        write_callers();
    }).detach();
}
#else
void profile_start() {}
void stall_watch_start() {}
void stall_render_thread() {}
void stall_frame() {}
void stall_test() {}
StallTimer::StallTimer(std::atomic<int> &t) : to(t), from(0) {}
StallTimer::~StallTimer() {}
#endif
