// What the sound driver's generated code (tools/recomp/gen_iop.py, iop_stream_*.cpp) calls: the few instructions that are more than
// a line, and the way out to the IOP kernel's functions.
#pragma once
#include "iop.h"

#include <cstdint>

// An import's stub, or one of the kernel's return addresses: the reserved opcode there names a function of the kernel's, which is
// called as the processor would reach it, returning to $ra. The address to go on from.
u32 iop_hle_call(IopRegs &c, u32 pc);
// a word that is no instruction was reached: written down, the code stops
u32 iop_gen_bad(u32 pc, u32 op);
void iop_gen_syscall(u32 pc);
void iop_gen_break(u32 pc);

static inline void iop_gen_div(IopRegs &c, u32 a, u32 b)
{
    const s32 n = (s32)a, d = (s32)b;
    if (d == 0) { c.lo = n < 0 ? 1 : 0xFFFFFFFF; c.hi = (u32)n; }
    else if (n == INT32_MIN && d == -1) { c.lo = (u32)n; c.hi = 0; }
    else { c.lo = (u32)(n / d); c.hi = (u32)(n % d); }
}

static inline void iop_gen_divu(IopRegs &c, u32 a, u32 b)
{
    if (b == 0) { c.lo = 0xFFFFFFFF; c.hi = a; }
    else { c.lo = a / b; c.hi = a % b; }
}

static inline void iop_gen_lwl(u32 &reg, u32 addr)
{
    const u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
    reg = (reg & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}

static inline void iop_gen_lwr(u32 &reg, u32 addr)
{
    const u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
    reg = (reg & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}

static inline void iop_gen_swl(u32 addr, u32 v)
{
    const u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
    iop_sw(addr & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (v >> (24 - sh)));
}

static inline void iop_gen_swr(u32 addr, u32 v)
{
    const u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
    iop_sw(addr & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (v << sh));
}
