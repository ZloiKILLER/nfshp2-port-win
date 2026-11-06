// VIF1 without VU1: the game's render-stage queue learns that a stage has finished from the interrupt bit of a VIFcode, and reads MARK.
// A pass that only follows the VIFcodes' sizes gives both; what they carry for VU1 and the GS is not looked at (native.h reads the
// game's draw lists before they become packets).
#include "hw.h"

#include <algorithm>

static struct {
    u32 code, need, cl = 1, wl = 1, mark, stat;
} g_vif;

static void command_done()
{
    if (g_vif.code >> 31) {             // interrupt bit
        g_vif.stat |= 1u << 11;         // INT
        raise_intc(INTC_VIF1);
    }
}

void vif1_scan(const u32 *w, u32 n)
{
    auto &s = g_vif;
    for (u32 i = 0; i < n;) {
        if (s.need) {
            u32 k = std::min(s.need, n - i);
            i += k;
            if ((s.need -= k) == 0) command_done();
            continue;
        }
        u32 code = s.code = w[i++];
        u32 imm = code & 0xFFFF, num = (code >> 16) & 0xFF, cmd = (code >> 24) & 0x7F;
        switch (cmd) {
        case 0x01: s.cl = imm & 0xFF; s.wl = imm >> 8; break;   // STCYCL
        case 0x07: s.mark = imm; break;                         // MARK
        case 0x20: s.need = 1; break;                           // STMASK
        case 0x30: case 0x31: s.need = 4; break;                // STROW / STCOL
        case 0x4A: s.need = (num ? num : 256) * 2; break;       // MPG
        case 0x50: case 0x51: s.need = (imm ? imm : 65536) * 4; break;  // DIRECT / DIRECTHL
        default:
            if ((cmd & 0x60) == 0x60) {                         // UNPACK
                u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
                u32 nv = num ? num : 256;
                u32 bits = vl == 3 ? 16 : (32 >> vl) * (vn + 1);
                u32 written = nv;
                if (s.wl < s.cl && s.wl) written = s.wl * (nv / s.wl);
                s.need = (bits * written + 31) / 32;
            }
            break;
        }
        if (!s.need) command_done();
    }
}

// VIF1 registers at 0x10003C00
bool vif_io_read(u32 addr, u64 &v)
{
    switch (addr) {
    case 0x10003C00: v = g_vif.stat; return true;
    case 0x10003C30: v = g_vif.mark; return true;
    case 0x10003C40: v = g_vif.cl | g_vif.wl << 8; return true;
    case 0x10003C80: v = g_vif.code; return true;
    }
    return addr >= 0x10003C00 && addr < 0x10003E00 ? (v = 0, true) : false;
}

bool vif_io_write(u32 addr, u64 value)
{
    if (addr == 0x10003C10) {           // FBRST: STC clears the stall/interrupt state
        if (value & 8) g_vif.stat &= ~(1u << 11);
        if (value & 1) g_vif.need = 0;
        return true;
    }
    if (addr == 0x10003C30) { g_vif.mark = (u32)value; return true; }
    return addr >= 0x10003C00 && addr < 0x10003E00;
}
