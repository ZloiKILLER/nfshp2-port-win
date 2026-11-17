# Need for Speed: Hot Pursuit 2 — Windows port

A static recompilation of the PlayStation 2 game for Windows: the game's own code, translated to C++, on a native Vulkan
renderer, with PC controls, menus, profiles and settings. Ported by ZloiKILLER.

**The game is not included.** You need your own copy, a disc image (`.iso`) or the disc itself in a DVD drive, of one of these releases:

- USA, SLUS-20362
- Europe v1.00 and v2.00, SLES-50731
- Asia, SLPM-65181
- Korea, SLPM-67527

## Playing

1. Download `speed.exe` from [Releases](https://github.com/ZloiKILLER/nfshp2-port-win/releases).
2. Start it and choose **Install**. It asks for your disc image (Browse) or takes the disc from a DVD drive (From drive), and for the folder
   to install into (any folder you like), checks the disc, and puts everything the game needs there (about 3 GB; the disc is only read).
   Then the game starts.
3. From then on start `speed.exe` in that folder (or the shortcut on the desktop).

To update, download the new `speed.exe`, start it and choose **Update**: it asks for the folder the game is installed in and replaces the
program and the port's own files there. Settings, saves and replays stay as they are, and no disc is needed.

`README.txt` in the game's folder has the settings and what is in the folder. The controls are in the game: Options > Controls.

**Saves from a PS2 or an emulator.** Install and Update also put `savestransfer.exe`, the Saves Manager, in the game's folder. It imports
Hot Pursuit 2 saves from PS2 memory cards (PCSX2's are found by themselves; `.ps2`/`.mcd` images of PCSX2, AetherSX2 and NetherSX2, PCSX2
folder cards) and from `.psu`, `.max` and `.cbs` save files as new profiles of the port, and exports a profile's save back onto a card or
into such a file, with the game's icon, for any of the discs above. Profiles that are there already are never changed; a card is backed
up (`.bak`) before it is written to.

Needs: Windows 10 or 11, 64-bit, a graphics card with Vulkan.

How to build it, how the repository is laid out and whose work it uses: [TECHNICAL.md](TECHNICAL.md).

Need for Speed and Hot Pursuit are trademarks of Electronic Arts. This project is not affiliated with Electronic Arts.
