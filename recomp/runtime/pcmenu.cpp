// The port's own items in the game's menus (Flailer UI), see recomp/docs/frontend-menus.md.
//
// Three tools, all working on guest memory:
//  - HOOK_<name>: the host runs around a recompiled game function (`hook <name>` in
//    hle_names.txt; tools/recomp/gen.py emits the wrapper);
//  - guest calls: host code calls game functions by their retail address (pc_call);
//  - Flailer patches: when the frontend bundle is loaded, screens are cloned in memory.
// On top of them, "pages" (pcmenu.h): screens of rows with values, run by the host.
#include "pcmenu.h"
#include "native.h"
#include "hw.h"
#include "pcconfig.h"
#include "pclang.h"
#include <map>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- guest access ------------------------------------------------------------------
Ctx *g_pc_ctx;

static u32 gp() { return g_pc_ctx->r[28].w[0]; }

u32 pc_call(u32 fn, u32 a0, u32 a1, u32 a2, u32 a3, u32 t0)
{
    Ctx &c = *g_pc_ctx;
    Ctx saved = c;
    c.r[4].d[0] = sx32(a0);
    c.r[5].d[0] = sx32(a1);
    c.r[6].d[0] = sx32(a2);
    c.r[7].d[0] = sx32(a3);
    c.r[8].d[0] = sx32(t0);
    c.r[29].d[0] -= 0x200;
    ps2_call(c, fn);
    u32 r = c.r[2].w[0];
    c = saved;
    return r;
}

std::string pc_str(u32 a)
{
    std::string s;
    for (u8 ch; a && (ch = LB(a)) != 0 && s.size() < 512; a++)
        s += (char)ch;
    return s;
}

// retail (SLUS_203.62) addresses of the game functions used here
namespace fn {
const u32 bMalloc = 0x2577c0;               // bMalloc(size, name, line, flags)
const u32 bFree = 0x257850;                 // bFree(ptr)
const u32 bStringHash = 0x2534a8;
const u32 SetString = 0x140e20;             // FlailerTextChunk::SetString(chunk, str)
const u32 SetVisibility = 0x140728;         // FlailerObjectChunk::SetVisibility(chunk, vis)
const u32 UpdateLanguageText = 0x1402a0;    // FlailerScreenChunk::UpdateLanguageText(screen)
const u32 ChangeToScreen = 0x14cc78;        // ChangeToScreen(name, push current)
const u32 GrayMenuItem = 0x16d198;          // GrayMenuItem(item hash, mode, screen hash)
const u32 ConfigCtor = 0x16adb0;            // FlailerScreenConfig(name, callback, hash, help items)
const u32 OptionsCallback = 0x1eef00;       // tOptionsScreen::Callback(msg, data)
const u32 BlurbNewText = 0x249518;          // tAnimatedBlurb::NewText(blurb, str)
const u32 BlurbAnimate = 0x249348;          // tAnimatedBlurb::Animate(blurb)
const u32 SetHighlightItem = 0x2488d0;      // tHighlightAnimation::SetCurrentMenuItem(anim, item hash)
const u32 MainMenuHighlight = 0x216880;     // NewMainMenu::HighlightMenuItem(menu, mask hash, language id)
}

static const u32 NAME_STRING = 0x2b9bb0;    // a string literal of the game, for bMalloc's name
static const u32 OPTIONS_SCREEN = 0x95ad096b, DISPLAY_SCREEN = 0x8bc68340, FRONTB_PROJECT = 0x255842fd;
static const u32 MAIN_MENU_SCREEN = 0xae0f8aa1;
static const u32 PAUSE_SCREEN = 0x605d2bf1, PAUSE_LOGO_TEXTURE = 0xc9f75a5b;

// What the frontend bundle's pages take is given back when the bundle is unloaded: the race
// needs the heap as the game leaves it (a block left in the middle breaks its big allocations).
static bool g_page_alloc;
static std::vector<u32> g_page_mem;

static u32 galloc(u32 size)
{
    u32 p = pc_call(fn::bMalloc, size, NAME_STRING, 0, 16);
    if (p) memset(host(p), 0, size);
    if (p && g_page_alloc) g_page_mem.push_back(p);
    return p;
}

static std::map<u32, std::string> g_texts;     // the port's texts on screen objects, in English (set_text)

static void free_pages()
{
    for (u32 p : g_page_mem) pc_call(fn::bFree, p);
    g_page_mem.clear();
    g_texts.clear();
}

// a guest copy of a string; the slot is reused after 16 more
static u32 gstr(const std::string &s)
{
    static u32 ring;
    static int slot;
    if (!ring) ring = galloc(16 * 256);
    u32 p = ring + (slot++ & 15) * 256;
    size_t n = s.size() < 255 ? s.size() : 255;
    memcpy(host(p), s.data(), n);
    SB(p + (u32)n, 0);
    return p;
}

// a guest string that stays
static u32 gstr_keep(const std::string &s)
{
    u32 p = galloc((u32)s.size() + 1);
    memcpy(host(p), s.c_str(), s.size());
    return p;
}

u32 pc_hash(const char *s) { return pc_call(fn::bStringHash, gstr(s)); }

u32 pc_alloc(u32 size) { return galloc(size); }

// CurrentGameFlowState (.sdata) is GAMEFLOW_STATE_RACING
bool pc_racing() { return g_pc_ctx && LW(gp() - 21420) == 6; }

// Player::SetInputMode: 0 none, 1 pause menu, 2 racing, 3 frozen zone, 4 AI demo, 5 pre-race,
// 6 finish, 7 instant replay. One player on a PC
static u32 g_player_mode;

// two players now: in a race, and on the screens where both choose at once (SetTwoPlayerScreenMode, src/FMenus.cpp)
static u32 g_two_player_screens;
bool pc_two_player_race() { return pc_racing() && LW(gp() - 20920) == 2; }     // Player::NumPlayers
bool pc_two_player_screen() { return g_two_player_screens != 0; }

void HOOK_SetTwoPlayerScreenMode__Fi(Ctx &c, void (*orig)(Ctx &))
{
    g_two_player_screens = c.r[4].w[0];
    orig(c);
}

// ReplayManager (gp-22440): +0 it plays back
bool pc_replay_playing()
{
    if (!pc_racing()) return false;
    const u32 rm = LW(gp() - 22440);
    return rm && LW(rm) != 0;
}

// A screen with a list to choose from is up. After the finish the player's mode can still be that of a car being driven (the AI takes over and
// the Race Over menu lies over it), so the mode alone does not say whether the keys are a menu's.
static bool menu_on_screen()
{
    if (!g_pc_ctx) return false;
    const u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0;
    const u32 screens = node ? node + 8 : 0, screen = screens ? LW(screens) : 0;
    const u32 menu = screen && screen != screens ? LW(screen + 8) : 0;
    return menu && LW(menu) && LW(menu) != menu;
}

// the hash of the screen whose menu is up, 0 with none
u32 pc_screen_hash()
{
    if (!g_pc_ctx) return 0;
    const u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0;
    const u32 screens = node ? node + 8 : 0, screen = screens ? LW(screens) : 0;
    const u32 menu = screen && screen != screens ? LW(screen + 8) : 0;
    return menu ? LW(menu + 20) : 0;
}

