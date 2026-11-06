// The EE's float arithmetic to the bit, on the host's own float unit. The FPU and VU0 keep the IEEE format but not its arithmetic:
//  - add and sub: the smaller operand loses its low d - 1 bits first (d the exponents' difference), then the sum is exact and cut toward
//    zero. So: those bits masked off (at d >= 25 all of them but the sign: the larger operand comes back as it is, where IEEE would take a
//    bit off it when the signs differ), then the host's add in round-toward-zero (ee_float_mode) is the EE's result.
//  - mul: the 48-bit product cut toward zero, but the EE's Booth multiplier sums only part of its partial products below bit 15, and
//    bit 15 can come out different. That reaches the result only through a borrow, when the product's bits 15..22 are all zero, and never
//    when the second operand's low 16 bits are (its Booth digits there are all 0): only then is the multiplier modelled, and where the
//    first operand's low 16 bits are 0 (95 % of these) by a table of 8 KB; otherwise the host's mul in round-toward-zero is the EE's.
//  - madd and msub: the product rounded as mul, then added as add.
//  - div and sqrt: the EE's is the host's division (root) cut toward zero, or one ulp more where its SRT recurrence ended above the true
//    value. A root, and a division of a power of two (1 / y), depend on one mantissa: the one ulp is a bit in a table built at the start.
//    Other divisions: the one ulp only past a bound on the quotient's fraction past the ulp, set by the divisor (ee_div); below it the
//    host's quotient; past it the last 4096 divisions' results, else the recurrence (ee_div_srt). The fraction comes from the host's
//    double, exact to 2^-52 there.
// The models are PCSX2's (pcsx2/PS2Float.cpp, GitHubProUser67's soft-float branch, PR 12001; GPL-3.0+, SPDX PCSX2 Dev Team 2002-2024).
// Overflow and exponent 255 are the host's clamp to the largest finite value, where the EE would have 0x7FFFFFFF: rare in a game's maths.
// Built with EE_HOST_FLOAT defined: the host's arithmetic alone, for comparing. Checked against the models: work/eefloat/*.cpp.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>

// ps2.cpp: the multiplier's correction where the first mantissa's low 16 bits are 0, a bit for each pattern of the second's low 16 bits
// (ee_float_init fills it from the model); the last divisions and roots that took the recurrence, by their operands' bits
extern uint8_t g_ee_booth16[8192];
struct EeDivEntry {
    uint64_t key;
    uint32_t value;
};
struct EeSqrtEntry {
    uint32_t key, value;
};
extern EeDivEntry g_ee_div_cache[4096];
extern EeSqrtEntry g_ee_sqrt_cache[4096];
// what the recurrence comes to where it depends on one mantissa only: a division of a power of two (1 / y and the like), by the divisor's
// 23 bits, and a square root, by its 23 bits and the exponent's lowest. A bit each: one ulp above the host's result. Built at the start on
// a thread of their own (ee_tables_build, from main, 2 to 3 s), and used once g_ee_tables is set; till then the recurrence.
extern uint8_t g_ee_recip_up[1 << 20], g_ee_sqrt_up[1 << 21];
extern std::atomic<bool> g_ee_tables;

static inline uint32_t ee_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline float ee_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// The EE FPU has no NaN/Inf; results are clamped to the largest finite value.
static inline float fclamp(float v)
{
    const uint32_t u = ee_bits(v);
    if (__builtin_expect((u & 0x7f800000u) == 0x7f800000u, 0)) return (u & 0x7fffffu) ? 0.0f : ee_float((u & 0x80000000u) | 0x7f7fffffu);
    return v;
}

static inline float ee_add(float a, float b)
{
#ifndef EE_HOST_FLOAT
    uint32_t x = ee_bits(a), y = ee_bits(b);
    const int d = (int)(x >> 23 & 255) - (int)(y >> 23 & 255);
    const int sy = d > 24 ? 31 : d - 1, sx = d < -24 ? 31 : -d - 1;
    y &= d > 0 ? ~0u << sy : ~0u;
    x &= d < 0 ? ~0u << sx : ~0u;
    return fclamp(ee_float(x) + ee_float(y));
#else
    return fclamp(a + b);
#endif
}

