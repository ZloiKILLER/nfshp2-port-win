// The processor's cores, who runs where, and the pool of workers (cores.h).
#include "cores.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "phase.h"

extern FILE *g_hle_log;
void ee_float_mode_here();              // the EE's rounding, for a thread that does the game's sums (ps2.cpp)

namespace {

// ---- the pool --------------------------------------------------------------------------------------
// A job is jobs_run's own, on its stack. A worker takes hold of it under the lock (users) and lets go when it finds no more pieces;
// jobs_run takes the job away under the lock and leaves when the pieces are done and no worker holds it. So a worker that wakes late
// finds no job, or the next one whole: never the next job's pieces by the last job's counter.
struct Job {
    void (*fn)(void *, int, int);
    void *ctx;
    int count;
    std::atomic<int> next{0};           // the next piece to take
    std::atomic<int> left{0};           // pieces not finished yet
    int users = 0;                      // workers holding the job (under the pool's lock)
};
struct Pool {
    std::mutex lock;
    std::condition_variable wake, done;
    Job *job = nullptr;
    unsigned generation = 0;            // counts the jobs: a worker that has seen this one sleeps till the next
    int workers = 0;
    std::atomic<bool> busy{false};      // a job is being run: jobs_run inside a piece does its pieces in place
} g_pool;

void take_pieces(Job &job, int slot)
{
    for (;;) {
        const int i = job.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= job.count) return;
        job.fn(job.ctx, i, slot);
        job.left.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void place_worker(int slot);

void worker(int slot)
{
    char name[24];
    snprintf(name, sizeof name, "worker %d", slot);
    phase_thread(name);
    place_worker(slot);
    ee_float_mode_here();
    unsigned seen = 0;
    for (;;) {
        Job *job;
        {
            std::unique_lock<std::mutex> hold(g_pool.lock);
            g_pool.wake.wait(hold, [&] { return g_pool.generation != seen; });
            seen = g_pool.generation;
            job = g_pool.job;
            if (!job) continue;         // over before this worker woke
            job->users++;
        }
        take_pieces(*job, slot);
        {
            std::lock_guard<std::mutex> hold(g_pool.lock);
            job->users--;
        }
        g_pool.done.notify_all();
    }
}

}   // namespace

int cores_workers() { return g_pool.workers; }

void jobs_run(int count, void (*fn)(void *ctx, int index, int slot), void *ctx)
{
    if (count <= 0) return;
    if (!g_pool.workers || count == 1 || g_pool.busy.exchange(true)) {
        for (int i = 0; i < count; i++) fn(ctx, i, 0);
        return;
    }
    Job job;
    job.fn = fn;
    job.ctx = ctx;
    job.count = count;
    job.left.store(count, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> hold(g_pool.lock);
        g_pool.job = &job;
        g_pool.generation++;
    }
    g_pool.wake.notify_all();
    take_pieces(job, 0);
    {
        std::unique_lock<std::mutex> hold(g_pool.lock);
        g_pool.job = nullptr;           // no worker takes hold of it from here on
        // the pieces a worker is still in, and the workers on their way out: short, so the wait is a look now and then, not a sleep
        while (job.left.load(std::memory_order_acquire) != 0 || job.users != 0) g_pool.done.wait_for(hold, std::chrono::microseconds(50));
    }
    g_pool.busy.store(false);
}

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

struct Core {
    std::vector<ULONG> ids;             // its logical processors' CPU set ids
    int klass = 0;                      // the system's efficiency class: the larger the faster
    ULONG index = 0;
};
std::vector<ULONG> g_fast, g_slow;      // CPU set ids: where the game's threads keep to, where the background does
bool g_differ;                          // the cores are of two kinds (or made so by HP2_CORES): the sets are applied
int g_fast_workers;                     // the first of the workers are on fast cores, the rest on slow ones

typedef BOOL(WINAPI *GetSets)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL(WINAPI *SetThreadSets)(HANDLE, const ULONG *, ULONG);
typedef BOOL(WINAPI *SetProcessSets)(HANDLE, const ULONG *, ULONG);
typedef BOOL(WINAPI *SetThreadInfo)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
SetThreadSets g_set_thread_sets;
SetThreadInfo g_set_thread_info;

// the system's hint: this thread's work is wanted now (the fast cores, the full clock), or may be saved on (the slow ones)
void quality(bool high)
{
    if (!g_set_thread_info) return;
    THREAD_POWER_THROTTLING_STATE s = {};
    s.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = high ? 0 : THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    g_set_thread_info(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof s);
}

void keep_to(const std::vector<ULONG> &sets)
{
    if (g_differ && g_set_thread_sets && !sets.empty()) g_set_thread_sets(GetCurrentThread(), sets.data(), (ULONG)sets.size());
}

void place_worker(int slot)
{
    quality(true);
    keep_to(slot <= g_fast_workers || g_slow.empty() ? g_fast : g_slow);
}

}   // namespace

void cores_thread(CoreKind kind)
{
    switch (kind) {
    case CORE_GAME:
    case CORE_RENDER:
        quality(true);
        keep_to(g_fast);
        break;
    case CORE_SOUND:
        quality(true);                  // short, and late is heard: any core, at once
        break;
    case CORE_BACKGROUND:
        if (g_differ) quality(false), keep_to(g_slow.empty() ? g_fast : g_slow);
        break;
    }
}