bool pc_race_menu() { return pc_racing() && (g_player_mode == 0 || g_player_mode == 6 || (g_player_mode != 1 && menu_on_screen())); }
// the name of the screen that is up (FlailerScreenNode +16 its FlailerScreenConfig, +8 the config's ScreenName), "" with none
static std::string screen_name()
{
    if (!g_pc_ctx) return "";
    const u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0, config = node ? LW(node + 16) : 0, name = config ? LW(config + 8) : 0;
    return name ? pc_str(name) : "";
}

// The pointer: in the front end, the pause menu, a replay and the screens after the finish; in a race and while one loads, none. And none
// at the start. There the game is on "BootAutoload" for a second, and then on "Title Screen" all through the logos and the opening film:
// it is the title screen that plays them (TitleScreen::pInstance, gp-21156; its State +12 is 2, eTITLE_SCREEN_PLAYING_MOVIES, while a film
// runs, 1 while it waits for a button on the title; it plays them again when left alone there). So: none before a screen is up, none on
// "BootAutoload", none while the title screen plays a film. The profiles' and the language's choice are screens of their own, with something
// to point at.
bool pc_cursor_wanted()
{
    const std::string screen = screen_name();
    const u32 title = g_pc_ctx ? LW(gp() - 21156) : 0;
    const bool film = title && LW(title + 12) == 2;
    {   // HP2_DIAG: the screens as they come
        static const bool diag = getenv("HP2_DIAG") != nullptr;
        static std::string last = "?";
        static bool last_film;
        if (diag && (screen != last || film != last_film))
            fprintf(stderr, "screen: \"%s\"%s%s\n", screen.c_str(), menu_on_screen() ? ", with a menu" : "", film ? ", the title's film" : "");
        last = screen;
        last_film = film;
    }
    if (film) return false;
    if (!pc_racing()) return !screen.empty() && screen != "BootAutoload";
    return g_player_mode == 1 || g_player_mode == 6 || menu_on_screen() || pc_replay_playing();
}
bool pc_hud_up() { return pc_racing() && (g_player_mode == 2 || g_player_mode == 3); }
bool pc_car_input() { return pc_racing() && g_player_mode != 0 && g_player_mode != 1 && g_player_mode != 6 && !menu_on_screen(); }

// ---- Flailer: the chunk layouts of this (retail) build ---------------------------------
// project  : Next, Prev, NameHash +8, ScreenList +12 (Head, Tail)
// screen   : Next, Prev, NameHash +8, ..., NumObjects +28, Visible +30, ObjectList +32
// object   : Next, Prev, NameHash +8, GroupHash +12, Name[24] +16, Type +40, Visible +41,
//            Z +42, Dirty +43, strip +44, X +48, Y +52, ScaleX +56, ScaleY +60
// bitmap   : TextureHash +80 ... 144 bytes;  text: FontHash +80, MaxChars +84,
//            FormatOptions +88, Color +92, LanguageID +96, String +100
// A chunk's size is the word before its payload. The projects are a bTList at gp - 22424
// (head, tail); a list is circular through its own address.

static float fl(u32 a)
{
    u32 t = LW(a);
    float f;
    memcpy(&f, &t, 4);
    return f;
}

static void set_fl(u32 a, float f)
{
    u32 t;
    memcpy(&t, &f, 4);
    SW(a, t);
}

static void dump_project(FILE *f, u32 p)
{
    fprintf(f, "project %08x\n", LW(p + 8));
    u32 sl = p + 12;
    for (u32 s = LW(sl); s && s != sl; s = LW(s)) {
        fprintf(f, " screen %08x objects %d at %08x\n", LW(s + 8), LH(s + 28), s);
        u32 ol = s + 32;
        for (u32 o = LW(ol); o && o != ol; o = LW(o)) {
            char name[25] = {};
            for (int i = 0; i < 24; i++) name[i] = (char)LB(o + 16 + i);
            int type = (s8)LB(o + 40);
            fprintf(f, "  %08x %-24s %s vis%d z%2d  %7.1f %7.1f  x%.2f y%.2f grp %08x", LW(o + 8), name,
                    type == 1 ? "text  " : type == 2 ? "bitmap" : "other ", (s8)LB(o + 41), (s8)LB(o + 42), fl(o + 48),
                    fl(o + 52), fl(o + 56), fl(o + 60), LW(o + 12));
            if (type == 1)
                fprintf(f, " font %08x max %u fmt %02x col %08x lang %u \"%s\"", LW(o + 80), LW(o + 84), LW(o + 88),
                        LW(o + 92), LW(o + 96), pc_str(o + 100).c_str());
            else if (type == 2)
                fprintf(f, " tex %08x col %08x", LW(o + 80), LW(o + 88));
            fprintf(f, " @%08x\n", o);
        }
    }
}

static u32 find_screen(u32 project, u32 hash)
{
    u32 sl = project + 12;
    for (u32 s = LW(sl); s && s != sl; s = LW(s))
        if (LW(s + 8) == hash) return s;
    return 0;
}

static u32 find_object(u32 screen, u32 hash, int type = 0)
{
    u32 ol = screen + 32;
    for (u32 o = LW(ol); o && o != ol; o = LW(o))
        if (LW(o + 8) == hash && (!type || (s8)LB(o + 40) == type)) return o;
    return 0;
}

static u32 find_project(u32 hash)
{
    u32 list = gp() - 22424;
    for (u32 p = LW(list); p && p != list; p = LW(p))
        if (LW(p + 8) == hash) return p;
    return 0;
}

static void list_append(u32 list, u32 node)
{
    u32 tail = LW(list + 4);
    SW(tail, node);
    SW(node + 4, tail);
    SW(node, list);
    SW(list + 4, node);
}

// a copy of an object chunk in `screen`; a text may get a longer string (max chars)
static u32 clone_object(u32 screen, u32 src, u32 max_chars = 0)
{
    bool text = (s8)LB(src + 40) == 1;
    u32 size = LW(src - 4);
    if (text && max_chars > LW(src + 84)) size = 100 + ((max_chars + 3) & ~3u);
    u32 o = galloc(size);
    u32 keep = LW(src - 4) < size ? LW(src - 4) : size;
    memcpy(host(o), host(src), keep);
    if (text && max_chars > LW(src + 84)) SW(o + 84, max_chars);
    SW(o + 44, 0);                          // no cached strip
    SB(o + 43, 5);
    list_append(screen + 32, o);
    SH(screen + 28, (u16)(LH(screen + 28) + 1));
    return o;
}

// The port's texts go through tr (pclang.h); each object's English is kept, so that a change of language can put them again.
static void set_text(u32 obj, const std::string &s)
{
    g_texts[obj] = s;
    pc_call(fn::SetString, obj, gstr(tr(s)));
}
static void blurb_text(u32 blurb, const std::string &s) { pc_call(fn::BlurbNewText, blurb, gstr(tr(s))); }

// ---- pages ------------------------------------------------------------------------------
static std::vector<PcPage *> &pages()
{
    static std::vector<PcPage *> v;
    return v;
}

PcPage &pc_add_page(const char *name, const char *title)
{
    PcPage *p = new PcPage;
    p->name = name;
    p->title = title;
    pages().push_back(p);
    return *p;
}

PcPage *pc_find_page(const char *name)
{
    for (PcPage *p : pages())
        if (p->name == name) return p;
    return nullptr;
}

