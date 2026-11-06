// DMAC: ten channels, source chain mode, D_STAT interrupts.
//
// A transfer runs to completion when the game sets STR in CHCR; there is no
// bus timing. VIF1 (channel 1) only has its VIFcodes followed for the interrupts and
// MARK; what goes to the GIF (channel 2) goes nowhere; the scratchpad (channels 8
// and 9) gets its data; other channels are acknowledged only.
#include "hw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern FILE *g_hle_log;

volatile u32 g_dmac_pending;

enum { CH_VIF0, CH_VIF1, CH_GIF, CH_IPU_FROM, CH_IPU_TO, CH_SIF0, CH_SIF1, CH_SIF2,
       CH_SPR_FROM, CH_SPR_TO, NUM_CH };
static const u32 CH_BASE[NUM_CH] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                    0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};

struct Chan {
    u32 chcr, madr, qwc, tadr, asr0, asr1, sadr;
};
static Chan g_ch[NUM_CH];
static u32 d_ctrl, d_stat, d_pcr, d_sqwc, d_rbsr, d_rbor, d_stadr, d_enable = 0x1201;

enum { CHCR_DIR = 1, CHCR_TTE = 1 << 6, CHCR_TIE = 1 << 7, CHCR_STR = 1 << 8 };

// tag and data addresses: bit 31 selects the scratchpad
static u32 bus_addr(u32 a) { return (a & 0x80000000u) ? (0x70000000u | (a & 0x3FF0)) : (a & 0x01FFFFF0); }

static void sink(int ch, u32 addr, u32 qwc)
{
    if (!qwc) return;
    if (ch == CH_VIF1) {
        // only the sizes are read: the packet where it lies, unless it runs past the end of its memory
        if (addr >> 28 != 7 && addr + qwc * 16 <= 0x02000000) {
            vif1_scan((const u32 *)(g_ram + addr), qwc * 4);
            return;
        }
        std::vector<u32> words(qwc * 4);
        for (u32 i = 0; i < qwc; i++) {
            u128 q = LQ(addr + i * 16);
            memcpy(&words[i * 4], &q, 16);
        }
        vif1_scan(words.data(), qwc * 4);
    } else if (ch == CH_SPR_TO) {
        for (u32 i = 0; i < qwc; i++)
            SQ(0x70000000u | ((g_ch[ch].sadr + i * 16) & 0x3FF0), LQ(addr + i * 16));
        g_ch[ch].sadr = (g_ch[ch].sadr + qwc * 16) & 0x3FF0;
    }
}

static void finish(int ch)
{
    Chan &c = g_ch[ch];
    __atomic_fetch_and(&c.chcr, ~(u32)CHCR_STR, __ATOMIC_SEQ_CST);
    u32 st = __atomic_or_fetch(&d_stat, 1u << ch, __ATOMIC_SEQ_CST);
    if (st & (1u << (16 + ch))) {
        __atomic_fetch_or(&g_dmac_pending, 1u << ch, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&g_irq_pending, 1u << 31, __ATOMIC_SEQ_CST);   // delivered with the INTC causes
    }
}

static void run_chain(int ch)
{
    Chan &c = g_ch[ch];
    u32 stack[2];
    int sp = 0;
    for (int guard = 0; guard < 1 << 20; guard++) {
        u32 tag_addr = bus_addr(c.tadr);
        u128 tag = LQ(tag_addr);
        u32 lo = (u32)tag, addr = (u32)(tag >> 32);
        u32 qwc = lo & 0xFFFF, id = (lo >> 28) & 7;
        bool irq = lo >> 31;
        c.chcr = (c.chcr & 0xFFFF) | (lo & 0xFFFF0000u);
        static int dbg;
        static const bool gs_log = getenv("HP2_GSLOG") != nullptr;
        if (ch == CH_GIF && dbg < 30 && gs_log) {
            dbg++;
            fprintf(g_hle_log, "  ch2 tag @%08x id %u qwc %u addr %08x\n", tag_addr, id, qwc, addr);
        }
        // VIF channels see the upper half of every tag as two VIFcodes
        if ((c.chcr & CHCR_TTE) && ch == CH_VIF1) {
            u32 codes[2] = {(u32)(tag >> 64), (u32)(tag >> 96)};
            vif1_scan(codes, 2);
        }
        u32 after_tag = c.tadr + 16;
        bool end = false;
        switch (id) {
        case 0:                         // refe
            sink(ch, bus_addr(addr), qwc);
            end = true;
            break;
        case 1:                         // cnt
            sink(ch, bus_addr(after_tag), qwc);
            c.tadr = after_tag + qwc * 16;
            break;
        case 2:                         // next
            sink(ch, bus_addr(after_tag), qwc);
            c.tadr = addr;
            break;
        case 3: case 4:                 // ref, refs
            sink(ch, bus_addr(addr), qwc);
            c.tadr = after_tag;
            break;
        case 5:                         // call
            sink(ch, bus_addr(after_tag), qwc);
            if (sp < 2) stack[sp++] = after_tag + qwc * 16;
            c.tadr = addr;
            break;
        case 6:                         // ret
            sink(ch, bus_addr(after_tag), qwc);
            if (sp > 0) c.tadr = stack[--sp];
            else end = true;
            break;
        case 7:                         // end
            sink(ch, bus_addr(after_tag), qwc);
            end = true;
            break;
        }
        c.qwc = 0;
        if (end || (irq && (c.chcr & CHCR_TIE))) break;
    }
    finish(ch);
}

