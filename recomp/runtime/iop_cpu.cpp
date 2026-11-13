// The IOP's code. The one module the port runs there, the game's sound driver STREAM.IRX, is translated to C++ ahead of time
// (tools/recomp/gen_iop.py): a case for each word of its code, in functions of 1024 words (iop_stream_*.cpp). Nothing is decoded
// while the game runs. What is here is the way in and out: iop_execute runs a thread's registers through those functions until its
// slice is spent or the kernel stops it, and the reserved opcode the kernel puts at an import's stub and at its own return
// addresses is a call of the kernel's function.
#include "iop.h"
#include "iop_gen.h"
#include "iop_gen_table.h"

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

u32 iop_gen_bad(u32 pc, u32 op)
{
    fprintf(iop_log(), "IOP: unknown instruction %08x at %08x\n", op, pc);
    g_stop = true;
    return pc + 4;
}

void iop_gen_syscall(u32 pc) { fprintf(iop_log(), "IOP: syscall at %08x\n", pc); }

void iop_gen_break(u32 pc)
{
    fprintf(iop_log(), "IOP: break at %08x\n", pc);
    g_stop = true;
}

u32 iop_hle_call(IopRegs &c, u32 pc)
{
    const u32 op = iop_lw(pc), index = op & 0x03FFFFFF;
    if ((op >> 26) != 0x3F || index >= g_hle.size()) return iop_gen_bad(pc, op);
    c.r[0] = 0;
    c.pc = c.r[31];
    g_iop_hle_index = index;
    g_hle[index].fn(c);
    return c.pc;
}

// the words the generated code was made from are the words in memory: asked by the kernel when it has loaded the module
bool iop_gen_matches(u32 base, u32 text_size)
{
    if (base != kIopGenBase || text_size != kIopGenTextSize) return false;
    u32 h = 0x811C9DC5;
    for (u32 a = base; a < base + text_size; a++) h = (h ^ iop_lb(a)) * 0x01000193;
    return h == kIopGenTextHash;
}

// runs until the slice of max instructions is spent (counted at the branches) or iop_stop() is called from a function of the kernel's
void iop_execute(IopRegs &c, int max)
{
    g_stop = false;
    int budget = max;
    while (budget > 0 && !g_stop) {
        const u32 pc = c.pc;
        if (pc - kIopGenBase < kIopGenTextSize) {
            c.pc = kIopGenChunks[(pc - kIopGenBase) >> kIopGenChunkShift](c, pc, budget);
        } else {                        // the kernel's own return addresses, below the module
            c.pc = iop_hle_call(c, pc);
            budget--;
        }
    }
}
