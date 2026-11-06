// The game's folder from the disc image (install.h). The layout and the exports are those of tools/recomp/unpack_data.py, export_textures.py and
// export_text.py, made again here so that the exe and an .iso give the same folder: the archive's files and the texts to the byte, the PNGs to the
// pixel (their deflate is this file's, not zlib's). The textures are exported in a fixed order (the Python tool's workers raced for which file a
// picture that several share is written from).
#include "install.h"

#include "setup_iso.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

namespace {

const uint32_t SECTOR = 2048;

// ---- small helpers ---------------------------------------------------------------------------------
std::wstring latin1_w(const std::string &s)
{
    std::wstring w;
    for (unsigned char c : s) w += (wchar_t)c;
    return w;
}

fs::path under(const fs::path &dir, const std::string &rel)
{
    fs::path p = dir;
    size_t a = 0;
    while (a <= rel.size()) {
        size_t b = rel.find('/', a);
        if (b == std::string::npos) b = rel.size();
        p /= latin1_w(rel.substr(a, b - a));
        a = b + 1;
    }
    return p;
}

bool write_file(const fs::path &path, const void *p, size_t n)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE *f = _wfopen(path.wstring().c_str(), L"wb");
    if (!f) return false;
    bool ok = !n || fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

bool read_all(const fs::path &path, std::vector<uint8_t> &out)
{
    FILE *f = _wfopen(path.wstring().c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t)std::max<int64_t>(n, 0));
    bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

uint32_t rd32(const std::vector<uint8_t> &b, size_t o) { uint32_t v; memcpy(&v, &b[o], 4); return v; }
uint16_t rd16(const std::vector<uint8_t> &b, size_t o) { uint16_t v; memcpy(&v, &b[o], 2); return v; }

bool stopped(InstallSteps &s) { return s.stop && *s.stop; }

// a resource of the exe (speed.rc): the port's own files
bool resource(const wchar_t *name, const uint8_t *&p, size_t &n)
{
    HRSRC r = FindResourceW(nullptr, name, (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    p = g ? (const uint8_t *)LockResource(g) : nullptr;
    n = r ? SizeofResource(nullptr, r) : 0;
    return p != nullptr;
}

// ---- unpack_data.py ----------------------------------------------------------------------------------
uint32_t bstring_hash(const std::string &text)
{
    uint32_t h = 0xFFFFFFFF;
    for (unsigned char c : text) h = h * 33 + c;
    return h;
}

// work/ref/zdir_names_ntsc.txt (the resource ZDIR_NAMES) and the names found since, each checked against its hash
bool load_names(std::unordered_map<uint32_t, std::string> &names, std::string &why)
{
    const uint8_t *p;
    size_t n;
    if (!resource(L"ZDIR_NAMES", p, n)) return why = "The exe has no table of the disc's file names.", false;
    std::string text((const char *)p, n);
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        if (!line.empty() && line[0] == '#') continue;
        // line.split(None, 5): five fields by whitespace, the sixth the rest
        std::vector<std::string> f;
        size_t i = 0;
        while (f.size() < 5) {
            while (i < line.size() && isspace((unsigned char)line[i])) i++;
            if (i >= line.size()) break;
            size_t j = i;
            while (j < line.size() && !isspace((unsigned char)line[j])) j++;
            f.push_back(line.substr(i, j - i));
            i = j;
        }
        while (i < line.size() && isspace((unsigned char)line[i])) i++;
        if (f.size() < 5 || i >= line.size()) continue;
        std::string name = line.substr(i);
        while (!name.empty() && isspace((unsigned char)name.back())) name.pop_back();
        if (name == "?") continue;
        uint32_t h = (uint32_t)strtoul(f[1].c_str(), nullptr, 16);
        if (bstring_hash(name) != h) return why = "The table of names is wrong at " + name + ".", false;
        names[h] = name;
    }
    for (const char *extra : {"SOUND\\JUKEBOX.JBC", "SOUND\\SNDDATA.BIN", "SOUND\\SNDHDR.BIN", "REPLAY\\JUL23\\T22REDCORVETTE.BIN", "NFS.TAG"})
        names[bstring_hash(extra)] = extra;
    return true;
}

std::string upper(std::string s)
{
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// os.path.splitext on a path with /: the dot of the last part only, and not a leading one
void splitext(const std::string &rel, std::string &stem, std::string &ext)
{
    size_t slash = rel.rfind('/');
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = rel.rfind('.');
    if (dot == std::string::npos || dot < start) {
        stem = rel, ext.clear();
        return;
    }
    size_t lead = start;
    while (lead < rel.size() && rel[lead] == '.') lead++;
    if (dot < lead) {
        stem = rel, ext.clear();
        return;
    }
    stem = rel.substr(0, dot), ext = rel.substr(dot);
}

std::string hex8(uint32_t h)
{
    char b[9];
    snprintf(b, sizeof b, "%08x", h);
    return b;
}

std::string unnamed_place(uint32_t h, const std::vector<uint8_t> &head)
{
    static const uint8_t replay_head[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x91, 0x03, 0x00};
    if (head.size() >= 4 && !memcmp(head.data(), "RIFF", 4)) return "MOVIES/unnamed/" + hex8(h) + ".avi";
    if (head.size() >= 12 && !memcmp(head.data(), replay_head, 12)) {
        std::string who;
        for (size_t i = 12; i < std::min<size_t>(head.size(), 64) && who.empty(); i++) {
            size_t j = i;
            while (j < std::min<size_t>(head.size(), 64) && head[j] >= 'A' && head[j] <= 'Z') j++;
            if (j - i >= 3) who.assign((const char *)&head[i], j - i);
        }
        for (char &c : who) c = (char)tolower((unsigned char)c);
        return "REPLAY/DEMO/" + (who.empty() ? std::string("unknown") : who) + "_" + hex8(h) + ".replay";
    }
    if (h == 0x38aae9f9) return "SOUND/unnamed_38aae9f9.MSB";
    if (h == 0x38aae9ff) return "SOUND/unnamed_38aae9f9.MSH";
    if (h == 0xcea9f930) return "SOUND/SPEECH/unnamed_cea9f930.table";
    return "unknown/" + hex8(h) + ".dat";
}

std::string place(uint32_t h, const std::unordered_map<uint32_t, std::string> &names, const std::set<std::string> &all_names,
                  const std::vector<uint8_t> &head)
{
    static const std::regex textures("^(CARS/TEXTURES|GLOBAL/(DYNTEX|HUDTEX|DRIVTEX)|FRONTEND/TSELECT|TRACKS/TEX\\d+(TRACK|LOCATION))\\.BIN$");
    static const std::regex geometry("^(CARS/[^/]+/(GEOMETRY|DASHGEOM)|GLOBAL/CHARACTERSA)\\.BIN$");
    static const std::pair<std::regex, const char *> kinds[] = {
        {textures, ".tex"},
        {geometry, ".geo"},
        {std::regex("^LANGUAGES/.*\\.BIN$"), ".lang"},
        {std::regex("^REPLAY/.*\\.BIN$"), ".replay"},
        {std::regex("^NIS/.*\\.BIN$"), ".nis"},
        {std::regex("^MARKERS/.*\\.BIN$"), ".markers"},
        {std::regex("^SOUND/SND.*\\.BIN$"), ".sndbank"},
    };
    auto it = names.find(h);
    if (it == names.end()) return unnamed_place(h, head);
    std::string rel = it->second;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    std::string stem, ext;
    splitext(rel, stem, ext);
    const std::string ue = upper(ext);
    const bool twin = (ue == ".BIN" || ue == ".BUN") && all_names.count(upper(stem) + ".LZC");
    for (const auto &k : kinds)
        if (std::regex_match(rel, k.first)) {
            rel = stem + k.second;
            break;
        }
    return twin ? "unused/" + rel : rel;
}

// ---- LZ: lzdecompress.py ---------------------------------------------------------------------------------
bool lz_decompress(const std::vector<uint8_t> &buf, std::vector<uint8_t> &out)
{
    if (buf.size() < 16 || rd32(buf, 0) != 0x504D4F43) return false;
    const uint16_t method = rd16(buf, 6);
    const uint32_t out_size = rd32(buf, 8), total = rd32(buf, 12);
    if (method == 1) {
        if (total < 16) return false;
        out.assign(buf.begin() + 16, buf.begin() + std::min<size_t>(total, buf.size()));
        return true;
    }
    size_t end = total, src = 16;
    out.clear();
    out.reserve(out_size);
    uint32_t ctrl = 1;
    auto byte = [&](size_t i, uint8_t &v) {
        if (i >= buf.size()) return false;
        v = buf[i];
        return true;
    };
    while (src != end) {
        if (src > end) return false;
        if (ctrl == 1) {
            uint8_t a, b;
            if (!byte(src, a) || !byte(src + 1, b)) return false;
            ctrl = a | 0x10000 | (uint32_t)b << 8;
            src += 2;
        }
        const int n = src + 0x20 <= end ? 16 : 1;
        for (int k = 0; k < n; k++) {
            if (ctrl & 1) {
                uint8_t b0, b1;
                if (!byte(src, b0) || !byte(src + 1, b1)) return false;
                src += 2;
                const size_t dist = ((size_t)(b0 & 0xF0) << 4) | b1, length = 3 + (b0 & 0x0F);
                if (dist > out.size() || dist == 0) return false;
                const size_t p = out.size() - dist;
                for (size_t q = 0; q < length; q++) out.push_back(out[p + q]);
            } else {
                uint8_t v;
                if (!byte(src, v)) return false;
                out.push_back(v);
                src += 1;
            }
            ctrl >>= 1;
        }
    }
    return out.size() == out_size;
}

// ---- PNG: deflate with the fixed codes and a hash chain ---------------------------------------------------
struct BitOut {
    std::vector<uint8_t> &o;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits)
    {
        acc |= v << n;
        n += bits;
        while (n >= 8) o.push_back((uint8_t)acc), acc >>= 8, n -= 8;
    }
    void huff(uint32_t code, int bits)       // a Huffman code goes in from its top bit
    {
        uint32_t r = 0;
        for (int i = 0; i < bits; i++) r |= ((code >> i) & 1) << (bits - 1 - i);
        put(r, bits);
    }
    void flush()
    {
        if (n) o.push_back((uint8_t)acc);
        acc = 0, n = 0;
    }
};

void lit(BitOut &b, int v)
{
    if (v < 144) b.huff(0x30 + v, 8);
    else if (v < 256) b.huff(0x190 + v - 144, 9);
    else if (v < 280) b.huff(v - 256, 7);
    else b.huff(0xC0 + v - 280, 8);
}

void zlib_compress(const std::vector<uint8_t> &in, std::vector<uint8_t> &out)
{
    static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    out.push_back(0x78);
    out.push_back(0x01);
    BitOut b{out};
    b.put(1, 1);    // the last block
    b.put(1, 2);    // fixed codes
    const size_t n = in.size();
    const int HBITS = 15, WINDOW = 32768, CHAIN = 48;
    std::vector<int32_t> head((size_t)1 << HBITS, -1), prev(n ? n : 1, -1);
    auto hash = [&](size_t i) { return ((uint32_t)in[i] << 10 ^ (uint32_t)in[i + 1] << 5 ^ in[i + 2]) & ((1u << HBITS) - 1); };
    size_t i = 0;
    while (i < n) {
        int best = 0, best_d = 0;
        if (i + 2 < n) {
            uint32_t hv = hash(i);
            int32_t c = head[hv];
            for (int k = 0; k < CHAIN && c >= 0 && (int64_t)i - c <= WINDOW; k++, c = prev[(size_t)c]) {
                size_t m = 0, lim = std::min<size_t>(258, n - i);
                while (m < lim && in[(size_t)c + m] == in[i + m]) m++;
                if ((int)m > best) best = (int)m, best_d = (int)(i - (size_t)c);
                if (m == lim) break;
            }
            prev[i] = head[hv];
            head[hv] = (int32_t)i;
        }
        if (best >= 3) {
            int li = 28;
            while (len_base[li] > best) li--;
            lit(b, 257 + li);
            if (len_extra[li]) b.put((uint32_t)(best - len_base[li]), len_extra[li]);
            int di = 29;
            while (dist_base[di] > best_d) di--;
            b.huff((uint32_t)di, 5);
            if (dist_extra[di]) b.put((uint32_t)(best_d - dist_base[di]), dist_extra[di]);
            for (size_t k = 1; k < (size_t)best && i + k + 2 < n; k++) {      // the bytes inside the match go on the chains too
                uint32_t hv = hash(i + k);
                prev[i + k] = head[hv];
                head[hv] = (int32_t)(i + k);
            }
            i += (size_t)best;
        } else {
            lit(b, in[i]);
            i++;
        }
    }
    lit(b, 256);
    b.flush();
    uint32_t a = 1, c2 = 0;
    for (uint8_t v : in) a = (a + v) % 65521, c2 = (c2 + a) % 65521;
    const uint32_t adler = c2 << 16 | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(adler >> s));
}

void put_be32(std::vector<uint8_t> &o, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s));
}