static std::string g_controls_page, g_graphics_page, g_profiles_page;

void pc_profiles_page(const char *name) { g_profiles_page = name; }

void pc_options_pages(const char *controls, const char *graphics)
{
    g_controls_page = controls;
    g_graphics_page = graphics;
}

static u32 page_hash(PcPage &pg)
{
    if (!pg.hash) {
        std::string up = pg.name;
        for (char &ch : up) ch = ch == ' ' ? '_' : (char)toupper((unsigned char)ch);
        pg.hash = pc_hash(up.c_str());
    }
    return pg.hash;
}

static PcPage *page_by_hash(u32 hash)
{
    for (PcPage *p : pages())
        if (p->hash == hash) return p;
    return nullptr;
}

static u32 menuitem_hash(int k) { return pc_hash(("MENUITEM_" + std::to_string(k)).c_str()); }

static const u32 AUDIO_SCREEN = 0xe116761c, SCROLL_UP_ARROW = 0xedf2e4ee, SCROLL_DOWN_ARROW = 0x20d73d01;

// the screen of a page: the Options screen with the rows it needs, a value text per row
// and the Display screen's two arrows
static void build_page(u32 project, PcPage &pg)
{
    u32 opt = find_screen(project, OPTIONS_SCREEN), dsp = find_screen(project, DISPLAY_SCREEN);
    if (!opt || !dsp || find_screen(project, page_hash(pg))) return;
    int n = (int)pg.rows.size() < 7 ? (int)pg.rows.size() : 7;
    u32 s = galloc(40);
    memcpy(host(s), host(opt), 40);
    SW(s + 8, pg.hash);
    SH(s + 28, 0);
    SH(s + 30, 1);                          // invisible until the game turns it on
    SW(s + 32, s + 32);
    SW(s + 36, s + 32);
    list_append(project + 12, s);

    u32 item_hash[8], mask_hash[8];
    for (int k = 1; k <= 7; k++) {
        item_hash[k] = menuitem_hash(k);
        mask_hash[k] = pc_hash(("MI_" + std::to_string(k) + "_MASK").c_str());
    }
    u32 ol = opt + 32;
    for (u32 o = LW(ol); o && o != ol; o = LW(o)) {
        u32 h = LW(o + 8);
        int label = 0;                      // MENUITEM_k: the row's label, with room for 24 characters
        bool skip = false;
        for (int k = 1; k <= 7; k++) {
            if (h == item_hash[k] && (s8)LB(o + 40) == 1) label = k;
            if (k > n && (h == item_hash[k] || h == mask_hash[k])) skip = true;
        }
        // the thin bars between rows: only down to the last row
        if ((s8)LB(o + 40) == 2 && h == 0xaa5d7d3f && fl(o + 52) > 71.0f + 37.0f * (n - 1) + 30.0f) skip = true;
        if (skip) continue;
        bool title = (s8)LB(o + 40) == 1 && LW(o + 96) == 10005;       // "Options"
        u32 c = clone_object(s, o, label || title ? 32 : 0);
        if (label) set_text(c, pg.rows[label - 1].label);
        if (title) set_text(c, pg.title);
    }
    u32 value_src = find_object(dsp, 0x55cb09f2, 1);        // TRACKMAP: the value text of Track Map
    for (int k = 1; k <= n; k++) {
        u32 item = find_object(s, item_hash[k], 1);
        if (!item) continue;
        u32 v = clone_object(s, value_src, 32);
        SW(v + 8, pc_hash(("PC_VALUE_" + std::to_string(k)).c_str()));
        SW(v + 12, LW(item + 12));          // the row's group
        set_fl(v + 48, 501.0f);
        set_fl(v + 52, fl(item + 52) + 6.0f);
        SW(v + 96, 0);
        set_text(v, "");
    }
    for (u32 h : {0xd1cda594u, 0x9345e4a7u})               // LEFT_ARROW, RIGHT_ARROW
        if (u32 a = find_object(dsp, h, 2)) clone_object(s, a);
    if (pg.rows.size() > 7)
        if (u32 audio = find_screen(project, AUDIO_SCREEN))
            for (u32 h : {SCROLL_UP_ARROW, SCROLL_DOWN_ARROW})
                if (u32 a = find_object(audio, h, 2)) {
                    u32 c = clone_object(s, a);
                    SW(c + 88, LW(c + 88) | 0x80000000u);       // the list in Audio pulses its alpha; here it stays
                }
    pc_call(fn::UpdateLanguageText, s);
}

static u32 value_object(u32 screen, int k)
{
    return find_object(screen, pc_hash(("PC_VALUE_" + std::to_string(k)).c_str()), 1);
}

// the values and the arrows of the page on screen
static void page_refresh(PcPage &pg)
{
    u32 project = find_project(FRONTB_PROJECT);
    u32 s = project ? find_screen(project, pg.hash) : 0;
    if (!s) return;
    const int total = (int)pg.rows.size();
    const bool scrolls = total > 7;
    if (pg.top > total - 7) pg.top = total > 7 ? total - 7 : 0;
    for (int k = 1; k <= total && k <= 7; k++) {
        PcRow &r = pg.rows[(size_t)(pg.top + k - 1)];
        if (u32 label = find_object(s, menuitem_hash(k), 1)) set_text(label, r.label);
        if (u32 v = value_object(s, k)) set_text(v, r.value ? r.value() : "");
    }
    for (int k = total + 1; k <= 7; k++)
        if (u32 v = value_object(s, k)) set_text(v, "");
    if (u32 a = find_object(s, SCROLL_UP_ARROW, 2)) pc_call(fn::SetVisibility, a, scrolls && pg.top > 0 ? 1 : 0);
    if (u32 a = find_object(s, SCROLL_DOWN_ARROW, 2)) pc_call(fn::SetVisibility, a, scrolls && pg.top + 7 < total ? 1 : 0);
    u32 la = find_object(s, 0xd1cda594, 2), ra = find_object(s, 0x9345e4a7, 2);
    const int at = pg.top + (int)pg.cur - 1;
    bool arrows = pg.cur >= 1 && at < total && pg.rows[(size_t)at].value && !pg.rows[(size_t)at].select_only;
    u32 item = pg.cur ? find_object(s, menuitem_hash(pg.cur), 1) : 0;
    for (u32 a : {la, ra}) {
        if (!a) continue;
        if (arrows && item) set_fl(a + 52, fl(item + 52) + 8.0f), SB(a + 43, 4);
        pc_call(fn::SetVisibility, a, arrows && item ? 1 : 0);
    }
}

static PcPage *g_current_page;
static u32 g_current_self;      // the page's tOptionsScreen object

static std::vector<std::function<void()>> g_frame_fns;
void pc_on_frame(std::function<void()> fn) { g_frame_fns.push_back(std::move(fn)); }

void pc_set_blurb(const std::string &text)
{
    // the screen object is freed when the screen goes (the game keeps the live one at gp - 16888)
    if (!g_current_self || LW(gp() - 16888) != g_current_self) return;
    blurb_text(g_current_self + 100, text);
    pc_call(fn::BlurbAnimate, g_current_self + 100);
}

void pc_refresh_page()
{
    if (g_current_page && g_current_self && LW(gp() - 16888) == g_current_self) page_refresh(*g_current_page);
}

void pc_open_page(const char *name) { pc_call(fn::ChangeToScreen, gstr(name), 1); }

