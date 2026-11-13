#pragma once
#include <string>

// The setup (setup.cpp). The game's folder is one with data\index.txt and data\SLUS_203.62 in it. setup_start: SETUP_PLAY when this exe's
// folder is one (or HP2_DATA names one), and the game goes on here; else the setup window: Install (from a disc image or from the disc in a
// drive, into a new folder) or Update (this exe and the port's own files into a folder where the game is installed; that is what the window
// opens on when it finds one: the folder remembered in the registry, or the game's folder beside this exe). Then the game is started from its
// folder: SETUP_EXIT, this process ends.
// For the tests: HP2_INSTALL=<iso or drive, E:\>|<folder> the install with no window, HP2_UPDATE=<folder> the update with no window,
// HP2_EXPORTS=<data folder> the PNGs and the texts again from a data\; HP2_SETUP_MODE=install|update, HP2_SETUP_ISO, HP2_SETUP_DIR,
// HP2_SETUP_UPDIR what the window opens with, HP2_SETUP_AUTO its button pressed, HP2_SETUP_SHOT=<png> the window's picture and no more.
enum SetupResult { SETUP_PLAY, SETUP_EXIT };
SetupResult setup_start();
