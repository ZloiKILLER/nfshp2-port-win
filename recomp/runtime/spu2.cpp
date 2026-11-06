// Software SPU2 (see spu2.h). ADPCM decoding, envelope and voice behaviour follow
// the PlayStation SPU, which the SPU2 voices keep: 16-byte blocks of 28 samples,
// flags end/repeat/loop start in byte 1, the ADSR rate formula, the Gaussian interpolation, the noise generator and the reverb from
// psx-spx; the effect presets and the core registers from libsd (ps2sdk's freesd, AFL 2.0: effect.c, libsd-common.h); the cores' mixing
// and MMIX's gates as PCSX2 has them.
#include "spu2.h"
#include "pcconfig.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "audio_out.h"

namespace {

enum Phase { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

struct Voice {
    u16 reg[8];
    u32 ssa, lsa, block;            // bytes
    bool keyed;                     // keyed on since reset: the voice runs
    s16 buf[28];
    int pos;
    u32 counter;
    s16 hist[4];
    s32 p1, p2;
    u8 flags;
    Phase phase;
    s32 env;
    u32 env_tick;
    s32 vol[2];                     // current volume L/R (VOLXL/VOLXR)
    u32 vol_tick[2];
    s32 out;                        // this sample, after the envelope: what the next voice's pitch modulation takes
};

// the effect's work area: the game's, past the samples at the end of sound RAM; here a buffer of the core's own, the same size, so that a
// wrong area can never write over the samples
static const u32 kFxMax = 0x20000;

struct Core {
    Voice v[24];
    u32 endx;
    u32 mix[6];                     // VMIXL VMIXEL VMIXR VMIXER PMON NON
    u16 mvol[2];                    // MVOLL/R as written
    s32 mvol_cur[2];                // the master volumes' levels, swept
    u32 mvol_tick[2];
    u16 param[7];                   // MMIX EVOL AVOL BVOL
    bool fx_on;
    u32 esa, eea;                   // the effect's work area, bytes
    u16 rev[32];                    // the effect's registers in libsd's order (FB_SRC_A ... IN_COEF_R), psx-spx's dAPF1 ... vRIN
    s16 fx[kFxMax / 2];
    u32 fx_pos;                     // the current buffer address, in samples from ESA
    s32 fx_in[2];                   // the first of the two 48 kHz input samples the 24 kHz effect takes
    s32 fx_out[2], fx_prev[2];
    bool fx_odd;
    u32 noise_clock;
    s32 noise_timer;
    u16 noise;
};

u8 g_sram[SPU2_RAM_SIZE];
Core g_core[2];
std::mutex g_lock;

const int K0[5] = {0, 60, 115, 98, 122};
const int K1[5] = {0, 0, -52, -55, -60};

const s16 kGauss[512] = {
#include "spu2_gauss.inc"
};

// libsd's effect presets (effect.c: EffectSizes, EffectParams), the 32 registers of each mode in its order
const u16 kFxSize[10] = {0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0};    // 16-byte units
const u32 kFxParams[160] = {
    // off
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00010001, 0x00000000, 0x00000000,
    0x00000000, 0x00010001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    // room
    0x005B007D, 0x54B86D80, 0x0000BED0, 0xBA800000, 0x53005800, 0x033304D6, 0x022703F0, 0x01EF0374,
    0x01B70336, 0x01B60335, 0x01B50334, 0x01B50334, 0x01B50334, 0x013601B4, 0x005C00B8, 0x80008000,
    // studio 1
    0x00250033, 0x4FA870F0, 0x4410BCE0, 0x9C00C0F0, 0x4EC05280, 0x031B03E4, 0x02AF03A4, 0x02660372,
    0x025D031C, 0x018E025C, 0x0135022F, 0x00B701D2, 0x00B5018F, 0x008000B4, 0x0026004C, 0x80008000,
    // studio 2
    0x007F00B1, 0x4FA870F0, 0x4510BCE0, 0xB4C0BEF0, 0x4EC05280, 0x076B0904, 0x065F0824, 0x061607A2,
    0x05ED076C, 0x042E05EC, 0x0305050F, 0x02B70462, 0x0265042F, 0x01B20264, 0x00800100, 0x80008000,
    // studio 3
    0x00A900E3, 0x4FA86F60, 0x4510BCE0, 0xA680BEF0, 0x52C05680, 0x0B580DFB, 0x0A3C0D09, 0x09730BD9,
    0x08DA0B59, 0x05E908D9, 0x04B007EC, 0x03D206EF, 0x031D05EA, 0x0238031C, 0x00AA0154, 0x80008000,
    // hall
    0x013901A5, 0x50006000, 0xB8004C00, 0xC000BC00, 0x5C006000, 0x11BB15BA, 0x10BD14C2, 0x0DC111BC,
    0x0DC311C0, 0x09C10DC0, 0x07C10BC4, 0x06CD0A00, 0x05C109C2, 0x041A05C0, 0x013A0274, 0x80008000,
    // space
    0x0231033D, 0x50007E00, 0xB000B400, 0xB0004C00, 0x54006000, 0x1A311ED6, 0x183B1D14, 0x16B21BC2,
    0x15EF1A32, 0x105515EE, 0x0F2D1334, 0x0C5D11F6, 0x0AE11056, 0x07A20AE0, 0x02320464, 0x80008000,
    // echo
    0x00030003, 0x7FFF7FFF, 0x00000000, 0x81000000, 0x00000000, 0x0FFD1FFD, 0x00091009, 0x00000000,
    0x00091009, 0x1FFF1FFF, 0x1FFE1FFE, 0x1FFE1FFE, 0x1FFE1FFE, 0x10041008, 0x00040008, 0x80008000,
    // delay
    0x00030003, 0x7FFF7FFF, 0x00000000, 0x00000000, 0x00000000, 0x0FFD1FFD, 0x00091009, 0x00000000,
    0x00091009, 0x1FFF1FFF, 0x1FFE1FFE, 0x1FFE1FFE, 0x1FFE1FFE, 0x10041008, 0x00040008, 0x80008000,
    // pipe
    0x00130017, 0x4FA870F0, 0x4510BCE0, 0x8500BEF0, 0x54C05F80, 0x02AF0371, 0x01DF02E5, 0x01D702B0,
    0x026A0358, 0x011E01D6, 0x00B1012D, 0x0059011F, 0x00E301A0, 0x00400058, 0x00140028, 0x80008000,
};

// a fixed volume register: 15-bit signed value
s32 volume(u16 v) { return (s16)(v << 1); }

// one step of the envelope / volume sweep counter (psx-spx): rate is
// shift << 2 | step, the level moves by the step every 2^(shift-11) samples
s32 rate_step(s32 level, int rate, bool decrease, bool exp, u32 &tick)
{
    int shift = rate >> 2, stepv = rate & 3;
    s32 step = decrease ? -8 + stepv : 7 - stepv;
    u32 cycles = 1u << std::max(0, shift - 11);
    step <<= std::max(0, 11 - shift);
    if (exp && !decrease && level > 0x6000) cycles *= 4;
    if (exp && decrease) step = step * level >> 15;
    if (++tick < cycles) return level;
    tick = 0;
    return std::clamp(level + step, 0, 0x7FFF);
}

// a volume register's level: fixed, or swept (bit 15; 14 exponential, 13 decrease, 12 negative phase, 6-0 rate) from the current level
void sweep(u16 r, s32 &level, u32 &tick)
{
    if (!(r & 0x8000)) {
        level = (s16)(r << 1);
        return;
    }
    s32 mag = std::abs(level);
    mag = rate_step(mag, r & 0x7F, (r >> 13) & 1, (r >> 14) & 1, tick);
    level = (r & 0x1000) ? -mag : mag;
}

void voice_volume(Voice &v, int lr) { sweep(v.reg[SPU2_VOLL + lr], v.vol[lr], v.vol_tick[lr]); }

void decode(Voice &v)
{
    const u8 *b = g_sram + (v.block & (SPU2_RAM_SIZE - 16));
    int shift = b[0] & 15, filt = std::min(b[0] >> 4, 4);
    if (shift > 12) shift = 9;
    v.flags = b[1];
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> (4 * (i & 1))) & 15;
        s32 s = ((s32)(s16)(nib << 12)) >> shift;
        s += (v.p1 * K0[filt] + v.p2 * K1[filt]) >> 6;
        s = std::clamp(s, -32768, 32767);
        v.p2 = v.p1;
        v.p1 = s;
        v.buf[i] = (s16)s;
    }
    if (v.flags & 4) v.lsa = v.block;
}