static bool press_up_soon;

// the rows that are there show, the others go; and the Back at the bottom
static void page_shape(PcPage &pg)
{
    const u32 hash = page_hash(pg);
    for (int k = 1; k <= 7; k++) pc_call(fn::GrayMenuItem, menuitem_hash(k), k <= (int)pg.rows.size() ? 1 : 2, hash);
    u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, hash) : 0;
    if (!s) return;
    for (const char *name : {"PC_BACK_ICON", "PC_BACK_TEXT"})
        if (u32 o = find_object(s, pc_hash(name), 1)) pc_call(fn::SetVisibility, o, pg.hide_back ? 0 : 1);
}

void pc_reshape_page()
{
    if (!g_current_page || !g_current_self || LW(gp() - 16888) != g_current_self) return;
    page_shape(*g_current_page);
    page_refresh(*g_current_page);
    // the highlight was on a row that went: the game's own move takes it to the last that is there
    if (g_current_page->cur > g_current_page->rows.size()) press_up_soon = true;
}

// a screen message for a page's tOptionsScreen: returns true when it was dealt with
static bool page_event(PcPage &pg, u32 self, u32 msg, u32 data)
{
    const int slot = (int)(LW(data) - menuitem_hash(1)) + 1, total = (int)pg.rows.size();
    bool known = slot >= 1 && slot <= total && slot <= 7;
    const int row = pg.top + slot;
    switch (msg) {
    case 4:                                 // MENUITEM_HIGHLIGHT: the help line and the arrows
        if (!known) return true;
        pg.cur = (u32)slot;
        blurb_text(self + 100, pg.rows[(size_t)row - 1].blurb);
        pc_call(fn::BlurbAnimate, self + 100);
        page_refresh(pg);
        return true;
    case 8: case 9: {                       // up and down: past the seventh slot the rows move under it. Param 1 keeps the selection where it is
        if (total <= 7 || !known) return true;
        const bool down = msg == 9;
        if (down ? slot == 7 && row < total : slot == 1 && pg.top > 0) {
            pg.top += down ? 1 : -1;
            SW(data + 8, 1);
            blurb_text(self + 100, pg.rows[(size_t)(pg.top + slot) - 1].blurb);
            pc_call(fn::BlurbAnimate, self + 100);
            page_refresh(pg);
        } else if (down ? slot == 7 : slot == 1) {
            pg.top = down ? 0 : total - 7;      // round: the game's menu goes to the other end
        }
        return true;
    }
    case 1: case 6: case 7: {               // Select, Left, Right
        if (!known) return true;
        PcRow &r = pg.rows[(size_t)row - 1];
        int dir = msg == 6 ? -1 : msg == 7 ? 1 : (r.value ? 1 : 0);
        if (msg != 1 && (!r.value || r.select_only)) return true;
        if (r.change) r.change(dir);
        if (msg != 1) SW(data + 12, msg == 6 ? 1 : 2);      // the sound of Left and Right
        page_refresh(pg);
        return true;
    }
    case 18:                                // SCREEN_TICK: the game's own animation
        return false;
    }
    return true;
}

// ---- Options -------------------------------------------------------------------------------
// Eight rows over the screen's seven slots, scrolling as a page does. A row of the game's goes to it as the item that does its job
// (Audio 1, Display 3, Load/Save 4, Names 5, Credits 6; item 7, Languages, is hidden on NTSC); the others open the port's pages.
// The labels and the help line are the port's, by the row.
enum { OPT_GAME, OPT_CONTROLS, OPT_GRAPHICS, OPT_PROFILES, OPT_LANGUAGES };
static const struct {
    const char *label, *blurb;
    int kind, item;
} kOptions[] = {
    {"Audio", "Set the volume and the Jukebox.", OPT_GAME, 1},
    {"Controls", "Keyboard and gamepad.", OPT_CONTROLS, 0},
    {"Graphics", "Window, resolution, detail.", OPT_GRAPHICS, 0},
    {"HUD", "Set what you want to see.", OPT_GAME, 3},
    {"Profiles", "Choose, add or delete a profile.", OPT_PROFILES, 4},
    {"Player Names", "Set your player names.", OPT_GAME, 5},
    {"Languages", "Choose the language of the game.", OPT_LANGUAGES, 0},
    {"Credits", "See who made this game.", OPT_GAME, 6},
};
static const int kOptionsRows = (int)(sizeof kOptions / sizeof kOptions[0]);
static int g_opt_top;
static std::string g_languages_page = "PC Languages";

static void options_refresh()
{
    u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, OPTIONS_SCREEN) : 0;
    if (!s) return;
    if (g_opt_top > kOptionsRows - 7) g_opt_top = kOptionsRows - 7;
    for (int k = 1; k <= 7; k++)
        if (u32 o = find_object(s, menuitem_hash(k), 1)) set_text(o, kOptions[g_opt_top + k - 1].label);
    if (u32 a = find_object(s, SCROLL_UP_ARROW, 2)) pc_call(fn::SetVisibility, a, g_opt_top > 0 ? 1 : 0);
    if (u32 a = find_object(s, SCROLL_DOWN_ARROW, 2)) pc_call(fn::SetVisibility, a, g_opt_top + 7 < kOptionsRows ? 1 : 0);
}

// the slot of the menu on screen that has the highlight, 1..: the game's menu (as mouse_frame finds it), its current item's object hash
static int highlighted_slot()
{
    u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0;
    u32 screens = node ? node + 8 : 0, screen = screens ? LW(screens) : 0;
    u32 menu = screen && screen != screens ? LW(screen + 8) : 0;
    u32 item = menu ? LW(menu + 8) : 0;
    return item ? (int)(LW(item + 12) - menuitem_hash(1)) + 1 : 0;
}

static bool options_event(u32 self, u32 msg, u32 data, Ctx &c, void (*orig)(Ctx &))
{
    if (msg == 24) {                        // the game puts the slot's own help line (three times after a highlight): the row's again
        orig(c);
        const int cur = highlighted_slot();
        if (cur >= 1 && cur <= 7) blurb_text(self + 100, kOptions[g_opt_top + cur - 1].blurb);
        return true;
    }
    const int slot = (int)(LW(data) - menuitem_hash(1)) + 1;
    if (slot < 1 || slot > 7) return false;
    const int row = g_opt_top + slot - 1;   // into kOptions
    switch (msg) {
    case 4:                                 // the highlight: the row's help line
        blurb_text(self + 100, kOptions[row].blurb);
        pc_call(fn::BlurbAnimate, self + 100);
        return true;                        // as a page: the game's would put the slot's own help line
    case 8: case 9: {                       // up and down: past the seventh slot the rows move under it
        const bool down = msg == 9;
        if (down ? slot == 7 && row + 1 < kOptionsRows : slot == 1 && g_opt_top > 0) {
            g_opt_top += down ? 1 : -1;
            SW(data + 8, 1);                // Param 1: the highlight stays on its slot
            options_refresh();
            blurb_text(self + 100, kOptions[g_opt_top + slot - 1].blurb);
            pc_call(fn::BlurbAnimate, self + 100);
            return true;
        }
        if (down ? slot == 7 : slot == 1) {
            g_opt_top = down ? 0 : kOptionsRows - 7;    // round: the game's menu goes to the other end
            options_refresh();
        }
        return true;
    }
    case 1: {
        const auto &o = kOptions[row];
        if (o.kind == OPT_CONTROLS) pc_open_page(g_controls_page.c_str());
        else if (o.kind == OPT_GRAPHICS) pc_open_page(g_graphics_page.c_str());
        else if (o.kind == OPT_LANGUAGES) pc_open_page(g_languages_page.c_str());
        else if (o.kind == OPT_PROFILES && !g_profiles_page.empty()) pc_open_page(g_profiles_page.c_str());
        else {
            u32 mine = LW(data);
            SW(data, menuitem_hash(o.item));
            orig(c);
            SW(data, mine);
        }
        return true;
    }
    case 6: case 7:
        return true;
    }
    return false;
}

