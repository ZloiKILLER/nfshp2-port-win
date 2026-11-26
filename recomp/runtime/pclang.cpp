// The languages (pclang.h): the list, the port's strings in each, and the switch.
#include "pclang.h"

#include "disc.h"
#include "host.h"
#include "pcconfig.h"
#include "pcmenu.h"
#include "ps2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>

namespace {

const u32 TheLanguageManager = 0x31c1c8;    // CurrLanguage +0
const u32 SetLanguage = 0x21e0d0;           // LanguageManager::SetLanguage(this, e_Languages): unloads, loads FE_*.BIN, speech, the texts
const u32 LanguageFilenames = 0x31c168;     // char *[8], by e_Languages

struct LangFile {
    std::unordered_map<std::string, std::string> port;     // English -> this language
    std::vector<std::pair<int, std::string>> game;         // LanguageID -> text
    std::string file;                                      // @file: the game's strings from another file (a slot of its own)
};

std::vector<PcLanguage> g_langs;
std::map<std::string, LangFile> g_files;
int g_current = 0;
bool g_loaded;

// The Russian disc (the user's NFS-HP2rus.iso, 2026-11-24: the USA disc with its English replaced). Its Russian is the texts of the
// English slot, FE_ENGLISH, and two files of fonts and menu textures, GLOBALB and FRONTB, with the Cyrillic letters where the accented
// Latin ones were, at their Windows-1251 codes. So on that disc:
//   - the first language is Russian, under that name, and the port's own strings for it are data\lang\RUSSIAN_DISC.txt (a file the
//     other discs do not look at);
//   - the other five need the disc's own fonts for their accents. The installer keeps those two files as the disc had them, in
//     data\original\ (install.cpp), and the disc is read from there when the language in use is not Russian (disc.cpp). The game
//     loads them once, at its start, so a change between Russian and another language starts the game again (pcsettings.cpp).
//     An install from before that has no such files: the Russian fonts stay, as it was.
bool g_ru_disc;             // FE_ENGLISH is Russian
bool g_ru_font = false;     // and the fonts this run reads are the Russian ones

std::filesystem::path lang_dir()
{
    const char *env = getenv("HP2_DATA");
    return (env ? std::filesystem::path(env) : std::filesystem::path(host_exe_dir())) / "data" / "lang";
}

// A letter for the Russian disc's font. The disc's own texts never use the Cyrillic letters that have a Latin look-alike (its
// "Bы пoлицeйcкий" has a Latin B, o, e, c; 17 letters in all), so whether the font has them at all is not known: the same is done
// here. No yo in it, no long dash, no angle quotes.
char ru_byte(u32 cp)
{
    static const char *const kLatin = "A\0B\0\0E\0\0\0\0K\0MHO\0PCT\0\0X\0\0\0\0\xfa\0\0\0\0\0"
                                      "a\0\0\0\0e\0\0\0\0\0\0\0\0o\0pc\0\0\0x\0\0\0\0\0\0\0\0\0\0";
    if (cp >= 0x410 && cp <= 0x44f) return kLatin[cp - 0x410] ? kLatin[cp - 0x410] : (char)(0xc0 + (cp - 0x410));
    if (cp == 0x401) return 'E';
    if (cp == 0x451) return 'e';
    if (cp == 0xab || cp == 0xbb || cp == 0x201c || cp == 0x201d || cp == 0x201e) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    return '?';
}

bool g_to_ru;       // the file being read is the Russian disc's

// UTF-8 to the game's Latin-1 (or to the Russian disc's letters), with \xNN for a byte as it is
std::string to_game(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            out += (char)strtoul(s.substr(i + 2, 2).c_str(), nullptr, 16);
            i += 4;
        } else if (c < 0x80) {
            out += (char)c;
            i++;
        } else {
            u32 cp = 0;
            int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
            cp = c & (0x3f >> n);
            for (int k = 1; k <= n && i + k < s.size(); k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3f);
            out += g_to_ru ? ru_byte(cp) : cp <= 0xff ? (char)cp : '?';
            i += (size_t)n + 1;
        }
    }
    return out;
}

