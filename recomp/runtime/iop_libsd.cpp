// libsd (LIBSD.IRX) at its API, on the software SPU2.
//
// Register entries follow libsd: bit 0 core, bits 1-5 voice, bits 8-15 register;
// core registers have bit 7 set, voice addresses bit 6. Transfers complete at once
// and raise the transfer interrupt handler.
//
// HP2_SPULOG=1: every call into libsd goes to the IOP log.
#include "iop.h"
#include "spu2.h"

#include <cstdio>
#include <cstdlib>

#define A0 c.r[4]
#define A1 c.r[5]
#define A2 c.r[6]
#define A3 c.r[7]
#define V0 c.r[2]
#define ARG(n) ((n) < 4 ? c.r[4 + (n)] : iop_lw(c.r[29] + 4 * (n)))

static bool spulog()
{
    static const bool on = getenv("HP2_SPULOG") != nullptr;
    return on;
}
#define SLOG(...) (spulog() ? (void)fprintf(iop_log(), __VA_ARGS__) : (void)0)

static u32 g_trans_handler[2], g_trans_arg[2];

static void set_param(u32 entry, u16 value)
{
    int core = entry & 1, reg = (entry >> 8) & 0xFF;
    if (entry & 0x80) {
        if (reg == 0x09) spu2_set_master(core, SPU2_MVOLL, value);
        else if (reg == 0x0A) spu2_set_master(core, SPU2_MVOLR, value);
        else SLOG("libsd: core param %04x = %04x ignored\n", entry, value);
        return;
    }
    if (reg < 8) spu2_set_voice(core, (entry >> 1) & 31, reg, value);
}

static u16 get_param(u32 entry)
{
    int reg = (entry >> 8) & 0xFF;
    if (entry & 0x80) return 0;
    return reg < 8 ? spu2_voice(entry & 1, (entry >> 1) & 31, reg) : 0;
}

static void set_switch(u32 entry, u32 value)
{
    int core = entry & 1;
    switch ((entry >> 8) & 0xFF) {
    case 0x15: spu2_key_on(core, value); break;
    case 0x16: spu2_key_off(core, value); break;
    case 0x17: spu2_clear_endx(core); break;
    case 0x18: spu2_set_mix(core, SPU2_VMIXL, value); break;
    case 0x19: spu2_set_mix(core, SPU2_VMIXEL, value); break;
    case 0x1A: spu2_set_mix(core, SPU2_VMIXR, value); break;
    case 0x1B: spu2_set_mix(core, SPU2_VMIXER, value); break;
    default: SLOG("libsd: switch %04x = %06x ignored\n", entry, value);
    }
}

static u32 get_switch(u32 entry)
{
    int core = entry & 1;
    switch ((entry >> 8) & 0xFF) {
    case 0x17: return spu2_endx(core);
    case 0x18: return spu2_mix(core, SPU2_VMIXL);
    case 0x19: return spu2_mix(core, SPU2_VMIXEL);
    case 0x1A: return spu2_mix(core, SPU2_VMIXR);
    case 0x1B: return spu2_mix(core, SPU2_VMIXER);
    }
    return 0;
}

static void set_addr(u32 entry, u32 value)
{
    int reg = (entry >> 8) & 0xFF;
    if (reg >= 0x20 && reg <= 0x22) spu2_set_voice_addr(entry & 1, (entry >> 1) & 31, reg - 0x20, value);
    else SLOG("libsd: addr %04x = %06x ignored\n", entry, value);
}

static u32 get_addr(u32 entry)
{
    int reg = (entry >> 8) & 0xFF;
    if (reg >= 0x20 && reg <= 0x22) return spu2_voice_addr(entry & 1, (entry >> 1) & 31, reg - 0x20);
    return 0;
}

static void sceSdInit(IopRegs &c)
{
    SLOG("libsd: sceSdInit(%u)\n", A0);
    spu2_reset();
    V0 = 0;
}
static void sceSdSetParam(IopRegs &c)
{
    SLOG("libsd: SetParam %04x = %04x\n", A0 & 0xFFFF, A1 & 0xFFFF);
    set_param(A0 & 0xFFFF, (u16)A1);
}
static void sceSdGetParam(IopRegs &c) { V0 = get_param(A0 & 0xFFFF); }
static void sceSdSetSwitch(IopRegs &c)
{
    SLOG("libsd: SetSwitch %04x = %06x\n", A0 & 0xFFFF, A1);
    set_switch(A0 & 0xFFFF, A1);
}
static void sceSdGetSwitch(IopRegs &c) { V0 = get_switch(A0 & 0xFFFF); }
static void sceSdSetAddr(IopRegs &c)
{
    SLOG("libsd: SetAddr %04x = %06x\n", A0 & 0xFFFF, A1);
    set_addr(A0 & 0xFFFF, A1);
}
static void sceSdGetAddr(IopRegs &c) { V0 = get_addr(A0 & 0xFFFF); }
static void sceSdSetCoreAttr(IopRegs &c)
{
    SLOG("libsd: SetCoreAttr %04x = %u\n", A0 & 0xFFFF, A1);
}