// the Options screen gets the scroll arrows of Audio's list, as a page with more than seven rows does, and its seven labels room for
// any row's in any language: each is put back as a copy with 32 characters (the game's have the length of their own word)
static void add_options_arrows(u32 project)
{
    u32 s = find_screen(project, OPTIONS_SCREEN), audio = find_screen(project, AUDIO_SCREEN);
    if (!s || !audio || find_object(s, SCROLL_UP_ARROW, 2)) return;
    for (int k = 1; k <= 7; k++) {
        u32 o = find_object(s, menuitem_hash(k), 1);
        if (!o || LW(o + 84) >= 32) continue;
        clone_object(s, o, 32);
        const u32 next = LW(o), prev = LW(o + 4);
        SW(prev, next);
        SW(next + 4, prev);
        SH(s + 28, (u16)(LH(s + 28) - 1));
    }
    for (u32 h : {SCROLL_UP_ARROW, SCROLL_DOWN_ARROW})
        if (u32 a = find_object(audio, h, 2)) {
            u32 c = clone_object(s, a);
            SW(c + 88, LW(c + 88) | 0x80000000u);
        }
}

// a change of language: the port's texts again, and the page on screen
void pc_retranslate()
{
    for (auto &t : g_texts) pc_call(fn::SetString, t.first, gstr(tr(t.second)));
    pc_refresh_page();
}

// ---- Audio: Master Volume -------------------------------------------------------------------
// A row above the game's first: the list is the objects of a table (AUDIO_LIST), which move a row
// down as FRONTB loads; the constructor's visible area and list get the new row after it runs.
static const u32 AUDIO_LIST = 0x2b9c30, MONOSTEREO = 0x2932dfca;
static const int AUDIO_SCROLL = 284;        // tScrollingMenuList in the screen
static const int AUDIO_BLURB = 372;         // its tAnimatedBlurb
static const char MASTER_BLURB[] = "Master volume of the game.";

namespace fn {
const u32 ScrollSetVisibleArea = 0x249f28;  // tScrollingMenuList::SetVisibleArea(list, top item, bottom item)
const u32 ScrollAddObject = 0x249f70;       // tScrollingMenuList::AddObject(list, object)
}

static u32 master_value_hash() { return pc_hash("PC_MASTER_VALUE"); }

static void add_master_row(u32 project)
{
    u32 s = find_screen(project, AUDIO_SCREEN);
    u32 one = s ? find_object(s, menuitem_hash(1), 1) : 0, two = s ? find_object(s, menuitem_hash(2), 1) : 0;
    u32 eight = s ? find_object(s, menuitem_hash(8), 1) : 0, mode = s ? find_object(s, MONOSTEREO, 1) : 0;
    if (!one || !two || !eight || !mode || find_object(s, menuitem_hash(0), 1)) return;
    float y = fl(one + 52), step = fl(two + 52) - y;
    u32 t = clone_object(s, one, 24);
    SW(t + 8, menuitem_hash(0));
    SW(t + 96, 0);
    set_text(t, "Master Volume");
    u32 v = clone_object(s, mode, 8);
    SW(v + 8, master_value_hash());
    SW(v + 12, LW(one + 12));
    set_fl(v + 52, y + fl(mode + 52) - fl(eight + 52));
    SW(v + 96, 0);
    for (int i = 0; i < 26; i++) {
        u32 o = find_object(s, LW(AUDIO_LIST + i * 4u));
        if (o) set_fl(o + 52, fl(o + 52) + step), SB(o + 43, 5);
    }
}

static void master_show()
{
    u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, AUDIO_SCREEN) : 0;
    if (u32 v = s ? find_object(s, master_value_hash(), 1) : 0) set_text(v, std::to_string(g_cfg.volume) + "%");
}

void HOOK___18AudioOptionsScreeni(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0];
    orig(c);
    u32 ret = c.r[2].w[0], list = self + AUDIO_SCROLL;
    pc_call(fn::ScrollSetVisibleArea, list, menuitem_hash(0), menuitem_hash(6));
    pc_call(fn::ScrollAddObject, list, menuitem_hash(0));
    pc_call(fn::ScrollAddObject, list, master_value_hash());
    SW(list + 20, 0);                       // the mask starts on the first row: the list only knows MENUITEM_1 on
    master_show();
    c.r[2].d[0] = sx32(ret);
}

void HOOK_HandleEvent__18AudioOptionsScreeniP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    u32 item = msg <= 10 ? LW(data) : 0;
    if (item != menuitem_hash(0)) {
        orig(c);
        if (msg == 8 && item == menuitem_hash(1)) SW(data + 8, 0);     // the game's list stopped here
        return;
    }
    if (msg == 6 || msg == 7) {
        int v = g_cfg.volume + (msg == 6 ? -10 : 10);
        g_cfg.volume = v < 0 ? 0 : v > 100 ? 100 : v;
        pc_config_save();
        master_show();
        SW(data + 12, msg == 6 ? 1 : 2);
        return;
    }
    if (msg == 1) return;
    orig(c);
    if (msg == 8) SW(data + 8, 1);          // the top of the list
    if (msg == 4) {
        blurb_text(self + AUDIO_BLURB, MASTER_BLURB);
        pc_call(fn::BlurbAnimate, self + AUDIO_BLURB);
    }
}

// ---- Main Menu: Exit --------------------------------------------------------------------
// The bundle has rows 1..3 here (55 apart). The game finds a row by its names: the menu takes
// MENUITEM_n, the slide-in MENUITEM_n_GROUP and MI_n_MASK, the selection a table that already
// lists MI_4_MASK. So the fourth row is just its objects, made as FRONTB loads.
static void add_exit_row(u32 project)
{
    u32 s = find_screen(project, MAIN_MENU_SCREEN);
    u32 item = s ? find_object(s, menuitem_hash(3), 1) : 0;
    u32 mask = s ? find_object(s, pc_hash("MI_3_MASK"), 2) : 0;
    if (!item || !mask || find_object(s, menuitem_hash(4), 1)) return;
    u32 group = pc_hash("MENUITEM_4_GROUP");
    u32 t = clone_object(s, item);
    SW(t + 8, menuitem_hash(4));
    SW(t + 12, group);
    set_fl(t + 48, 152.0f);                 // as rows 1 and 2 (row 3 is 2 to the right)
    set_fl(t + 52, 236.0f);
    set_text(t, "Exit");
    u32 m = clone_object(s, mask);
    SW(m + 8, pc_hash("MI_4_MASK"));
    SW(m + 12, group);
    set_fl(m + 52, 238.0f);
    // the panel under row 3 (a 16-pixel texture, 113 high) moves under row 4; a thin bar stays
    u32 ol = s + 32;
    for (u32 o = LW(ol); o && o != ol; o = LW(o))
        if (LW(o + 8) == 0xaa5d7d3f && (s8)LB(o + 40) == 2 && fl(o + 60) > 1.0f) {
            u32 panel = clone_object(s, o);
            set_fl(panel + 52, 283.0f);
            set_fl(panel + 60, fl(o + 60) - 55.0f / 16.0f);
            set_fl(o + 60, 10.0f / 16.0f);
            SB(o + 43, 5);
            break;
        }
}