void png_chunk(std::vector<uint8_t> &o, const char *tag, const std::vector<uint8_t> &payload)
{
    put_be32(o, (uint32_t)payload.size());
    size_t at = o.size();
    o.insert(o.end(), tag, tag + 4);
    o.insert(o.end(), payload.begin(), payload.end());
    put_be32(o, crc32_update(0, &o[at], o.size() - at));
}

bool write_png(const fs::path &path, int w, int h, const std::vector<uint8_t> &rgba)
{
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4);
    }
    std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, hdr, z;
    put_be32(hdr, (uint32_t)w);
    put_be32(hdr, (uint32_t)h);
    hdr.insert(hdr.end(), {8, 6, 0, 0, 0});
    png_chunk(o, "IHDR", hdr);
    zlib_compress(raw, z);
    png_chunk(o, "IDAT", z);
    png_chunk(o, "IEND", {});
    return write_file(path, o.data(), o.size());
}

// ---- texture_convert.py --------------------------------------------------------------------------------
const uint32_t TEXTURE_PACK = 0xB0300001, PACK_HEADER = 0x30300002, PACK_INFO = 0x30300003, PACK_DATA = 0x30300004, RECORD = 164;

void find_packs(const std::vector<uint8_t> &buf, size_t off, size_t end, int depth, std::vector<std::pair<size_t, size_t>> &out)
{
    while (off + 8 <= end) {
        uint32_t type = rd32(buf, off), size = rd32(buf, off + 4);
        size_t cend = off + 8 + size;
        if (cend > end) break;
        if (type == TEXTURE_PACK) out.push_back({off, size});
        else if ((type & 0x80000000u) && depth < 8) find_packs(buf, off + 8, cend, depth + 1, out);
        off = cend;
    }
}

