// What is left of the GS: the privileged registers the game's libraries poll. Nothing is drawn through it (native.h),
// and a FINISH never has anything to wait for.
#include "hw.h"

static u64 g_csr, g_imr = 0xFF00;
static int g_field;

bool gs_io_write(u32 addr, u64 v)
{
    if (addr < 0x12000000 || addr >= 0x12002000) return addr == 0x10006000 || (addr >= 0x10003000 && addr < 0x10003800);
    if (addr == 0x12001000) __atomic_fetch_and(&g_csr, ~(v & 0x1F), __ATOMIC_SEQ_CST);     // write 1 to clear
    if (addr == 0x12001010) g_imr = v;
    return true;
}

bool gs_io_read(u32 addr, u64 &v)
{
    if (addr == 0x12001000) {           // CSR: FINISH always, FIELD in bit 13, revision/ID in the top half
        v = g_csr | 2 | (u64)g_field << 13 | 0x551Bull << 16;
        return true;
    }
    if (addr == 0x12001010) { v = g_imr; return true; }
    if (addr >= 0x12000000 && addr < 0x12002000) { v = 0; return true; }
    if (addr >= 0x10003000 && addr < 0x10003800) { v = 0; return true; }   // GIF_STAT: idle
    return false;
}

void gs_vblank()
{
    g_field ^= 1;
    __atomic_fetch_or(&g_csr, 8, __ATOMIC_SEQ_CST);     // VSINT
}
