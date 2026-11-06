// R3000A (MIPS I) interpreter for IOP modules.
//
// Branch delay slots are executed together with the branch. Load delay slots are
// ignored: compiled IOP code never reads a load target in its delay slot.
#include "iop.h"

#include <cstdio>
#include <vector>

static const u32 HLE_OP = 0x3Fu << 26;

struct HleEntry {
    IopHle fn;
    std::string name;
};
static std::vector<HleEntry> g_hle;
static bool g_stop;
u32 g_iop_hle_index;                // the HLE function being called

u32 iop_hle_opcode(IopHle fn, const char *name)
{
    g_hle.push_back({fn, name});
    return HLE_OP | (u32)(g_hle.size() - 1);
}

const char *iop_hle_name(u32 index)
{
    return index < g_hle.size() ? g_hle[index].name.c_str() : "?";
}

void iop_stop() { g_stop = true; }

u32 iop_bad_read(u32 a, int size)
{
    static int n;
    if (n++ < 20) fprintf(iop_log(), "IOP: read%d from unmapped %08x\n", size * 8, a);
    return 0;
}

void iop_bad_write(u32 a, u32 v, int size)
{
    static int n;
    if (n++ < 20) fprintf(iop_log(), "IOP: write%d %08x to unmapped %08x\n", size * 8, v, a);
}

std::string iop_str(u32 a)
{
    std::string s;
    for (u8 ch; a && (ch = iop_lb(a)) != 0 && s.size() < 1024; a++)
        s += (char)ch;
    return s;
}

static inline s32 sx16(u32 v) { return (s16)(u16)v; }

static void bad_insn(IopRegs &c, u32 pc, u32 op)
{
    fprintf(iop_log(), "IOP: unknown instruction %08x at %08x\n", op, pc);
    g_stop = true;
}