void cores_init()
{
    const HMODULE k32 = GetModuleHandleA("kernel32.dll");
    const GetSets get_sets = (GetSets)(void *)GetProcAddress(k32, "GetSystemCpuSetInformation");
    g_set_thread_sets = (SetThreadSets)(void *)GetProcAddress(k32, "SetThreadSelectedCpuSets");
    const SetProcessSets set_process_sets = (SetProcessSets)(void *)GetProcAddress(k32, "SetProcessDefaultCpuSets");
    g_set_thread_info = (SetThreadInfo)(void *)GetProcAddress(k32, "SetThreadInformation");

    // the cores, each with its logical processors
    std::vector<Core> cores;
    ULONG bytes = 0;
    if (get_sets) get_sets(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    std::vector<unsigned char> buf(bytes);
    if (get_sets && bytes && get_sets((PSYSTEM_CPU_SET_INFORMATION)buf.data(), bytes, &bytes, GetCurrentProcess(), 0)) {
        for (ULONG at = 0; at < bytes;) {
            const SYSTEM_CPU_SET_INFORMATION *e = (const SYSTEM_CPU_SET_INFORMATION *)(buf.data() + at);
            at += e->Size;
            if (e->Type != CpuSetInformation) continue;
            const ULONG index = (ULONG)e->CpuSet.Group << 16 | e->CpuSet.CoreIndex;
            Core *core = nullptr;
            for (Core &c : cores)
                if (c.index == index) core = &c;
            if (!core) cores.push_back({{}, e->CpuSet.EfficiencyClass, index}), core = &cores.back();
            core->ids.push_back(e->CpuSet.Id);
        }
    }
    int logical = 0;
    for (const Core &c : cores) logical += (int)c.ids.size();
    if (cores.empty()) {                // an older Windows: the count alone
        logical = (int)std::thread::hardware_concurrency();
        if (logical < 1) logical = 1;
    }
    int top = 0;
    for (const Core &c : cores) top = c.klass > top ? c.klass : top;
    int fast = 0, slow = 0;
    for (const Core &c : cores) (c.klass == top ? fast : slow)++;
    if (cores.empty()) fast = logical;

    // HP2_CORES=<fast>,<slow>: the first cores as the fast ones, the next as the slow ones (one logical processor each, as the E cores
    // have), the rest not used
    bool pretend = false;
    if (const char *e = getenv("HP2_CORES")) {
        int f = 0, s = 0;
        if (sscanf(e, "%d,%d", &f, &s) >= 1 && f >= 1 && !cores.empty()) {
            if (f > (int)cores.size()) f = (int)cores.size();
            if (s > (int)cores.size() - f) s = (int)cores.size() - f;
            if (s < 0) s = 0;
            for (int i = 0; i < f; i++) g_fast.insert(g_fast.end(), cores[i].ids.begin(), cores[i].ids.end());
            for (int i = f; i < f + s; i++) g_slow.push_back(cores[i].ids[0]);
            fast = f, slow = s;
            pretend = true;
            std::vector<ULONG> all = g_fast;
            all.insert(all.end(), g_slow.begin(), g_slow.end());
            if (set_process_sets) set_process_sets(GetCurrentProcess(), all.data(), (ULONG)all.size());
        }
    }
    if (!pretend)
        for (const Core &c : cores) {
            std::vector<ULONG> &to = c.klass == top ? g_fast : g_slow;
            to.insert(to.end(), c.ids.begin(), c.ids.end());
        }
    g_differ = pretend || slow > 0;

    // The workers: the game's thread does pieces too, so a core for each of the others, six at most; fast cores first. On one core
    // none: the pieces are done in order by the caller.
    const int most = 6;
    g_fast_workers = fast - 1 < most ? fast - 1 : most;
    if (g_fast_workers < 0) g_fast_workers = 0;
    int workers = g_fast_workers + slow;
    if (workers > most) workers = most;
    if (const char *e = getenv("HP2_THREADS")) {
        workers = atoi(e);
        if (workers < 0) workers = 0;
        if (workers > 15) workers = 15;
        if (g_fast_workers > workers) g_fast_workers = workers;
    }
    fprintf(g_hle_log, "cores: %d cores, %d threads: %d fast, %d slow%s; the game and the renderer on %s, %d workers (%d on fast cores)\n", fast + slow,
            pretend ? (int)(g_fast.size() + g_slow.size()) : logical, fast, slow, pretend ? " (as HP2_CORES says)" : "",
            g_differ ? "the fast ones" : "any", workers, g_fast_workers < workers ? g_fast_workers : workers);
    g_pool.workers = workers;
    for (int i = 1; i <= workers; i++) std::thread(worker, i).detach();
}

#else
namespace {
void place_worker(int) {}
}
void cores_thread(CoreKind) {}
void cores_init()
{
    // Android (later): the cores' capacities are in /sys/devices/system/cpu/cpu<N>/cpu_capacity, the threads placed by sched_setaffinity
    int n = (int)std::thread::hardware_concurrency();
    g_pool.workers = n > 1 ? (n - 1 < 6 ? n - 1 : 6) : 0;
    for (int i = 1; i <= g_pool.workers; i++) std::thread(worker, i).detach();
}
#endif