// sceSdBatch {u16 func, u16 entry, u32 value}; (batch, returns, count)
static void sceSdProcBatch(IopRegs &c)
{
    u32 n = A2;
    for (u32 i = 0; i < n; i++) {
        u32 b = A0 + 8 * i, func = iop_lh(b), entry = iop_lh(b + 2), value = iop_lw(b + 4), ret = 0;
        switch (func) {
        case 1: set_param(entry, (u16)value); break;
        case 2: set_switch(entry, value); break;
        case 3: set_addr(entry, value); break;
        case 0x11: ret = get_param(entry); break;
        case 0x12: ret = get_switch(entry); break;
        case 0x13: ret = get_addr(entry); break;
        default: SLOG("libsd: batch func %u ignored\n", func);
        }
        if (A1) iop_sw(A1 + 4 * i, ret);
    }
    V0 = n;
}

// (channel, mode, iop address, spu address, size)
static void sceSdVoiceTrans(IopRegs &c)
{
    u32 ch = A0 & 1, mode = A1, iop = A2, spu = A3, size = ARG(4);
    SLOG("libsd: VoiceTrans ch %u mode %u iop %06x spu %06x size %u\n", ch, mode, iop, spu, size);
    u8 *p = iop_host(iop);
    if (p && (iop & 0x1FFFFFFF) + size <= IOP_RAM_SIZE) {
        if ((mode & 3) == 0) spu2_write(spu, p, size);
        else if ((mode & 3) == 1) spu2_read(spu, p, size);
    }
    iop_raise(g_trans_handler[ch], ch, g_trans_arg[ch]);
    V0 = size;
}
static void sceSdVoiceTransStatus(IopRegs &c) { V0 = 1; }

static void sceSdBlockTrans(IopRegs &c)
{
    static int n;
    if (n++ < 10) fprintf(iop_log(), "libsd: BlockTrans ch %u mode %x addr %06x size %u (not emulated)\n",
                          A0, A1, A2, A3);
    V0 = 0;
}
static void sceSdBlockTransStatus(IopRegs &c) { V0 = 0; }
static void sceSdSetEffectAttr(IopRegs &c)
{
    SLOG("libsd: SetEffectAttr core %u mode %u (not emulated)\n", iop_lw(A0), iop_lw(A0 + 4));
    V0 = 0;
}
static void sceSdClearEffectWorkArea(IopRegs &c) { V0 = 0; }
static void sceSdSetTransIntrHandler(IopRegs &c)
{
    u32 ch = A0 & 1;
    V0 = g_trans_handler[ch];
    g_trans_handler[ch] = A1;
    g_trans_arg[ch] = A2;
}

void iop_libsd_register()
{
    iop_add_hle("sceSdInit", sceSdInit);
    iop_add_hle("sceSdSetParam", sceSdSetParam);
    iop_add_hle("sceSdGetParam", sceSdGetParam);
    iop_add_hle("sceSdSetSwitch", sceSdSetSwitch);
    iop_add_hle("sceSdGetSwitch", sceSdGetSwitch);
    iop_add_hle("sceSdSetAddr", sceSdSetAddr);
    iop_add_hle("sceSdGetAddr", sceSdGetAddr);
    iop_add_hle("sceSdSetCoreAttr", sceSdSetCoreAttr);
    iop_add_hle("sceSdProcBatch", sceSdProcBatch);
    iop_add_hle("sceSdVoiceTrans", sceSdVoiceTrans);
    iop_add_hle("sceSdVoiceTransStatus", sceSdVoiceTransStatus);
    iop_add_hle("sceSdBlockTrans", sceSdBlockTrans);
    iop_add_hle("sceSdBlockTransStatus", sceSdBlockTransStatus);
    iop_add_hle("sceSdSetEffectAttr", sceSdSetEffectAttr);
    iop_add_hle("sceSdClearEffectWorkArea", sceSdClearEffectWorkArea);
    iop_add_hle("sceSdSetTransIntrHandler", sceSdSetTransIntrHandler);
}
