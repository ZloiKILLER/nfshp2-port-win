// PS2 EE runtime for the statically recompiled NFS Hot Pursuit 2 (SLUS_203.62).
//
// Ctx holds the full EE state the generated code touches: 128-bit GPRs,
// HI/LO pairs, the FPU, and VU0 in macro mode. Memory is the 32 MB main RAM
// plus the 16 KB scratchpad; anything else goes to the I/O handlers.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include "eefloat.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef unsigned __int128 u128;

union GPR {
    u128 q;
    u64 d[2];
    u32 w[4];
    u16 h[8];
    u8 b[16];
};

union FPR {
    float f;
    u32 u;
};

union VF {
    u128 q;
    float f[4];
    u32 u[4];
};

struct Ctx {
    GPR r[32];
    u64 hi, lo, hi1, lo1;
    u64 sa;
    FPR f[32];
    float acc;
    u32 fcr31;
    VF vf[32];
    u16 vi[16];
    VF vacc;
    float vq, vi_i;
    u32 vstatus, vmac, vclip;
};

struct FuncEntry {
    u32 addr;
    void (*fn)(Ctx &);
};
extern const FuncEntry g_functable[];
extern const int g_functable_size;

#define FCR31_C (1u << 23)

// ---- memory ---------------------------------------------------------------
// The game's whole address space is 4 GB of the host's in one piece: an address of the game's is an offset from g_ram, and the
// generated code reaches memory by it with no test (UL and US below). g_ram is an array of the program's image (mem_block.cpp), not
// a pointer and not a number: an array's address is a constant, so nothing is read again after a store, and the compiler knows that a
// store into it touches none of the program's own variables. (With a fixed number for the address the hot hooks of hot_math.cpp and
// native.cpp came out two to six times longer.) The array is guest 0 to 0x1200FFFF; the image is loaded at a fixed place and
// mem_reserve (first thing in main) reserves what follows it up to the end of the 4 GB. Memory is there at guest 0 to 0x0FFFFFFF
// (the PS2's 32 MB, and the PC's own memory past them, below), at the VU's memory and at the scratchpad; the hardware's registers and
// the mirrors are without access, so that a load or a store the generator did not expect there faults and is not lost.
// All of that only in a build with -DHP2_MEM4G=ON (recomp/CMakeLists.txt), which gen.py --direct needs. It is not the build that is
// released: the image at a fixed place is an exe without address randomization, which a system that demands it will not load, and the
// untested accesses were about 1 % in the build by counts. Without it, as before: three arrays, each where the linker puts it, and
// every access of the generated code tested for which of them it is in (L and S below).
#ifdef HP2_MEM4G
extern u8 g_ram[];
static u8 *const g_spr = g_ram + 0x70000000u;       // 16 KB scratchpad at 0x70000000
static u8 *const g_vumem = g_ram + 0x11000000u;     // VU0/VU1 micro and data memory, 0x11000000-0x1100FFFF
void mem_reserve();
#else
// Arrays, not pointers: every load and store of the game goes through these, and a pointer would be read again after each store.
// One block for guest 0 to 0x0FFFFFFF: the PS2's 32 MB, and the PC's own memory past them (below) right behind, at its own addresses.
extern u8 g_ram[0x10000000 + 64];
extern u8 g_spr[0x4000 + 64];       // 16 KB scratchpad at 0x70000000
extern u8 g_vumem[0x10000 + 64];    // VU0/VU1 micro and data memory, 0x11000000-0x1100FFFF
static inline void mem_reserve() {}
#endif

u64 io_read(u32 addr, int size);
void io_write(u32 addr, u64 value, int size);
u128 io_read128(u32 addr);
void io_write128(u32 addr, u128 value);

// The PC's own memory past the PS2's 32 MB, at guest 0x02000000 up to 0x0FFFFFFF: reserved whole, committed as it is needed. Only the port
// puts things there (the frame malloc buffers, native.cpp); the game's heap stays in the 32 MB. ext_commit(off, n): those bytes usable.
static const u32 EXT_BASE = 0x02000000, EXT_MAX = 0x0E000000;
extern u8 *g_ext;
extern u32 g_ext_size;
bool ext_commit(u32 off, u32 bytes);

// Where a guest address is in the host. Nearly every one is in the block (the game's data, its heap, the frame's buffers), and the
// stack is in the scratchpad: those two are tested for here, in that order; the mirrors (0x2..., 0x3..., kseg0, kseg1) and the VU's
// memory are host_slow's (ps2.cpp), and what is none of these is I/O (null).
u8 *host_slow(u32 a);
static inline u8 *host(u32 a)
{
    if (a < 0x10000000u) return g_ram + a;
    if ((a & 0xFFFFC000u) == 0x70000000u) return g_spr + (a & 0x3FFF);
    return host_slow(a);
}

// A load or a store: the block, then the scratchpad, in line; what is neither (a mirror, the VU's memory, I/O) is one call out of
// line, so that the thousands of accesses in the generated code are each a few instructions and not four ways of doing it.
u64 mem_read_slow(u32 a, int size);
void mem_write_slow(u32 a, u64 v, int size);
#define MEMR(T, n)                                                  \
    static inline T L##n(u32 a)                                     \
    {                                                               \
        T v;                                                        \
        if (__builtin_expect(a < 0x10000000u, 1)) { memcpy(&v, g_ram + a, sizeof v); return v; }                 \
        if ((a & 0xFFFFC000u) == 0x70000000u) { memcpy(&v, g_spr + (a & 0x3FFF), sizeof v); return v; }         \
        return (T)mem_read_slow(a, sizeof(T));                      \
    }
#define MEMW(T, n)                                                  \
    static inline void S##n(u32 a, T v)                             \
    {                                                               \
        if (__builtin_expect(a < 0x10000000u, 1)) { memcpy(g_ram + a, &v, sizeof v); return; }                   \
        if ((a & 0xFFFFC000u) == 0x70000000u) { memcpy(g_spr + (a & 0x3FFF), &v, sizeof v); return; }           \
        mem_write_slow(a, (u64)v, sizeof(T));                       \
    }