static inline float ee_sub(float a, float b) { return ee_add(a, ee_float(ee_bits(b) ^ 0x80000000u)); }

// radix-4 Booth digit `bit` of b times a, and its negation's +1 (PCSX2's Booth)
struct EeBooth {
    uint32_t data, negate;
};
static inline EeBooth ee_booth(uint32_t a, uint32_t b, uint32_t bit)
{
    uint32_t test = (bit ? b >> (bit * 2 - 1) : b << 1) & 7;
    a <<= (bit * 2);
    a += (test == 3 || test == 4) ? a : 0;
    uint32_t neg = (test >= 4 && test <= 6) ? ~0u : 0;
    uint32_t pos = 1u << (bit * 2);
    a ^= (neg & (0u - pos));
    a &= (test >= 1 && test <= 6) ? ~0u : 0;
    return {a, neg & pos};
}
struct EeAdd3 {
    uint32_t lo, hi;
};
static inline EeAdd3 ee_add3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t u = a ^ b;
    return {u ^ c, ((u & c) | (a & b)) << 1};
}
// the 48-bit product of two 24-bit mantissas as the EE's multiplier has it: exact but for bit 15 (PCSX2's MulMantissa)
static inline uint64_t ee_mul_mantissa(uint32_t a, uint32_t b)
{
    uint64_t full = (uint64_t)a * (uint64_t)b;
    EeBooth b0 = ee_booth(a, b, 0), b1 = ee_booth(a, b, 1), b2 = ee_booth(a, b, 2), b3 = ee_booth(a, b, 3);
    EeBooth b4 = ee_booth(a, b, 4), b5 = ee_booth(a, b, 5), b6 = ee_booth(a, b, 6), b7 = ee_booth(a, b, 7);
    // first cycle
    EeAdd3 t0 = ee_add3(b1.data, b2.data, b3.data);
    EeAdd3 t1 = ee_add3(b4.data & ~0x7ffu, b5.data & ~0xfffu, b6.data);
    // a few adds get skipped, squeeze them back in
    t1.hi |= b6.negate | (b5.data & 0x800);
    b7.data |= (b5.data & 0x400) + b5.negate;
    // second, third and fourth cycles
    EeAdd3 t2 = ee_add3(b0.data, t0.lo, t0.hi);
    EeAdd3 t3 = ee_add3(b7.data, t1.lo, t1.hi);
    EeAdd3 t4 = ee_add3(t2.hi, t3.lo, t3.hi);
    EeAdd3 t5 = ee_add3(t2.lo, t4.lo, t4.hi);
    // discard bits and sum
    t5.hi += b7.negate;
    t5.lo &= ~0x7fffu;
    t5.hi &= ~0x7fffu;
    uint32_t ps2lo = t5.lo + t5.hi;
    return full - (((uint64_t)ps2lo ^ full) & 0x8000);
}

// whether the multiplier can give other than the host: both normal, and a borrow from bit 15 can reach the result
static inline bool ee_mul_differs(uint32_t x, uint32_t y)
{
    const uint32_t ma = (x & 0x7fffff) | 0x800000, mb = (y & 0x7fffff) | 0x800000;
    return ((ma * mb) & 0x7f8000) == 0 && (mb & 0xffff) && (x & 0x7f800000) && (y & 0x7f800000);
}

// the result where the multiplier is modelled. With the first mantissa's low 16 bits 0 (0.5, 1, 2, a small integer: 95 % of these in a
// race), the low 16 bits of every partial product come from the second's Booth digits alone, and so does the correction: a table.
// Checked on 1.4 * 10^8 products, work/eefloat/boothtable.cpp.
static inline float ee_mul_slow(uint32_t x, uint32_t y, float r)
{
    const uint32_t ma = (x & 0x7fffff) | 0x800000, mb = (y & 0x7fffff) | 0x800000;
    uint64_t p;
    if (!(ma & 0xffff)) {
        const uint32_t i = mb & 0xffff;
        p = (uint64_t)ma * mb - ((g_ee_booth16[i >> 3] >> (i & 7) & 1u) << 15);
    } else {
        p = ee_mul_mantissa(ma, mb);
    }
    uint32_t m = (uint32_t)(p >> 23);
    int e = (int)(x >> 23 & 255) + (int)(y >> 23 & 255) - 127;
    if (m > 0xffffff) m >>= 1, e++;
    if (e < 1 || e > 254) return r;                         // underflow and overflow: the host's
    return ee_float(((x ^ y) & 0x80000000u) | (uint32_t)e << 23 | (m & 0x7fffff));
}

