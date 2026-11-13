// What the player sees of the PS2, as a PC game has it (recomp/docs/frontend-menus.md):
//  - the strings: FE_ENGLISH's table is patched as it loads, so every reader gets the new text;
//  - the button icons: BUTTONSOUT's sheet is repainted with the keys or gamepad buttons bound,
//    in the game's own FONT_A, whenever the bindings or the device in use change.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pclang.h"
#include "pcmenu.h"
#include "hw.h"
#include "pcconfig.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// ---- strings -----------------------------------------------------------------------------
// The high bytes are the game's help characters: 0x80 START, 0x81 SELECT, 0x82..0x85 the D-pad,
// 0x86 Cross, 0x87 Triangle, 0x88 Square, 0x89 Circle, 0x8a L1, 0x8b R1. They draw as the icons below.
static const struct { int id; const char *text; } kText[] = {
    {10000, "PRESS ENTER"},
    {100004, "HUD"},
    {100021, "Keyboard and gamepad."},
    {100022, "Window, resolution, detail."},
    {100023, "Set what you want to see."},
    {100024, "Choose, add or delete a profile."},
    {100025, "Set your player names."},
    {100026, "See who made this game."},
    {150004, "Saved Game"},
    {151001, "Loading game settings."},
    {151003, "No saved game settings found."},
    {151004, "Saving game settings.@Do not close the game."},
    {151006, "The save folder cannot be opened.@ @Would you like to retry?"},
    {151007, "The save folder is damaged. Would you like to create it again?"},
    {151008, "Creating the save folder..."},
    {151009, "The save folder was created."},
    {151010, "The save folder cannot be created.@ @Do you want to try again?"},
    {151011, "There is not enough disk space to save the game. Do you want to try again?"},
    {151012, "The game could not be saved.@Do you want to try again?"},
    {151013, "Looking for saved games..."},
    {151014, "A new save folder was created."},
    {151015, "The save folder cannot be opened.@ Would you like to retry loading?"},
    {151016, "The save folder cannot be used."},
    {151017, "The save folder cannot be opened."},
    {151018, "The saved game could not be loaded.@Would you like to try again?"},
    {151020, "Overwrite the@saved game?"},
    {151022, "Current settings will be overwritten by the saved game. Continue?"},
    {151023, "Create a saved game?"},
    {151024, "The saved game could not be loaded."},
    {151025, "The save folder cannot be opened."},
    {151027, "The save folder cannot be opened. Do you want to try again?"},
    {151101, "The save folder is damaged."},
    {152001, "Creating the save folder..."},
    {300350, "Press \x81" " to reset your car."},
    {800023, "\x82 \x83 Tilt car"},
    {800024, "\x84 \x85 Rotate car"},
    {800026, "\x8b \x8d Zoom camera"},
    {800070, "\x82 \x83 Select save option"},
    {800071, "\x86 \x80 Use save option"},
    {802004, "\x82 \x83 Tilt car"},
    {802005, "\x84 \x85 Rotate car"},
    {802006, "\x82 \x83 \x84 \x85 Rotate and tilt car"},
    {802007, "\x8b \x8d Zoom camera"},
    {802010, "\x82 \x83 Tilt car"},
    {802011, "\x84 \x85 Rotate car"},
    {802012, "\x82 \x83 \x84 \x85 Rotate and tilt car"},
    {802013, "\x8b \x8d Zoom camera"},
    {900005, "Please connect@a gamepad for@player %d."},
    {900011, "Please connect a gamepad."},
    {940001, "Press \x80" " to enter this event."},
};

static u32 g_press_text;                    // the title's line, "PRESS ENTER" or "PRESS START"

// the LanguageDescription (payload of the chunk): NumHash +76, NumID +80, pHash +88, pID +92,
// pText +96. A table entry is {key, offset into pText}; the English hash table lists the same
// offsets, so a replaced string's entries there follow it.
static void patch_language(u32 desc)
{
    // Cross and START do the same in the menus and Enter is both: "Cross START Enter game mode"
    // says it once. In place, in the pool: the string only gets shorter
    u32 size = LW(desc + 84), text = LW(desc + 96);
    u8 *pool_text = host(text);
    for (u32 i = 0; i + 3 < size; i++)
        if (pool_text[i] == 0x86 && pool_text[i + 1] == ' ' && pool_text[i + 2] == 0x80) {
            u32 end = i + 3;
            while (end < size && pool_text[end]) end++;
            memmove(pool_text + i + 1, pool_text + i + 3, end - (i + 3));
            memset(pool_text + end - 2, 0, 2);
        }
    // The port's texts: in English the code's (kText), then for any language its file's #id lines (pclang.h). The title's line (10000)
    // gets room of its own: the device in use rewrites it (pc_paint_buttons).
    const int slot = (int)LW(desc + 72);
    std::vector<std::pair<int, std::string>> texts;
    if (slot == 0)
        for (auto &t : kText) texts.push_back({t.id, t.text});
    for (auto &o : pc_language_overrides(slot)) {
        bool had = false;
        for (auto &t : texts)
            if (t.first == o.first) t.second = o.second, had = true;
        if (!had) texts.push_back(o);
    }
    bool press = false;
    for (auto &t : texts) press |= t.first == 10000;
    if (!press) texts.push_back({10000, tr("PRESS ENTER")});
    u32 size_all = 0;
    for (auto &t : texts) size_all += t.first == 10000 ? 64 : (u32)t.second.size() + 1;
    u32 p = pc_alloc(size_all);             // a language's own: a change of language makes another (a few kilobytes)
    u32 nhash = LW(desc + 76), nid = LW(desc + 80), phash = LW(desc + 88), pid = LW(desc + 92), ptext = LW(desc + 96);
    for (auto &t : texts) {
        const u32 room = t.first == 10000 ? 64 : (u32)t.second.size() + 1;
        memcpy(host(p), t.second.c_str(), std::min<size_t>(t.second.size() + 1, room - 1));
        if (t.first == 10000) g_press_text = p;
        for (u32 i = 0; i < nid; i++) {
            if ((s32)LW(pid + i * 8) != t.first) continue;
            u32 was = LW(pid + i * 8 + 4), now = p - ptext;
            SW(pid + i * 8 + 4, now);
            for (u32 k = 0; k < nhash; k++)
                if (LW(phash + k * 8 + 4) == was) SW(phash + k * 8 + 4, now);
            break;
        }
        p += room;
    }
}

void HOOK_Loader__15LanguageManagerP6bChunk(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 chunk = c.r[5].w[0];
    orig(c);
    if (c.r[2].w[0] != 1) return;
    u32 ret = c.r[2].w[0];
    patch_language(chunk + 8);              // LanguageDescription +72: the e_Languages it is
    c.r[2].d[0] = ret;
}

// ---- the sheet ----------------------------------------------------------------------------
// The keyboard and the gamepad's layouts. A port's layout is the word at +0xc8 of its joystick state (0x2f9530, 0xcc a port; 0x155020
// reads it): the configuration's id, 0x1e for the DualShock 2's first (A), and the game looks every button up by it each time. The
// layouts are the gamepad's (eight of them with layouts_patch.h, some with the D-pad for other things than steering). The keyboard
// has nothing to do with them: its keys are actions, pressed as the buttons that have those actions in layout A, and while the
// keyboard is what a player drives with, that player's port is given layout A. The layout chosen for the gamepad is kept and put
// back as soon as the gamepad is used, or the race is left: the game's screens and its save see only the chosen one.
static const u32 kKeyboardLayout = 0x1e;
static u32 g_layout_chosen[2];
static bool g_layout_keys[2];

void pc_keyboard_layout(int port, bool keys)
{
    static const bool off = getenv("HP2_NOKEYLAYOUT") != nullptr;      // a test: the keys under the gamepad's layout, as they were
    static const char *force = getenv("HP2_PADLAYOUT");                // a test: the gamepad's layout, as if chosen (hex id)
    port &= 1;
    const u32 at = 0x2f9530 + (u32)port * 0xcc + 0xc8;
    if (force && !g_layout_keys[port]) SW(at, (u32)strtoul(force, nullptr, 16));
    if (off) return;
    if (keys) {
        if (!g_layout_keys[port]) g_layout_chosen[port] = LW(at), g_layout_keys[port] = true;
        if (LW(at) != kKeyboardLayout) SW(at, kKeyboardLayout);
    } else if (g_layout_keys[port]) {
        SW(at, g_layout_chosen[port]);
        g_layout_keys[port] = false;
    }
}