MEMR(u8, B) MEMR(u16, H) MEMR(u32, W) MEMR(u64, D)
MEMW(u8, B) MEMW(u16, H) MEMW(u32, W) MEMW(u64, D)

// through a host pointer already taken (gen.py's locals mode: the stack frame)
#define MEMP(T, n)                                                                                  static inline T PL##n(const u8 *p) { T v; memcpy(&v, p, sizeof v); return v; }                  static inline void PS##n(u8 *p, T v) { memcpy(p, &v, sizeof v); }
MEMP(u8, B) MEMP(u16, H) MEMP(u32, W) MEMP(u64, D)

// The generated code's accesses that no run has seen leave the block and the scratchpad (gen.py, slow_sites.txt): the address is the
// offset, nothing is tested.
#ifdef HP2_MEM4G
#define MEMU(T, n)                                                                                                         \
    static inline T UL##n(u32 a) { T v; memcpy(&v, g_ram + a, sizeof v); return v; }                                       \
    static inline void US##n(u32 a, T v) { memcpy(g_ram + a, &v, sizeof v); }
MEMU(u8, B) MEMU(u16, H) MEMU(u32, W) MEMU(u64, D)
static inline u128 ULQ(u32 a) { u128 v; memcpy(&v, g_ram + (a & ~15u), 16); return v; }
static inline void USQ(u32 a, u128 v) { memcpy(g_ram + (a & ~15u), &v, 16); }
#endif

static inline u128 LQ(u32 a)
{
    a &= ~15u;
    u8 *p = host(a);
    if (p) { u128 v; memcpy(&v, p, 16); return v; }
    return io_read128(a);
}
static inline void SQ(u32 a, u128 v)
{
    a &= ~15u;
    u8 *p = host(a);
    if (p) { memcpy(p, &v, 16); return; }
    io_write128(a, v);
}

// gen.py --sites: the same accesses as L and S, and the instruction (its address in the game) is noted when the access is not in the
// block or the scratchpad: HP2_SITELOG=<file> gets each such instruction once. The runs' lists together are slow_sites.txt.
void mem_site(u32 pc);
static inline bool mem_plain(u32 a) { return a < 0x10000000u || (a & 0xFFFFC000u) == 0x70000000u; }
#define MEMX(T, n)                                                                                                         \
    static inline T XL##n(u32 pc, u32 a) { if (__builtin_expect(!mem_plain(a), 0)) mem_site(pc); return L##n(a); }          \
    static inline void XS##n(u32 pc, u32 a, T v) { if (__builtin_expect(!mem_plain(a), 0)) mem_site(pc); S##n(a, v); }
MEMX(u8, B) MEMX(u16, H) MEMX(u32, W) MEMX(u64, D) MEMX(u128, Q)

// INTC cause bits raised by host timers; delivered at the next function entry
extern volatile u32 g_irq_pending;
void ps2_idle(Ctx &c);                  // a loop waiting for an interrupt: takes it, or sleeps until one comes
void ps2_take_interrupts(Ctx &c);
// EI/DI set and clear Status.EIE; interrupts wait while it is clear (the game
// guards its draw queue with DI), and EI takes the ones that came meanwhile
extern bool g_eie;
void op_ei(Ctx &c);
static inline void op_di(Ctx &) { g_eie = false; }

// ---- runtime services -------------------------------------------------------
void ps2_call(Ctx &c, u32 addr);
void ps2_syscall(Ctx &c, u32 pc);
void ps2_break(Ctx &c, u32 pc);
void ps2_unimpl(Ctx &c, u32 pc, u32 word);
u32 op_mfc0(Ctx &c, int rd);
void op_mtc0(Ctx &c, int rd, u32 v);
void vu0_callms(Ctx &c, u32 addr);

static inline void wr64(Ctx &c, int r, u64 v)
{
    if (r) c.r[r].d[0] = v;
}
static inline u64 sx32(u32 v) { return (u64)(s64)(s32)v; }

// ---- integer multiply / divide (pipeline 0 or 1) ----------------------------
static inline void set_hilo(Ctx &c, int p, u64 lo, u64 hi)
{
    if (p) { c.lo1 = lo; c.hi1 = hi; } else { c.lo = lo; c.hi = hi; }
}
static inline void op_mult(Ctx &c, int rd, int rs, int rt, int p)
{
    s64 r = (s64)(s32)c.r[rs].w[0] * (s64)(s32)c.r[rt].w[0];
    set_hilo(c, p, sx32((u32)r), sx32((u32)(r >> 32)));
    wr64(c, rd, sx32((u32)r));
}
static inline void op_multu(Ctx &c, int rd, int rs, int rt, int p)
{
    u64 r = (u64)c.r[rs].w[0] * (u64)c.r[rt].w[0];
    set_hilo(c, p, sx32((u32)r), sx32((u32)(r >> 32)));
    wr64(c, rd, sx32((u32)r));
}
static inline void op_madd(Ctx &c, int rd, int rs, int rt, int p)
{
    u64 &lo = p ? c.lo1 : c.lo, &hi = p ? c.hi1 : c.hi;
    s64 acc = (s64)(((u64)(u32)hi << 32) | (u32)lo);
    s64 r = acc + (s64)(s32)c.r[rs].w[0] * (s64)(s32)c.r[rt].w[0];
    lo = sx32((u32)r);
    hi = sx32((u32)(r >> 32));
    wr64(c, rd, lo);
}
static inline void op_maddu(Ctx &c, int rd, int rs, int rt, int p)
{
    u64 &lo = p ? c.lo1 : c.lo, &hi = p ? c.hi1 : c.hi;
    u64 acc = ((u64)(u32)hi << 32) | (u32)lo;
    u64 r = acc + (u64)c.r[rs].w[0] * (u64)c.r[rt].w[0];
    lo = sx32((u32)r);
    hi = sx32((u32)(r >> 32));
    wr64(c, rd, lo);
}
static inline void op_div(Ctx &c, int rs, int rt, int p)
{
    s32 n = (s32)c.r[rs].w[0], d = (s32)c.r[rt].w[0];
    if (d == 0)
        set_hilo(c, p, sx32(n < 0 ? 1 : (u32)-1), sx32((u32)n));
    else if (n == INT32_MIN && d == -1)
        set_hilo(c, p, sx32((u32)n), 0);
    else
        set_hilo(c, p, sx32((u32)(n / d)), sx32((u32)(n % d)));
}
static inline void op_divu(Ctx &c, int rs, int rt, int p)
{
    u32 n = c.r[rs].w[0], d = c.r[rt].w[0];
    if (d == 0)
        set_hilo(c, p, sx32(0xFFFFFFFF), sx32(n));
    else
        set_hilo(c, p, sx32(n / d), sx32(n % d));
}

