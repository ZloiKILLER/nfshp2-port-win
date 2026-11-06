// PS2 EE runtime for the statically recompiled NFS Hot Pursuit 2 (SLUS_203.62).
//
// Ctx holds the full EE state the generated code touches: 128-bit GPRs,
// HI/LO pairs, the FPU, and VU0 in macro mode. Memory is the 32 MB main RAM
// plus the 16 KB scratchpad; anything else goes to the I/O handlers.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

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
// Arrays, not pointers: every load and store of the game goes through these, and a pointer would be read again after each store.
extern u8 g_ram[0x02000000 + 64];   // 32 MB
extern u8 g_spr[0x4000 + 64];       // 16 KB scratchpad at 0x70000000
extern u8 g_vumem[0x10000 + 64];    // VU0/VU1 micro and data memory, 0x11000000-0x1100FFFF

u64 io_read(u32 addr, int size);
void io_write(u32 addr, u64 value, int size);
u128 io_read128(u32 addr);
void io_write128(u32 addr, u128 value);

static inline u8 *host(u32 a)
{
    if ((a >> 28) == 7)
        return (a & 0x0FFFFFFF) < 0x4000 ? g_spr + (a & 0x3FFF) : nullptr;
    u32 top = a >> 28;
    u32 p = (top == 2 || top == 3) ? (a & 0x01FFFFFF) : (a & 0x1FFFFFFF);
    if (p < 0x02000000) return g_ram + p;
    if (p - 0x11000000u < 0x10000u) return g_vumem + (p - 0x11000000u);
    return nullptr;
}

#define MEMR(T, n)                                                  \
    static inline T L##n(u32 a)                                     \
    {                                                               \
        u8 *p = host(a);                                            \
        if (p) { T v; memcpy(&v, p, sizeof v); return v; }          \
        return (T)io_read(a, sizeof(T));                            \
    }
#define MEMW(T, n)                                                  \
    static inline void S##n(u32 a, T v)                             \
    {                                                               \
        u8 *p = host(a);                                            \
        if (p) { memcpy(p, &v, sizeof v); return; }                 \
        io_write(a, (u64)v, sizeof(T));                             \
    }
MEMR(u8, B) MEMR(u16, H) MEMR(u32, W) MEMR(u64, D)
MEMW(u8, B) MEMW(u16, H) MEMW(u32, W) MEMW(u64, D)

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