// ---- Back, for the mouse ------------------------------------------------------------------
// The screens that show the help button at the bottom get the back button beside it: its icon and a word, clones of those two. The icon is the
// help character of Triangle, so it shows Esc, B or Triangle by the device (pcplatform.cpp).
static u32 back_text_hash() { return pc_hash("PC_BACK_TEXT"); }

static void add_back_hints(u32 project)
{
    u32 sl = project + 12;
    for (u32 s = LW(sl); s && s != sl; s = LW(s)) {
        if (LW(s + 8) == MAIN_MENU_SCREEN || find_object(s, back_text_hash(), 1)) continue;
        u32 icon = 0, word = 0, ol = s + 32;
        for (u32 o = LW(ol); o && o != ol; o = LW(o)) {
            if ((s8)LB(o + 40) != 1) continue;
            if (LW(o + 96) == 12000) word = o;
            else if (!LW(o + 96) && fl(o + 52) > 400.0f && pc_str(o + 100) == "k") icon = o;
        }
        if (!icon || !word) continue;
        u32 i = clone_object(s, icon);
        SW(i + 8, pc_hash("PC_BACK_ICON"));
        set_fl(i + 48, fl(icon + 48) + 88.0f);
        set_text(i, "j");
        u32 t = clone_object(s, word, 16);      // Back, Tillbaka, Indietro
        SW(t + 8, back_text_hash());
        SW(t + 96, 0);
        set_fl(t + 48, fl(word + 48) + 88.0f);
        set_text(t, "Back");
    }
}

// ---- hooks -----------------------------------------------------------------------------
// HP2_FLDUMP=<file>: every Flailer project as it is loaded, one line per object
static void on_project_loaded(u32 tail)
{
    if (const char *path = getenv("HP2_FLDUMP"))
        if (FILE *f = fopen(path, "a")) {
            dump_project(f, tail);
            fclose(f);
        }
    if (LW(tail + 8) != FRONTB_PROJECT) return;
    g_page_alloc = true;
    for (PcPage *pg : pages()) build_page(tail, *pg);
    add_exit_row(tail);
    add_master_row(tail);
    add_back_hints(tail);
    add_options_arrows(tail);
    g_page_alloc = false;
}

// The pause menu in the middle of the picture with the logo over it: on the PS2 it runs up to the right edge, the logo stands aside to the left.
// In Flailer's 640 across: the panel is 170 + 480 wide, the logo 296 x 0.6.
static void center_pause_menu(u32 project)
{
    if (getenv("HP2_NATIVEEXACT")) return;
    u32 screen = find_screen(project, PAUSE_SCREEN);
    if (!screen) return;
    u32 list = screen + 32;
    for (u32 o = LW(list); o && o != list; o = LW(o)) {
        if ((s8)LB(o + 40) == 2 && LW(o + 80) == PAUSE_LOGO_TEXTURE) set_fl(o + 48, (640 - 296 * fl(o + 56)) / 2);
        else if (fl(o + 48) >= 100) set_fl(o + 48, fl(o + 48) - 90);    // not the fade over the whole screen
    }
}

void HOOK_LoaderFlailerChunk__FP6bChunk(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    orig(c);
    if (c.r[2].w[0] != 1) return;           // not a Flailer chunk
    u32 tail = LW(gp() - 22420);            // the project just appended
    if (tail != gp() - 22424) {
        on_project_loaded(tail);
        center_pause_menu(tail);
    }
}

void HOOK_UnloaderFlailerChunk__FP6bChunk(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    orig(c);
    if (!find_project(FRONTB_PROJECT)) free_pages();
}

// the screens the game makes from its configs: ours go in before the screen list is built
void HOOK___17FlailerScreenList(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    static bool done;
    if (!done) {
        done = true;
        pc_settings_register();
        for (PcPage *pg : pages()) {
            u32 cfg = galloc(56);
            pc_call(fn::ConfigCtor, cfg, gstr_keep(pg->name), fn::OptionsCallback, page_hash(*pg), 0);
        }
    }
    orig(c);
}

// the Options screen and our pages are tOptionsScreen objects
void HOOK___14tOptionsScreeni(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0], hash = c.r[5].w[0];
    orig(c);
    u32 ret = c.r[2].w[0];
    if (hash == OPTIONS_SCREEN) {
        // NTSC hides item 7 (Languages): all seven slots are the port's rows
        pc_call(fn::GrayMenuItem, menuitem_hash(7), 1, hash);
        // the scroll stays: coming back, the game puts the highlight on the slot it had
        options_refresh();
    } else if (PcPage *pg = page_by_hash(hash)) {
        // the constructor hides MENUITEM_7 (NTSC has no language row): a page with seven rows needs it
        if (pg->rows.size() >= 7) pc_call(fn::GrayMenuItem, menuitem_hash(7), 1, hash);
        if (pg->on_open) {
            pg->on_open(*pg);
            pg->top = 0;
            page_shape(*pg);
        }
        g_current_page = pg;
        g_current_self = self;
        pg->cur = 0;
        page_refresh(*pg);
    }
    (void)self;
    c.r[2].d[0] = sx32(ret);
}

void HOOK_HandleEvent__14tOptionsScreeniP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    u32 hash = LW(self);
    if (PcPage *pg = page_by_hash(hash)) {
        if (page_event(*pg, self, msg, data)) return;
    } else if (hash == OPTIONS_SCREEN && options_event(self, msg, data, c, orig)) {
        return;
    }
    orig(c);
}

// Display: "Calibrate Display" lines the picture up on a TV (MENUITEM_9, the list's last)
void HOOK___21tDisplayOptionsScreeni(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 hash = c.r[5].w[0];
    orig(c);
    u32 ret = c.r[2].w[0];
    pc_call(fn::GrayMenuItem, menuitem_hash(9), 2, hash);
    c.r[2].d[0] = sx32(ret);
}

void HOOK_SetInputMode__6Player16PlayerInputModes(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    g_player_mode = c.r[5].w[0];
    orig(c);
    pc_paint_buttons();
}

// the game's OnHighlight and Select know rows 1..3 only
void HOOK_HandleEvent__11NewMainMenu13ScreenMessageP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    if ((msg != 1 && msg != 4) || LW(data) != menuitem_hash(4)) {
        orig(c);
    } else if (msg == 1) {
        window_quit();
    } else {
        pc_call(fn::SetHighlightItem, self + 424, LW(data));
        pc_call(fn::MainMenuHighlight, self, pc_hash("MI_4_MASK"), 10009);      // its help line is replaced next
        blurb_text(self + 4, "Close the game.");
    }
}