// The same on values, for a function that keeps its registers in locals (gen.py's locals mode): the result is the function's own
// to store, hi and lo stay in the Ctx.
static inline u64 opl_mult(Ctx &c, u32 s, u32 t, int p)
{
    s64 r = (s64)(s32)s * (s64)(s32)t;
    set_hilo(c, p, sx32((u32)r), sx32((u32)(r >> 32)));
    return sx32((u32)r);
}
static inline u64 opl_multu(Ctx &c, u32 s, u32 t, int p)
{
    u64 r = (u64)s * (u64)t;
    set_hilo(c, p, sx32((u32)r), sx32((u32)(r >> 32)));
    return sx32((u32)r);
}
static inline u64 opl_madd(Ctx &c, u32 s, u32 t, int p)
{
    u64 &lo = p ? c.lo1 : c.lo, &hi = p ? c.hi1 : c.hi;
    s64 acc = (s64)(((u64)(u32)hi << 32) | (u32)lo);
    s64 r = acc + (s64)(s32)s * (s64)(s32)t;
    lo = sx32((u32)r);
    hi = sx32((u32)(r >> 32));
    return lo;
}
static inline u64 opl_maddu(Ctx &c, u32 s, u32 t, int p)
{
    u64 &lo = p ? c.lo1 : c.lo, &hi = p ? c.hi1 : c.hi;
    u64 acc = ((u64)(u32)hi << 32) | (u32)lo;
    u64 r = acc + (u64)s * (u64)t;
    lo = sx32((u32)r);
    hi = sx32((u32)(r >> 32));
    return lo;
}
static inline void opl_div(Ctx &c, u32 s, u32 t, int p)
{
    s32 n = (s32)s, d = (s32)t;
    if (d == 0)
        set_hilo(c, p, sx32(n < 0 ? 1 : (u32)-1), sx32((u32)n));
    else if (n == INT32_MIN && d == -1)
        set_hilo(c, p, sx32((u32)n), 0);
    else
        set_hilo(c, p, sx32((u32)(n / d)), sx32((u32)(n % d)));
}
static inline void opl_divu(Ctx &c, u32 n, u32 d, int p)
{
    if (d == 0)
        set_hilo(c, p, sx32(0xFFFFFFFF), sx32(n));
    else
        set_hilo(c, p, sx32(n / d), sx32(n % d));
}

// ---- unaligned loads / stores (little endian) -------------------------------
static inline void op_lwl(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = 0x00FFFFFFu >> sh;
    u32 v = (c.r[rt].w[0] & mask) | (w << (24 - sh));
    wr64(c, rt, sx32(v));
}
static inline void op_lwr(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = sh ? 0xFFFFFFFFu << (32 - sh) : 0;
    u32 v = (c.r[rt].w[0] & mask) | (w >> sh);
    if (sh == 0) wr64(c, rt, sx32(v));
    else if (rt) c.r[rt].w[0] = v;
}
static inline void op_ldl(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = 0x00FFFFFFFFFFFFFFull >> sh;
    wr64(c, rt, (c.r[rt].d[0] & mask) | (d << (56 - sh)));
}
static inline void op_ldr(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = sh ? 0xFFFFFFFFFFFFFFFFull << (64 - sh) : 0;
    wr64(c, rt, (c.r[rt].d[0] & mask) | (d >> sh));
}
static inline void op_swl(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = sh == 24 ? 0 : 0xFFFFFFFFu << (sh + 8);
    SW(a & ~3u, (w & mask) | (c.r[rt].w[0] >> (24 - sh)));
}
static inline void op_swr(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = sh ? 0xFFFFFFFFu >> (32 - sh) : 0;
    SW(a & ~3u, (w & mask) | (c.r[rt].w[0] << sh));
}
static inline void op_sdl(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = sh == 56 ? 0 : 0xFFFFFFFFFFFFFFFFull << (sh + 8);
    SD(a & ~7u, (d & mask) | (c.r[rt].d[0] >> (56 - sh)));
}
static inline void op_sdr(Ctx &c, int rt, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = sh ? 0xFFFFFFFFFFFFFFFFull >> (64 - sh) : 0;
    SD(a & ~7u, (d & mask) | (c.r[rt].d[0] << sh));
}

