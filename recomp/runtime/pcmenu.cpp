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
#include <chrono>
#include <cmath>

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

// the same with one float argument after the first integer one (f12)
static void pc_call_f(u32 fn, u32 a0, float f12)
{
    Ctx &c = *g_pc_ctx;
    Ctx saved = c;
    c.r[4].d[0] = sx32(a0);
    c.f[12].f = f12;
    c.r[29].d[0] -= 0x200;
    ps2_call(c, fn);
    c = saved;
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
const u32 SetY = 0x140a48;                  // FlailerObjectChunk::SetY(chunk, y), a float: what the game's scrolling list moves a row with
const u32 SetAlpha = 0x1406d8;              // FlailerObjectChunk::SetAlpha(chunk, 0..128): a text's or a bitmap's
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

static void g_row_home_clear();
static void free_pages()
{
    for (u32 p : g_page_mem) pc_call(fn::bFree, p);
    g_page_mem.clear();
    g_texts.clear();
    g_row_home_clear();
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
// Before a race starts and after it ends the game waits for JOY_EVENT_SKIP, which is Cross in every layout (Player::SetInputMode,
// 0x1776b0: modes 5 and 6 add PreRaceJoyHandler and FinishRaceJoyHandler for it; 2 and 7 are the race itself).
bool pc_skip_wanted() { return pc_racing() && (g_player_mode == 5 || g_player_mode == 6); }
bool pc_two_player_race() { return pc_racing() && LW(gp() - 20920) == 2; }     // Player::NumPlayers
bool pc_two_player_screen() { return g_two_player_screens != 0; }

void HOOK_SetTwoPlayerScreenMode__Fi(Ctx &c, void (*orig)(Ctx &))
{
    g_two_player_screens = c.r[4].w[0];
    orig(c);
}

// ReplayManager (gp-22440): +0 it plays back
// The ReplayManager's first word is set on the frames a replay shows and clear on the frames between (it goes 1, 0, 1, 0 while the replay
// screen is up): a replay is on from one such frame until a quarter of a second without any. Asked as it was, the pointer came and went
// with it, frame by frame.
bool pc_replay_playing()
{
    static std::chrono::steady_clock::time_point seen;
    static bool ever;
    if (!pc_racing()) return ever = false;
    const u32 rm = LW(gp() - 22440);
    const auto now = std::chrono::steady_clock::now();
    if (rm && LW(rm) != 0) seen = now, ever = true;
    return ever && now - seen < std::chrono::milliseconds(250);
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

static float row_home(u32 obj);

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
        if (u32 v = value_object(s, k)) {
            set_text(v, r.value ? r.value() : "");
            pc_call(fn::SetAlpha, v, r.disabled && r.disabled() ? 40 : 128);   // a greyed row's value goes dim with it
        }
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
        if (arrows && item) set_fl(a + 52, row_home(item) + 8.0f), SB(a + 43, 4);
        pc_call(fn::SetVisibility, a, arrows && item ? 1 : 0);
    }
}

static PcPage *g_current_page;
static u32 g_current_self;      // the page's tOptionsScreen object

// ---- a list that scrolls, as Audio's does --------------------------------------------------
// The game's own list (tScrollingMenuList) slides its rows to their new places and pulses its two arrows' alpha by a sine
// (tfeSineWave: up to 128, 10 radians a second). The port's lists are seven slots whose texts change, so the same is shown with what
// they have: after a step the seven rows start a row away from their places and slide home, the one that comes in fading up, and the
// arrows pulse while they show.
static std::map<u32, float> g_row_home;     // a row's text or value: its own Y on the screen
static float g_slide;                       // rows: where the texts are from home now, in rows; 0 at rest
static double g_slide_from;                 // when the slide began, seconds
static int g_slide_dir;
static const float kRowStep = 37.0f;        // between two rows of the Options screen
static const double kSlideSeconds = 0.2;

static void g_row_home_clear() { g_row_home.clear(); }

static double menu_seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static float row_home(u32 obj)
{
    auto it = g_row_home.find(obj);
    if (it == g_row_home.end()) it = g_row_home.emplace(obj, fl(obj + 52)).first;
    return it->second;
}

// the list on the screen stepped a row: down, and the rows come up from below
static void slide_start(bool down)
{
    g_slide_dir = down ? 1 : -1;
    g_slide = (float)g_slide_dir;
    g_slide_from = menu_seconds();
}

// each frame of the frontend: the rows of the screen that is up to where the slide has them, and its arrows' pulse
static void grid_typed_frame();
static void list_frame()
{
    // the tOptionsScreen that is live (gp-16888) is a page's or the Options screen's own; its first word is its screen's hash
    const u32 live = LW(gp() - 16888), hash = live ? LW(live) : 0;
    PcPage *pg = hash ? page_by_hash(hash) : nullptr;
    u32 project = find_project(FRONTB_PROJECT), s = project && (pg || hash == OPTIONS_SCREEN) ? find_screen(project, hash) : 0;
    static u32 sliding;                     // the screen whose rows are away from home
    if (sliding && sliding != s) {          // it went while they were: home, for when it comes back
        for (int k = 1; k <= 7; k++)
            for (u32 o : {find_object(sliding, menuitem_hash(k), 1), value_object(sliding, k)})
                if (o) pc_call_f(fn::SetY, o, row_home(o));
        sliding = 0;
        g_slide = 0;
    }
    if (!s) {
        g_slide = 0;
        return;
    }
    const double now = menu_seconds();
    const int alpha = 32 + (int)(96.0 * (1.0 + sin(now * 10.0)) * 0.5);
    for (u32 h : {SCROLL_UP_ARROW, SCROLL_DOWN_ARROW})
        if (u32 a = find_object(s, h, 2)) pc_call(fn::SetAlpha, a, (u32)alpha);
    if (g_slide == 0) return;
    float t = (float)((now - g_slide_from) / kSlideSeconds);
    if (t >= 1) t = 1;
    const float left = (1 - t) * (1 - t) * (1 - t);     // fast, then easing in: the cubic of the game's own
    g_slide = t >= 1 ? 0 : left * (float)g_slide_dir;
    sliding = g_slide != 0 ? s : 0;
    const int entering = g_slide_dir > 0 ? 7 : 1;
    for (int k = 1; k <= 7; k++) {
        u32 objs[2] = {find_object(s, menuitem_hash(k), 1), pg ? value_object(s, k) : 0};
        for (u32 o : objs) {
            if (!o) continue;
            pc_call_f(fn::SetY, o, row_home(o) + g_slide * kRowStep);   // not the field alone: the text is laid out again only when told
        }
        if (k == entering && objs[1]) {
            const PcRow *r = pg && pg->top + k - 1 < (int)pg->rows.size() ? &pg->rows[(size_t)(pg->top + k - 1)] : nullptr;
            const int full = r && r->disabled && r->disabled() ? 40 : 128;
            pc_call(fn::SetAlpha, objs[1], (u32)(full * (1 - left)));
        }
    }
}

// The slots' states as GrayMenuItem takes them: 1 as usual, 0 greyed (a row that is disabled now), 2 hidden (no row). Told to the game only
// when one changes: a row can go grey or come back by what another row was set to.
static void page_gray(PcPage &pg, bool all)
{
    static u32 last_hash;
    static int last[8];
    const u32 hash = page_hash(pg);
    if (hash != last_hash) all = true;
    last_hash = hash;
    const int total = (int)pg.rows.size();
    for (int k = 1; k <= 7; k++) {
        int mode = 2;
        if (k <= total) {
            const PcRow &r = pg.rows[(size_t)(pg.top + k - 1 < total ? pg.top + k - 1 : total - 1)];
            mode = r.disabled && r.disabled() ? 0 : 1;
        }
        if (!all && last[k] == mode) continue;
        last[k] = mode;
        pc_call(fn::GrayMenuItem, menuitem_hash(k), (u32)mode, hash);
    }
}

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

int pc_page_row(const char *name)
{
    PcPage *pg = g_current_page;
    if (!pg || pg->name != name || !g_current_self || LW(gp() - 16888) != g_current_self || pg->cur < 1) return -1;
    const int row = pg->top + (int)pg->cur - 1;
    return row < (int)pg->rows.size() ? row : -1;
}

// ---- a question with Yes and No ------------------------------------------------------------------------
// FlailerScreenList::SummonPopup (0x16cd68) with a PopupConfig as MemcardPrompter::DisplayYesNoMessage (0x245f60) fills it: two buttons,
// flags 5, the screen that is up as the parent, the memory card's popup screen (0x0ce3f7ce), the message as a string, the buttons' texts
// by their language ids (Yes 150103, No 150104), a parameter for each. The answer comes to the parent's callback as message 21 with the
// parameter at +8 of its data (tPopupMenu::Dismiss, 0x16b9c0): page_event hands it on.
// The popup's screen is the memory card's, made to stand over an empty one: its panel is drawn before a page's own objects, which would
// show through its text. So the page's objects that are visible (+41) are hidden while it is up, and shown again with the answer.
static std::function<void(bool)> g_answer;
static bool g_asking;
static u32 g_ask_mem[2];
static std::vector<u32> g_under_question;

static void ask_free()
{
    for (u32 &m : g_ask_mem) {
        if (m) pc_call(fn::bFree, m);
        m = 0;
    }
}

static void page_under_question(bool hide)
{
    if (!hide) {
        for (u32 o : g_under_question) pc_call(fn::SetVisibility, o, 1);
        g_under_question.clear();
        return;
    }
    PcPage *pg = g_current_page;
    u32 project = find_project(FRONTB_PROJECT), scr = project && pg ? find_screen(project, page_hash(*pg)) : 0;
    // not a page of the port's: the main menu, whose Exit asks (its rows are drawn over the popup's panel as a page's are)
    if (!scr || !g_current_self || LW(gp() - 16888) != g_current_self) scr = project ? find_screen(project, MAIN_MENU_SCREEN) : 0;
    if (!scr) return;
    for (u32 o = LW(scr + 32); o && o != scr + 32; o = LW(o))
        if (LB(o + 41)) {
            g_under_question.push_back(o);
            pc_call(fn::SetVisibility, o, 0);
        }
}

bool pc_asking() { return g_asking; }

// the answer, from the screen the question stood over: message 21, the button's parameter at +8 of its data
static void ask_answer(u32 data)
{
    if (!g_asking) return;
    g_asking = false;
    page_under_question(false);
    ask_free();
    auto answer = std::move(g_answer);
    g_answer = nullptr;
    if (answer) answer(LW(data + 8) == 1);
}

void pc_ask(const std::string &title, const std::string &question, std::function<void(bool yes)> answer, const char *yes, const char *no)
{
    const u32 list = LW(gp() - 21324);
    if (g_asking || !list) return;
    // the game's heap, given back with the answer: a block left in it is in the way of the race's large ones
    const u32 cfg = pc_alloc(72), text = pc_alloc(256);
    if (!cfg || !text) return;
    g_ask_mem[0] = cfg, g_ask_mem[1] = text;
    const std::string &q = tr(question);
    memcpy(host(text), q.c_str(), std::min<size_t>(q.size(), 255));
    SW(cfg + 0, 2);                 // nButtons
    SW(cfg + 4, 7);                 // Flags: 5 as the prompter's, and 2: Back leaves with CancelParam (tPopupMenu::HandleJoy, 0x16bac0)
    SW(cfg + 8, LW(list + 412));    // ParentScreen: the screen that is up
    SW(cfg + 12, 0x0ce3f7ce);       // PopupScreenHash
    SW(cfg + 16, text);             // MessageString
    SW(cfg + 20, 150103);           // Yes
    SW(cfg + 24, 150104);           // No
    SW(cfg + 32, 1);                // Button1Param
    SW(cfg + 36, 2);                // Button2Param
    SW(cfg + 44, 2);                // CancelParam
    SW(cfg + 48, 2);                // DismissParam
    SW(cfg + 52, 1);                // StartButton: No has the highlight
    SW(cfg + 56, (u32)-1);          // JoyPort: any
    SW(cfg + 68, 150004);           // Title, the prompter's own
    g_answer = std::move(answer);
    g_asking = true;
    if ((s32)pc_call(0x16cd68, list, cfg) < 0) {    // no popup free
        g_asking = false;
        g_answer = nullptr;
        ask_free();
        return;
    }
    page_under_question(true);
    // the title is a language id in the config ("Saved Game"); the popup's screen has it as a text by now, which takes the asker's
    if (u32 project = find_project(FRONTB_PROJECT))
        if (u32 scr = find_screen(project, 0x0ce3f7ce))
            for (u32 o = LW(scr + 32); o && o != scr + 32; o = LW(o))
                if ((s8)LB(o + 40) == 1) {
                    const u32 id = LW(o + 96);
                    if (id == 150004) pc_call(fn::SetString, o, gstr(tr(title)));
                    if (id == 150103 && yes) pc_call(fn::SetString, o, gstr(tr(yes)));     // the buttons' words, when they are not Yes and No
                    if (id == 150104 && no) pc_call(fn::SetString, o, gstr(tr(no)));
                }
}

void pc_ask_exit()
{
    pc_ask("Exit", "Leave the game?", [](bool yes) {
        if (yes) window_quit();
    }, "Exit", "Cancel");
}

static bool press_up_soon;

// the rows that are there show, the others go; and the Back at the bottom
static u32 back_text_hash();
static u32 g_grid_kb;
static void page_shape(PcPage &pg)
{
    const u32 hash = page_hash(pg);
    page_gray(pg, true);
    u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, hash) : 0;
    if (!s) return;
    for (const char *name : {"PC_BACK_ICON", "PC_BACK_TEXT"})
        if (u32 o = find_object(s, pc_hash(name), 1)) pc_call(fn::SetVisibility, o, pg.hide_back ? 0 : 1);
    if (u32 o = find_object(s, back_text_hash(), 1)) set_text(o, pg.back_text.empty() ? "Back" : pg.back_text);
}