void end_block(Core &c, int n)
{
    Voice &v = c.v[n];
    if (v.flags & 1) {
        c.endx |= 1u << n;
        v.block = v.lsa;
        if (!(v.flags & 2)) {        // end without repeat: the voice goes silent
            v.phase = PH_OFF;
            v.env = 0;
        }
    } else {
        v.block = (v.block + 16) & (SPU2_RAM_SIZE - 1);
    }
    decode(v);
}

void env_step(Voice &v, int rate, bool decrease, bool exp)
{
    v.env = rate_step(v.env, rate, decrease, exp, v.env_tick);
}

void envelope(Voice &v)
{
    u16 a1 = v.reg[SPU2_ADSR1], a2 = v.reg[SPU2_ADSR2];
    switch (v.phase) {
    case PH_ATTACK:
        env_step(v, (a1 >> 8) & 0x7F, false, a1 >> 15);
        if (v.env >= 0x7FFF) v.phase = PH_DECAY;
        break;
    case PH_DECAY:
        env_step(v, ((a1 >> 4) & 15) * 4, true, true);
        if (v.env <= (((a1 & 15) + 1) << 11)) v.phase = PH_SUSTAIN;
        break;
    case PH_SUSTAIN:
        env_step(v, (a2 >> 6) & 0x7F, (a2 >> 14) & 1, a2 >> 15);
        break;
    case PH_RELEASE:
        env_step(v, (a2 & 31) * 4, true, (a2 >> 5) & 1);
        if (v.env == 0) v.phase = PH_OFF;
        break;
    case PH_OFF:
        v.env = 0;
        break;
    }
}