namespace {

const u32 FindFont = 0x13fe18;              // FindFont(hash)
const u32 FontTexture = 0x1400f8;           // Font::GetTextureInfo(font)
const u32 UpdateAllText = 0x141648;         // FlailerUpdateLanguageText()

// A help character's glyph is the byte of this table (0x2f09c0), from 0x80 on
const char kGlyphOf[30 + 1] = "abdefgijhknolm3wpv\x92uqrtscx2yz1";

// 16 colours, RGBA with the GS's alpha (0x80 opaque)
const u8 kPal[16][4] = {
    {0, 0, 0, 0},         {0, 0, 0, 0x30},         {0x28, 0x28, 0x28, 0x80}, {0x50, 0x50, 0x50, 0x80},
    {0x80, 0x80, 0x80, 0x80}, {0xb8, 0xb8, 0xb8, 0x80}, {0xff, 0xff, 0xff, 0x80}, {0x30, 0xa0, 0x30, 0x80},
    {0xd0, 0x30, 0x30, 0x80}, {0x30, 0x68, 0xd8, 0x80}, {0xd8, 0xb0, 0x10, 0x80}, {0x98, 0xd0, 0x98, 0x80},
    {0xe8, 0x98, 0x98, 0x80}, {0x98, 0xb4, 0xec, 0x80}, {0xec, 0xd8, 0x88, 0x80}, {0, 0, 0, 0x80},
};

struct Rgba { float r, g, b, a; };
const Rgba WHITE = {255, 255, 255, 128}, CAP = {0x28, 0x28, 0x28, 128}, EDGE = {0xb8, 0xb8, 0xb8, 128};

// the nibble of (x, y) in a 4-bit texture stored for an upload as PSMCT32 (Is32BitSwizzelled)
u32 swizzled4(int w, int x, int y)
{
    int page_y = y >> 7, py = y & 127, block_y = py >> 4, cy = py & 15, col = cy >> 2;
    int swap = ((cy >> 1) & 1) ^ (col & 1);
    int page_x = x >> 7, px = x & 127, block_x = px >> 5, cx = px & 31;
    return (u32)(page_y * 128 * w + col * 8 * w + (cy & 1) * 4 * w + block_y * 64 + ((cy >> 1) & 1) + page_x * 512 +
                 block_x * 32 * w + ((cx >> 3) & 1) * 2 + ((cx >> 4) & 1) * 4 + (cx & 1) * 8 + ((cx >> 1) & 1) * 16 +
                 (((cx >> 2) & 1) ^ swap) * 32);
}

struct Sheet {
    u32 pixels = 0, palette = 0;
    int w = 0;
    bool load(u32 font)
    {
        u32 ti = font ? pc_call(FontTexture, font) : 0;
        if (!ti || LB(ti + 0x28) != 4 || !LW(ti + 0x90)) return false;
        pixels = LW(ti + 0x90);
        palette = LW(ti + 0x94);
        w = LH(ti + 0x24);
        return palette != 0;
    }
    int get(int x, int y) const
    {
        u32 n = swizzled4(w, x, y);
        u8 b = LB(pixels + n / 2);
        return n & 1 ? b >> 4 : b & 15;
    }
    void set(int x, int y, int v) const
    {
        u32 n = swizzled4(w, x, y), a = pixels + n / 2;
        u8 b = LB(a);
        SB(a, (u8)(n & 1 ? (b & 0x0f) | v << 4 : (b & 0xf0) | v));
    }
};

float glyph_f(u32 g, int off)
{
    u32 t = LW(g + off);
    float f;
    memcpy(&f, &t, 4);
    return f;
}

// a glyph record of a font: x +4, y +8, width +12, height +16, advance +20
u32 font_glyph(u32 font, unsigned char ch) { return LW(font + 8 + ch * 4u); }

// one icon, drawn here and then written with the nearest of the 16 colours
struct Cell {
    int w, h;
    std::vector<Rgba> px;
    Cell(int w_, int h_) : w(w_), h(h_), px((size_t)(w_ * h_), Rgba{0, 0, 0, 0}) {}

    void blend(int x, int y, const Rgba &c, float cov)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        Rgba &d = px[(size_t)(y * w + x)];
        float a = c.a / 128 * cov, da = d.a / 128, out = a + da * (1 - a);
        if (out <= 0) return;
        d.r = (c.r * a + d.r * da * (1 - a)) / out;
        d.g = (c.g * a + d.g * da * (1 - a)) / out;
        d.b = (c.b * a + d.b * da * (1 - a)) / out;
        d.a = out * 128;
    }

    // a shape by a point test, four samples a side
    void fill(const std::function<bool(float, float)> &inside, const Rgba &c)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int n = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        if (inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4)) n++;
                if (n) blend(x, y, c, n / 16.0f);
            }
    }

    void round_rect(float x0, float y0, float x1, float y1, float r, const Rgba &c)
    {
        fill([=](float x, float y) {
            float dx = std::fmax(std::fmax(x0 + r - x, x - (x1 - r)), 0.0f);
            float dy = std::fmax(std::fmax(y0 + r - y, y - (y1 - r)), 0.0f);
            return dx * dx + dy * dy <= r * r;
        }, c);
    }

    void triangle(float ax, float ay, float bx, float by, float cx, float cy, const Rgba &col)
    {
        fill([=](float x, float y) {
            float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
            float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
            float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
            return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
        }, col);
    }

    // text centered at (cx, cy) in the game's FONT_A, as large as fits the width
    void text(const Sheet &sheet, u32 font, const std::string &s, float cx, float cy, float max_w, float max_scale,
              const Rgba &c)
    {
        float total = 0;
        for (unsigned char ch : s)
            if (u32 g = font_glyph(font, ch)) total += glyph_f(g, 20);
        if (total <= 0) return;
        float scale = std::fmin(max_scale, max_w / total), pen = cx - total * scale / 2;
        for (unsigned char ch : s) {
            u32 g = font_glyph(font, ch);
            if (!g) continue;
            float gx = glyph_f(g, 4), gy = glyph_f(g, 8), gw = glyph_f(g, 12), gh = glyph_f(g, 16);
            float top = cy - gh * scale / 2;
            for (int y = (int)std::floor(top); y < std::ceil(top + gh * scale); y++)
                for (int x = (int)std::floor(pen); x < std::ceil(pen + gw * scale); x++) {
                    float sum = 0;
                    for (int sy = 0; sy < 3; sy++)
                        for (int sx = 0; sx < 3; sx++) {
                            int tx = (int)(gx + (x + (sx + 0.5f) / 3 - pen) / scale);
                            int ty = (int)(gy + (y + (sy + 0.5f) / 3 - top) / scale);
                            if (tx < gx || ty < gy || tx >= gx + gw || ty >= gy + gh) continue;
                            // the letter, not its black outline: light times alpha
                            u32 e = sheet.palette + (u32)sheet.get(tx, ty) * 4;
                            sum += LB(e + 3) / 128.0f * std::fmin(LB(e) / 222.0f, 1.0f);
                        }
                    if (sum > 0) blend(x, y, c, std::fmin(sum / 9, 1.0f));
                }
            pen += glyph_f(g, 20) * scale;
        }
    }

    void store(const Sheet &sheet, int x0, int y0, const u8 *pal = nullptr) const
    {
        const u8 (*kPal)[4] = pal ? (const u8 (*)[4])pal : ::kPal;       // another palette of 16: the disc's own
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const Rgba &p = px[(size_t)(y * w + x)];
                int best = 0;
                float best_d = 1e30f;
                for (int i = 0; i < 16; i++) {
                    float da = (p.a - kPal[i][3]) * 2, dr = p.r - kPal[i][0], dg = p.g - kPal[i][1], db = p.b - kPal[i][2];
                    // the colour of a nearly transparent pixel does not show
                    float k = std::fmin(p.a, (float)kPal[i][3]) / 128;
                    float d = da * da + k * (dr * dr + dg * dg + db * db);
                    if (d < best_d) best_d = d, best = i;
                }
                sheet.set(x0 + x, y0 + y, best);
            }
    }
};

std::string key_label(int vk)
{
    static const struct { int vk; const char *name; } kShort[] = {
        {0x08, "Bksp"}, {0x0D, "Ent"}, {0x14, "Caps"}, {0x20, "Spc"}, {0x21, "PgUp"}, {0x22, "PgDn"}, {0x2D, "Ins"},
        {0x2E, "Del"}, {0xA0, "Shft"}, {0xA1, "Shft"}, {0xA2, "Ctrl"}, {0xA3, "Ctrl"}, {0xA4, "Alt"}, {0xA5, "Alt"}};
    for (auto &k : kShort)
        if (k.vk == vk) return k.name;
    if (vk >= 0x60 && vk <= 0x69) return "N" + std::to_string(vk - 0x60);
    return pc_key_name(vk).substr(0, 4);
}

