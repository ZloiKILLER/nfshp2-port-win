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

    void store(const Sheet &sheet, int x0, int y0) const
    {
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
    bool pad = window_input_kind() == 1, driving = (pc_racing() && !pc_race_menu()) || layouts, ps = pad && g_pad_playstation;
    int state = (pad ? 1 : 0) | (driving ? 2 : 0) | (ps ? 4 : 0);
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
        if (i < 16 && kBtn[i] >= 0) draw_button(cell, ft, letters, kBtn[i], pad, driving);
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

void HOOK_PostMessage__15RaceCoordinator9RCMessagei(Ctx &c, void (*orig)(Ctx &))
{
    extern bool g_in_verify;
    if (diag_on() && !(c.r[5].w[0] == 11 && g_in_verify)) fprintf(g_hle_log, "rc: PostMessage %u %u (ra %08x)%s\n", c.r[5].w[0], c.r[6].w[0], c.r[31].w[0], g_in_verify ? " dropped" : ""), fflush(g_hle_log);
    if (c.r[5].w[0] == 11 && g_in_verify) return;           // RC_SKIP_REPLAY from VerifyDebugChecksums: see there
    orig(c);
}

static u32 g_replay_steps, g_replay_diffs;
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
    }
    if (state == 4 || state == 5 || state == 9) {
        g_replay_steps = g_replay_diffs = 0;                // a replay starts
        QueryThreadCycleTime(GetCurrentThread(), &cycles_at);
    }
    rc_state = state;
    if (diag_on()) fprintf(g_hle_log, "rc: ChangeState %u (ra %08x)\n", c.r[5].w[0], c.r[31].w[0]), fflush(g_hle_log);
    orig(c);
}

// ---- replays: the world's checksum -------------------------------------------------------------------------------------------------
// A replay (the attract demo's REPLAY\ files, the instant and auto replays) stores, each world step, a byte of a checksum of the world:
// WorldTimer and each car's (x + y) * 65536 (World::CalculateDebugChecksum). VerifyDebugChecksums compares it while replaying and on any
// difference posts RC_SKIP_REPLAY. The PC's floats are not the EE's to the last bit, so the attract demo, recorded on a PS2, differed
// at its first step and was skipped at once: the menu came back every 30 seconds and the music started over. The check still reads its
// byte (the replay's data stays in step); a difference only counts here.
// Each replay's steps are counted, and the first that differs goes to the log: how far the PC's world keeps to the PS2's (HP2_DIAG).
bool g_in_verify;

void HOOK_VerifyDebugChecksums__5World(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rm = LW(gp - 22440);      // pReplayManager: +0 playing, +4 recording
    const u32 flag = rm ? LW(rm + 12) : 0;
    const bool playing = rm && LW(rm) && !LW(rm + 4);
    g_in_verify = true;
    orig(c);
    g_in_verify = false;
    if (!playing) return;
    g_replay_steps++;
    if (g_replay_steps == 1500 && diag_on()) {             // TEMP: the work of a replay's first 1500 steps, in the game thread's cycles
        extern ULONG64 g_replay_cycles_at;
        ULONG64 now = 0;
        QueryThreadCycleTime(GetCurrentThread(), &now);
        fprintf(g_hle_log, "replay: 1500 steps took %.1f million cycles of the game's thread\n", (now - g_replay_cycles_at) / 1e6);
        fflush(g_hle_log);
    }
    if (LW(rm + 12) != flag) {                             // a difference: the game set this and posted the skip, which went nowhere
        SW(rm + 12, flag);
        if (!g_replay_diffs++ && diag_on())
            fprintf(g_hle_log, "replay: first difference at step %u (world loop %u, timer %u)\n", g_replay_steps, LW(gp - 19552), LW(gp - 19576)),
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
}
void HOOK_DoTimestep__5Worldf(Ctx &c, void (*orig)(Ctx &))
{
    const double t0 = now_ms();
    orig(c);
    g_t_dotimestep += now_ms() - t0;
    if (++g_t_steps % 300 == 0 && diag_on()) {
        fprintf(g_hle_log, "timing: %u steps: GetTimestep %.2f ms a step, DoTimestep %.2f ms a step\n", g_t_steps, g_t_gettimestep / 300, g_t_dotimestep / 300);
        fflush(g_hle_log);
        g_t_gettimestep = g_t_dotimestep = 0;
    }
}

// TEMP: HP2_DEMOREPLAY=<1..5>, the attract demo plays that recording (DemoModeReplay +36, which ConsiderLaunching takes over its random one)
void HOOK_ConsiderLaunching__14DemoModeReplay(Ctx &c, void (*orig)(Ctx &))
{
    static const int which = getenv("HP2_DEMOREPLAY") ? atoi(getenv("HP2_DEMOREPLAY")) : -1;
    if (which >= 1 && which <= 5) SW(c.r[4].w[0] + 36, (u32)which);
    orig(c);
}
