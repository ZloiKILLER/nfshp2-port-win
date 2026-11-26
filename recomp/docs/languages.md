# Languages

The game has six languages: English, French, German, Italian, Spanish and Swedish (`e_Languages` 0..5; 6 `LONGEST` and 7 `ID` are for the
game's own use). The US disc holds all of them: the strings `LANGUAGES\FE_<LANGUAGE>.BIN` (`.lang` in `data\`), the speech
`SOUND\SPEECH\<LANGUAGE>BAN.PCK` and `DAT.PCK` (there is no Swedish speech), and the French frontend textures (`fr_frontend`, the
"Poursuite infernale" logo). The NTSC program only hid the Languages item in Options (`MENUITEM_7`).

## How the language changes

`LanguageManager::SetLanguage(&TheLanguageManager, language)`: `0x21e0d0`, the object at `0x31c1c8` (`CurrLanguage` at +0). It unloads the
old string file, loads `LanguageFilenames[language]` (`0x31c168`, `char *[8]`), then calls `HandleLanguageChange`: the speech,
`GameFlowNotifyLanguageChange` (the French texture swap) and `FlailerUpdateLanguageText`. It works on the fly, on any frontend screen.

In the port: Options, Languages (`pcsettings.cpp`, the page `PC Languages`), `pc_language_choose` in `pclang.cpp`. The language is kept in
`hp2.ini` (`language=FRENCH`) and set on the frontend's first frame (`pc_language_apply`).

`LanguageDescription +72` is the `e_Languages` of the loaded file. `patch_language` (`pcplatform.cpp`) puts the port's strings for that
language into the pool on every load: the English ones from `kText`, plus the `#id` strings from the language file.

`FlailerTextChunk::SetString` looks the English string up in the language's hash table by itself: if the game knows the string ("Audio"),
the screen shows its translation ("Sonido"). Strings the game does not know stay as they were given.

## The port's strings

`data\lang\<LANGUAGE>.txt`, UTF-8, the format is in `recomp/runtime/pclang.h`. They are written by `tools/recomp/make_lang.py` from one table
for all languages. `tr(english)` in `pcmenu.cpp` (through `set_text` and `blurb_text`) gives the translation by the English text. `set_text`
remembers each object's English text, and a change of language sets them all again (`pc_retranslate`). Room for text: the labels of Options
and of the pages 32 characters, the Back hint 16. A long hint at the bottom scrolls by itself.

The "SAVED" notice in the corner is drawn with the port's own 5x7 font (`draw_notice`, `native_vk.cpp`): A-Z, accents are dropped.

## The Russian disc (2026-11-24)

A US disc rebuilt with Russian text (`NFS-HP2rus.iso`): the Russian is in the English slot (`FE_ENGLISH`), and the fonts and menu textures
with Cyrillic are in replaced `GLOBAL/GLOBALB` and `FRONTEND/FRONTB` (Cyrillic on the Windows-1251 codes `0xC0..0xFF`, where the accented
letters were; 17 letters, `ABEKMHOPCTXaeopcx`, the disc writes in Latin). The port recognises such a disc by the letters of
`FE_ENGLISH.lang` (`russian_disc`, `pclang.cpp`), and on it:

- the first language is called "Русский", and the port's own strings for it are `data\lang\RUSSIAN_DISC.txt` (written by
  `tools/recomp/make_lang.py` from `tools/recomp/lang_ru.py`; on other discs the file is not read);
- the other five languages need their own fonts: the installer puts both original files into `data\original\` (`install.cpp`), and
  `disc.cpp` gives them to the game in place of the Russian ones when a language other than Russian is chosen. The game loads its fonts once
  at start, so going between Russian and another language restarts the game (a question on the Languages page). An install made before
  this has no `data\original\` files: the languages switch at once as before, on the Russian fonts;
- the language screen of the first start is not shown (the game starts in Russian);
- the text export (`data\text`) and the modding tool (`hp2text.py`) read this table as Russian text and put the look-alike letters back when
  writing.

The name `RUSSIAN.txt` is left for "a language of your own" below: that is another mechanism (slot 6 or 7 and a text file of its own).

## A language of your own (Russian, say)

1. A file `data\lang\RUSSIAN.txt`: `@name`, `@slot 6` (the place of `LONGEST`; `7` is the place of `ID`), `@file LANGUAGES\FE_RUSSIAN.BIN`,
   and the translations of the port's strings (a copy of any of the files). The port sets `LanguageFilenames[6]` to this file.
2. `FE_RUSSIAN.BIN` itself: a chunk `0x39000` in the format of `data-files.md` (the ID table, the hash table by English text, the text pool).
   There is no tool to build it yet: the simplest way is to take `FE_ENGLISH.lang` and replace the texts. The file has to be in `data\` and
   in `data\index.txt` (by the hash of its name, as `tools/recomp/unpack_data.py` writes it); `disc.cpp` builds the disc from `index.txt`
   only.
3. Fonts. The texts go to the game in Latin-1, and the game's fonts have only those letters. Cyrillic needs glyphs of its own: the font
   textures `FONT_*` through texture replacement (`HP2_REPLACE=<folder>`; there has been no `data\replace` folder since 2026-11-13) and
   their character tables. That is a separate piece of work.
4. Speech: without a bank of its own the game keeps the English one.