// the 4 most recent samples (oldest first) by the counter's bits 4-11
s32 gauss(const s16 *h, u32 counter)
{
    const int i = (counter >> 4) & 0xFF;
    s32 out = (kGauss[0xFF - i] * h[0]) >> 15;
    out += (kGauss[0x1FF - i] * h[1]) >> 15;
    out += (kGauss[0x100 + i] * h[2]) >> 15;
    out += (kGauss[i] * h[3]) >> 15;
    return std::clamp(out, -32768, 32767);
}

// the noise generator (psx-spx): clock bits 0-1 the step, 2-5 the shift; a sample of the core's at 48 kHz
void noise_step(Core &c)
{
    const int step = (int)(c.noise_clock & 3) + 4, shift = (int)(c.noise_clock >> 2) & 15;
    c.noise_timer -= step;
    const u16 parity = ((c.noise >> 15) ^ (c.noise >> 12) ^ (c.noise >> 11) ^ (c.noise >> 10) ^ 1) & 1;
    if (c.noise_timer < 0) c.noise = (u16)(c.noise * 2 + parity);
    if (c.noise_timer < 0) c.noise_timer += 0x20000 >> shift;
    if (c.noise_timer < 0) c.noise_timer += 0x20000 >> shift;
}

// one 48 kHz sample of voice n, envelope applied; the pitch modulated by the voice before it where PMON says so
s32 voice_sample(Core &c, int n)
{
    Voice &v = c.v[n];
    s32 s = (c.mix[SPU2_NON] >> n & 1) ? (s16)c.noise : gauss(v.hist, v.counter);
    u32 step = std::min<u32>(v.reg[SPU2_PITCH], 0x3FFF);
    if (n && (c.mix[SPU2_PMON] >> n & 1)) step = std::min<u32>((u32)((s64)step * (c.v[n - 1].out + 0x8000) >> 15), 0x3FFF);
    v.counter += step;
    while (v.counter >= 0x1000) {
        v.counter -= 0x1000;
        v.hist[0] = v.hist[1];
        v.hist[1] = v.hist[2];
        v.hist[2] = v.hist[3];
        v.hist[3] = v.buf[v.pos];
        if (++v.pos == 28) {
            v.pos = 0;
            end_block(c, n);
        }
    }
    envelope(v);
    v.reg[SPU2_ENVX] = (u16)v.env;
    v.out = s * v.env >> 15;
    return v.out;
}

// ---- the effect (psx-spx's reverb formula; the SPU2 runs it at 24 kHz on its work area) ----------------------------------------
enum {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
    mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
    mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN
};

u32 fx_len(const Core &c) { return std::clamp<u32>((c.eea + 1 - c.esa) / 2, 1, kFxMax / 2); }

