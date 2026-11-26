# savestransfer.exe — PS2 saves into the port's profiles

PS2 Memcard Saves manager, version 0.1 (its own, not the game's version; the resource `savestransfer.rc`). The icon: a PS2 memory card
(a black 8 MB one, ridged at the bottom) in `savestransfer.ico`, drawn by `make_icon.py` (Pillow, the Arial font) in every size from 16 to
256, the small sizes simplified. The lettering is set in an ordinary font; the Sony and PlayStation logos are not copied.

A separate, optional console program. It goes beside `speed.exe`; the game does not need it.
It finds Hot Pursuit 2 saves, shows them and makes each save chosen a **new** profile:
`saves/<name>/save.dat` in the game's folder. These are the same bytes the game writes to the card (`/BASLUS-20362HP2/BASLUS-20362HP2`,
see `work/memcard/README.md`).

## What it reads

| Source | How it is recognised |
|---|---|
| A card image `.ps2` / `.mcd` (PCSX2, AetherSX2, NetherSX2...), with ECC or without | the superblock `Sony PS2 Memory Card Format` |
| A PCSX2 folder card, and the save's own folder (`BASLUS-20362HP2\...`) | the folder's name |
| EMS / uLaunchELF `.psu` | the first three entries: the directory, `.`, `..` |
| Max Drive `.max` (LZARI) | `Ps2PowerSave` |
| CodeBreaker `.cbs` (RC4 + zlib) | `CFU\0` |

The formats are taken as mymc+ reads them (github.com/thestr4ng3r/mymcplus). LZARI is Okumura's LZARI.C, inflate follows puff.c, all of it
inside one `.cpp`.

The discs supported are the ones the port installs from, and the window lists them in a line under its title:
- USA (SLUS-20362), the directory `BASLUS-20362HP2`;
- Europe v1.00 and v2.00 (SLES-50731), the directory `BESLES-50731HP2` (USA and Europe have one program);
- Asia (SLPM-65181), the directory `BISLPM-65181HP2`;
- Korea (SLPM-67527), the directory `BKSLPM-67527HP2`.

Asia and Korea have another link of the same program (handoff.md, 2026-11-11), and card codes of their own: `BI` for Asia and `BK` for
Korea. This shows in the constructor of `cDBFileSys` (`0x12dee0`), which builds the directory from `.sdata` at `0x32fdb0`, the disc's
number and `HP2`. Until 2026-11-14 the export for Korea wrote to `BISLPM-67527HP2`, and the Korean game did not see such a save. Asia and
Korea saves are accepted by the same checks. A save must be 70880 bytes long (0x114e0, the FrontendDatabase image), and its first word
must equal that length. The prototype's save (`...NFS6`, 70876 bytes) and saves of other lengths are shown with the reason and are not
imported.

With no arguments the program looks in `Documents\PCSX2\memcards`, in the cards' folder from `inis\PCSX2.ini` / `PCSX2_ui.ini`
(`[Folders] MemoryCards`) and in its own folder. Files can be dropped on the exe or on the program's window.

## What it never does

- It does not write into an existing profile. The name is checked without regard to case, the folder itself is made by
  `create_directory`, which does not touch a folder that exists already, and `save.dat` is opened with `CREATE_NEW`. If writing fails,
  only the file and the folder made in that same run are removed.
- It touches nothing else in the game's folder and does not change the files it reads from.
- The name `Player1` counts as taken if an old `saves/save.dat` is there: the port makes Player1 of it on its next start.

The rules for the name are those of New Profile in the game (`pcsettings.cpp`): up to 15 printable ASCII characters, spaces and dots at
the end are dropped. On top of that, everything a Windows folder cannot be called is refused (`<>:"/\|?*`, CON, NUL, COM1...).
The save's time on the card (JST) becomes the modification time of `save.dat`.

## Running it

A double click opens the "Saves Manager" window:
- a header of two lines, "NFS Hot Pursuit 2" and "PS2 Memory card game saves manager"; on the right the buttons Help (a description,
  the discs and formats supported, a step-by-step guide) and About (the name, the version, the author ZloiKILLER, 2026) with the system's
  icons;
- below it the game's folder (the Browse... button);
- on the left a table of the saves found: the region, the save's time, its state and its source. Those that do not fit are grey; the
  reason and the full path show in the tooltip and in the status line;
- on the right the game's profiles;
- at the bottom the new profile's name and the Import button (Enter and a double click on a row press it too).

Cards and files are added with the buttons Add cards or files... and Add a folder..., and by dropping them on the window or the exe.
Refresh looks at everything again.

From the command line no window opens; the output goes to the console or to a pipe (for scripts and the test):

```
savestransfer [card or save files, folders...] --list [--game <game folder>]
savestransfer [files...] --import <n> --name <profile> [--game <game folder>]
```

The game's folder by default: the program's own folder, then `HKCU\Software\NFSHP2Port\InstallDir` (setup.cpp); if none is found, the
window asks for it.

For checks:
- `SAVESTRANSFER_SHOT=<file.bmp>` draws the window into a picture and closes it;
- `SAVESTRANSFER_SHOT_OF=help|about` takes Help or About in place of the main window;
- `SAVESTRANSFER_SHOT_SIZE=<w>x<h>` resizes the window before the shot (so it shows that nothing is left behind after it narrows);
- `SAVESTRANSFER_NAME` fills in the name beforehand.

## Export back to a card