// ---- FPU (COP1) ---------------------------------------------------------------
// The EE FPU has no NaN/Inf; results are clamped to the largest finite value.
static inline float fclamp(float v)
{
    if (std::isnan(v)) return 0.0f;
    if (std::isinf(v)) return v > 0 ? 3.4028235e38f : -3.4028235e38f;
    return v;
}
#define F(n) c.f[n].f
static inline void FPU_add_s(Ctx &c, int d, int s, int t) { F(d) = fclamp(F(s) + F(t)); }
static inline void FPU_sub_s(Ctx &c, int d, int s, int t) { F(d) = fclamp(F(s) - F(t)); }
static inline void FPU_mul_s(Ctx &c, int d, int s, int t) { F(d) = fclamp(F(s) * F(t)); }
static inline void FPU_div_s(Ctx &c, int d, int s, int t)
{
    F(d) = F(t) == 0.0f ? (std::signbit(F(s)) != std::signbit(F(t)) ? -3.4028235e38f : 3.4028235e38f)
                        : fclamp(F(s) / F(t));
}
static inline void FPU_sqrt_s(Ctx &c, int d, int, int t) { F(d) = std::sqrt(std::fabs(F(t))); }
static inline void FPU_rsqrt_s(Ctx &c, int d, int s, int t) { F(d) = fclamp(F(s) / std::sqrt(std::fabs(F(t)))); }
static inline void FPU_abs_s(Ctx &c, int d, int s, int) { F(d) = std::fabs(F(s)); }
static inline void FPU_mov_s(Ctx &c, int d, int s, int) { c.f[d].u = c.f[s].u; }
static inline void FPU_neg_s(Ctx &c, int d, int s, int) { c.f[d].u = c.f[s].u ^ 0x80000000u; }
static inline void FPU_adda_s(Ctx &c, int, int s, int t) { c.acc = fclamp(F(s) + F(t)); }
static inline void FPU_suba_s(Ctx &c, int, int s, int t) { c.acc = fclamp(F(s) - F(t)); }
static inline void FPU_mula_s(Ctx &c, int, int s, int t) { c.acc = fclamp(F(s) * F(t)); }
static inline void FPU_madd_s(Ctx &c, int d, int s, int t) { F(d) = fclamp(c.acc + F(s) * F(t)); }
static inline void FPU_msub_s(Ctx &c, int d, int s, int t) { F(d) = fclamp(c.acc - F(s) * F(t)); }
static inline void FPU_madda_s(Ctx &c, int, int s, int t) { c.acc = fclamp(c.acc + F(s) * F(t)); }
static inline void FPU_msuba_s(Ctx &c, int, int s, int t) { c.acc = fclamp(c.acc - F(s) * F(t)); }
static inline void FPU_max_s(Ctx &c, int d, int s, int t) { F(d) = F(s) > F(t) ? F(s) : F(t); }
static inline void FPU_min_s(Ctx &c, int d, int s, int t) { F(d) = F(s) < F(t) ? F(s) : F(t); }
static inline void FPU_cvt_w_s(Ctx &c, int d, int s, int)
{
    float v = F(s);
    c.f[d].u = v >= 2147483647.0f ? 0x7FFFFFFFu : v <= -2147483648.0f ? 0x80000000u : (u32)(s32)v;
}
static inline void FPU_cvt_s_w(Ctx &c, int d, int s, int) { F(d) = (float)(s32)c.f[s].u; }
static inline void fcond(Ctx &c, bool b) { c.fcr31 = b ? (c.fcr31 | FCR31_C) : (c.fcr31 & ~FCR31_C); }
static inline void FPU_c_f_s(Ctx &c, int, int, int) { fcond(c, false); }
static inline void FPU_c_eq_s(Ctx &c, int, int s, int t) { fcond(c, F(s) == F(t)); }
static inline void FPU_c_lt_s(Ctx &c, int, int s, int t) { fcond(c, F(s) < F(t)); }
static inline void FPU_c_le_s(Ctx &c, int, int s, int t) { fcond(c, F(s) <= F(t)); }
#undef F
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
VU_BIN(vadd, x + y)
VU_BIN(vsub, x - y)
VU_BIN(vmul, x * y)
VU_BIN(vmax, x > y ? x : y)
VU_BIN(vmini, x < y ? x : y)
#define ACC (c.vacc.f[i])
VU_BIN(vmadd, ACC + x * y)
VU_BIN(vmsub, ACC - x * y)
#undef ACC
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
VU_ACC(vadda, x + y)
VU_ACC(vsuba, x - y)
VU_ACC(vmula, x * y)
VU_ACC(vmadda, acc + x * y)
VU_ACC(vmsuba, acc - x * y)
BC_ALIAS(vadda, vadda) BC_ALIAS(vsuba, vsuba) BC_ALIAS(vmula, vmula)
BC_ALIAS(vmadda, vmadda) BC_ALIAS(vmsuba, vmsuba)

// outer product: OPMULA.xyz ACC = fs.yzx * ft.zxy; OPMSUB fd = ACC - fs.yzx * ft.zxy
// (OPMULA ACC,a,b then OPMSUB d,b,a is the cross product a x b)
static inline void VU_vopmula(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)], b = c.vf[VFT(w)];
    c.vacc.f[0] = fclamp(a.f[1] * b.f[2]);
    c.vacc.f[1] = fclamp(a.f[2] * b.f[0]);
    c.vacc.f[2] = fclamp(a.f[0] * b.f[1]);
}
static inline void VU_vopmsub(Ctx &c, u32 w)
{
    VF a = c.vf[VFS(w)], b = c.vf[VFT(w)];
    int d = VFD(w);
    float r0 = c.vacc.f[0] - a.f[1] * b.f[2];
    float r1 = c.vacc.f[1] - a.f[2] * b.f[0];
    float r2 = c.vacc.f[2] - a.f[0] * b.f[1];
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
    float t = vftf(c, w);
    c.vq = t == 0.0f ? 3.4028235e38f : fclamp(vfsf(c, w) / t);
}
static inline void VU_vsqrt(Ctx &c, u32 w) { c.vq = std::sqrt(std::fabs(vftf(c, w))); }
static inline void VU_vrsqrt(Ctx &c, u32 w)
{
    float t = std::sqrt(std::fabs(vftf(c, w)));
    c.vq = t == 0.0f ? 3.4028235e38f : fclamp(vfsf(c, w) / t);
}
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