// ---- the mouse in the menus ------------------------------------------------------------
// Pointing moves the selection, a left click is Enter, anywhere on a screen without rows; the right button does nothing, as in the PC NFS games.
// It goes through the pad, a press at a time, so that the game's own code selects, sounds and skips what is greyed out.
bool window_mouse(float &x, float &y, bool &left);
int window_wheel();

static int g_mouse_out, g_mouse_wait;

int pc_mouse_buttons() { return g_mouse_out; }

// the object of a menu item: by the screen's hash, in whichever project has it
static u32 find_menu_screen(u32 hash)
{
    u32 list = gp() - 22424;
    for (u32 p = LW(list); p && p != list; p = LW(p))
        if (u32 s = find_screen(p, hash)) return s;
    return 0;
}

static void mouse_frame()
{
    static float last_x = -1, last_y = -1;
    static bool last_left;
    static int pending = -1, tries;
    static bool click;
    float x = 0, y = 0;
    bool left = false;
    bool inside = window_mouse(x, y, left);
    bool pressed = left && !last_left;
    last_left = left;
    if (g_mouse_wait > 0) {                 // a press, then a release: the game takes the edge
        if (--g_mouse_wait <= 2) g_mouse_out = 0;
    }
    if (pc_car_input()) {
        pending = -1, click = false;
        return;
    }
    bool moved = inside && (x != last_x || y != last_y);
    if (inside) last_x = x, last_y = y;

    // A question over the screen (quit the race, restart it, and the like): a tPopupMenu, FlailerScreenList +392, with two or three buttons
    // side by side that are no menu's rows. The pointer on a button highlights it the popup's own way, a click on one is Cross on it; a click
    // beside them does nothing. bIsActive +0, Config.nButtons +4, Config.PopupScreenHash +16, MenuItems +76, CurrentMenuIndex +92.
    if (u32 lists = LW(gp() - 21324))
        for (int k = 0; k < 2; k++) {
            const u32 pop = LW(lists + 392 + 4 * k);
            if (!pop || !LB(pop)) continue;
            pending = -1, click = false;
            const u32 pchunk = find_menu_screen(LW(pop + 16));
            const int buttons = std::min<int>((int)LW(pop + 4), 3);
            int over = -1;
            for (int b = 0; b < buttons && pchunk; b++) {
                const u32 item = LW(pop + 76 + 4 * b);
                u32 o = item ? find_object(pchunk, LW(item + 12)) : 0;
                if (!o) o = find_object(pchunk, 0xe0105663u + (u32)b);      // POPUP_1, POPUP_2, POPUP_3
                if (!o || (s8)LB(o + 41) != 1) continue;
                // a button's text is centred on its place: Flailer's 640 x 480 in the 512 x 448, as below
                const float bx = fl(o + 48) * 0.8f, by = fl(o + 52) * (448.0f / 480.0f) + 15.0f;
                if (inside && std::fabs(x - bx) < 80 && std::fabs(y - by) < 24) over = b;
            }
            if (over >= 0 && (moved || pressed) && (u32)over != LW(pop + 92)) {
                pc_call(0x16b5f0, pop);         // tPopupMenu::UnHighlightCurrentMenuItem
                SW(pop + 92, (u32)over);
                pc_call(0x16b618, pop);         // tPopupMenu::HighlightCurrentMenuItem
            }
            if (pressed && over >= 0 && g_mouse_wait <= 0) {
                g_mouse_out = 1 << PCB_CROSS;
                g_mouse_wait = 4;
            }
            return;
        }

    u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0;
    u32 screens = node ? node + 8 : 0, screen = screens ? LW(screens) : 0;
    u32 menu = screen && screen != screens ? LW(screen + 8) : 0;
    u32 chunk = menu ? find_menu_screen(LW(menu + 20)) : 0;
    int count = 0, current = -1, hovered = -1;
    if (chunk) {
        float best = 1e9f;
        for (u32 item = LW(menu); item && item != menu; item = LW(item), count++) {
            if (item == LW(menu + 8)) current = count;
            u32 o = find_object(chunk, LW(item + 12));
            if (!o || (s8)LB(o + 41) != 1) continue;
            // Flailer's 640 x 480 in the 512 x 448, measured on the screens: the middle of a text's line
            float ix = fl(o + 48) * 0.8f, iy = fl(o + 52) * (448.0f / 480.0f) + 15.0f;
            float d = y - iy;
            if (d < 0) d = -d;
            if (inside && x >= ix - 24 && d < 22 && d < best) best = d, hovered = count;
        }
    }
    if (moved && hovered >= 0) pending = hovered, tries = count + 2;
    // the back button at the bottom: Flailer's 640 x 480 in the 512 x 448, as above
    bool on_back = false;
    if (u32 b = chunk ? find_object(chunk, back_text_hash(), 1) : 0)
        if ((s8)LB(b + 41) == 1) {
            const float bx = fl(b + 48) * 0.8f, by = fl(b + 52) * (448.0f / 480.0f);
            on_back = inside && x >= bx - 30 && x < bx + 44 && y >= by - 8 && y < by + 20;
            const u32 color = on_back ? 0x80004080u : 0x80808080u;
            if (LW(b + 92) != color) SW(b + 92, color), SB(b + 43, 7);
        }
    // and Help, the screen's own word (LanguageID 12000) with its icon to the left of it
    bool on_help = false;
    if (chunk)
        for (u32 o = LW(chunk + 32); o && o != chunk + 32; o = LW(o))
            if ((s8)LB(o + 40) == 1 && LW(o + 96) == 12000 && (s8)LB(o + 41) == 1) {
                const float hx = fl(o + 48) * 0.8f, hy = fl(o + 52) * (448.0f / 480.0f);
                on_help = inside && x >= hx - 30 && x < hx + 40 && y >= hy - 8 && y < hy + 20;
                const u32 color = on_help ? 0x80004080u : 0x80808080u;
                if (LW(o + 92) != color) SW(o + 92, color), SB(o + 43, 7);
                break;
            }
    if (pressed && (on_back || on_help)) {
        if (g_mouse_wait <= 0) {
            g_mouse_out = 1 << (on_back ? PCB_TRIANGLE : PCB_CIRCLE);
            g_mouse_wait = 4;
            pending = -1;
        }
    } else if (pressed && inside) {
        // the arrows of a row's value
        static u32 left_arrow, right_arrow;
        if (!left_arrow) left_arrow = pc_hash("LEFT_ARROW"), right_arrow = pc_hash("RIGHT_ARROW");
        int arrow = 0;
        if (chunk)
            for (int side = 0; side < 2 && !arrow; side++) {
                u32 o = find_object(chunk, side ? right_arrow : left_arrow);
                if (!o || (s8)LB(o + 41) != 1) continue;
                float ax = fl(o + 48) * 0.8f, ay = fl(o + 52) * (448.0f / 480.0f) + 6.0f;
                if (x > ax - 16 && x < ax + 24 && y > ay - 16 && y < ay + 16) arrow = side ? 2 : 1;
            }
        if (arrow && g_mouse_wait <= 0) {
            g_mouse_out = 1 << (arrow == 1 ? PCB_LEFT : PCB_RIGHT);
            g_mouse_wait = 4;
        } else if (!arrow && (hovered >= 0 || !count)) {
            click = true;
            if (hovered >= 0) pending = hovered, tries = count + 2;
        }
    }
    if (g_mouse_wait > 0) return;
    if (press_up_soon) {
        press_up_soon = false;
        g_mouse_out = 1 << PCB_UP;
        g_mouse_wait = 4;
        return;
    }
    if (const int wheel = window_wheel()) {     // the wheel moves the selection, a list with it
        g_mouse_out = 1 << (wheel > 0 ? PCB_UP : PCB_DOWN);
        g_mouse_wait = 4;
        pending = -1;
        return;
    }
    if (pending >= 0 && count && current >= 0 && pending != current && tries > 0) {
        int down = (pending - current + count) % count;
        g_mouse_out = 1 << (down <= count / 2 ? PCB_DOWN : PCB_UP);
        g_mouse_wait = 4;
        tries--;
        return;
    }
    if (click && (pending < 0 || !count || pending == current)) {
        g_mouse_out = 1 << PCB_CROSS;
        g_mouse_wait = 4;
        pending = -1;
    }
    click = false;
    // Reached, or given up: from here the selection is the keys' again, until the mouse moves.
    if (pending == current || tries <= 0) pending = -1;
}

