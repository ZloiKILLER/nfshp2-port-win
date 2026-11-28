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
#include "pcwheel.h"
#include <algorithm>
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
static std::map<u32, u32> g_wheel_was;         // a text of the Controller screen with the wheels' word on it: the language id it had
static std::map<u32, u32> g_pad_was;           // the same for the words the port puts on it as the gamepads' screen

static void g_row_home_clear();
static void free_pages()
{
    for (u32 p : g_page_mem) pc_call(fn::bFree, p);
    g_page_mem.clear();
    g_texts.clear();
    g_wheel_was.clear();
    g_pad_was.clear();
    g_row_home_clear();
    native_menu_part(0, 0.0f);              // the car select's parts were of the file's objects
}

// The ring of strings below, and after it some lasting room (pc_keep): one block, taken when the first string is made, early,
// as the game's own lasting blocks are. What the port needs for good later on (a screen's name, a layout's table) comes out of
// that room and not out of the game's heap, where a small block left behind in the middle of a session is in the way of the
// race's big allocations (free_pages, above).
static const u32 kKeepRoom = 6144;
static u32 ring()
{
    static u32 at;
    if (!at) at = galloc(16 * 256 + kKeepRoom);
    return at;
}

u32 pc_keep(u32 size)
{
    static u32 used;
    size = (size + 15) & ~15u;
    if (used + size > kKeepRoom) return galloc(size);       // (more than was thought of: the heap after all)
    const u32 p = ring() + 16 * 256 + used;
    used += size;
    return p;
}

