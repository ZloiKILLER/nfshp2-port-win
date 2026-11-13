// The game's memory: guest addresses 0 to 0x1200FFFF, the first piece of its 4 GB (ps2.h says why it is an array of the image, and
// mem_reserve in ps2.cpp what becomes of it). Alone in this file, which never touches it: here its size is known to the compiler,
// and an address past it (the scratchpad's, 0x70000000 on) must not be worked out where that is known.
#ifdef HP2_MEM4G       // (recomp/CMakeLists.txt; without it the memory is ps2.cpp's three arrays)
alignas(4096) unsigned char g_ram[0x12010000];
#endif
