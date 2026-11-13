// Which pages of the game's 32 MB were written since they were last looked at: so what the renderer keeps of the game's data (the strips
// the GPU's vertex cache holds) is known unchanged by its address alone, without reading its bytes again every frame.
//
// A page that something is to be known about is made read-only (memwatch_watch). The first write to it, from any thread, faults; the
// handler counts the page's generation up and makes it writable again, and the write goes on as if nothing had been. So "the generation
// is what it was when I read these bytes under the watch" says the bytes are what they were. A page whose watch keeps being taken off is
// left alone for a while: what lies on it is read as before.
//
// Sound only because nothing writes the game's memory but this process's own code: file data comes through buffers of the runtime's and
// is copied in (disc.cpp, hle_iop.cpp). A read by the system straight into a watched page would fail, not fault.
// Windows: VirtualProtect and a vectored exception handler. Elsewhere: none yet (memwatch_watch says no). HP2_NOMEMWATCH=1: none.
#pragma once
#include <cstdint>

extern uint32_t g_memwatch_gen[8192];           // a page of the 32 MB: the times a write took its watch off

// [addr, addr + bytes) of the 32 MB, two pages at most: watched from now on. False where it cannot be.
bool memwatch_watch(uint32_t addr, uint32_t bytes);
// the same for a longer range (a texture's pixels), up to 1 MB of it; what vouches for it is memwatch_sum
bool memwatch_watch_range(uint32_t addr, uint32_t bytes);
void memwatch_frame();                          // once a frame: the busy pages get their chance again in time
void memwatch_stats(uint64_t *faults, uint32_t *watched, uint32_t *busy);

// the generations of the range's first and last page: unchanged, with the pages watched when it was taken, says no byte was written
static inline uint64_t memwatch_stamp(uint32_t addr, uint32_t bytes)
{
    return (uint64_t)g_memwatch_gen[addr >> 12 & 8191] << 32 | g_memwatch_gen[(addr + bytes - 1) >> 12 & 8191];
}

// the generations of every page of the range, summed: they only go up, so the same sum, with the pages watched when it was taken, says
// no byte of the range was written
static inline uint64_t memwatch_sum(uint32_t addr, uint32_t bytes)
{
    uint64_t sum = 0;
    for (uint32_t p = addr >> 12, last = (addr + bytes - 1) >> 12; p <= last; p++) sum += g_memwatch_gen[p & 8191];
    return sum;
}
