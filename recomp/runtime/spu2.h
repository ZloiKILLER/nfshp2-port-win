// Software SPU2: 2 MB sound RAM, two cores of 24 ADPCM voices, mixed to 48 kHz stereo.
//
// Addresses are in bytes. Registers keep their hardware encoding (volumes, pitch
// 0x1000 = 48 kHz, ADSR1/ADSR2). Each core: voices dry and wet (VMIX*), the gates of MMIX, the effect
// (the PS1/PS2 reverb on its work area, at 24 kHz), effect, input and external volumes (EVOL AVOL BVOL),
// master volume with its sweeps; core 0's output is core 1's external input. Voices: Gaussian
// interpolation, pitch modulation (PMON), noise (NON). Not emulated: the IRQ address and the cores'
// sound data input (AutoDMA PCM), which this game does not use.
#pragma once
#include "ps2.h"

static const u32 SPU2_RAM_SIZE = 0x200000;

enum { SPU2_VOLL, SPU2_VOLR, SPU2_PITCH, SPU2_ADSR1, SPU2_ADSR2, SPU2_ENVX, SPU2_VOLXL, SPU2_VOLXR };
enum { SPU2_SSA, SPU2_LSAX, SPU2_NAX };
enum { SPU2_MVOLL, SPU2_MVOLR };
enum { SPU2_VMIXL, SPU2_VMIXEL, SPU2_VMIXR, SPU2_VMIXER, SPU2_PMON, SPU2_NON };
// core parameters (libsd's SD_PARAM_*), by spu2_set_core_param
enum { SPU2_MMIX, SPU2_EVOLL, SPU2_EVOLR, SPU2_AVOLL, SPU2_AVOLR, SPU2_BVOLL, SPU2_BVOLR };

void spu2_reset();
void spu2_set_voice(int core, int v, int reg, u16 value);
u16 spu2_voice(int core, int v, int reg);
void spu2_set_voice_addr(int core, int v, int which, u32 addr);
u32 spu2_voice_addr(int core, int v, int which);
void spu2_key_on(int core, u32 mask);
void spu2_key_off(int core, u32 mask);
u32 spu2_endx(int core);
void spu2_clear_endx(int core);
void spu2_set_mix(int core, int which, u32 mask);
u32 spu2_mix(int core, int which);
void spu2_set_master(int core, int which, u16 value);
u16 spu2_master(int core, int which);                 // the current level, MVOLX
void spu2_set_core_param(int core, int which, u16 value);
u16 spu2_core_param(int core, int which);
void spu2_set_effect_enable(int core, bool on);       // SD_CORE_EFFECT_ENABLE
void spu2_set_noise_clock(int core, u32 clock);       // SD_CORE_NOISE_CLK: bits 0-1 step, 2-5 shift
void spu2_set_effect_area(int core, u32 esa, u32 eea);   // bytes, inclusive end
u32 spu2_effect_end(int core);
// sceSdSetEffectAttr: mode 0..9 (off room studio1-3 hall space echo delay pipe), depth to EVOL, delay and feedback for echo and delay
void spu2_set_effect(int core, int mode, s16 depth_l, s16 depth_r, int delay, int feedback, bool clear);
void spu2_clear_effect(int core);
void spu2_write(u32 addr, const u8 *src, u32 size);
void spu2_read(u32 addr, u8 *dst, u32 size);

// audio thread: next `frames` stereo samples
void spu2_render(s16 *out, int frames);
// starts the host output pulling spu2_render (HP2_NOAUDIO: none, HP2_WAVOUT=<file>: record)
void spu2_start_output();