void arrow(Cell &c, int dir, float cx, float cy, float r, const Rgba &col)        // 0 up, 1 down, 2 left, 3 right
{
    if (dir == 0) c.triangle(cx, cy - r, cx - r, cy + r * 0.8f, cx + r, cy + r * 0.8f, col);
    if (dir == 1) c.triangle(cx, cy + r, cx - r, cy - r * 0.8f, cx + r, cy - r * 0.8f, col);
    if (dir == 2) c.triangle(cx - r, cy, cx + r * 0.8f, cy - r, cx + r * 0.8f, cy + r, col);
    if (dir == 3) c.triangle(cx + r, cy, cx - r * 0.8f, cy - r, cx - r * 0.8f, cy + r, col);
}

void key_cap(Cell &c)
{
    c.round_rect(1, 3, 25, 25, 4, EDGE);
    c.round_rect(2, 4, 24, 24, 3, CAP);
}

// a keyboard key: the cap, and an arrow or the key's name
void draw_key(Cell &c, const Sheet &ft, u32 font, int vk)
{
    key_cap(c);
    if (vk >= 0x25 && vk <= 0x28) {
        static const int dir[4] = {2, 0, 3, 1};     // VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN
        arrow(c, dir[vk - 0x25], 13, 14, 6, WHITE);
    } else {
        c.text(ft, font, vk ? key_label(vk) : "-", 13, 14, 20, 0.9f, WHITE);
    }
}

// a gamepad button, by its name in pcconfig.cpp
void draw_pad(Cell &c, const Sheet &ft, u32 font, const std::string &name)
{
    static const struct { const char *n; Rgba col; } kFace[] = {
        {"A", {0x30, 0xa0, 0x30, 128}}, {"B", {0xd0, 0x30, 0x30, 128}}, {"X", {0x30, 0x68, 0xd8, 128}}, {"Y", {0xd8, 0xb0, 0x10, 128}}};
    for (auto &f : kFace)
        if (name == f.n) {
            c.fill([](float x, float y) { return (x - 13) * (x - 13) + (y - 14) * (y - 14) <= 11.5f * 11.5f; }, f.col);
            c.text(ft, font, name, 13, 14, 16, 0.9f, WHITE);
            return;
        }
    if (name.rfind("Pad ", 0) == 0) {           // the D-pad, with the direction lit
        c.round_rect(9, 3, 17, 25, 2, EDGE);
        c.round_rect(2, 10, 24, 18, 2, EDGE);
        c.round_rect(10, 4, 16, 24, 1, CAP);
        c.round_rect(3, 11, 23, 17, 1, CAP);
        int dir = name == "Pad Up" ? 0 : name == "Pad Down" ? 1 : name == "Pad Left" ? 2 : 3;
        static const float at[4][2] = {{13, 7.5f}, {13, 20.5f}, {6.5f, 14}, {19.5f, 14}};
        arrow(c, dir, at[dir][0], at[dir][1], 3, WHITE);
        return;
    }
    std::string label = name == "L Stick" ? "LS" : name == "R Stick" ? "RS" : name == "Start" ? "Strt" : name;
    c.round_rect(1, 6, 25, 22, 7, EDGE);
    c.round_rect(2, 7, 24, 21, 6, CAP);
    c.text(ft, font, label, 13, 14, 19, 0.75f, WHITE);
}

// A PS2 button as the device in use has it. In a race the strings name what a button does while
// driving, but what they ask to press to go on (Cross) and pause (Start) are the fixed ones.
void draw_button(Cell &c, const Sheet &ft, u32 font, int b, bool pad, bool driving)
{
    bool fixed = !driving || b == PCB_CROSS || b == PCB_START;
    if (pad) {
        draw_pad(c, ft, font, pc_pad_name(fixed ? pc_menu_pad(b) : g_cfg.pad[b]));
        return;
    }
    int vk = b == PCB_START && driving ? PC_BACK_KEY : fixed ? pc_menu_key(b) : g_cfg.key[b][0] ? g_cfg.key[b][0] : g_cfg.key[b][1];
    draw_key(c, ft, font, vk);
}

// the disc's icon of a PlayStation button, by its name in pcconfig.cpp: 0 when it has none
unsigned char ps_glyph(const std::string &n)
{
    static const struct { const char *n; char g; } k[] = {
        {"Cross", 'i'}, {"Circle", 'k'}, {"Square", 'h'}, {"Triangle", 'j'}, {"L1", 'n'}, {"R1", 'o'}, {"L2", 'l'}, {"R2", 'm'},
        {"L3", '3'}, {"R3", 'w'}, {"Options", 'a'}, {"Create", 'b'}, {"Pad Up", 'd'}, {"Pad Down", 'e'}, {"Pad Left", 'f'}, {"Pad Right", 'g'}};
    for (auto &e : k)
        if (n == e.n) return (unsigned char)e.g;
    return 0;
}

// 0x90..0x98: the sticks. On the keyboard the left stick is the arrows
void draw_stick(Cell &c, const Sheet &ft, u32 font, bool left, bool pad)
{
    if (pad || !left) {
        draw_pad(c, ft, font, left ? "L Stick" : "R Stick");
        return;
    }
    key_cap(c);
    for (int d = 0; d < 4; d++) {
        static const float at[4][2] = {{13, 8.5f}, {13, 19.5f}, {7.5f, 14}, {18.5f, 14}};
        arrow(c, d, at[d][0], at[d][1], 3, WHITE);
    }
}

}   // namespace

// The cursor of a text being typed: half a second on, half off. The game's fonts are not all whole: the first of these the font has.
std::string pc_text_cursor(u32 font_hash)
{
    if ((GetTickCount64() / 500) & 1) return " ";
    const u32 font = font_hash ? pc_call(FindFont, font_hash) : 0;
    for (unsigned char ch : {'_', '|', '-'})
        if (font && font_glyph(font, ch)) return std::string(1, (char)ch);
    return "-";
}

bool pc_text_cursor_changed()
{
    static uint64_t last;
    const uint64_t now = GetTickCount64() / 500;
    const bool changed = now != last;
    last = now;
    return changed;
}

// ---- the replay's bar --------------------------------------------------------------------
// REPLAYBOTTOM_NEW has the PS2's button under each symbol. Rows 30..60 are a flat band: each button's columns get a copy of the bare column beside
// them and the icon of the same button from BUTTONSOUT, so the bar and the help's text always say the same.
static std::atomic<u32> g_bar_stamp{1};
static struct BarIcon { int w = 0, h = 0; std::vector<u8> rgba; } g_bar_icon[9];
static const struct { int x0, x1, button; } kBarKeys[9] = {
    {16, 39, PCB_L1}, {62, 89, PCB_LEFT}, {107, 131, PCB_SQUARE}, {153, 177, PCB_CROSS}, {196, 221, PCB_TRIANGLE},
    {241, 265, PCB_CIRCLE}, {281, 309, PCB_RIGHT}, {325, 354, PCB_SELECT}, {373, 399, PCB_START}};

u32 pc_replay_bar_stamp() { return g_bar_stamp.load(); }

void pc_replay_bar_retouch(u32 *px, int stride)
{
    for (int i = 0; i < 9; i++) {
        const auto &k = kBarKeys[i];
        const BarIcon &ic = g_bar_icon[i];
        for (int y = 29; y <= 60; y++)
            for (int x = k.x0 - 2; x <= k.x1 + 2; x++) px[(size_t)y * stride + x] = px[(size_t)y * stride + k.x0 - 3];
        if (!ic.w) continue;
        const int ox = (k.x0 + k.x1 + 1 - ic.w) / 2, oy = 45 - ic.h / 2;
        for (int y = 0; y < ic.h; y++)
            for (int x = 0; x < ic.w; x++) {
                const u8 *s = &ic.rgba[(size_t)(y * ic.w + x) * 4];
                u32 &d = px[(size_t)(oy + y) * stride + ox + x];
                const int a = s[3] > 128 ? 128 : s[3];
                u32 out = d & 0xff000000;
                for (int c = 0; c < 3; c++) {
                    const int was = (d >> (c * 8)) & 255;
                    out |= (u32)((s[c] * a + was * (128 - a) + 64) / 128) << (c * 8);
                }
                d = out;
            }
    }
}