// FE_ENGLISH's letters: most of them from 0xC0 up is Cyrillic; in the other languages the accented ones are a few in a hundred
// (the same test as the text export's, install.cpp)
bool russian_disc()
{
    FILE *f = fopen((lang_dir().parent_path() / "LANGUAGES" / "FE_ENGLISH.lang").string().c_str(), "rb");
    if (!f) return false;
    std::vector<u8> b(1u << 20);
    b.resize(fread(b.data(), 1, b.size(), f));
    fclose(f);
    auto w = [&](size_t at) { return (u32)b[at] | (u32)b[at + 1] << 8 | (u32)b[at + 2] << 16 | (u32)b[at + 3] << 24; };
    if (b.size() < 108 || w(0) != 0x39000) return false;
    const size_t pool = 108 + 8 * ((size_t)w(84) + w(88)), end = std::min(b.size(), pool + (size_t)w(92));
    size_t high = 0, letters = 0;
    for (size_t i = pool; i < end; i++) {
        if (b[i] >= 0xc0) high++, letters++;
        else if ((b[i] | 0x20) >= 'a' && (b[i] | 0x20) <= 'z') letters++;
    }
    return letters > 1000 && high * 4 > letters;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
    g_ru_disc = russian_disc();
    std::string ru_name;
    // the game's six, whether or not a file says more about them
    static const char *const kKey[6] = {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "SWEDISH"};
    static const char *const kName[6] = {"English", "Fran\xe7" "ais", "Deutsch", "Italiano", "Espa\xf1ol", "Svenska"};
    for (int i = 0; i < 6; i++) g_langs.push_back({kKey[i], kName[i], i});
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it(lang_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".txt") continue;
        std::string key = it->path().stem().string();
        for (char &ch : key) ch = (char)toupper((unsigned char)ch);
        const bool ru = key == "RUSSIAN_DISC";
        if (ru && !g_ru_disc) continue;
        FILE *f = fopen(it->path().string().c_str(), "rb");
        if (!f) continue;
        g_to_ru = ru;
        LangFile &lf = g_files[key];
        std::string name;
        int slot = -1;
        char line[4096];
        bool first = true;
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            if (first && l.size() >= 3 && (unsigned char)l[0] == 0xef && (unsigned char)l[1] == 0xbb && (unsigned char)l[2] == 0xbf) l.erase(0, 3);
            first = false;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.empty() || l[0] == ';') continue;
            if (l[0] == '@') {
                size_t sp = l.find(' ');
                std::string what = l.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
                std::string arg = sp == std::string::npos ? "" : l.substr(sp + 1);
                if (what == "name") name = to_game(arg);
                else if (what == "slot") slot = atoi(arg.c_str());
                else if (what == "file") lf.file = arg;
                continue;
            }
            size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            std::string a = l.substr(0, tab), b = to_game(l.substr(tab + 1));
            if (a[0] == '#') lf.game.push_back({atoi(a.c_str() + 1), b});
            else lf.port[a] = b;        // the English is plain letters
        }
        fclose(f);
        g_to_ru = false;
        if (ru) {
            ru_name = name;
            continue;
        }
        PcLanguage *have = nullptr;
        for (PcLanguage &l : g_langs)
            if (l.key == key) have = &l;
        if (have) {
            if (!name.empty()) have->name = name;
        } else if ((slot == 6 || slot == 7) && !lf.file.empty()) {
            g_langs.push_back({key, name.empty() ? key : name, slot});
        } else {
            fprintf(stderr, "lang: %s.txt is not one of the game's languages and has no @slot 6 or 7 with an @file\n", key.c_str());
        }
    }
    for (size_t i = 0; i < g_langs.size(); i++)
        if (g_langs[i].key == g_cfg.language) g_current = (int)i;
    if (!g_ru_disc) return;
    // the English slot is Russian: its strings of the port, its name; the fonts by the language this run starts in
    auto ru = g_files.find("RUSSIAN_DISC");
    if (ru != g_files.end()) {
        g_files["ENGLISH"] = std::move(ru->second);
        g_files.erase("RUSSIAN_DISC");
    } else {
        g_files.erase("ENGLISH");
    }
    g_ru_font = g_langs[(size_t)g_current].slot == 0 || !disc_has_originals();
    disc_use_originals(!g_ru_font);
    // the names as the font in use can draw them: no Cyrillic in the disc's own, no accents in the Russian one
    static const char *const kPlain = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    for (PcLanguage &l : g_langs) {
        if (l.slot == 0) l.name = g_ru_font && !ru_name.empty() ? ru_name : "Russian";
        else if (g_ru_font)
            for (char &ch : l.name)
                if ((unsigned char)ch >= 0xc0) ch = kPlain[(unsigned char)ch - 0xc0];
    }
}

// a language of the port's own: the game's strings from its file, in the slot it takes
void point_slot(const PcLanguage &l)
{
    auto f = g_files.find(l.key);
    if (l.slot < 6 || f == g_files.end() || f->second.file.empty()) return;
    const std::string &path = f->second.file;
    u32 s = pc_alloc((u32)path.size() + 1);
    memcpy(host(s), path.c_str(), path.size() + 1);
    SW(LanguageFilenames + 4 * (u32)l.slot, s);
}

}   // namespace