// the work area's sample at `off` samples from the current address
s16 &fx_at(Core &c, s32 off)
{
    const u32 n = fx_len(c);
    s64 i = ((s64)c.fx_pos + off) % (s64)n;
    if (i < 0) i += n;
    return c.fx[i];
}
// a register's offset: an address register counts 8 bytes, 4 samples
s32 fx_off(const Core &c, int r) { return (s32)c.rev[r] * 4; }
s32 vr(const Core &c, int r) { return (s16)c.rev[r]; }
s16 sat(s32 v) { return (s16)std::clamp(v, -32768, 32767); }
s32 mulv(s32 a, s32 v) { return a * v >> 15; }

void fx_tick(Core &c, s32 inl, s32 inr, s32 &outl, s32 &outr)
{
    const s32 lin = mulv(inl, vr(c, vLIN)), rin = mulv(inr, vr(c, vRIN));
    const s32 wall = vr(c, vWALL), iir = vr(c, vIIR);
    auto refl = [&](int dst, int src, s32 in) {
        const s32 prev = fx_at(c, fx_off(c, dst) - 1);
        fx_at(c, fx_off(c, dst)) = sat(mulv(in + mulv(fx_at(c, fx_off(c, src)), wall) - prev, iir) + prev);
    };
    refl(mLSAME, dLSAME, lin);
    refl(mRSAME, dRSAME, rin);
    refl(mLDIFF, dRDIFF, lin);
    refl(mRDIFF, dLDIFF, rin);
    s32 l = mulv(fx_at(c, fx_off(c, mLCOMB1)), vr(c, vCOMB1)) + mulv(fx_at(c, fx_off(c, mLCOMB2)), vr(c, vCOMB2)) +
            mulv(fx_at(c, fx_off(c, mLCOMB3)), vr(c, vCOMB3)) + mulv(fx_at(c, fx_off(c, mLCOMB4)), vr(c, vCOMB4));
    s32 r = mulv(fx_at(c, fx_off(c, mRCOMB1)), vr(c, vCOMB1)) + mulv(fx_at(c, fx_off(c, mRCOMB2)), vr(c, vCOMB2)) +
            mulv(fx_at(c, fx_off(c, mRCOMB3)), vr(c, vCOMB3)) + mulv(fx_at(c, fx_off(c, mRCOMB4)), vr(c, vCOMB4));
    auto apf = [&](s32 x, int m, int d, int v) {
        const s32 back = fx_at(c, fx_off(c, m) - fx_off(c, d));
        x -= mulv(back, vr(c, v));
        fx_at(c, fx_off(c, m)) = sat(x);
        return mulv(x, vr(c, v)) + back;
    };
    l = apf(l, mLAPF1, dAPF1, vAPF1);
    r = apf(r, mRAPF1, dAPF1, vAPF1);
    l = apf(l, mLAPF2, dAPF2, vAPF2);
    r = apf(r, mRAPF2, dAPF2, vAPF2);
    outl = sat(l);
    outr = sat(r);
    c.fx_pos = (c.fx_pos + 1) % fx_len(c);
}

// the effect for one 48 kHz sample: the input of two samples averaged into one 24 kHz step, its output between the last two steps
void fx_sample(Core &c, s32 inl, s32 inr, s32 &outl, s32 &outr)
{
    if (!c.fx_odd) {
        c.fx_in[0] = inl;
        c.fx_in[1] = inr;
        outl = (c.fx_prev[0] + c.fx_out[0]) / 2;
        outr = (c.fx_prev[1] + c.fx_out[1]) / 2;
    } else {
        c.fx_prev[0] = c.fx_out[0];
        c.fx_prev[1] = c.fx_out[1];
        fx_tick(c, (c.fx_in[0] + inl) / 2, (c.fx_in[1] + inr) / 2, c.fx_out[0], c.fx_out[1]);
        outl = c.fx_prev[0];
        outr = c.fx_prev[1];
    }
    c.fx_odd = !c.fx_odd;
}

