// IOP side of the sound path: the game's own STREAM.IRX (MultiStream 5.2), translated to C++ ahead of time
// (tools/recomp/gen_iop.py), over a small HLE IOP kernel; libsd sits on a software SPU2.
//
// Only modules loaded through iop_start_module run here (STREAM.IRX); everything
// else on the IOP stays at the EE SDK boundary in hle_iop.cpp.
#pragma once
#include "ps2.h"

#include <string>
#include <vector>

// ---- used by hle_iop.cpp (EE thread) ----------------------------------------------
int iop_start_module(const std::string &name, std::vector<u8> irx);   // result of start()
bool iop_has_server(u32 sid);
// blocking: copies ssize bytes from EE to the server buffer, runs the server function,
// copies rsize bytes of its answer back to EE memory
void iop_call_rpc(u32 sid, u32 fno, u32 ee_send, u32 ssize, u32 ee_recv, u32 rsize);
void iop_sif_from_ee(u32 iop_dst, u32 ee_src, u32 size);
bool iop_active();

// ---- shared by iop_*.cpp ----------------------------------------------------------
static const u32 IOP_RAM_SIZE = 0x200000;
extern u8 *g_iop_ram;

static inline u8 *iop_host(u32 a)
{
    a &= 0x1FFFFFFF;
    return a < IOP_RAM_SIZE ? g_iop_ram + a : nullptr;
}
u32 iop_bad_read(u32 a, int size);
void iop_bad_write(u32 a, u32 v, int size);

#define IOPMEM(T, n)                                                              \
    static inline T iop_l##n(u32 a)                                               \
    {                                                                             \
        u8 *p = iop_host(a);                                                      \
        if (p) { T v; memcpy(&v, p, sizeof v); return v; }                        \
        return (T)iop_bad_read(a, sizeof(T));                                     \
    }                                                                             \
    static inline void iop_s##n(u32 a, T v)                                       \
    {                                                                             \
        u8 *p = iop_host(a);                                                      \
        if (p) { memcpy(p, &v, sizeof v); return; }                               \
        iop_bad_write(a, (u32)v, sizeof(T));                                      \
    }
IOPMEM(u8, b) IOPMEM(u16, h) IOPMEM(u32, w)
#undef IOPMEM

std::string iop_str(u32 a);

struct IopRegs {
    u32 r[32];
    u32 hi, lo, pc;
};

// An HLE function is reached through a reserved opcode (0x3F) whose low 26 bits index
// this table. Before the call pc is set to $ra, so a plain function just returns.
typedef void (*IopHle)(IopRegs &);
u32 iop_hle_opcode(IopHle fn, const char *name);
const char *iop_hle_name(u32 index);

// runs until max instructions have executed or iop_stop() is called from an HLE function
void iop_execute(IopRegs &c, int max);
bool iop_gen_matches(u32 base, u32 text_size);     // the module in memory is the one the code was made from
void iop_stop();

// kernel (iop_kernel.cpp)
void iop_add_hle(const char *name, IopHle fn);
u32 iop_alloc(u32 size);
void iop_raise(u32 fn, u32 a0, u32 a1);     // run fn(a0, a1) as an interrupt handler
FILE *iop_log();

// libsd on the software SPU2 (iop_libsd.cpp)
void iop_libsd_register();
