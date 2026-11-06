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
    if (__builtin_expect(w[0] | w[1], 0))
        for (int i = 0; i < 4; i++) v[i] = fclamp(v[i]);
    return v;
}

#define SPLAT(v, k) __builtin_shufflevector(v, v, k, k, k, k)

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
    v4f acc = clamp4(m.r[0] * SPLAT(v, 0));
    acc = clamp4(acc + m.r[1] * SPLAT(v, 1));
    return clamp4(acc + m.r[2] * SPLAT(v, 2));
}

static inline v4f mul4(const Mat &m, v4f v) { return clamp4(axes(m, v) + m.r[3] * SPLAT(v, 3)); }

// v.w taken as 1: VF0.w
__attribute__((always_inline)) static inline v4f mul_point(const Mat &m, v4f v) { return clamp4(axes(m, v) + m.r[3]); }

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
    float w = v[3], q = w == 0.0f ? 3.4028235e38f : fclamp(1.0f / w);
    v4f out = clamp4(v * q);
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
    v4f k = load(0x320a60), v = clamp4(k + load(c.r[6].w[0]));
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
static inline float fDeterminant(float *m)
{
    return (M(0, 3) * M(3, 0)) * (M(2, 1) * M(1, 2)) - (M(0, 2) * M(3, 0)) * (M(2, 1) * M(1, 3))
         - (M(0, 3) * M(3, 0)) * (M(2, 2) * M(1, 1)) + (M(0, 1) * M(3, 0)) * (M(2, 2) * M(1, 3))
         + (M(0, 2) * M(3, 0)) * (M(2, 3) * M(1, 1)) - (M(0, 1) * M(3, 0)) * (M(2, 3) * M(1, 2))
         - (M(0, 3) * M(3, 1)) * (M(2, 0) * M(1, 2)) + (M(0, 2) * M(3, 1)) * (M(2, 0) * M(1, 3))
         + (M(0, 3) * M(3, 1)) * (M(2, 2) * M(1, 0)) - (M(0, 0) * M(3, 1)) * (M(2, 2) * M(1, 3))
         - (M(0, 2) * M(3, 1)) * (M(2, 3) * M(1, 0)) + (M(0, 0) * M(3, 1)) * (M(2, 3) * M(1, 2))
         + (M(0, 3) * M(3, 2)) * (M(2, 0) * M(1, 1)) - (M(0, 1) * M(3, 2)) * (M(2, 0) * M(1, 3))
         - (M(0, 3) * M(3, 2)) * (M(2, 1) * M(1, 0)) + (M(0, 0) * M(3, 2)) * (M(2, 1) * M(1, 3))
         + (M(0, 1) * M(3, 2)) * (M(2, 3) * M(1, 0)) - (M(0, 0) * M(3, 2)) * (M(2, 3) * M(1, 1))
         - (M(0, 2) * M(3, 3)) * (M(2, 0) * M(1, 1)) + (M(0, 1) * M(3, 3)) * (M(2, 0) * M(1, 2))
         + (M(0, 2) * M(3, 3)) * (M(2, 1) * M(1, 0)) - (M(0, 0) * M(3, 3)) * (M(2, 1) * M(1, 2))
         - (M(0, 1) * M(3, 3)) * (M(2, 2) * M(1, 0)) + (M(0, 0) * M(3, 3)) * (M(2, 2) * M(1, 1));
}

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

// A box against the view: 0 all outside one plane, 1 across, 2 all inside
void HOOK_ClassifyAABB__FPC8bVector4T0PC8bMatrix4(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    v4f lo = load(c.r[4].w[0]), hi = load(c.r[5].w[0]);
    Mat m = load_mat(c.r[6].w[0]);
    v4f size = clamp4(hi - lo);
    v4f p0 = mul_point(m, lo);
    v4f x = clamp4(m.r[0] * SPLAT(size, 0)), y = clamp4(m.r[1] * SPLAT(size, 1)), z = clamp4(m.r[2] * SPLAT(size, 2));
    v4f p1 = clamp4(p0 + x), p2 = clamp4(p1 + y), p3 = clamp4(p0 + y);
    v4f near_corners[4] = {p0, p1, p2, p3};
    v4f far_corners[4] = {clamp4(p0 + z), clamp4(p1 + z), clamp4(p2 + z), clamp4(p3 + z)};
    u32 any = 0, all = 63;
    for (int i = 0; i < 4; i++) {
        u32 a = clip_flags(near_corners[i]), b = clip_flags(far_corners[i]);
        any |= a | b;
        all &= a & b;
    }
    u32 result = !any ? 2 : all == 0 ? 1 : 0;
    if (g_check) {
        orig(c);
        counted(4, "ClassifyAABB");
        if (c.r[2].w[0] != result) differs(4, "ClassifyAABB");
        return;
    }
    c.r[2].d[0] = result;
}