// the unaligned ones on a register by reference (locals mode)
static inline void opl_lwl(GPR &r, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = 0x00FFFFFFu >> sh;
    r.d[0] = sx32((r.w[0] & mask) | (w << (24 - sh)));
}
static inline void opl_lwr(GPR &r, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = sh ? 0xFFFFFFFFu << (32 - sh) : 0;
    u32 v = (r.w[0] & mask) | (w >> sh);
    if (sh == 0) r.d[0] = sx32(v);
    else r.w[0] = v;
}
static inline void opl_ldl(GPR &r, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = 0x00FFFFFFFFFFFFFFull >> sh;
    r.d[0] = (r.d[0] & mask) | (d << (56 - sh));
}
static inline void opl_ldr(GPR &r, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = sh ? 0xFFFFFFFFFFFFFFFFull << (64 - sh) : 0;
    r.d[0] = (r.d[0] & mask) | (d >> sh);
}
static inline void opl_swl(u32 v, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = sh == 24 ? 0 : 0xFFFFFFFFu << (sh + 8);
    SW(a & ~3u, (w & mask) | (v >> (24 - sh)));
}
static inline void opl_swr(u32 v, u32 a)
{
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    u32 mask = sh ? 0xFFFFFFFFu >> (32 - sh) : 0;
    SW(a & ~3u, (w & mask) | (v << sh));
}
static inline void opl_sdl(u64 v, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = sh == 56 ? 0 : 0xFFFFFFFFFFFFFFFFull << (sh + 8);
    SD(a & ~7u, (d & mask) | (v >> (56 - sh)));
}
static inline void opl_sdr(u64 v, u32 a)
{
    u32 sh = (a & 7) * 8;
    u64 d = LD(a & ~7u);
    u64 mask = sh ? 0xFFFFFFFFFFFFFFFFull >> (64 - sh) : 0;
    SD(a & ~7u, (d & mask) | (v << sh));
}

