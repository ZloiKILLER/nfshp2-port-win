# The game's data as files

`recompwin/data/` holds what `ZZDATA.BIN` holds, one file each. `disc.cpp` builds `ZDIR.BIN` and `ZZDATA.BIN` from it at start, so the game's code is untouched and sees the archive it knows. Without `data/index.txt` the game reads the two files in `NFSHP2/` as before.

```
python tools/recomp/unpack_data.py       data/ out of NFSHP2/; over an existing data/ it moves the files to the layout
python tools/recomp/export_textures.py   data/textures/, 30 s
python tools/recomp/export_text.py       data/text/
```

`index.txt` keeps ZDIR's order, `<hash> <path>`. The game goes by the hash, so a file's name and folder here are free: `unpack_data.py` sets them, and moving a file only takes changing its line. Sizes are read when the game starts.

| Folder | Files | Size | What |
|---|---:|---:|---|
| `SOUND/` | 70 | 1.8 GB | sound banks `.MSB`/`.MSH`, music `.JBC`, speech `.PCK`, `.EVT`, engine tables `.CTB`/`.LTB`, `.sndbank` |
| `MOVIES/` | 21 | 650 MB | `.AVI`, MV30; four names unknown in `unnamed/`, one of them MVDV, which the game rejects |
| `TRACKS/` | 120 | 306 MB | `TRACKB*.LZC` the track, `TEX*TRACK.tex`, `TEX*LOCATION.tex`, empty `TRACKA*.BUN`, marker notes `.txt` |
| `unused/` | 58 | 111 MB | twins of `.LZC` files; none read in a run through menus and a race |
| `FRONTEND/` | 3 | 26 MB | `FRONTB.LZC` the menus, `TSELECT.tex` |
| `CARS/` | 48 | 25 MB | `<car>/GEOMETRY.LZC`, `TEXTURES.tex` |
| `GLOBAL/` | 9 | 6 MB | `GLOBALB`, `INGAMEB`, `PRERACEANIM`, `HUDTEX.tex`, `DYNTEX.tex`, `DRIVTEX.tex` |
| `REPLAY/` | 36 | 4 MB | demo-mode replays `.replay`; those with no known name in `DEMO/`, by the author written in them |
| `NIS/`, `MARKERS/`, `LANGUAGES/` | | 2 MB | cutscenes `.nis` + `.CAM`, `.markers`, string tables `.lang` |

Ours, which the game does not read: `textures/`, `text/`, `replace/`. Not read by the game, as far as is known: `KEY.TXT`, a line of hex digits between two x; `NFS.TAG` holds "Hello." and the name fits its hash, as does `NG2.TAG`. Names are not read from anywhere: `index.txt` is what matters.

Extensions of ours, all the game's `.BIN`: `.tex` a texture pack, `.geo` geometry, `.lang` strings, `.replay`, `.nis`, `.markers`, `.sndbank`. `.LZC` is a COMP file of chunks, `.BUN` the same not compressed.

Some names stand twice in ZDIR with one hash (`CARS/TEXTURES`, `GLOBAL/HUDTEX`, `SOUND/SPEECH/EVENTS.EVT`) and share one file here: the check below shows the two copies are the same bytes.

## Text

`LANGUAGES/FE_<language>.lang`, chunk 0x39000: a header, then two tables of `{s32 key, u32 offset}` into one pool of text, one by LanguageID and one by the hash of the English text. Six languages, and two that are tools: `FE_ID` has each ID's own number, `FE_LONGEST` the longest translation of each. 2437 strings each. `data/text/<language>.txt` and `all.tsv` are for reading; the game goes on reading the `.lang` files.

## Textures

`data/textures/<file>/<NAME>.png` are all of them, a picture found in several files once; `data/textures/index.csv` says where each is used. The alpha is doubled, as a PNG is read by everyone else: 255 is the GS's 0x80.

A PNG in `data/replace/` (any subfolder) takes a texture's place:

- `<NAME>.png` wherever that name is used; `<NAME>@<crc>.png` for one exact texture, the crc from `index.csv`, when one name has several pictures. The name is checked first with the crc.
- Its size is the texture's times a whole number. Larger ones go through the GPU's filter, since the GS's levels would be wrong for them, and get a new chain of levels if the texture had any.
- An unedited export changes nothing, the game's own levels stay.
- `MOVIE_*` are frames of the movies and are not looked up.
- Textures the game changes as it runs (`WATER`, `WATERC`, `CREEKBED`, `BUTTONSOUT`, `TRACK_BARRIER_NEON`): by name only, their crc moves.

## Checks

- `HP2_DISCCHECK=1`: the built archive against the real two files, then exits. 2026-11-05, in this layout: ZDIR the same, ZZDATA 3116437504 bytes, 0 differ.
- `HP2_FILELOG=<file>`: each file of `data/` at its first read, with the time.
- `HP2_TEXCHECK=1` with `export_textures.py --replace-all` in `data/replace/`: each texture the game uses against its PNG. 2026-11-05, menus and a race, 200+ textures, 0 differ.

## Not known yet

- Whether a file may change size. The decompiled loading code (`src/bFileE.cpp`, `src/ResourceLoader.cpp`) has no reference by sector, it takes each file's place from ZDIR, which `disc.cpp` builds from the sizes now. That is the EE's code only: the contents of the game's files and the IRX were not searched for sector numbers, and no file of another size has been tried in the game. A texture table has to fit in a file's first 0x20000 bytes.
- A file not starting with `COMP` is taken as it is under a `.LZC` name: from the code, not tried.
- The formats of `SOUND/`, of geometry and of tracks are not exported.
- 32 files keep their hash for a name: 25 replays, 4 movies, one sound bank (2 files), one table of the speech's events.
