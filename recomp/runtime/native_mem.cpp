// The game's memory functions as host ones, and LZDecompress from src/LZCompress.cpp. Where a range is not plain memory, or the two of a
// copy overlap, the generated code does it as retail would. HP2_HOTCHECK=1 checks LZDecompress against the generated code.
#include "ps2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// n bytes at a guest address, if all of them are main memory or the scratchpad
static inline u8 *span(u32 a, u32 n)
{
    const u32 top = a >> 28;
    if (top == 7) {
        const u32 p = a & 0x0FFFFFFF;
        return p + (u64)n <= 0x4000 ? g_spr + p : nullptr;
    }
    if (top != 0 && top != 2 && top != 3) return nullptr;
    const u32 e = (a & 0x0FFFFFFF) - EXT_BASE;              // the PC's memory past 32 MB (ps2.h)
    if (e < g_ext_size) return e + (u64)n <= g_ext_size ? g_ext + e : nullptr;
    const u32 p = a & (top ? 0x01FFFFFF : 0x0FFFFFFF);
    return p + (u64)n <= 0x02000000 ? g_ram + p : nullptr;
}

static inline bool copy(u32 d, u32 s, u32 n)
{
    u8 *to = span(d, n), *from = span(s, n);
    if (!to || !from || (to < from + n && from < to + n)) return false;
    memcpy(to, from, n);
    return true;
}

static inline bool fill(u32 d, u32 v, u32 n)
{
    u8 *to = span(d, n);
    if (!to) return false;
    memset(to, (int)(v & 255), n);
    return true;
}

void HOOK_bMemCpy(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    if (!copy(c.r[4].w[0], c.r[5].w[0], c.r[6].w[0])) orig(c);
}

void HOOK_bMemSet(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    if (!fill(c.r[4].w[0], c.r[5].w[0], c.r[6].w[0])) orig(c);
}

void HOOK_memcpy(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    if (copy(c.r[4].w[0], c.r[5].w[0], c.r[6].w[0])) c.r[2].d[0] = c.r[4].d[0];
    else orig(c);
}

void HOOK_memset(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    if (fill(c.r[4].w[0], c.r[5].w[0], c.r[6].w[0])) c.r[2].d[0] = c.r[4].d[0];
    else orig(c);
}

// a whole COMP file; what it unpacks to may lie under it, and is written from the front as ever. Returns the bytes written.
static u32 lz_decompress(const u8 *p_src_first, u8 *p_dst_first, u32 src_len)
{
    const u8 *p_src = p_src_first + 16, *const p_src_post = p_src_first + src_len, *const p_src_max16 = p_src_post - 32;
    u8 *p_dst = p_dst_first;
    u32 control = 1;
    while (p_src != p_src_post) {
        if (control == 1) {
            control = *p_src++ | 0x10000;
            control |= (u32)*p_src++ << 8;
        }
        int unroll = p_src <= p_src_max16 ? 16 : 1;
        while (unroll--) {
            if (control & 1) {
                int lenmt = *p_src++;
                const u8 *p = p_dst - (((lenmt & 0xf0) << 4) | *p_src++);
                lenmt &= 0xf;
                *p_dst++ = *p++;
                *p_dst++ = *p++;
                *p_dst++ = *p++;
                while (lenmt) {
                    *p_dst++ = *p++;
                    lenmt--;
                }
            } else {
                *p_dst++ = *p_src++;
            }
            control >>= 1;
        }
    }
    return (u32)(p_dst - p_dst_first);
}

void HOOK_LZDecompress__FPUcT0(Ctx &c, void (*orig)(Ctx &))
{
    if (g_irq_pending) ps2_take_interrupts(c);
    const u32 src = c.r[4].w[0], dst = c.r[5].w[0];
    const u8 *head = src ? span(src, 16) : nullptr;
    u32 magic = 0, unpacked = 0, src_len = 0;
    u16 method = 0;
    if (head) {
        memcpy(&magic, head, 4);
        memcpy(&method, head + 6, 2);
        memcpy(&unpacked, head + 8, 4);
        memcpy(&src_len, head + 12, 4);
    }
    const u8 *from = head && magic == 0x504d4f43 && method != 1 && src_len >= 48 ? span(src, src_len) : nullptr;
    u8 *to = from && dst ? span(dst, unpacked) : nullptr;
    if (!to) {
        orig(c);
        return;
    }
    static const bool check = getenv("HP2_HOTCHECK") != nullptr;
    if (check) {
        static u64 files, differ;
        std::vector<u8> mine(unpacked + 64);
        const u32 n = lz_decompress(from, mine.data(), src_len);
        orig(c);
        files++;
        if (n != c.r[2].w[0] || memcmp(to, mine.data(), n)) differ++;
        fprintf(stderr, "hotcheck: LZDecompress %llu files, %llu differ, %u bytes\n", (unsigned long long)files, (unsigned long long)differ, n);
        return;
    }
    c.r[2].d[0] = lz_decompress(from, to, src_len);
}
