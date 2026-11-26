# Technical notes

For playing the game see [README.md](README.md). This file is for building the port and finding your way around the repository.

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

   It writes `work/recomp/gen/`. The sound driver, the one program of the PS2's second processor that the port runs, is
   translated the same way, into the same folder:

   ```
   python tools/recomp/gen_iop.py --irx <game folder>\data\IRX253\STREAM.IRX
   ```
3. Build:

   ```
   cmake -S recomp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" -DHP2_PGO=USE
   cmake --build build --target hp2recomp
   ```

   `speed.exe` appears in `recompwin/`. SDL3 is built from `third_party/SDL3-3.4.18.tar.gz`. The Saves Manager `savestransfer.exe`
   (target `savestransfer`) is built first, beside it: `speed.exe` carries it and its setup puts it in the game's folder.

   `-DHP2_PGO=USE` builds by `recomp/pgo/speed.profdata`: counts of what the game really runs (five demos, the menus, a race), by which
   the compiler lays the code out. It changes no result, only the speed, about a seventh less processor time; the release is built
   with it. The file was written by LLVM 23's `llvm-profdata`: with an older clang leave the option out.

The same tools give the same `speed.exe` as in Releases, and with the same disc image the first start gives the same folder.

## Layout

| Path | What |
|---|---|
| `recomp/runtime/` | the runtime: the PS2's hardware where the game needs it, the renderer, sound, input, menus, the first start's setup |
| `recomp/data/` | what the exe carries into the game's folder: the port's texts in six languages and for the Russian disc, the cursor, the README, the names of the disc's files; `recomp/data/patch/` has the patches the port applies |
| `recomp/savestransfer/` | the Saves Manager: PS2 memory cards (images, PCSX2 folder cards) and `.psu`/`.max`/`.cbs` save files to and from the port's profiles, its icon's maker and its tests (`README.md` there) |
| `recomp/symbols/` | the recompiler's table of the game's functions |
| `recomp/pgo/` | the counts the release is built by (`-DHP2_PGO=USE`) |
| `recomp/docs/` | notes on the data files, the menus, the languages and the cheat codes |
| `tools/recomp/` | the recompilers (`gen.py` for the game, `gen_iop.py` for its sound driver), the language files' source (`make_lang.py`, the Russian disc's strings in `lang_ru.py`) and the makers of the patches' tables (`make_disc_patch.py`, `make_layouts.py`, `make_traffic_patch.py`) |
| `third_party/` | SDL3's source archive |

## Credits

- **Gamepad layouts with the triggers** — "Trigger control mappings", a PCSX2 patch by **Silent**. It gives the gamepad four more
  layouts with the throttle and the brake on the triggers (E to H in Options > Controls > Gamepad > Layouts). The patch is
  `recomp/data/patch/SLUS-20362_1D2818AF_triggers.pnach`, as its author made it; `recomp/runtime/layouts_patch.h` is the same
  as a table, and the port applies it when the game starts.
- **Reset during a chase** — pull request 10 by **spartaque12**. On the PS2, resetting the car (Select, **R** on the keyboard)
  at any point in a pursuit is an arrest, even with every cop out of sight; with this change it is an arrest only when a cop is
  close to the car, and otherwise the chase goes on. The hook on `RealDriver::JoyHandler_ResetCar` is in
  `recomp/runtime/native.cpp`; `HP2_PS2RESET` brings back the PS2's rule.
- **Speed Boost for All Cars** and **Disable Catchup** (Options > Extras) — two PCSX2 patches by **Silent**: the boost a police
  car has in You're the Cop for every car and mode, and the game's Catch-up off in Quick Race and the career. The port does what
  the patches do in hooks of the same functions (`recomp/runtime/pcsettings.cpp`), switched from the menu.
- **Playable Traffic Cars** (Options > Extras) — the mod "Playable traffic cars" v1.4.1 by **379Felipe**, with thanks there to
  **Xanvier** and **nlgzrgn**. Its changes are in `recomp/runtime/traffic_patch.h` (made from the mod's patch by
  `tools/recomp/make_traffic_patch.py`), laid over the game while the switch is on.
- **SMAA** by Jorge Jimenez and others (MIT license, `LICENSE.txt` in `recomp/runtime/shaders/smaa/`): the antialiasing
  shader and its tables (`recomp/runtime/shaders/smaa/`, `recomp/runtime/smaa/`).
- **SDL3** (zlib license), for gamepads: `third_party/`.