// ---- FPU (COP1) ---------------------------------------------------------------
// The EE FPU has no NaN/Inf (fclamp), and its add, sub, mul and madd are the EE's to the bit (eefloat.h).
#define F(n) c.f[n].f
static inline void FPU_add_s(Ctx &c, int d, int s, int t) { F(d) = ee_add(F(s), F(t)); }
static inline void FPU_sub_s(Ctx &c, int d, int s, int t) { F(d) = ee_sub(F(s), F(t)); }
static inline void FPU_mul_s(Ctx &c, int d, int s, int t) { F(d) = ee_mul(F(s), F(t)); }
static inline void FPU_div_s(Ctx &c, int d, int s, int t) { F(d) = ee_div(F(s), F(t)); }
static inline void FPU_sqrt_s(Ctx &c, int d, int, int t) { F(d) = ee_sqrt(F(t)); }
static inline void FPU_rsqrt_s(Ctx &c, int d, int s, int t) { F(d) = ee_rsqrt(F(s), F(t)); }
static inline void FPU_abs_s(Ctx &c, int d, int s, int) { F(d) = std::fabs(F(s)); }
static inline void FPU_mov_s(Ctx &c, int d, int s, int) { c.f[d].u = c.f[s].u; }
static inline void FPU_neg_s(Ctx &c, int d, int s, int) { c.f[d].u = c.f[s].u ^ 0x80000000u; }
static inline void FPU_adda_s(Ctx &c, int, int s, int t) { c.acc = ee_add(F(s), F(t)); }
static inline void FPU_suba_s(Ctx &c, int, int s, int t) { c.acc = ee_sub(F(s), F(t)); }
static inline void FPU_mula_s(Ctx &c, int, int s, int t) { c.acc = ee_mul(F(s), F(t)); }
static inline void FPU_madd_s(Ctx &c, int d, int s, int t) { F(d) = ee_madd(c.acc, F(s), F(t)); }
static inline void FPU_msub_s(Ctx &c, int d, int s, int t) { F(d) = ee_msub(c.acc, F(s), F(t)); }
static inline void FPU_madda_s(Ctx &c, int, int s, int t) { c.acc = ee_madd(c.acc, F(s), F(t)); }
static inline void FPU_msuba_s(Ctx &c, int, int s, int t) { c.acc = ee_msub(c.acc, F(s), F(t)); }
#ifdef EE_CHECK
#define EE_GT(a, b) (g_ee_chk & 2 ? ee_cmp(a, b) > 0 : (a) > (b))
#define EE_LT(a, b) (g_ee_chk & 2 ? ee_cmp(a, b) < 0 : (a) < (b))
#define EE_LE(a, b) (g_ee_chk & 2 ? ee_cmp(a, b) <= 0 : (a) <= (b))
#define EE_EQ(a, b) (g_ee_chk & 2 ? ee_cmp(a, b) == 0 : (a) == (b))
#else
#define EE_GT(a, b) ((a) > (b))
#define EE_LT(a, b) ((a) < (b))
#define EE_LE(a, b) ((a) <= (b))
#define EE_EQ(a, b) ((a) == (b))
#endif
static inline void FPU_max_s(Ctx &c, int d, int s, int t) { F(d) = EE_GT(F(s), F(t)) ? F(s) : F(t); }
static inline void FPU_min_s(Ctx &c, int d, int s, int t) { F(d) = EE_LT(F(s), F(t)) ? F(s) : F(t); }
static inline void FPU_cvt_w_s(Ctx &c, int d, int s, int)
{
    float v = F(s);
#ifdef EE_CHECK
    if ((c.f[s].u >> 23 & 255) >= 158) {        // 2^31 and up, the values of exponent 255 with them
        c.f[d].u = c.f[s].u >> 31 ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
#endif
    c.f[d].u = v >= 2147483647.0f ? 0x7FFFFFFFu : v <= -2147483648.0f ? 0x80000000u : (u32)(s32)v;
}
static inline void FPU_cvt_s_w(Ctx &c, int d, int s, int) { F(d) = (float)(s32)c.f[s].u; }
static inline void fcond(Ctx &c, bool b) { c.fcr31 = b ? (c.fcr31 | FCR31_C) : (c.fcr31 & ~FCR31_C); }
static inline void FPU_c_f_s(Ctx &c, int, int, int) { fcond(c, false); }
static inline void FPU_c_eq_s(Ctx &c, int, int s, int t) { fcond(c, EE_EQ(F(s), F(t))); }
static inline void FPU_c_lt_s(Ctx &c, int, int s, int t) { fcond(c, EE_LT(F(s), F(t))); }
static inline void FPU_c_le_s(Ctx &c, int, int s, int t) { fcond(c, EE_LE(F(s), F(t))); }
#undef F
// The same on registers by reference (locals mode): what each reads and writes is what its twin above does, the accumulator and the
// condition bit stay in the Ctx.
static inline void FPL_add_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = ee_add(s.f, t.f); }
static inline void FPL_sub_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = ee_sub(s.f, t.f); }
static inline void FPL_mul_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = ee_mul(s.f, t.f); }
static inline void FPL_div_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = ee_div(s.f, t.f); }
static inline void FPL_sqrt_s(Ctx &, FPR &d, const FPR &, const FPR &t) { d.f = ee_sqrt(t.f); }
static inline void FPL_rsqrt_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = ee_rsqrt(s.f, t.f); }
static inline void FPL_abs_s(Ctx &, FPR &d, const FPR &s, const FPR &) { d.f = std::fabs(s.f); }
static inline void FPL_mov_s(Ctx &, FPR &d, const FPR &s, const FPR &) { d.u = s.u; }
static inline void FPL_neg_s(Ctx &, FPR &d, const FPR &s, const FPR &) { d.u = s.u ^ 0x80000000u; }
static inline void FPL_adda_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { c.acc = ee_add(s.f, t.f); }
static inline void FPL_suba_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { c.acc = ee_sub(s.f, t.f); }
static inline void FPL_mula_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { c.acc = ee_mul(s.f, t.f); }
static inline void FPL_madd_s(Ctx &c, FPR &d, const FPR &s, const FPR &t) { d.f = ee_madd(c.acc, s.f, t.f); }
static inline void FPL_msub_s(Ctx &c, FPR &d, const FPR &s, const FPR &t) { d.f = ee_msub(c.acc, s.f, t.f); }
static inline void FPL_madda_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { c.acc = ee_madd(c.acc, s.f, t.f); }
static inline void FPL_msuba_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { c.acc = ee_msub(c.acc, s.f, t.f); }
static inline void FPL_max_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = EE_GT(s.f, t.f) ? s.f : t.f; }
static inline void FPL_min_s(Ctx &, FPR &d, const FPR &s, const FPR &t) { d.f = EE_LT(s.f, t.f) ? s.f : t.f; }
static inline void FPL_cvt_w_s(Ctx &, FPR &d, const FPR &s, const FPR &)
{
    float v = s.f;
#ifdef EE_CHECK
    if ((s.u >> 23 & 255) >= 158) {
        d.u = s.u >> 31 ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
#endif
    d.u = v >= 2147483647.0f ? 0x7FFFFFFFu : v <= -2147483648.0f ? 0x80000000u : (u32)(s32)v;
}
static inline void FPL_cvt_s_w(Ctx &, FPR &d, const FPR &s, const FPR &) { d.f = (float)(s32)s.u; }
static inline void FPL_c_f_s(Ctx &c, FPR &, const FPR &, const FPR &) { fcond(c, false); }
static inline void FPL_c_eq_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { fcond(c, EE_EQ(s.f, t.f)); }
static inline void FPL_c_lt_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { fcond(c, EE_LT(s.f, t.f)); }
static inline void FPL_c_le_s(Ctx &c, FPR &, const FPR &s, const FPR &t) { fcond(c, EE_LE(s.f, t.f)); }
static inline u32 op_cfc1(Ctx &c, int rd) { return rd == 31 ? c.fcr31 : rd == 0 ? 0x2E00u : 0; }
static inline void op_ctc1(Ctx &c, int rd, u32 v) { if (rd == 31) c.fcr31 = v; }

// ---- VU0 macro mode (COP2) -------------------------------------------------
#define VDEST(w) (((w) >> 21) & 15)
#define VFT(w) (((w) >> 16) & 31)
#define VFS(w) (((w) >> 11) & 31)
#define VFD(w) (((w) >> 6) & 31)
#define VBC(w) ((w) & 3)
static inline bool dx(u32 d, int i) { return d & (8 >> i); }    // x=8 y=4 z=2 w=1
static inline void vf_set(Ctx &c, int r, int i, float v) { if (r) c.vf[r].f[i] = fclamp(v); }
static inline void op_lqc2(Ctx &c, int r, u32 a) { if (r) c.vf[r].q = LQ(a); }
static inline void op_qmtc2(Ctx &c, int r, u128 v) { if (r) c.vf[r].q = v; }
u32 op_cfc2(Ctx &c, int rd);
void op_ctc2(Ctx &c, int rd, u32 v);

// Four lanes at once for the arithmetic ones: ee_add4, ee_sub4 and ee_mul4 are the scalar ones lane by lane (eefloat.h), so a vector
// operation is one of them and its result put into the lanes the destination mask names, each clamped as vf_set clamps it. A lane
// the mask leaves out is worked out and dropped. (-DEE_CHECK keeps the lane-by-lane way, which that build measures.)
static inline ee_v4f vf_get(const VF &v)
{
    ee_v4f r;
    memcpy(&r, &v, 16);
    return r;
}
static inline void vf_put(VF &to, u32 m, ee_v4f v)
{
    v = ee_clamp4(v);
    if (m == 15) {
        memcpy(&to, &v, 16);
        return;
    }
    for (int i = 0; i < 4; i++)
        if (dx(m, i)) to.f[i] = v[i];
}
static inline ee_v4f vf_splat(float y) { return ee_v4f{y, y, y, y}; }
#define VU_BIN4(name, expr)                                                        \
    static inline void VU_##name(Ctx &c, u32 w)                                    \
    {                                                                              \
        int d = VFD(w); ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_get(c.vf[VFT(w)]), acc = vf_get(c.vacc); (void)acc; \
        if (d) vf_put(c.vf[d], VDEST(w), expr);                                    \
    }                                                                              \
    static inline void VU_##name##bc(Ctx &c, u32 w)                                \
    {                                                                              \
        int d = VFD(w); ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_splat(c.vf[VFT(w)].f[VBC(w)]), acc = vf_get(c.vacc); (void)acc; \
        if (d) vf_put(c.vf[d], VDEST(w), expr);                                    \
    }                                                                              \
    static inline void VU_##name##q(Ctx &c, u32 w)                                 \
    {                                                                              \
        int d = VFD(w); ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_splat(c.vq), acc = vf_get(c.vacc); (void)acc; \
        if (d) vf_put(c.vf[d], VDEST(w), expr);                                    \
    }                                                                              \
    static inline void VU_##name##i(Ctx &c, u32 w)                                 \
    {                                                                              \
        int d = VFD(w); ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_splat(c.vi_i), acc = vf_get(c.vacc); (void)acc; \
        if (d) vf_put(c.vf[d], VDEST(w), expr);                                    \
    }
#define VU_ACC4(name, expr)                                                        \
    static inline void VU_##name(Ctx &c, u32 w)                                    \
    {                                                                              \
        ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_get(c.vf[VFT(w)]), acc = vf_get(c.vacc); (void)acc; \
        vf_put(c.vacc, VDEST(w), expr);                                            \
    }                                                                              \
    static inline void VU_##name##bc(Ctx &c, u32 w)                                \
    {                                                                              \
        ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_splat(c.vf[VFT(w)].f[VBC(w)]), acc = vf_get(c.vacc); (void)acc; \
        vf_put(c.vacc, VDEST(w), expr);                                            \
    }                                                                              \
    static inline void VU_##name##q(Ctx &c, u32 w)                                 \
    {                                                                              \
        ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_splat(c.vq), acc = vf_get(c.vacc); (void)acc; \
        vf_put(c.vacc, VDEST(w), expr);                                            \
    }                                                                              \
    static inline void VU_##name##i(Ctx &c, u32 w)                                 \
    {                                                                              \
        ee_v4f x = vf_get(c.vf[VFS(w)]), y = vf_splat(c.vi_i), acc = vf_get(c.vacc); (void)acc; \
        vf_put(c.vacc, VDEST(w), expr);                                            \
    }

// binary ops: full vector, broadcast field, Q or I
#define VU_BIN(name, expr)                                                         \
    static inline void VU_##name(Ctx &c, u32 w)                                    \
    {                                                                              \
        int s = VFS(w), t = VFT(w), d = VFD(w); u32 m = VDEST(w);                  \
        VF a = c.vf[s], b = c.vf[t];                                               \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i], y = b.f[i]; (void)y; vf_set(c, d, i, expr); } \
    }                                                                              \
    static inline void VU_##name##bc(Ctx &c, u32 w)                                \
    {                                                                              \
        int s = VFS(w), d = VFD(w); u32 m = VDEST(w);                              \
        VF a = c.vf[s]; float y = c.vf[VFT(w)].f[VBC(w)];                          \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i]; vf_set(c, d, i, expr); } \
    }                                                                              \
    static inline void VU_##name##q(Ctx &c, u32 w)                                 \
    {                                                                              \
        int s = VFS(w), d = VFD(w); u32 m = VDEST(w);                              \
        VF a = c.vf[s]; float y = c.vq;                                            \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i]; vf_set(c, d, i, expr); } \
    }                                                                              \
    static inline void VU_##name##i(Ctx &c, u32 w)                                 \
    {                                                                              \
        int s = VFS(w), d = VFD(w); u32 m = VDEST(w);                              \
        VF a = c.vf[s]; float y = c.vi_i;                                          \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i]; vf_set(c, d, i, expr); } \
    }