void core_init(Core &c, int index)
{
    c.mix[SPU2_VMIXL] = c.mix[SPU2_VMIXR] = 0xFFFFFF;
    for (Voice &v : c.v) v.phase = PH_OFF;
    // till the driver sets them: all of MMIX's gates open, the external input at full, no effect
    c.param[SPU2_MMIX] = 0xFFF;
    c.param[SPU2_AVOLL] = c.param[SPU2_AVOLR] = 0x7FFF;
    c.param[SPU2_BVOLL] = c.param[SPU2_BVOLR] = 0x7FFF;
    c.mvol_cur[0] = c.mvol_cur[1] = 0x7FFF;
    c.mvol[0] = c.mvol[1] = 0x3FFF;
    c.eea = index ? 0x1FFFFF : 0x1DFFFF;
    c.esa = c.eea + 1 - 0x10;
    memcpy(c.rev, kFxParams, sizeof c.rev);
    c.noise = 1;
}

void clear_effect(Core &c)
{
    memset(c.fx, 0, sizeof c.fx);
    c.fx_pos = 0;
    c.fx_out[0] = c.fx_out[1] = c.fx_prev[0] = c.fx_prev[1] = 0;
}

// MMIX (PCSX2's gates): voices ("sound") 0x800 dry L, 0x400 dry R, 0x200 wet L, 0x100 wet R; the core's input 0x80 .. 0x10;
// the external input (core 0's output, for core 1) 0x8 dry L, 0x4 dry R, 0x2 wet L, 0x1 wet R
bool gate(const Core &c, u16 bit) { return (c.param[SPU2_MMIX] & bit) != 0; }

void log_once(const char *what, int core, u32 value)
{
    static int n;
    if (n++ < 40) fprintf(stderr, "spu2: core %d %s %04x\n", core, what, value);
}

} // namespace

void spu2_reset()
{
    std::lock_guard<std::mutex> lock(g_lock);
    memset(g_core, 0, sizeof g_core);
    for (int i = 0; i < 2; i++) core_init(g_core[i], i);
}

void spu2_set_voice(int core, int n, int reg, u16 value)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Voice &v = g_core[core & 1].v[n % 24];
    v.reg[reg & 7] = value;
    if (reg == SPU2_VOLL || reg == SPU2_VOLR) {
        v.vol_tick[reg] = 0;
        if (!(value & 0x8000)) v.vol[reg] = (s16)(value << 1);
    }
}

u16 spu2_voice(int core, int n, int reg)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Voice &v = g_core[core & 1].v[n % 24];
    if (reg == SPU2_VOLXL || reg == SPU2_VOLXR) return (u16)v.vol[reg - SPU2_VOLXL];
    return v.reg[reg & 7];
}

void spu2_set_voice_addr(int core, int n, int which, u32 addr)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Voice &v = g_core[core & 1].v[n % 24];
    addr &= SPU2_RAM_SIZE - 16;
    if (which == SPU2_SSA) v.ssa = addr;
    else if (which == SPU2_LSAX) v.lsa = addr;
    else v.block = addr;
}

u32 spu2_voice_addr(int core, int n, int which)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Voice &v = g_core[core & 1].v[n % 24];
    return which == SPU2_SSA ? v.ssa : which == SPU2_LSAX ? v.lsa : v.block;
}

void spu2_key_on(int core, u32 mask)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    for (int n = 0; n < 24; n++) {
        if (!(mask & (1u << n))) continue;
        Voice &v = c.v[n];
        v.keyed = true;
        v.block = v.ssa;
        v.lsa = v.ssa;
        v.pos = 0;
        v.counter = 0;
        v.p1 = v.p2 = 0;
        memset(v.hist, 0, sizeof v.hist);
        v.phase = PH_ATTACK;
        // The SPU2 starts the attack within a sample of the key-on; here the voices are mixed when the host asks, up to 10 ms later. The
        // sound driver takes a voice whose ENVX reads 0 for one that has ended (SOUND_UpdateKeyStatus) and closed a stream it had just
        // started: two thirds of the race's speech stopped after 0.1 to 0.3 s. So the envelope leaves 0 at once, as the chip's does.
        v.env = 1;
        v.reg[SPU2_ENVX] = 1;
        v.env_tick = 0;
        c.endx &= ~(1u << n);
        decode(v);
    }
}

void spu2_key_off(int core, u32 mask)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    for (int n = 0; n < 24; n++)
        if ((mask & (1u << n)) && c.v[n].phase != PH_OFF) {
            c.v[n].phase = PH_RELEASE;
            c.v[n].env_tick = 0;
        }
}

u32 spu2_endx(int core)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return g_core[core & 1].endx;
}

void spu2_clear_endx(int core)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_core[core & 1].endx = 0;
}

