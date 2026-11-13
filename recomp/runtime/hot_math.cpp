// The hottest of the game's VU0 maths as plain C++ with clang's vectors: SSE on x86-64, NEON on arm64, no intrinsics.
// The results are bit for bit those of the generated code's VU0 macro ops, with their clamping (fclamp in ps2.h): HP2_HOTCHECK=1 runs both and
// counts what differs. The VU0 registers these functions use as scratch are not kept: in the game's ABI they do not survive a call.
#include "ps2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef float v4f __attribute__((vector_size(16)));
typedef int v4i __attribute__((vector_size(16)));

__attribute__((always_inline)) static inline v4f load(u32 a)
{
    u128 q = LQ(a);
    v4f v;
    memcpy(&v, &q, 16);
    return v;
}

static inline void store(u32 a, v4f v)
{
    u128 q;
    memcpy(&q, &v, 16);
    SQ(a, q);
}

// NaN to 0, an infinity to the largest float; nearly never needed
__attribute__((always_inline)) static inline v4f clamp4(v4f v)
{
    v4i special = ((v4i)v & 0x7fffffff) > 0x7f7fffff;
    u64 w[2];
    memcpy(w, &special, 16);
    if (__builtin_expect((w[0] | w[1]) != 0, 0))
        for (int i = 0; i < 4; i++) v[i] = fclamp(v[i]);
    return v;
}

#define SPLAT(v, k) __builtin_shufflevector(v, v, k, k, k, k)

// the EE's add, sub and mul (eefloat.h), four lanes at once
__attribute__((always_inline)) static inline v4f vadd(v4f a, v4f b) { return ee_add4(a, b); }
__attribute__((always_inline)) static inline v4f vsub(v4f a, v4f b) { return ee_sub4(a, b); }
__attribute__((always_inline)) static inline v4f vmul(v4f a, v4f b) { return ee_mul4(a, b); }

struct Mat {
    v4f r[4];
};

__attribute__((always_inline)) static inline Mat load_mat(u32 a)
{
    return {{load(a), load(a + 16), load(a + 32), load(a + 48)}};
}

// the three axes as the game accumulates them
__attribute__((always_inline)) static inline v4f axes(const Mat &m, v4f v)
{
    v4f acc = vmul(m.r[0], SPLAT(v, 0));
    acc = vadd(acc, vmul(m.r[1], SPLAT(v, 1)));
    return vadd(acc, vmul(m.r[2], SPLAT(v, 2)));
}

static inline v4f mul4(const Mat &m, v4f v) { return vadd(axes(m, v), vmul(m.r[3], SPLAT(v, 3))); }

// v.w taken as 1: VF0.w
__attribute__((always_inline)) static inline v4f mul_point(const Mat &m, v4f v) { return vadd(axes(m, v), m.r[3]); }

// VCLIP with w: +x -x +y -y +z -z from the low bit
__attribute__((always_inline)) static inline u32 clip_flags(v4f v)
{
    float lim = __builtin_fabsf(v[3]);
    u32 f = 0;
    for (int i = 0; i < 3; i++) {
        if (v[i] > lim) f |= 1u << (i * 2);
        if (v[i] < -lim) f |= 2u << (i * 2);
    }
    return f;
}

static const bool g_check = getenv("HP2_HOTCHECK") != nullptr;
static u64 g_calls[9], g_differs[9];

static void differs(int which, const char *name)
{
    static const char *names[9];
    names[which] = name;
    if (g_differs[which]++ < 20) fprintf(stderr, "hotcheck: %s differs (%llu calls)\n", name, (unsigned long long)g_calls[which]);
}

static void counted(int which, const char *name)
{
    if (++g_calls[which] % 1000000 == 0)
        fprintf(stderr, "hotcheck: %s %llu calls, %llu differ\n", name, (unsigned long long)g_calls[which], (unsigned long long)g_differs[which]);
}

// the new result against the generated code's
static void check_vec(Ctx &c, void (*orig)(Ctx &), int which, const char *name, u32 dest, const v4f *mine, int count)
{
    orig(c);
    counted(which, name);
    if (memcmp(host(dest & ~15u), mine, 16 * (size_t)count)) differs(which, name);
}

