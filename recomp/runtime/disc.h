// The game's data as files in the game folder (disc.cpp). The game itself addresses it by disc
// sector, so the files are laid out on a virtual disc.
#pragma once
#include <string>

#include "ps2.h"

struct DiscFile {
    u32 lsn, size;
};

bool disc_open();                       // find the data: data\ (or NFSHP2\) next to the exe, or in HP2_DATA=<folder>
bool disc_find(const std::string &path, DiscFile &out);     // "\NFSHP2\ZZDATA.BIN;1", "cdrom0:\NFSHP2\ZDIR.BIN;1"
bool disc_read(u32 lsn, u32 sectors, void *dst);            // whole sectors, as a drive reads them
u32 disc_read_file(const DiscFile &f, u32 pos, u32 bytes, void *dst);   // returns the bytes read