void spu2_set_mix(int core, int which, u32 mask)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_core[core & 1].mix[std::clamp(which, 0, 5)] = mask & 0xFFFFFF;
}

u32 spu2_mix(int core, int which)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return g_core[core & 1].mix[std::clamp(which, 0, 5)];
}

void spu2_set_master(int core, int which, u16 value)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    c.mvol[which & 1] = value;
    c.mvol_tick[which & 1] = 0;
    if (!(value & 0x8000)) c.mvol_cur[which & 1] = volume(value);
}

u16 spu2_master(int core, int which)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return (u16)g_core[core & 1].mvol_cur[which & 1];
}

void spu2_set_core_param(int core, int which, u16 value)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    if (which == SPU2_MMIX) {
        if (c.param[SPU2_MMIX] != value) log_once("MMIX", core & 1, value);
        static const bool ignore = getenv("HP2_NOMMIX") != nullptr;      // the old mixing, all gates open, for comparing
        if (ignore) return;
    }
    c.param[std::clamp(which, 0, 6)] = value;
}

u16 spu2_core_param(int core, int which)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return g_core[core & 1].param[std::clamp(which, 0, 6)];
}

void spu2_set_effect_enable(int core, bool on)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    if (c.fx_on != on) log_once(on ? "effect on" : "effect off", core & 1, 0);
    c.fx_on = on;
}

void spu2_set_noise_clock(int core, u32 clock)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_core[core & 1].noise_clock = clock & 0x3F;
}

void spu2_set_effect_area(int core, u32 esa, u32 eea)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    c.esa = esa & (SPU2_RAM_SIZE - 2);
    c.eea = eea & (SPU2_RAM_SIZE - 1);
    if (c.eea < c.esa) c.eea = c.esa + 1;
    c.fx_pos %= fx_len(c);
}

u32 spu2_effect_end(int core)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return g_core[core & 1].eea;
}

void spu2_set_effect(int core, int mode, s16 depth_l, s16 depth_r, int delay, int feedback, bool clear)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Core &c = g_core[core & 1];
    if (mode < 0 || mode > 9) return;
    u16 data[32];
    memcpy(data, &kFxParams[mode * 16], 64);
    if (mode == 7 || mode == 8) {                       // echo, delay: libsd's own sums of the delay and the feedback
        const u16 d = (u16)((delay + 1) << 5), fb = (u16)(((feedback << 7) + feedback) << 1);
        data[7] = fb;
        data[10] = (u16)((d << 1) - data[0]);
        data[11] = (u16)(d - data[1]);
        data[16] = (u16)(data[17] + d);
        data[12] = (u16)(data[13] + d);
        data[27] = (u16)(data[29] + d);
        data[26] = (u16)(data[28] + d);
    }
    memcpy(c.rev, data, sizeof c.rev);
    c.param[SPU2_EVOLL] = (u16)depth_l;
    c.param[SPU2_EVOLR] = (u16)depth_r;
    const u32 size = (u32)kFxSize[mode] << 4;
    c.esa = c.eea - (size - 2);
    c.fx_pos = 0;
    if (clear) clear_effect(c);
    log_once("effect mode", core & 1, (u32)mode);
}

void spu2_clear_effect(int core)
{
    std::lock_guard<std::mutex> lock(g_lock);
    clear_effect(g_core[core & 1]);
}

void spu2_write(u32 addr, const u8 *src, u32 size)
{
    std::lock_guard<std::mutex> lock(g_lock);
    for (u32 k = 0; k < size; k++) g_sram[(addr + k) & (SPU2_RAM_SIZE - 1)] = src[k];
}

void spu2_read(u32 addr, u8 *dst, u32 size)
{
    std::lock_guard<std::mutex> lock(g_lock);
    for (u32 k = 0; k < size; k++) dst[k] = g_sram[(addr + k) & (SPU2_RAM_SIZE - 1)];
}

