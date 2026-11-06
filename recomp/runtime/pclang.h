#pragma once
// The languages: the game's own six (FE_<NAME>.BIN, e_Languages 0..5) and what data\lang\ adds. A language is a file there,
// data\lang\<NAME>.txt, UTF-8:
//   @name Fran<c-cedilla>ais             the name the Languages page shows
//   @slot 6                              (only for a language the game does not have) the e_Languages it takes, 6 or 7, and its strings
//   @file LANGUAGES\FE_RUSSIAN.BIN       come from this file in data\ (as the game's FE_*.BIN, see recomp/docs/languages.md)
//   #100021<TAB>Clavier et manette.      one of the game's strings, by its LanguageID, in place of what the file has
//   Graphics<TAB>Graphismes              one of the port's: its English, then what it is in this language
// Lines starting with ; are comments. In a text, \xNN is that byte (the game's button icons are 0x80..0x8d) and @ breaks the line, as the
// game's own strings do. The text goes to the game in Latin-1: the fonts have those letters and no others.
#include <string>
#include <vector>

struct PcLanguage {
    std::string key;            // ENGLISH: the file's name, and what hp2.ini keeps
    std::string name;           // as the Languages page shows it
    int slot = 0;               // the game's e_Languages
};

const std::vector<PcLanguage> &pc_languages();
int pc_language_index();                        // the one in use, into pc_languages()
void pc_language_choose(int index);             // switch the game to it now and keep it in hp2.ini (from a menu)
void pc_language_apply();                       // the frontend is up: the game to hp2.ini's language, once
const std::string &tr(const std::string &english);     // the port's string in the language in use, or the English itself
// the port's texts of the game's strings for a language (by e_Languages slot): id -> Latin-1 text, the English ones from the code included
const std::vector<std::pair<int, std::string>> &pc_language_overrides(int slot);