// the keys, the device or the context changed: the sheet again
void pc_paint_buttons()
{
    static PcConfig last;
    static int last_state = -1;
    // the game's controller layouts (Joystick Config, b6657464) show what each button does in a race: the race's keys and buttons
    const bool layouts = pc_screen_hash() == 0xb6657464;
    // That screen is the gamepad's: it shows a gamepad's buttons whatever is in use, PlayStation's unless an Xbox pad is there or chosen.
    // Its two hints at the bottom are the device's in use, as on every screen: they have cells of their own there (Select's and L3's,
    // which the layouts do not show), since Circle's and Triangle's are the layout's.
    const bool hand = window_input_kind() == 1;
    bool pad = hand || layouts, driving = (pc_racing() && !pc_race_menu()) || layouts;
    bool ps = layouts ? g_layout_playstation != 0 : pad && g_pad_playstation;
    int state = (pad ? 1 : 0) | (driving ? 2 : 0) | (ps ? 4 : 0) | (layouts ? 8 : 0) | (hand ? 16 : 0);
    if (state == last_state && !memcmp(last.key, g_cfg.key, sizeof last.key) && !memcmp(last.pad, g_cfg.pad, sizeof last.pad))
        return;
    u32 buttons = pc_call(FindFont, pc_hash("BUTTONSOUT")), letters = pc_call(FindFont, pc_hash("FONT_A"));
    Sheet out, ft;
    if (!out.load(buttons) || !ft.load(letters)) return;
    last_state = state;
    memcpy(last.key, g_cfg.key, sizeof last.key);
    memcpy(last.pad, g_cfg.pad, sizeof last.pad);

    // what the disc has, before the first paint: a PlayStation pad gets its own icons from it
    static std::vector<u8> disc_px;
    static u8 disc_pal[64];
    if (disc_px.empty()) {
        const int h = 256;                  // BUTTONSOUT is 128 x 256
        disc_px.resize((size_t)out.w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < out.w; x++) disc_px[(size_t)y * out.w + x] = (u8)out.get(x, y);
        memcpy(disc_pal, host(out.palette), 64);
    }

    static const int kBtn[16] = {PCB_START, PCB_SELECT, PCB_UP, PCB_DOWN, PCB_LEFT, PCB_RIGHT, PCB_CROSS, PCB_TRIANGLE,
                                 PCB_SQUARE, PCB_CIRCLE, PCB_L1, PCB_R1, PCB_L2, PCB_R2, -1, PCB_R3};
    for (int i = 0; i < 30; i++) {
        unsigned char ch = (unsigned char)kGlyphOf[i];
        u32 g = ch == 0x92 ? 0 : font_glyph(buttons, ch);
        if (!g) continue;
        int gx = (int)glyph_f(g, 4), gy = (int)glyph_f(g, 8), gw = (int)glyph_f(g, 12), gh = (int)glyph_f(g, 16);
        if (layouts && (i == 1 || i == 14)) {       // Back and Help at the bottom of the layouts' screen
            const int menu = i == 1 ? PCB_TRIANGLE : PCB_CIRCLE;
            if (hand && ps) {
                u32 src = font_glyph(buttons, ps_glyph(pc_pad_name(pc_menu_pad(menu))));
                int sx = src ? (int)glyph_f(src, 4) : gx, sy = src ? (int)glyph_f(src, 8) : gy;
                for (int y = 0; y < gh; y++)
                    for (int x = 0; x < gw; x++) out.set(gx + x, gy + y, disc_px[(size_t)(sy + y) * out.w + sx + x]);
            } else {
                Cell cell(gw, gh);
                draw_button(cell, ft, letters, menu, hand, false);
                cell.store(out, gx, gy, ps ? disc_pal : nullptr);        // a key's cap among the disc's icons: in their colours
            }
            continue;
        }
        if (ps) {
            // the button bound, in the icon of its help character; the sticks and the rest as they are
            unsigned char from = ch;
            if (i < 16 && kBtn[i] >= 0) {
                int b = kBtn[i];
                bool fixed = !driving || b == PCB_CROSS || b == PCB_START;
                if (unsigned char want = ps_glyph(pc_pad_name(fixed ? pc_menu_pad(b) : g_cfg.pad[b]))) from = want;
            }
            u32 src = font_glyph(buttons, from);
            int sx = src ? (int)glyph_f(src, 4) : gx, sy = src ? (int)glyph_f(src, 8) : gy;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) out.set(gx + x, gy + y, disc_px[(size_t)(sy + y) * out.w + sx + x]);
            continue;
        }
        Cell cell(gw, gh);
        if (i == 14) draw_key(cell, ft, letters, 0x2E);     // the font's L3, which no screen shows: the Delete key, for the profiles' hint
        else if (i < 16 && kBtn[i] >= 0) draw_button(cell, ft, letters, kBtn[i], pad, driving);
        else if (i >= 16 && i < 21) draw_stick(cell, ft, letters, true, pad);
        else if (i >= 21 && i < 25) draw_stick(cell, ft, letters, false, pad);
        cell.store(out, gx, gy);
    }
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) SB(out.palette + (u32)i * 4 + (u32)k, ps ? disc_pal[i * 4 + k] : kPal[i][k]);
    for (int i = 0; i < 9; i++) {
        BarIcon &ic = g_bar_icon[i];
        ic.w = 0;
        for (int n = 0; n < 16; n++)
            if (kBtn[n] == kBarKeys[i].button)
                if (u32 g = font_glyph(buttons, (unsigned char)kGlyphOf[n])) {
                    const int gx = (int)glyph_f(g, 4), gy = (int)glyph_f(g, 8);
                    ic.w = (int)glyph_f(g, 12);
                    ic.h = (int)glyph_f(g, 16);
                    ic.rgba.resize((size_t)ic.w * ic.h * 4);
                    for (int y = 0; y < ic.h; y++)
                        for (int x = 0; x < ic.w; x++)
                            memcpy(&ic.rgba[(size_t)(y * ic.w + x) * 4], ps ? &disc_pal[out.get(gx + x, gy + y) * 4] : kPal[out.get(gx + x, gy + y)], 4);
                }
    }
    g_bar_stamp++;
    // too small as an icon in the title's line: the word, of the same length
    if (g_press_text && !pc_racing()) {     // the title's text: gone once the race has been loaded
        const std::string &s = tr(pad ? "PRESS START" : "PRESS ENTER");
        memcpy(host(g_press_text), s.c_str(), std::min<size_t>(s.size() + 1, 63));
        SB(g_press_text + 63, 0);
        pc_call(UpdateAllText);
    }
}

// ---- TEMP diagnostics (HP2_DIAG=1): the attract demo's idle timer and the speech library's own messages ----------------------------
extern FILE *g_hle_log;
static bool diag_on()
{
    static const bool on = getenv("HP2_DIAG") != nullptr;
    return on;
}

// ConsiderStartDemoMode(Timer &last_active_time): the demo starts when (RealTimer - last) * k > BeginDemoModeTimeout
void HOOK_ConsiderStartDemoMode__FR5Timer(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) {
        const u32 gp = c.r[28].w[0], timer = c.r[4].w[0];
        static u32 calls, last_value;
        const u32 now = LW(gp - 19608), last = LW(timer);
        float k, timeout;
        u32 kw = LW(gp - 30500), tw = LW(gp - 23100);
        memcpy(&k, &kw, 4);
        memcpy(&timeout, &tw, 4);
        if (last != last_value || ++calls % 120 == 0) {
            fprintf(g_hle_log, "demo: timer %08x now %u last %u%s idle %.2f s of %.2f, attract mgr %08x replay state %u %u\n", timer, now, last,
                    last != last_value ? " (reset)" : "", (float)(s32)(now - last) * k, timeout, LW(0x2f0000 - 32), LW(0x2f0000 + 2400 + 4),
                    LW(0x2f0000 + 2400 + 8));
            fflush(g_hle_log);
            last_value = last;
        }
    }
    orig(c);
}

void HOOK_StartDemoMode__Fv(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) fprintf(g_hle_log, "demo: StartDemoMode\n"), fflush(g_hle_log);
    orig(c);
    if (diag_on()) fprintf(g_hle_log, "demo: StartDemoMode returned\n"), fflush(g_hle_log);
}

static void spch_string(const char *what, u32 s)
{
    char buf[300];
    int i = 0;
    for (; i < 299 && s; i++) {
        char ch = (char)LB(s + (u32)i);
        if (!ch) break;
        buf[i] = ch == '\n' ? ' ' : ch;
    }
    buf[i] = 0;
    fprintf(g_hle_log, "spch: %s %s\n", what, buf);
    fflush(g_hle_log);
}

