// memwatch.h
#include "memwatch.h"
#include "ps2.h"

#include <atomic>
#include <cstdlib>

uint32_t g_memwatch_gen[8192];

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

const uint32_t kPages = 8192;                   // 32 MB in pages of 4 KB (g_ram is aligned to one)
const uint8_t kBusy = 4;                        // watches taken off before the page is left alone
std::atomic<uint8_t> g_watched[kPages];
std::atomic<uint8_t> g_taken[kPages];           // how often lately
std::atomic<uint64_t> g_faults;
std::atomic<uint32_t> g_watched_n;
bool g_ready;
const bool g_off = getenv("HP2_NOMEMWATCH") != nullptr;

// A write to a watched page. The generation goes up before the page is writable again: whoever compares generations after the write
// has landed sees the new one.
LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at = (uintptr_t)r->ExceptionInformation[1], base = (uintptr_t)g_ram;
    if (at < base || at - base >= (uintptr_t)kPages << 12) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t page = (uint32_t)((at - base) >> 12);
    InterlockedIncrement((volatile LONG *)&g_memwatch_gen[page]);
    if (g_watched[page].exchange(0)) {
        g_watched_n--;
        if (g_taken[page] < 255) g_taken[page]++;
    }
    g_faults++;
    DWORD old;
    VirtualProtect((void *)(base + ((uintptr_t)page << 12)), 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

}   // namespace

static bool watch_pages(uint32_t addr, uint32_t bytes, uint32_t most)
{
    if (g_off || !bytes || addr >= 0x02000000u || bytes > 0x02000000u - addr) return false;
    const uint32_t first = addr >> 12, last = (addr + bytes - 1) >> 12;
    if (last - first >= most) return false;
    for (uint32_t p = first; p <= last; p++)
        if (g_taken[p] >= kBusy) return false;
    if (!g_ready) {
        if ((uintptr_t)g_ram & 4095) return false;
        AddVectoredExceptionHandler(1, on_fault);
        g_ready = true;
    }
    for (uint32_t p = first; p <= last; p++) {
        if (g_watched[p].exchange(1)) continue;
        g_watched_n++;
        DWORD old;
        if (!VirtualProtect(g_ram + ((size_t)p << 12), 4096, PAGE_READONLY, &old)) {
            if (g_watched[p].exchange(0)) g_watched_n--;
            return false;
        }
    }
    return true;
}

bool memwatch_watch(uint32_t addr, uint32_t bytes) { return watch_pages(addr, bytes, 2); }
bool memwatch_watch_range(uint32_t addr, uint32_t bytes) { return watch_pages(addr, bytes, 256); }

void memwatch_frame()
{
    static uint32_t frames;
    if (++frames % 1800) return;
    for (uint32_t p = 0; p < kPages; p++)
        if (uint8_t t = g_taken[p]) g_taken[p] = t / 2;
}

void memwatch_stats(uint64_t *faults, uint32_t *watched, uint32_t *busy)
{
    *faults = g_faults;
    *watched = g_watched_n;
    uint32_t n = 0;
    for (uint32_t p = 0; p < kPages; p++) n += g_taken[p] >= kBusy;
    *busy = n;
}

#else

bool memwatch_watch(uint32_t, uint32_t) { return false; }
bool memwatch_watch_range(uint32_t, uint32_t) { return false; }
void memwatch_frame() {}
void memwatch_stats(uint64_t *faults, uint32_t *watched, uint32_t *busy) { *faults = 0, *watched = 0, *busy = 0; }

#endif