void HOOK_eMulMatrix__FP8bMatrix4T0T0(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    u32 dest = c.r[4].w[0];
    Mat a = load_mat(c.r[5].w[0]), b = load_mat(c.r[6].w[0]);
    v4f out[4] = {mul4(b, a.r[0]), mul4(b, a.r[1]), mul4(b, a.r[2]), mul4(b, a.r[3])};
    if (g_check) {
        check_vec(c, orig, 0, "eMulMatrix", dest, out, 4);
        return;
    }
    for (int i = 0; i < 4; i++) store(dest + i * 16, out[i]);
}

void HOOK_eMulVector__FP8bVector4PC8bMatrix4PC8bVector4(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    u32 dest = c.r[4].w[0];
    v4f out = mul4(load_mat(c.r[5].w[0]), load(c.r[6].w[0]));
    if (g_check) {
        check_vec(c, orig, 1, "eMulVector", dest, &out, 1);
        return;
    }
    store(dest, out);
}

void HOOK_eRotTransPers__FP8bVector4P8bMatrix4T0(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    u32 dest = c.r[4].w[0];
    v4f v = mul_point(load_mat(c.r[5].w[0]), load(c.r[6].w[0]));
    float w = v[3], q = ee_div(1.0f, w);
    v4f out = vmul(v, v4f{q, q, q, q});
    out[3] = w;
    if (g_check) {
        check_vec(c, orig, 2, "eRotTransPers", dest, &out, 1);
        return;
    }
    store(dest, out);
}

void HOOK_eGetClipFlags__FP8bVector4P8bMatrix4(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    u32 flags = clip_flags(mul_point(load_mat(c.r[5].w[0]), load(c.r[4].w[0])));
    if (g_check) {
        orig(c);
        counted(3, "eGetClipFlags");
        if (c.r[2].w[0] != flags) differs(3, "eGetClipFlags");
        return;
    }
    c.r[2].d[0] = flags;
}

// the same multiplication in the engine's other library
void HOOK_bMulMatrix__FP8bMatrix4PC8bMatrix4T1(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    u32 dest = c.r[4].w[0];
    Mat m = load_mat(c.r[5].w[0]), a = load_mat(c.r[6].w[0]);
    v4f out[4] = {mul4(m, a.r[0]), mul4(m, a.r[1]), mul4(m, a.r[2]), mul4(m, a.r[3])};
    if (g_check) {
        check_vec(c, orig, 5, "bMulMatrix", dest, out, 4);
        return;
    }
    for (int i = 0; i < 4; i++) store(dest + i * 16, out[i]);
}

void HOOK_bMulMatrix__FP8bVector3PC8bMatrix4PC8bVector3(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    u32 dest = c.r[4].w[0];
    // a constant at 0x320a60 gives the vector its w, and its xyz is added
    v4f k = load(0x320a60), v = vadd(k, load(c.r[6].w[0]));
    v[3] = k[3];
    v4f out = mul4(load_mat(c.r[5].w[0]), v);
    if (g_check) {
        check_vec(c, orig, 6, "bMulMatrix3", dest, &out, 1);
        return;
    }
    store(dest, out);
}