// A page with on_back takes Back itself (the first screen of profiles: Exit). FlailerScreenList +0x190, set through 0x16d448 as the
// Player Names screen does for its grid, makes Back the screen's message 17 and nothing more. Kept each frame: the page's while it is
// the screen that is up, with no question over it and no name being typed (the grid sets and clears the same for itself).
static void page_back_frame()
{
    const u32 list = LW(gp() - 21324);
    if (!list) return;
    static bool mine;
    PcPage *pg = g_current_page;
    const bool live = pg && g_current_self && LW(gp() - 16888) == g_current_self;
    const bool want = live && pg->on_back && !g_grid_kb && !g_asking;
    const u32 kTakeBack = 0x31a6e8, now = LW(list + 0x190);
    if (want) {
        if (now != kTakeBack) pc_call(0x16d448, kTakeBack);
        mine = true;
    } else if (mine) {
        if (!g_grid_kb && now == kTakeBack) pc_call(0x16d448, 0);
        mine = false;
    }
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
// ---- a name typed on a page: the keyboard, or the game's letter grid ------------------------------------------
// The grid is tKeyboard, which the Player Names screen (NameEntry) owns; it finds everything it shows by hash on the screen it is
// given: the group 0x670b6fdd (the panel, its heading, 46 keys, the key's highlight), the seven places of the name being typed
// (P1C1..7, group 0x7b95e55c), the bar under the place being typed (0xc01086b5) and the name's own text (P1NAME). Those are copied to
// the profiles' page when the frontend loads, hidden; a tKeyboard made for the page's hash then works there as on its own screen.
// Its own name is 7 characters. A profile's is 15, so here tKeyboard keeps the grid, its cursor and its two sets of letters, and the
// name is the port's: 15 places (PC_NAME_1.., copies of P1C1 a little closer than the game's 20), the letter at the cursor in the
// game's colour for it, the game's bar under it, pulsed by tKeyboard::Tick as ever. Select on the grid is taken before tKeyboard sees
// it: the key under its cursor (GetChar; ')' is its Done key, '(' its Backspace) goes into the port's name. With the keyboard the grid
// stays hidden and the keys type; the arrows, Home and End move the cursor, Backspace and Delete take a letter.
static const u32 NAME_SCREEN = 0x617a4e0a, GRID_GROUP = 0x670b6fdd, GRID_NAME_GROUP = 0x7b95e55c, GRID_BAR = 0xc01086b5,
                 GRID_NAME = 0x71450a81, GRID_ANCHOR = 0x35b2f256, GRID_PLACE_1 = 0x001a9f14, GRID_PLACE_2P = 0x001aa355;
static const u32 kFredPlayer1Name = 0x2fc050 + 0x10d90;
static const int kNamePlaces = 15;
static const float kNameX = 285.0f, kNameStep = 15.0f;
static u32 g_grid_self;
static bool g_name_armed;                   // the Player Names screen has its grid up, or its name typed
static bool g_grid_typed;                   // the keyboard types: the grid itself is not shown
static std::function<void(const std::string &)> g_grid_done;
static u8 g_grid_kept[11];
static std::vector<u32> g_grid_covered;     // the rows' texts the grid stands on, hidden for it

// a name being typed: the text and where the next letter goes
struct NameEdit {
    std::string text;
    int cur = 0, max = kNamePlaces;
    // a key of window_text_next: a letter, 8 Backspace, 127 Delete, 1 left, 2 right, 3 Home, 4 End
    void key(int ch)
    {
        const int len = (int)text.size();
        if (ch == 8) {
            if (cur > 0) text.erase((size_t)--cur, 1);
        } else if (ch == 127) {
            if (cur < len) text.erase((size_t)cur, 1);
        } else if (ch == 1) {
            if (cur > 0) cur--;
        } else if (ch == 2) {
            if (cur < len) cur++;
        } else if (ch == 3) {
            cur = 0;
        } else if (ch == 4) {
            cur = len;
        } else if (ch >= 32 && ch < 127 && len < max && (ch != ' ' || cur > 0)) {
            text.insert((size_t)cur++, 1, (char)ch);
        }
    }
    std::string name() const
    {
        std::string t = text;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    }
};
static NameEdit g_edit;

static u32 name_place_hash(int i) { return pc_hash(("PC_NAME_" + std::to_string(i + 1)).c_str()); }

static void add_name_grid(u32 project, const std::string &page)
{
    PcPage *pg = pc_find_page(page.c_str());
    u32 s = pg ? find_screen(project, page_hash(*pg)) : 0, from = find_screen(project, NAME_SCREEN);
    if (!s || !from || find_object(s, GRID_ANCHOR, 2)) return;
    std::vector<u32> take;
    for (u32 o = LW(from + 32); o && o != from + 32; o = LW(o)) {
        const u32 h = LW(o + 8), g = LW(o + 12);
        if (g == GRID_GROUP || g == GRID_NAME_GROUP || h == GRID_BAR || h == GRID_NAME) take.push_back(o);
    }
    for (u32 o : take) SB(clone_object(s, o) + 41, 0);
    if (u32 place = find_object(from, GRID_PLACE_1, 1))
        for (int i = 0; i < kNamePlaces; i++) {
            u32 c = clone_object(s, place);
            SW(c + 8, name_place_hash(i));
            SW(c + 12, 0);                          // of no group: tKeyboard shows and hides its own seven
            set_fl(c + 48, kNameX + kNameStep * (float)i);
            SB(c + 41, 0);
        }
    // the game's own places, on the Player Names screen, as close as the port's
    for (u32 first : {GRID_PLACE_1, GRID_PLACE_2P})
        for (u32 i = 0; i < 7; i++)
            if (u32 o = find_object(from, first + i, 1)) set_fl(o + 48, kNameX + kNameStep * (float)i);
}

// the port's places and the bar as the name is now
static void name_show(u32 s, bool on)
{
    const int at = g_edit.cur < kNamePlaces ? g_edit.cur : kNamePlaces - 1;
    u32 lit = 0;
    for (int i = 0; i < kNamePlaces; i++) {
        u32 o = find_object(s, name_place_hash(i), 1);
        if (!o) continue;
        const char ch[2] = {i < (int)g_edit.text.size() ? g_edit.text[(size_t)i] : (char)0, 0};
        pc_call(fn::SetString, o, gstr(ch));
        SW(o + 92, i == at ? 0x80004080u : 0x80808080u);     // tKeyboard::SyncEditWithDisplay's two colours: 128 64 0, and white
        SB(o + 43, 4);
        if ((LB(o + 41) != 0) != on) pc_call(fn::SetVisibility, o, on ? 1 : 0);
        if (i == at) lit = o;
    }
    u32 bar = find_object(s, GRID_BAR, 2);
    if (bar && lit && on) {
        set_fl(bar + 48, fl(lit + 48));
        SB(bar + 43, 4);
        pc_call_f(fn::SetY, bar, fl(lit + 52) + 22.0f);     // under the letter: its height and 2, as the game puts it
    }
}

static u32 grid_screen()
{
    PcPage *pg = g_current_page;
    u32 project = find_project(FRONTB_PROJECT);
    return project && pg ? find_screen(project, page_hash(*pg)) : 0;
}

static void grid_end(bool keep, bool call)
{
    const u32 kb = g_grid_kb;
    if (!kb) return;
    g_grid_kb = 0;
    const std::string name = keep ? g_edit.name() : std::string();
    memcpy(host(kFredPlayer1Name), g_grid_kept, sizeof g_grid_kept);
    if (g_grid_typed) window_text_end();
    g_grid_typed = false;
    if (call) {
        pc_call(0x16d448, 0);
        pc_call(0x24c5d0, kb, 0, 0);                // tKeyboard::SetVisible(off)
        if (u32 s = grid_screen()) {
            name_show(s, false);
            if (u32 o = find_object(s, GRID_NAME, 1)) pc_call(fn::SetVisibility, o, 0);    // tKeyboard shows the name's text as it goes
        }
        for (u32 o : g_grid_covered) pc_call(fn::SetVisibility, o, 1);
    }
    pc_call(fn::bFree, kb);
    g_grid_covered.clear();
    auto done = std::move(g_grid_done);
    g_grid_done = nullptr;
    if (call && done) done(name);
}

// The grid is 140 high: under the rows for a row of the upper four, over them for one further down. The rows it stands on go.
static void grid_cover(u32 s, bool top)
{
    for (int k = top ? 1 : 6; k <= (top ? 4 : 7); k++)
        for (u32 o : {find_object(s, menuitem_hash(k), 1), value_object(s, k)})
            if (o && LB(o + 41)) {
                g_grid_covered.push_back(o);
                pc_call(fn::SetVisibility, o, 0);
            }
}

static const char *const kGridBlurb = "Pick the letters. The last key of the grid makes it.";
static const char *const kTypeBlurb = "Type the name. Enter makes it, Esc leaves it.";

bool pc_name_grid(std::function<void(const std::string &name)> done, bool grid)
{
    PcPage *pg = g_current_page;
    u32 s = grid_screen();
    if (g_grid_kb || !s || !g_current_self || LW(gp() - 16888) != g_current_self || pg->cur < 1) return false;
    u32 row = find_object(s, menuitem_hash((int)pg->cur), 1), anchor = find_object(s, GRID_ANCHOR, 2);
    if (!row || !anchor || !find_object(s, name_place_hash(0), 1)) return false;
    // the name is typed in the row that asked, where the Player Names screen has its values
    const float y = row_home(row) + 2.0f;
    for (int i = 0; i < kNamePlaces; i++)
        if (u32 o = find_object(s, name_place_hash(i), 1)) pc_call_f(fn::SetY, o, y);
    const bool top = pg->cur >= 5;
    if (grid) grid_cover(s, top);
    const u32 kb = galloc(0x100);
    if (!kb) return false;
    pc_call(0x24c458, kb, page_hash(*pg));          // tKeyboard::tKeyboard(screen hash)
    if (top) set_fl(kb + 0xc4, 66.0f);              // where its heading stands: the copy's own place is the Player Names screen's, 272
    memcpy(g_grid_kept, host(kFredPlayer1Name), sizeof g_grid_kept);
    memset(host(kFredPlayer1Name), 0, sizeof g_grid_kept);
    pc_call(0x24c708, kb, 0);                       // tKeyboard::Activate(player 1): its own name stays empty
    // FlailerScreenList +0x190, which NameEntry sets while its grid is up (0x16d448): with it Back is the screen's message 17 and does
    // not also leave the screen
    pc_call(0x16d448, 0x31a6e8);
    g_grid_typed = !grid;
    if (!grid) {
        pc_call(0x1454e8, page_hash(*pg), GRID_GROUP, 0);       // the group's visibility
        window_text_begin();
    }
    g_edit = NameEdit();
    name_show(s, true);
    g_grid_kb = kb;
    g_grid_self = g_current_self;
    g_grid_done = std::move(done);
    pc_set_blurb(grid ? kGridBlurb : kTypeBlurb);
    return true;
}

bool pc_name_grid_up() { return g_grid_kb != 0; }
int pc_name_grid_mode() { return !g_grid_kb ? 0 : g_grid_typed ? 1 : 2; }

static void grid_to_typed();

// the page's event while the name is typed; false: the page's own tick goes on
static bool grid_event(u32 msg, u32 data)
{
    const u32 kb = g_grid_kb;
    if (msg == 18) {
        pc_call(0x24d088, kb, msg, data);           // tKeyboard::HandleEvent: its Tick
        return false;
    }
    if (msg > 17) return true;
    static const bool grid_log = getenv("HP2_GRIDLOG") != nullptr;      // which message each button is
    if (grid_log && msg != 4) fprintf(stderr, "grid: message %u\n", msg);
    if (g_grid_typed) {                             // the keyboard is the name's (grid_typed_frame)
        if (msg <= 10) SW(data + 8, 1);
        return true;
    }
    if (window_input_kind() != 1) {                 // a key: it brings the keyboard back, and is not the grid's
        grid_to_typed();
        if (msg <= 10) SW(data + 8, 1);
        return true;
    }
    u32 s = grid_screen();
    if (msg == 1 || msg == 15 || msg == 16 || msg == 17) {      // Select, the place left and right, Start: the port's name
        const int key = msg == 1 ? (int)(pc_call(0x24cbe0, kb, LW(kb + 0x10), LW(kb + 0x14)) & 0xff) : 0;
        // The grid's last key takes the name. Back (message 17 here: tKeyboard takes the name by it, having no way to leave) leaves
        // it, as Esc does for the keyboard: a profile is made by a key that says so.
        if (msg == 17 || key == ')') {
            SW(data + 8, 0);                        // as tKeyboard answers message 17
            SW(data + 12, 0x1d);                    // the sounds tKeyboard gives each
            grid_end(msg != 17, true);
            return true;
        }
        if (msg == 15 || msg == 16) g_edit.key(msg == 15 ? 1 : 2), SW(data + 12, msg == 15 ? 1 : 2);
        else if (key == '(') g_edit.key(8), SW(data + 12, 0x1a);
        else if (key) g_edit.key(key), SW(data + 12, 0x19);
        if (s) name_show(s, true);
        return true;
    }
    SW(data + 4, 0);                                // any gamepad moves it: the grid hears its player only
    pc_call(0x24d088, kb, msg, data);
    return true;
}

// the keyboard was used while the grid was up: the grid goes, the keys type
static void grid_to_typed()
{
    PcPage *pg = g_current_page;
    u32 s = grid_screen();
    if (pg) pc_call(0x1454e8, page_hash(*pg), GRID_GROUP, 0);
    for (u32 o : g_grid_covered) pc_call(fn::SetVisibility, o, 1);
    g_grid_covered.clear();
    g_grid_typed = true;
    window_text_begin(true);
    if (s) name_show(s, true);
    pc_set_blurb(kTypeBlurb);
}

// each frame: what the keyboard typed into the page's name; Enter takes it, Esc leaves it. The way of typing is the device's that was
// used last: a gamepad's button brings the grid, a key takes it away.
static void grid_typed_frame()
{
    window_text_arm(g_grid_kb != 0 || g_name_armed);
    if (g_grid_kb && !g_grid_typed && window_input_kind() != 1) grid_to_typed();
    if (!g_grid_kb || !g_grid_typed) return;
    if (window_input_kind() == 1) {
        PcPage *pg = g_current_page;
        u32 s = grid_screen();
        window_text_end();                          // and the button that said so is not the grid's first
        g_grid_typed = false;
        if (s && pg) {
            pc_call(0x1454e8, page_hash(*pg), GRID_GROUP, 1);
            grid_cover(s, pg->cur >= 5);
        }
        g_edit.cur = (int)g_edit.text.size();       // the grid types at the end
        if (s) name_show(s, true);
        pc_set_blurb(kGridBlurb);
        return;
    }
    bool typed = false;
    while (const int ch = window_text_next()) {
        if (ch == 13 || ch == 27) {
            grid_end(ch == 13, true);
            return;
        }
        g_edit.key(ch);
        typed = true;
    }
    if (typed)
        if (u32 s = grid_screen()) name_show(s, true);
}

// The same for a name tKeyboard itself holds (the Player Names screen's, 7 characters): the port's text and cursor into its edit text
// (+0x84) and place (+0x7c), shown by its SyncEditWithDisplay (0x24cd40).
static void kb_typed_begin(u32 kb)
{
    pc_call(0x1454e8, LW(kb), GRID_GROUP, 0);
    g_edit = NameEdit();
    g_edit.max = 7;
}

static void kb_typed_show(u32 kb)
{
    memset(host(kb + 0x84), 0, 8);
    memcpy(host(kb + 0x84), g_edit.text.data(), g_edit.text.size());
    SW(kb + 0x7c, (u32)(g_edit.cur < 7 ? g_edit.cur : 6));
    pc_call(0x24cd40, kb);
}

static bool page_event(PcPage &pg, u32 self, u32 msg, u32 data)
{
    if (g_grid_kb && self == g_grid_self) return grid_event(msg, data);
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
            page_gray(pg, false);
            slide_start(down);
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
        if (r.disabled && r.disabled()) return true;
        if (r.change) r.change(dir);
        if (msg != 1) SW(data + 12, msg == 6 ? 1 : 2);      // the sound of Left and Right
        page_refresh(pg);
        if (g_current_page == &pg) page_gray(pg, false);
        return true;
    }
    case 21:                                // a question's answer (pc_ask)
        ask_answer(data);
        return true;
    case 17:                                // Back, while the page keeps it for itself (page_back_frame)
        if (pg.on_back) {
            SW(data + 8, 0);                // as tKeyboard answers it
            pg.on_back();
        }
        return true;
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
            slide_start(down);
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
// Its value is shown as the game's rows show theirs: a bar (AUDIO_BAR1) and a marker on it (AUDIO_BAR1_V), copies of the first row's
// made before the rows move down, so they stand where that row stood. The game's markers are placed by FlailerSlider::Animate
// (0x16de30): the bar's left edge plus the value's share of its width. This one is placed the same way, when the value changes.
static const u32 AUDIO_LIST = 0x2b9c30, MONOSTEREO = 0x2932dfca;
static const int AUDIO_SCROLL = 284;        // tScrollingMenuList in the screen
static const int AUDIO_BLURB = 372;         // its tAnimatedBlurb
static const char MASTER_BLURB[] = "Master volume of the game.";

namespace fn {
const u32 ScrollSetVisibleArea = 0x249f28;  // tScrollingMenuList::SetVisibleArea(list, top item, bottom item)
const u32 ScrollAddObject = 0x249f70;       // tScrollingMenuList::AddObject(list, object)
}

static const u32 AUDIO_BAR1 = 0xa29f7a16, AUDIO_BAR1_V = 0xc866642b;
static u32 master_bar_hash() { return pc_hash("PC_MASTER_BAR"); }
static u32 master_marker_hash() { return pc_hash("PC_MASTER_BAR_V"); }

namespace fn {
const u32 ObjectWidth = 0x140588;           // FlailerObjectChunk::GetWidth(object), in pixels
const u32 SetTopLeftX = 0x1444a0;           // FlailerSetTopLeftX(screen's hash, object's hash, x)
}

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
    for (u32 from : {AUDIO_BAR1, AUDIO_BAR1_V})
        if (u32 src = find_object(s, from, 2)) {
            u32 o = clone_object(s, src);
            SW(o + 8, from == AUDIO_BAR1 ? master_bar_hash() : master_marker_hash());
        }
    for (int i = 0; i < 26; i++) {
        u32 o = find_object(s, LW(AUDIO_LIST + i * 4u));
        if (o) set_fl(o + 52, fl(o + 52) + step), SB(o + 43, 5);
    }
}