// a guest copy of a string; the slot is reused after 16 more
static u32 gstr(const std::string &s)
{
    static int slot;
    u32 p = ring() + (slot++ & 15) * 256;
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
static u32 g_player_modes[2];               // each player's own, by the Player's number

// two players now: in a race, and on the screens where both choose at once (SetTwoPlayerScreenMode, src/FMenus.cpp)
static u32 g_two_player_screens;
// Before a race starts and after it ends the game waits for JOY_EVENT_SKIP, which is Cross in every layout (Player::SetInputMode,
// 0x1776b0: modes 5 and 6 add PreRaceJoyHandler and FinishRaceJoyHandler for it; 2 and 7 are the race itself).
bool pc_skip_wanted() { return pc_racing() && (g_player_mode == 5 || g_player_mode == 6); }
bool pc_two_player_race() { return pc_racing() && LW(gp() - 20920) == 2; }     // Player::NumPlayers
bool pc_two_player_screen() { return g_two_player_screens != 0; }
// the word at 0x31ab0c (RaceSettings +274 words in: what RealDriver's horn and boost and Player::SetInputMode ask) is set in a race
// where a player is the cop
bool pc_cop_mode() { return pc_racing() && LW(0x31ab0c) != 0; }

void HOOK_SetTwoPlayerScreenMode__Fi(Ctx &c, void (*orig)(Ctx &))
{
    g_two_player_screens = c.r[4].w[0];
    orig(c);
}

// The event tree (Championship / Ultimate Racer, issue 39): tCarLogoAnimation::SetEvent (0x1f8e18) also puts the event's car logos
// (CARLOGO_...) into the texture list of the menus' background animation (FlailerScreenList +456, tGlobalBackgroundAnimation;
// AddTextureHash 0x249730), the only place in the game that does. ChangeToScreen activates the screen (0x16c548) before it resets
// the background (ChangeScreenHash 0x16c558), and the reset, as every 50 s cycle after it, takes a random texture of that list:
// with a one-car event a logo in the left grid instead of a blueprint one time in three, for a whole cycle. The list is put back
// to the two blueprints (DefaultTextureList 0x249828) after the call; the logo over the map (FADING_LOGO_1) is the call's own.
void HOOK_SetEvent__17tCarLogoAnimationP9EventInfo(Ctx &c, void (*orig)(Ctx &))
{
    orig(c);
    g_pc_ctx = &c;
    const u32 list = LW(gp() - 21324), bg = list ? LW(list + 456) : 0;
    if (bg) pc_call(0x249828, bg);
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

u32 pc_input_mode() { return g_player_mode; }
bool pc_race_menu() { return pc_racing() && (g_player_mode == 0 || g_player_mode == 6 || (g_player_mode != 1 && menu_on_screen())); }
static std::string screen_name();
// The pause menu is the screen that is up. The player's mode does not always say so: paused while the zone camera is on (mode 3), the
// mode stays what it was.
bool pc_pause_menu() { return pc_racing() && menu_on_screen() && screen_name() == "Pause Menu"; }
bool pc_debug_menu() { return pc_racing() && screen_name() == "Debug Menu"; }
// a question over the screen: FlailerScreenList (gp-21324) +392, two tPopupMenu, each active by its first byte
bool pc_popup_up()
{
    if (!g_pc_ctx) return false;
    const u32 lists = LW(gp() - 21324);
    for (int k = 0; lists && k < 2; k++)
        if (const u32 pop = LW(lists + 392 + 4 * k))
            if (LB(pop)) return true;
    return false;
}
// A page under the pause menu: the race is paused (RaceCoordinator, gp-22216: state 6) and the screen up is another than "Pause Menu":
// "Pause Options", "Pause audio", "Pause display", "Race Objective", "Pause BAC controls", "Debug Menu", a page of the port's.
bool pc_pause_subpage()
{
    if (!pc_racing() || !menu_on_screen()) return false;
    const u32 rc = LW(gp() - 22216);
    if (!rc || LW(rc) != 6) return false;
    const std::string screen = screen_name();
    return !screen.empty() && screen != "Pause Menu";
}
// the name of the screen that is up (FlailerScreenNode +16 its FlailerScreenConfig, +8 the config's ScreenName), "" with none
static std::string screen_name()
{
    if (!g_pc_ctx) return "";
    const u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0, config = node ? LW(node + 16) : 0, name = config ? LW(config + 8) : 0;
    return name ? pc_str(name) : "";
}

bool pc_on_title() { return g_pc_ctx && screen_name() == "Title Screen"; }
// A race's objective: before the start (PreRaceBlurb, PreRace2PBeACop) or a page of the pause menu (PauseRaceObjective). Its
// lines say what to press in the race (a cop's target, helicopter, road block and boost), so the keyboard's icons there are the
// driving keys (pc_paint_buttons).
bool pc_objective_screen()
{
    if (!g_pc_ctx) return false;
    const std::string s = screen_name();
    return s == "Pre Race Blurb" || s == "Pre Race 2P Be A Cop" || s == "Race Objective" || s == "Race Objective 2PBAC";
}
std::string pc_screen() { return screen_name(); }
// The title screen waits for a button: TitleScreen::pInstance (gp-21156), its State +12 is 1 (2 while it plays its logos and films)
bool pc_title_waiting()
{
    const u32 title = g_pc_ctx ? LW(gp() - 21156) : 0;
    return title && LW(title + 12) == 1;
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
// The race's HUD is what the 2D is: on a wide picture its left and right parts keep to the screen's edges (native.cpp, wide_place).
// Not while a menu is up over the race, whatever the player's mode says: a menu's lines are short and stand left of the middle, so
// each would be taken for a part of the HUD's left side and pulled out of its panel toward the edge, the shorter the further. A
// tester's picture showed just that (fullscreen 16:9, the pause menu's texts left of their panel, Quit Race furthest); the pause
// menu opened in the usual way has mode 1 and was never touched by this, so some way leads to a menu with the mode of driving, as
// the Race Over menu is known to be. With a menu up all of the 2D is narrowed around the middle as one block. The log says which
// menu and which mode when it happens, once for each.
bool pc_hud_up()
{
    if (!pc_racing() || (g_player_mode != 2 && g_player_mode != 3)) return false;
    if (!menu_on_screen()) return true;
    static u32 told;
    const u32 screen = pc_screen_hash();
    if (told != screen) {
        told = screen;
        fprintf(stderr, "hud: a menu (%08x, \"%s\") is up while the input mode is %u: its 2D is laid out as one block\n", screen, screen_name().c_str(), g_player_mode);
    }
    return false;
}
bool pc_car_input() { return pc_racing() && g_player_mode != 0 && g_player_mode != 1 && g_player_mode != 6 && !menu_on_screen(); }
// The same for one of two players. With two in a race each Player has its own mode (g_player_modes, by Player +8, its controller port): one
// busted (mode 6 or none) while the other drives on. g_player_mode, what the rest goes by, is then the mode of a player who drives
// if there is one. One mode for both took the second player's car away when the first was busted: his keys became a menu's.
bool pc_car_input_port(int port)
{
    if (!pc_two_player_race()) return pc_car_input();
    const u32 mode = g_player_modes[port & 1];
    return mode != 0 && mode != 1 && mode != 6 && !menu_on_screen();
}

// The keyboard's driving keys are the game's actions (pcactions.cpp) for a player who drives, in the modes whose handlers a key
// stands for: racing (2), the frozen zone (3: pause alone), before the start (5: skip and pause). Not in a replay, whose keys are
// the bar's buttons, nor after the finish or in the AI's demo (any button there is the old way's business), nor with hp2.ini's
// [keyboard] actions=0.
bool pc_actions_port(int port)
{
    if (!g_cfg.key_actions || !pc_car_input_port(port) || pc_replay_playing()) return false;
    const u32 mode = pc_two_player_race() ? g_player_modes[port & 1] : g_player_mode;
    return mode == 2 || mode == 3 || mode == 5;
}

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
// (a text of player 2's on a page both players share, the Keyboard page's rows: "P2 " and then the text. The text is
// translated, the mark stays as it is)
static std::string tr_label(const std::string &s) { return s.rfind("P2 ", 0) == 0 ? "P2 " + tr(s.substr(3)) : tr(s); }

static void set_text(u32 obj, const std::string &s)
{
    g_texts[obj] = s;
    pc_call(fn::SetString, obj, gstr(tr_label(s)));
}
static void blurb_text(u32 blurb, const std::string &s) { pc_call(fn::BlurbNewText, blurb, gstr(tr(s))); }
// a row's help line and whether only Select works on it, as the row is now (PcRow::blurb_if, select_only_if)
static std::string row_blurb(const PcRow &r) { return r.blurb_if ? r.blurb_if() : r.blurb; }
static bool row_select_only(const PcRow &r) { return r.select_only || (r.select_only_if && r.select_only_if()); }

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
    if (pc_asking()) return;                // a question stands over the page, whose objects are hidden: redrawn with the answer
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
    bool arrows = pg.cur >= 1 && at < total && pg.rows[(size_t)at].value && !row_select_only(pg.rows[(size_t)at]);
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
// The game's own list (tScrollingMenuList) slides its rows to their new places in 0.5 s by a smoothstep (3u^2 - 2u^3: its Tick and
// SetDesiredTopLeftY) and pulses its two arrows' alpha by a sine (tfeSineWave: 0 to 128, 10 radians a second), as the value's two
// arrows pulse (tLeftRightArrowScroll, the same wave). At its ends it goes no further: the screen refuses the move (Display's
// Up on its first row, Down on its last; Audio's the same). The port's lists are seven slots whose texts change, so the same is
// shown with what they have: after a step the seven rows start a row away from their places and slide home, the one that comes in
// fading up. (The user, 2026-11-28: "the long pages as Audio: there is the game's own code for how a menu behaves"; his choice of
// the port's slots behaving as the game's list, as a page may have more rows than the game's menu takes, MENUITEM_0..15.)
static std::map<u32, float> g_row_home;     // a row's text or value: its own Y on the screen
static float g_slide;                       // rows: where the texts are from home now, in rows; 0 at rest
static double g_slide_from;                 // when the slide began, seconds
static int g_slide_dir;
static const float kRowStep = 37.0f;        // between two rows of the Options screen
static const double kSlideSeconds = 0.5;

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

// No pulse while a list scrolls. The pulse (tHighlightAnimation, OptionsScreenBase +28: a copy of the row's text, the object
// ANIMATING_MENUITEM, swells 1 -> 2 and fades 128 -> 0 in 0.5 s, from the row's text where it is, and again every 4 s; a new row
// starts it at once) is an object of its own, outside the list's clip and the port's fade of the row that comes in: the row
// coming up under the window was seen pulsing before it was in (the user, 2026-11-28: "the highlight's animation stayed; take it
// off where a list scrolls"). For the 0.5 s of a scroll, the port's lists' slide and the game's list's (Audio, HUD), the pulse's
// clock is held at its end (Animate draws nothing past 0.5 s and starts the next after 4) and the copy is put out; the next
// pulse comes 3.5 s after the scroll. The screen up is the one at gp-16888 (a tOptionsScreen, an AudioOptionsScreen, a
// tDisplayOptionsScreen: all OptionsScreenBase).
static const u32 ANIMATING_MENUITEM = 0xab671c9a;
static double g_pulse_quiet_until;          // menu_seconds()
static void pulse_quiet_apply()
{
    if (menu_seconds() >= g_pulse_quiet_until) return;
    const u32 live = LW(gp() - 16888), hash = live ? LW(live) : 0;
    const u32 project = hash ? find_project(FRONTB_PROJECT) : 0, s = project ? find_screen(project, hash) : 0;
    if (!s) return;
    SW(live + 28 + 8, LW(gp() - 19608) - 2000);             // its timer: RealTimeNow (4000 a second) less the pulse's 0.5 s
    if (u32 o = find_object(s, ANIMATING_MENUITEM, 1)) pc_call(fn::SetAlpha, o, 0);
}
static void pulse_quiet_start()
{
    g_pulse_quiet_until = menu_seconds() + 0.5;
    pulse_quiet_apply();
}

// the list on the screen stepped a row: down, and the rows come up from below
static void slide_start(bool down)
{
    g_slide_dir = down ? 1 : -1;
    g_slide = (float)g_slide_dir;
    g_slide_from = menu_seconds();
    pulse_quiet_start();
}

// each frame of the frontend: the rows of the screen that is up to where the slide has them, and its arrows' pulse
static void grid_typed_frame();
static void list_frame()
{
    pulse_quiet_apply();                    // (after the screens' Tick: their pulse has been laid out for this frame)
    // the tOptionsScreen that is live (gp-16888) is a page's or the Options screen's own; its first word is its screen's hash
    const u32 live = LW(gp() - 16888), hash = live ? LW(live) : 0;
    PcPage *pg = hash ? page_by_hash(hash) : nullptr;
    u32 project = find_project(FRONTB_PROJECT), s = project && (pg || hash == OPTIONS_SCREEN) ? find_screen(project, hash) : 0;
    static u32 sliding;                     // the screen whose rows are away from home
    if (sliding && sliding != s) {          // it went while they were: home, for when it comes back
        for (int k = 1; k <= 7; k++) {
            for (u32 o : {find_object(sliding, menuitem_hash(k), 1), value_object(sliding, k)})
                if (o) pc_call_f(fn::SetY, o, row_home(o));
            if (u32 label = find_object(sliding, menuitem_hash(k), 1)) pc_call(fn::SetAlpha, label, 128);    // (the one coming in, whole)
        }
        sliding = 0;
        g_slide = 0;
    }
    if (!s) {
        g_slide = 0;
        return;
    }
    const double now = menu_seconds();
    const int alpha = (int)(64.0 * (1.0 + sin(now * 10.0)));         // 0..128, as tfeSineWave(0, 128, 10)
    for (u32 h : {SCROLL_UP_ARROW, SCROLL_DOWN_ARROW})
        if (u32 a = find_object(s, h, 2)) pc_call(fn::SetAlpha, a, (u32)alpha);
    // and the two beside the highlighted row's value (the user, 2026-11-23: "arrows that blink, as on the game's screens")
    if (pg)
        for (u32 h : {0xd1cda594u, 0x9345e4a7u})           // LEFT_ARROW, RIGHT_ARROW
            if (u32 a = find_object(s, h, 2))
                if ((s8)LB(a + 41) == 1) pc_call(fn::SetAlpha, a, (u32)alpha);
    if (g_slide == 0) return;
    float t = (float)((now - g_slide_from) / kSlideSeconds);
    if (t >= 1) t = 1;
    const float left = 1 - t * t * (3 - 2 * t);         // the way still to go: the game's smoothstep
    g_slide = t >= 1 ? 0 : left * (float)g_slide_dir;
    sliding = g_slide != 0 ? s : 0;
    const int entering = g_slide_dir > 0 ? 7 : 1;
    for (int k = 1; k <= 7; k++) {
        u32 objs[2] = {find_object(s, menuitem_hash(k), 1), pg ? value_object(s, k) : 0};
        for (u32 o : objs) {
            if (!o) continue;
            pc_call_f(fn::SetY, o, row_home(o) + g_slide * kRowStep);   // not the field alone: the text is laid out again only when told
        }
        // The row that comes in is not seen before it is in: Audio's list clips its rows to its window (ClipPoly), so the row it
        // scrolls to comes out from under the window's edge. Here it is the slot's text that comes from a row away, outside the
        // list, already highlighted: it showed below (above) the list before the list had moved, "the next row highlighted before
        // the list has scrolled" (the user, 2026-11-28). Its label and its value come up with the slide, as the clip lets it in.
        if (k == entering) {
            const PcRow *r = pg && pg->top + k - 1 < (int)pg->rows.size() ? &pg->rows[(size_t)(pg->top + k - 1)] : nullptr;
            const int full = r && r->disabled && r->disabled() ? 40 : 128;
            if (objs[1]) pc_call(fn::SetAlpha, objs[1], (u32)(full * (1 - left)));
            if (objs[0]) pc_call(fn::SetAlpha, objs[0], (u32)(128 * (1 - left)));
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

// A screen's name as the game is given it, kept for good. ChangeToScreen (0x14cc78: FlailerScreenList's, 0x16c4c0) first takes
// away the screen that is up and only then looks for the new one by the name, and the going screen's last messages come
// through the port's hooks, which make strings of their own (gstr: sixteen slots, used round and round). From the wheels'
// Controller screen, whose hook makes more than sixteen for a message, the name was something else by the time it was looked
// for: no screen was found and none was up, the black screen of 2026-11-24 ("Settings", then "Calibration", four builds). The
// game's own calls give names that lie in its data.
static u32 screen_name_kept(const std::string &name)
{
    static std::map<std::string, u32> kept;
    u32 &p = kept[name];
    if (!p) {
        p = pc_keep((u32)name.size() + 1);
        memcpy(host(p), name.c_str(), name.size() + 1);
    }
    return p;
}

// Leaving a list screen by its row slides its rows out first, as the game's list screens do (OptionsScreenBase: a
// tExitScreenAnimation in its +92, which takes every message of the screen while it lasts, slides the rows' groups out to x 650,
// row i in 0.05 (i + 1) s, with the bar under the highlighted row, MI_UNDERSCORE's group, leaving with the last; then it changes
// the screen itself: 0x2490d8, its HandleMessages 0x249128). So from the Options screen and from the port's pages (tOptionsScreen
// objects, the live one at gp-16888) a page opens by that slide; elsewhere at once, as before. (The user, 2026-11-28: the game's
// screens have their rows slide out and come back to the last row; the port's had only the slide in.)
static void exit_slide_group(u32 e, u32 group, u32 master, float from, float to, float seconds)
{
    Ctx &c = *g_pc_ctx;
    Ctx saved = c;
    c.r[4].d[0] = sx32(e);
    c.r[5].d[0] = sx32(group);
    c.r[6].d[0] = sx32(master);
    c.f[12].f = from;
    c.f[13].f = to;
    c.f[14].f = seconds;
    c.r[29].d[0] -= 0x200;
    ps2_call(c, 0x248c60);                  // tSlideAnimation::AddGroup
    c = saved;
}
static float object_left(u32 screen, u32 object)
{
    Ctx &c = *g_pc_ctx;
    Ctx saved = c;
    c.r[4].d[0] = sx32(screen);
    c.r[5].d[0] = sx32(object);
    c.r[29].d[0] -= 0x200;
    ps2_call(c, 0x144140);                  // FlailerGetTopLeftX
    const float x = c.f[0].f;
    c = saved;
    return x;
}

void pc_open_page(const char *name)
{
    const u32 live = g_pc_ctx ? LW(gp() - 16888) : 0, hash = live ? LW(live) : 0;
    // (the live object only while its screen is the one up: gp-16888 may still hold one that has gone)
    if (live && hash == pc_screen_hash() && (hash == OPTIONS_SCREEN || page_by_hash(hash)) && !LW(live + 92)) {
        if (const u32 e = pc_call(fn::bMalloc, 28, NAME_STRING, 0, 16)) {     // freed with the screen, as the game's own
            pc_call(0x2490d8, e, hash, screen_name_kept(name), 1);
            float total;
            memcpy(&total, host(e + 16), 4);
            exit_slide_group(e, 0x97bf4b25, 0x32c91dae, object_left(hash, 0x32c91dae), 650.0f, total);    // MI_UNDERSCORE
            SW(live + 92, e);
            return;
        }
    }
    pc_call(fn::ChangeToScreen, screen_name_kept(name), 1);
}

// The game's own way of starting several screens deep (LaunchFrontendMenus, 0x16f950): the names of the screens on the way are
// pushed onto the list's stack of screens to go back to (FlailerScreenList, gp-21324: 12 names of 32 bytes from +0, their count at
// +384; PushScreenOntoStack, 0x16c760, copies a name in), and the last screen alone is shown, without a push of its own.
void pc_open_page_under(const std::vector<std::string> &under, const char *name)
{
    const u32 list = LW(gp() - 21324);
    if (!list) return;
    for (const std::string &s : under)
        if (LW(list + 384) < 12 && s.size() < 32) pc_call(0x16c760, list, gstr(s));
    pc_call(fn::ChangeToScreen, screen_name_kept(name), 0);
}

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
//
// A question asked from a row's own change (Extras' Playable Traffic Cars: the user's picture of 2026-11-24, the page's rows, its
// arrows and the highlight's glow over the question) is put off to the next frame: the press that asked is still being handled
// by the game's menu and by the page (the row's value and arrows drawn again, the item's own flash), all after the page's objects
// were hidden. And while a question is up, whatever of the page's screen shows again is hidden again, every frame.
static std::function<void(bool)> g_answer;
static bool g_asking;
static u32 g_ask_mem[2];
static std::vector<u32> g_under_question;
static u32 g_under_screen;                  // the screen whose objects they are
static bool g_in_row_change;                // a row's change is being called (page_event)
// A row being edited (pc_row_edit: the wheel's rotation, digit by digit; the user, 2026-11-24, "the field must take a gamepad's
// input too"). While it lasts every button of the menu is the editing's, a gamepad's as the keyboard's: the function gets the
// message (1 Select, 6 Left, 7 Right, 8 Up, 9 Down, 17 Back) and says whether the editing goes on.
static std::function<bool(int)> g_row_edit;
void pc_row_edit(std::function<bool(int msg)> fn) { g_row_edit = std::move(fn); }
bool pc_row_editing() { return (bool)g_row_edit; }
static std::function<void()> g_ask_later;   // the question it asked, for the next frame
static int g_ask_later_wait;

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
        g_under_screen = 0;
        return;
    }
    PcPage *pg = g_current_page;
    u32 project = find_project(FRONTB_PROJECT), scr = project && pg ? find_screen(project, page_hash(*pg)) : 0;
    // not a page of the port's: the main menu, whose Exit asks (its rows are drawn over the popup's panel as a page's are)
    if (!scr || !g_current_self || LW(gp() - 16888) != g_current_self) scr = project ? find_screen(project, MAIN_MENU_SCREEN) : 0;
    if (!scr) return;
    g_under_screen = scr;
    for (u32 o = LW(scr + 32); o && o != scr + 32; o = LW(o))
        if (LB(o + 41)) {
            g_under_question.push_back(o);
            pc_call(fn::SetVisibility, o, 0);
        }
}

// every frame: a question put off is asked; under a question that is up, what shows again is hidden again
static void ask_frame()
{
    if (g_ask_later && !g_asking && --g_ask_later_wait <= 0) {
        auto ask = std::move(g_ask_later);
        g_ask_later = nullptr;
        ask();
    }
    if (!g_asking || !g_under_screen) return;
    const u32 scr = g_under_screen;
    for (u32 o = LW(scr + 32); o && o != scr + 32; o = LW(o))
        if (LB(o + 41)) {
            if (std::find(g_under_question.begin(), g_under_question.end(), o) == g_under_question.end()) g_under_question.push_back(o);
            pc_call(fn::SetVisibility, o, 0);
        }
}

bool pc_asking() { return g_asking || g_ask_later; }

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
    pc_refresh_page();                      // the page as it is now: nothing of it was drawn anew under the question
}

void pc_ask(const std::string &title, const std::string &question, std::function<void(bool yes)> answer, const char *yes, const char *no)
{
    const u32 list = LW(gp() - 21324);
    if (g_asking || g_ask_later || !list) return;
    if (g_in_row_change) {                  // asked by a row's change: once the press is done with (ask_frame)
        const std::string y = yes ? yes : "", n = no ? no : "";
        g_ask_later = [title, question, answer, y, n] { pc_ask(title, question, answer, y.empty() ? nullptr : y.c_str(), n.empty() ? nullptr : n.c_str()); };
        g_ask_later_wait = 2;
        return;
    }
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
    if (!live) g_row_edit = nullptr;        // the page with the row being edited is gone
    const bool want = live && (pg->on_back || g_row_edit) && !g_grid_kb && !g_asking;
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
    if (g_row_edit && (msg == 1 || (msg >= 6 && msg <= 9) || msg == 17)) {
        if (msg == 8 || msg == 9) SW(data + 8, 1);          // the highlight stays on the row
        if (msg == 17) SW(data + 8, 0);                     // Back is the editing's: the screen stays
        if (msg == 6 || msg == 7) SW(data + 12, msg == 6 ? 1 : 2);      // the sound of Left and Right
        const std::function<bool(int)> fn = g_row_edit;
        if (!fn((int)msg)) g_row_edit = nullptr;
        page_refresh(pg);
        return true;
    }
    const int slot = (int)(LW(data) - menuitem_hash(1)) + 1, total = (int)pg.rows.size();
    bool known = slot >= 1 && slot <= total && slot <= 7;
    const int row = pg.top + slot;
    switch (msg) {
    case 4:                                 // MENUITEM_HIGHLIGHT: the help line and the arrows
        if (!known) return true;
        pg.cur = (u32)slot;
        blurb_text(self + 100, row_blurb(pg.rows[(size_t)row - 1]));
        pc_call(fn::BlurbAnimate, self + 100);
        page_refresh(pg);
        return true;
    case 8: case 9: {                       // up and down: past the seventh slot the rows move under it. Param 1 keeps the selection where it is
        if (!known) return true;
        const bool down = msg == 9;
        if (total <= 7) {                   // a short page: no round at its ends either, as Audio (the user, 2026-11-28: "on the short ones the
            if (down ? slot == total : slot == 1) SW(data + 8, 1);         //  frame flies round to the top")
            return true;
        }
        if (down ? slot == 7 && row < total : slot == 1 && pg.top > 0) {
            pg.top += down ? 1 : -1;
            {   // a row that cannot be chosen (a divider) does not stop under the highlight: one more, if there is one
                const PcRow &under = pg.rows[(size_t)(pg.top + slot) - 1];
                if (under.disabled && under.disabled() && (down ? pg.top + 7 < total : pg.top > 0)) pg.top += down ? 1 : -1;
            }
            SW(data + 8, 1);
            blurb_text(self + 100, row_blurb(pg.rows[(size_t)(pg.top + slot) - 1]));
            pc_call(fn::BlurbAnimate, self + 100);
            page_refresh(pg);
            page_gray(pg, false);
            slide_start(down);
        } else if (down ? slot == 7 : slot == 1) {
            SW(data + 8, 1);                    // the list's end: the move refused, as the game's own long lists do (no round)
        }
        return true;
    }
    case 1: case 6: case 7: {               // Select, Left, Right
        if (!known) return true;
        PcRow &r = pg.rows[(size_t)row - 1];
        int dir = msg == 6 ? -1 : msg == 7 ? 1 : (r.value ? 1 : 0);
        if (msg != 1 && (!r.value || row_select_only(r))) return true;
        if (r.disabled && r.disabled()) return true;
        g_in_row_change = true;
        if (r.change) r.change(dir);
        g_in_row_change = false;
        if (pc_asking()) return true;       // the row asks first: nothing has changed yet
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

// ---- Extras: DEBUG.not.pretty ---------------------------------------------------------------
// The pause menu has an eighth item the developers left in, Debug (MENUITEM_8, 0x52e039fa): PauseMenu::Activate (0x170738) shows it
// when a function that the released game has returning 0 (0x16fdb0) says so, and hides it otherwise. Its screen, "Debug Menu"
// (PauseDebugMenu, src/PauseMenu.cpp), is the developers' own: the finishing place, car-to-car collisions, the screen's prints, the
// weather, the mirror, the environment mapping, the players' debug info, the easy AI. With the switch on (Options > Extras) the
// item is shown as the game would show it (GrayMenuItem, 0x16d198: 1 is shown and live). Nothing of it is made prettier.
void HOOK_Activate__9PauseMenuUi(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 screen = c.r[5].w[0];
    orig(c);
    if (g_cfg.debug_menu) pc_call(0x16d198, 0x52e039fa, 1, screen);
}

// ---- Options -------------------------------------------------------------------------------
// Nine rows over the screen's seven slots, scrolling as a page does. A row of the game's goes to it as the item that does its job
// (Audio 1, Display 3, Load/Save 4, Names 5, Credits 6; item 7, Languages, is hidden on NTSC); the others open the port's pages.
// The labels and the help line are the port's, by the row.
enum { OPT_GAME, OPT_CONTROLS, OPT_GRAPHICS, OPT_PROFILES, OPT_LANGUAGES, OPT_EXTRAS };
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
    {"Extras", "Bonus features of the game.", OPT_EXTRAS, 0},
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
        else if (o.kind == OPT_EXTRAS) pc_open_page("PC Extras");
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
    for (auto &t : g_texts) pc_call(fn::SetString, t.first, gstr(tr_label(t.second)));
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

// The Audio list's mask (the slot's highlight: the list's +20, slot 0..6, MI_1..7_MASK). The game's list puts it once, at the
// first highlight after the screen is made (tScrollingMenuList::Highlight, 0x24a248: MENUITEM_k -> slot k - 1, only while it is
// -1 and only for rows 1..7), and from then on moves it itself with Up and Down (MoveMaskUp / Down, 0x24a3c0 / 0x24a388). The
// rows scroll (the list moves MENUITEM_1..11 and the bars: the table at 0x2b9c30); the masks stand still. With the port's
// master row above row 1 the game's k - 1 is one slot off, and the port's first fix (slot 0 at the screen's making) left the
// mask on the top slot whenever the screen came up on another row (back from the Jukebox). So here, at the first highlight
// only, the slot is measured: the still mask whose height the row highlighted stands at (MI_1_MASK and MI_2_MASK give the first
// slot's height and the step), kept within the 7 (a row below the window: the last, as the list then scrolls just so far).
// Then the game moves it, as on the PS2. It is put BEFORE the game handles the highlight: in that same call the screen asks
// the list for its mask (GetCurrentMenuMask, 0x24a368) and sets the highlight's animation on it. (The user's pictures,
// 2026-11-28: "no mask when the menu opens", "mask and highlight on different rows after coming back", and, from a try that put
// every highlight by a model of 8 rows, "past Audio Mode the mask flies down": the screen has 11 rows and the port's.)
static void audio_mask(u32 self, u32 item)
{
    const u32 list = self + AUDIO_SCROLL;
    if ((s32)LW(list + 20) != -1) return;
    const u32 project = find_project(FRONTB_PROJECT), s = project ? find_screen(project, AUDIO_SCREEN) : 0;
    const u32 row = s ? find_object(s, item, 1) : 0;
    const u32 m1 = s ? find_object(s, pc_hash("MI_1_MASK"), 2) : 0, m2 = s ? find_object(s, pc_hash("MI_2_MASK"), 2) : 0;
    if (!row || !m1 || !m2) return;
    const float step = fl(m2 + 52) - fl(m1 + 52);
    if (step < 1.0f) return;
    int slot = (int)std::lround((fl(row + 52) - fl(m1 + 52)) / step);
    slot = slot < 0 ? 0 : slot > 6 ? 6 : slot;
    SW(list + 20, (u32)slot);
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
    master_show();
    c.r[2].d[0] = sx32(ret);
}

// A game's list (Audio's, Display's: tScrollingMenuList) scrolls on Up or Down when its mask is at the edge of its 7 slots and the
// move goes on: the mask stays where it was and the rows slide (0.5 s). Then no pulse for the slide (pulse_quiet_start); and on the
// highlight that follows, the game's own restart of the pulse put out in the same call.
static void game_list_event(u32 list, u32 msg, u32 data, Ctx &c, void (*orig)(Ctx &), void (*handle)(Ctx &, void (*)(Ctx &)))
{
    const s32 before = (s32)LW(list + 20);
    handle(c, orig);
    if ((msg == 8 || msg == 9) && LW(data + 8) != 1 && before != -1 && (s32)LW(list + 20) == before) pulse_quiet_start();
    if (msg == 4) pulse_quiet_apply();
}

static void audio_event(Ctx &c, void (*orig)(Ctx &));
void HOOK_HandleEvent__18AudioOptionsScreeniP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    game_list_event(c.r[4].w[0] + AUDIO_SCROLL, c.r[5].w[0], c.r[6].w[0], c, orig, audio_event);
}

static void audio_event(Ctx &c, void (*orig)(Ctx &))
{
    u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    u32 item = msg <= 10 ? LW(data) : 0;
    if (msg == 4) audio_mask(self, item);   // (the first highlight: before the game's own handling, above)
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

// The layouts' screen with a layout of the player's own on it: the hint of the button that edits it, "<Delete> Edit", at the
// bottom after Help and Back, as far from Back as Back is from Help (the user, 2026-11-25: "move it down by the Back button
// and cut it to just DEL Edit, and for this layout only: on the other layouts this button must not appear"; it was "Press
// <button> to edit" in the bar under the list until then). Two objects, made as FRONTB loads: copies of the screen's own Help
// icon and word (the icon is a character of the buttons' font, here 'u', which pc_paint_buttons paints by the device: the
// Delete key, X, Square). Shown with that layout only (config_edit_hint, from the screen's drawing).
static const u32 CONFIG_SCREEN = 0xb6657464;
static const int kHelpEdit = 801603;        // the port's own LanguageID of that screen's help line for the button (HOOK_GetTranslatedString)
// The port's own hints of a key are the pointer's buttons, as the game's Back and Help are (mouse_frame; the user, 2026-11-25:
// "why can the mouse not open it ... can the same Del be a widget that the mouse presses, as the replay's buttons were
// made?"): "<Delete> Edit" with a layout of the player's own, and "Delete" on the profiles' pages. A click is
// kept here until the one that asks for that key takes it: each has its own, since both ask every frame.
static bool g_mouse_edit, g_mouse_delete;
bool pc_delete_pressed()
{
    const bool key = window_delete_pressed(), mouse = g_mouse_delete;
    g_mouse_delete = false;
    return key || mouse;
}
static u32 g_help_icon;                     // the screen's own Help icon, as FRONTB was last loaded
static void add_edit_hint(u32 lay, u32 icon, u32 word)
{
    if (!icon || !word || find_object(lay, pc_hash("PC_EDIT_ICON"), 1)) return;
    // (the place after Back: Back's own icon, if the screen has it by this time, is one step from Help's)
    const u32 back_icon = find_object(lay, pc_hash("PC_BACK_ICON"), 1);
    float step = back_icon ? fl(back_icon + 48) - fl(icon + 48) : 88.0f;
    if (!(step > 40.0f && step < 200.0f)) step = 88.0f;
    const u32 i = clone_object(lay, icon);
    SW(i + 8, pc_hash("PC_EDIT_ICON"));
    set_fl(i + 48, fl(icon + 48) + 2.0f * step);
    pc_call(fn::SetString, i, gstr("u"));
    SB(i + 41, 0);
    const u32 t = clone_object(lay, word, 40);
    SW(t + 8, pc_hash("PC_EDIT_WORD"));
    SW(t + 96, 0);
    set_fl(t + 48, fl(word + 48) + 2.0f * step);
    set_text(t, "Edit");
    SB(t + 41, 0);
}

// The layouts' screen with the modern layout on it: a line in the same bar, "The right stick turns the camera." (the user,
// 2026-11-25: "an info line at the bottom that the right stick is the camera, where the running line is, in its font": the
// camera is no event of the game's, so the list above has no row for it). The bar is the place of the menus' help line, which
// this screen does not use; the line is a copy of that help line's own text object (SCROLLING_TEXT: its font, its size, its
// place in the bar), the screen's own if it has one, else the Options screen's, moved by what the two screens' bars differ
// (SCROLL_TEXT_CLIP); with neither, a copy of the Help word where the "edit" hint stands. Shown from the screen's drawing.
static void add_info_hint(u32 project, u32 lay, u32 help_word)
{
    if (find_object(lay, pc_hash("PC_INFO_TEXT"), 1)) return;
    const u32 text_hash = pc_hash("SCROLLING_TEXT"), clip_hash = pc_hash("SCROLL_TEXT_CLIP");
    u32 src = find_object(lay, text_hash, 1);
    float x = src ? fl(src + 48) : 127.0f, y = src ? fl(src + 52) : 0.0f;
    const char *from = "the screen's own help line";
    if (!src) {
        const u32 other = find_screen(project, OPTIONS_SCREEN);
        src = other ? find_object(other, text_hash, 1) : 0;
        if (src) {
            const u32 here = find_object(lay, clip_hash), there = find_object(other, clip_hash);
            x = fl(src + 48), y = fl(src + 52) + (here && there ? fl(here + 52) - fl(there + 52) : 0.0f);
            from = "the Options screen's help line";
        }
    }
    if (!src && help_word) src = help_word, x = 127.0f, y = fl(help_word + 52) - 84.0f, from = "the Help word";
    if (!src) return;
    const u32 t = clone_object(lay, src, 64);
    SW(t + 8, pc_hash("PC_INFO_TEXT"));
    SW(t + 96, 0);
    // (its colour is the help line's own, the grey of the main menu's running line: the user, 2026-11-25, of the white it
    // was first given, "the same grey as in the main menu". Only a line the file keeps unseen is made seen.)
    if ((LW(t + 92) >> 24) == 0) SW(t + 92, LW(t + 92) | 0x80000000u);
    set_fl(t + 48, x);
    set_fl(t + 52, y);
    set_text(t, "The right stick turns the camera.");
    SB(t + 41, 0);
    fprintf(stderr, "layouts: the modern layout's info line is a copy of %s, at %.0f, %.0f\n", from, x, y);
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
// HP2_FLDUMP=<file>: every Flailer project as the port leaves it at load (its own pages and rows in), one line per object. The mod
// manager's Menu screens tab reads it to show the menus as the port draws them.
static void port_edits(u32 tail);

// The HUD's objects get the parts of a wide picture they are put by (native.cpp, native_hud_part) as their file loads: each
// object of a screen the table has (hud_anchors.h, made from the file by tools/recomp/make_hud_anchors.py) is looked up by its
// name's hash and the place the layout gives it, which it still has here, before the game moves anything. Gone with the file.
#include "hud_anchors.h"
static u32 g_hud_project;                   // the project (its hash) whose objects have parts now
static void hud_anchors(u32 project)
{
    int given = 0, objects = 0;
    for (u32 sl = project + 12, s = LW(sl); s && s != sl; s = LW(s)) {
        const u32 screen = LW(s + 8);
        bool known = false;
        for (const HudAnchor &a : kHudAnchors) known = known || a.screen == screen;
        if (!known) continue;
        for (u32 ol = s + 32, o = LW(ol); o && o != ol; o = LW(o)) {
            objects++;
            const u32 name = LW(o + 8);
            const float x = fl(o + 48), y = fl(o + 52);
            for (const HudAnchor &a : kHudAnchors)
                if (a.screen == screen && a.name == name && std::fabs(a.x - x) < 0.01f && std::fabs(a.y - y) < 0.01f) {
                    const NativeHudPart part{a.k, a.x, a.mid, a.ax, a.ak, a.bx, a.bk, a.kind};
                    native_hud_part(o, &part);
                    given++;
                    break;
                }
        }
    }
    if (!objects) return;
    g_hud_project = LW(project + 8);
    fprintf(stderr, "hud: %d of the %d objects of the HUD's screens have their parts of a wide picture (the rest go by the lines)\n", given, objects);
}

// The jukebox's song window (the chyron: GLOBALB's screen 0x54987eb2, RECORD_LABEL, ARTIST_NAME, SONG_TITLE, RECORD_TITLE and its
// bars CHYRON_BG_1..5) on a wide picture in the menus: part 9, by the left edge (native.cpp, unstretch_2d).
// GLOBALB stays loaded; the parts are cleared with the front end's (free_pages), so they are given again whenever a project loads.
static void chyron_parts()
{
    const u32 list = gp() - 22424;
    int n = 0;
    for (u32 p = LW(list); p && p != list; p = LW(p))
        if (const u32 s = find_screen(p, 0x54987eb2))
            for (u32 ol = s + 32, o = LW(ol); o && o != ol; o = LW(o)) native_menu_part(o, 9.0f), n++;
    static int told;
    if (n && told < 4) told++, fprintf(stderr, "menus: the song window, %d objects by the left edge\n", n);
}

static void on_project_loaded(u32 tail)
{
    port_edits(tail);
    chyron_parts();
    hud_anchors(tail);
    if (const char *path = getenv("HP2_FLDUMP"))
        if (FILE *f = fopen(path, "a")) {
            dump_project(f, tail);
            fclose(f);
        }
}

// ---- Controller (the game's "Joystick PreConfig" screen): Select Device ------------------------------------------------
// The user, 2026-11-23: "add Select Device between Configuration and Calibration, a selector of which gamepad is player 1's
// or 2's; change the distances between the rows so that all of it fits one screen". The screen (JOYSTICK_PRECONFIG, 0x8bd5060b)
// has three rows a player in the bundle: MENUITEM_1..3 and 4..6, each with its MI_n_MASK (27 pixels) and a dark bar under it
// (BOTTOM_MASK, seven of them: 10 pixels between rows, the whole fourth cell between the players, 20 at the end), 37 apart,
// over the film's panel 73..341. The menu takes every MENUITEM_n the screen has, so the two new rows are just their objects,
// made as FRONTB loads like the main menu's Exit: MENUITEM_7 and _8 with MI_7_MASK and MI_8_MASK, and a text for the gamepad's
// name (a copy of the vibration's On / Off). Eight rows of 27 with 4 between them and 15 between the players fill the same
// panel. The layout was drawn before it was built, by ModManager's renderer (work/nshots/controller).
//
// The menu goes through its rows by their numbers, and 7 and 8 belong second in each player's four: up and down are answered
// here, in the order shown (JoystickPreConfig::HandleEvent, 0x2402c0, gets messages 8 and 9 and may refuse the menu's own step).
static const u32 CONTROLLER_SCREEN = 0x8bd5060b;
static const int kControllerOrder[8] = {1, 7, 2, 3, 4, 8, 5, 6};
// (and as the wheels have them, top to bottom: Select Device, Calibration, Force Feedback, Advanced; see the wheels' screen below)
static const int kWheelOrder[8] = {7, 2, 3, 1, 8, 5, 6, 4};
static u32 device_text_hash(int player) { return pc_hash(player ? "CONTROLLER2_DEVICE" : "CONTROLLER1_DEVICE"); }
// the two arrows beside a player's choice (copies of the vibration row's LEFT_ARROW_n and RIGHT_ARROW_n)
static u32 device_arrow_hash(int player, int right)
{
    static const char *const kNames[2][2] = {{"LEFT_ARROW_D1", "RIGHT_ARROW_D1"}, {"LEFT_ARROW_D2", "RIGHT_ARROW_D2"}};
    return pc_hash(kNames[player & 1][right & 1]);
}

static void add_device_rows(u32 project)
{
    const u32 s = find_screen(project, CONTROLLER_SCREEN);
    if (!s || find_object(s, menuitem_hash(7), 1)) return;
    u32 item[9] = {}, mask[9] = {};
    float dy[9] = {}, was[9] = {}, row_y[9] = {};
    for (int n = 1; n <= 6; n++) {
        item[n] = find_object(s, menuitem_hash(n), 1);
        mask[n] = find_object(s, pc_hash(("MI_" + std::to_string(n) + "_MASK").c_str()), 2);
        if (!item[n] || !mask[n]) return;
        was[n] = fl(mask[n] + 52);
        dy[n] = fl(item[n] + 52) - was[n];
    }
    const u32 vib[2] = {find_object(s, 0x3fabc377, 1), find_object(s, 0xe2f33138, 1)};      // CONTROLLERn_VIBRATE_ONOFF
    const u32 arrows[2][2] = {{find_object(s, 0x7bcd6704, 2), find_object(s, 0x7c51b6d7, 2)},  // LEFT_ARROW_1, RIGHT_ARROW_1
                              {find_object(s, 0x7bcd6705, 2), find_object(s, 0x7c51b6d8, 2)}};
    std::vector<u32> bars;
    u32 label[2] = {};
    const u32 bar_hash = pc_hash("BOTTOM_MASK");
    for (u32 o = LW(s + 32); o && o != s + 32; o = LW(o)) {
        if (LW(o + 8) == bar_hash && (s8)LB(o + 40) == 2) bars.push_back(o);
        if ((s8)LB(o + 40) == 1 && LW(o + 96) == 160205) label[0] = o;      // "Player 1"
        if ((s8)LB(o + 40) == 1 && LW(o + 96) == 160206) label[1] = o;      // "Player 2"
    }
    if (!vib[0] || !vib[1] || bars.empty()) return;
    const float kRow = 27.0f, kBar = 4.0f, kGap = 15.0f, kTop = 72.0f, kEnd = 341.0f;
    float y = kTop;
    for (int i = 0; i < 8; i++) {
        row_y[kControllerOrder[i]] = y;
        y += kRow + (i == 3 ? kGap : kBar);
    }
    for (int p = 0; p < 2; p++) {
        const int n = 7 + p, src = p ? 4 : 1;
        item[n] = clone_object(s, item[src], 32);
        SW(item[n] + 8, menuitem_hash(n));
        SW(item[n] + 96, 0);
        set_text(item[n], "Select Device");
        mask[n] = clone_object(s, mask[src]);
        SW(mask[n] + 8, pc_hash(("MI_" + std::to_string(n) + "_MASK").c_str()));
        dy[n] = dy[src];
    }
    for (int n = 1; n <= 8; n++) {
        set_fl(mask[n] + 52, row_y[n]);
        set_fl(item[n] + 52, row_y[n] + dy[n]);
        SB(mask[n] + 43, 5), SB(item[n] + 43, 5);
    }
    for (int p = 0; p < 2; p++) {
        const int row = p ? 6 : 3, mine = 7 + p;
        const float value_dy = fl(vib[p] + 52) - was[row];
        set_fl(vib[p] + 52, row_y[row] + value_dy);
        SB(vib[p] + 43, 5);
        for (u32 a : arrows[p])
            if (a) set_fl(a + 52, fl(a + 52) - was[row] + row_y[row]), SB(a + 43, 5);
        const u32 v = clone_object(s, vib[p], 40);
        SW(v + 8, device_text_hash(p));
        SW(v + 96, 0);
        set_fl(v + 52, row_y[mine] + value_dy);
        set_text(v, "None");
        for (int side = 0; side < 2; side++)
            if (const u32 a = arrows[p][side]) {
                const u32 mine_a = clone_object(s, a);
                SW(mine_a + 8, device_arrow_hash(p, side));
                set_fl(mine_a + 52, fl(a + 52) - row_y[row] + row_y[mine]);     // (the vibration's arrows are in their new row by now)
                SB(mine_a + 41, 0);                     // shown with its row highlighted (the handler below)
            }
        if (label[p]) {
            const int first = p ? 4 : 1;
            set_fl(label[p] + 52, fl(label[p] + 52) - was[first] + row_y[first]);
            SB(label[p] + 43, 5);
        }
    }
    // a bar under every row, down to the next row (the players' gap under the fourth, the panel's end under the last)
    std::sort(bars.begin(), bars.end(), [](u32 a, u32 b) { return fl(a + 52) < fl(b + 52); });
    while (bars.size() < 8) bars.push_back(clone_object(s, bars[0]));
    for (int i = 0; i < 8; i++) {
        const float top = row_y[kControllerOrder[i]] + kRow, bottom = i == 7 ? kEnd : row_y[kControllerOrder[i + 1]];
        set_fl(bars[(size_t)i] + 52, top);
        set_fl(bars[(size_t)i] + 60, (bottom - top) / 16.0f);      // the bar's texture is 16 high
        SB(bars[(size_t)i] + 43, 5);
    }
}

static u32 find_menu_screen(u32 hash);
// the menu of the screen that is up (FlailerScreenList, gp-21324: its node +412, the node's screens +8, a screen's menu +8)
static u32 menu_now()
{
    const u32 list = LW(gp() - 21324), node = list ? LW(list + 412) : 0, screens = node ? node + 8 : 0;
    const u32 screen = screens ? LW(screens) : 0;
    return screen && screen != screens ? LW(screen + 8) : 0;
}

// Each player's choice in its row: Automatic, None or the gamepad's name, with an arrow on each side while the row is highlighted
// (the user, 2026-11-23: "add arrows to the gamepad's choice too", pulsing as the game's are: alpha by a sine, 10 radians a
// second). A short word stands where the vibration's On / Off does, between arrows where its arrows are (446 and 583, the word
// centred on 524); a gamepad's name is longer than that room, so its right arrow goes to the panel's edge and the name and the
// left arrow as far left as the name is long.
//
// The places are set every tick and from the vibration row's objects, never as plain numbers: the screen's rows slide in from
// the right when it opens (OptionsScreenBase's slide moves an object with its group, and a copy keeps its original's group),
// and a place written while they slid was left behind by the rest of the slide: the choice ended up at the screen's left edge
// until it was changed (the user's picture, 2026-11-23). A copy is its original's distance from home away from its own.
//
// The choice is made in two steps (the user, 2026-11-23: "the choice is complete only when the player leaves the selector";
// taken at every step, a gamepad that chose None for its own player went to the other player at once and its held button went
// on choosing there). Select on the row opens the selector: its arrows start to pulse, Left and Right go through the choices,
// which are only shown; Select again takes what is shown, Back leaves it as it was. Up and Down do nothing meanwhile. With the
// selector closed the arrows stand still and Left and Right do nothing.
static bool g_wheel_screen;                 // the screen is up as the steering wheels' (pc_open_controller, below)
// A Controller screen object just made. Its address does not say so: the game makes the object anew every time the screen comes
// up, and the heap gives it the place of the one before, so "another address" was true the first time only. The wheels' rows
// were greyed once after the game's start and live again from the second visit on (the user, 2026-11-25: "on the inactive
// rows the selector must be inactive too"). The screen list's tick counts the frames in which another screen is up; a count
// that has moved since this screen's handler last ran means the screen went and came back.
static unsigned g_controller_away;
static bool g_rows_fresh, g_rows_look;      // the wheels' rows: their states to be told; the highlight to be looked at
static bool g_device_fresh;                 // the devices' names to be written again (the screens may have been loaded anew)
static u32 g_highlight_hidden[2];           // the highlight's two lines, hidden while no row of the wheels' screen is live
static bool wheel_row_live(int n);
static int g_wheel_back_row;                // the row of the wheels' screen a page or a screen was opened from: highlighted on the way back
static int g_calibration_watch;             // frames until speed.log is told what screen came after the wheels' Calibration was chosen
static int g_device_row;                    // the Select Device row that is highlighted: 7, 8, or 0
static int g_device_edit;                   // the row whose selector is open: 7, 8, or 0
static std::string g_device_shown;          // the choice shown in the open selector, not taken yet
static void controller_device_layout(u32 self, bool now)
{
    static u32 last_self;
    static std::string last[2];
    static int width[2];
    const u32 chunk = find_menu_screen(CONTROLLER_SCREEN);
    if (!chunk) return;
    const int alpha = 32 + (int)(96.0 * (1.0 + sin(menu_seconds() * 10.0)) * 0.5);
    for (int p = 0; p < 2; p++) {
        const u32 o = find_object(chunk, device_text_hash(p), 1);
        const u32 vib = find_object(chunk, p ? 0xe2f33138u : 0x3fabc377u, 1);
        if (!o || !vib) continue;
        // "Automatic", "None" (texts of the menus) or the gamepad's name: the choice made, or the one shown in the open selector
        std::string name = g_device_edit == 7 + p ? g_device_shown : g_wheel_screen ? pc_wheel_value(p) : window_pad_value(p);
        name = name.empty() ? "Automatic" : name == "none" ? "None" : name;
        if (name.size() > 26) name = name.substr(0, 25) + ".";
        if (now || g_device_fresh || self != last_self || name != last[p]) {
            last[p] = name;
            set_text(o, name);
            width[p] = (int)pc_call(fn::ObjectWidth, o);
            if (width[p] <= 0 || width[p] > 600) width[p] = (int)name.size() * 10;
        }
        const float w = (float)width[p];
        float centre = 524.0f, left = 446.0f, right = 583.0f;
        if (w > 105.0f) right = 616.0f, centre = 608.0f - w / 2, left = centre - w / 2 - 24.0f;
        const auto place = [](u32 obj, float x) {
            if (fl(obj + 48) != x) set_fl(obj + 48, x), SB(obj + 43, 5);
        };
        place(o, centre + fl(vib + 48) - 524.0f);
        for (int side = 0; side < 2; side++) {
            const u32 mine = find_object(chunk, device_arrow_hash(p, side), 2);
            const u32 src = find_object(chunk, (side ? 0x7c51b6d7u : 0x7bcd6704u) + (u32)p, 2);     // RIGHT_ARROW_n : LEFT_ARROW_n
            if (!mine || !src) continue;
            const bool show = g_device_row == 7 + p && !(g_wheel_screen && !wheel_row_live(7 + p));      // (none on a greyed row)
            if (((s8)LB(mine + 41) == 1) != show) pc_call(fn::SetVisibility, mine, show ? 1 : 0);
            if (!show) continue;
            place(mine, (side ? right : left) + fl(src + 48) - (side ? 583.0f : 446.0f));
            pc_call(fn::SetAlpha, mine, g_device_edit == 7 + p ? (u32)alpha : 128u);      // pulsing: the selector is open
        }
    }
    last_self = self;
    g_device_fresh = false;
}

// ---- the Controller screen as the steering wheels' ---------------------------------------------------------------------
// The user, 2026-11-24: "the wheels as an item of their own in the controls, with the same layout as the gamepads have now",
// and after the first build, a plain page of settings: "the wheel is still not made in the likeness of the gamepads". So the
// wheels' screen is the game's Controller screen itself, opened as the wheels' (Controls > Steering Wheel): the same two
// players' rows, with the wheels' words on them and the wheels' things behind them.
//      the gamepads'       the wheels'
//      Select Device       Select Device       whose wheel is whose (pcwheel.cpp: Automatic, None, a wheel by its name)
//      Calibration         Calibration         the game's own calibration screen of that player's port
//      Vibration           Force Feedback      On or Off, that player's wheel (hp2.ini's wheel_ffb1 / wheel_ffb2)
//      Configuration       Advanced            that player's wheel's own settings, the forces and the rotation ("PC Wheel 1" and
//                                              "PC Wheel 2", pcsettings.cpp). The wheels have one layout, so the row's place in
//                                              the game's handler is free; it stands last here (the user, 2026-11-25: "in the
//                                              row Advanced of each player, after the on / off switch")
// The words are put on the screen's own texts (the title, the rows' labels, the vibration's On / Off) while the screen is the
// wheels', and the game's are given back when it is the gamepads' again. A text set by its string loses its language id
// (FlailerTextChunk::SetString, 0x140e20, clears +96), so the id is kept here and set again (SetLanguageID, 0x140ea8). The game
// writes its own On / Off over the vibration's text when it likes: an id back on a text says so, and the word is put again.
static void wheel_words_off();
static void pad_words(bool on);
static void controller_rows_place(bool wheels);
void pc_open_controller(bool wheels)
{
    g_wheel_screen = wheels;
    g_wheel_back_row = 0;
    // (the words and the rows' places before the screen comes up: its first highlight is drawn where its row stands then)
    if (!wheels && !g_wheel_was.empty()) wheel_words_off();
    pad_words(!wheels);
    controller_rows_place(wheels);
    pc_open_page("Joystick PreConfig");
}
bool pc_controller_wheels() { return g_wheel_screen; }

// Where the rows stand. The wheels have no Configuration (one layout for all), and their three rows stand one place up, Select
// Device where the gamepads' Configuration is (the user, 2026-11-24: "raise the rows, so that Select Device is where
// Configuration was"); the Configuration row itself goes to the fourth place, as the wheels' Advanced. A row is its mask, its label and what
// it shows (the device's name and its arrows; On / Off and its arrows): all of them move by what the mask is away from its
// place, so this can be asked any number of times, and after the screens are loaded anew as well.
static float controller_row_y(int n)
{
    float y = 72.0f;                                    // add_device_rows: 27 a row, 4 between rows, 15 between the players
    for (int i = 0; i < 8; i++) {
        if (kControllerOrder[i] == n) return y;
        y += 27.0f + (i == 3 ? 15.0f : 4.0f);
    }
    return y;
}

static void controller_rows_place(bool wheels)
{
    const u32 chunk = find_menu_screen(CONTROLLER_SCREEN);
    if (!chunk) return;
    for (int p = 0; p < 2; p++) {
        const int rows[4] = {p ? 4 : 1, 7 + p, p ? 5 : 2, p ? 6 : 3};       // as the gamepads have them, top to bottom
        for (int i = 0; i < 4; i++) {
            const int n = rows[i], at = wheels ? rows[(i + 3) % 4] : n;
            const u32 mask = find_object(chunk, pc_hash(("MI_" + std::to_string(n) + "_MASK").c_str()), 2);
            if (!mask) continue;
            const float by = controller_row_y(at) - fl(mask + 52);
            if (by > -0.01f && by < 0.01f) continue;
            u32 group[5] = {mask, find_object(chunk, menuitem_hash(n), 1), 0, 0, 0};
            if (i == 1) {
                group[2] = find_object(chunk, device_text_hash(p), 1);
                group[3] = find_object(chunk, device_arrow_hash(p, 0), 2);
                group[4] = find_object(chunk, device_arrow_hash(p, 1), 2);
            } else if (i == 3) {
                group[2] = find_object(chunk, p ? 0xe2f33138u : 0x3fabc377u, 1);
                group[3] = find_object(chunk, 0x7bcd6704u + (u32)p, 2);         // LEFT_ARROW_n
                group[4] = find_object(chunk, 0x7c51b6d7u + (u32)p, 2);         // RIGHT_ARROW_n
            }
            for (u32 o : group)
                if (o) set_fl(o + 52, fl(o + 52) + by), SB(o + 43, 5);
        }
    }
}

// A player's rows as the wheels have them (the user, 2026-11-25: "in the wheel's settings every setting must be inactive
// where the game detects no wheel in the system"). A player's Calibration, Force Feedback and Advanced are live while that
// player has a wheel; Select Device, where a wheel is given to a player, while there is a wheel at all. With none connected
// every row is greyed, and what a greyed row shows (the device's name, On / Off) is dimmed with it, as the port's pages do.
// Told when the game sets the rows' states itself (its UpdateMenuItems), for a screen object just made, and when a wheel comes
// or goes: never frame after frame (the game takes the rows away as a screen goes).
static bool wheel_row_live(int n)
{
    if (n == 7 || n == 8) return pc_wheel_any();
    return n >= 1 && n <= 6 && pc_wheel_of(n >= 4 ? 1 : 0) >= 0;
}

static void wheel_rows_live(int player, u32 screen)
{
    for (int n : {player ? 4 : 1, player ? 5 : 2, player ? 6 : 3, 7 + player})
        pc_call(fn::GrayMenuItem, menuitem_hash(n), wheel_row_live(n) ? 1 : 0, screen);
    const u32 chunk = find_menu_screen(CONTROLLER_SCREEN);
    if (!chunk) return;
    if (const u32 o = find_object(chunk, device_text_hash(player), 1)) pc_call(fn::SetAlpha, o, wheel_row_live(7 + player) ? 128 : 40);
    if (const u32 o = find_object(chunk, player ? 0xe2f33138u : 0x3fabc377u, 1)) pc_call(fn::SetAlpha, o, wheel_row_live(player ? 6 : 3) ? 128 : 40);
}

// a word of the port's on a text of the screen, the text's own language id kept (was) for the day the game's word comes back
static void screen_word(std::map<u32, u32> &kept, u32 obj, const std::string &s)
{
    if (!obj) return;
    const u32 id = LW(obj + 96);
    const auto was = kept.find(obj);
    if (was == kept.end()) kept[obj] = id;
    else if (id) was->second = id;
    const auto shown = g_texts.find(obj);
    if (id || shown == g_texts.end() || shown->second != s) set_text(obj, s);
}

static void screen_words_off(std::map<u32, u32> &kept)
{
    for (const auto &w : kept) {
        g_texts.erase(w.first);
        if (w.second) pc_call(0x140ea8, w.first, w.second);
    }
    kept.clear();
}

static void wheel_word(u32 obj, const std::string &s) { screen_word(g_wheel_was, obj, s); }

static void wheel_words_off()
{
    screen_words_off(g_wheel_was);
    for (u32 &o : g_highlight_hidden) {     // (the highlight's lines, if the wheels' screen hid them)
        if (o) pc_call(fn::SetVisibility, o, 1);
        o = 0;
    }
    // (and what the wheels' greyed rows dimmed)
    if (const u32 chunk = find_menu_screen(CONTROLLER_SCREEN))
        for (int p = 0; p < 2; p++)
            for (u32 o : {find_object(chunk, device_text_hash(p), 1), find_object(chunk, p ? 0xe2f33138u : 0x3fabc377u, 1)})
                if (o) pc_call(fn::SetAlpha, o, 128);
}

// The gamepads' screen: a player's Calibration row, which is that player's page of the gamepad's settings here, is named for
// what it is (the user, 2026-11-25: "rename the gamepads' Calibration to Advanced as well", as the wheels' row of settings
// is). The wheels' Calibration is the game's calibration and keeps the game's word.
static void pad_words(bool on)
{
    if (!on) {
        if (!g_pad_was.empty()) screen_words_off(g_pad_was);
        return;
    }
    const u32 chunk = find_menu_screen(CONTROLLER_SCREEN);
    if (!chunk) return;
    for (int n : {2, 5}) screen_word(g_pad_was, find_object(chunk, menuitem_hash(n), 1), "Advanced");
}

// the screen's title: the text that says "Controller" (the language's 100002 or 100003), found once for a screen's data
static u32 controller_title(u32 chunk)
{
    static u32 of, title;
    if (of != chunk || (title && !g_wheel_was.count(title) && LW(title + 96) != 100002 && LW(title + 96) != 100003)) {
        of = chunk, title = 0;
        for (u32 o = LW(chunk + 32); o && o != chunk + 32; o = LW(o))
            if ((s8)LB(o + 40) == 1 && (LW(o + 96) == 100002 || LW(o + 96) == 100003)) title = o;
    }
    return title;
}

static void wheel_rows(u32 self, bool tick)
{
    const u32 chunk = find_menu_screen(CONTROLLER_SCREEN);
    if (!chunk) return;
    wheel_word(controller_title(chunk), "Wheel");           // (one word: "Steering Wheel" did not fit the title, and "Steering" was left of it)
    for (int p = 0; p < 2; p++) {
        wheel_word(find_object(chunk, menuitem_hash(p ? 4 : 1), 1), "Advanced");
        wheel_word(find_object(chunk, menuitem_hash(p ? 6 : 3), 1), "Force Feedback");
        wheel_word(find_object(chunk, p ? 0xe2f33138u : 0x3fabc377u, 1), g_cfg.wheel_ffb[p] ? "On" : "Off");
    }
    // A screen object just made, or a wheel come or gone: the rows' states, and the highlight. The menu starts on its first row
    // by number, which is the wheels' last (Advanced): the highlight goes to the row the player came back to (a page or the
    // calibration was opened from it), else to player 1's Select Device, the first row shown; and off a row that is greyed, to
    // the first one that is not (with no wheel there is none, and it stays).
    static int last_state = -1;
    static bool placed;
    const int state = (pc_wheel_any() ? 1 : 0) | (pc_wheel_of(0) >= 0 ? 2 : 0) | (pc_wheel_of(1) >= 0 ? 4 : 0);
    if (g_rows_fresh || state != last_state) {
        if (g_rows_fresh) placed = false;
        g_rows_fresh = false;
        last_state = state;
        g_rows_look = true;
        for (int p = 0; p < 2; p++) wheel_rows_live(p, LW(self + 100));
    }
    // What a greyed row shows stays dim: the game sets the On / Off's colour itself, and the rows' slide in sets their alpha.
    // (A text's alpha is the top byte of its colour, +92: FlailerTextChunk's SetAlpha, 0x140ed0.)
    for (int p = 0; p < 2; p++) {
        const u32 shown[2] = {find_object(chunk, device_text_hash(p), 1), find_object(chunk, p ? 0xe2f33138u : 0x3fabc377u, 1)};
        const bool live[2] = {wheel_row_live(7 + p), wheel_row_live(p ? 6 : 3)};
        for (int k = 0; k < 2; k++)
            if (shown[k] && !live[k] && (LW(shown[k] + 92) >> 24) > 40) pc_call(fn::SetAlpha, shown[k], 40);
    }
    // With no row live there is nothing to highlight (the user, 2026-11-25, of the frame on a greyed Select Device: "this
    // highlight must not be there either"). The highlight is OptionsScreenBase::HandleHighlight's (0x1f4e68): two lines, over
    // and under the row (0xc671680c and 0x32c91dae, moved to the row's mask), and the row's mask as the current one of the
    // screen's selection set (this +4; SelectionSet::SetCurrentItem, 0x16e270: the current object +132 is hidden and the others
    // shown where its +144 is set, the other way round where not). Both are undone here and put back when a row comes alive.
    {
        const u32 set = LW(self + 4), current = set ? LW(set + 132) : 0;
        const bool current_hidden = set && LW(set + 144) != 0;
        if (!pc_wheel_any()) {
            const u32 own = find_menu_screen(LW(self));
            const u32 lines[2] = {own ? find_object(own, 0xc671680cu) : 0, own ? find_object(own, 0x32c91daeu) : 0};
            for (int k = 0; k < 2; k++)
                if (lines[k] && (s8)LB(lines[k] + 41) == 1) {
                    pc_call(fn::SetVisibility, lines[k], 0);
                    g_highlight_hidden[k] = lines[k];
                }
            if (current && ((s8)LB(current + 41) == 1) != current_hidden) pc_call(fn::SetVisibility, current, current_hidden ? 1 : 0);
        } else if (g_highlight_hidden[0] || g_highlight_hidden[1]) {
            for (u32 &o : g_highlight_hidden) {
                if (o) pc_call(fn::SetVisibility, o, 1);
                o = 0;
            }
            if (current) pc_call(fn::SetVisibility, current, current_hidden ? 0 : 1);
        }
    }
    // a greyed Force Feedback row shows no arrows either (the game shows the vibration's two with the row highlighted)
    for (int p = 0; p < 2; p++) {
        if (wheel_row_live(p ? 6 : 3)) continue;
        for (u32 h : {0x7bcd6704u + (u32)p, 0x7c51b6d7u + (u32)p})         // LEFT_ARROW_n, RIGHT_ARROW_n
            if (const u32 a = find_object(chunk, h, 2))
                if ((s8)LB(a + 41) == 1) pc_call(fn::SetVisibility, a, 0);
    }
    if (!tick || !g_rows_look) return;
    const u32 menu = menu_now();
    if (!menu) return;
    g_rows_look = false;
    const u32 active = LW(menu + 8);
    int on = 0;
    for (int n = 1; active && n <= 8; n++)
        if (LW(active + 12) == menuitem_hash(n)) on = n;
    int want = on;
    if (!placed) {
        placed = true;
        want = g_wheel_back_row ? g_wheel_back_row : 7;
        g_wheel_back_row = 0;
    }
    if (!want || !wheel_row_live(want)) {
        want = 0;
        for (int n : kWheelOrder)
            if (!want && wheel_row_live(n)) want = n;
    }
    if (want && want != on) pc_call(0x16a3a8, menu, menuitem_hash(want));           // FlailerMenu::SetActiveMenuItem
}

// JoystickPreConfig::HandleEvent (0x2402c0: this, message, ScreenCallbackData: +0 the row's hash, +4 the port, +8 Param, +12 the
// sound). The two Select Device rows are the port's (the selector above: window_pad_step through Automatic, None and each
// gamepad plugged in; window_pad_set takes one, never the one the other player has by name: the two change places). They
// answer either pad and the keys, as a player with no gamepad must be able to choose one. Up and Down are answered for every
// row, in the order shown.
void HOOK_JoystickPreConfigHandleEvent(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    const int row = data ? (int)(LW(data) - menuitem_hash(1)) + 1 : 0;      // MENUITEM_n's n
    if (!g_wheel_screen && !g_wheel_was.empty()) wheel_words_off();         // the gamepads' again: the game's own words
    if (g_wheel_screen) pad_words(false);
    bool fresh = false;
    {   // a screen object just made (g_controller_away)
        static unsigned seen;
        static u32 last_self;
        fresh = seen != g_controller_away || self != last_self;
        seen = g_controller_away;
        last_self = self;
    }
    if (fresh) {
        // its rows where this mode has them (the screens may have been loaded anew since it was opened), and their states
        controller_rows_place(g_wheel_screen);
        g_rows_fresh = true;
        g_device_fresh = true;
    }
    // another screen object, or the highlight on another row (the pointer put it there): an open selector is left as it was; and
    // one whose row went grey under it (the wheel went) is closed
    if (fresh || (msg == 4 && row != g_device_edit)) g_device_edit = 0;
    if (g_wheel_screen && g_device_edit && !wheel_row_live(g_device_edit)) g_device_edit = 0;
    if (g_device_edit) {
        // the selector is open: Left and Right show the next choice, Select takes it, Back gives it up; nothing else is heard
        const int player = g_device_edit - 7;
        if (msg == 6 || msg == 7) {
            g_device_shown = g_wheel_screen ? pc_wheel_step(g_device_shown, msg == 6 ? -1 : 1) : window_pad_step(g_device_shown, msg == 6 ? -1 : 1);
            SW(data + 12, msg == 6 ? 1 : 2);
        } else if (msg == 1) {
            if (g_wheel_screen) pc_wheel_set(player, g_device_shown);
            else window_pad_set(player, g_device_shown);
            pc_config_save();
            g_device_edit = 0;
        } else if (msg == 17) {
            SW(data + 8, 0);                            // Back is the selector's: the screen stays
            g_device_edit = 0;
        } else if (msg == 8 || msg == 9) {
            SW(data + 8, 1);                            // no step to another row
            SW(data + 12, 0);
        } else if (msg == 18 || msg == 4) {
            orig(c);
        }
        if (msg != 18) controller_device_layout(self, true);
        else controller_device_layout(self, false);
        if (g_wheel_screen) wheel_rows(self, msg == 18);
        return;
    }
    if (g_wheel_screen && (row == 7 || row == 8) && (msg == 1 || msg == 6 || msg == 7) && !wheel_row_live(row)) {
        // (no wheel connected: there is nothing to choose from, and the row is greyed)
        blurb_text(self + 224, row == 8 ? "Player 2 has no steering wheel." : "Player 1 has no steering wheel.");
        pc_call(fn::BlurbAnimate, self + 224);
        SW(data + 12, 0);
        return;
    }
    if (msg == 1 && (row == 7 || row == 8)) {
        g_device_edit = row;
        g_device_shown = g_wheel_screen ? pc_wheel_value(row - 7) : window_pad_value(row - 7);
        controller_device_layout(self, true);
        return;
    }
    if ((msg == 6 || msg == 7) && (row == 7 || row == 8)) {
        SW(data + 12, 0);                               // the selector is closed: Select opens it
        return;
    }
    if ((msg == 8 || msg == 9) && row >= 1 && row <= 8) {
        const u32 menu = menu_now();
        const int *const order = g_wheel_screen ? kWheelOrder : kControllerOrder;
        // (the menu's own step is refused whatever comes of this one: it goes by the rows' numbers, and with every row of the
        // wheels' screen greyed it was the menu's step that still moved the highlight)
        SW(data + 8, 1);
        bool stepped = false;
        int at = 0;
        for (int i = 0; i < 8; i++)
            if (order[i] == row) at = i;
        for (int k = 1; menu && k < 8; k++) {
            const int next = order[(at + (msg == 9 ? k : 8 - k)) % 8];
            bool usable = false;
            for (u32 item = LW(menu); item && item != menu; item = LW(item))
                if (LW(item + 12) == menuitem_hash(next)) usable = (LW(item + 8) & 1) != 0;
            if (!usable || (g_wheel_screen && !wheel_row_live(next))) continue;       // greyed: stepped over, as the menu does
            pc_call(0x16a3a8, menu, menuitem_hash(next));       // FlailerMenu::SetActiveMenuItem
            pc_call(0x16e038, msg == 9 ? 4 : 3);        // MakeFrontendSound: up / down
            stepped = true;
            break;
        }
        if (!stepped) SW(data + 12, 0);                 // nowhere to go: no sound of a step either
        return;
    }
    // The screen as the wheels' (above): a player's Advanced, Calibration and Force Feedback.
    if (g_wheel_screen && row >= 1 && row <= 6 && (msg == 1 || msg == 6 || msg == 7)) {
        const int player = row >= 4 ? 1 : 0, kind = (row - 1) % 3;      // 0 Advanced, 1 Calibration, 2 Force Feedback
        if (pc_wheel_of(player) < 0) {
            // (the rows of a player with no wheel are greyed; if one is pressed all the same, it says why nothing comes of it)
            blurb_text(self + 224, player ? "Player 2 has no steering wheel." : "Player 1 has no steering wheel.");
            pc_call(fn::BlurbAnimate, self + 224);
            SW(data + 12, 0);
        } else if (kind == 0) {
            if (msg == 1) {
                g_wheel_back_row = row;
                pc_open_page(player ? "PC Wheel 2" : "PC Wheel 1");
            } else {
                SW(data + 12, 0);
            }
        } else if (kind == 1) {
            if (msg == 1) {
                // The game's own way to its calibration screen: its handler's Select of this row (0x240564) changes the screen
                // from inside itself, for the row's own player, who is put in as the one who pressed. (Four builds opened the
                // screen from here with the port's own call of the same ChangeToScreen, and nothing came up: the user,
                // 2026-11-24, "still does not open". Why that call does not do what the game's does is not known; the game's
                // own did open the screen in the build of 19:52. speed.log has what the port's device was, and what screen
                // was up half a second later.)
                const u32 type = pc_call(0x154308, (u32)player);
                fprintf(stderr, "menus: the wheels' Calibration, player %d: wheel %d, the port's device type %u, plugged in %u, can be calibrated %u\n",
                        player + 1, pc_wheel_of(player), type, pc_call(0x1542f0, (u32)player), pc_call(0x11d430, type));
                const u32 pressed_by = LW(data + 4);
                SW(data + 4, (u32)player);
                g_wheel_back_row = row;
                orig(c);
                SW(data + 4, pressed_by);
                g_calibration_watch = 30;
            } else {
                SW(data + 12, 0);
            }
        } else {
            g_cfg.wheel_ffb[player] ^= 1;
            pc_config_save();
            if (msg != 1) SW(data + 12, msg == 6 ? 1 : 2);
            wheel_rows(self, false);
        }
        return;
    }
    // Calibration (rows 2 and 5: the game's MakeCalibrationScreenAvailable names MENUITEM_2 and _5) with a device the game has
    // nothing to calibrate in, a gamepad: that player's own page of the gamepad's settings (the user, 2026-11-24; pcsettings.cpp,
    // "PC Pad 1" and "PC Pad 2"). A wheel, which the game does calibrate, goes the game's way.
    // (Whatever the port's device is at the moment: this is the gamepads' screen, and a wheel's calibration is on the wheel's
    // own page. The first build asked the game whether the device can be calibrated, and with a wheel there the row went on
    // opening the wheel's screen: the user, 2026-11-24, "Calibration still has the wheel".)
    const bool pad_setup_row = row == 2 || row == 5;
    if (msg == 1 && pad_setup_row) {
        pc_open_page(row == 5 ? "PC Pad 2" : "PC Pad 1");
        return;
    }
    // A player's rows answer that player's own device in the game: the handler compares the port the press came from (the
    // callback data's +4) with the row's player, and drops another's. With one keyboard or one gamepad nobody could open player
    // 2's layouts or switch their vibration (the user, 2026-11-24: "player 2's layouts do not open"). Here a row is its
    // player's whoever pressed; the layouts' own screen is made to listen the same way (HOOK_JoystickConfigHandleScreenCallback).
    const bool own_row = data && row >= 1 && row <= 6 && (msg == 1 || msg == 6 || msg == 7);
    const u32 pressed_by = own_row ? LW(data + 4) : 0;
    if (own_row) SW(data + 4, row >= 4 ? 1 : 0);
    orig(c);
    if (own_row) SW(data + 4, pressed_by);
    if (msg == 4 && pad_setup_row && !g_wheel_screen) {
        blurb_text(self + 224, row == 5 ? "The dead zones, the steering and the vibration of player 2's gamepad."
                                        : "The dead zones, the steering and the vibration of player 1's gamepad.");
        pc_call(fn::BlurbAnimate, self + 224);
    }
    // The frame of the highlighted row (the lines over and under it, the row's mask in the selection set) is the base screen's,
    // OptionsScreenBase::HandleHighlight (0x1f4e68: this, message, data, the row's mask), called before this handler with no
    // mask: it then finds the mask by the row, and knows MENUITEM_1..7 only. For the eighth it took no object at all and put
    // the lines at the top of the screen (the user's picture, 2026-11-23: a yellow line up there with player 2's Select Device
    // chosen). So it is called again here for that row, with its mask named.
    if (msg == 4 && row == 8) pc_call(0x1f4e68, self, 4, data, pc_hash("MI_8_MASK"));
    if (msg == 4 && (row == 7 || row == 8) && !g_wheel_screen) {
        blurb_text(self + 224, row == 7 ? "Select, then choose the gamepad of player 1." : "Select, then choose the gamepad of player 2.");
        pc_call(fn::BlurbAnimate, self + 224);
    }
    if (msg == 4 && g_wheel_screen && row >= 1 && row <= 8) {
        // the wheels' screen: what each row is
        const char *text = !wheel_row_live(row) ? (row == 8 || (row >= 4 && row <= 6) ? "Player 2 has no steering wheel." : "Player 1 has no steering wheel.")
                         : row == 7 ? "Select, then choose the steering wheel of player 1."
                         : row == 8 ? "Select, then choose the steering wheel of player 2."
                         : row % 3 == 1 ? "The forces and the rotation of this player's wheel."
                         : row % 3 == 2 ? "The game's own calibration of the wheel and its pedals."
                         : "Force feedback of this player's wheel, on or off.";
        blurb_text(self + 224, text);
        pc_call(fn::BlurbAnimate, self + 224);
    }
    if (msg == 4) g_device_row = row == 7 || row == 8 ? row : 0;
    if (msg == 18 || msg == 4) controller_device_layout(self, false);
    if (g_wheel_screen && msg == 4 && row >= 1 && row <= 8 && !wheel_row_live(row)) g_rows_look = true;     // (off a greyed row)
    if (g_wheel_screen) wheel_rows(self, msg == 18);
    else if (msg == 18 || msg == 4) pad_words(true);
}

// JoystickPreConfig::UpdateMenuItems (0x240870: this, the port, a bool): the rows a player's device allows. The game opens
// Calibration to what it can calibrate (a wheel: CanJoystickBeCalibrated) and greys it for a gamepad. Here the row is every
// device's: a gamepad's is the port's page of that player's settings (HOOK_JoystickPreConfigHandleEvent, rows 2 and 5).
void HOOK_JoystickPreConfigUpdateMenuItems(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 self = c.r[4].w[0], port = c.r[5].w[0];
    orig(c);
    pc_call(0x240988, self, port, 1);       // MakeCalibrationScreenAvailable(this, port, true)
    if (g_wheel_screen && port < 2) wheel_rows_live((int)port, LW(self + 100));        // the screen as the wheels': its own rows
}

// The layouts' list is the pad type's, ten at most, and the same for both players: the eight of the game's, the modern one
// and player 1's own (ps2.cpp). A player's screen must have that player's own as the tenth, so while a function of the
// layouts' screen runs (JoystickConfigScreen +0 is its port) the list read for the DualShock 2 (GetJoystickConfigs,
// 0x155040: the buffer, its room, the pad's type; the count back) has player 2's number in that place for player 2's screen.
// Until 2026-11-25 the list had both players' and each screen stepped over the other's; with the modern layout there are
// eleven, and the list has ten places. A layout's letter is its place in this list, so the modern one is I and a player's
// own J, for both players (the user, the same day: "let the Custom layout be J").
static int g_config_port = -1;
struct ConfigPort {
    int before;
    explicit ConfigPort(u32 screen) : before(g_config_port) { g_config_port = screen ? (int)(LW(screen) & 1) : -1; }
    ~ConfigPort() { g_config_port = before; }
};

void HOOK_GetJoystickConfigs(Ctx &c, void (*orig)(Ctx &))
{
    const u32 list = c.r[4].w[0], type = c.r[6].w[0];
    orig(c);
    if (type != 0x79 || g_config_port != 1) return;
    const u32 n = c.r[2].w[0];
    for (u32 i = 0; i < n && i < 10; i++)
        if (LW(list + i * 4) == PC_CUSTOM_LAYOUT) SW(list + i * 4, PC_CUSTOM_LAYOUT + 1);
}

// ChooseJoystickConfigurationNumber (0x1550d0: the pad's type, the layout wanted): the layout if the type's list has it, else
// the list's first. Player 2's own is not in the list as it is kept, and is a DualShock 2's layout all the same.
void HOOK_ChooseJoystickConfig(Ctx &c, void (*orig)(Ctx &))
{
    if (c.r[4].w[0] == 0x79 && c.r[5].w[0] == PC_CUSTOM_LAYOUT + 1) {
        c.r[2].d[0] = PC_CUSTOM_LAYOUT + 1;
        return;
    }
    orig(c);
}

// JoystickConfigScreen::InitCurrentDisplayedConfig (0x23e0b0: this): the layout the screen shows first, the port's if the list
// has it. From the screen's constructor too, where no other hook of the screen is around it.
void HOOK_InitCurrentDisplayedConfig(Ctx &c, void (*orig)(Ctx &))
{
    const ConfigPort whose(c.r[4].w[0]);
    orig(c);
}

// JoystickConfigScreen::HandleScreenCallback (0x23e148: this, message, ScreenCallbackData): a player's layouts, A to D. The game
// hears the screen's own player alone there (+0 is the screen's port; Back from another port is dropped), so a screen opened by
// the other player's device could not even be left. Every device is the screen's player here.
void HOOK_JoystickConfigHandleScreenCallback(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0], data = c.r[6].w[0];
    const bool mine = self && data;
    const ConfigPort whose(self);
    // Select (message 1) is "take this layout and go back" there, for the player's own layout as for any other. The page
    // where its buttons are chosen (pcsettings.cpp, "PC Layout 1" and "PC Layout 2") opens by Delete, or Square / X on a
    // gamepad (the user, 2026-11-25: "let Enter / Cross / A confirm the choice, and Square / X / Del open the editing"; until
    // then Select opened the page and the layout could only be taken by Back). Asked at every tick of the screen (message
    // 18), so that a press is one press; the layout shown is the port's already (CycleConfiguration sets it at each step).
    // The pointer's click on the hint under the list is the same press (mouse_frame).
    bool edit = false;
    if (self && c.r[5].w[0] == 18) {
        edit = window_delete_pressed();
        edit = g_mouse_edit || edit;
        g_mouse_edit = false;
    }
    if (edit && pc_custom_layout(LW(self + 4))) {
        g_pc_ctx = &c;
        pc_open_page(LW(self) & 1 ? "PC Layout 2" : "PC Layout 1");
        return;
    }
    const u32 pressed_by = mine ? LW(data + 4) : 0;
    const bool tick = self && c.r[5].w[0] == 18;
    if (mine) SW(data + 4, LW(self));
    orig(c);
    if (mine) SW(data + 4, pressed_by);
    // (the screen sets its help at every tick, SetHelpOverride, 0x16d448, with nothing for "the screen's own list": after it)
    if (tick && pc_custom_layout(LW(self + 4))) {
        static u32 lines;
        g_pc_ctx = &c;
        if (!lines) lines = pc_keep(16), SW(lines, 801601), SW(lines + 4, (u32)kHelpEdit), SW(lines + 8, 801602);
        pc_call(0x16d448, lines);
    }
}

// FlailerScreenList::JoyHandler (0x16cab0: the port, the event, pressed, the list): every button of the menus comes through here.
// A screen made for one player hears that player's port alone (FlailerScreen's port, +16: FlailerScreen::JoyHandler, 0x16aa00,
// and IsPortFiltered, 0x16b3b0, drop the others), and a player's layouts ("Joystick Config Port2") and calibration are such
// screens: opened from player 1's device or the keyboard, they heard nothing of it, not even Back (the user, 2026-11-24:
// "player 2's layouts open, but I cannot leave the menu"). So while one of those screens is up, a button of any port is the
// screen's own player's. (The first try gave the screen itself to every port, SetScreenFilterPort with -1. But that port is
// also whose screen it is: FlailerScreen::Activate hands it to the screen's callback, which made the layouts of "player -1",
// and both players' screens said "No configs available": the user's picture of the same day.)
void HOOK_JoyHandler__17FlailerScreenList12JoystickPort8JoyEventUcPv(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const std::string name = screen_name();
    if (name.rfind("Joystick Config Port", 0) == 0 || name.rfind("Joystick Calibration Port", 0) == 0)
        c.r[4].d[0] = name.back() == '2' ? 1 : 0;
    orig(c);
}

// The highlight's flare (tHighlightAnimation, 0x248870: on a row's highlight a copy of the row's text swells and fades over it) is
// a text object of each screen, 0xab671c9a. Animate (0x248970) copies the row into it with CopyFlailerText (0x1410a0), which takes
// the object's 64 bytes from +40: the font, the format, the colour, the LanguageID and only the first four bytes of the string.
// The game's rows have a LanguageID, and the copy shows the row's word. A text the port writes has none (SetString clears it), so
// the copy was its first four letters and then what the flare object had before: a word of another length, swelling round the
// wrong middle (the user, 2026-11-26: "the highlight's animation does not match the row's name, on every screen"). The string is
// now copied whole (HOOK_CopyFlailerText), and here the flare object of every screen gets room for it: Options' had 5 letters.
// A copy with 40, appended to the screen; the old one loses its name, so the game finds the new one (FindFlailerObject by name).
static const u32 kHighlightText = 0xab671c9a;
static void roomy_highlight_texts(u32 project)
{
    u32 sl = project + 12;
    for (u32 s = LW(sl); s && s != sl; s = LW(s)) {
        const u32 o = find_object(s, kHighlightText, 1);
        if (!o || LW(o + 84) >= 40) continue;
        clone_object(s, o, 40);
        SW(o + 8, 0);
    }
}

// CopyFlailerText (0x1410a0: to, from), the flare's copy above. A text with no LanguageID shows its own string: that is copied
// whole, as far as `to` has room (its MaxChars before the copy, which the copy overwrites with `from`'s), and `to` keeps its own
// MaxChars. A text with a LanguageID is the game's copy as it was.
void HOOK_CopyFlailerText(Ctx &c, void (*orig)(Ctx &))
{
    const u32 to = c.r[4].w[0], from = c.r[5].w[0];
    const u32 room = to ? LW(to + 84) : 0;
    orig(c);
    if (!to || !from || LW(from + 96) != 0 || room < 2) return;
    const u32 from_room = LW(from + 84);
    u32 n = 0;
    while (n + 1 < room && n < from_room && LB(from + 100 + n)) {
        SB(to + 100 + n, LB(from + 100 + n));
        n++;
    }
    SB(to + 100 + n, 0);
    SW(to + 84, room);
}

// The car select screen on a wide picture (the pilot of wide menus, native.cpp g_carsel_now; the user, 2026-11-27: "to the
// edges"): the hints at the bottom left (Help, Back) by the left edge; the colours' column by the right edge; everything else
// (the car's name, the cars' strip and its arrows, the specs, the counts, the locked panel) in the middle, all of it
// together. The sheets across the whole screen and the grid are not these (unstretch_2d).
// The same for the two players' car select (SELECT_CAR_2P, 0x18dc8e52: a half of the screen each, one above the other, each with its
// showroom, strip, specs and colours; the lower half is 0x18dc8e53, player 2's): the user, 2026-11-27, "as on
// one player".
static void carsel_parts(u32 project)
{
    // (the car's name, CAR_LOGO, was by the left edge till the user's picture of 2026-11-27: "let the cars' names stay as on 4:3";
    // its picture is 832 texels across, the name, a red line and the NFS mark: in the middle with the rest now)
    const u32 right[] = {pc_hash("SWATCH_FRAME"), pc_hash("UP_ARROW"), pc_hash("DOWN_ARROW")};
    int counts[3] = {};
    for (u32 hash : {0xb5074ab4u, 0x18dc8e52u, 0x18dc8e53u}) {
    const u32 s = find_screen(project, hash);
    if (!s) continue;
    for (u32 ol = s + 32, o = LW(ol); o && o != ol; o = LW(o)) {
        const u32 name = LW(o + 8);
        const std::string nm = pc_str(o + 16);
        float k = 0.5f;
        for (u32 h : right) k = name == h ? 1.0f : k;
        if (nm.rfind("SWATCH_", 0) == 0) k = 1.0f;
        if (fl(o + 48) < 200.0f && fl(o + 52) > 400.0f && fl(o + 52) < 480.0f) k = 0.0f;      // the hints at the bottom left
        // The two players' car select, his "variant 2" (2026-11-27, on his picture: "move the text, the selector and the cars
        // more organically"): each half's specs and car name (with its class and line) by the left edge; the cars in the middle
        // (native.cpp, NativeFrame::view_shift). The halves are two screens: 0x18dc8e52 the upper (player 1), 0x18dc8e53 the lower
        // (player 2: FADEOVERLAYP2, y 240..480). The strip of cars stays in the middle as 4:3 has it (his next picture: "leave
        // player 1's selector where player 2's is now", that is, in the middle; it had gone to the right edge).
        if (hash == 0x18dc8e52u || hash == 0x18dc8e53u)
            if (nm.rfind("SPEC", 0) == 0 || nm == "CAR_LOGO" || nm == "CAR_LOGO_NFS" || nm == "CLASS" || nm == "CLASS_NAME" || nm == "ORANGELINE")
                k = 0.0f;
        native_menu_part(o, k);
        counts[k == 0.0f ? 0 : k == 1.0f ? 2 : 1]++;
    }
    }
    fprintf(stderr, "menus: the car select screens on a wide picture: %d objects by the left edge, %d in the middle, %d by the right\n", counts[0],
            counts[1], counts[2]);
    // Its windows (the transmission's choice, SELECT_CAR_DIALOG, and the two of the same look, 0x6344da01 and 02: the handling's):
    // their bands (the title's bar, the red line, the sheet of the choices, the white line under it, at 86 of 640) part 2, as the
    // game has them (the same fractions of the wide screen as of 4:3); the title (POPUP_LINE_1) part 3 and the choices (POPUP_1..3)
    // part 4: their left ends at the game's fraction, their letters at the 4:3 size; the flash part 5, by its row (native.cpp,
    // unstretch_2d; the user's pictures, 2026-11-27). Not the dark sheet over the whole screen (at 0 or
    // -1: across as it is), nor a band the window keeps off the screen (876..).
    int bands = 0, texts = 0;
    const u32 title = pc_hash("POPUP_LINE_1"), choices[] = {pc_hash("POPUP_1"), pc_hash("POPUP_2"), pc_hash("POPUP_3")};
    for (u32 hash : {0x41c2fdc3u, 0x6344da01u, 0x6344da02u})
        if (const u32 d = find_screen(project, hash))
            for (u32 ol = d + 32, o = LW(ol); o && o != ol; o = LW(o)) {
                const u32 name = LW(o + 8);
                if ((s8)LB(o + 40) != 1) {
                    if (fl(o + 48) > 50.0f && fl(o + 48) < 100.0f) native_menu_part(o, 2.0f), bands++;
                } else if (name == title) {
                    native_menu_part(o, 3.0f), texts++;
                } else if (name == choices[0] || name == choices[1] || name == choices[2]) {
                    native_menu_part(o, 4.0f), texts++;
                } else if (name == kHighlightText) {
                    // the selection's flash, the copy of the chosen row the game blows up from its middle (roomy_highlight_texts'
                    // copy by now): placed by its row (the user, 2026-11-27: "now the selection's animation slid off")
                    native_menu_part(o, 5.0f), texts++;
                }
            }
    fprintf(stderr, "menus: the car select's windows: %d bands as 4:3 has them, %d texts in them\n", bands, texts);
}

// The wide menus (native.cpp, g_wide_menu_now): the main menu first (2026-11-27), then every screen with its background (the user,
// the same day: "can you make all the other screens the same?", by groups: first these). The screens of FRONTB with the main
// menu's background, the grid, the blueprints, the fade, the top bar (and most the video panel): the modes, Challenge and its
// event tree, Race Setup, Options and its screens, the gamepad's, the profiles', the jukebox, the credits. And the port's own pages
// (copies of Options' screens).
static const u32 kWideMenus[] = {
    MAIN_MENU_SCREEN, 0x5b35f533 /* COPS_ON_MODE_SELECT */, 0x033e7771 /* COPS_OFF_MODE_SELECT */, 0xe55f1297 /* CHALLENGE_BE_A_COP */,
    0x2d62cfe2 /* CHALLENGE */, 0xd1eadbca /* RACE_SETUP */, 0xb6657464 /* JOYSTICK_CONFIG */, OPTIONS_SCREEN, 0x28891a22 /* CUSTOMIZE_HUD */,
    DISPLAY_SCREEN, 0xa4031ef6 /* JOYSTICK_CALIBRATION */, 0xe116761c /* AUDIO_OPTIONS */, 0x617a4e0a /* USERNAME_OPTIONS */,
    0x8bd5060b /* JOYSTICK_PRECONFIG */, 0x95b2b627 /* JUKEBOX */, 0x147f8a59 /* the event tree */, 0x39c4c62c /* the credits */,
    // the third group (the user, 2026-11-27, "do the other screens"): the Challenge event's window over the event tree (its video,
    // bars and texts), an event's blurb and its reward over the tree, the language's window in Options, the language's flags
    // at the first start, the credits' text page. Not the EA legal page (0xdd4197c9, a picture of text: 4:3), nor the calibrate
    // screens (the port hides their row; the other is unreachable in retail: his word, not to be touched).
    0x492e86e9 /* the event's window */, 0xfd297fd4 /* an event's blurb */, 0xf1eed4cd /* its reward */, 0x632b068e /* LANGUAGE_OPTIONS */,
    0xadb8e59e /* the flags at the first start */, 0xbec7cd92 /* the credits' text */,
};
bool pc_wide_menu(u32 hash)
{
    if (!hash) return false;
    for (u32 h : kWideMenus)
        if (h == hash) return true;
    for (PcPage *pg : pages())
        if (page_hash(*pg) == hash) return true;
    return false;
}
// The windows over a screen, each a screen of its own (the question with two answers, with one, the list of a few rows, the memory
// card's message): over a wide menu the screen under them stays wide and they stand in the middle, their dark sheet across (the
// user, 2026-11-27: "the window in the middle").
bool pc_popup_screen(u32 hash)
{
    return hash == 0x037f3133 || hash == 0x74d2dfa2 || hash == 0xfd8a3b6d || hash == 0x9829bfab || hash == 0x0ce3f7ce;
}

// A wide menu's parts: its button hints at the bottom left (Help, Back, the port's Edit) by the screen's left edge, and the game's
// logo at the bottom right (the main menu's) by its right edge (the user, 2026-11-27: "the buttons' widgets at the screen's edge",
// "leave the logo at the right edge"; for all screens: "the hints at the bottom left by the left edge, the logo by the right where
// it is shown"). The rest stays in the 4:3 block, cut at its edges; what is not here is not by an edge. A hint is a text at the
// hints' height (the game's at 427 and 431, x 24 and 54; the port's 88 and 176 further on), not a row of the menu (the audio
// screen's last rows are at 403 and 439) and, on the credits, not a line of the credits running up (at 101).
static void wide_menu_parts(u32 project)
{
    const u32 logo = pc_hash("TITLE_SCREEN_TITLE");
    int hints = 0, screens = 0;
    for (u32 hash : kWideMenus)
        if (const u32 s = find_screen(project, hash)) {
            screens++;
            for (u32 ol = s + 32, o = LW(ol); o && o != ol; o = LW(o)) {
                if (LW(o + 8) == logo) { native_menu_part(o, 1.0f); continue; }
                const float x = fl(o + 48), y = fl(o + 52);
                if (hash == 0xbec7cd92) continue;       // (the credits' text page: no hints, its lines run up past them)
                // (on the credits only what is left of 100, as its lines run up past the hints' row: and the port's Back by its name,
                // which stands at 113 / 142, else it stayed in the middle, far from Help: the user, 2026-11-28)
                const bool port_back = LW(o + 8) == pc_hash("PC_BACK_ICON") || LW(o + 8) == back_text_hash();
                if ((s8)LB(o + 40) != 1 || y < 400.0f || y > 445.0f || x >= (hash == 0x39c4c62c && !port_back ? 100.0f : 260.0f)) continue;
                if (pc_str(o + 16).rfind("MENUITEM", 0) == 0) continue;
                native_menu_part(o, 0.0f);
                hints++;
            }
        }
    for (PcPage *pg : pages())
        if (const u32 s = find_screen(project, page_hash(*pg))) {
            screens++;
            for (u32 ol = s + 32, o = LW(ol); o && o != ol; o = LW(o)) {
                const float x = fl(o + 48), y = fl(o + 52);
                if ((s8)LB(o + 40) != 1 || y < 415.0f || y > 445.0f || x >= 260.0f || pc_str(o + 16).rfind("MENUITEM", 0) == 0) continue;
                native_menu_part(o, 0.0f);
                hints++;
            }
        }
    fprintf(stderr, "menus: %d wide menu screens, %d hints by the left edge\n", screens, hints);
}

// The track select screen on a wide picture (native.cpp, g_tracksel_now; the user, 2026-11-27: the track's picture "as on 4:3 in
// the middle", the figures and the rows "by the edges"). By the left edge (0): the track's figures and their labels (length,
// elevation, difficulty, the best times, the track's logo and its lines) and the hints. By the right edge (1): the time of day's
// column (its pictures, its frame, its arrows), the rows Laps / Time of Day. The selection's flash (5) as the text it copies. The track's picture (6) in
// the middle at its 4:3 size. The lap window and the "locked" window slide in as the car select's windows (2: their bands as the
// game has them, the same fractions as on 4:3; 3: their texts, arrows and lock at 4:3 size about their left ends). The sheets
// across the whole screen (the grid, its shadows, the mask over the picture's top, the locked mask) have no part; the rest (the
// thumbnails' film, its frame and arrows, the thumbnails) in the middle (0.5).
static void tracksel_parts(u32 project)
{
    const u32 s = find_screen(project, 0x15417b53);
    if (!s) return;
    static const char *const right[] = {"TOD_3PM", "TOD_NIGHT", "TOD_OVERCAST", "TOD_SUNSET", "TOD_FRAME", "MENUITEM_1", "MENUITEM_2",
                                        "UP_ARROW", "DOWN_ARROW"};
    static const char *const left[] = {"TRACK_LENGTH", "TRACK_ELEVATION", "TRACK_DIFFICULTY", "BEST_TOTAL_TIME", "BEST_LAP_TIME",
                                       "BEST_TOTAL_TIME_STATS", "BEST_LAP_TIME_STATS", "TRACK_LOGO", "DIVERBAR", "DIVIDER"};
    static const char *const bands[] = {"SELECTLAP_TOP", "SELECTLAP_BG", "SELECTLAP_TOPBAR", "SELECTLAP_LOWERBAR", "LOCKED_BG1",
                                        "BLURB_BOX", "BOOGLE",
                                        "LOCKED_BG2", "LOCKED_BOTTOMLINE", "LOCKED_MIDLINE", "LOCKED_TEXTPOLY", "LOCKEDHEAD_BGBLUE"};
    static const char *const window[] = {"POPUP_LINE_1", "LAP_COUNT", "LAP_LEFT_ARROW", "LAP_RIGHT_ARROW", "LOCKED", "LOCKED_HEADING",
                                         "LOCKED_TEXTTEST"};
    static const char *const across[] = {"MASK_THE_PICTURE", "GRID_SHADOW", "GRID", "LOCKED_MASK"};
    static const char *const picture[] = {"TRACK_IMAGE_BOO", "TRACK_IMAGE_XFADE"};
    auto in = [](const std::string &nm, const auto &list) {
        for (const char *x : list)
            if (nm == x) return true;
        return false;
    };
    int counts[5] = {};
    for (u32 ol = s + 32, o = LW(ol); o && o != ol; o = LW(o)) {
        const std::string nm = pc_str(o + 16);
        const float x = fl(o + 48), y = fl(o + 52);
        const bool text = (s8)LB(o + 40) == 1;
        float k = 0.5f;
        if (in(nm, across)) continue;
        if (in(nm, picture)) k = 6.0f;
        else if (in(nm, bands)) k = 2.0f;
        else if (in(nm, window)) k = 3.0f;
        else if (LW(o + 8) == kHighlightText) k = 5.0f;         // the flash: as the text it copies (the lap count too)
        else if (in(nm, right)) k = 1.0f;
        else if (in(nm, left) || (text && x < 100.0f && y < 400.0f)) k = 0.0f;
        else if (text && x < 260.0f && y >= 415.0f && y <= 445.0f) k = 0.0f;        // the hints at the bottom left
        native_menu_part(o, k);
        counts[k == 0.0f ? 0 : k == 1.0f ? 2 : k == 0.5f ? 1 : k == 6.0f ? 4 : 3]++;
    }
    fprintf(stderr, "menus: the track select screen on a wide picture: %d by the left edge, %d in the middle, %d by the right, %d in its "
            "windows, %d the picture\n", counts[0], counts[1], counts[2], counts[3], counts[4]);
    // The track's blurb (0xa744ba7b, a box at 140..640 with its texts over the track select): as its lap window, sliding in by the
    // same fractions (the box and its edge 2, the texts 3). native.cpp keeps the track select's frame while it is up.
    if (const u32 b = find_screen(project, 0xa744ba7b))
        for (u32 ol = b + 32, o = LW(ol); o && o != ol; o = LW(o))
            native_menu_part(o, (s8)LB(o + 40) == 1 ? 3.0f : 2.0f);
}

// The windows over the car select and the track select (the question, its one-answer kind, the short list, the memory card's
// message): as the car select's own windows, sliding in by the same fractions (the user's choice for the windows, 2026-11-27):
// their bands (the sheet, the title's bar, the lines, from 50..200 of 640) 2, their texts 3, the flash 5; the dark sheet over the
// whole screen (at 0) has no part and runs across. Only the car / track select read these (native.cpp: there the window keeps the
// screen's frame); over the main menu's screens they keep its rules, which leave a part above 1 alone.
static void window_parts(u32 project)
{
    int n = 0;
    for (u32 hash : {0x037f3133u, 0x74d2dfa2u, 0xfd8a3b6du, 0x9829bfabu, 0x0ce3f7ceu})
        if (const u32 w = find_screen(project, hash))
            for (u32 ol = w + 32, o = LW(ol); o && o != ol; o = LW(o)) {
                const float x = fl(o + 48);
                if (LW(o + 8) == kHighlightText) native_menu_part(o, 5.0f), n++;
                else if ((s8)LB(o + 40) == 1) native_menu_part(o, 3.0f), n++;
                else if (x > 50.0f && x < 200.0f) native_menu_part(o, 2.0f), n++;
            }
    fprintf(stderr, "menus: the windows over the car and track select: %d objects\n", n);
}

static void port_edits(u32 tail)
{
    if (LW(tail + 8) != FRONTB_PROJECT) return;
    g_page_alloc = true;
    for (PcPage *pg : pages()) build_page(tail, *pg);
    add_exit_row(tail);
    add_device_rows(tail);
    add_master_row(tail);
    add_back_hints(tail);
    if (!g_profiles_page.empty()) add_delete_hint(tail, g_profiles_page);
    // The layouts' screen (Joystick Config): Help's icon is the buttons' font's 'k' and Back's 'j', Circle's and Triangle's cells, which
    // the layouts themselves show as the gamepad's. There the hints have cells of their own, three of the sticks' ('p' Back, 'v'
    // Help, 'u' the button that edits a layout of the player's own): pc_paint_buttons.
    if (u32 lay = find_screen(tail, CONFIG_SCREEN)) {
        u32 help_icon = 0, help_word = 0;
        for (u32 o = LW(lay + 32); o && o != lay + 32; o = LW(o)) {
            if ((s8)LB(o + 40) != 1) continue;
            if (LW(o + 96) == 12000) help_word = o;
            else if (LW(o + 80) == 0xb85de386 && pc_str(o + 100) == "k" && fl(o + 52) > 400.0f) help_icon = o;
        }
        if (u32 o = find_object(lay, pc_hash("PC_BACK_ICON"), 1)) set_text(o, "p");
        if (help_icon) pc_call(fn::SetString, help_icon, gstr("v"));
        g_help_icon = help_icon;
        add_edit_hint(lay, help_icon, help_word);
        add_info_hint(tail, lay, help_word);
    }
    if (!g_profiles_page.empty()) add_name_grid(tail, g_profiles_page);
    g_project_gen++;
    add_options_arrows(tail);
    roomy_highlight_texts(tail);
    carsel_parts(tail);
    tracksel_parts(tail);
    window_parts(tail);
    wide_menu_parts(tail);
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
    if (g_hud_project && !find_project(g_hud_project)) g_hud_project = 0, native_hud_part(0, nullptr);
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
            if (pg->rows.size() >= 7) pc_call(fn::GrayMenuItem, menuitem_hash(7), 1, hash);     // (by the rows it has now, not those of before)
            // (its scroll kept: the game puts the highlight back on the slot it had, as on its own screens, so the rows stay as
            // they were and the highlight comes back to the last row used; page_refresh keeps it within the rows there are now.
            // It was set back to the top here, and the highlight came back on another row: the user, 2026-11-28)
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

// Display's list ends at its row 9 for the game: HandleEvent (0x1f3a28) refuses Down only there (Param 1) and Up on row 1. With
// row 9 hidden (above), Down on row 8 went on to the menu's next row shown, row 1: the highlight went round to the top and the
// list slid back up, while the list's mask stayed on the bottom slot (it moves by Up and Down within the 7, it has no round).
// The user, 2026-11-28: "the selection frame sticks and the text highlight goes on, though we removed the round" on HUD. Now
// row 8 is the end, as row 9 was: Down refused there, as the game does at its own last row.
static void display_event(Ctx &c, void (*orig)(Ctx &))
{
    const u32 msg = c.r[5].w[0], data = c.r[6].w[0];
    orig(c);
    if (msg == 9 && LW(data) == menuitem_hash(8)) SW(data + 8, 1);
}
static const int DISPLAY_SCROLL = 108;      // tDisplayOptionsScreen's tScrollingMenuList (mMenuScroller)
void HOOK_HandleEvent__21tDisplayOptionsScreeniP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    game_list_event(c.r[4].w[0] + DISPLAY_SCROLL, c.r[5].w[0], c.r[6].w[0], c, orig, display_event);
}

// "<button> Edit" at the bottom of the layouts' screen (add_edit_hint): shown with a layout of the player's own only
static void config_edit_hint(bool show)
{
    const u32 s = find_menu_screen(CONFIG_SCREEN);
    if (!s) return;
    for (const u32 o : {find_object(s, pc_hash("PC_EDIT_ICON"), 1), find_object(s, pc_hash("PC_EDIT_WORD"), 1)})
        if (o && ((s8)LB(o + 41) == 1) != show) pc_call(fn::SetVisibility, o, show ? 1 : 0);
}

// The layouts' screen's Help with a layout of the player's own on it: the screen's own two lines (801601 Left / Right "Select
// controller configuration", 801602 "Back to Controller Options": the list at 0x31a668) and between them a line of the
// port's for the button that edits (the user, 2026-11-25: "do not forget to update the Help screen there"). The lines are
// LanguageIDs, and the game's table has no room for a new one (pcplatform.cpp, patch_language, can only replace), so the id
// is the port's own, the first free after the screen's, and GetTranslatedString (0x21e560: the id) answers it here. Its icon
// is the help character 0x93, the cell 'u' of the buttons' font: the same button as the hint at the bottom shows.
void HOOK_GetTranslatedString(Ctx &c, void (*orig)(Ctx &))
{
    if ((int)c.r[4].w[0] != kHelpEdit) {
        orig(c);
        return;
    }
    g_pc_ctx = &c;
    static u32 text;
    if (!text) text = pc_keep(128);
    const std::string s = "\x93 " + tr("Edit the buttons of this layout");
    memcpy(host(text), s.c_str(), std::min<size_t>(s.size() + 1, 127));
    c.r[2].d[0] = sx32(text);
}

// The layouts' screen shows a picture of the controller, the texture CONTROLLER<the layout's id>: 30 to 33 are the DualShock 2's. The four
// layouts of the triggers have the ids of the DualShock 1's old ones (20 to 22, with its picture) and a new one (34, with none). They are
// all a DualShock 2's here: its picture goes on after the game has set its own (0x144a28: a screen's object gets a texture by hash).
void HOOK_DrawScreen__20JoystickConfigScreen(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 self = c.r[4].w[0];
    const ConfigPort whose(self);
    orig(c);
    pc_call(0x144a28, LW(self + 0x10), 0x0b2ad6df, pc_hash("CONTROLLER30"));
    // The layout's name is its letter, 'A' and its place in the list ("%c": FlailerSetString, 0x143e70, of the screen +16 and
    // the item +24): the game's own doing. The modern layout is the ninth, I, and the player's own the tenth, J, for both
    // players (the list as a player's screen reads it: HOOK_GetJoystickConfigs). A word there, "Custom", ran over the arrows.
    const bool own = pc_custom_layout(LW(self + 4));
    if (const u32 s = find_menu_screen(CONFIG_SCREEN)) {
        // The letter stands by its left edge, where the game's eight are all about one width; I and J are narrow and stood to
        // the left of the others' middle (the user's picture, 2026-11-25: "line I and J up as the other letters in this
        // space"). They are moved right by half of what they lack to the mean width of A to H (the font's own advances at the
        // object's size, as its GetWidth, 0x140d30, adds them). The object's own place is kept as it was first met.
        if (const u32 o = find_object(s, LW(self + 24), 1)) {
            static int gen = -1;
            static float home;
            if (gen != g_project_gen) gen = g_project_gen, home = fl(o + 48);
            float x = home;
            const u32 font = pc_port_layout(LW(self + 4)) ? pc_call(0x13fe18, LW(o + 80)) : 0;      // FindFont
            if (font) {
                const auto advance = [font](u32 ch) {
                    const u32 g = LW(font + 8 + ch * 4);
                    return g ? fl(g + 20) : 0.0f;
                };
                float mean = 0.0f;
                for (u32 ch = 'A'; ch <= 'H'; ch++) mean += advance(ch) / 8.0f;
                const float mine = advance(LB(o + 100));
                if (mine > 0.0f && mean > mine) x = home + (mean - mine) * fl(o + 56) / 2.0f;
            }
            if (fl(o + 48) != x) set_fl(o + 48, x), SB(o + 43, 5);
        }
        // Steering has two icons with a slash between them, the stick's (event 112: the object 0xcdf8d89a, shown with the
        // slash, 0x7f670764, only when there is one) and the D-pad's (111: 0xff20a4fb, next to the word). A layout where the
        // stick alone steers had the stick's icon far from the word, then the slash, then nothing (the same picture: "move
        // the Steering icon closer to the words and take the slash away"): its icon goes where the D-pad's would be.
        const u32 dpad = find_object(s, 0xff20a4fb, 1), stick = find_object(s, 0xcdf8d89a, 1), slash = find_object(s, 0x7f670764);
        if (dpad && stick && !LB(dpad + 100) && LB(stick + 100) && (s8)LB(stick + 41) == 1) {
            if (fl(dpad + 48) >= fl(stick + 48)) {
                pc_call(fn::SetString, dpad, gstr(pc_str(stick + 100)));
                pc_call(fn::SetVisibility, stick, 0);
            }
            if (slash) pc_call(fn::SetVisibility, slash, 0);
        }
        if (const u32 o = find_object(s, pc_hash("PC_INFO_TEXT"), 1)) {
            const bool show = LW(self + 4) == PC_MODERN_LAYOUT;
            if (((s8)LB(o + 41) == 1) != show) pc_call(fn::SetVisibility, o, show ? 1 : 0);
        }
    }
    {   // the hints' characters by the cells the buttons' font has for them (pc_layout_hint_cells); "edit" without a cell of
        // its own is Square's, which shows the gamepad's button whatever is in use
        static int cells = -2, gen = -1;
        const int now = pc_layout_hint_cells();
        const u32 s = now >= 0 && (now != cells || gen != g_project_gen) ? find_menu_screen(CONFIG_SCREEN) : 0;
        if (s) {
            cells = now, gen = g_project_gen;
            if (const u32 o = find_object(s, pc_hash("PC_BACK_ICON"), 1)) set_text(o, now ? "p" : "b");
            if (g_help_icon) pc_call(fn::SetString, g_help_icon, gstr(now ? "v" : "3"));
            if (const u32 o = find_object(s, pc_hash("PC_EDIT_ICON"), 1)) pc_call(fn::SetString, o, gstr(now ? "u" : "h"));
        }
    }
    config_edit_hint(own);
}

// JoystickConfigScreen::CycleConfiguration (0x23e448: this, the way): the next layout of the list, or the one before. The list
// is the screen's player's (HOOK_GetJoystickConfigs).
void HOOK_CycleConfiguration__20JoystickConfigScreenb(Ctx &c, void (*orig)(Ctx &))
{
    const ConfigPort whose(c.r[4].w[0]);
    orig(c);
}

void HOOK_SetInputMode__6Player16PlayerInputModes(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    {
        // Whose mode: the Player's controller port, +8 (the constructor, 0x175188: +8 the port, +16 the player's number; the
        // handlers below are added on that port). It was taken from +88 till 2026-11-18, and +88 is not a number but the zone
        // camera the player is in (RequestZone writes it, ClearZone clears it): with two players, who have no zones, it was 0
        // for both, so both wrote the first one's cell and the second one's was never written.
        const u32 player = c.r[4].w[0], who = LW(player + 8) & 1, mode = c.r[5].w[0];
        {   // speed.log: every change of a player's mode, with what the game decides it by (a tester's report, 0.14: the car's
            // controls gone after a zone camera in a chase). From: the mode the Player has (+196). The zone camera it is in
            // (+88: 1 and 2 are the frozen ones, mode 3), whether it is locked (+80: the finish, an arrest), the race's state
            // (RaceCoordinator, gp-22216: 3 racing, 6 paused) and who drives its car (Car +32: 1 a person, 2 the AI).
            static int told;
            const u32 was = LW(player + 196), car = LW(player + 4), rc = LW(gp() - 22216);
            if (was != mode && told < 3000)
                told++, fprintf(stderr, "input: port %u, mode %u to %u; zone %u, locked %u, race state %u, car driven by %d\n", who, was, mode,
                                LW(player + 88), LW(player + 80), rc ? LW(rc) : 99u, car ? (int)LW(car + 32) : -1);
        }
        g_player_modes[who] = mode;
        g_player_mode = mode;
        if (pc_two_player_race()) {
            // the mode of a player who drives, if one does; else the first player's
            g_player_mode = g_player_modes[0];
            for (u32 m : g_player_modes)
                if (m == 2 || m == 3) g_player_mode = m;
        }
    }
    static const bool diag = getenv("HP2_DIAG") != nullptr;
    if (diag) fprintf(stderr, "input mode %u (player %08x)\n", g_player_mode, c.r[4].w[0]);
    const u32 player = c.r[4].w[0], asked = c.r[5].w[0];
    orig(c);
    {   // TEMP HP2_BUSTTEST=1: the Busted overlay (Player::StartBustedDisplay, 0x228f08) as the race starts, to see its 2D
        static const bool test = getenv("HP2_BUSTTEST") != nullptr;
        if (test && asked == 2) pc_call(0x228f08, player);
    }
    pc_paint_buttons();
}

// Player::DetermineInputMode (0x177608): every frame for each player (DetermineAllPlayersInputModes, from MainLoop). A safety net
// for a zone camera that has no end coming (a tester's report, 0.14: after the "look around" camera in a chase the car's controls
// were gone). A frozen zone (Player +88 is 1, Freeze, or 2, Preview: the byte table 0x330790) holds the player in mode 3, where the
// car has no controls, and it ends in one way only: World::UpdateWorldPaused (0x14d678) counts the world's pause (World +380) down
// by the real time step and, as it passes 0, calls the one call the world keeps (+396, +400), PlayerClearZone. RequestZone
// (0x177c88) sets that pause to the camera's length less a world step: 3 s for Freeze, for Preview what
// ZonePreviewCameraMover::Duration (0x1637c8) works out for the fly-by. A pause that is not above 0 from the start, or a call that
// is not there, never ends the zone: UpdateWorldPaused does nothing for it. That state held for half a second is ended here, with
// the game's own PlayerClearZone (the camera back, the time scale back, the zone 0), and written to speed.log.
static u32 g_player_car;
u32 pc_player_car() { return pc_racing() ? g_player_car : 0; }

// ---- the mirror's view for the car's own mirrors -----------------------------------------------------------------------
// The glass of the player's car shows the rear-view mirror's picture (native.cpp, mirror_glass_now), and the game makes that
// picture only while the HUD shows its mirror: Player::UpdateHud (0x17ecf0) works out the HUD's features (0x178da0: none before
// the start, in a zone camera, in a replay, with the mirror turned off in the pause menu's display options) and
// Player::SetHudFeatures (0x1796b0), where bit 0x1000 changes, calls Player::SetRearViewMirrorCameraMover (0x1767a8, player, on):
// that deletes the mover of the mirror's view (Player +212), then whatever mover is first in that view's list (the view's
// number is Player +28; the list is eView +60), and, if on, makes a new one for the player's car (48 bytes,
// RearViewMirrorCameraMover(view, car), 0x158f78) and keeps it in +212. A view with no mover is not drawn (eUpdateViewMode,
// 0x107ce0). The features in force are Player +768. So the glass was the game's plain grey until the race's start and then
// filled at once (the user, 2026-11-24).
//
// Here the view has a mover of the port's own while the glass wants the picture and the game has none there (one player,
// Reflections not the PS2's): made as the game makes its own, once the race's 3D is drawn with the player's car. It is NOT kept
// in Player +212: Player::UpdateNonGameState (0x180e60), before the start, takes any of +200, +212, +216, +220, +224 for "the
// player has a camera" and only without one begins the countdown's cameras and then the driving camera. The first try kept the
// game's own mover on through +212, and the camera stayed where the fly-by left it (the user, 2026-11-24: "the camera does not
// go to the race after the fly-by"). The game's own call deletes the port's mover as the first of the view's list, so the
// hook forgets it before the call, and makes it again after a call that turned the game's off. While the port's mover is the
// one there, the HUD's picture of the mirror (DisplayRVMs, 0x10edc8: a quad with the view's target for every mirror view that
// is on) is not drawn. The player's end (its destructor, 0x175bf0) deletes it first.
static bool g_mirror_closing;
static u32 g_mirror_mover, g_mirror_player;         // the port's mover of the mirror's view, and whose
static bool mirror_view_wanted(u32 player)
{
    return !g_mirror_closing && g_cfg.side_mirrors && g_cfg.reflections && pc_racing() && !pc_two_player_race() && player &&
           (LW(player + 8) & 1) == 0 && LW(player + 4) && LW(player + 28);
}

// the port's mover is still one of the view's (the game has not deleted it some way this file does not know)
static bool mirror_mover_alive(u32 player)
{
    if (!g_mirror_mover || !player || !LW(player + 28)) return false;
    const u32 head = pc_call(0x107c90, LW(player + 28)) + 60;          // eGetView(id): the list of its movers
    int n = 0;
    for (u32 m = LW(head); m && m != head && n < 16; m = LW(m), n++)
        if (m == g_mirror_mover) return true;
    return false;
}

static void mirror_mover_delete(u32 player)
{
    if (mirror_mover_alive(player)) {
        const u32 vt = LW(g_mirror_mover + 40);                         // the virtual destructor, as the game calls it
        pc_call(LW(vt + 12), g_mirror_mover + (u32)(s32)(s16)LH(vt + 8), 3);
    }
    g_mirror_mover = 0;
}

static void mirror_mover_make(u32 player)
{
    const u32 mem = pc_call(0x2577c0, 48, 0x2b3a00, 0, 16);            // bMalloc, with the game's own name for it
    if (!mem) return;
    g_mirror_mover = pc_call(0x158f78, mem, LW(player + 28), LW(player + 4));
    g_mirror_player = player;
    static int told;
    if (told < 4) told++, fprintf(stderr, "mirror: the mirror's view is on for the car's own mirrors (the HUD's mirror is off)\n");
}

static void mirror_view_frame(u32 player)
{
    if ((LW(player + 8) & 1) != 0) return;
    if (g_mirror_mover && g_mirror_player != player) g_mirror_mover = 0;       // (another race's: gone with it)
    if (g_mirror_mover && !mirror_mover_alive(player)) g_mirror_mover = 0;
    const bool wanted = mirror_view_wanted(player) && !LW(player + 212);
    // (the mover follows the car it was made for, +44: should the player's car ever be another one, it is made anew)
    if (g_mirror_mover && (!wanted || LW(g_mirror_mover + 44) != LW(player + 4))) mirror_mover_delete(player);
    if (wanted && !g_mirror_mover && native_player_car_drawn()) mirror_mover_make(player);
}

void HOOK_SetRearViewMirrorCameraMover(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 player = c.r[4].w[0];
    const bool off = c.r[5].w[0] == 0;
    if (player == g_mirror_player) g_mirror_mover = 0;      // the call deletes it: the first mover of the view's list
    orig(c);
    if (off && mirror_view_wanted(player) && !LW(player + 212) && native_player_car_drawn()) mirror_mover_make(player);
}

void HOOK_DisplayRVMs(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    if (g_mirror_mover && g_mirror_player && !LW(g_mirror_player + 212)) return;       // the view is on for the glass alone
    orig(c);
}

// ---- the lights of the cars behind, with the mirror off ------------------------------------------------------------------
// The yellow lights of the HUD (Player::DrawTracker, 0x17c278: under the mirror, at the bottom of the screen without it) are
// the cars within 50 m whose Car +2192 is between -0.5 and 0.5. That number is written by the function at 0x135280 (car,
// position, direction), which RenderCars (0x124060) calls for every car of every view it draws, with the view's camera
// (+64 its place, +80 where it looks): (dot + 1) / 2 of the direction and the unit vector from the car to the camera, with
// the sign of their cross product's z. So -0.5..0.5 is "the camera looks at the car". eDisplayFrame draws the players' views
// and then the mirror's (views 1, 2, 3), the last one drawn wins, and with the mirror that is the view backward: the lights
// are the cars behind. With the mirror turned off its view has no mover and is not drawn, the player's own view is the last,
// and the lights showed the cars AHEAD (a tester, 2026-11-25: "they work backwards when it's turned off"; by this code the
// PS2 does the same). Where the port keeps the mirror's view on for the car's own mirrors (above) it never showed.
//
// Here, for one player, the number written in the player's view while the mirror's view is not drawn is made again with the
// camera the mirror would have (RearViewMirrorCameraMover::Update, 0x158ff8: the car's matrix transposed, turned a quarter
// about x and then about y, the car's place taken off, the three numbers at 0x334180 added to the translation): it looks
// against the matrix's first row, from the car's place (+80) less the first of those numbers along the second row, plus the
// second along the third row and the third along the first. Whatever else the function does stays the game's own, with the
// player's camera.
void HOOK_CarTrackerSide(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 car = c.r[4].w[0], place = c.r[5].w[0];
    orig(c);
    const u32 player = LW(gp() - 20928);                                // the first player
    if (!player || !car || LW(gp() - 20920) == 2) return;               // (two players have no such lights)
    const u32 own = LW(player + 4), view = 0x2c95b0 + LW(player + 24) * 128, mirror = 0x2c95b0 + LW(player + 28) * 128;    // eGetView
    if (!own || own == car || !LW(player + 24) || !LW(player + 28) || LW(view + 56) + 64 != place) return;
    const u32 target = LW(mirror + 48);
    if (LB(mirror + 4) && target && LW(target + 8)) return;             // the mirror's view is drawn after this one: its number stays
    const float off[3] = {fl(0x334180), fl(0x334184), fl(0x334188)};
    float at[3], look[3], to[3], len = 0.0f, dot = 0.0f;
    for (u32 i = 0; i < 3; i++) {
        const float fwd = fl(own + 144 + i * 4), across = fl(own + 160 + i * 4), up = fl(own + 176 + i * 4);
        at[i] = fl(own + 80 + i * 4) - off[0] * across + off[1] * up + off[2] * fwd;
        look[i] = -fwd;
        to[i] = at[i] - fl(car + 96 + i * 4);
        len += to[i] * to[i];
    }
    const float norm = sqrtf(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    len = sqrtf(len);
    if (!(len > 0.0f) || !(norm > 0.0f)) return;
    for (u32 i = 0; i < 3; i++) look[i] /= norm, to[i] /= len, dot += look[i] * to[i];
    float side = (dot + 1.0f) * 0.5f;
    if (look[0] * to[1] - look[1] * to[0] >= 0.0f) side = -side;
    set_fl(car + 2192, side);
}

void HOOK_PlayerDestructor(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 player = c.r[4].w[0];
    g_mirror_closing = true;
    if (player && player == g_mirror_player) {
        mirror_mover_delete(player);
        g_mirror_player = 0;
    }
    orig(c);
    g_mirror_closing = false;
}

void HOOK_DetermineInputMode__6Player(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 player = c.r[4].w[0];
    if (player && (LW(player + 8) & 1) == 0) g_player_car = LW(player + 4);     // the first player's car, every frame
    if (player) mirror_view_frame(player);
    if (player) {
        static u32 whose[2];
        static int lost[2];
        const u32 who = LW(player + 8) & 1, zone = LW(player + 88), world = LW(gp() - 22120);
        bool no_end = false;
        if (world && zone && zone < 16 && LB(0x330790 + zone) != 0) no_end = !(fl(world + 380) > 0.0f) || LW(world + 396) == 0;
        if (whose[who] != player) whose[who] = player, lost[who] = 0;
        lost[who] = no_end ? lost[who] + 1 : 0;
        if (lost[who] >= 30) {
            lost[who] = 0;
            fprintf(stderr, "input: port %u was left in zone camera %u with no end coming (the world's pause %g s, its call %08x): the zone is ended\n", who, zone,
                    (double)fl(world + 380), LW(world + 396));
            pc_call(0x177c80, player);          // PlayerClearZone(player)
        }
    }
    orig(c);
}

// ---- a help line too long for its place runs --------------------------------------------------------------------------
// The menus' help line is a tAnimatedBlurb: +0 its screen, +4 its text object (SCROLLING_TEXT: 100 characters, left-aligned at
// x 127, half the font's size), +148 the line (256 bytes), +412 its state (1 the old line fading, 2 the new one sliding in, 3
// at rest). Animate (0x249348), once a frame, prints the line into the object ("%s", FlailerPrintf) and slides it in. A line
// longer than the place simply went on past the screen's right edge and was cut there (the user's picture, 2026-11-24: "The
// slow motion key works. On a gamepad: both stick", and: "can these lines be a running line?").
// Here a line at rest that is wider than its place runs: it stands for three seconds, then moves left at an even pace until
// its end is in view, stands there for two seconds and starts again. The move is by the pixel: the object itself is moved
// left by what has passed of its first letter, and when a letter has passed whole it is dropped from what is printed and the
// object steps back by its width; at the other end a letter is added as soon as there is room for it. So nothing is printed
// past the place's right end, and at the left a letter goes at most its own width into the bar's margin before it is gone
// (the menus have nothing to cut a text by, so a letter comes and goes whole). A line that fits is not touched.
// The frontend's 2D is laid out 640 wide (a full-screen picture has the scale 1.2 of its 512), so the place is from the
// object's x to the screen's right edge at 640, less a margin. (The first try took 512 for that edge and cut lines that had
// always fitted: the user's second picture.) The widths are the font's own (FindFont, 0x13fe18; a glyph's advance at +20, as
// GetStringWidth, 0x144e28, adds them).
struct BlurbRun {
    std::string line;
    double since = 0;                       // when the line came to rest, or began to run, or reached its end
    int phase = 0;                          // 0 standing at its start, 1 running, 2 standing at its end
    int from = -1, to = -1;                 // what is printed now
    float home = 0;                         // the object's own x
    u32 obj = 0;
};
static std::map<u32, BlurbRun> g_blurb_runs;
// The object's own x, as it was when first met. The object is left moved by up to a letter when its screen closes in the middle
// of a run, and the next blurb made for it takes the x it finds for its home: without this the line would creep left, a little
// with every visit. By the object's address and what it is (a screen's objects stay where they are while its file is loaded;
// after another load the same object at the same address has the same x, and another object is another entry).
static std::map<u64, float> g_blurb_home;

void HOOK_BlurbAnimate(Ctx &c, void (*orig)(Ctx &))
{
    const u32 blurb = c.r[4].w[0];
    orig(c);
    if (!blurb || LW(blurb + 412) != 3) {
        if (blurb) {                        // the line is changing: the object back at its own x, the run over
            auto it = g_blurb_runs.find(blurb);
            if (it != g_blurb_runs.end()) {
                const u32 o = pc_call(0x143d50, LW(blurb), LW(blurb + 4), 1);
                if (o && o == it->second.obj && LW(blurb + 412) == 1 && fl(o + 48) != it->second.home) set_fl(o + 48, it->second.home), SB(o + 43, 4);
                g_blurb_runs.erase(it);
            }
        }
        return;
    }
    g_pc_ctx = &c;
    const u32 obj = pc_call(0x143d50, LW(blurb), LW(blurb + 4), 1);     // FindFlailerObject(screen, object, a text)
    const u32 font = obj ? pc_call(0x13fe18, LW(obj + 80)) : 0;         // FindFont(the object's font)
    if (!obj || !font) return;
    const char *now = (const char *)host(blurb + 148);
    const size_t len = strnlen(now, 255);
    BlurbRun &r = g_blurb_runs[blurb];
    if (r.line.size() != len || memcmp(r.line.data(), now, len)) {
        r = BlurbRun();
        r.line.assign(now, len);
        r.since = menu_seconds();
        r.obj = obj;
        const u64 key = (u64)obj << 32 | (u32)(LW(blurb + 4) ^ LW(blurb) * 31u);
        auto home = g_blurb_home.find(key);
        if (home == g_blurb_home.end()) {
            if (g_blurb_home.size() > 256) g_blurb_home.clear();
            home = g_blurb_home.emplace(key, fl(obj + 48)).first;
        }
        r.home = home->second;
        if (fl(obj + 48) != r.home) set_fl(obj + 48, r.home), SB(obj + 43, 4);      // (left moved by a run its screen closed on)
    }
    const float scale = fl(obj + 56), room = 640.0f - 14.0f - r.home;
    if (len < 2 || !(scale > 0) || !(room > 40)) return;
    float wide[256], total = 0;
    for (size_t i = 0; i < len; i++) {
        const u32 glyph = LW(font + 8 + (u8)r.line[i] * 4);
        wide[i] = glyph ? fl(glyph + 20) * scale : 0;
        total += wide[i];
    }
    if (total <= room) return;              // it fits: as the game has it
    const double t = menu_seconds();
    const float kPace = 45.0f, way = total - room;      // pixels a second; how far the line has to go for its end to show
    float gone = 0;
    if (r.phase == 0 && t - r.since > 3.0) r.phase = 1, r.since = t;
    if (r.phase == 1) {
        gone = (float)(t - r.since) * kPace;
        if (gone >= way) gone = way, r.phase = 2, r.since = t;
    } else if (r.phase == 2) {
        gone = way;
        if (t - r.since > 2.0) r.phase = 0, r.since = t, gone = 0;
    }
    // the letters gone whole, what is left of the first one in view, and the letters that fit after it
    int first = 0;
    float part = gone;
    while (first < (int)len - 1 && part >= wide[first]) part -= wide[first++];
    int last = first;
    float w = -part;
    while (last < (int)len && w + wide[last] <= room + 0.01f) w += wide[last++];
    const float x = r.home - part;
    if (fl(obj + 48) != x) {
        set_fl(obj + 48, x);
        SB(obj + 43, 4);                    // as SetTopLeftX (0x140a20) marks it
    }
    if (first == r.from && last == r.to) return;
    r.from = first, r.to = last;
    pc_call(0x143e70, LW(blurb), LW(blurb + 4), 0x331e50, gstr(r.line.substr((size_t)first, (size_t)(last - first))));     // FlailerPrintf "%s"
}

// ---- Zone Slow Motion: a second press ends it --------------------------------------------------------------------------
// The slow motion is the third of the player's zones: Player::RacingJoyHandler (0x178978: player, port, event, value) answers
// JOY_EVENT_ZONE_SLOMO (97, which only the port's keyboard has a key for) with RequestZone(player, 3, 1, 1, 1), which, granted,
// sets the world's time scale for a while (World::SetTimeScale) and Player +88 to 3; when the while is over the world calls
// ClearZone (0x178358: the blur off, World::ResetTimeScale, +88 to 0, the zone's sound stopped). Pressed again meanwhile the game
// asks for the zone anew (and mostly refuses: the zones' own waits).
// The user, 2026-11-24: "can the slow motion be turned off by pressing the key again?" So a press that comes while the player's
// zone is the slow motion ends it the game's own way (PlayerClearZone, 0x177c80) and asks for nothing. It is done here, on the
// event as the handler gets it, and not where the key is read: the event is what a replay keeps, so a replay ends it at the
// same place.
void HOOK_RacingJoyHandler(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 player = c.r[4].w[0], event = c.r[6].w[0], value = c.r[7].w[0] & 0xff;
    if (event == 97 && value && player && LW(player + 88) == 3) {
        static int told;
        if (told < 8) told++, fprintf(stderr, "zone: the slow motion is ended by its key\n");
        pc_call(0x177c80, player);
        return;
    }
    orig(c);
}

// ---- the Debug menu's Environment row: the wipers ---------------------------------------------------------------------
// The row was the storm's kind: PauseDebugMenu::OnMenuItemScroll (0x174364) steps it (SetStormType(GetStormType() - 1), 0 to 2),
// prints "Environment: %s" with GetStormTypeString's word, and gives every car of the world its "wipers always" word, Car +2116,
// from GetStormType() != 0 (CarRenderInfo::Render draws the two wiper models moving while it is set, or between the times at
// +2108 and +2112 that water under a wheel sets: Car::TurnWipersOnForDuration, 0x133528). The storm itself is not in the retail
// game: SetStormType (0x1703c0) is an empty function, GetStormType (0x1703c8) answers 1 always, the word is "<default>". So any
// step of the row turned every car's wipers on for good, with no way to turn them off but another race (the user, 2026-11-24:
// "there is a way to turn the wipers on, but then they do not go off").
// Here the row is the switch it was to be (the user: "yes, do it"): a step turns the wipers of every car on, the next one off;
// the three functions answer by what the cars have, and the row says which.
static bool wipers_always()
{
    const u32 world = g_pc_ctx ? LW(gp() - 22120) : 0;
    const u32 car = world ? LW(world) : 0;
    return car && car != world && LW(car + 2116) != 0;
}

void HOOK_SetStormType(Ctx &c, void (*orig)(Ctx &))
{
    (void)orig;
    g_pc_ctx = &c;
    const u32 on = wipers_always() ? 0 : 1;
    const u32 world = LW(gp() - 22120);
    int n = 0;
    for (u32 car = world ? LW(world) : 0; car && car != world && n < 64; car = LW(car), n++) SW(car + 2116, on);
    fprintf(stderr, "debug menu: the wipers of %d cars are %s\n", n, on ? "on" : "off");
}

void HOOK_GetStormType(Ctx &c, void (*orig)(Ctx &))
{
    (void)orig;
    g_pc_ctx = &c;
    c.r[2].d[0] = wipers_always() ? 1 : 0;
}

void HOOK_GetStormTypeString(Ctx &c, void (*orig)(Ctx &))
{
    (void)orig;
    g_pc_ctx = &c;
    static u32 on, off;
    if (!on) on = gstr_keep("Wipers On"), off = gstr_keep("Wipers Off");
    c.r[2].d[0] = (s64)(s32)(wipers_always() ? on : off);
}

// The developers' Debug menu (Extras' DEBUG.not.pretty) is always left with Esc (the user, 2026-11-18; a tester, 0.14: "ESCAPE
// doesn't back out"). Its own way back is MENU_BACK: PauseDebugMenu::MenuCallback (0x1740c0) answers message 17 with
// ChangeToScreen("Pause Menu", 0). Esc used to press Start with Triangle there, and the menus take one press a frame
// (FlailerScreenList::JoyHandler, 0x16cab0), so Start, a Select of the row, took Back's place; now Esc is Triangle alone on that
// screen (window_pad). And if the screen is still up eight frames after Esc went down (a popup, the help, a filtered port: what
// keeps MENU_BACK from a screen), the same change of screen is made here.
static void debug_menu_esc_frame()
{
    static bool held;
    static int wait = -1;
    const bool esc = window_key_down(0x1B);
    if (!pc_debug_menu()) {
        wait = -1;
        held = esc;
        return;
    }
    if (esc && !held) wait = 8;
    held = esc;
    if (wait > 0 && --wait == 0) {
        wait = -1;
        fprintf(stderr, "menus: Esc did not take the Debug menu back by itself: the pause menu is opened\n");
        pc_call(fn::ChangeToScreen, gstr("Pause Menu"), 0);
    }
}

// the game's OnHighlight and Select know rows 1..3 only
void HOOK_HandleEvent__11NewMainMenu13ScreenMessageP18ScreenCallbackData(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 self = c.r[4].w[0], msg = c.r[5].w[0], data = c.r[6].w[0];
    pc_profile_loaded();
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

bool pc_tap(int button)
{
    if (g_mouse_wait > 0) return false;
    g_mouse_out = 1 << button;
    g_mouse_wait = 4;
    return true;
}

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

    // The title screen has no rows, only its line "PRESS ENTER": a click anywhere in the window is that press, Cross, as Enter is
    // (the user, 2026-11-25: "make it so that the mouse can press Press Enter on the title screen"). Not while it plays a film
    // (TitleScreen::pInstance gp-21156, State +12 == 2): no pointer shows then.
    if (pc_on_title()) {
        pending = -1, click = false;
        const u32 title = LW(gp() - 21156);
        if (pressed && inside && g_mouse_wait <= 0 && !(title && LW(title + 12) == 2)) {
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
    // The pointer in the menus, directly (the user, 2026-11-23: "hit the items directly"). The highlight used to walk to the row
    // pointed at, a pad's Up or Down every four frames, and a click waited for it to arrive. Now the menu's own handler of those
    // events is called here, at once and as often as it takes: FlailerMenu::JoyHandler (0x16a458: menu, port, event, value) is
    // what FlailerScreenList::JoyHandler ends in for a pad's event. Event 3 / 4 (up / down) asks the screen (message 8 / 9, which
    // moves a list's mask and may refuse) and steps to the next row that can be chosen, with the sound; 9 (MENU_SELECT) is the
    // row's Select (message 1); 5 / 6 its Left / Right. So everything the game does for a press is done, only not through a pad.
    // Not while the help panel is up, nor on a screen that listens to the other pad alone (FlailerScreen +16, its filter port:
    // the mouse is the first player's, as its presses were). hp2.ini [keyboard] mouse_direct=0: the old way.
    bool direct = false;
    if (g_cfg.mouse_direct && menu && chunk && count) {
        const u32 filter = LW(screen + 16);
        bool help = false;
        if (const u32 panel = LW(list + 0x1c4))
            if (const u32 mover = LW(panel + 0x14)) help = fl(mover + 8) < 640.0f;
        direct = !help && (filter == 0xffffffffu || filter == 0);
    }
    const auto current_now = [&]() {
        int i = 0;
        for (u32 item = LW(menu); item && item != menu; item = LW(item), i++)
            if (item == LW(menu + 8)) return i;
        return -1;
    };
    // the highlight to a row, the shorter way round by the rows that can be chosen; false: the menu would not go there
    const auto go = [&](int target) {
        if (LW(menu + 20) == CONTROLLER_SCREEN) {
            // the Controller screen's rows are not in the order of their numbers (Select Device, above): the row itself is set
            int i = 0;
            for (u32 item = LW(menu); item && item != menu; item = LW(item), i++)
                if (i == target) pc_call(0x16a3a8, menu, LW(item + 12));     // FlailerMenu::SetActiveMenuItem
            return current_now() == target;
        }
        for (int step = 0; step < count + 2; step++) {
            const int at = current_now();
            if (at < 0) return false;
            if (at == target) return true;
            int down = 0, up = 0;
            for (int i = at; i != target;) {
                i = (i + 1) % count;
                if (i >= 64 || (usable >> i & 1)) down++;
            }
            for (int i = at; i != target;) {
                i = (i + count - 1) % count;
                if (i >= 64 || (usable >> i & 1)) up++;
            }
            pc_call(0x16a458, menu, 0, down <= up ? 4 : 3, 1);
            if (current_now() == at) return false;      // refused: an edge that scrolls instead, a screen on its way out
        }
        return current_now() == target;
    };
    if (direct) {
        pending = -1, click = false;
        if (inside && (moved || pressed)) {
            // the screen hears of the activity as it does with every pad event (message 24: the attract demo's idle clock),
            // and so does the pads' own clock (GenerateJoyEvents: 0x2effe0 +12 = RealTimeNow, gp-19608)
            if (const u32 callback = LW(menu + 12)) {
                const u32 data = g_pc_ctx->r[29].w[0] - 0x40;      // room under the stack pointer that pc_call steps over
                SW(data, 0), SW(data + 4, 0), SW(data + 8, 0), SW(data + 12, 0);
                pc_call(callback, 24, data);
            }
            SW(0x2effe0 + 12, LW(gp() - 19608));
        }
        if (moved && hovered >= 0 && hovered != current) go(hovered);
    } else if (moved && hovered >= 0) {
        pending = hovered, tries = count + 2;
    }
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
    // and the port's own hints of the Delete key: the three objects under a layout of the player's own, the word on a page
    // of the profiles (not while a name is typed there: the button is the letters' then)
    bool on_edit = false, on_delete = false;
    if (screen_name().rfind("Joystick Config Port", 0) == 0) {
        static const u32 word_hash = pc_hash("PC_EDIT_WORD");
        const u32 lay = find_menu_screen(CONFIG_SCREEN);
        const u32 word = lay ? find_object(lay, word_hash, 1) : 0;
        if (word && (s8)LB(word + 41) == 1) {       // (the word with its icon to the left of it, as Back and Help are)
            const float ex = fl(word + 48) * 0.8f, ey = fl(word + 52) * (448.0f / 480.0f);
            on_edit = inside && x >= ex - 30 && x < ex + 44 && y >= ey - 8 && y < ey + 20;
            const u32 color = on_edit ? 0x80004080u : 0x80808080u;
            if (LW(word + 92) != color) SW(word + 92, color), SB(word + 43, 7);
        }
    } else {
        g_mouse_edit = false;               // (a click nobody took: the layouts' screen went away under it)
    }
    if (!on_edit && chunk && !pc_name_grid_up() && screen_name().rfind("Joystick Config Port", 0) != 0) {
        static const u32 del_hash = pc_hash("PC_DEL_TEXT");
        const u32 o = find_object(chunk, del_hash, 1);
        if (o && (s8)LB(o + 41) == 1) {
            const float dx = fl(o + 48) * 0.8f, dy = fl(o + 52) * (448.0f / 480.0f);
            on_delete = inside && x >= dx - 30 && x < dx + 60 && y >= dy - 8 && y < dy + 20;
            const u32 color = on_delete ? 0x80004080u : 0x80808080u;
            if (LW(o + 92) != color) SW(o + 92, color), SB(o + 43, 7);
        }
    }
    if (pressed && (on_edit || on_delete)) {
        (on_edit ? g_mouse_edit : g_mouse_delete) = true;
        pending = -1, click = false;
    } else if (pressed && (on_back || on_help)) {
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
        if (arrow && direct) {
            pc_call(0x16a458, menu, 0, arrow == 1 ? 5 : 6, 1);      // the row's Left or Right
            return;
        } else if (!arrow && direct && hovered >= 0) {
            if (go(hovered)) pc_call(0x16a458, menu, 0, 9, 1);      // the row's Select; the screen may be another after it
            return;
        } else if (arrow && g_mouse_wait <= 0) {
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
        pending = -1;
        if (direct) {
            pc_call(0x16a458, menu, 0, wheel > 0 ? 3 : 4, 1);
            return;
        }
        g_mouse_out = 1 << (wheel > 0 ? PCB_UP : PCB_DOWN);
        g_mouse_wait = 4;
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

// ---- the game's Autosave, silent ----------------------------------------------------------------------
// Wherever the game saves by its "Autosave" screen (leaving Audio, Display, the Controller's screens, the names, the
// calibration: PushScreenOntoStack("Autosave"), so going back passes through it; and the screens after a race:
// ChangeToScreen("Autosave") and then SetAutosaveCompletionCallback), the screen is an empty one (0xfa2ac5c6) shown while the
// save is written: a black moment between the screens (the user, 2026-11-28: "a black screen for a split second between the
// screens; the save is a rudiment with our settings; everywhere it is used I want only a silent save"). Its Autosaver
// (0x247b48..0x247d98) writes Fred and, on the first tick after, calls its completion callback if one was set, else
// MoveBackScreen(1). Here the screen is not entered: the port writes the profile's save (pcsettings.cpp, the same image as the
// game's, to the profile's save.dat) and a frame later does what the Autosaver would have done. A completion callback set meanwhile
// (no Autosaver is up for it to go into) is kept for that frame. Without a profile in Fred the game's own way is left.
static bool g_autosave_done;                // the save written in place of the screen: the Autosaver's end, next frame
static u32 g_autosave_callback;

void HOOK_ChangeToScreen__17FlailerScreenListPCci(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    const u32 name = c.r[5].w[0];
    if (name && pc_str(name) == "Autosave" && pc_profile_save_ready()) {
        pc_save_profile_now();
        g_autosave_done = true;
        g_autosave_callback = 0;
        return;
    }
    orig(c);
}

void HOOK_SetAutosaveCompletionCallback__FPFv_v(Ctx &c, void (*orig)(Ctx &))
{
    if (g_autosave_done && !LW(gp() - 15080)) {     // no Autosaver: the screen was not entered
        g_autosave_callback = c.r[4].w[0];
        return;
    }
    orig(c);
}

static void autosave_frame()
{
    if (!g_autosave_done) return;
    g_autosave_done = false;
    const u32 cb = g_autosave_callback;
    g_autosave_callback = 0;
    if (cb) pc_call(cb);
    else pc_call(0x170320, 1);              // MoveBackScreen(1)
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
    debug_menu_esc_frame();
    ask_frame();
    autosave_frame();
    for (auto &f : g_frame_fns) f();
    if (screen_name() != "Joystick PreConfig") g_controller_away++;         // (HOOK_JoystickPreConfigHandleEvent: the screen is new)
    if (g_calibration_watch && --g_calibration_watch == 0)
        fprintf(stderr, "menus: half a second after the wheels' Calibration was chosen the screen is \"%s\"\n", screen_name().c_str());
    if (g_pc_ui_dirty.exchange(false)) pc_refresh_page();
    if (go && ++tick == (strchr(go, ',') ? atoi(strchr(go, ',') + 1) : 900)) {
        std::string name(go, strchr(go, ',') ? (size_t)(strchr(go, ',') - go) : strlen(go));
        pc_call(fn::ChangeToScreen, gstr(name), 1);
    }
}