// src/AcidFXE.cpp. Its fInvertMatrix written the same way differed from the generated code in most calls (1.49 of 1.6 million) and is not here.
#define M(r, c) (((float (*)[4])m)[r][c])
// the products as the generated code has them, (a * b) * (c * d), and the sum from the left
#define P(r0, c0, r1, c1, r2, c2, r3, c3) ee_mul(ee_mul(M(r0, c0), M(r1, c1)), ee_mul(M(r2, c2), M(r3, c3)))
static inline float fDeterminant(float *m)
{
    float d = P(0, 3, 3, 0, 2, 1, 1, 2);
    d = ee_sub(d, P(0, 2, 3, 0, 2, 1, 1, 3));
    d = ee_sub(d, P(0, 3, 3, 0, 2, 2, 1, 1));
    d = ee_add(d, P(0, 1, 3, 0, 2, 2, 1, 3));
    d = ee_add(d, P(0, 2, 3, 0, 2, 3, 1, 1));
    d = ee_sub(d, P(0, 1, 3, 0, 2, 3, 1, 2));
    d = ee_sub(d, P(0, 3, 3, 1, 2, 0, 1, 2));
    d = ee_add(d, P(0, 2, 3, 1, 2, 0, 1, 3));
    d = ee_add(d, P(0, 3, 3, 1, 2, 2, 1, 0));
    d = ee_sub(d, P(0, 0, 3, 1, 2, 2, 1, 3));
    d = ee_sub(d, P(0, 2, 3, 1, 2, 3, 1, 0));
    d = ee_add(d, P(0, 0, 3, 1, 2, 3, 1, 2));
    d = ee_add(d, P(0, 3, 3, 2, 2, 0, 1, 1));
    d = ee_sub(d, P(0, 1, 3, 2, 2, 0, 1, 3));
    d = ee_sub(d, P(0, 3, 3, 2, 2, 1, 1, 0));
    d = ee_add(d, P(0, 0, 3, 2, 2, 1, 1, 3));
    d = ee_add(d, P(0, 1, 3, 2, 2, 3, 1, 0));
    d = ee_sub(d, P(0, 0, 3, 2, 2, 3, 1, 1));
    d = ee_sub(d, P(0, 2, 3, 3, 2, 0, 1, 1));
    d = ee_add(d, P(0, 1, 3, 3, 2, 0, 1, 2));
    d = ee_add(d, P(0, 2, 3, 3, 2, 1, 1, 0));
    d = ee_sub(d, P(0, 0, 3, 3, 2, 1, 1, 2));
    d = ee_sub(d, P(0, 1, 3, 3, 2, 2, 1, 0));
    d = ee_add(d, P(0, 0, 3, 3, 2, 2, 1, 1));
    return d;
}
#undef P

void HOOK_fDeterminant__FP8bMatrix4(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    float m[16];
    memcpy(m, host(c.r[4].w[0]), 64);
    const float det = fDeterminant(m);
    if (g_check) {
        orig(c);
        counted(7, "fDeterminant");
        u32 mine;
        memcpy(&mine, &det, 4);
        if (__builtin_isfinite(det) && c.f[0].u != mine) differs(7, "fDeterminant");
        return;
    }
    if (!__builtin_isfinite(det)) {
        orig(c);
        return;
    }
    c.f[0].f = det;
}

// ---- ClassifyAABB: a box against the view, 0 all outside one plane, 1 across, 2 all inside ----
// All that comes out of it is that answer: 48 comparisons of a corner's x, y or z with its |w|. So the corners are first made with the
// host's own arithmetic, with a bound on how far each coordinate can be from the EE's; where every comparison is further from equality
// than the bounds of its two sides, the EE's comparisons come out the same and so does the answer. Any comparison nearer than that, or
// anything not a plain number: the EE's arithmetic, as before (classify_exact).
//
// The bound. Every value in a corner is a sum of products of the inputs: the three axes times the box's low corner, the matrix's fourth
// row, and the three axes times the box's size (high - low). Against the true sum of true products an operation of the EE's is off by
// under 2 ulp of its larger operand or result (the multiplier cuts and may borrow from bit 15; the adder drops the smaller operand's bits
// below half the larger's ulp and cuts), the host's by under 1; an error made early goes through the later sums unchanged and through a
// product times the matrix's element, which `mag` has in it. A corner is at most 14 operations deep, each on values no larger than `mag`,
// the sum of the magnitudes of every term: the two results are within (14 * 2 + 14) * 2^-23 * mag of each other, under 2^-17.6 mag.
// kAabbSlack is 2^-16, three times that. HP2_HOTCHECK compares the answer with the generated code's on every call; HP2_AABBTEST runs
// millions of boxes whose corners are put within a few ulp of a plane.
static const float kAabbSlack = 0x1p-16f;
static u64 g_aabb_fast, g_aabb_exact;

__attribute__((always_inline)) static inline v4f habs(v4f v) { return (v4f)((v4i)v & 0x7fffffff); }