static inline float ee_mul(float a, float b)
{
    const float r = fclamp(a * b);
#ifndef EE_HOST_FLOAT
    const uint32_t x = ee_bits(a), y = ee_bits(b);
    if (__builtin_expect(ee_mul_differs(x, y), 0)) return ee_mul_slow(x, y, r);
#endif
    return r;
}

// The divider and the square root: 25 steps of a radix-2 SRT recurrence on a carry-save remainder (PCSX2's, after DOI
// 10.1109/ARITH.1995.465363), the quotient then cut to 24 bits. Its last digit may leave the quotient above the true one.
struct EeCsa {
    uint32_t sum, carry;
};
static inline EeCsa ee_csa(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t u = a ^ b;
    return {u ^ c, ((a & b) | (u & c)) << 1};
}
static inline int ee_qsel(EeCsa r)
{
    // the binary point between bits 24 and 25; bit 23 is or'd in, not added
    const uint32_t mask = (1u << 24) - 1;
    const int32_t test = (int32_t)(((r.sum & ~mask) + r.carry) | (r.sum & mask));
    return test >= (1 << 23) ? 1 : test < (int32_t)(~0u << 24) ? -1 : 0;
}

static inline float ee_div_srt(float a, float b)
{
    const uint32_t x = ee_bits(a), y = ee_bits(b), sign = (x ^ y) & 0x80000000u;
    const uint32_t ex = x >> 23 & 255, ey = y >> 23 & 255;
    if (!ey) return ee_float(sign | 0x7f7fffffu);           // by zero: the largest value (the EE's is 0x7FFFFFFF, past the host's)
    if (!ex) return ee_float(sign);
    const uint32_t bm = ((y & 0x7fffff) | 0x800000) << 2;
    EeCsa cur = {((x & 0x7fffff) | 0x800000) << 2, 0};
    uint32_t q = 0;
    int bit = 1;
    for (int i = 0; i < 25; i++) {
        q = (q << 1) + (uint32_t)bit;
        const uint32_t add = bit > 0 ? ~bm : bit < 0 ? bm : 0;
        cur.carry += bit > 0;
        const EeCsa s = ee_csa(cur.sum, cur.carry, add);
        bit = ee_qsel(bit ? s : cur);
        cur.sum = s.sum << 1;
        cur.carry = s.carry << 1;
    }
    int e = (int)ex - (int)ey + 126;
    if (q >= (1u << 24)) e++, q >>= 1;
    if (e > 254) return ee_float(sign | 0x7f7fffffu);
    if (e < 1) return ee_float(sign);
    return ee_float(sign | (uint32_t)e << 23 | (q & 0x7fffff));
}

// sqrt(|a|): the EE takes the magnitude and only flags a negative
static inline float ee_sqrt_srt(float a)
{
    const uint32_t x = ee_bits(a), ex = x >> 23 & 255;
    if (!ex) return 0.0f;
    uint32_t m = ((x & 0x7fffff) | 0x800000) << 1;
    if (!(x & 0x800000)) m <<= 1;                           // an odd exponent once the bias is off
    EeCsa cur = {m, 0};
    uint32_t q = 0;
    int bit = 1;
    for (int i = 0; i < 25; i++) {
        // adding n to the root adds n * (2 * root + n) to its square, which comes off the remainder
        const uint32_t adjust = q + ((uint32_t)bit << (24 - i));
        q += (uint32_t)bit << (25 - i);
        const uint32_t add = bit > 0 ? ~adjust : bit < 0 ? adjust : 0;
        cur.carry += bit > 0;
        const EeCsa s = ee_csa(cur.sum, cur.carry, add);
        bit = ee_qsel(bit ? s : cur);
        cur.sum = s.sum << 1;
        cur.carry = s.carry << 1;
    }
    return ee_float(((q >> 2) & 0x7fffff) | ((ex + 127) >> 1) << 23);
}