static void master_show()
{
    u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, AUDIO_SCREEN) : 0;
    u32 bar = s ? find_object(s, master_bar_hash(), 2) : 0, marker = s ? find_object(s, master_marker_hash(), 2) : 0;
    if (!bar || !marker) return;
    // the marker's own width off the bar's: at 100 it ends where the bar ends
    const int width = (int)pc_call(fn::ObjectWidth, bar) - (int)pc_call(fn::ObjectWidth, marker);
    const int x = (int)(fl(bar + 48) + (float)width * (float)g_cfg.volume / 100.0f);
    pc_call(fn::SetTopLeftX, AUDIO_SCREEN, master_marker_hash(), (u32)x);
}

void HOOK___18AudioOptionsScreeni(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0];
    orig(c);
    u32 ret = c.r[2].w[0], list = self + AUDIO_SCROLL;
    pc_call(fn::ScrollSetVisibleArea, list, menuitem_hash(0), menuitem_hash(6));
    pc_call(fn::ScrollAddObject, list, menuitem_hash(0));
    pc_call(fn::ScrollAddObject, list, master_bar_hash());
    pc_call(fn::ScrollAddObject, list, master_marker_hash());
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

// A third hint beside Help and Back, on the profiles' screen: the button that deletes. Its icon is a help character of the buttons' font:
// Square's on a gamepad, and on the keyboard the one the sheet paints as the Delete key (pcplatform.cpp, the font's L3, which the menus
// never show). Which of the two is set by the screen as the device changes (pc_delete_hint).
static int g_project_gen;
static void add_delete_hint(u32 project, const std::string &page)
{
    PcPage *pg = pc_find_page(page.c_str());
    u32 s = pg ? find_screen(project, page_hash(*pg)) : 0;
    u32 icon = s ? find_object(s, pc_hash("PC_BACK_ICON"), 1) : 0, word = s ? find_object(s, back_text_hash(), 1) : 0;
    if (!icon || !word || find_object(s, pc_hash("PC_DEL_TEXT"), 1)) return;
    u32 i = clone_object(s, icon);
    SW(i + 8, pc_hash("PC_DEL_ICON"));
    set_fl(i + 48, fl(icon + 48) + 88.0f);
    set_text(i, "3");
    u32 t = clone_object(s, word, 16);
    SW(t + 8, pc_hash("PC_DEL_TEXT"));
    SW(t + 96, 0);
    set_fl(t + 48, fl(word + 48) + 88.0f);
    set_text(t, "Delete");
}