#ifdef EE_CHECK
VU_BIN(vadd, ee_add(x, y))
VU_BIN(vsub, ee_sub(x, y))
VU_BIN(vmul, ee_mul(x, y))
#else
VU_BIN4(vadd, ee_add4(x, y))
VU_BIN4(vsub, ee_sub4(x, y))
VU_BIN4(vmul, ee_mul4(x, y))
#endif
VU_BIN(vmax, EE_GT(x, y) ? x : y)
VU_BIN(vmini, EE_LT(x, y) ? x : y)
#ifdef EE_CHECK
#define ACC (c.vacc.f[i])
VU_BIN(vmadd, ee_madd(ACC, x, y))
VU_BIN(vmsub, ee_msub(ACC, x, y))
#undef ACC
#else
VU_BIN4(vmadd, ee_add4(acc, ee_mul4(x, y)))
VU_BIN4(vmsub, ee_sub4(acc, ee_mul4(x, y)))
#endif
#define BC_ALIAS(op, base)                                                        \
    static inline void VU_##op##x(Ctx &c, u32 w) { VU_##base##bc(c, w); }        \
    static inline void VU_##op##y(Ctx &c, u32 w) { VU_##base##bc(c, w); }        \
    static inline void VU_##op##z(Ctx &c, u32 w) { VU_##base##bc(c, w); }        \
    static inline void VU_##op##w(Ctx &c, u32 w) { VU_##base##bc(c, w); }
BC_ALIAS(vadd, vadd) BC_ALIAS(vsub, vsub) BC_ALIAS(vmul, vmul) BC_ALIAS(vmax, vmax)
BC_ALIAS(vmini, vmini) BC_ALIAS(vmadd, vmadd) BC_ALIAS(vmsub, vmsub)

// accumulator forms: ACC = op(fs, ft)
#define VU_ACC(name, expr)                                                         \
    static inline void VU_##name(Ctx &c, u32 w)                                    \
    {                                                                              \
        int s = VFS(w), t = VFT(w); u32 m = VDEST(w); VF a = c.vf[s], b = c.vf[t]; \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i], y = b.f[i], acc = c.vacc.f[i]; (void)acc; c.vacc.f[i] = fclamp(expr); } \
    }                                                                              \
    static inline void VU_##name##bc(Ctx &c, u32 w)                                \
    {                                                                              \
        int s = VFS(w); u32 m = VDEST(w); VF a = c.vf[s]; float y = c.vf[VFT(w)].f[VBC(w)]; \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i], acc = c.vacc.f[i]; (void)acc; c.vacc.f[i] = fclamp(expr); } \
    }                                                                              \
    static inline void VU_##name##q(Ctx &c, u32 w)                                 \
    {                                                                              \
        int s = VFS(w); u32 m = VDEST(w); VF a = c.vf[s]; float y = c.vq;          \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i], acc = c.vacc.f[i]; (void)acc; c.vacc.f[i] = fclamp(expr); } \
    }                                                                              \
    static inline void VU_##name##i(Ctx &c, u32 w)                                 \
    {                                                                              \
        int s = VFS(w); u32 m = VDEST(w); VF a = c.vf[s]; float y = c.vi_i;        \
        for (int i = 0; i < 4; i++) if (dx(m, i)) { float x = a.f[i], acc = c.vacc.f[i]; (void)acc; c.vacc.f[i] = fclamp(expr); } \
    }