In the window, choose a profile in the list on the right and press Export to a card or file... (or double-click the profile). Then
choose the disc the card is for, and where to write:
- into a card image `.ps2` / `.mcd`;
- into a PCSX2 folder card or any folder;
- into a file `.psu`, `.max` or `.cbs`.

The card gets what the game itself writes: the disc's directory (`BASLUS-20362HP2`, `BESLES-50731HP2`, `BISLPM-65181HP2`,
`BKSLPM-67527HP2`), and in it the save (the profile's `save.dat`), `icon.sys` and `nfs6.ico`.
- `icon.sys` is built by code with the same values the game sets (the title "ＮＦＳ：ＨＰ２", the background, three light sources).
  It matches, byte for byte, what the game wrote to a PCSX2 card.
- The icon `nfs6.ico` is taken from `data/NFS6.ICO` of the game's folder (the same file the game writes to the card), so the exe holds
  none of the game's data.
- The profile's name stays in the port only.

Writing into a card image:
- clusters are allocated in memory by the FAT;
- the save's directory takes the root's first deleted entry or goes at the root's end; if need be the root grows by a cluster;
- the entries `.` and `..` are filled in as the game does (`.` has the root's cluster and the entry's number in the root, `..` the root's
  time), modes 0x8427 and 0x8497;
- the ECC is computed again for each page written, 3 bytes for every 128 bytes, as in mymc+. The algorithm was checked against 655 pages
  of a real PCSX2 card;
- the new image is read back, and only if the save in it is whole is a copy of the old card put beside it as `.bak` (`.bak2`...), and
  the new image takes its place.

Refusals, with nothing written:
- there is not enough room;
- the file chosen is not a card;
- the profile has no save yet.

**Overwriting.** If the card already has a save in this disc's directory, the window asks whether to write over it and shows the old
save's time. From the command line it is overwritten only with `--overwrite`. In a card image the old save is deleted and the new one is
written in one go: one `.bak` copy, no clusters lost. In a folder card the old folder goes to the Recycle Bin.

If PCSX2 is running, the window warns and asks whether to write all the same. The files `.psu`, `.max` and `.cbs` are overwritten from the
window only after Windows' question, and never from the command line. `.max` is compressed by an LZARI coder after Okumura's LZARI.C,
`.cbs` is zlib of stored blocks under RC4; the `.cbs` header, 0x128 long, is laid out as mymc+ reads it.

## Deleting a save from a card

Choose the save in the table, press Delete from card... and confirm. The question shows which save it is, its time, the card and the exact
name of the copy.
- **A card image:** the deletion goes as with the PS2's card driver (mcman). The clusters of the files and the directory are freed in the
  FAT, and the entry in the root stays with its "exists" bit off; the next save takes it. The root is not shortened. Then the ECC, the
  read back and the `.bak` copy, as for an export.
- **A folder card or a save's folder:** the folder goes to the Recycle Bin, and can be brought back from there.
- **Saves from `.psu`, `.max`, `.cbs` files** are not deleted here: such a file is the save.

Saves that do not fit can be deleted too, the prototype's `NFS6`, for example.

```
savestransfer --export <profile> --to <card image | folder | file.psu/.max/.cbs> [--disc usa|europe|asia|korea] [--overwrite] [--game <game folder>]
savestransfer --delete <the save's directory, BASLUS-20362HP2 for example> --from <card image | folder card>
```

## Checks

```
python recomp/savestransfer/test_savestransfer.py recompwin/saves/Player1/save.dat [--card <a real .ps2>]
python recomp/savestransfer/test_export.py recompwin/saves/Player1/save.dat recompwin/data/NFS6.ICO [--card <a real .ps2>] [--icon-sys <the game's icon.sys>]
```

`test_export.py` checks every export with readers of its own, not with the program's code:
- a card: the file system taken apart, the ECC of every changed page and the FAT's integrity (every cluster in use in exactly one chain,
  nothing lost);
- `.max`: its own LZARI decoder and the CRC;
- `.cbs`: zlib.

Then every export is imported back and must give the same save. The test checks the refusals as well: a second export, a full card, a
profile with no save, a file that is not a card. A real card given with `--card` is copied first, and the writing goes into the copy.

`make_test_saves.py` builds every format from a real `save.dat` on its own: a card with ECC and without, cluster chains in reverse order,
a folder card, `.psu`, `.max` with its own LZARI coder, `.cbs` with the RC4 permutation from mymc+ (`cbs_rc4.txt`; the test checks it
against the table in the `.cpp`), and saves that must be refused. The test imports each format, compares the bytes and the time, and
checks that taken and bad names change nothing in `saves/`.

Checked as of 2026-11-14: all the formats and refusals, the real PCSX2 cards of this machine (the prototype's save refused, an empty card
called empty), the window (import by the button; the same name in another case refused, the save untouched), 360 damaged files with no
crash or hang. **Not checked:** real `.max` and `.cbs` files made by Max Drive / CodeBreaker (there is none at hand; the test's coders are
written from the same descriptions). Import from a PCSX2 card has been checked in the game.

Export was checked as of 2026-11-14 like this:
- `test_export.py` passed in full, on a copy of a real PCSX2 card too: it has a USA save, so USA was refused and Europe written beside it;
  the ECC and the FAT are whole, the saves that were there are in place;
- `icon.sys` matches the game's byte for byte.

**Not checked:** loading the exported card in PCSX2 and in the game itself on a PS2, and reading `.max` and `.cbs` with a real Max Drive
and CodeBreaker.
