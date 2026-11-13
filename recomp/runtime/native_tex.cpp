// The native renderer's textures: a TextureInfo of the game as RGBA8 (native.h). The texels stay the GS's
// bytes (alpha 0x80 is 1.0): the shader does the GS's arithmetic on them.
#include "native.h"

#include "host.h"
#include "ps2.h"
#include "memwatch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "pcmenu.h"
#include "png_read.h"

// A flagged level goes to the GS as a PSMCT32 image, half as wide and half (8 bits) or a quarter (4 bits) as tall, and is read as
// PSMT8 or PSMT4 (texture-pack.md): the same memory, here a small one, whatever the sizes. Where it lies in the GS's does not matter.
static std::vector<u8> g_flagged;

// GS memory's layout: pages of 32 blocks of 4 columns (a byte address, the nibble for 4 bits). Only these three formats are ever packed.
enum { PSMCT32 = 0, PSMT8 = 0x13, PSMT4 = 0x14 };
static const u8 kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                  {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const u8 kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
                                  {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 kColumn32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13},         {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29},   {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45},   {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61},   {50, 51, 54, 55, 58, 59, 62, 63}};
static const u8 kColumn8[16][16] = {
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255}};
static const u16 kColumn4[16][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
    {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
    {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
    {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
    {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
    {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
    {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
    {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
    {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511}};

static u32 gs_addr(u32 base, u32 width, u32 psm, u32 x, u32 y, u32 &nibble)
{
    u32 bp = base >> 8, bw = width >> 6;
    nibble = 0;
    if (psm == PSMT8) {
        u32 blk = bp + ((y >> 6) * (bw >> 1) + (x >> 7)) * 32 + kBlock32[(y >> 4) & 3][(x >> 4) & 7];
        return ((blk << 8) + kColumn8[y & 15][x & 15]) & 0x3FFFFF;
    }
    if (psm == PSMT4) {
        u32 blk = bp + ((y >> 7) * (bw >> 1) + (x >> 7)) * 32 + kBlock16[(y >> 4) & 7][(x >> 5) & 3];
        u32 n = (blk << 9) + kColumn4[y & 15][x & 31];
        nibble = n & 1;
        return (n >> 1) & 0x3FFFFF;
    }
    u32 blk = bp + ((y >> 5) * bw + (x >> 6)) * 32 + kBlock32[(y >> 3) & 3][(x >> 3) & 7];
    return ((blk << 6) + kColumn32[y & 7][x & 7]) * 4 & 0x3FFFFC;
}

static void load_flagged(const u8 *px, int w, int h, int bpp, int tbw)
{
    int hw = w / 2, hh = bpp == 8 ? h / 2 : h / 4;
    u32 dbw = (u32)(tbw / 2) * 64;
    if (g_flagged.size() != 4u << 20) g_flagged.resize(4u << 20);
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
            u32 nib, at = gs_addr(0, dbw, PSMCT32, (u32)x, (u32)y, nib);
            memcpy(&g_flagged[at], px + ((size_t)y * hw + x) * 4, 4);
        }
}

// by words when it is all of it: every texture's palette, every frame
static u32 checksum(const u8 *p, size_t bytes, size_t step)
{
    u64 h = 14695981039346656037ull;
    if (step > 1) {
        for (size_t i = 0; i < bytes; i += step) h = (h ^ p[i]) * 1099511628211ull;
    } else {
        size_t i = 0;
        for (; i + 8 <= bytes; i += 8) {
            u64 w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 1099511628211ull;
            h ^= h >> 29;
        }
        for (; i < bytes; i++) h = (h ^ p[i]) * 1099511628211ull;
    }
    return (u32)(h ^ h >> 32);
}

// one level: an image of w x h, kept in a level of tw x th. Past the image the GS has whatever is in its memory: nothing reads
// it but the filter at the edge.
static void decode(u32 *out, int tw, int th, int w, int h, const u8 *px, const u8 *pal, int bpp, int tbw, bool flagged)
{
    auto pad = [&] {
        for (int y = 0; y < h; y++)
            for (int x = w; x < tw; x++) out[(size_t)y * tw + x] = out[(size_t)y * tw + w - 1];
        for (int y = h; y < th; y++) memcpy(&out[(size_t)y * tw], &out[(size_t)(h - 1) * tw], (size_t)tw * 4);
    };
    if (bpp == 32) {
        for (int y = 0; y < h; y++) memcpy(&out[(size_t)y * tw], px + (size_t)y * w * 4, (size_t)w * 4);
        pad();
        return;
    }
    if (flagged) load_flagged(px, w, h, bpp, tbw);
    u32 clut[256] = {};
    memcpy(clut, pal, (size_t)(bpp == 8 ? 256 : 16) * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 i;
            if (bpp == 8) {
                u32 nib;
                i = flagged ? g_flagged[gs_addr(0, (u32)tbw * 64, PSMT8, (u32)x, (u32)y, nib)] : px[(size_t)y * w + x];
                // the CLUT is stored in the GS's CSM1 order: entries 8..15 and 16..23 of each 32 swap
                if ((i & 0x18) == 0x08 || (i & 0x18) == 0x10) i ^= 0x18;
            } else {
                if (flagged) {
                    u32 nib, at = gs_addr(0, (u32)tbw * 64, PSMT4, (u32)x, (u32)y, nib);
                    i = nib ? g_flagged[at] >> 4 : g_flagged[at] & 15;
                } else {
                    size_t n = (size_t)y * w + x;
                    i = (px[n >> 1] >> ((n & 1) * 4)) & 15;
                }
            }
            out[(size_t)y * tw + x] = clut[i];
        }
    pad();
}

// ---- replacements: data/replace/<name>.png or <name>@<crc>.png takes a texture's place (tools/recomp/export_textures.py) -----------

struct ReplacementImage {
    std::vector<u32> px;    // the GS's bytes: alpha 0x80 is 1.0
    int w = 0, h = 0;
};
static const bool g_tex_check = getenv("HP2_TEXCHECK") != nullptr;
static u64 g_tex_checked, g_tex_differ, g_tex_missing;

// lower-case name of the file without .png -> its path
static const std::unordered_map<std::string, std::string> &replacement_files()
{
    static std::unordered_map<std::string, std::string> files;
    static bool scanned;
    if (scanned) return files;
    scanned = true;
    namespace fs = std::filesystem;
    const char *env = getenv("HP2_DATA");
    const fs::path dir = (env ? fs::path(env) : fs::path(host_exe_dir())) / "data" / "replace";
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".png") continue;
        std::string stem = it->path().stem().string();
        for (char &ch : stem) ch = (char)tolower((unsigned char)ch);
        files[stem] = it->path().string();
    }
    if (!files.empty()) fprintf(stderr, "native: %zu replacement textures\n", files.size());
    return files;
}

static u32 crc32_of(const u8 *p, size_t bytes, u32 crc = 0)
{
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < bytes; i++) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static const ReplacementImage *load_replacement(const std::string &path)
{
    static std::unordered_map<std::string, ReplacementImage> loaded;
    auto found = loaded.find(path);
    if (found != loaded.end()) return found->second.px.empty() ? nullptr : &found->second;
    ReplacementImage &r = loaded[path];
    std::vector<u8> rgba;
    const char *why = "";
    if (!png_read::load(path, rgba, r.w, r.h, why)) {
        fprintf(stderr, "native: %s: %s\n", path.c_str(), why);
        return nullptr;
    }
    r.px.resize((size_t)r.w * r.h);
    memcpy(r.px.data(), rgba.data(), r.px.size() * 4);
    for (u32 &p : r.px) {
        const u32 a = p >> 24;
        p = (p & 0xFFFFFF) | ((a + 1) >> 1) << 24;
    }
    return &r;
}

// after decode: t has the game's own levels
static void replace_texture(NativeTexture &t, const char *debug_name, const u8 *px, size_t bytes, const u8 *pal, size_t pal_bytes, int w, int h)
{
    const auto &files = replacement_files();
    if (files.empty()) return;
    char raw[25] = {};
    memcpy(raw, debug_name, 24);
    std::string name = raw;
    for (char &ch : name) ch = isalnum((unsigned char)ch) || ch == '.' || ch == '_' || ch == '-' ? (char)tolower((unsigned char)ch) : '_';
    if (!name.compare(0, 6, "movie_")) return;     // the frames of a movie
    char exact[16];
    u32 crc = crc32_of(px, bytes);
    if (pal_bytes) crc = crc32_of(pal, pal_bytes, crc);     // as zlib's over the two together
    snprintf(exact, sizeof exact, "@%08x", crc);
    auto file = files.find(name + exact);
    if (file == files.end()) file = files.find(name);
    if (file == files.end()) {
        if (g_tex_check && g_tex_missing++ < 40) fprintf(stderr, "texcheck: %s%s is not among the files\n", name.c_str(), exact);
        return;
    }
    const ReplacementImage *r = load_replacement(file->second);
    if (!r) return;
    const int k = r->w / w;
    if (k < 1 || r->w != k * w || r->h != k * h) {
        static int said;
        if (said++ < 20) fprintf(stderr, "native: %s is %dx%d, it has to be a whole multiple of %dx%d\n", file->second.c_str(), r->w, r->h, w, h);
        return;
    }
    // the very picture of the game, as it comes out of the export: the game's own levels stay
    if (k == 1) {
        bool same = true;
        for (int y = 0; y < h && same; y++) same = !memcmp(&t.px[(size_t)y * t.w], &r->px[(size_t)y * w], (size_t)w * 4);
        if (g_tex_check) {
            g_tex_checked++;
            g_tex_differ += !same;
            if (!same && g_tex_differ <= 20) fprintf(stderr, "texcheck: %s%s differs\n", name.c_str(), exact);
            if (g_tex_checked % 200 == 0)
                fprintf(stderr, "texcheck: %llu textures, %llu differ, %llu not among the files\n", (unsigned long long)g_tex_checked, (unsigned long long)g_tex_differ, (unsigned long long)g_tex_missing);
        }
        if (same) return;
    }
    const int tw = t.w * k, th = t.h * k;
    if (k > 1 && t.levels > 1) {
        t.levels = 1;
        for (int m = tw > th ? tw : th; m > 1; m >>= 1) t.levels++;
    }
    size_t texels = 0;
    for (int l = 0; l < t.levels; l++) {
        const int lw = tw >> l, lh = th >> l;
        texels += (size_t)(lw ? lw : 1) * (lh ? lh : 1);
    }
    t.px.resize(texels);
    u32 *out = t.px.data();
    for (int y = 0; y < r->h; y++) {
        memcpy(&out[(size_t)y * tw], &r->px[(size_t)y * r->w], (size_t)r->w * 4);
        for (int x = r->w; x < tw; x++) out[(size_t)y * tw + x] = out[(size_t)y * tw + r->w - 1];
    }
    for (int y = r->h; y < th; y++) memcpy(&out[(size_t)y * tw], &out[(size_t)(r->h - 1) * tw], (size_t)tw * 4);
    // the levels below: the mean of four
    for (int l = 1; l < t.levels; l++) {
        const int sw = (tw >> (l - 1)) ? tw >> (l - 1) : 1, sh = (th >> (l - 1)) ? th >> (l - 1) : 1;
        const int dw = (tw >> l) ? tw >> l : 1, dh = (th >> l) ? th >> l : 1;
        const u32 *src = out;
        out += (size_t)sw * sh;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                const int x0 = sw > 1 ? x * 2 : x, y0 = sh > 1 ? y * 2 : y, x1 = sw > 1 ? x0 + 1 : x0, y1 = sh > 1 ? y0 + 1 : y0;
                const u32 a = src[(size_t)y0 * sw + x0], b = src[(size_t)y0 * sw + x1], c = src[(size_t)y1 * sw + x0], d = src[(size_t)y1 * sw + x1];
                u32 v = 0;
                for (int s = 0; s < 32; s += 8) v |= ((((a >> s) & 255) + ((b >> s) & 255) + ((c >> s) & 255) + ((d >> s) & 255) + 2) >> 2) << s;
                out[(size_t)y * dw + x] = v;
            }
    }
    t.w = tw;
    t.h = th;
    t.upscaled = k > 1;
}

// TextureInfo: Width +0x24, Height +0x26, BitDepth +0x28, NumPaletteEntries +0x2a, ImageSize +0x38, PaletteSize +0x3c,
// ShiftWidth +0x48, ShiftHeight +0x49, TilableUV +0x4c, ImageInfos +0x52 (4 bytes a level, Is32BitSwizzelled the last),
// NumMipMapLevels +0x6e (after the base), MMag +0x6f, MMin +0x70, ImageData +0x90, PaletteData +0x94.
// The levels follow each other in ImageData, halving (texture-pack.md, "The mip chain").
static double g_decode_ms;
static uint32_t g_decoded;

void native_texture_take_time(double &ms, uint32_t &decoded)
{
    ms = g_decode_ms;
    decoded = g_decoded;
    g_decode_ms = 0;
    g_decoded = 0;
}

static NativeTexture *native_texture_now(uint32_t info, uint32_t frame);
static uint64_t g_tex_vouched, g_tex_summed, g_tex_vouched_bad;     // textures asked for: known unchanged by their pages, summed

void native_texture_watch_stats(uint64_t *vouched, uint64_t *summed, uint64_t *bad) { *vouched = g_tex_vouched, *summed = g_tex_summed, *bad = g_tex_vouched_bad; }

std::mutex g_native_tex_mutex;
static std::vector<NativeTexUpload> g_uploads;

void native_texture_take_uploads(std::vector<NativeTexUpload> &out)
{
    for (NativeTexUpload &u : g_uploads) out.push_back(std::move(u));
    g_uploads.clear();
}

uint16_t native_tex_mode(const NativeTexture &t)
{
    return (uint16_t)((t.repeat_s ? 1 : 0) | (t.repeat_t ? 2 : 0) | (t.mag_linear ? 4 : 0) | (t.min_linear ? 8 : 0) | (t.mip_linear ? 16 : 0) | (t.levels - 1) << 8);
}

// what decodes a texture is timed: a frame that waited on it shows in HP2_NATIVELOG's long frames
NativeTexture *native_texture(uint32_t info, uint32_t frame)
{
    // Asked for again in the same frame (every draw asks): the answer is the same texture while the info's image, palette and depth
    // are what they were, which is all native_texture_now would look at before it gave the same one back. No map, no clock.
    struct Asked {
        uint32_t info, image, palette, frame;
        int bpp;
        NativeTexture *tex;
    };
    static Asked asked[1024];
    Asked &was = asked[info >> 4 & 1023];
    if (info && was.tex && was.info == info && was.frame == frame && was.image == LW(info + 0x90) && was.palette == LW(info + 0x94) &&
        was.bpp == (int)LB(info + 0x28))
        return was.tex;
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t before = g_decoded;
    NativeTexture *t = native_texture_now(info, frame);
    if (g_decoded != before) g_decode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (t && t->frame == frame && t->w) was = {info, LW(info + 0x90), LW(info + 0x94), frame, (int)LB(info + 0x28), t};
    return t;
}

static NativeTexture *native_texture_now(uint32_t info, uint32_t frame)
{
    static std::unordered_map<u64, NativeTexture> cache;
    u32 image = info ? LW(info + 0x90) : 0, palette = info ? LW(info + 0x94) : 0;
    int bpp = info ? LB(info + 0x28) : 0;
    if (!image || (bpp != 4 && bpp != 8 && bpp != 32) || (bpp != 32 && !palette)) return nullptr;
    NativeTexture &t = cache[(u64)image << 32 | palette];
    if (t.frame == frame && t.w) return &t;
    bool on_gpu;
    {   // the stamp and the look in one: the renderer lets a texture go only by a stamp it reads under the same lock
        std::lock_guard<std::mutex> hold(g_native_tex_mutex);
        t.frame = frame;
        on_gpu = t.loaded;
    }
    int w = LH(info + 0x24), h = LH(info + 0x26);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return nullptr;
    size_t bytes = (size_t)w * h * bpp / 8, pal_bytes = bpp == 8 ? 1024 : bpp == 4 ? 64 : 0;
    const u8 *px = host(image), *pal = pal_bytes ? host(palette) : nullptr;
    // A streamed or animated texture changes in place: the palette whole, the pixels by a sample. A movie's frames all of them. The step is
    // odd so that it walks across the rows: one that is a multiple of the width sees a single column, which on a white movie never changes.
    // That sum reads 512 bytes all over the pixels, for every texture of every frame, and nearly all of them are the track's, which
    // nothing writes once loaded. So the pixels' and the palette's pages are watched for writes (memwatch.h): while none has had one
    // since the sum was made under the watch, the bytes are those and so is the sum, and nothing is read. A texture on pages that are
    // written all the time (the watch is refused there) is summed every frame, as before. HP2_TEXWATCHCHECK=1: summed anyway, and compared.
    static const bool watch_check = getenv("HP2_TEXWATCHCHECK") != nullptr;
    const u32 raw_bytes = (u32)bytes;
    const auto raw_stamp = [&] { return memwatch_sum(image, raw_bytes) + (pal ? memwatch_sum(palette, (u32)pal_bytes) * 0x100000001ull : 0); };
    const auto raw_sum = [&] {
        const bool movie = !strncmp((const char *)host(info + 8), "MOVIE_", 6);
        return checksum(px, bytes, bytes > 4096 && !movie ? (bytes / 512) | 1 : 1) ^ (pal ? checksum(pal, pal_bytes, 1) * 31 : 0);
    };
    u32 sum;
    if (t.raw_watched && t.raw_bytes == raw_bytes && t.raw_stamp == raw_stamp()) {
        sum = t.raw_sum;
        g_tex_vouched++;
        if (watch_check && raw_sum() != sum && g_tex_vouched_bad++ < 10)
            fprintf(stderr, "texwatch: %s: its pages were not written, its sum is another\n", (const char *)host(info + 8));
    } else {
        t.raw_watched = image < 0x02000000u && memwatch_watch_range(image, raw_bytes) && (!pal || memwatch_watch_range(palette, (u32)pal_bytes));
        t.raw_stamp = t.raw_watched ? raw_stamp() : 0;
        sum = raw_sum();
        t.raw_sum = sum;
        t.raw_bytes = raw_bytes;
        g_tex_summed++;
    }
    // the two that show the keys: a sample of the pixels can miss one key's name changing
    const bool replay_bar = !strcmp((const char *)host(info + 8), "REPLAYBOTTOM_NEW");
    if (replay_bar || !strcmp((const char *)host(info + 8), "BUTTONSOUT")) sum ^= pc_replay_bar_stamp() * 0x9e3779b1u;
    u8 til = LB(info + 0x4c);
    t.repeat_s = til & 1;
    t.repeat_t = (til & 2) != 0;
    u32 mmin = LB(info + 0x70);
    t.mag_linear = LB(info + 0x6f) != 0;
    t.min_linear = mmin == 1 || mmin == 4 || mmin == 5;
    t.mip_linear = mmin == 3 || mmin == 5;
    if (t.image_w == w && t.image_h == h && t.sum == sum && on_gpu) return &t;
    t.image_w = w;
    t.image_h = h;
    t.w = 1 << LB(info + 0x48);
    t.h = 1 << LB(info + 0x49);
    if (t.w < w || t.h < h) return nullptr;
    t.sum = sum;
    t.levels = 1;
    if (mmin >= 2) {
        int most = 1;
        for (int m = t.w > t.h ? t.w : t.h; m > 1; m >>= 1) most++;
        t.levels = 1 + (s8)LB(info + 0x6e);
        if (t.levels < 1) t.levels = 1;
        if (t.levels > most) t.levels = most;
    }
    size_t texels = 0;
    for (int l = 0; l < t.levels; l++) {
        int lw = t.w >> l, lh = t.h >> l;
        texels += (size_t)(lw ? lw : 1) * (lh ? lh : 1);
    }
    t.px.resize(texels);
    u32 *out = t.px.data();
    const u8 *raw = px;
    for (int l = 0; l < t.levels; l++) {
        int tw = t.w >> l, th = t.h >> l, iw = w >> l, ih = h >> l;
        if (!tw) tw = 1;
        if (!th) th = 1;
        if (!iw) iw = 1;
        if (!ih) ih = 1;
        decode(out, tw, th, iw, ih, raw, pal, bpp, LB(info + 0x54 + l * 4), (s8)LB(info + 0x55 + l * 4) == 1);
        out += (size_t)tw * th;
        raw += (size_t)iw * ih * bpp / 8;
    }
    // TEMP (HP2_TEXSTAT): a car skin's level 0 as decoded: the mean colour, and its alpha (the gloss map)
    if (getenv("HP2_TEXSTAT") && info == LW(0x32f6d8)) {     // the showroom's reflection map: its brightness by an 8x8 grid, and its alpha
        fprintf(stderr, "texstat: env %s frame %u bpp %d %dx%d\n", (const char *)host(info + 8), frame, bpp, w, h);
        if (FILE *f = fopen(getenv("HP2_TEXSTAT"), "wb")) fwrite(t.px.data(), 4, (size_t)t.w * t.h, f), fclose(f);
        for (int gy = 0; gy < 8; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                unsigned long long sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
                for (int y = gy * t.h / 8; y < (gy + 1) * t.h / 8; y++)
                    for (int x = gx * t.w / 8; x < (gx + 1) * t.w / 8; x++) {
                        const u32 v = t.px[(size_t)y * t.w + x];
                        sr += v & 255, sg += v >> 8 & 255, sb += v >> 16 & 255, sa += v >> 24, n++;
                    }
                fprintf(stderr, " %3llu,%3llu,%3llu/%3llu", sr / n, sg / n, sb / n, sa / n);
            }
            fputc(10, stderr);
        }
    }
    if (getenv("HP2_TEXSTAT") && strstr((const char *)host(info + 8), "SKIN")) {
        unsigned long long sr = 0, sg = 0, sb = 0, sa = 0;
        u32 amin = 255, amax = 0, hist[9] = {};
        const size_t n = (size_t)t.w * t.h;
        for (size_t i = 0; i < n; i++) {
            const u32 v = t.px[i], al = v >> 24;
            sr += v & 255, sg += v >> 8 & 255, sb += v >> 16 & 255, sa += al;
            amin = al < amin ? al : amin, amax = al > amax ? al : amax;
            hist[al >> 5 > 8 ? 8 : al >> 5]++;
        }
        fprintf(stderr, "texstat: %s frame %u bpp %d %dx%d mean %llu %llu %llu alpha mean %llu min %u max %u, by 32:", (const char *)host(info + 8), frame, bpp, w, h,
                sr / n, sg / n, sb / n, sa / n, amin, amax);
        for (u32 k = 0; k < 8; k++) fprintf(stderr, " %u", (u32)(hist[k] * 100 / n));
        fputc(10, stderr);
    }
    t.upscaled = false;
    snprintf(t.name, sizeof t.name, "%s", (const char *)host(info + 8));
    if (replay_bar && w == 640 && h >= 62) pc_replay_bar_retouch(t.px.data(), t.w);
    replace_texture(t, (const char *)host(info + 8), px, bytes, pal, pal_bytes, w, h);
    if (!strncmp((const char *)host(info + 8), "FONT_", 5)) {
        t.font_alpha.resize((size_t)t.w * t.h);
        for (size_t i = 0; i < t.font_alpha.size(); i++) t.font_alpha[i] = (u8)(t.px[i] >> 24);
    }
    {   // the pixels go with the frame; the next decoding of this texture starts from an empty px
        NativeTexUpload up;
        up.tex = &t;
        up.px.swap(t.px);
        up.w = t.w, up.h = t.h, up.levels = t.levels;
        up.repeat_s = t.repeat_s, up.repeat_t = t.repeat_t, up.mag_linear = t.mag_linear, up.min_linear = t.min_linear, up.mip_linear = t.mip_linear;
        g_uploads.push_back(std::move(up));
        std::lock_guard<std::mutex> hold(g_native_tex_mutex);
        t.loaded = true;
    }
    g_decoded++;
    return &t;
}