void HOOK_S_AbortMessage__9cSEDWedgePce(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) spch_string("abort", c.r[4].w[0]);
    orig(c);
}

void HOOK_SPCHLOG_SaveMessage(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) spch_string("message", c.r[4].w[0]);
    orig(c);
}

// SPCH_AddEvent(event *): the event's first u16 is its ID
void HOOK_SPCH_AddEvent(Ctx &c, void (*orig)(Ctx &))
{
    const u32 ev = c.r[4].w[0];
    const u32 id = diag_on() ? LH(ev) : 0, gp = c.r[28].w[0];
    orig(c);
    if (diag_on()) {
        float ms;
        u32 w = LW(gp - 16548);
        memcpy(&ms, &w, 4);
        fprintf(g_hle_log, "spch: AddEvent %u (%04x %04x %04x %04x) -> %d, clock %.0f ms, frames %u\n", id, LH(ev + 2), LH(ev + 4), LH(ev + 6), LH(ev + 8),
                (int)c.r[2].w[0], ms, LW(gp - 19600));
        fflush(g_hle_log);
    }
}

void HOOK_PlaySentence__9cSEDWedgeif(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0];
    const u32 parts = LW(self + 108), state = LW(self + 120);
    orig(c);
    if (diag_on()) fprintf(g_hle_log, "spch: PlaySentence parts %u state %u -> %u, played %d\n", parts, state, LW(self + 120), (int)c.r[2].w[0]), fflush(g_hle_log);
}

void HOOK_ChangeGameFlow__F13GameFlowStateT0(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) fprintf(g_hle_log, "flow: ChangeGameFlow %u %u (ra %08x)\n", c.r[4].w[0], c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    // After an attract demo the front end opens by how it ended (0x2fc1d0, RCStateReplayDemoModeCallback and RCStateAIDemoModeCallback
    // write it, LaunchFrontendMenus reads it): 2, a button or the recording left, straight to the main menu; 1, played out, the title
    // screen and its films again, the PS2's loop, which on a PC looks like the game starting over. Here a demo that played out goes back
    // to the main menu it was started from, as one that was stopped does. 7 is the race being left for the front end.
    if (c.r[4].w[0] == 7 && LW(0x2fc1d0) == 1) SW(0x2fc1d0, 2);
    orig(c);
}