// Each frame the page is up: cheap when nothing changed. Back is shown or not by the page's shape (the first screen has none), and the
// frontend's objects are made anew, at their first places and texts, each time it loads.

void pc_delete_hint(const char *page, bool pad, bool show, const char *text)
{
    PcPage *pg = pc_find_page(page);
    u32 project = find_project(FRONTB_PROJECT), s = project && pg ? find_screen(project, page_hash(*pg)) : 0;
    if (!s) return;
    u32 icon = find_object(s, pc_hash("PC_DEL_ICON"), 1), word = find_object(s, pc_hash("PC_DEL_TEXT"), 1);
    if (!icon || !word) return;
    static int last_gen = -1, last_pad = -1;
    if (last_gen != g_project_gen || last_pad != (int)pad) {
        last_gen = g_project_gen, last_pad = pad;
        pc_call(fn::SetString, icon, gstr(pad ? "h" : "3"));       // Square's help character; the Delete key's
    }
    static std::string last_text;
    static int last_text_gen = -1;
    if (show && (last_text != text || last_text_gen != g_project_gen)) {
        last_text = text, last_text_gen = g_project_gen;
        set_text(word, text);
    }
    u32 back_icon = find_object(s, pc_hash("PC_BACK_ICON"), 1), back_word = find_object(s, back_text_hash(), 1);
    if (back_icon && back_word) {
        const float step = LB(back_icon + 41) ? 88.0f : 0.0f;       // Back's own place when it is not shown
        const u32 pairs[2][2] = {{icon, back_icon}, {word, back_word}};
        for (auto &p : pairs)
            if (fl(p[0] + 48) != fl(p[1] + 48) + step) {
                set_fl(p[0] + 48, fl(p[1] + 48) + step);
                SB(p[0] + 43, 4);
            }
    }
    for (u32 o : {icon, word})
        if ((LB(o + 41) != 0) != show) pc_call(fn::SetVisibility, o, show ? 1 : 0);
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
    if (!g_profiles_page.empty()) add_delete_hint(tail, g_profiles_page);
    // The layouts' screen (Joystick Config): Help's icon is the buttons' font's 'k' and Back's 'j', Circle's and Triangle's cells, which
    // the layouts themselves show as the gamepad's. There the two hints take Select's cell ('b') and L3's ('3'): pc_paint_buttons.
    if (u32 lay = find_screen(tail, 0xb6657464)) {
        if (u32 o = find_object(lay, pc_hash("PC_BACK_ICON"), 1)) set_text(o, "b");
        for (u32 o = LW(lay + 32); o && o != lay + 32; o = LW(o))
            if ((s8)LB(o + 40) == 1 && LW(o + 80) == 0xb85de386 && pc_str(o + 100) == "k" && fl(o + 52) > 400.0f) pc_call(fn::SetString, o, gstr("3"));
    }
    if (!g_profiles_page.empty()) add_name_grid(tail, g_profiles_page);
    g_project_gen++;
    add_options_arrows(tail);
    g_page_alloc = false;
}