struct Pack {
    std::string name;
    std::vector<uint8_t> info, data;
};

Pack parse_pack(const std::vector<uint8_t> &buf, size_t off, size_t size)
{
    Pack p;
    size_t q = off + 8, end = off + 8 + size;
    while (q + 8 <= end) {
        uint32_t type = rd32(buf, q), csize = rd32(buf, q + 4);
        size_t a = std::min(q + 8, buf.size()), b = std::min(q + 8 + (size_t)csize, buf.size());
        if (type == PACK_HEADER) p.name.assign((const char *)&buf[a], strnlen((const char *)&buf[a], b - a));
        else if (type == PACK_INFO) p.info.assign(buf.begin() + a, buf.begin() + b);
        else if (type == PACK_DATA) {
            size_t al = std::min((q + 8 + 127) & ~(size_t)127, b);
            p.data.assign(buf.begin() + al, buf.begin() + b);
        }
        q += 8 + csize;
    }
    return p;
}

struct Tex {
    std::string name;
    uint32_t hash, data_offset, palette_offset, palette_size;
    int width, height, bpp, levels;
    int8_t swizzled0;               // image_infos[0]'s Is32BitSwizzelled
};

Tex texture_at(const std::vector<uint8_t> &info, size_t r)
{
    Tex t;
    t.name.assign((const char *)&info[r + 8], strnlen((const char *)&info[r + 8], 0x18));
    t.hash = rd32(info, r + 0x20);
    t.width = rd16(info, r + 0x24);
    t.height = rd16(info, r + 0x26);
    t.bpp = info[r + 0x28];
    t.data_offset = rd32(info, r + 0x30);
    t.palette_offset = rd32(info, r + 0x34);
    t.palette_size = rd32(info, r + 0x3C);
    t.levels = (int8_t)info[r + 0x6E] + 1;
    t.swizzled0 = (int8_t)info[r + 0x52 + 3];
    return t;
}

