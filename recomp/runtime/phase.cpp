// HP2_PHASES=1 (phase.h): the game's thread by what it is doing, each thread of the process by what it costs.
#include "phase.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "ps2.h"

bool g_phases_on = getenv("HP2_PHASES") != nullptr;

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

bool pc_racing();
extern FILE *g_hle_log;

namespace {

const char *const kNames[PH_COUNT] = {
    "other",        "wait: tick",   "wait: vsync", "wait: renderer", "ai",       "physics",       "collisions", "particles", "sound (game)",
    "world: rest",  "iop call",     "disc read",   "file queue",     "loading",  "game flow",     "menu",       "display: rest",
    "culling",      "scenery",      "cars",        "world objects",  "effects",  "polys",         "hud",        "strips: visible", "strips: out", "lists (port)", "textures (port)",
};
const int kViews = 34;              // view 0: none; a render target's number + 1, the last for any above
struct Account {
    unsigned long long tsc[kViews][PH_COUNT];
    unsigned long long tsc_from, qpc_from;
    unsigned frames;
    std::map<DWORD, unsigned long long> cycles_from;    // each thread's cycles when the account began
};
Account g_acc[2];                   // 0 all but the race, 1 the race
int g_now;                          // the account being added to
int g_stack[32], g_depth;           // the phases entered; the innermost counts
int g_view;
unsigned long long g_last;          // the counter when the phase or the view last changed
DWORD g_game_thread;
std::mutex g_names_lock;
std::map<DWORD, std::string> g_names;

inline void lap()
{
    const unsigned long long t = __rdtsc();
    g_acc[g_now].tsc[g_view][g_depth ? g_stack[g_depth - 1] : PH_OTHER] += t - g_last;
    g_last = t;
}

std::map<DWORD, unsigned long long> thread_cycles()
{
    std::map<DWORD, unsigned long long> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 e = {sizeof e};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
        if (e.th32OwnerProcessID != pid) continue;
        if (const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID)) {
            ULONG64 cycles = 0;
            if (QueryThreadCycleTime(h, &cycles)) out[e.th32ThreadID] = cycles;
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return out;
}

void begin(Account &a)
{
    memset(a.tsc, 0, sizeof a.tsc);
    a.frames = 0;
    a.tsc_from = __rdtsc();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    a.qpc_from = (unsigned long long)q.QuadPart;
    a.cycles_from = thread_cycles();
}

void report(const Account &a, const char *what)
{
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const double seconds = (double)((unsigned long long)q.QuadPart - a.qpc_from) / (double)f.QuadPart;
    const unsigned long long span = __rdtsc() - a.tsc_from;
    if (seconds < 0.5 || !span || !a.frames) return;
    const double ms = seconds * 1000.0 / (double)span;      // a count of the counter in milliseconds
    fprintf(g_hle_log, "phases: %s: %.1f s, %u frames, %.2f ms a frame\n", what, seconds, a.frames, seconds * 1000.0 / a.frames);
    unsigned long long by_phase[PH_COUNT] = {}, by_view[kViews] = {}, all = 0;
    for (int v = 0; v < kViews; v++)
        for (int p = 0; p < PH_COUNT; p++) by_phase[p] += a.tsc[v][p], by_view[v] += a.tsc[v][p], all += a.tsc[v][p];
    for (int p = 0; p < PH_COUNT; p++)
        if (by_phase[p])
            fprintf(g_hle_log, "phases:   %-16s %7.3f ms a frame  %5.1f %%\n", kNames[p], (double)by_phase[p] * ms / a.frames, 100.0 * (double)by_phase[p] / (double)all);
    // the display by the picture it was building: every phase of it under that picture
    for (int v = 1; v < kViews; v++) {
        if (!by_view[v]) continue;
        fprintf(g_hle_log, "phases:   picture %2d: %7.3f ms a frame  %5.1f %% |", v - 1, (double)by_view[v] * ms / a.frames, 100.0 * (double)by_view[v] / (double)all);
        for (int p = 0; p < PH_COUNT; p++)
            if ((double)a.tsc[v][p] * ms / a.frames >= 0.005) fprintf(g_hle_log, " %s %.3f", kNames[p], (double)a.tsc[v][p] * ms / a.frames);
        fprintf(g_hle_log, "\n");
    }
    // the threads: cycles of the processor each has used since the account began, as a share of one core over that time
    const std::map<DWORD, unsigned long long> cycles = thread_cycles();
    std::lock_guard<std::mutex> lock(g_names_lock);
    double sum = 0;
    for (const auto &t : cycles) {
        const auto was = a.cycles_from.find(t.first);
        const unsigned long long used = t.second - (was == a.cycles_from.end() ? 0 : was->second);
        const double share = 100.0 * (double)used / (double)span;
        sum += share;
        if (share < 0.05) continue;
        const auto name = g_names.find(t.first);
        fprintf(g_hle_log, "phases:   thread %-12s %6.2f %% of a core, %.3f bn cycles\n", name == g_names.end() ? ("#" + std::to_string(t.first)).c_str() : name->second.c_str(),
                share, (double)used / 1e9);
    }
    fprintf(g_hle_log, "phases:   all threads %.1f %% of a core\n", sum);
    fflush(g_hle_log);
}

}   // namespace

void phase_thread(const char *name)
{
    if (!g_phases_on) return;
    std::lock_guard<std::mutex> lock(g_names_lock);
    g_names[GetCurrentThreadId()] = name;
}