static void start(int ch)
{
    Chan &c = g_ch[ch];
    u32 mode = (c.chcr >> 2) & 3;
    static int n;
    if (n++ < 6)
        fprintf(g_hle_log, "DMA ch%d start mode %u madr %08x qwc %u tadr %08x\n", ch, mode, c.madr,
                c.qwc, c.tadr);
    if (ch == CH_SPR_FROM) {            // scratchpad -> RAM, normal mode
        for (u32 i = 0; i < c.qwc; i++)
            SQ(c.madr + i * 16, LQ(0x70000000u | ((c.sadr + i * 16) & 0x3FF0)));
        c.madr += c.qwc * 16;
        c.qwc = 0;
        finish(ch);
        return;
    }
    if (mode == 1) {
        // a block left over from a previous tag goes first, then the chain
        if (c.qwc) {
            sink(ch, bus_addr(c.madr), c.qwc);
            c.madr += c.qwc * 16;
            c.qwc = 0;
        }
        run_chain(ch);
        return;
    }
    sink(ch, bus_addr(c.madr), c.qwc);
    c.madr += c.qwc * 16;
    c.qwc = 0;
    finish(ch);
}

static Chan *chan_of(u32 addr, u32 &reg)
{
    for (int i = 0; i < NUM_CH; i++)
        if (addr >= CH_BASE[i] && addr < CH_BASE[i] + 0x100) {
            reg = addr - CH_BASE[i];
            return &g_ch[i];
        }
    return nullptr;
}

bool dma_io_read(u32 addr, u64 &v)
{
    u32 reg;
    if (Chan *c = chan_of(addr, reg)) {
        switch (reg) {
        case 0x00: v = c->chcr; return true;
        case 0x10: v = c->madr; return true;
        case 0x20: v = c->qwc; return true;
        case 0x30: v = c->tadr; return true;
        case 0x40: v = c->asr0; return true;
        case 0x50: v = c->asr1; return true;
        case 0x80: v = c->sadr; return true;
        }
        v = 0;
        return true;
    }
    switch (addr) {
    case 0x1000E000: v = d_ctrl; return true;
    case 0x1000E010: v = d_stat; return true;
    case 0x1000E020: v = d_pcr; return true;
    case 0x1000E030: v = d_sqwc; return true;
    case 0x1000E040: v = d_rbsr; return true;
    case 0x1000E050: v = d_rbor; return true;
    case 0x1000E060: v = d_stadr; return true;
    case 0x1000F520: v = d_enable; return true;
    }
    return false;
}

bool dma_io_write(u32 addr, u64 v64)
{
    u32 v = (u32)v64, reg;
    if (Chan *c = chan_of(addr, reg)) {
        int ch = (int)(c - g_ch);
        switch (reg) {
        case 0x00:
            c->chcr = v;
            if (v & CHCR_STR) {
                start(ch);
            }
            break;
        case 0x10: c->madr = v; break;
        case 0x20: c->qwc = v & 0xFFFF; break;
        case 0x30: c->tadr = v; break;
        case 0x40: c->asr0 = v; break;
        case 0x50: c->asr1 = v; break;
        case 0x80: c->sadr = v & 0x3FF0; break;
        }
        return true;
    }
    switch (addr) {
    case 0x1000E000: d_ctrl = v; return true;
    case 0x1000E010:                    // CIS bits clear on 1, CIM bits toggle on 1
    {
        u32 old = __atomic_load_n(&d_stat, __ATOMIC_SEQ_CST), nw;
        do nw = (old & ~(v & 0xFFFF)) ^ (v & 0xFFFF0000u);
        while (!__atomic_compare_exchange_n(&d_stat, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        return true;
    }
    case 0x1000E020: d_pcr = v; return true;
    case 0x1000E030: d_sqwc = v; return true;
    case 0x1000E040: d_rbsr = v; return true;
    case 0x1000E050: d_rbor = v; return true;
    case 0x1000E060: d_stadr = v; return true;
    case 0x1000F590: d_enable = v; return true;
    }
    return false;
}