// ---- Player Names, with the keyboard, for both players --------------------------------------
// The game answers each row only from its own player's pad and types on its letter grid. Here any device has every row. With the keyboard or the mouse in use
// the name is typed on the keyboard; with a gamepad the game's letter grid opens, as on the PS2, and any pad works it for either player.
// The names live in Fred (0x2fc050): at +0x10d90, 1744 bytes on for player 2. tKeyboard's limit stays: 7 characters, which the race's HUD has room for.
static const u32 NAME_ROW_1 = 0x52e039f3, NAME_RED = 0x71450a81, NAME_BLUE = 0x71572302;
static const int NAME_BLURB = 356, NAME_MAX = 7;
static const u32 kFredNames = 0x2fc050 + 0x10d90;
static bool g_name_typing;
static bool g_name_whole;               // the name as it was is taken by the first letter, as a selected text is: the game's names fill all 7 places
static int g_name_player;
static std::string g_name_text;

static u32 name_object(u32 self, int player)
{
    u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, LW(self + 4)) : 0;
    return s ? find_object(s, player ? NAME_BLUE : NAME_RED, 1) : 0;
}

static std::string player_name(int player) { return pc_str(kFredNames + (u32)player * 1744); }

void HOOK_HandleEvent__9NameEntry13ScreenMessageP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    if (g_name_typing) {
        // the keyboard is the name's; the screen's animations go on
        if (msg != 18) {
            if (msg <= 10) SW(data + 8, 1);
            return;
        }
        orig(c);
        bool done = false, keep = false, typed = false;
        while (const int ch = window_text_next()) {
            typed = true;
            if (ch == 13) done = keep = true;
            else if (ch == 27) done = true;
            else if (ch == 8) {
                g_name_whole = false;
                if (!g_name_text.empty()) g_name_text.pop_back();
            } else {
                if (g_name_whole) g_name_text.clear();
                g_name_whole = false;
                if ((int)g_name_text.size() < NAME_MAX) g_name_text += (char)ch;
            }
        }
        if (!done) {
            if (pc_text_cursor_changed() || typed)
                if (u32 o = name_object(self, g_name_player)) set_text(o, g_name_text + pc_text_cursor(LW(o + 80)));
            return;
        }
        g_name_typing = false;
        window_text_end();
        if (keep && !g_name_text.empty()) {
            u8 *name = host(kFredNames + (u32)g_name_player * 1744);
            memset(name, 0, 11);
            memcpy(name, g_name_text.data(), g_name_text.size());
        }
        if (u32 o = name_object(self, g_name_player)) set_text(o, player_name(g_name_player));
        blurb_text(self + NAME_BLURB, keep ? "The name is set." : "The name stays as it was.");
        pc_call(fn::BlurbAnimate, self + NAME_BLURB);
        return;
    }
    if (msg <= 10 && LW(self)) SW(data + 4, (u32)g_name_player);     // the letter grid is up: it hears the player whose name it is
    if (msg <= 10 && !LW(self)) {
        const u32 row = LW(data) - NAME_ROW_1;     // 0 and 1 player 1's name and gender, 2 and 3 player 2's
        if (row < 4) {
            const int player = row >= 2 ? 1 : 0;
            SW(data + 4, (u32)player);              // the game's test of the pad: it is that player's
            if (msg == 1 && !(row & 1)) g_name_player = player;
            if (msg == 1 && !(row & 1) && window_input_kind() != 1) {
                g_name_typing = true;
                g_name_whole = true;
                g_name_player = player;
                g_name_text = player_name(player);
                if ((int)g_name_text.size() > NAME_MAX) g_name_text.resize(NAME_MAX);
                window_text_begin();
                if (u32 o = name_object(self, player)) set_text(o, g_name_text + pc_text_cursor(LW(o + 80)));
                blurb_text(self + NAME_BLURB, "Type a new name, Backspace edits this one. Enter sets it, Esc leaves it.");
                pc_call(fn::BlurbAnimate, self + NAME_BLURB);
                return;
            }
        }
    }
    orig(c);
}

// ---- saving and loading, at the speed of a disc ---------------------------------------------
// The card's screens wait seconds on a timer so that a message can be read and the PS2's card can catch up. Here the timers have always run out, the messages
// that only say what is going on do not show, a file already there is overwritten, and "Saved" is a word in the corner. Errors and questions stay.
static const u32 CardStartWrite = 0x246b10;

void HOOK_HandleEvent__12MemcardSaver13ScreenMessageP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 self = c.r[4].w[0];
    if (c.r[5].w[0] == 18) SW(self + 12, LW(gp() - 19608) - 40000);
    orig(c);
    const u32 ret = c.r[2].w[0];
    if (LW(self + 16) == 7) pc_call(CardStartWrite, self);      // the overwrite question was not put
    c.r[2].d[0] = sx32(ret);
}

void HOOK_HandleEvent__13MemcardLoader13ScreenMessageP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    if (c.r[5].w[0] == 18) SW(c.r[4].w[0] + 12, LW(gp() - 19608) - 40000);
    orig(c);
}

void HOOK_DisplayMessage__15MemcardPrompteriii(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    switch (c.r[5].w[0]) {
    case 151005:                        // Saved
        native_notice_saved();
        return;
    case 151001: case 151002: case 151003: case 151004: case 151013:    // Loading, Loaded, no save file, Saving, Checking
        return;
    }
    orig(c);
}

void HOOK_DisplayYesNoMessage__15MemcardPrompteriiiii(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    if (c.r[5].w[0] == 151020) return;  // overwrite the file? It is this profile's
    orig(c);
}

// once per frame in the frontend: the test hook HP2_PCGOTO=<screen name>[,<tick>]
void HOOK_Tick__17FlailerScreenList(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    static const char *go = getenv("HP2_PCGOTO");
    static int tick;
    orig(c);
    pc_language_apply();                    // hp2.ini's language, once the frontend runs
    pc_paint_buttons();
    mouse_frame();
    for (auto &f : g_frame_fns) f();
    if (g_pc_ui_dirty.exchange(false)) pc_refresh_page();
    if (go && ++tick == (strchr(go, ',') ? atoi(strchr(go, ',') + 1) : 900)) {
        std::string name(go, strchr(go, ',') ? (size_t)(strchr(go, ',') - go) : strlen(go));
        pc_call(fn::ChangeToScreen, gstr(name), 1);
    }
}