#ifdef EE_CHECK
VU_ACC(vadda, ee_add(x, y))
VU_ACC(vsuba, ee_sub(x, y))
VU_ACC(vmula, ee_mul(x, y))
VU_ACC(vmadda, ee_madd(acc, x, y))
VU_ACC(vmsuba, ee_msub(acc, x, y))
#else
VU_ACC4(vadda, ee_add4(x, y))
VU_ACC4(vsuba, ee_sub4(x, y))
VU_ACC4(vmula, ee_mul4(x, y))
VU_ACC4(vmadda, ee_add4(acc, ee_mul4(x, y)))
VU_ACC4(vmsuba, ee_sub4(acc, ee_mul4(x, y)))
#endif
BC_ALIAS(vadda, vadda) BC_ALIAS(vsuba, vsuba) BC_ALIAS(vmula, vmula)
BC_ALIAS(vmadda, vmadda) BC_ALIAS(vmsuba, vmsuba)

// outer product: OPMULA.xyz ACC = fs.yzx * ft.zxy; OPMSUB fd = ACC - fs.yzx * ft.zxy
// (OPMULA ACC,a,b then OPMSUB d,b,a is the cross product a x b)
static inline void VU_vopmula(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)], b = c.vf[VFT(w)];
    c.vacc.f[0] = ee_mul(a.f[1], b.f[2]);
    c.vacc.f[1] = ee_mul(a.f[2], b.f[0]);
    c.vacc.f[2] = ee_mul(a.f[0], b.f[1]);
}
static inline void VU_vopmsub(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)], b = c.vf[VFT(w)];
    int d = VFD(w);
    float r0 = ee_msub(c.vacc.f[0], a.f[1], b.f[2]);
    float r1 = ee_msub(c.vacc.f[1], a.f[2], b.f[0]);
    float r2 = ee_msub(c.vacc.f[2], a.f[0], b.f[1]);
    vf_set(c, d, 0, r0);
    vf_set(c, d, 1, r1);
    vf_set(c, d, 2, r2);
}
static inline void VU_vmove(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)];
    for (int i = 0; i < 4; i++) if (dx(VDEST(w), i) && VFT(w)) c.vf[VFT(w)].u[i] = a.u[i];
}
static inline void VU_vmr32(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)];
    for (int i = 0; i < 4; i++) if (dx(VDEST(w), i) && VFT(w)) c.vf[VFT(w)].u[i] = a.u[(i + 1) & 3];
}
static inline void VU_vabs(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)];
    for (int i = 0; i < 4; i++) if (dx(VDEST(w), i)) vf_set(c, VFT(w), i, std::fabs(a.f[i]));
}
// fsf/ftf field selectors live in bits 21-22 and 23-24
static inline float vfsf(Ctx &c, u32 w) { return c.vf[VFS(w)].f[(w >> 21) & 3]; }
static inline float vftf(Ctx &c, u32 w) { return c.vf[VFT(w)].f[(w >> 23) & 3]; }
static inline void VU_vdiv(Ctx &c, u32 w)
{
    c.vq = ee_div(vfsf(c, w), vftf(c, w));
}
static inline void VU_vsqrt(Ctx &c, u32 w) { c.vq = ee_sqrt(vftf(c, w)); }
static inline void VU_vrsqrt(Ctx &c, u32 w) { c.vq = ee_rsqrt(vfsf(c, w), vftf(c, w)); }
static inline void VU_vwaitq(Ctx &, u32) {}
static inline void VU_vnop(Ctx &, u32) {}
static inline void VU_vclipw(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)];
    float lim = std::fabs(c.vf[VFT(w)].f[3]);
    u32 f = 0;
    for (int i = 0; i < 3; i++) {
        if (a.f[i] > lim) f |= 1u << (i * 2);
        if (a.f[i] < -lim) f |= 2u << (i * 2);
    }
    c.vclip = ((c.vclip << 6) | f) & 0xFFFFFF;
}
#define VU_FTOI(n, sh)                                                              \
    static inline void VU_vftoi##n(Ctx &c, u32 w)                                   \
    {                                                                               \
        VF a = c.vf[VFS(w)];                                                        \
        for (int i = 0; i < 4; i++) if (dx(VDEST(w), i) && VFT(w)) {               \
            double v = (double)a.f[i] * (1 << sh);                                  \
            c.vf[VFT(w)].u[i] = v >= 2147483647.0 ? 0x7FFFFFFFu : v <= -2147483648.0 ? 0x80000000u : (u32)(s32)v; } \
    }                                                                               \
    static inline void VU_vitof##n(Ctx &c, u32 w)                                   \
    {                                                                               \
        VF a = c.vf[VFS(w)];                                                        \
        for (int i = 0; i < 4; i++) if (dx(VDEST(w), i)) vf_set(c, VFT(w), i, (float)(s32)a.u[i] / (1 << sh)); \
    }
