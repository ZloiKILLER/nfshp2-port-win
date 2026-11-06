// The languages (pclang.h): the list, the port's strings in each, and the switch.
#include "pclang.h"

#include "host.h"
#include "pcconfig.h"
#include "pcmenu.h"
#include "ps2.h"

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

std::filesystem::path lang_dir()
{
    const char *env = getenv("HP2_DATA");
    return (env ? std::filesystem::path(env) : std::filesystem::path(host_exe_dir())) / "data" / "lang";
}

// UTF-8 to the game's Latin-1, with \xNN for a byte as it is
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
            out += cp <= 0xff ? (char)cp : '?';
            i += (size_t)n + 1;
        }
    }
    return out;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
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
        FILE *f = fopen(it->path().string().c_str(), "rb");
        if (!f) continue;
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
            else lf.port[to_game(a)] = b;
        }
        fclose(f);
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
