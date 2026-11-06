# Need for Speed: Hot Pursuit 2 — Windows port

A static recompilation of the PlayStation 2 game (USA, SLUS-20362) for Windows: the game's own code, translated to C++, on a native Vulkan
renderer, with PC controls, menus, profiles and settings. Ported by ZloiKILLER.

**The game is not included.** You need your own copy: the disc image (`.iso`) of the USA release, SLUS-20362.

## Playing

1. Download `speed.exe` from [Releases](../../releases).
2. Start it. The first time it asks for your disc image and where to install, checks the image, and makes the folder `nfshp2port` there with
   everything the game needs (about 3.6 GB; the disc image is only read). Then the game starts.
3. From then on start `nfshp2port\speed.exe` (or the shortcut on the desktop).

`README.txt` in `nfshp2port` has the controls and the settings.

Needs: Windows 10 or 11, 64-bit, a graphics card with Vulkan.

## Building

What a build needs: this repository, your disc image, and

| Tool | Version used |
|---|---|
| LLVM (clang++, lld) | 23.1.4 |
| CMake | 4.4.3 |
| Ninja | 1.13.2 |
| Vulkan SDK (headers, glslc) | 1.4.363.0 |
| Python | 3.14 |

1. The game's program from your disc: start a built or released `speed.exe` once and install, or copy `SLUS_203.62` out of the disc image.
2. Generate the C++ from it:

   ```
   python tools/recomp/gen.py --elf <path>\SLUS_203.62
   ```

   It writes `work/recomp/gen/`.
3. Build:

   ```
   cmake -S recomp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe"
   cmake --build build --target hp2recomp
   ```

   `speed.exe` appears in `recompwin/`. SDL3 is built from `third_party/SDL3-3.4.18.tar.gz`.

The same tools give the same `speed.exe` as in Releases, and with the same disc image the first start gives the same folder.

## Layout

| Path | What |
|---|---|
| `recomp/runtime/` | the runtime: the PS2's hardware where the game needs it, the renderer, sound, input, menus, the first start's setup |
| `recomp/data/` | what the exe carries into the game's folder: the port's texts in six languages, the cursor, the README, the names of the disc's files |
| `recomp/symbols/` | the recompiler's table of the game's functions |
| `recomp/docs/` | notes on the data files, the menus and the languages |
| `tools/recomp/` | the recompiler (`gen.py`) and the language files' source (`make_lang.py`) |
| `third_party/` | SDL3 (zlib license), for gamepads |

Need for Speed and Hot Pursuit are trademarks of Electronic Arts. This project is not affiliated with Electronic Arts.