// The pause menu in the middle of the picture with the logo over it: on the PS2 it runs up to the right edge, the logo stands aside to the left.
// In Flailer's 640 across: the panel is 170 + 480 wide, the logo 296 x 0.6.
static void center_pause_menu(u32 project)
{
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    if (exact) return;
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
    grid_end(false, false);
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
        if (!pg->on_open)
            for (const PcRow &r : pg->rows)
                if (r.disabled) {
                    page_gray(*pg, true);       // a page with a row that can be greyed: the slots' states as they are now
                    break;
                }
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

// The layouts' screen shows a picture of the controller, the texture CONTROLLER<the layout's id>: 30 to 33 are the DualShock 2's. The four
// layouts of the triggers have the ids of the DualShock 1's old ones (20 to 22, with its picture) and a new one (34, with none). They are
// all a DualShock 2's here: its picture goes on after the game has set its own (0x144a28: a screen's object gets a texture by hash).
void HOOK_DrawScreen__20JoystickConfigScreen(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 self = c.r[4].w[0];
    orig(c);
    pc_call(0x144a28, LW(self + 0x10), 0x0b2ad6df, pc_hash("CONTROLLER30"));
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
    if (msg == 21 && g_asking) {
        ask_answer(data);
    } else if ((msg != 1 && msg != 4) || LW(data) != menuitem_hash(4)) {
        orig(c);
    } else if (msg == 1) {
        pc_ask_exit();
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
    // A replay's bar of buttons (ReplayScreen draws it itself, REPLAYBOTTOM_NEW: no Flailer rows). Each symbol with the key under it is
    // a button for the pointer: a click is that button's press, and held on the two that wind the replay it stays pressed. The places
    // are the bar's own (pcplatform.cpp, kBarKeys: texels of a picture 640 wide, shown 0.8 of that in the 512), the rows as measured.
    if (pc_replay_playing() && !menu_on_screen()) {
        static const struct { float x; int button; bool hold; } kKeys[] = {
            {60.4f, PCB_LEFT, true}, {95.2f, PCB_SQUARE, false}, {132.0f, PCB_CROSS, false}, {166.8f, PCB_TRIANGLE, false},
            {202.4f, PCB_CIRCLE, false}, {236.0f, PCB_RIGHT, true}, {271.6f, PCB_SELECT, false}, {308.8f, PCB_START, false}};
        pending = -1, click = false;
        // Its help (the game's panel that slides in over the screen: FlailerScreenList +0x1c4, on screen while its X is under 640, the
        // test of 0x1e4170) hears only the button that opened it. A left click anywhere is that button.
        bool help = false;
        if (const u32 list = LW(gp() - 21324))
            if (const u32 panel = LW(list + 0x1c4))
                if (const u32 mover = LW(panel + 0x14)) help = fl(mover + 8) < 640.0f;
        if (help) {
            if (pressed && inside && g_mouse_wait <= 0) {
                g_mouse_out = 1 << PCB_CIRCLE;      // the bar's "?"
                g_mouse_wait = 4;
            }
            return;
        }
        if (inside && left && y >= 366 && y <= 426)
            for (const auto &k : kKeys)
                if (std::fabs(x - k.x) <= 15 && (pressed || (k.hold && g_mouse_out == 1 << k.button))) {
                    g_mouse_out = 1 << k.button;
                    g_mouse_wait = 4;
                }
        return;
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
    unsigned long long usable = 0;          // by the item's place in the menu: it can be chosen
    if (chunk) {
        float best = 1e9f;
        static const bool diag = getenv("HP2_DIAG") != nullptr;
        static u32 diag_menu;
        if (diag && diag_menu != LW(menu + 20)) {
            diag_menu = LW(menu + 20);
            fprintf(stderr, "menu %08x: the items' states", LW(menu + 20));
            for (u32 item = LW(menu); item && item != menu; item = LW(item)) fprintf(stderr, " %x", LW(item + 8));
            fprintf(stderr, "\n");
        }
        for (u32 item = LW(menu); item && item != menu; item = LW(item), count++) {
            if (item == LW(menu + 8)) current = count;
            // A greyed item (View Replay in the first second of a race) is not the pointer's to pick: the game steps over it, the selection
            // never gets there and went back and forth instead. FlailerMenuItem::State +8, bit 0: the item can be chosen; GrayMenuItem
            // (0x16d198) clears it for "grey" and for "hidden" and sets it for "as usual".
            if (!(LW(item + 8) & 1)) continue;
            if (count < 64) usable |= 1ull << count;
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
    // A row pointed at on one screen is not a row of the next: when the screen under the pointer changed to one with fewer rows before the
    // selection got there, the walk below, which ends on that row, never ended, and the game stood still with it.
    if (pending >= count || current >= count) pending = -1;
    if (pending >= 0 && count && current >= 0 && pending != current && tries > 0) {
        // The shorter way round, in presses: the game steps over what cannot be chosen (greyed, hidden), so only the items that can are
        // counted. By places alone, from Quit back up to Options past a greyed View Replay looked the longer way and the selection went
        // down, round over the top.
        int down = 0, up = 0;
        for (int i = current; i != pending;) {
            i = (i + 1) % count;
            if (i >= 64 || (usable >> i & 1)) down++;
        }
        for (int i = current; i != pending;) {
            i = (i + count - 1) % count;
            if (i >= 64 || (usable >> i & 1)) up++;
        }
        g_mouse_out = 1 << (down <= up ? PCB_DOWN : PCB_UP);
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
static int g_name_player;

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
    if (g_name_typing) g_name_armed = true;
    if (g_name_typing) {
        // the keyboard is the name's; the screen's animations go on, and the grid's own, which pulses the bar under the place typed
        const u32 kb = self + 8;
        if (msg != 18) {
            if (msg <= 10) SW(data + 8, 1);
            return;
        }
        orig(c);
        if (window_input_kind() == 1) {             // a gamepad's button: the screen's own grid, with what is typed
            g_name_typing = false;
            window_text_end();
            g_edit.cur = (int)g_edit.text.size();
            kb_typed_show(kb);
            pc_call(0x1454e8, LW(kb), GRID_GROUP, 1);
            SW(self, 1);                            // NameEntry: the grid is up, the events are its
            pc_call(0x16d448, 0x31a6e8);            // as NameEntry::MaybeActivateKeyboard does with it
            return;
        }
        pc_call(0x24ca60, kb);                      // tKeyboard::Tick: the screen ticks it only while its grid is up
        bool done = false, keep = false;
        bool typed = false;
        while (const int ch = window_text_next()) {
            if (ch == 13 || ch == 27) {
                done = true, keep = ch == 13;
                break;
            }
            g_edit.key(ch);
            typed = true;
        }
        if (typed) kb_typed_show(kb);
        if (!done) return;
        g_name_typing = false;
        window_text_end();
        const std::string text = g_edit.name();
        pc_call(0x24c5d0, kb, (u32)g_name_player, 0);       // tKeyboard::SetVisible(off): the name's own text is back
        keep = keep && !text.empty();
        if (keep) {
            u8 *name = host(kFredNames + (u32)g_name_player * 1744);
            memset(name, 0, 11);
            memcpy(name, text.data(), text.size());
        }
        if (u32 o = name_object(self, g_name_player)) set_text(o, player_name(g_name_player));
        blurb_text(self + NAME_BLURB, keep ? "The name is set." : "The name stays as it was.");
        pc_call(fn::BlurbAnimate, self + NAME_BLURB);
        return;
    }
    g_name_armed = LW(self) != 0;
    if (LW(self)) {
        // The screen's own grid is up. A key brings the keyboard back, with what the grid has; Back leaves the name as it was (the
        // game takes it by Back, message 17, having no other way out: here only the grid's last key takes it).
        const u32 kb = self + 8, player = LW(kb + 4);
        const bool to_keys = window_input_kind() != 1;
        if (to_keys || msg == 17) {
            if (to_keys && msg != 18) {             // the key's own event is not the grid's; the change is made on the tick
                if (msg <= 10) SW(data + 8, 1);
                return;
            }
            g_name_player = (int)player;
            if (to_keys) {
                orig(c);
                g_edit = NameEdit();
                g_edit.max = 7;
                g_edit.text = pc_str(kb + 0x84);
                if (g_edit.text.size() > 7) g_edit.text.resize(7);
                g_edit.cur = (int)g_edit.text.size();
                pc_call(0x1454e8, LW(kb), GRID_GROUP, 0);
                SW(self, 0);
                pc_call(0x16d448, 0);
                g_name_typing = true;
                window_text_begin(true);
                kb_typed_show(kb);
                blurb_text(self + NAME_BLURB, "Type the new name. Enter sets it, Esc leaves it.");
                pc_call(fn::BlurbAnimate, self + NAME_BLURB);
                return;
            }
            pc_call(0x24c5d0, kb, player, 0);       // tKeyboard::SetVisible(off)
            SW(self, 0);
            pc_call(0x16d448, 0);
            if (u32 o = name_object(self, (int)player)) set_text(o, player_name((int)player));
            SW(data + 8, 0);
            SW(data + 12, 0x1d);
            blurb_text(self + NAME_BLURB, "The name stays as it was.");
            pc_call(fn::BlurbAnimate, self + NAME_BLURB);
            return;
        }
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
                g_name_player = player;
                pc_call(0x24c708, self + 8, (u32)player);   // tKeyboard::Activate: the places and the bar, in this player's row
                kb_typed_begin(self + 8);
                kb_typed_show(self + 8);
                window_text_begin();
                blurb_text(self + NAME_BLURB, "Type the new name. Enter sets it, Esc leaves it.");
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
    list_frame();
    grid_typed_frame();
    page_back_frame();
    for (auto &f : g_frame_fns) f();
    if (g_pc_ui_dirty.exchange(false)) pc_refresh_page();
    if (go && ++tick == (strchr(go, ',') ? atoi(strchr(go, ',') + 1) : 900)) {
        std::string name(go, strchr(go, ',') ? (size_t)(strchr(go, ',') - go) : strlen(go));
        pc_call(fn::ChangeToScreen, gstr(name), 1);
    }
}