const std::vector<PcLanguage> &pc_languages()
{
    load();
    return g_langs;
}

int pc_language_index()
{
    load();
    return g_current;
}

const std::string &tr(const std::string &english)
{
    load();
    auto f = g_files.find(g_langs[(size_t)g_current].key);
    if (f == g_files.end()) return english;
    auto t = f->second.port.find(english);
    return t == f->second.port.end() ? english : t->second;
}

const std::vector<std::pair<int, std::string>> &pc_language_overrides(int slot)
{
    load();
    static const std::vector<std::pair<int, std::string>> none;
    // The Russian disc on the disc's own fonts: the game starts in its first language, the Russian one, until the frontend sets the
    // one in use (pc_language_apply). What the port says in between is said in the language in use, whose letters the font has.
    if (g_ru_disc && !g_ru_font && slot == 0) {
        auto f = g_files.find(g_langs[(size_t)g_current].key);
        return f == g_files.end() ? none : f->second.game;
    }
    for (const PcLanguage &l : g_langs)
        if (l.slot == slot) {
            auto f = g_files.find(l.key);
            // a slot of the port's: the language that has it now
            if (slot >= 6 && l.key != g_langs[(size_t)g_current].key) continue;
            return f == g_files.end() ? none : f->second.game;
        }
    return none;
}

void pc_language_choose(int index)
{
    load();
    if (index < 0 || index >= (int)g_langs.size()) return;
    g_current = index;
    snprintf(g_cfg.language, sizeof g_cfg.language, "%s", g_langs[(size_t)index].key.c_str());
    pc_config_save();
    point_slot(g_langs[(size_t)index]);
    pc_call(SetLanguage, TheLanguageManager, (u32)g_langs[(size_t)index].slot);
}

void pc_language_start() { load(); }

bool pc_language_restarts(int index)
{
    load();
    if (index < 0 || index >= (int)g_langs.size()) return false;
    return g_ru_disc && disc_has_originals() && (g_langs[(size_t)index].slot == 0) != g_ru_font;
}

void pc_language_keep(int index)
{
    load();
    if (index < 0 || index >= (int)g_langs.size()) return;
    snprintf(g_cfg.language, sizeof g_cfg.language, "%s", g_langs[(size_t)index].key.c_str());
    pc_config_save();
}

void pc_language_apply()
{
    load();
    static bool done;
    if (done) return;
    done = true;
    const PcLanguage &l = g_langs[(size_t)g_current];
    if ((s32)LW(TheLanguageManager) == l.slot) return;
    point_slot(l);
    pc_call(SetLanguage, TheLanguageManager, (u32)l.slot);
}

// The game's own choice of language, "Language Options Pre FE" between the title and the menus: the PAL discs show it on their first
// boot (ShouldSeeLanguageSelectScreen 0x16f8c8: the first pass through the boot screens, gp-21132 == 1, nothing at 0x2effe0 skipping
// them, GetVideoMode() == PAL). The port draws NTSC; it shows the screen on its own very first start, when hp2.ini has no language.
static bool g_asking;

void HOOK_ShouldSeeLanguageSelectScreen__Fv(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0];
    orig(c);
    load();
    if (g_ru_disc) {
        // The Russian disc starts in Russian: that screen's choice would need the other fonts at once (pcsettings.cpp's Languages
        // page changes it, with a restart).
        c.r[2].d[0] = 0;
        if (g_cfg.ask_language) {
            g_cfg.ask_language = false;
            pc_config_save();
        }
        return;
    }
    if (!c.r[2].w[0] && g_cfg.ask_language && (s32)LW(gp - 21132) == 1 && !LW(0x2effe0)) c.r[2].d[0] = 1;
    if (!c.r[2].w[0]) return;
    g_asking = true;
    g_cfg.ask_language = false;     // asked once: the language in use stays, unless the screen picks another (it need not set the same)
    pc_config_save();
}

// what that screen chose goes to hp2.ini; the port's own switches (pc_language_choose, pc_language_apply) are already the one in use
void HOOK_SetLanguage__15LanguageManager11e_Languages(Ctx &c, void (*orig)(Ctx &))
{
    const int slot = (s32)c.r[5].w[0];
    orig(c);
    if (!g_asking) return;
    load();
    if (g_langs[(size_t)g_current].slot == slot) return;
    for (size_t i = 0; i < g_langs.size(); i++)
        if (g_langs[i].slot == slot) {
            g_current = (int)i;
            snprintf(g_cfg.language, sizeof g_cfg.language, "%s", g_langs[i].key.c_str());
            pc_config_save();
            return;
        }
}