// one non-branch instruction; returns false for a branch or jump (handled by the caller)
static bool exec_simple(IopRegs &c, u32 pc, u32 op)
{
    u32 *r = c.r;
    u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    u32 imm = op & 0xFFFF;
    u32 addr = r[rs] + sx16(imm);
    switch (op >> 26) {
    case 0x00:
        switch (op & 63) {
        case 0x00: r[rd] = r[rt] << sa; break;
        case 0x02: r[rd] = r[rt] >> sa; break;
        case 0x03: r[rd] = (u32)((s32)r[rt] >> sa); break;
        case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;
        case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;
        case 0x07: r[rd] = (u32)((s32)r[rt] >> (r[rs] & 31)); break;
        case 0x08: case 0x09: return false;
        case 0x0C: fprintf(iop_log(), "IOP: syscall at %08x\n", pc); break;
        case 0x0D: fprintf(iop_log(), "IOP: break at %08x\n", pc); g_stop = true; break;
        case 0x10: r[rd] = c.hi; break;
        case 0x11: c.hi = r[rs]; break;
        case 0x12: r[rd] = c.lo; break;
        case 0x13: c.lo = r[rs]; break;
        case 0x18: {
            s64 p = (s64)(s32)r[rs] * (s32)r[rt];
            c.lo = (u32)p; c.hi = (u32)(p >> 32);
            break;
        }
        case 0x19: {
            u64 p = (u64)r[rs] * r[rt];
            c.lo = (u32)p; c.hi = (u32)(p >> 32);
            break;
        }
        case 0x1A: {
            s32 n = (s32)r[rs], d = (s32)r[rt];
            if (d == 0) { c.lo = n < 0 ? 1 : 0xFFFFFFFF; c.hi = (u32)n; }
            else if (n == INT32_MIN && d == -1) { c.lo = (u32)n; c.hi = 0; }
            else { c.lo = (u32)(n / d); c.hi = (u32)(n % d); }
            break;
        }
        case 0x1B:
            if (r[rt] == 0) { c.lo = 0xFFFFFFFF; c.hi = r[rs]; }
            else { c.lo = r[rs] / r[rt]; c.hi = r[rs] % r[rt]; }
            break;
        case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;
        case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;
        case 0x24: r[rd] = r[rs] & r[rt]; break;
        case 0x25: r[rd] = r[rs] | r[rt]; break;
        case 0x26: r[rd] = r[rs] ^ r[rt]; break;
        case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
        case 0x2A: r[rd] = (s32)r[rs] < (s32)r[rt]; break;
        case 0x2B: r[rd] = r[rs] < r[rt]; break;
        default: bad_insn(c, pc, op);
        }
        break;
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        return false;
    case 0x08: case 0x09: r[rt] = r[rs] + sx16(imm); break;
    case 0x0A: r[rt] = (s32)r[rs] < sx16(imm); break;
    case 0x0B: r[rt] = r[rs] < (u32)sx16(imm); break;
    case 0x0C: r[rt] = r[rs] & imm; break;
    case 0x0D: r[rt] = r[rs] | imm; break;
    case 0x0E: r[rt] = r[rs] ^ imm; break;
    case 0x0F: r[rt] = imm << 16; break;
    case 0x10:                                   // COP0: only mfc0/mtc0, status and cause
        if (rs == 0) r[rt] = 0;
        break;
    case 0x20: r[rt] = (u32)(s8)iop_lb(addr); break;
    case 0x21: r[rt] = (u32)(s16)iop_lh(addr); break;
    case 0x22: {                                 // lwl
        u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
        r[rt] = (r[rt] & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
        break;
    }
    case 0x23: r[rt] = iop_lw(addr); break;
    case 0x24: r[rt] = iop_lb(addr); break;
    case 0x25: r[rt] = iop_lh(addr); break;
    case 0x26: {                                 // lwr
        u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
        r[rt] = (r[rt] & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
        break;
    }
    case 0x28: iop_sb(addr, (u8)r[rt]); break;
    case 0x29: iop_sh(addr, (u16)r[rt]); break;
    case 0x2A: {                                 // swl
        u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
        iop_sw(addr & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (r[rt] >> (24 - sh)));
        break;
    }
    case 0x2B: iop_sw(addr, r[rt]); break;
    case 0x2E: {                                 // swr
        u32 sh = (addr & 3) * 8, w = iop_lw(addr & ~3u);
        iop_sw(addr & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (r[rt] << sh));
        break;
    }
    case 0x3F: {
        u32 idx = op & 0x03FFFFFF;
        c.r[0] = 0;
        c.pc = c.r[31];
        g_iop_hle_index = idx;
        if (idx < g_hle.size()) g_hle[idx].fn(c);
        else bad_insn(c, pc, op);
        return true;
    }
    default: bad_insn(c, pc, op);
    }
    r[0] = 0;
    return true;
}

void iop_execute(IopRegs &c, int max)
{
    g_stop = false;
    while (max-- > 0 && !g_stop) {
        u32 pc = c.pc, op = iop_lw(pc);
        c.pc = pc + 4;
        if (exec_simple(c, pc, op))
            continue;
        // branch or jump: evaluate, run the delay slot, then go
        u32 *r = c.r;
        u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31;
        u32 target = pc + 8;
        bool taken = false;
        switch (op >> 26) {
        case 0x00:                                   // jr / jalr
            target = r[rs];
            taken = true;
            if ((op & 63) == 0x09) r[(op >> 11) & 31] = pc + 8;
            break;
        case 0x01: {
            bool lt = (s32)r[rs] < 0;
            taken = (rt & 1) ? !lt : lt;
            if (rt & 0x10) r[31] = pc + 8;          // bltzal / bgezal
            target = pc + 4 + (sx16(op) << 2);
            break;
        }
        case 0x02: case 0x03:
            target = (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
            taken = true;
            if ((op >> 26) == 3) r[31] = pc + 8;
            break;
        case 0x04: taken = r[rs] == r[rt]; target = pc + 4 + (sx16(op) << 2); break;
        case 0x05: taken = r[rs] != r[rt]; target = pc + 4 + (sx16(op) << 2); break;
        case 0x06: taken = (s32)r[rs] <= 0; target = pc + 4 + (sx16(op) << 2); break;
        case 0x07: taken = (s32)r[rs] > 0; target = pc + 4 + (sx16(op) << 2); break;
        }
        r[0] = 0;
        u32 slot = iop_lw(pc + 4);
        c.pc = pc + 8;
        if (!exec_simple(c, pc + 4, slot)) {
            fprintf(iop_log(), "IOP: branch in delay slot at %08x\n", pc + 4);
            g_stop = true;
        }
        if (taken) c.pc = target;
        max--;
    }
}