// data[a:b] as Python slices it
std::vector<uint8_t> slice(const std::vector<uint8_t> &d, size_t a, size_t n)
{
    if (a >= d.size()) return {};
    return std::vector<uint8_t>(d.begin() + a, d.begin() + std::min(d.size(), a + n));
}

uint32_t texture_crc(const Tex &t, const std::vector<uint8_t> &data)
{
    const size_t n = (size_t)t.width * t.height * t.bpp / 8, pal = t.bpp == 8 ? 1024 : t.bpp == 4 ? 64 : 0;
    std::vector<uint8_t> a = slice(data, t.data_offset, n), b = slice(data, t.palette_offset, pal);
    a.insert(a.end(), b.begin(), b.end());
    return crc32_update(0, a.data(), a.size());
}

// str.isalnum on a Latin-1 character, as Python has it
bool py_alnum(unsigned char c)
{
    if (isalnum(c) && c < 0x80) return true;
    return c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || (c >= 0xBC && c <= 0xBE) || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}

std::string safe_name(const Tex &t)
{
    std::string n = t.name.empty() ? hex8(t.hash) : t.name;
    for (char &c : n)
        if (!py_alnum((unsigned char)c) && c != '.' && c != '_' && c != '-') c = '_';
    return n;
}

void unswizzle8(int w, int h, std::vector<uint32_t> &m)
{
    m.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int page_y = y >> 6, py = y & 63, block_y = py >> 4, cy = py & 15, col = cy >> 2;
        int swap = ((cy >> 1) & 1) ^ (col & 1);
        int base = page_y * 64 * w + block_y * 16 * w + col * 4 * w + (cy & 1) * 2 * w + ((cy >> 1) & 1);
        for (int x = 0; x < w; x++) {
            int page_x = x >> 7, px = x & 127, block_x = px >> 4, cx = px & 15;
            m[(size_t)y * w + x] = (uint32_t)(base + page_x * 256 + block_x * 32 + ((cx >> 3) & 1) * 2 + (cx & 1) * 4 + ((cx >> 1) & 1) * 8 +
                                              ((((cx >> 2) & 1) ^ swap) * 16));
        }
    }
}