// the true result's bits past the kept 24, out of 2^29, from the host's double
static inline uint32_t ee_past_ulp(double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    return (uint32_t)(b & 0x1fffffff);
}

static inline float ee_div(float a, float b)
{
#ifndef EE_HOST_FLOAT
    const uint32_t x = ee_bits(a), y = ee_bits(b);
    const uint32_t ex = x >> 23 & 255, ey = y >> 23 & 255;
    // The recurrence ends one ulp above the host's quotient only past a bound on the fraction f past the ulp, set by the divisor's
    // mantissa d in [1, 2): 1 - 1/(2d) when the dividend's mantissa is not the smaller, 1 - 1/d for d <= 1.5 and 1/(2d) above when it is
    // (measured, work/eefloat/srtstudy.cpp; at the bound itself it does go up). With a margin of 0.01, (f + 0.01) d against the bound
    // times d, so no division: about 43 % of the divisions are the host's.
    if (ex - 1 < 254 && ey - 1 < 254) {
        const double d = 1.0 + (double)(y & 0x7fffff) * 0x1p-23, fd = ((double)ee_past_ulp((double)a / (double)b) * 0x1p-29 + 0.01) * d;
        if ((x & 0x7fffff) >= (y & 0x7fffff) ? fd < d - 0.5 : d <= 1.5 ? fd < d - 1.0 : fd < 0.5) return fclamp(a / b);
    }
    // a power of two over y (a third of these in a race, mostly 1 / y): the recurrence's quotient depends on y's mantissa alone, and is
    // the host's or one ulp more; that is learnt once for each mantissa. Only where the host's quotient is normal, as the EE's then is.
    if (!(x & 0x7fffff) && ex - 1 < 254 && ey - 1 < 254 && g_ee_tables.load(std::memory_order_acquire)) {
        const uint32_t hb = ee_bits(a / b), i = y & 0x7fffff;
        if ((hb >> 23 & 255) - 1 < 254) return ee_float(hb + (g_ee_recip_up[i >> 3] >> (i & 7) & 1u));
    }
    // the recurrence, unless these operands had it lately (a race repeats a third of its divisions). Normal numbers only, so an empty
    // entry's key never matches
    if (ex - 1 < 254 && ey - 1 < 254) {
        const uint64_t key = (uint64_t)x << 32 | y;
        EeDivEntry &e = g_ee_div_cache[(uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 52)];
        if (e.key == key) return ee_float(e.value);
        const float q = ee_div_srt(a, b);
        e.key = key;
        e.value = ee_bits(q);
        return q;
    }
    return ee_div_srt(a, b);
#else
    return b == 0.0f ? ((ee_bits(a) ^ ee_bits(b)) >> 31 ? -3.4028235e38f : 3.4028235e38f) : fclamp(a / b);
#endif
}

static inline float ee_sqrt(float a)
{
#ifndef EE_HOST_FLOAT
    const uint32_t x = ee_bits(a);
    // the root's mantissa depends on the operand's 23 bits and its exponent's lowest alone: the host's or one ulp more (the table)
    if ((x >> 23 & 255) - 1 < 254 && g_ee_tables.load(std::memory_order_acquire)) {
        const uint32_t i = x & 0xffffff;
        return ee_float(ee_bits(std::sqrt(std::fabs(a))) + (g_ee_sqrt_up[i >> 3] >> (i & 7) & 1u));
    }
    // before the table: the host's root where the fraction past the ulp is small, else the recurrence
    const uint32_t limit = (x & 0x800000) ? (uint32_t)(0.45 * (1 << 29)) : (uint32_t)(0.60 * (1 << 29));
    if ((x & 0x7f800000) && ee_past_ulp(std::sqrt(std::fabs((double)a))) < limit) return std::sqrt(std::fabs(a));
    return ee_sqrt_srt(a);
#else
    return std::sqrt(std::fabs(a));
#endif
}

// a / sqrt(|b|), each rounded as the EE has it
static inline float ee_rsqrt(float a, float b) { return ee_div(a, ee_sqrt(b)); }

static inline float ee_madd(float acc, float a, float b) { return ee_add(acc, ee_mul(a, b)); }
static inline float ee_msub(float acc, float a, float b) { return ee_sub(acc, ee_mul(a, b)); }

