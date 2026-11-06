// Software SPU2: 2 MB sound RAM, two cores of 24 ADPCM voices, mixed to 48 kHz stereo.
//
// Addresses are in bytes. Registers keep their hardware encoding (volumes, pitch
// 0x1000 = 48 kHz, ADSR1/ADSR2). Not emulated yet: reverb, noise, pitch modulation,
// volume sweeps, IRQ address.
#pragma once
#include "ps2.h"

static const u32 SPU2_RAM_SIZE = 0x200000;

enum { SPU2_VOLL, SPU2_VOLR, SPU2_PITCH, SPU2_ADSR1, SPU2_ADSR2, SPU2_ENVX, SPU2_VOLXL, SPU2_VOLXR };
enum { SPU2_SSA, SPU2_LSAX, SPU2_NAX };
enum { SPU2_MVOLL, SPU2_MVOLR };
enum { SPU2_VMIXL, SPU2_VMIXEL, SPU2_VMIXR, SPU2_VMIXER };

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
void spu2_write(u32 addr, const u8 *src, u32 size);
void spu2_read(u32 addr, u8 *dst, u32 size);

// audio thread: next `frames` stereo samples
void spu2_render(s16 *out, int frames);
// starts the host output pulling spu2_render (HP2_NOAUDIO: none, HP2_WAVOUT=<file>: record)
void spu2_start_output();