void spu2_render(s16 *out, int frames)
{
    std::lock_guard<std::mutex> lock(g_lock);
    for (int f = 0; f < frames; f++) {
        s32 ext[2] = {0, 0};                            // core 0's output, core 1's external input
        s32 final[2] = {0, 0};
        for (int ci = 0; ci < 2; ci++) {
            Core &c = g_core[ci];
            noise_step(c);
            s32 dry[2] = {}, wet[2] = {};
            for (int n = 0; n < 24; n++) {
                Voice &v = c.v[n];
                if (!v.keyed) continue;
                s32 s = voice_sample(c, n);
                voice_volume(v, 0);
                voice_volume(v, 1);
                const s32 l = s * v.vol[0] >> 15, r = s * v.vol[1] >> 15;
                if (c.mix[SPU2_VMIXL] >> n & 1) dry[0] += l;
                if (c.mix[SPU2_VMIXR] >> n & 1) dry[1] += r;
                if (c.mix[SPU2_VMIXEL] >> n & 1) wet[0] += l;
                if (c.mix[SPU2_VMIXER] >> n & 1) wet[1] += r;
            }
            dry[0] = std::clamp(dry[0], -32768, 32767), dry[1] = std::clamp(dry[1], -32768, 32767);
            wet[0] = std::clamp(wet[0], -32768, 32767), wet[1] = std::clamp(wet[1], -32768, 32767);
            // the voices and the external input through MMIX's gates
            s32 td[2] = {gate(c, 0x800) ? dry[0] : 0, gate(c, 0x400) ? dry[1] : 0};
            s32 tw[2] = {gate(c, 0x200) ? wet[0] : 0, gate(c, 0x100) ? wet[1] : 0};
            if (ci == 1) {
                const s32 el = mulv(ext[0], (s16)c.param[SPU2_BVOLL]), er = mulv(ext[1], (s16)c.param[SPU2_BVOLR]);
                if (gate(c, 0x8)) td[0] += el;
                if (gate(c, 0x4)) td[1] += er;
                if (gate(c, 0x2)) tw[0] += el;
                if (gate(c, 0x1)) tw[1] += er;
            }
            // the effect: off, it neither writes its area nor sounds
            if (c.fx_on) {
                s32 rl, rr;
                fx_sample(c, std::clamp(tw[0], -32768, 32767), std::clamp(tw[1], -32768, 32767), rl, rr);
                td[0] += mulv(rl, (s16)c.param[SPU2_EVOLL]);
                td[1] += mulv(rr, (s16)c.param[SPU2_EVOLR]);
            }
            sweep(c.mvol[0], c.mvol_cur[0], c.mvol_tick[0]);
            sweep(c.mvol[1], c.mvol_cur[1], c.mvol_tick[1]);
            const s32 ol = (s32)std::clamp<s64>((s64)td[0] * c.mvol_cur[0] >> 15, -32768, 32767);
            const s32 orr = (s32)std::clamp<s64>((s64)td[1] * c.mvol_cur[1] >> 15, -32768, 32767);
            if (ci == 0) ext[0] = ol, ext[1] = orr;
            else final[0] = ol, final[1] = orr;
        }
        out[2 * f] = (s16)final[0];
        out[2 * f + 1] = (s16)final[1];
    }
}

// ---- output -------------------------------------------------------------------------
static FILE *g_wav;
static u32 g_wav_bytes;

static void wav_header()
{
    u32 h[11] = {0x46464952, 36 + g_wav_bytes, 0x45564157, 0x20746D66, 16, 0x00020001, AUDIO_RATE,
                 AUDIO_RATE * 4, 0x00100004, 0x61746164, g_wav_bytes};
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 4, 11, g_wav);
    fseek(g_wav, 0, SEEK_END);
}

static void pull(s16 *out, int frames)
{
    spu2_render(out, frames);
    if (g_wav) {
        fwrite(out, 4, frames, g_wav);
        u32 before = g_wav_bytes;
        g_wav_bytes += frames * 4;
        if (before / (AUDIO_RATE * 4) != g_wav_bytes / (AUDIO_RATE * 4)) wav_header();
    }
    // the player's volume (Options > PC Settings) is for the speakers, not for the recording
    int vol = g_cfg.volume;
    if (vol < 100)
        for (int i = 0; i < frames * 2; i++) out[i] = (s16)(out[i] * vol / 100);
}

void spu2_start_output()
{
    static bool started;
    if (started) return;
    started = true;
    if (const char *path = getenv("HP2_WAVOUT")) {
        g_wav = fopen(path, "wb");
        if (g_wav) wav_header();
    }
    if (!getenv("HP2_NOAUDIO") && audio_out_start(pull)) return;
    // no device: keep the SPU2 running in real time anyway
    std::thread([] {
        static s16 buf[480 * 2];
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
            pull(buf, 480);
        }
    }).detach();
}