static u32 classify_exact(v4f lo, v4f hi, const Mat &m);

// the answer, or -1 where the host's arithmetic cannot vouch for it
__attribute__((always_inline)) static inline int classify_fast(v4f lo, v4f hi, const Mat &m)
{
    const v4f size = hi - lo, reach = habs(hi) + habs(lo);
    const v4f ax = m.r[0] * SPLAT(lo, 0), ay = m.r[1] * SPLAT(lo, 1), az = m.r[2] * SPLAT(lo, 2);
    const v4f x = m.r[0] * SPLAT(size, 0), y = m.r[1] * SPLAT(size, 1), z = m.r[2] * SPLAT(size, 2);
    const v4f mag = habs(ax) + habs(ay) + habs(az) + habs(m.r[3]) + habs(m.r[0]) * SPLAT(reach, 0) + habs(m.r[1]) * SPLAT(reach, 1) +
                    habs(m.r[2]) * SPLAT(reach, 2);
    const v4f err = mag * kAabbSlack + 1e-30f;
    const v4f tol = err + SPLAT(err, 3);
    const v4f p0 = ax + ay + az + m.r[3], p1 = p0 + x, p2 = p1 + y, p3 = p0 + y;
    const v4f corners[8] = {p0, p1, p2, p3, p0 + z, p1 + z, p2 + z, p3 + z};
    v4i sure = mag < 1e30f, any_hi = {}, any_lo = {}, all_hi = {-1, -1, -1, -1}, all_lo = {-1, -1, -1, -1};
    for (int i = 0; i < 8; i++) {
        const v4f v = corners[i], w = SPLAT(habs(v), 3);
        sure &= habs(habs(v) - w) > tol;
        const v4i over = v > w, under = v < -w;
        any_hi |= over, any_lo |= under, all_hi &= over, all_lo &= under;
    }
    const v4i any = any_hi | any_lo, all = all_hi | all_lo;
    if (__builtin_expect(!(sure[0] & sure[1] & sure[2]), 0)) return -1;
    return !(any[0] | any[1] | any[2]) ? 2 : (all[0] | all[1] | all[2]) ? 0 : 1;
}

void HOOK_ClassifyAABB__FPC8bVector4T0PC8bMatrix4(Ctx &c, void (*orig)(Ctx &))
{
    static const bool no_fast = getenv("HP2_NOAABBFAST") != nullptr;
    if (g_irq_pending) ps2_take_interrupts(c);
    v4f lo = load(c.r[4].w[0]), hi = load(c.r[5].w[0]);
    Mat m = load_mat(c.r[6].w[0]);
    const int fast = no_fast ? -1 : classify_fast(lo, hi, m);
    u32 result = fast >= 0 ? (u32)fast : classify_exact(lo, hi, m);
    if (g_check) {
        orig(c);
        counted(4, "ClassifyAABB");
        fast >= 0 ? g_aabb_fast++ : g_aabb_exact++;
        if (c.r[2].w[0] != result) differs(4, fast >= 0 ? "ClassifyAABB (the host's arithmetic)" : "ClassifyAABB");
        if (g_calls[4] % 1000000 == 0)
            fprintf(stderr, "hotcheck: ClassifyAABB by the host's arithmetic %llu, by the EE's %llu\n", (unsigned long long)g_aabb_fast, (unsigned long long)g_aabb_exact);
        return;
    }
    c.r[2].d[0] = result;
}

static u32 classify_exact(v4f lo, v4f hi, const Mat &m)
{
    v4f size = vsub(hi, lo);
    v4f p0 = mul_point(m, lo);
    v4f x = vmul(m.r[0], SPLAT(size, 0)), y = vmul(m.r[1], SPLAT(size, 1)), z = vmul(m.r[2], SPLAT(size, 2));
    v4f p1 = vadd(p0, x), p2 = vadd(p1, y), p3 = vadd(p0, y);
    v4f near_corners[4] = {p0, p1, p2, p3};
    v4f far_corners[4] = {vadd(p0, z), vadd(p1, z), vadd(p2, z), vadd(p3, z)};
    u32 any = 0, all = 63;
    for (int i = 0; i < 4; i++) {
        u32 a = clip_flags(near_corners[i]), b = clip_flags(far_corners[i]);
        any |= a | b;
        all &= a & b;
    }
    return !any ? 2 : all == 0 ? 1 : 0;
}