// ---- four lanes at once (clang's vectors: SSE on x86-64, NEON on arm64), the same as the scalar ones lane by lane
typedef float ee_v4f __attribute__((vector_size(16)));
typedef uint32_t ee_v4u __attribute__((vector_size(16)));
typedef int32_t ee_v4i __attribute__((vector_size(16)));

static inline ee_v4f ee_clamp4(ee_v4f v)
{
    ee_v4u u = (ee_v4u)v;
    ee_v4i special = (ee_v4i)((u & 0x7f800000u) == 0x7f800000u);
    uint64_t w[2];
    memcpy(w, &special, 16);
    if (__builtin_expect((w[0] | w[1]) != 0, 0))
        for (int i = 0; i < 4; i++) v[i] = fclamp(v[i]);
    return v;
}

static inline ee_v4f ee_add4(ee_v4f a, ee_v4f b)
{
#ifndef EE_HOST_FLOAT
    ee_v4u x = (ee_v4u)a, y = (ee_v4u)b;
    const ee_v4i d = (ee_v4i)(x >> 23 & 255u) - (ee_v4i)(y >> 23 & 255u);
    // the shift for the smaller operand: |d| - 1, 31 from 25 on (only its sign left), 0 where it is not the smaller
    const ee_v4i far_y = d > 24, far_x = d < -24, y_small = d > 0, x_small = d < 0;
    const ee_v4u sy = (ee_v4u)((far_y & 31) | (~far_y & y_small & (d - 1)));
    const ee_v4u sx = (ee_v4u)((far_x & 31) | (~far_x & x_small & (-d - 1)));
    y &= ~0u << sy;
    x &= ~0u << sx;
    return ee_clamp4((ee_v4f)x + (ee_v4f)y);
#else
    return ee_clamp4(a + b);
#endif
}

static inline ee_v4f ee_sub4(ee_v4f a, ee_v4f b) { return ee_add4(a, (ee_v4f)((ee_v4u)b ^ 0x80000000u)); }

static inline ee_v4f ee_mul4(ee_v4f a, ee_v4f b)
{
    ee_v4f r = ee_clamp4(a * b);
#ifndef EE_HOST_FLOAT
    const ee_v4u x = (ee_v4u)a, y = (ee_v4u)b;
    const ee_v4u ma = (x & 0x7fffffu) | 0x800000u, mb = (y & 0x7fffffu) | 0x800000u;
    const ee_v4i maybe = (ee_v4i)(((ma * mb) & 0x7f8000u) == 0u) & (ee_v4i)((mb & 0xffffu) != 0u) & (ee_v4i)((x & 0x7f800000u) != 0u) &
                         (ee_v4i)((y & 0x7f800000u) != 0u);
    uint64_t w[2];
    memcpy(w, &maybe, 16);
    if (__builtin_expect((w[0] | w[1]) != 0, 0))
        for (int i = 0; i < 4; i++) {
            const uint32_t xi = x[i], yi = y[i];
            if (ee_mul_differs(xi, yi)) r[i] = ee_mul_slow(xi, yi, r[i]);
        }
#endif
    return r;
}

// the multiplier's table, from the model: once, before any of the game's maths
static inline void ee_float_init()
{
    for (uint32_t i = 0; i < 65536; i++) {
        const uint32_t ma = 0x800000, mb = 0x800000 | i;
        if (ee_mul_mantissa(ma, mb) != (uint64_t)ma * mb) g_ee_booth16[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}
// the one-mantissa tables (above), for a thread of their own; the host's float mode must be the EE's there too
static inline void ee_tables_build()
{
    for (uint32_t i = 0; i < (1u << 23); i++) {
        const float y = ee_float(0x3f800000u | i), q = ee_div_srt(1.0f, y);
        if (ee_bits(q) != ee_bits(1.0f / y)) g_ee_recip_up[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    for (uint32_t i = 0; i < (1u << 24); i++) {
        const float x = ee_float((i >> 23 ? 127u : 128u) << 23 | (i & 0x7fffff)), r = ee_sqrt_srt(x);
        if (ee_bits(r) != ee_bits(std::sqrt(x))) g_ee_sqrt_up[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    g_ee_tables.store(true, std::memory_order_release);
}
