// Software SPU2 (see spu2.h). ADPCM decoding, envelope and voice behaviour follow
// the PlayStation SPU, which the SPU2 voices keep: 16-byte blocks of 28 samples,
// flags end/repeat/loop start in byte 1, the ADSR rate formula from psx-spx.
// Interpolation is 4-point cubic, not the hardware's Gaussian table.
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
};

struct Core {
    Voice v[24];
    u32 endx;
    u32 mix[4];
    u16 mvol[2];
};

u8 g_sram[SPU2_RAM_SIZE];
Core g_core[2];
std::mutex g_lock;

const int K0[5] = {0, 60, 115, 98, 122};
const int K1[5] = {0, 0, -52, -55, -60};

// a fixed volume register: 15-bit signed value
s32 volume(u16 v)
{
    if (v & 0x8000) return 0x7FFF;  // sweep mode: only voices sweep, masters hold full volume
    return (s16)(v << 1);
}

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

// voice volume: fixed, or swept (bit 15; 14 exponential, 13 decrease, 12 negative
// phase, 6-0 rate) from the current level
void voice_volume(Voice &v, int lr)
{
    u16 r = v.reg[SPU2_VOLL + lr];
    if (!(r & 0x8000)) {
        v.vol[lr] = (s16)(r << 1);
        return;
    }
    s32 mag = std::abs(v.vol[lr]);
    mag = rate_step(mag, r & 0x7F, (r >> 13) & 1, (r >> 14) & 1, v.vol_tick[lr]);
    v.vol[lr] = (r & 0x1000) ? -mag : mag;
}

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

s32 cubic(const s16 *h, u32 frac)
{
    float t = frac / 4096.0f, p0 = h[0], p1 = h[1], p2 = h[2], p3 = h[3];
    float r = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    return std::clamp((s32)r, -32768, 32767);
}

// one 48 kHz sample of voice n, envelope applied
s32 voice_sample(Core &c, int n)
{
    Voice &v = c.v[n];
    s32 s = cubic(v.hist, v.counter & 0xFFF);
    v.counter += std::min<u32>(v.reg[SPU2_PITCH], 0x3FFF);
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
    return s * v.env >> 15;
}

} // namespace

void spu2_reset()
{
    std::lock_guard<std::mutex> lock(g_lock);
    memset(g_core, 0, sizeof g_core);
    for (Core &c : g_core) {
        c.mix[SPU2_VMIXL] = c.mix[SPU2_VMIXR] = 0xFFFFFF;
        for (Voice &v : c.v) v.phase = PH_OFF;
    }
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
        v.env = 0;
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
    g_core[core & 1].mix[which & 3] = mask & 0xFFFFFF;
}

u32 spu2_mix(int core, int which)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return g_core[core & 1].mix[which & 3];
}

void spu2_set_master(int core, int which, u16 value)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_core[core & 1].mvol[which & 1] = value;
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
        s32 l[2] = {}, r[2] = {};
        for (int ci = 0; ci < 2; ci++) {
            Core &c = g_core[ci];
            for (int n = 0; n < 24; n++) {
                Voice &v = c.v[n];
                if (!v.keyed) continue;
                s32 s = voice_sample(c, n);
                voice_volume(v, 0);
                voice_volume(v, 1);
                if (c.mix[SPU2_VMIXL] & (1u << n)) l[ci] += s * v.vol[0] >> 15;
                if (c.mix[SPU2_VMIXR] & (1u << n)) r[ci] += s * v.vol[1] >> 15;
            }
        }
        // core 0 feeds core 1, whose output goes to the DAC
        s32 ml0 = volume(g_core[0].mvol[0]), mr0 = volume(g_core[0].mvol[1]);
        s32 ml1 = volume(g_core[1].mvol[0]), mr1 = volume(g_core[1].mvol[1]);
        s64 L = ((s64)l[0] * ml0 >> 15) + l[1], R = ((s64)r[0] * mr0 >> 15) + r[1];
        out[2 * f] = (s16)std::clamp<s64>(L * ml1 >> 15, -32768, 32767);
        out[2 * f + 1] = (s16)std::clamp<s64>(R * mr1 >> 15, -32768, 32767);
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
