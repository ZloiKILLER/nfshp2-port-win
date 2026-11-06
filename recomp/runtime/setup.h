#pragma once
#include <string>

// The first start (setup.cpp). The game's folder is one with data\index.txt and data\SLUS_203.62 in it. setup_start: SETUP_PLAY when this exe's
// folder is one (or HP2_DATA names one), and the game goes on here; else the game is started from nfshp2port beside it, or the setup window asks
// for the disc image and where, makes the folder, puts this exe in it and starts that: SETUP_EXIT, this process ends.
// HP2_INSTALL=<iso>|<folder>: the install with no window (the tests), HP2_EXPORTS=<data folder>: the PNGs and the texts again from a data\.
enum SetupResult { SETUP_PLAY, SETUP_EXIT };
SetupResult setup_start();