void HOOK_SetEndReason__15DemoDiscManager17DemoDiscEndReason(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on()) fprintf(g_hle_log, "demo: SetEndReason %u (ra %08x)\n", c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

void HOOK_RCStateNoneCallback__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on() && c.r[5].w[0] != 3) fprintf(g_hle_log, "rc: None msg %u param %u (ra %08x)\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

void HOOK_RCStateAIDemoModeCallback__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    if (diag_on() && c.r[5].w[0] != 3) fprintf(g_hle_log, "rc: AIDemo msg %u param %u (ra %08x)\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

// HP2_REPLAYKEEPON=1: a replay that has left its recording plays on, as the port had it before its floats were the EE's (for looking at
// where the world goes once it differs). Without it the game does what it does on a PS2: it skips the rest (VerifyDebugChecksums).
static bool replay_keeps_on()
{
    static const bool on = getenv("HP2_REPLAYKEEPON") != nullptr;
    return on;
}

static u32 g_replay_steps, g_replay_diffs;
static bool g_replay_left_told;             // this replay's leaving its recording is in the log

// RC_SKIP_REPLAY from VerifyDebugChecksums (see there). An attract demo that leaves its recording is ended, as on a PS2: the next one
// starts. A replay of the player's own race is another matter: the game closes it on the spot and the player, who asked to see the race,
// is back in it with no word (and what the port knows to have made such replays differ is mended: the racers' turns, the road blocks'
// gaps). So one of the player's that still differs plays on, as the port had it before its floats were the EE's, and the log says so.
void HOOK_PostMessage__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    extern bool g_in_verify;
    const u32 rc = LW(c.r[28].w[0] - 22216);
    const bool skip = c.r[5].w[0] == 11 && g_in_verify, demo = rc && LW(rc) == 9;     // RCSTATE_REPLAY_DEMO_MODE
    if (diag_on() && !skip) fprintf(g_hle_log, "rc: PostMessage %u %u (ra %08x)\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0]), fflush(g_hle_log);
    if (skip && (replay_keeps_on() || !demo)) {
        if (!g_replay_left_told) {
            g_replay_left_told = true;
            fprintf(g_hle_log, "replay: left its recording at step %u (the world's timer %u); it plays on\n", g_replay_steps + 1, LW(c.r[28].w[0] - 0x4c78));
            fflush(g_hle_log);
        }
        return;
    }
    orig(c);
}

ULONG64 g_replay_cycles_at;
void HOOK_ChangeState__15RaceCoordinator7RCState(Ctx &c, void (*orig)(Ctx &))
{
    const u32 state = c.r[5].w[0];
    // TEMP (HP2_DIAG): the game thread's cycles over a replay, the measure of its work that the clock's speed and the waits do not change
    ULONG64 &cycles_at = g_replay_cycles_at;
    static u32 rc_state = 0;
    if (diag_on() && (rc_state == 9 || rc_state == 4 || rc_state == 5) && state != rc_state) {
        ULONG64 now = 0;
        QueryThreadCycleTime(GetCurrentThread(), &now);
        fprintf(g_hle_log, "replay: done, %u steps, %.1f million cycles of the game's thread, %.3f million a step\n", g_replay_steps,
                (now - cycles_at) / 1e6, g_replay_steps ? (now - cycles_at) / 1e6 / g_replay_steps : 0.0);
        // HP2_DEMOQUIT: a checking run ends with its replay, not by the clock (work/nshots/t5/demos5.sh)
        static const bool quit = getenv("HP2_DEMOQUIT") != nullptr;
        if (quit) {
            fprintf(g_hle_log, "replay: %u steps in all, %u differ\n", g_replay_steps, g_replay_diffs);
            fflush(g_hle_log), fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    if (state == 4 || state == 5 || state == 9) {
        g_replay_steps = g_replay_diffs = 0;                // a replay starts
        g_replay_left_told = false;
        // HP2_QUIETCORE=<logical processor, an even one>: for measuring on a machine that does other work. The game's thread stays
        // on that processor above the others' priority, and a thread of ours that only pauses holds the processor's other half
        // (the next one), so nobody else's work shares the core; what a pausing neighbour costs is the same in every run.
        static const char *quiet = getenv("HP2_QUIETCORE");
        static bool quiet_set;
        if (quiet && !quiet_set) {
            quiet_set = true;
            const int cpu = atoi(quiet) & ~1;
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << (int)(intptr_t)p);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                for (;;) YieldProcessor();
            }, (LPVOID)(intptr_t)(cpu + 1), 0, nullptr));
            fprintf(g_hle_log, "replay: the game's thread on processor %d, its other half held\n", cpu);
        }
        QueryThreadCycleTime(GetCurrentThread(), &cycles_at);
    }
    rc_state = state;
    if (diag_on()) fprintf(g_hle_log, "rc: ChangeState %u (ra %08x)\n", c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

// ---- replays: the world's checksum -------------------------------------------------------------------------------------------------
// A replay (the attract demo's REPLAY\ files, the instant and auto replays) stores, each world step, a byte of a checksum of the world:
// WorldTimer and each car's (x + y) * 65536 (World::CalculateDebugChecksum). VerifyDebugChecksums compares it while replaying and on any
// difference posts RC_SKIP_REPLAY: the game does not show a replay that has left its recording, it goes on to the next. The port used to
// drop that message, when its floats were not the EE's to the last bit and every replay differed at its first step. They are now
// (eefloat.h), and a replay that differs is left to the game, as on a PS2: of the five attract recordings one does, T43GreenM5 at its
// step 2745, with every float operation checked against the model (-DEE_CHECK). HP2_REPLAYKEEPON=1 plays on regardless.
// Each replay's steps are counted, and the first that differs goes to the log: how far the PC's world keeps to the PS2's (HP2_DIAG).
bool g_in_verify;
static u32 g_recorded = ~0u;                // TEMP: the recording's byte VerifyDebugChecksums has just read (HP2_CHKLOG)

void HOOK_GetData__13ReplayManagerii(Ctx &c, void (*orig)(Ctx &))
{
    orig(c);
    if (g_in_verify) g_recorded = c.r[2].w[0] & 0xff;
}

// TEMP: HP2_WATCH=<timer>,<hex address>: from the replay step of that timer on, every write to that word of the game's memory and the
// code that made it (a debug register of the game's thread; the place as an offset into speed.exe, for speed.map)
static u32 g_watch_addr, g_watch_timer;
static LONG CALLBACK watch_hit(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    static int n;
    if (n++ < 600)
        fprintf(stderr, "watch: %08x = %08x at +%llx after the step of timer %u\n", g_watch_addr, LW(g_watch_addr),
                (unsigned long long)(e->ContextRecord->Rip - (DWORD64)GetModuleHandleW(nullptr)), g_watch_timer);
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_step(u32 timer)
{
    static const char *const want = getenv("HP2_WATCH");
    static bool armed;
    g_watch_timer = timer;
    if (!want || armed || (u32)strtoul(want, nullptr, 10) != timer) return;
    const char *comma = strchr(want, ',');
    if (!comma) return;
    armed = true;
    g_watch_addr = (u32)strtoul(comma + 1, nullptr, 16);
    AddVectoredExceptionHandler(1, watch_hit);
    HANDLE me = nullptr;                            // a thread's registers are set from another, while it is stopped
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &me, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE helper = CreateThread(nullptr, 0, [](void *h) -> DWORD {
        SuspendThread((HANDLE)h);
        CONTEXT cx = {};
        cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        cx.Dr0 = (DWORD64)host(g_watch_addr);
        cx.Dr7 = 1 | (1 << 16) | (3 << 18);         // the first register, on a write, four bytes
        const BOOL ok = SetThreadContext((HANDLE)h, &cx);
        ResumeThread((HANDLE)h);
        return ok;
    }, me, 0, nullptr);
    WaitForSingleObject(helper, INFINITE);
    CloseHandle(helper), CloseHandle(me);
    fprintf(stderr, "watch: armed on %08x at timer %u\n", g_watch_addr, timer);
}

void HOOK_VerifyDebugChecksums__5World(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rm = LW(gp - 22440);      // pReplayManager: +0 playing, +4 recording
    const u32 flag = rm ? LW(rm + 12) : 0;
    const bool playing = rm && LW(rm) && !LW(rm + 4);
    const u32 world = c.r[4].w[0];
    g_in_verify = true;
    orig(c);
    g_in_verify = false;
    if (!playing) return;
    g_replay_steps++;
    watch_step(LW(gp - 0x4c78));
    // TEMP: HP2_MEMPATCH=<timer>,<file>: the file's pairs of words, an address and what to put there, after the step of that timer
    static const char *const mp = getenv("HP2_MEMPATCH");
    if (mp)
        if ((u32)strtoul(mp, nullptr, 10) == LW(gp - 0x4c78) && strchr(mp, ','))
            if (FILE *f = fopen(strchr(mp, ',') + 1, "rb")) {
                u32 pair[2], n = 0;
                while (fread(pair, 4, 2, f) == 2) SW(pair[0], pair[1]), n++;
                fclose(f);
                fprintf(stderr, "mempatch: %u words\n", n);
            }
    // TEMP: HP2_POKE=<timer>,<hex address>=<hex word>[,<address>=<word>...]: words put into the game's memory after the step of that timer
    static const char *const poke = getenv("HP2_POKE");
    if (poke)
        if ((u32)strtoul(poke, nullptr, 10) == LW(gp - 0x4c78))
            for (const char *p = strchr(poke, ','); p; p = strchr(p + 1, ',')) {
                char *eq = nullptr;
                const u32 a = (u32)strtoul(p + 1, &eq, 16);
                if (eq && *eq == '=') {
                    fprintf(stderr, "poke: %08x was %08x", a, LW(a));
                    SW(a, (u32)strtoul(eq + 1, nullptr, 16));
                    fprintf(stderr, ", now %08x\n", LW(a));
                }
            }
    // TEMP: HP2_MEMDUMP=<prefix> with HP2_MEMDUMPT=<timer>[,<timer>...]: the game's whole memory as the step of that timer leaves it
    static const char *const pre = getenv("HP2_MEMDUMP"), *const list = getenv("HP2_MEMDUMPT");
    if (pre) {
        char want[16], name[400];
        snprintf(want, sizeof want, "%u", LW(gp - 0x4c78));
        if (list && strstr(list, want)) {
            snprintf(name, sizeof name, "%s_%s.bin", pre, want);
            if (FILE *f = fopen(name, "wb")) fwrite(g_ram, 1, 0x02000000, f), fclose(f);
        }
    }
    // TEMP: HP2_CHKLOG=<file>, the parts of the checksum each step of a replay: the timer, each car's (x + y) * 65536 as the sum takes it
    // (World +0x1c cars at +0x2c; a car's place at +0x60), the word the object at World +0x18 adds (+0x380), and whether the step matched
    static FILE *chk = getenv("HP2_CHKLOG") ? fopen(getenv("HP2_CHKLOG"), "w") : nullptr;
    if (chk) {
        const u32 n = LW(world + 0x1c), extra = LW(world + 0x18);
        fprintf(chk, "%u t %u %s rec %02x cars %u extra %08x:%08x seed %08x |", g_replay_steps, LW(gp - 0x4c78), LW(rm + 12) != flag ? "DIFF" : "ok",
                g_recorded, n, extra, extra ? LW(extra + 0x380) : 0, LW(world + 0x178));
        for (u32 i = 0; i < n && i < 40; i++) {
            const u32 car = LW(world + 0x2c + i * 4);
            float x, y, z;
            const u32 wx = LW(car + 0x60), wy = LW(car + 0x64), wz = LW(car + 0x68);
            memcpy(&x, &wx, 4), memcpy(&y, &wy, 4), memcpy(&z, &wz, 4);
            fprintf(chk, " %08x(%.3f %.3f %.3f)[%08x %08x]", car, x, y, z, wx, wy);
        }
        fprintf(chk, "\n");
        if (extra && g_replay_steps >= 2738 && g_replay_steps <= 2752) {      // the object's own words about the one that is summed
            fprintf(chk, "  obj");
            for (u32 a = 0x2c0; a < 0x3c0; a += 4) fprintf(chk, " %03x:%08x", a, LW(extra + a));
            fprintf(chk, "\n");
        }
        if (g_replay_steps % 64 == 0) fflush(chk);
        static const u32 quit_at = getenv("HP2_REPLAYQUIT") ? (u32)strtoul(getenv("HP2_REPLAYQUIT"), nullptr, 10) : 0;   // TEMP: the run ends there
        if (quit_at && LW(gp - 0x4c78) == quit_at) fflush(chk), fflush(stderr), TerminateProcess(GetCurrentProcess(), 0);
    }
    if (g_replay_steps == 1500 && diag_on()) {             // TEMP: the work of a replay's first 1500 steps, in the game thread's cycles
        extern ULONG64 g_replay_cycles_at;
        ULONG64 now = 0;
        QueryThreadCycleTime(GetCurrentThread(), &now);
        fprintf(g_hle_log, "replay: 1500 steps took %.1f million cycles of the game's thread\n", (now - g_replay_cycles_at) / 1e6);
        fflush(g_hle_log);
    }
    static ULONGLONG began;                                // when the replay's first step was
    if (g_replay_steps == 1) began = GetTickCount64();
    // TEMP: HP2_SHOTSTEPS=<step>[,<step>...]: the renderer's picture (HP2_NATIVESHOT, as F11 takes it, two frames on) at those steps of a replay
    static const char *const steps = getenv("HP2_SHOTSTEPS");
    if (steps) {
        extern void native_dump_next();
        char want[16];
        snprintf(want, sizeof want, ",%u,", g_replay_steps);
        static const std::string list = std::string(",") + steps + ",";
        if (list.find(want) != std::string::npos) native_dump_next();
    }
    if (LW(rm + 12) != flag) {                             // a difference: the game set this and posted the skip
        if (replay_keeps_on()) SW(rm + 12, flag);
        if (!g_replay_diffs++ && diag_on())
            fprintf(g_hle_log, "replay: first difference at step %u (world loop %u, timer %u), %.2f s after its first step\n", g_replay_steps,
                    LW(gp - 19552), LW(gp - 19576), (GetTickCount64() - began) / 1000.0),
                fflush(g_hle_log);
    }
    if (g_replay_steps % 300 == 0 && diag_on())
        fprintf(g_hle_log, "replay: %u steps, %u differ%s\n", g_replay_steps, g_replay_diffs, g_replay_diffs ? "" : " (in step with the recording)"),
            fflush(g_hle_log);
}


// TEMP (HP2_DIAG): where a frame's CPU goes in a replay: GetTimestep (which waits for real time while a replay plays) and the world's step
static double g_t_gettimestep, g_t_dotimestep;
static u32 g_t_steps;
static double now_ms()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static bool g_in_gettimestep;
void HOOK_GetTimestep__5World(Ctx &c, void (*orig)(Ctx &))
{
    const double t0 = now_ms();
    g_in_gettimestep = true;
    orig(c);
    g_in_gettimestep = false;
    g_t_gettimestep += now_ms() - t0;
}
// HP2_REPLAYNOWAIT: a replay does not wait for real time (GetTimestep spins on the ticker until its step is due): the demo as fast as it goes,
// its steps the same, for timing and profiles
void HOOK_bGetTickerDifference__FUiUi(Ctx &c, void (*orig)(Ctx &))
{
    static const bool nowait = getenv("HP2_REPLAYNOWAIT") != nullptr;
    orig(c);
    if (nowait && g_in_gettimestep) c.f[0].f = 1e9f;
    // The game spins here, a core kept busy, till the step is due: GetTimestep's loop is while (difference * $f22 < $f20), both registers
    // its own and still set in this call. While more than 2.5 ms are left the thread sleeps a millisecond (the timer's resolution is 1 ms,
    // iop_kernel.cpp), and the loop asks again; the end of the wait is the game's own spin, as exact as before. HP2_SPINWAIT=1: as it was.
    static const bool spin = getenv("HP2_SPINWAIT") != nullptr;
    if (g_in_gettimestep && !spin && !nowait) {
        const float left = c.f[20].f - c.f[0].f * c.f[22].f;
        if (left > 0.0025f && left < 1.0f) Sleep(1);
    }
}
#ifdef EE_COUNT
uint64_t g_ee_count[8];
// where the divisions and multiplications outside the world's step are made: return addresses, the most frequent to the log
static bool g_in_step;
static struct Site { void *at; uint64_t n; } g_sites[2][65536];
__attribute__((noinline)) void ee_count_site(int kind)
{
    if (g_in_step) return;
    void *ra = __builtin_return_address(0);
    Site &e = g_sites[kind][((uintptr_t)ra * 0x9E3779B97F4A7C15ull) >> 48];
    if (e.at != ra) e.at = ra, e.n = 0;
    e.n++;
}
static void ee_sites_log()
{
    for (int kind = 0; kind < 2; kind++)
        for (int k = 0; k < 12; k++) {
            Site *best = nullptr;
            for (Site &e : g_sites[kind])
                if (e.n && (!best || e.n > best->n)) best = &e;
            if (!best) break;
            fprintf(g_hle_log, "site: %s at image + %llx: %llu\n", kind ? "mul" : "div", (unsigned long long)((uintptr_t)best->at - (uintptr_t)GetModuleHandleA(nullptr)), (unsigned long long)best->n);
            best->n = 0;
        }
    memset(g_sites, 0, sizeof g_sites);
}
#endif
void HOOK_DoTimestep__5Worldf(Ctx &c, void (*orig)(Ctx &))
{
    const double t0 = now_ms();
#ifdef EE_COUNT
    uint64_t at_entry[8];
    memcpy(at_entry, g_ee_count, sizeof at_entry);
    g_in_step = true;
#endif
    orig(c);
    g_t_dotimestep += now_ms() - t0;
#ifdef EE_COUNT
    g_in_step = false;
    if ((g_t_steps + 1) % 900 == 0) ee_sites_log();
    {
        static uint64_t in_step[8], before[8];
        for (int i = 0; i < 8; i++) in_step[i] += g_ee_count[i] - at_entry[i];
        if ((g_t_steps + 1) % 300 == 0) {
            fprintf(g_hle_log, "count: a step: add %.0f mul %.0f div %.0f sqrt %.0f add4 %.0f mul4 %.0f recurrences %.0f; outside the step, a step: add %.0f mul %.0f div %.0f sqrt %.0f add4 %.0f mul4 %.0f\n",
                    in_step[0] / 300.0, in_step[1] / 300.0, in_step[2] / 300.0, in_step[3] / 300.0, in_step[4] / 300.0, in_step[5] / 300.0, in_step[6] / 300.0,
                    (g_ee_count[0] - before[0] - in_step[0]) / 300.0, (g_ee_count[1] - before[1] - in_step[1]) / 300.0, (g_ee_count[2] - before[2] - in_step[2]) / 300.0,
                    (g_ee_count[3] - before[3] - in_step[3]) / 300.0, (g_ee_count[4] - before[4] - in_step[4]) / 300.0, (g_ee_count[5] - before[5] - in_step[5]) / 300.0);
            for (int i = 0; i < 8; i++) before[i] = g_ee_count[i], in_step[i] = 0;
        }
    }
#endif
    if (++g_t_steps % 300 == 0 && diag_on()) {
        fprintf(g_hle_log, "timing: %u steps: GetTimestep %.2f ms a step, DoTimestep %.2f ms a step\n", g_t_steps, g_t_gettimestep / 300, g_t_dotimestep / 300);
        fflush(g_hle_log);
        g_t_gettimestep = g_t_dotimestep = 0;
    }
}

// TEMP: HP2_DEMOWHOLE=1, the attract demo does not jump from scene to scene: the scene it starts with has no end (the snapshot's +0x34,
// which ServiceDemoMode waits for), so the recording plays on through what lies between the scenes. For telling a world that has left
// its recording from a scene that does not start as the recording was.
void HOOK_AdvanceSnapshot__14DemoModeReplayb(Ctx &c, void (*orig)(Ctx &))
{
    static const bool whole = getenv("HP2_DEMOWHOLE") != nullptr;
    const u32 self = c.r[4].w[0];
    orig(c);
    if (!whole) return;
    if (const u32 snap = LW(self + 0x20)) {
        fprintf(stderr, "demo: scene %u, snapshot %08x: times %u..%u, end %u, now endless\n", LW(self + 0x1c), snap, LW(snap + 0x28), LW(snap + 0x2c), LW(snap + 0x34));
        SW(snap + 0x34, 0x7fffffff);
    }
}

// What a recording's snapshot leaves out. A scene of an attract demo starts from a snapshot in the recording, and the game's snapshots do
// not hold everything. A road block (tCopManager, gp-15620: +316 of them from +320, 120 bytes each) comes back with its place (+68) but
// without its gap (+92, the point AIActionAvoidRoadBlock, 0x1c98e0, steers its car through): CalculateRoadBlockParameters (0x226478) made
// that with the world's random numbers when the block was called, the snapshot does not carry it and nothing makes it again, so the car
// steers by what fresh memory holds (MemoryPool's 0xaa), leaves the recording, and VerifyDebugChecksums ends the demo. That is the green
// BMW's demo (JUL23\T43GreenM5, the fourth), three steps into its second scene, and by the game's own code the PS2's too. The gap below
// is the recording's own: what that road block holds when the recording is played through without the jump (HP2_DEMOWHOLE), with which
// every step of the scene matches the recorded checksums. HP2_NOREPLAYFIX=1 leaves the recording as it is.
struct ReplayFix {
    u32 demo, timer;            // which recording (DemoModeReplay +36), and the world's timer the snapshot brings
    float cx, cy;               // the road block, by its place
    u32 gap_x, gap_y;           // its gap, as the words of the two floats
};
static const ReplayFix kReplayFixes[] = {{4, 1328074, -211.068f, 2533.61f, 0xc35c400bu, 0x451efe57u}};

// The same for a race played here. The game takes a snapshot by writing it and reading it back at once (TakeSnapshot, 0x13e9f0, calls
// RestoreSnapshot), so that the race goes on from what a replay will start from; and the road blocks come out of that without their gaps
// too, in the race itself. Their gaps are noted before a snapshot is taken and put back after it, and kept by the world's timer: a replay
// that starts from that snapshot gets the same ones. HP2_NOREPLAYFIX=1: none of it.
struct BlockGap {
    u32 cx, cy, gap_x, gap_y;   // the block's place and its gap, as the floats' words
};
struct SnapshotGaps {
    u32 timer;
    std::vector<BlockGap> blocks;
    long long think_tick;       // the racers' turns: the count they go on from (native.cpp, HOOK_Think)
};
static std::vector<SnapshotGaps> g_snapshot_gaps;       // the last 4096 snapshots (two a second in a race)
long long native_think_state();
void native_think_restore(long long tick);
static bool g_taking_snapshot;

static std::vector<BlockGap> road_block_gaps(u32 gp)
{
    std::vector<BlockGap> out;
    const u32 cops = LW(gp - 15620);
    if (!cops) return out;
    const u32 n = LW(cops + 316);
    for (u32 i = 0; i < n && i < 64; i++) {
        const u32 block = cops + 320 + i * 120;
        out.push_back({LW(block + 68), LW(block + 72), LW(block + 92), LW(block + 96)});
    }
    return out;
}

static u32 road_block_gaps_back(u32 gp, const std::vector<BlockGap> &gaps)
{
    const u32 cops = LW(gp - 15620);
    if (!cops) return 0;
    const u32 n = LW(cops + 316);
    u32 put = 0;
    for (u32 i = 0; i < n && i < 64; i++) {
        const u32 block = cops + 320 + i * 120;
        for (const BlockGap &g : gaps)
            if (g.cx == LW(block + 68) && g.cy == LW(block + 72)) {
                if (LW(block + 92) != g.gap_x || LW(block + 96) != g.gap_y) put++;
                SW(block + 92, g.gap_x);
                SW(block + 96, g.gap_y);
                break;
            }
    }
    return put;
}

void HOOK_TakeSnapshot__13ReplayManagerPCc(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NOREPLAYFIX") != nullptr;
    const u32 gp = c.r[28].w[0];
    if (off) {
        orig(c);
        return;
    }
    const std::vector<BlockGap> gaps = road_block_gaps(gp);
    const long long think_tick = native_think_state();
    const bool was = g_taking_snapshot;
    g_taking_snapshot = true;
    orig(c);
    g_taking_snapshot = was;
    const u32 put = road_block_gaps_back(gp, gaps);
    const u32 timer = LW(gp - 0x4c78);
    for (auto it = g_snapshot_gaps.begin(); it != g_snapshot_gaps.end();)
        it = it->timer == timer ? g_snapshot_gaps.erase(it) : it + 1;
    if (g_snapshot_gaps.size() >= 4096) g_snapshot_gaps.erase(g_snapshot_gaps.begin());
    g_snapshot_gaps.push_back({timer, gaps, think_tick});
    if (diag_on()) fprintf(g_hle_log, "replay: snapshot taken at timer %u with %zu road blocks, %u gaps put back\n", timer, gaps.size(), put), fflush(g_hle_log);
}

void HOOK_RestoreSnapshot__13ReplayManagerP14ReplaySnapshot(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0];
    orig(c);
    static const bool off = getenv("HP2_NOREPLAYFIX") != nullptr;
    const u32 demo = LW(0x2f0960 + 36), timer = LW(gp - 0x4c78), cops = LW(gp - 15620);
    if (diag_on() && !g_taking_snapshot) fprintf(g_hle_log, "replay: snapshot restored, demo %u, timer %u\n", demo, timer), fflush(g_hle_log);
    if (off) return;
    if (!g_taking_snapshot)                 // a replay starts from a snapshot of this race: the gaps its road blocks had
        for (const SnapshotGaps &s : g_snapshot_gaps)
            if (s.timer == timer) {
                native_think_restore(s.think_tick);
                const u32 put = road_block_gaps_back(gp, s.blocks);
                if (diag_on()) fprintf(g_hle_log, "replay: %zu road blocks of that snapshot, %u gaps put back\n", s.blocks.size(), put), fflush(g_hle_log);
            }
    if (!cops) return;
    for (const ReplayFix &f : kReplayFixes) {
        if (f.demo != demo || f.timer != timer) continue;
        const u32 n = LW(cops + 316);
        for (u32 i = 0; i < n && i < 64; i++) {
            const u32 block = cops + 320 + i * 120, wx = LW(block + 68), wy = LW(block + 72);
            float x, y;
            memcpy(&x, &wx, 4), memcpy(&y, &wy, 4);
            if (std::fabs(x - f.cx) > 0.01f || std::fabs(y - f.cy) > 0.01f) continue;
            if (diag_on())
                fprintf(g_hle_log, "replay: road block %u gets its gap, %08x %08x for %08x %08x\n", i, f.gap_x, f.gap_y, LW(block + 92), LW(block + 96)),
                    fflush(g_hle_log);
            SW(block + 92, f.gap_x);
            SW(block + 96, f.gap_y);
        }
    }
}

// TEMP: HP2_DEMOREPLAY=<1..5>, the attract demo plays that recording (DemoModeReplay +36, which ConsiderLaunching takes over its random one)
void HOOK_ConsiderLaunching__14DemoModeReplay(Ctx &c, void (*orig)(Ctx &))
{
    static const int which = getenv("HP2_DEMOREPLAY") ? atoi(getenv("HP2_DEMOREPLAY")) : -1;
    if (which >= 1 && which <= 5) SW(c.r[4].w[0] + 36, (u32)which);
    orig(c);
}

// ---- HP2_HEAPLOG=<file>: what the game's heap holds ---------------------------------------------------
// The heap is the PS2's: one MemoryPool from the program's end to the stack, under 29 MB. A block's header (8 bytes before what bMalloc
// returns: flags, padding, the mark 0x2222, the block's bytes) has no name, but nearly every allocation is asked for with one
// (bMalloc(size, name, line, flags), which goes on to bMalloc(size, flags)). With HP2_HEAPLOG the live blocks are kept here by that
// name, or by the caller's return address where there is none, and every 900 frames the file gets the sum by name: bytes, blocks, the
// largest. Without it the hooks only pass on.
#include <algorithm>
#include <map>
#include <vector>
#include <unordered_map>
namespace {
struct HeapBlock {
    std::string name;
    u32 bytes;
};
const char *const g_heap_log = getenv("HP2_HEAPLOG");
std::unordered_map<u32, HeapBlock> g_heap_live;
std::string g_heap_name;
u64 g_heap_allocs, g_heap_frees;
u32 g_heap_peak;
}   // namespace

void HOOK_bMalloc__FiPCcii(Ctx &c, void (*orig)(Ctx &))
{
    if (g_heap_log) g_heap_name = pc_str(c.r[5].w[0]);
    orig(c);
    g_heap_name.clear();
}

void HOOK_bMalloc__Fii(Ctx &c, void (*orig)(Ctx &))
{
    if (!g_heap_log) {
        orig(c);
        return;
    }
    char by[24];
    snprintf(by, sizeof by, "@%08x", c.r[31].w[0]);
    const std::string name = g_heap_name.empty() ? by : g_heap_name;
    orig(c);
    const u32 p = c.r[2].w[0];
    if (!p) return;
    g_heap_live[p] = {name, LW(p - 4) + LB(p - 7)};
    g_heap_allocs++;
}

void HOOK_bFree__FPv(Ctx &c, void (*orig)(Ctx &))
{
    if (g_heap_log && g_heap_live.erase(c.r[4].w[0])) g_heap_frees++;
    orig(c);
}

void HOOK_bResizeMalloc__FPvi(Ctx &c, void (*orig)(Ctx &))
{
    const u32 p = c.r[4].w[0];
    orig(c);
    if (!g_heap_log || !p) return;
    auto it = g_heap_live.find(p);
    if (it != g_heap_live.end()) it->second.bytes = LW(p - 4) + LB(p - 7);
}

// once a frame, from the renderer's end of frame
void pc_heap_report(u32 frame)
{
    if (!g_heap_log) return;
    u32 live = 0;
    for (auto &b : g_heap_live) live += b.second.bytes;
    if (live > g_heap_peak) g_heap_peak = live;
    if (frame % 900) return;
    struct Sum {
        u64 bytes = 0;
        u32 blocks = 0, largest = 0;
    };
    std::map<std::string, Sum> by;
    for (auto &b : g_heap_live) {
        Sum &s = by[b.second.name];
        s.bytes += b.second.bytes, s.blocks++;
        if (b.second.bytes > s.largest) s.largest = b.second.bytes;
    }
    std::vector<std::pair<u64, std::string>> order;
    for (auto &s : by) order.push_back({s.second.bytes, s.first});
    std::sort(order.rbegin(), order.rend());
    static FILE *f = fopen(g_heap_log, "w");
    if (!f) return;
    fprintf(f, "---- frame %u %s: live %u bytes in %zu blocks (peak %u), free %u, largest free %u; %llu allocations, %llu frees so far\n", frame,
            pc_racing() ? "race" : "menus", live, g_heap_live.size(), g_heap_peak, pc_call(0x257988, 0), pc_call(0x2579b0, 0),
            (unsigned long long)g_heap_allocs, (unsigned long long)g_heap_frees);
    for (auto &o : order) {
        const Sum &s = by[o.second];
        fprintf(f, "%10llu %6u %9u  %s\n", (unsigned long long)s.bytes, s.blocks, s.largest, o.second.c_str());
    }
    fflush(f);
}