// HP2_AABBTEST=<millions>: boxes and matrices at random, and then the matrix's fourth row moved so that a corner's x, y or z lands within
// a few ulp of its |w|, either side, or a few thousand: on both sides of where the bound lets the host's arithmetic answer. Each is
// classified both ways: what the host's arithmetic vouched for must be the EE's answer.
void hot_math_selftest()
{
    const char *env = getenv("HP2_AABBTEST");
    if (!env) return;
    const u64 total = (u64)atoi(env) * 1000000;
    u64 seed = 0x9E3779B97F4A7C15ull, sure = 0, wrong = 0, near_sure = 0, near_n = 0, by[3] = {};
    const auto rnd = [&]() {
        seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
        return seed;
    };
    const auto real = [&](float scale) { return ((float)(s32)(u32)rnd() * 0x1p-31f) * scale; };
    for (u64 n = 0; n < total; n++) {
        // a view's matrix in the game's shape: a rotation's size of numbers, a w row that grows with the depth, sizes of a track
        static const float kScale[4] = {1.0f, 40.0f, 600.0f, 5000.0f};
        const float world = kScale[rnd() & 3];
        Mat m;
        for (int r = 0; r < 3; r++) m.r[r] = v4f{real(2), real(2), real(1), real(1)};
        m.r[3] = v4f{real(world), real(world), real(world), real(world)};
        v4f lo = {real(world), real(world), real(world * 0.1f), 1.0f};
        v4f hi = lo + v4f{(real(1) + 1.0f) * world * 0.05f, (real(1) + 1.0f) * world * 0.05f, (real(1) + 1.0f) * world * 0.02f, 0.0f};
        hi[3] = 1.0f;
        const bool near_case = n & 1;
        if (near_case) {
            // the EE's corner as it is, then the fourth row's x, y or z moved by what that corner's coordinate lacks of +-|w|, and a few ulp more
            const int corner = (int)(rnd() & 7), axis = (int)(rnd() % 3);
            v4f size = vsub(hi, lo), p = mul_point(m, lo);
            if (corner & 1) p = vadd(p, vmul(m.r[0], SPLAT(size, 0)));
            if (corner & 2) p = vadd(p, vmul(m.r[1], SPLAT(size, 1)));
            if (corner & 4) p = vadd(p, vmul(m.r[2], SPLAT(size, 2)));
            const float want = (rnd() & 1 ? 1.0f : -1.0f) * __builtin_fabsf(p[3]);
            float moved = m.r[3][axis] + (want - p[axis]);
            u32 bits;
            memcpy(&bits, &moved, 4);
            bits += (u32)(((int)(rnd() % 9) - 4) << (rnd() % 14));      // up to 4 ulp, then 8, 16 ... 32768: across where the bound ends
            memcpy(&moved, &bits, 4);
            m.r[3][axis] = moved;
        }
        const int fast = classify_fast(lo, hi, m);
        const u32 exact = classify_exact(lo, hi, m);
        by[exact]++;
        if (near_case) near_n++;
        if (fast < 0) continue;
        sure++;
        if (near_case) near_sure++;
        if ((u32)fast != exact && wrong++ < 10) fprintf(stderr, "aabbtest: box %llu: the host's arithmetic says %d, the EE's %u\n", (unsigned long long)n, fast, exact);
    }
    fprintf(stderr, "aabbtest: %llu boxes (outside %llu, across %llu, inside %llu): the host's arithmetic vouched for %llu, wrong %llu; of the %llu put by a plane it vouched for %llu\n",
            (unsigned long long)total, (unsigned long long)by[0], (unsigned long long)by[1], (unsigned long long)by[2], (unsigned long long)sure,
            (unsigned long long)wrong, (unsigned long long)near_n, (unsigned long long)near_sure);
}