void phase_enter(int phase)
{
    if (GetCurrentThreadId() != g_game_thread || g_depth >= 32) {
        if (g_game_thread) return;
        // the first phase of all is the game's thread's
        g_game_thread = GetCurrentThreadId();
        phase_thread("game");
        g_last = __rdtsc();
        begin(g_acc[0]);
        begin(g_acc[1]);
    }
    lap();
    g_stack[g_depth++] = phase;
}

void phase_leave()
{
    if (GetCurrentThreadId() != g_game_thread || !g_depth) return;
    lap();
    g_depth--;
}

void phase_view(int target)
{
    if (!g_phases_on || GetCurrentThreadId() != g_game_thread) return;
    lap();
    g_view = target < 0 ? 0 : target + 1 < kViews ? target + 1 : kViews - 1;
}

void phase_frame()
{
    if (!g_phases_on || GetCurrentThreadId() != g_game_thread) return;
    lap();
    g_acc[g_now].frames++;
    static ULONGLONG told;
    const ULONGLONG t = GetTickCount64();
    const int now = pc_racing() ? 1 : 0;
    if (now != g_now) {
        report(g_acc[g_now], g_now ? "the race, at its end" : "all but the race, till this race");
        g_now = now;
        begin(g_acc[g_now]);
        told = t;
        g_last = __rdtsc();
    } else if (t - told >= 15000) {
        report(g_acc[g_now], g_now ? "the race so far" : "all but the race so far");
        told = t;
        g_last = __rdtsc();         // the report's own time is no phase
    }
}

#else
void phase_enter(int) {}
void phase_leave() {}
void phase_view(int) {}
void phase_frame() {}
void phase_thread(const char *) {}
#endif

// ---- the game's functions that are phases as they are ----------------------------------------------
#define PHASE_HOOK(name, phase) \
    void HOOK_##name(Ctx &c, void (*orig)(Ctx &)) \
    { \
        Phase timed(phase); \
        orig(c); \
    }
// (the cars' phases of World::DoTimestep also go by native.cpp's order check, a diagnostic: HP2_ORDERCHECK)
void native_order_check(Ctx &c, void (*orig)(Ctx &), int which);
#define CAR_PHASE_HOOK(name, phase, which) \
    void HOOK_##name(Ctx &c, void (*orig)(Ctx &)) \
    { \
        Phase timed(phase); \
        native_order_check(c, orig, which); \
        orig(c); \
    }
CAR_PHASE_HOOK(Think__3Carf, PH_AI, 0)
PHASE_HOOK(GlobalCopThink__Ff, PH_AI)
PHASE_HOOK(DoWheelForces__3Car, PH_PHYSICS)
CAR_PHASE_HOOK(Drive__3Carf, PH_PHYSICS, 1)
CAR_PHASE_HOOK(BeginMovement__3Carf, PH_PHYSICS, 2)
PHASE_HOOK(DoTimestepMove__5Worldf, PH_PHYSICS)
CAR_PHASE_HOOK(FinishMovement__3Carf, PH_PHYSICS, 3)
PHASE_HOOK(DoTimestepCollisions__5Worldf, PH_COLLISIONS)
void native_particles_step(Ctx &c, void (*orig)(Ctx &));
void HOOK_DoTimeStep__16AcidEffectSystemf(Ctx &c, void (*orig)(Ctx &))
{
    Phase timed(PH_PARTICLES);
    native_particles_step(c, orig);         // and the clock of the port's own particles (native.cpp, Particles: High)
}
PHASE_HOOK(emProcessAllEvents__Fv, PH_SOUND)
PHASE_HOOK(FakeEngineTask__3Car, PH_SOUND)
PHASE_HOOK(Update__5soundv, PH_SOUND)
PHASE_HOOK(ServiceQueuedFiles__Fv, PH_FILEQ)
PHASE_HOOK(LoadResourceIfFileTransferFinished__12ResourceFile, PH_LOADING)
PHASE_HOOK(WaitUntilVSync__Fv, PH_WAIT_VSYNC)
PHASE_HOOK(eWaitUntilRenderingDone__Fv, PH_WAIT_VSYNC)
PHASE_HOOK(MenuSystemTick__Fv, PH_MENU)
PHASE_HOOK(ServiceGameFlow__Fv, PH_GAMEFLOW)
PHASE_HOOK(DoCulling__20GrandSceneryCullInfo, PH_CULL)
PHASE_HOOK(RenderCars__FP5eView, PH_CARS)
PHASE_HOOK(RenderPlayers__6PlayerP5eView, PH_CARS)
PHASE_HOOK(RenderWorldObjects__FP5eView, PH_WORLD_OBJECTS)
// the particles' sprites: what this call puts into the frame's memory is theirs (native.cpp, mid_particles)
uint32_t native_particles_begin(Ctx &c);
void native_particles_end(Ctx &c, uint32_t from);
bool native_particles_draw(Ctx &c);
void HOOK_RenderEffects__16AcidEffectSystemP5eView(Ctx &c, void (*orig)(Ctx &))
{
    Phase timed(PH_EFFECTS);
    if (native_particles_draw(c)) return;       // Particles: Advanced or High: the port makes the sprites (native.cpp)
    const uint32_t from = native_particles_begin(c);
    orig(c);
    native_particles_end(c, from);
}
PHASE_HOOK(AttachSolidPolys__FP9eDmaChainT0, PH_POLYS)
PHASE_HOOK(AttachAlphaPolys__FP9eDmaChainT0, PH_POLYS)

// the picture the game builds from here on: the render target's first word is its number
void HOOK_eSetCurrentRenderTarget__FP13eRenderTarget(Ctx &c, void (*orig)(Ctx &))
{
    if (g_phases_on) phase_view(c.r[4].w[0] ? (int)LW(c.r[4].w[0]) : -1);
    orig(c);
}