void unswizzle4(int w, int h, std::vector<uint32_t> &m)
{
    m.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int page_y = y >> 7, py = y & 127, block_y = py >> 4, cy = py & 15, col = cy >> 2;
        int swap = ((cy >> 1) & 1) ^ (col & 1);
        int base = page_y * 128 * w + col * 8 * w + (cy & 1) * 4 * w + block_y * 64 + ((cy >> 1) & 1);
        for (int x = 0; x < w; x++) {
            int page_x = x >> 7, px = x & 127, block_x = px >> 5, cx = px & 31;
            m[(size_t)y * w + x] = (uint32_t)(base + page_x * 512 + block_x * 32 * w + ((cx >> 3) & 1) * 2 + ((cx >> 4) & 1) * 4 + (cx & 1) * 8 +
                                              ((cx >> 1) & 1) * 16 + ((((cx >> 2) & 1) ^ swap) * 32));
        }
    }
}

// decode(t, data) at level 0: the RGBA with the alpha doubled, or false
bool decode(const Tex &t, const std::vector<uint8_t> &data, std::vector<uint8_t> &out)
{
    const int w = std::max(1, t.width), h = std::max(1, t.height);
    const size_t size = (size_t)w * h * t.bpp / 8;
    if (t.width == 0 && t.height == 0) {}
    if (size == 0) return false;
    std::vector<uint8_t> px = slice(data, t.data_offset, size), pal = slice(data, t.palette_offset, t.palette_size);
    out.assign((size_t)w * h * 4, 0);
    if (t.bpp == 32) {
        if (px.size() < (size_t)w * h * 4) return false;
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4) {
            memcpy(&out[i], &px[i], 3);
            out[i + 3] = (uint8_t)std::min(255, px[i + 3] * 2);
        }
        return true;
    }
    if ((t.bpp != 4 && t.bpp != 8) || pal.empty()) return false;
    const size_t entries = pal.size() / 4;
    std::vector<uint8_t> rgba(entries * 4);
    for (size_t i = 0; i < entries; i++) {
        size_t s = i;
        if (t.bpp == 8 && ((i & 0x18) == 0x08 || (i & 0x18) == 0x10)) s = (i ^ 0x18) < entries ? i ^ 0x18 : i;
        memcpy(&rgba[i * 4], &pal[s * 4], 3);
        rgba[i * 4 + 3] = (uint8_t)std::min(255, pal[s * 4 + 3] * 2);
    }
    const bool closes = w % 128 == 0 && h % (t.bpp == 8 ? 64 : 128) == 0;
    const bool unswizzle = t.swizzled0 == 1 && closes;
    std::vector<uint32_t> map;
    if (t.bpp == 8) {
        if (px.size() < (size_t)w * h) return false;
        if (unswizzle) unswizzle8(w, h, map);
    } else {
        if (px.size() * 2 < (size_t)w * h) return false;
        if (unswizzle) unswizzle4(w, h, map);
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        size_t a = unswizzle ? map[i] : i, v;
        if (t.bpp == 8) v = px[a];
        else v = (px[a >> 1] >> ((a & 1) * 4)) & 0xF;
        if (v >= entries) v = 0;
        memcpy(&out[i * 4], &rgba[v * 4], 4);
    }
    return true;
}

// ---- export_textures.py ---------------------------------------------------------------------------------
struct Row {
    std::string file, pack, name;
    int width, height, bpp, levels;
    std::string crc, png;
    bool operator<(const Row &o) const
    {
        return std::tie(file, pack, name, width, height, bpp, levels, crc, png) < std::tie(o.file, o.pack, o.name, o.width, o.height, o.bpp, o.levels, o.crc, o.png);
    }
};

