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

   `speed.exe` appears in `recompwin/`. SDL3 is built from `third_party/SDL3-3.4.18.tar.gz`.

   `-DHP2_PGO=USE` builds by `recomp/pgo/speed.profdata`: counts of what the game really runs (five demos, the menus, a race), by which
   the compiler lays the code out. It changes no result, only the speed, about a seventh less processor time; the release is built
   with it. The file was written by LLVM 23's `llvm-profdata`: with an older clang leave the option out.

The same tools give the same `speed.exe` as in Releases, and with the same disc image the first start gives the same folder.

## Layout

| Path | What |
|---|---|
| `recomp/runtime/` | the runtime: the PS2's hardware where the game needs it, the renderer, sound, input, menus, the first start's setup |
| `recomp/data/` | what the exe carries into the game's folder: the port's texts in six languages, the cursor, the README, the names of the disc's files; `recomp/data/patch/` has the patches the port applies |
| `recomp/symbols/` | the recompiler's table of the game's functions |
| `recomp/pgo/` | the counts the release is built by (`-DHP2_PGO=USE`) |
| `recomp/docs/` | notes on the data files, the menus and the languages |
| `tools/recomp/` | the recompilers (`gen.py` for the game, `gen_iop.py` for its sound driver), the language files' source (`make_lang.py`) and the makers of the patches' tables (`make_disc_patch.py`, `make_layouts.py`) |
| `third_party/` | SDL3's source archive |

## Credits

- **Gamepad layouts with the triggers** — "Trigger control mappings", a PCSX2 patch by **Silent**. It gives the gamepad four more
  layouts with the throttle and the brake on the triggers (E to H in Options > Controls > Gamepad > Layouts). The patch is
  `recomp/data/patch/SLUS-20362_1D2818AF_triggers.pnach`, as its author made it; `recomp/runtime/layouts_patch.h` is the same
  as a table, and the port applies it when the game starts.
- **SMAA** by Jorge Jimenez and others (MIT license, `LICENSE.txt` in `recomp/runtime/shaders/smaa/`): the antialiasing
  shader and its tables (`recomp/runtime/shaders/smaa/`, `recomp/runtime/smaa/`).
- **SDL3** (zlib license), for gamepads: `third_party/`.