VU_FTOI(0, 0) VU_FTOI(4, 4) VU_FTOI(12, 12) VU_FTOI(15, 15)
static inline void VU_vlqi(Ctx &c, u32 w)
{
    int is = VFS(w);
    u128 v = LQ((u32)c.vi[is] * 16);
    for (int i = 0; i < 4; i++) if (dx(VDEST(w), i) && VFT(w)) c.vf[VFT(w)].u[i] = ((VF *)&v)->u[i];
    if (is) c.vi[is]++;
}
static inline void VU_vsqi(Ctx &c, u32 w)
{
    int it = VFT(w);
    SQ((u32)c.vi[it] * 16, c.vf[VFS(w)].q);
    if (it) c.vi[it]++;
}
static inline void VU_viadd(Ctx &c, u32 w) { int d = VFD(w); if (d) c.vi[d] = c.vi[VFS(w)] + c.vi[VFT(w)]; }
static inline void VU_visub(Ctx &c, u32 w) { int d = VFD(w); if (d) c.vi[d] = c.vi[VFS(w)] - c.vi[VFT(w)]; }
static inline void VU_viand(Ctx &c, u32 w) { int d = VFD(w); if (d) c.vi[d] = c.vi[VFS(w)] & c.vi[VFT(w)]; }
static inline void VU_vior(Ctx &c, u32 w) { int d = VFD(w); if (d) c.vi[d] = c.vi[VFS(w)] | c.vi[VFT(w)]; }
static inline void VU_vcallms(Ctx &c, u32 w) { vu0_callms(c, ((w >> 6) & 0x7FFF) * 8); }
static inline void VU_vcallmsr(Ctx &c, u32) { vu0_callms(c, op_cfc2(c, 27) * 8); }

// ---- MMI (128-bit parallel integer) -------------------------------------------
#define MMI_FN(name) static inline void MMI_##name(Ctx &c, int rd, int rs, int rt, int sa)
#define RQ(n) c.r[n]
MMI_FN(pextlw) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return; GPR &d = RQ(rd);
    d.w[0] = t.w[0]; d.w[1] = s.w[0]; d.w[2] = t.w[1]; d.w[3] = s.w[1]; }
MMI_FN(pextuw) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return; GPR &d = RQ(rd);
    d.w[0] = t.w[2]; d.w[1] = s.w[2]; d.w[2] = t.w[3]; d.w[3] = s.w[3]; }
MMI_FN(pcpyld) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return; RQ(rd).d[0] = t.d[0]; RQ(rd).d[1] = s.d[0]; }
MMI_FN(pcpyud) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return; RQ(rd).d[0] = s.d[1]; RQ(rd).d[1] = t.d[1]; }
MMI_FN(pcpyh) { (void)sa; (void)rs; GPR t = RQ(rt); if (!rd) return;
    for (int i = 0; i < 4; i++) { RQ(rd).h[i] = t.h[0]; RQ(rd).h[4 + i] = t.h[4]; } }
MMI_FN(pand) { (void)sa; if (rd) RQ(rd).q = RQ(rs).q & RQ(rt).q; }
MMI_FN(por) { (void)sa; if (rd) RQ(rd).q = RQ(rs).q | RQ(rt).q; }
MMI_FN(pxor) { (void)sa; if (rd) RQ(rd).q = RQ(rs).q ^ RQ(rt).q; }
MMI_FN(pnor) { (void)sa; if (rd) RQ(rd).q = ~(RQ(rs).q | RQ(rt).q); }
MMI_FN(pmaxw) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return;
    for (int i = 0; i < 4; i++) RQ(rd).w[i] = (s32)s.w[i] > (s32)t.w[i] ? s.w[i] : t.w[i]; }
MMI_FN(pminw) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return;
    for (int i = 0; i < 4; i++) RQ(rd).w[i] = (s32)s.w[i] < (s32)t.w[i] ? s.w[i] : t.w[i]; }
MMI_FN(psubw) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return;
    for (int i = 0; i < 4; i++) RQ(rd).w[i] = s.w[i] - t.w[i]; }
MMI_FN(psubb) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return;
    for (int i = 0; i < 16; i++) RQ(rd).b[i] = s.b[i] - t.b[i]; }
MMI_FN(prot3w) { (void)sa; (void)rs; GPR t = RQ(rt); if (!rd) return;
    RQ(rd).w[0] = t.w[1]; RQ(rd).w[1] = t.w[2]; RQ(rd).w[2] = t.w[0]; RQ(rd).w[3] = t.w[3]; }
MMI_FN(pexew) { (void)sa; (void)rs; GPR t = RQ(rt); if (!rd) return;
    RQ(rd).w[0] = t.w[2]; RQ(rd).w[1] = t.w[1]; RQ(rd).w[2] = t.w[0]; RQ(rd).w[3] = t.w[3]; }
MMI_FN(pexcw) { (void)sa; (void)rs; GPR t = RQ(rt); if (!rd) return;
    RQ(rd).w[0] = t.w[0]; RQ(rd).w[1] = t.w[2]; RQ(rd).w[2] = t.w[1]; RQ(rd).w[3] = t.w[3]; }
MMI_FN(pmfhi) { (void)sa; (void)rs; (void)rt; if (rd) { RQ(rd).d[0] = c.hi; RQ(rd).d[1] = c.hi1; } }
MMI_FN(pmflo) { (void)sa; (void)rs; (void)rt; if (rd) { RQ(rd).d[0] = c.lo; RQ(rd).d[1] = c.lo1; } }
MMI_FN(pmulth) { (void)sa; GPR s = RQ(rs), t = RQ(rt); s32 p[8];
    for (int i = 0; i < 8; i++) p[i] = (s32)(s16)s.h[i] * (s32)(s16)t.h[i];
    c.lo = (u32)p[0] | (u64)(u32)p[1] << 32; c.hi = (u32)p[2] | (u64)(u32)p[3] << 32;
    c.lo1 = (u32)p[4] | (u64)(u32)p[5] << 32; c.hi1 = (u32)p[6] | (u64)(u32)p[7] << 32;
    if (rd) { RQ(rd).w[0] = p[0]; RQ(rd).w[1] = p[2]; RQ(rd).w[2] = p[4]; RQ(rd).w[3] = p[6]; } }
MMI_FN(qfsrv) { (void)sa; GPR s = RQ(rs), t = RQ(rt); if (!rd) return;
    u32 sh = (u32)c.sa & 127;
    RQ(rd).q = sh ? (t.q >> sh) | (s.q << (128 - sh)) : t.q; }
#undef RQ