// std::string compares as signed char on some hosts: Python compares the code points, so by unsigned bytes
struct ByBytes {
    bool operator()(const std::string &a, const std::string &b) const
    {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                            [](char x, char y) { return (unsigned char)x < (unsigned char)y; });
    }
};

bool export_textures(const fs::path &data, const std::vector<std::string> &index_paths, InstallSteps &steps, int from, int to)
{
    std::set<std::string, ByBytes> files;
    for (const std::string &r : index_paths) {
        std::string stem, ext;
        splitext(r, stem, ext);
        std::string e = ext;
        for (char &c : e) c = (char)tolower((unsigned char)c);
        if ((e == ".tex" || e == ".geo" || e == ".nis" || e == ".bun" || e == ".lzc") && r.compare(0, 7, "unused/")) files.insert(r);
    }
    std::error_code ec;
    fs::create_directories(data / L"textures", ec);
    std::unordered_map<std::string, std::string> seen;      // name.lower()@crc -> the PNG, "" when it did not decode
    std::vector<Row> rows;
    size_t done = 0;
    for (const std::string &rel : files) {
        if (stopped(steps)) return false;
        steps.progress(from + (int)((to - from) * done++ / std::max<size_t>(files.size(), 1)), L"Exporting the textures: " + latin1_w(rel));
        std::vector<uint8_t> buf, plain;
        if (!read_all(under(data, rel), buf)) continue;
        if (buf.size() >= 4 && !memcmp(buf.data(), "COMP", 4)) {
            if (!lz_decompress(buf, plain)) continue;         // the Python tool skips it ("skipped")
            buf.swap(plain);
        }
        std::string stem, ext;
        splitext(rel, stem, ext);
        std::string folder = stem;
        std::replace(folder.begin(), folder.end(), '/', '_');
        std::vector<std::pair<size_t, size_t>> packs;
        find_packs(buf, 0, buf.size(), 0, packs);
        for (auto &pk : packs) {
            Pack p = parse_pack(buf, pk.first, pk.second);
            for (size_t r = 0; r + RECORD <= p.info.size(); r += RECORD) {
                Tex t = texture_at(p.info, r);
                const uint32_t crc = texture_crc(t, p.data);
                const std::string name = safe_name(t);
                std::string key = name;
                for (char &c : key) c = (char)tolower((unsigned char)c);
                key += "@" + hex8(crc);
                std::string first;
                auto s = seen.find(key);
                if (s == seen.end()) {
                    first = folder + "/" + name + ".png";
                    std::vector<uint8_t> rgba;
                    if (decode(t, p.data, rgba)) {
                        write_png(under(data / L"textures", folder + "/" + name + ".png"), std::max(1, t.width), std::max(1, t.height), rgba);
                    } else {
                        first.clear();
                    }
                    seen[key] = first;
                } else {
                    first = s->second;
                }
                rows.push_back({rel, p.name, name, t.width, t.height, t.bpp, t.levels, hex8(crc), first});
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        ByBytes lt;
        if (a.file != b.file) return lt(a.file, b.file);
        if (a.pack != b.pack) return lt(a.pack, b.pack);
        if (a.name != b.name) return lt(a.name, b.name);
        if (a.width != b.width) return a.width < b.width;
        if (a.height != b.height) return a.height < b.height;
        if (a.bpp != b.bpp) return a.bpp < b.bpp;
        if (a.levels != b.levels) return a.levels < b.levels;
        if (a.crc != b.crc) return lt(a.crc, b.crc);
        return lt(a.png, b.png);
    });
    std::string csv = "file,pack,name,width,height,bpp,levels,crc,png\n";
    for (const Row &r : rows)
        csv += r.file + "," + r.pack + "," + r.name + "," + std::to_string(r.width) + "," + std::to_string(r.height) + "," + std::to_string(r.bpp) + "," +
               std::to_string(r.levels) + "," + r.crc + "," + r.png + "\n";
    return write_file(data / L"textures" / L"index.csv", csv.data(), csv.size());
}

// ---- export_text.py ---------------------------------------------------------------------------------------
// cp1252 with "replace": the five bytes it leaves out are U+FFFD
std::string cp1252_utf8(const std::string &s)
{
    static const uint16_t high[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                      0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string o;
    for (unsigned char c : s) {
        uint32_t cp = c >= 0x80 && c < 0xA0 ? high[c - 0x80] : c;
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) o += (char)(0xC0 | cp >> 6), o += (char)(0x80 | (cp & 0x3F));
        else o += (char)(0xE0 | cp >> 12), o += (char)(0x80 | ((cp >> 6) & 0x3F)), o += (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

std::string escape_text(const std::string &u)
{
    std::string o;
    for (char c : u) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    return o;
}

bool read_table(const fs::path &path, std::map<int32_t, std::string> &out)
{
    std::vector<uint8_t> b;
    if (!read_all(path, b) || b.size() < 108 || rd32(b, 0) != 0x39000) return false;
    const uint32_t num_hash = rd32(b, 84), num_id = rd32(b, 88), text_size = rd32(b, 92);
    const size_t ids = 108 + 8 * (size_t)num_hash, pool = ids + 8 * (size_t)num_id;
    std::vector<uint8_t> text = slice(b, pool, text_size);
    for (uint32_t i = 0; i < num_id; i++) {
        if (ids + i * 8 + 8 > b.size()) return false;
        int32_t key = (int32_t)rd32(b, ids + i * 8);
        uint32_t off = rd32(b, ids + i * 8 + 4);
        std::string s;
        if (off < text.size()) {
            size_t e = off;
            while (e < text.size() && text[e]) e++;
            s.assign((const char *)&text[off], e - off);
        }
        out[key] = escape_text(cp1252_utf8(s));
    }
    return true;
}

bool export_text(const fs::path &data)
{
    const fs::path out = data / L"text";
    std::error_code ec;
    fs::create_directories(out, ec);
    std::vector<fs::path> paths;
    for (fs::directory_iterator it(data / L"LANGUAGES", ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (n.size() > 8 && !n.compare(0, 3, L"FE_") && !_wcsicmp(n.c_str() + n.size() - 5, L".lang")) paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());
    std::vector<std::pair<std::string, std::map<int32_t, std::string>>> tables;
    for (const fs::path &p : paths) {
        std::string lang = p.stem().string().substr(3);
        for (char &c : lang) c = (char)tolower((unsigned char)c);
        std::map<int32_t, std::string> t;
        if (!read_table(p, t)) return false;
        std::string txt;
        for (auto &kv : t) txt += std::to_string(kv.first) + "\t" + kv.second + "\n";
        if (!write_file(out / (lang + ".txt"), txt.data(), txt.size())) return false;
        tables.push_back({lang, std::move(t)});
    }
    std::set<int32_t> all_ids;
    for (auto &t : tables)
        for (auto &kv : t.second) all_ids.insert(kv.first);
    std::string tsv = "id";
    for (auto &t : tables)
        if (t.first != "id" && t.first != "longest") tsv += "\t" + t.first;
    tsv += "\n";
    for (int32_t key : all_ids) {
        tsv += std::to_string(key);
        for (auto &t : tables) {
            if (t.first == "id" || t.first == "longest") continue;
            auto v = t.second.find(key);
            tsv += "\t" + (v == t.second.end() ? std::string() : v->second);
        }
        tsv += "\n";
    }
    return write_file(out / L"all.tsv", tsv.data(), tsv.size());
}

// ---- the port's own files, from the exe ---------------------------------------------------------------------
bool write_port_files(const fs::path &root)
{
    static const struct {
        const wchar_t *res;
        const wchar_t *path;
    } kFiles[] = {
        {L"LANG_ENGLISH", L"data\\lang\\ENGLISH.txt"}, {L"LANG_FRENCH", L"data\\lang\\FRENCH.txt"},   {L"LANG_GERMAN", L"data\\lang\\GERMAN.txt"},
        {L"LANG_ITALIAN", L"data\\lang\\ITALIAN.txt"}, {L"LANG_SPANISH", L"data\\lang\\SPANISH.txt"}, {L"LANG_SWEDISH", L"data\\lang\\SWEDISH.txt"},
        {L"UI_CURSOR", L"data\\ui\\cursor.cur"},       {L"README", L"README.txt"},
    };
    for (auto &f : kFiles) {
        const uint8_t *p;
        size_t n;
        if (!resource(f.res, p, n) || !write_file(root / f.path, p, n)) return false;
    }
    std::error_code ec;
    fs::create_directories(root / L"data" / L"replace", ec);
    return true;
}

}   // namespace

bool install_exports(const std::wstring &data_dir, InstallSteps &steps)
{
    const fs::path data = data_dir;
    std::vector<uint8_t> idx;
    if (!read_all(data / L"index.txt", idx)) return steps.why = "No data\\index.txt.", false;
    std::vector<std::string> paths;
    std::string text(idx.begin(), idx.end());
    for (size_t a = 0; a < text.size();) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(a, e - a);
        a = e + 1;
        size_t sp = line.find(' ');
        if (sp != std::string::npos) paths.push_back(line.substr(sp + 1));
    }
    if (!export_textures(data, paths, steps, 0, 950)) return steps.why = stopped(steps) ? "Cancelled." : "The textures could not be exported.", false;
    steps.progress(960, L"Exporting the texts");
    if (!export_text(data)) return steps.why = "The texts could not be exported.", false;
    return true;
}

bool install_game(Iso &iso, const std::wstring &root_dir, InstallSteps &steps)
{
    const fs::path root = root_dir, data = root / L"data";
    std::unordered_map<uint32_t, std::string> names;
    if (!load_names(names, steps.why)) return false;
    const Iso::File *elf = iso.find("/SLUS_203.62"), *zdir_f = iso.find("/NFSHP2/ZDIR.BIN"), *zz = iso.find("/NFSHP2/ZZDATA.BIN"),
                    *irx = iso.find("/NFSHP2/IRX253/STREAM.IRX");
    if (!elf || !zdir_f || !zz || !irx) return steps.why = "The disc image lacks the game's files.", false;

    // the program and the sound module, as they are
    for (auto &x : {std::make_pair(elf, std::string("SLUS_203.62")), std::make_pair(irx, std::string("IRX253/STREAM.IRX"))}) {
        std::vector<uint8_t> b((size_t)x.first->size);
        if (!iso.read_file(*x.first, 0, b.data(), b.size()) || !write_file(under(data, x.second), b.data(), b.size()))
            return steps.why = "The disc image cannot be read, or the folder cannot be written.", false;
    }

    // the archive: its files one after another, each from a new sector, in ZDIR's order
    std::vector<uint8_t> zdir((size_t)zdir_f->size);
    if (!iso.read_file(*zdir_f, 0, zdir.data(), zdir.size())) return steps.why = "The disc image cannot be read.", false;
    struct Entry {
        uint32_t hash, at, size;
    };
    std::vector<Entry> entries;
    uint64_t lba = 0, total = 0;
    for (size_t i = 0; i + 12 <= zdir.size(); i += 12) {
        Entry e = {rd32(zdir, i), rd32(zdir, i + 4), rd32(zdir, i + 8)};
        if (e.at != lba) return steps.why = "The archive on the disc is not laid out as the port expects.", false;
        lba += (e.size + SECTOR - 1) / SECTOR;
        total += e.size;
        entries.push_back(e);
    }
    std::set<std::string> all_names;
    for (auto &n : names) {
        std::string u = n.second;
        std::replace(u.begin(), u.end(), '\\', '/');
        all_names.insert(upper(u));
    }
    std::string index;
    std::vector<std::string> paths;
    std::vector<uint8_t> buf;
    uint64_t written = 0;
    for (const Entry &e : entries) {
        if (stopped(steps)) return steps.why = "Cancelled.", false;
        std::vector<uint8_t> head(std::min<uint32_t>(e.size, 64));
        if (!head.empty() && !iso.read_file(*zz, (uint64_t)e.at * SECTOR, head.data(), head.size())) return steps.why = "The disc image cannot be read.", false;
        const std::string rel = place(e.hash, names, all_names, head);
        steps.progress((int)(700 * written / std::max<uint64_t>(total, 1)), L"Unpacking " + latin1_w(rel));
        buf.resize(e.size);
        if (e.size && !iso.read_file(*zz, (uint64_t)e.at * SECTOR, buf.data(), buf.size())) return steps.why = "The disc image cannot be read.", false;
        if (!write_file(under(data, rel), buf.data(), buf.size())) return steps.why = "The folder cannot be written (free space?).", false;
        written += e.size;
        index += hex8(e.hash) + " " + rel + "\n";
        paths.push_back(rel);
    }
    if (!write_file(data / L"index.txt", index.data(), index.size())) return steps.why = "The folder cannot be written.", false;
    if (!write_port_files(root)) return steps.why = "The port's own files could not be written.", false;

    InstallSteps sub;
    sub.stop = steps.stop;
    sub.progress = [&](int pm, const std::wstring &w) { steps.progress(700 + pm * 290 / 1000, w); };
    if (!install_exports(data.wstring(), sub)) return steps.why = sub.why, false;
    steps.progress(1000, L"Done");
    return true;
}

#else
bool install_game(Iso &, const std::wstring &, InstallSteps &s) { return s.why = "Windows only for now.", false; }
bool install_exports(const std::wstring &, InstallSteps &s) { return s.why = "Windows only for now.", false; }
#endif
