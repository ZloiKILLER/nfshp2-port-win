// savestransfer.exe: the port's optional importer of saves. It finds saves of Need for Speed: Hot Pursuit 2 where PS2 emulators and save
// tools keep them, shows them, and makes each one the user picks a new profile of the port: saves/<name>/save.dat in the game's folder, the
// same bytes the game wrote on the card (hle_iop.cpp, work/memcard/README.md). It only ever adds: a profile folder that is there already,
// in any case of its letters, is never written to, and no other file of the game is touched.
//
// Where it reads saves from:
//   - memory card images, .ps2 / .mcd (PCSX2, AetherSX2, NetherSX2...): the PS2's card file system, with or without the 16 ECC bytes a page;
//   - PCSX2's folder cards (a folder per card) and a save's own folder copied off a card (BASLUS-20362HP2 with its files);
//   - EMS / uLaunchELF .psu, Max Drive .max (LZARI), CodeBreaker .cbs (RC4 + zlib).
// The formats as mymc+ reads them (github.com/thestr4ng3r/mymcplus); LZARI is Haruhiko Okumura's LZARI.C.
//
// A window (Saves Manager): the saves found, the game's profiles, a name and Import. Files and folders come by its buttons, by
// a drop on the window or on the exe. From a command line, for scripts and the test, it prints instead of opening the window:
//   savestransfer [card or save files, folders...] --list | --import <n> --name <profile>  [--game <folder>]
//     with no files: PCSX2's memory card folders and this program's own folder
//     --game     the game's folder (default: this program's folder, else where the port was installed last)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#endif

namespace fs = std::filesystem;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef std::vector<u8> Bytes;

namespace {

struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};

u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

std::string cstr(const u8 *p, size_t n)
{
    size_t len = 0;
    while (len < n && p[len]) len++;
    return std::string((const char *)p, len);
}

std::string u8str(const fs::path &p)
{
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_u8(const std::string &s)
{
#ifdef _WIN32
    std::wstring w(s.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? (size_t)n : 0);
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

bool read_file(const fs::path &p, Bytes &out)
{
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > (256u << 20)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize((size_t)size);
    f.read((char *)out.data(), (std::streamsize)out.size());
    return (size_t)f.gcount() == out.size();
}

// ---- time ------------------------------------------------------------------------------------------
// A card's time of day: 8 bytes, (unused, sec, min, hour, day, month, year:u16), the PS2's clock in Japan's time (UTC+9).
struct Tod {
    int sec = 0, min = 0, hour = 0, day = 0, month = 0, year = 0;
};

Tod tod_at(const u8 *p) { return {p[1], p[2], p[3], p[4], p[5], rd16(p + 6)}; }

bool tod_ok(const Tod &t)
{
    return t.year >= 1995 && t.year < 2200 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour < 24 && t.min < 60 && t.sec < 61;
}

// days since 1970-01-01 of a date (H. Hinnant's days_from_civil)
int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t tod_unix(const Tod &t)
{
    return days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.min * 60 + t.sec - 9 * 3600;
}

int64_t file_unix(const fs::path &p)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    const u64 ft = (u64)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    return (int64_t)(ft / 10000000ull) - 11644473600ll;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (int64_t)st.st_mtime : 0;
#endif
}

std::string local_time(int64_t t)
{
    if (t <= 0) return "time unknown";
    time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &tt)) return "time unknown";
#else
    if (!localtime_r(&tt, &lt)) return "time unknown";
#endif
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &lt);
    return buf;
}

// ---- what a source holds ----------------------------------------------------------------------------
struct SaveFile {
    std::string name;
    Bytes data;
};

struct SaveDir {
    std::string name;
    int64_t when = 0;               // last saved, Unix time; 0 unknown
    std::vector<SaveFile> files;
};

// ---- inflate (zlib), for CodeBreaker ---------------------------------------------------------------------
// After Mark Adler's puff.c: a plain, careful decoder; speed does not matter for a save.
struct Inflate {
    const u8 *in;
    size_t len, pos = 0;
    u64 bitbuf = 0;
    int bitcnt = 0;
    Bytes out;
    size_t limit;

    struct Huff {
        short count[16], symbol[288];
    };

    int bits(int need)
    {
        u64 val = bitbuf;
        while (bitcnt < need) {
            if (pos >= len) throw Bad("the compressed data ends too early");
            val |= (u64)in[pos++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return (int)(val & ((1ull << need) - 1));
    }

    int decode(const Huff &h)
    {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l <= 15; l++) {
            code |= bits(1);
            const int count = h.count[l];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Bad("the compressed data is damaged");
    }

    static int construct(Huff &h, const short *length, int n)
    {
        short offs[16];
        for (int l = 0; l <= 15; l++) h.count[l] = 0;
        for (int s = 0; s < n; s++) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int l = 1; l <= 15; l++) {
            left <<= 1;
            left -= h.count[l];
            if (left < 0) return left;
        }
        offs[1] = 0;
        for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h.count[l]);
        for (int s = 0; s < n; s++)
            if (length[s]) h.symbol[offs[length[s]]++] = (short)s;
        return left;
    }

    void put(u8 c)
    {
        if (out.size() < limit) out.push_back(c);
    }

    void codes(const Huff &lencode, const Huff &distcode)
    {
        static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                        6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            if (out.size() >= limit) return;
            int symbol = decode(lencode);
            if (symbol < 256) {
                put((u8)symbol);
            } else if (symbol == 256) {
                return;
            } else {
                symbol -= 257;
                if (symbol >= 29) throw Bad("the compressed data is damaged");
                const int l = lens[symbol] + bits(lext[symbol]);
                symbol = decode(distcode);
                if (symbol >= 30) throw Bad("the compressed data is damaged");
                const size_t dist = (size_t)(dists[symbol] + bits(dext[symbol]));
                if (dist > out.size()) throw Bad("the compressed data is damaged");
                for (int i = 0; i < l; i++) put(out[out.size() - dist]);
            }
        }
    }

    void stored()
    {
        bitbuf = 0;
        bitcnt = 0;
        if (pos + 4 > len) throw Bad("the compressed data ends too early");
        const unsigned n = in[pos] | in[pos + 1] << 8;
        if ((in[pos + 2] | in[pos + 3] << 8) != (~n & 0xffff)) throw Bad("the compressed data is damaged");
        pos += 4;
        if (pos + n > len) throw Bad("the compressed data ends too early");
        for (unsigned i = 0; i < n; i++) put(in[pos + i]);
        pos += n;
    }

    void fixed()
    {
        static Huff lencode, distcode;
        static bool made = false;
        if (!made) {
            short lengths[288];
            int s = 0;
            for (; s < 144; s++) lengths[s] = 8;
            for (; s < 256; s++) lengths[s] = 9;
            for (; s < 280; s++) lengths[s] = 7;
            for (; s < 288; s++) lengths[s] = 8;
            construct(lencode, lengths, 288);
            for (s = 0; s < 30; s++) lengths[s] = 5;
            construct(distcode, lengths, 30);
            made = true;
        }
        codes(lencode, distcode);
    }

    void dynamic()
    {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        Huff lencode, distcode;
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw Bad("the compressed data is damaged");
        int index = 0;
        for (; index < ncode; index++) lengths[order[index]] = (short)bits(3);
        for (; index < 19; index++) lengths[order[index]] = 0;
        if (construct(lencode, lengths, 19) != 0) throw Bad("the compressed data is damaged");
        index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 16) {
                lengths[index++] = (short)symbol;
            } else {
                short l = 0;
                if (symbol == 16) {
                    if (index == 0) throw Bad("the compressed data is damaged");
                    l = lengths[index - 1];
                    symbol = 3 + bits(2);
                } else if (symbol == 17) {
                    symbol = 3 + bits(3);
                } else {
                    symbol = 11 + bits(7);
                }
                if (index + symbol > nlen + ndist) throw Bad("the compressed data is damaged");
                while (symbol--) lengths[index++] = l;
            }
        }
        if (lengths[256] == 0) throw Bad("the compressed data is damaged");
        int err = construct(lencode, lengths, nlen);
        if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) throw Bad("the compressed data is damaged");
        err = construct(distcode, lengths + nlen, ndist);
        if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) throw Bad("the compressed data is damaged");
        codes(lencode, distcode);
    }

    // a zlib stream: its 2-byte header, then deflate blocks
    Bytes run()
    {
        if (len < 2 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) throw Bad("not zlib data");
        pos = 2;
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (type == 0) stored();
            else if (type == 1) fixed();
            else if (type == 2) dynamic();
            else throw Bad("the compressed data is damaged");
        } while (!last && out.size() < limit);
        return std::move(out);
    }
};

Bytes zlib_inflate(const Bytes &in, size_t limit)
{
    Inflate z{in.data(), in.size()};
    z.limit = limit;
    return z.run();
}

// ---- LZARI (Max Drive) -------------------------------------------------------------------------------
// Okumura's LZARI.C decoder: a 4096-byte window that starts as spaces, matches of 3 to 60 bytes, the symbols and positions arithmetic
// coded with 15 bits. Past the end of the input the bits are zeros, as mymc+ reads it.
struct Lzari {
    enum { N = 4096, F = 60, THRESHOLD = 2, N_CHAR = 256 - THRESHOLD + F, M = 15 };
    static constexpr u64 Q1 = 1ull << M, Q2 = 2 * Q1, Q3 = 3 * Q1, Q4 = 4 * Q1, MAX_CUM = Q1 - 1;
    const u8 *in;
    size_t len, pos = 0;
    unsigned mask = 0, buffer = 0;
    u64 low = 0, high = Q4, value = 0;
    int sym_to_char[N_CHAR + 1];
    unsigned sym_freq[N_CHAR + 1], sym_cum[N_CHAR + 1], position_cum[N + 1];

    int bit()
    {
        if ((mask >>= 1) == 0) {
            buffer = pos < len ? in[pos] : 0;
            pos++;
            mask = 128;
        }
        return (buffer & mask) != 0;
    }

    void start_model()
    {
        sym_cum[N_CHAR] = 0;
        for (int sym = N_CHAR; sym >= 1; sym--) {
            sym_to_char[sym] = sym - 1;
            sym_freq[sym] = 1;
            sym_cum[sym - 1] = sym_cum[sym] + sym_freq[sym];
        }
        sym_freq[0] = 0;
        position_cum[N] = 0;
        for (int i = N; i >= 1; i--) position_cum[i - 1] = position_cum[i] + 10000 / (i + 200);
    }

    void update_model(int sym)
    {
        if (sym_cum[0] >= MAX_CUM) {
            unsigned c = 0;
            for (int i = N_CHAR; i > 0; i--) {
                sym_cum[i] = c;
                c += (sym_freq[i] = (sym_freq[i] + 1) >> 1);
            }
            sym_cum[0] = c;
        }
        int i;
        for (i = sym; sym_freq[i] == sym_freq[i - 1]; i--) {
        }
        if (i < sym) std::swap(sym_to_char[i], sym_to_char[sym]);
        sym_freq[i]++;
        while (--i >= 0) sym_cum[i]++;
    }

    int search_sym(unsigned x) const
    {
        int i = 1, j = N_CHAR;
        while (i < j) {
            const int k = (i + j) / 2;
            if (sym_cum[k] > x) i = k + 1;
            else j = k;
        }
        return i;
    }

    int search_pos(unsigned x) const
    {
        int i = 1, j = N;
        while (i < j) {
            const int k = (i + j) / 2;
            if (position_cum[k] > x) i = k + 1;
            else j = k;
        }
        return i - 1;
    }

    void renormalize()
    {
        for (;;) {
            if (low >= Q2) {
                value -= Q2;
                low -= Q2;
                high -= Q2;
            } else if (low >= Q1 && high <= Q3) {
                value -= Q1;
                low -= Q1;
                high -= Q1;
            } else if (high > Q2) {
                break;
            }
            low += low;
            high += high;
            value = 2 * value + bit();
        }
    }

    int decode_char()
    {
        const u64 range = high - low;
        const int sym = search_sym((unsigned)(((value - low + 1) * sym_cum[0] - 1) / range));
        high = low + range * sym_cum[sym - 1] / sym_cum[0];
        low += range * sym_cum[sym] / sym_cum[0];
        renormalize();
        const int ch = sym_to_char[sym];
        update_model(sym);
        return ch;
    }

    int decode_position()
    {
        const u64 range = high - low;
        const int p = search_pos((unsigned)(((value - low + 1) * position_cum[0] - 1) / range));
        high = low + range * position_cum[p] / position_cum[0];
        low += range * position_cum[p + 1] / position_cum[0];
        renormalize();
        return p;
    }

    Bytes run(size_t textsize)
    {
        Bytes out;
        out.reserve(textsize);
        for (int i = 0; i < M + 2; i++) value = 2 * value + bit();
        start_model();
        u8 text_buf[N];
        memset(text_buf, ' ', N - F);
        memset(text_buf + N - F, 0, F);
        int r = N - F;
        while (out.size() < textsize) {
            if (pos > len + 64) throw Bad("the compressed data ends too early");
            const int c = decode_char();
            if (c < 256) {
                out.push_back((u8)c);
                text_buf[r++] = (u8)c;
                r &= N - 1;
            } else {
                const int i = (r - decode_position() - 1) & (N - 1);
                const int j = c - 255 + THRESHOLD;
                for (int k = 0; k < j && out.size() < textsize; k++) {
                    const u8 b = text_buf[(i + k) & (N - 1)];
                    out.push_back(b);
                    text_buf[r++] = b;
                    r &= N - 1;
                }
            }
        }
        return out;
    }
};

Bytes lzari_decode(const u8 *in, size_t len, size_t textsize)
{
    Lzari z;
    z.in = in;
    z.len = len;
    return z.run(textsize);
}

// ---- the PS2 card's file system ------------------------------------------------------------------------
// The superblock at the start; clusters of pages (pages of 512 bytes, in the image each followed by 16 bytes of ECC, or not); a FAT
// reached through the indirect clusters the superblock lists; directories of 512-byte entries. Cluster numbers in the FAT and in the
// entries count from the first allocatable cluster.
const u16 DF_FILE = 0x0010, DF_DIR = 0x0020, DF_EXISTS = 0x8000;

bool mode_dir(u16 m) { return (m & (DF_FILE | DF_DIR | DF_EXISTS)) == (DF_DIR | DF_EXISTS); }
bool mode_file(u16 m) { return (m & (DF_FILE | DF_DIR | DF_EXISTS)) == (DF_FILE | DF_EXISTS); }

const char kCardMagic[] = "Sony PS2 Memory Card Format ";

struct Card {
    const Bytes &d;
    u32 page = 0, ppc = 0, raw = 0, cluster_size = 0, epc = 0, alloc_off = 0, alloc_end = 0, root = 0;
    u32 ifc[32] = {};
    Bytes buf;

    explicit Card(const Bytes &data) : d(data) {}

    void open()
    {
        if (d.size() < 0x154 || memcmp(d.data(), kCardMagic, sizeof kCardMagic - 1)) throw Bad("not a PS2 memory card image");
        page = rd16(&d[0x28]);
        ppc = rd16(&d[0x2a]);
        const u32 clusters = rd32(&d[0x30]);
        alloc_off = rd32(&d[0x34]);
        alloc_end = rd32(&d[0x38]);
        root = rd32(&d[0x3c]);
        for (int i = 0; i < 32; i++) ifc[i] = rd32(&d[0x50 + 4 * i]);
        if (page < 512 || page > 8192 || ppc < 1 || ppc > 16) throw Bad("the card's superblock is damaged");
        cluster_size = page * ppc;
        epc = cluster_size / 4;
        const u32 spare = (page + 127) / 128 * 4;
        raw = d.size() % (page + spare) == 0 && d.size() / (page + spare) >= (u64)clusters * ppc ? page + spare : page;
        buf.resize(cluster_size);
    }

    // an absolute cluster into buf
    const u8 *cluster(u32 n)
    {
        for (u32 i = 0; i < ppc; i++) {
            const u64 at = ((u64)n * ppc + i) * raw;
            if (at + page > d.size()) throw Bad("the card image is cut short");
            memcpy(&buf[i * page], &d[(size_t)at], page);
        }
        return buf.data();
    }

    u32 fat(u32 n)
    {
        if (n >= alloc_end) throw Bad("the card's FAT is damaged");
        const u32 fat_index = n / epc, ind_index = fat_index / epc;
        if (ind_index >= 32) throw Bad("the card's FAT is damaged");
        const u32 fat_cluster = rd32(cluster(ifc[ind_index]) + 4 * (fat_index % epc));
        return rd32(cluster(fat_cluster) + 4 * (n % epc));
    }

    Bytes read(u32 first, u64 length)
    {
        Bytes out;
        if (length > (64u << 20)) throw Bad("the card's directory is damaged");
        out.reserve((size_t)length);
        u32 c = first;
        for (u32 steps = 0; out.size() < length; steps++) {
            if (steps > alloc_end) throw Bad("the card's FAT is damaged");
            const u8 *p = cluster(c + alloc_off);
            out.insert(out.end(), p, p + std::min<u64>(cluster_size, length - out.size()));
            if (out.size() >= length) break;
            const u32 e = fat(c);
            if (e == 0xffffffff || !(e & 0x80000000)) throw Bad("a file on the card is cut short");
            c = e & 0x7fffffff;
        }
        return out;
    }

    struct Ent {
        u16 mode;
        u32 length, cluster;
        Tod modified;
        std::string name;
    };

    std::vector<Ent> dir(u32 first, u32 count)
    {
        if (count > 65536) throw Bad("the card's directory is damaged");
        const Bytes b = read(first, (u64)count * 512);
        std::vector<Ent> out;
        for (u32 i = 0; i < count; i++) {
            const u8 *e = &b[(size_t)i * 512];
            out.push_back({rd16(e), rd32(e + 4), rd32(e + 0x10), tod_at(e + 0x18), cstr(e + 0x40, 32)});
        }
        return out;
    }
};

bool is_game_dir_name(const std::string &n);

// the saves of an image: only the directories whose name could be this game's, with their files
std::vector<SaveDir> card_saves(const Bytes &d)
{
    Card c(d);
    c.open();
    const std::vector<Card::Ent> top = c.dir(c.root, 1);
    if (top[0].name != "." || !mode_dir(top[0].mode)) throw Bad("the card's root directory is damaged");
    std::vector<SaveDir> out;
    for (const Card::Ent &e : c.dir(c.root, top[0].length)) {
        if (!mode_dir(e.mode) || e.name == "." || e.name == ".." || !is_game_dir_name(e.name)) continue;
        SaveDir s;
        s.name = e.name;
        s.when = tod_ok(e.modified) ? tod_unix(e.modified) : 0;
        for (const Card::Ent &f : c.dir(e.cluster, e.length))
            if (mode_file(f.mode) && f.name == e.name) {
                s.files.push_back({f.name, c.read(f.cluster, f.length)});
                if (tod_ok(f.modified)) s.when = tod_unix(f.modified);
            }
        out.push_back(std::move(s));
    }
    return out;
}

// ---- the save files ------------------------------------------------------------------------------------
// EMS / uLaunchELF .psu: the directory's entry (its length counts "." and ".."), ".", "..", then each file's entry and its bytes,
// padded to 1024.
bool looks_psu(const Bytes &d)
{
    return d.size() >= 1536 && mode_dir(rd16(&d[0])) && mode_dir(rd16(&d[512])) && mode_dir(rd16(&d[1024])) && rd32(&d[4]) >= 2 &&
           cstr(&d[512 + 0x40], 32) == "." && cstr(&d[1024 + 0x40], 32) == "..";
}

SaveDir psu_save(const Bytes &d)
{
    SaveDir s;
    s.name = cstr(&d[0x40], 32);
    const Tod t = tod_at(&d[0x18]);
    s.when = tod_ok(t) ? tod_unix(t) : 0;
    const u32 count = rd32(&d[4]) - 2;
    size_t pos = 1536;
    for (u32 i = 0; i < count; i++) {
        if (pos + 512 > d.size()) throw Bad("the file is cut short");
        const u8 *e = &d[pos];
        if (!mode_file(rd16(e))) throw Bad("the save holds a folder, which a PS2 save does not");
        const u32 flen = rd32(e + 4);
        pos += 512;
        if (pos + flen > d.size()) throw Bad("the file is cut short");
        s.files.push_back({cstr(e + 0x40, 32), Bytes(d.begin() + pos, d.begin() + pos + flen)});
        pos += (flen + 1023) / 1024 * 1024;
    }
    return s;
}

// Max Drive .max: "Ps2PowerSave", a CRC, the directory's name, the icon's title, the compressed length (+4), the number of files,
// the length unpacked; then LZARI data. Unpacked: per file its length, its name (32) and its bytes, the next starting at 16n + 8.
SaveDir max_save(const Bytes &d)
{
    if (d.size() < 0x5c) throw Bad("the file is cut short");
    SaveDir s;
    s.name = cstr(&d[0x10], 32);
    const u32 clen = rd32(&d[0x50]), count = rd32(&d[0x54]), length = rd32(&d[0x58]);
    if (length > (64u << 20) || count > 4096) throw Bad("the file's header is damaged");
    size_t avail = d.size() - 0x5c;
    if (clen != length) {
        if (clen < 4 || clen - 4 > avail) throw Bad("the file is cut short");
        avail = clen - 4;
    }
    const Bytes u = lzari_decode(&d[0x5c], avail, length);
    size_t off = 0;
    for (u32 i = 0; i < count; i++) {
        if (off + 36 > u.size()) throw Bad("the save's data is cut short");
        const u32 l = rd32(&u[off]);
        const std::string name = cstr(&u[off + 4], 32);
        off += 36;
        if (off + l > u.size()) throw Bad("the save's data is cut short");
        s.files.push_back({name, Bytes(u.begin() + off, u.begin() + off + l)});
        off += l;
        off = (off + 8 + 15) / 16 * 16 - 8;
    }
    return s;
}

// CodeBreaker .cbs: "CFU\0", ?, the header's length, the length unpacked, the body's length, the directory's name, its times, its mode,
// the title; the body is RC4 (with a fixed starting permutation rather than a key) over zlib. Unpacked: per file a 64-byte header
// (created, modified, length, mode, ?, ?, name) and its bytes.
const u8 kCbsRc4[256] = {
    0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c, 0x07, 0x4c, 0x9c, 0xb4, 0x81, 0xb8, 0xef, 0x98, 0x59, 0xae, 0xf9, 0x26,
    0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62, 0x7c, 0x64, 0x46, 0xf4, 0x34, 0x1a, 0xf6, 0xe1, 0xba, 0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16,
    0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56, 0x0c, 0x91, 0x00, 0x0b, 0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52,
    0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0, 0x65, 0x84, 0xb2, 0xd6, 0xdf, 0x15, 0x3c, 0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1,
    0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03, 0x92, 0x70, 0x05, 0x7d, 0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d,
    0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5, 0x54, 0xcf, 0xe7, 0x41, 0xe4, 0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06,
    0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0, 0x11, 0xc2, 0xea, 0x74, 0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38,
    0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e, 0x9b, 0x7f, 0xd8, 0x6e, 0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed,
    0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a, 0xbc, 0xa8, 0xf2, 0xee, 0xeb, 0xf5, 0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d,
    0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3, 0x58, 0xdb, 0xc7, 0x4b, 0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b,
    0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8, 0x6b, 0x93, 0x32, 0x48, 0xb6, 0x30, 0x43, 0xa5};

SaveDir cbs_save(const Bytes &d)
{
    if (d.size() < 12) throw Bad("the file is cut short");
    const u32 hlen = rd32(&d[8]);
    if (hlen < 92 + 32 || hlen > d.size()) throw Bad("the file's header is damaged");
    const u32 dlen = rd32(&d[12]), flen = rd32(&d[16]);
    if (dlen > (64u << 20)) throw Bad("the file's header is damaged");
    SaveDir s;
    s.name = cstr(&d[20], 32);
    const Tod t = tod_at(&d[60]);
    s.when = tod_ok(t) ? tod_unix(t) : 0;
    // the body's length is that of the body, or of the whole file
    const size_t avail = d.size() - hlen;
    size_t blen;
    if (avail >= flen) blen = flen;
    else if (flen >= hlen && avail == flen - hlen) blen = avail;
    else throw Bad("the file is cut short");
    Bytes body(d.begin() + hlen, d.begin() + hlen + blen);
    u8 st[256];
    memcpy(st, kCbsRc4, 256);
    unsigned j = 0;
    for (size_t k = 0; k < body.size(); k++) {
        const unsigned i = (unsigned)((k + 1) & 255);
        j = (j + st[i]) & 255;
        std::swap(st[i], st[j]);
        body[k] ^= st[(st[i] + st[j]) & 255];
    }
    const Bytes u = zlib_inflate(body, dlen);
    size_t off = 0;
    while (off < u.size()) {
        if (off + 64 > u.size()) throw Bad("the save's data is cut short");
        const u8 *e = &u[off];
        const u32 size = rd32(e + 16);
        if (!mode_file(rd16(e + 20))) throw Bad("the save holds a folder, which a PS2 save does not");
        if (off + 64 + size > u.size()) throw Bad("the save's data is cut short");
        s.files.push_back({cstr(e + 32, 32), Bytes(u.begin() + off + 64, u.begin() + off + 64 + size)});
        off += 64 + size;
    }
    return s;
}

// ---- this game's saves -----------------------------------------------------------------------------------
// The game's directory is key code + product number + title: BASLUS-20362HP2 on the USA disc, BESLES-50731HP2 on Europe's. The port plays
// the USA program for every disc, and the save is the image of the game's state (FrontendDatabase, 0x114e0 bytes, its first word its own
// length). A pre-release build named its directory ...NFS6 and its save is 4 bytes shorter: not this game's.
const u32 kSaveSize = 0x114e0;

bool is_game_dir_name(const std::string &n)
{
    if (n.size() != 15 && n.size() != 16) return false;
    if (n[0] != 'B' || n[6] != '-') return false;
    for (int i = 7; i < 12; i++)
        if (n[i] < '0' || n[i] > '9') return false;
    const std::string title = n.substr(12);
    return title == "HP2" || title == "NFS6";
}

std::string region_of(const std::string &dir)
{
    const std::string product = dir.substr(2, 10);
    if (product == "SLUS-20362") return "USA";
    if (product == "SLES-50731") return "Europe";
    if (product == "SLPM-65181") return "Asia";
    if (product == "SLPM-67527") return "Korea";
    return product;
}

// where a save was found: a card image, a folder card, a save's own folder, a save file
enum { SRC_CARD, SRC_FOLDER_CARD, SRC_SAVE_FOLDER, SRC_FILE };

struct Found {
    fs::path source;            // the card, folder or file
    std::string dir;            // the save's folder on it
    int src = SRC_FILE;
    std::string where;          // the card or file it came from, as shown: its kind and full path
    std::string short_where;    // its kind and file name (the window's column)
    std::string what;           // "USA save"
    int64_t when = 0;
    Bytes save;
    std::string problem;        // why it cannot be imported; empty: it can
};

// a file or folder looked at that has nothing to import, and why
struct Note {
    std::string where, text, short_where;
};

void add_saves(std::vector<Found> &out, const std::string &kind, const fs::path &path, int src, std::vector<SaveDir> dirs, int64_t fallback_when,
               std::vector<Note> &notes)
{
    const std::string where = kind + " " + u8str(path), short_where = kind + " " + u8str(path.filename());
    bool any = false;
    for (SaveDir &s : dirs) {
        if (!is_game_dir_name(s.name)) continue;
        any = true;
        Found f;
        f.source = path;
        f.dir = s.name;
        f.src = src;
        f.where = where;
        f.short_where = short_where;
        f.when = s.when ? s.when : fallback_when;
        f.what = region_of(s.name) + " save (" + s.name + ")";
        const SaveFile *file = nullptr;
        for (const SaveFile &x : s.files)
            if (x.name == s.name) file = &x;
        if (s.name.substr(12) == "NFS6") f.problem = "a save of a pre-release version of the game, not of the released one the port plays";
        else if (!file) f.problem = "the folder has no save file " + s.name + " in it";
        else if (file->data.size() != kSaveSize || rd32(file->data.data()) != kSaveSize) f.problem = "its save is " + std::to_string(file->data.size()) + " bytes, not the game's " + std::to_string(kSaveSize);
        else f.save = file->data;
        out.push_back(std::move(f));
    }
    if (!any) notes.push_back({where, "no Hot Pursuit 2 save in it", short_where});
}

// A file or a folder: what is in it. Folders: a save's own folder (BASLUS-20362HP2 with its files), or a card as a folder (PCSX2's
// folder cards: a folder per save in it).
void look_in(const fs::path &p, std::vector<Found> &out, std::vector<Note> &notes)
{
    std::error_code ec;
    const std::string shown = u8str(p);
    if (fs::is_directory(p, ec)) {
        auto read_dir = [&](const fs::path &dir) {
            SaveDir s;
            s.name = u8str(dir.filename());
            const fs::path file = dir / dir.filename();
            Bytes b;
            if (fs::is_regular_file(file, ec) && read_file(file, b)) {
                s.files.push_back({s.name, std::move(b)});
                s.when = file_unix(file);
            }
            return s;
        };
        if (is_game_dir_name(u8str(p.filename()))) {
            add_saves(out, "folder", p, SRC_SAVE_FOLDER, {read_dir(p)}, 0, notes);
            return;
        }
        std::vector<SaveDir> dirs;
        for (const auto &e : fs::directory_iterator(p, ec))
            if (e.is_directory(ec) && is_game_dir_name(u8str(e.path().filename()))) dirs.push_back(read_dir(e.path()));
        add_saves(out, "folder card", p, SRC_FOLDER_CARD, std::move(dirs), 0, notes);
        return;
    }
    Bytes d;
    if (!read_file(p, d)) {
        notes.push_back({shown, "cannot be read", u8str(p.filename())});
        return;
    }
    const int64_t mtime = file_unix(p);
    try {
        if (d.size() >= 28 && !memcmp(d.data(), kCardMagic, sizeof kCardMagic - 1)) add_saves(out, "card", p, SRC_CARD, card_saves(d), 0, notes);
        else if (d.size() >= 12 && !memcmp(d.data(), "Ps2PowerSave", 12)) add_saves(out, "Max Drive file", p, SRC_FILE, {max_save(d)}, mtime, notes);
        else if (d.size() >= 4 && !memcmp(d.data(), "CFU\0", 4)) add_saves(out, "CodeBreaker file", p, SRC_FILE, {cbs_save(d)}, 0, notes);
        else if (looks_psu(d)) add_saves(out, "PSU file", p, SRC_FILE, {psu_save(d)}, 0, notes);
        else if (d.size() >= 8u << 20 && std::all_of(d.begin(), d.begin() + 512, [](u8 b) { return b == 0xff; }))
            notes.push_back({shown, "an empty card, never formatted", u8str(p.filename())});
        else notes.push_back({shown, "not a memory card or a save file this program knows", u8str(p.filename())});
    } catch (const Bad &e) {
        notes.push_back({shown, e.what(), u8str(p.filename())});
    }
}

bool card_like(const fs::path &p)
{
    std::string ext = u8str(p.extension());
    for (char &c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".ps2" || ext == ".mcd" || ext == ".mc2" || ext == ".psu" || ext == ".max" || ext == ".cbs";
}

// ---- where to look --------------------------------------------------------------------------------------
fs::path exe_dir()
{
#ifdef _WIN32
    wchar_t p[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    return fs::path(std::wstring(p, n)).parent_path();
#else
    std::error_code ec;
    return fs::current_path(ec);
#endif
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

// PCSX2's memory card folders: its own default (Documents\PCSX2\memcards) and what its settings name instead
std::vector<fs::path> pcsx2_card_folders()
{
    std::vector<fs::path> out;
#ifdef _WIN32
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        const fs::path root = fs::path(docs) / L"PCSX2";
        out.push_back(root / L"memcards");
        for (const wchar_t *ini : {L"PCSX2.ini", L"PCSX2_ui.ini"}) {
            std::ifstream f(root / L"inis" / ini);
            std::string line;
            bool folders = false;
            while (std::getline(f, line)) {
                line = trim(line);
                if (!line.empty() && line[0] == '[') folders = line == "[Folders]";
                else if (folders && line.rfind("MemoryCards", 0) == 0) {
                    const size_t eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    fs::path p = from_u8(trim(line.substr(eq + 1)));
                    if (p.empty()) continue;
                    if (p.is_relative()) p = root / p;
                    out.push_back(p);
                }
            }
        }
    }
    if (docs) CoTaskMemFree(docs);
#endif
    std::vector<fs::path> uniq;
    std::error_code ec;
    for (const fs::path &p : out) {
        bool seen = false;
        for (const fs::path &q : uniq) seen = seen || fs::equivalent(p, q, ec);
        if (!seen && fs::is_directory(p, ec)) uniq.push_back(p);
    }
    return uniq;
}

bool is_game_folder(const fs::path &dir)
{
    std::error_code ec;
    return fs::is_regular_file(dir / "data" / "index.txt", ec) && fs::is_regular_file(dir / "data" / "SLUS_203.62", ec);
}

// the game's folder: this program's, else where the port's setup installed it last (setup.cpp)
fs::path find_game()
{
    const fs::path here = exe_dir();
    if (is_game_folder(here)) return here;
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD bytes = sizeof buf - sizeof buf[0];
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NFSHP2Port", L"InstallDir", RRF_RT_REG_SZ, nullptr, buf, &bytes) == ERROR_SUCCESS && is_game_folder(buf))
        return fs::path(buf);
#endif
    if (is_game_folder(here / "Need for Speed Hot Pursuit 2")) return here / "Need for Speed Hot Pursuit 2";
    if (is_game_folder(here / "nfshp2port")) return here / "nfshp2port";
    return {};
}

// ---- the profiles ------------------------------------------------------------------------------------
// Names as the port's own New Profile takes them (pcsettings.cpp): up to 15 letters of plain ASCII, no spaces or dots at the end, unique
// whatever the case of the letters; and nothing a Windows folder cannot be called.
bool ieq(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

std::vector<std::string> profiles(const fs::path &saves)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(saves, ec)) out.push_back(u8str(e.path().filename()));
    // a save from before profiles is made Player1 at the game's next start (hle_iop.cpp, mc_profiles)
    if (fs::exists(saves / "save.dat", ec)) out.push_back("Player1");
    std::sort(out.begin(), out.end());
    return out;
}

std::string check_name(std::string &name, const fs::path &saves)
{
    name = trim(name);
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) return "A profile needs a name.";
    if (name.size() > 15) return "At most 15 letters, as in the game.";
    for (char c : name)
        if ((unsigned char)c < 32 || (unsigned char)c > 126 || strchr("<>:\"/\\|?*", c)) return "Letters, digits and plain signs only (not < > : \" / \\ | ? *).";
    std::string base = name.substr(0, name.find('.'));
    static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM0", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                           "LPT0", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char *r : reserved)
        if (ieq(base, r)) return "Windows keeps that name for itself.";
    for (const std::string &p : profiles(saves))
        if (ieq(p, name)) return "There is a profile of that name. Its save stays as it is: choose another name.";
    return "";
}

// The new profile: its folder made here and now (one there already is never used) and save.dat made new in it, with the time the save
// was made on the card. Nothing that was there is written.
bool make_profile(const fs::path &saves, const std::string &name, const Found &f, std::string &err)
{
    std::error_code ec;
    fs::create_directories(saves, ec);
    const fs::path dir = saves / from_u8(name);
    if (!fs::create_directory(dir, ec)) {
        err = ec ? "the folder " + u8str(dir) + " cannot be made: " + ec.message() : "there is a profile of that name";
        return false;
    }
    const fs::path file = dir / "save.dat";
    bool ok = false;
#ifdef _WIN32
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        ok = WriteFile(h, f.save.data(), (DWORD)f.save.size(), &wrote, nullptr) && wrote == f.save.size();
        if (ok && f.when > 0) {
            const u64 ft = ((u64)f.when + 11644473600ull) * 10000000ull;
            FILETIME t = {(DWORD)ft, (DWORD)(ft >> 32)};
            SetFileTime(h, nullptr, nullptr, &t);
        }
        ok = CloseHandle(h) && ok;
    }
#else
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        ok = write(fd, f.save.data(), f.save.size()) == (ssize_t)f.save.size();
        ok = close(fd) == 0 && ok;
        if (ok && f.when > 0) {
            struct utimbuf t = {(time_t)f.when, (time_t)f.when};
            utime(file.c_str(), &t);
        }
    }
#endif
    if (!ok) {
        err = "the save could not be written in " + u8str(dir);
        fs::remove(file, ec);           // the file and the folder this program made, nothing else
        fs::remove(dir, ec);
        return false;
    }
    return true;
}

// ---- writing back: a profile's save onto a card or into a save file -----------------------------------------
// What the game itself puts on the card: its folder (named by the disc, see kDiscs) holding the save, icon.sys and nfs6.ico. The save
// is the profile's save.dat; the icon is the game's own data/NFS6.ICO from the game's folder (the same file the game writes); icon.sys
// is made here with the values the game's code gives it. The profile's name stays in the port: on the card there is only the save.
struct Disc {
    const char *key, *name, *dir;
};
const Disc kDiscs[] = {{"usa", "USA (SLUS-20362)", "BASLUS-20362HP2"},
                       {"europe", "Europe v1.00 and v2.00 (SLES-50731)", "BESLES-50731HP2"},
                       {"asia", "Asia (SLPM-65181)", "BISLPM-65181HP2"},
                       {"korea", "Korea (SLPM-67527)", "BKSLPM-67527HP2"}};   // key codes BI, BK: the Asia and Korea programs' own (cDBFileSys ctor, 0x12dee0)

const u16 MODE_DIR = 0x8427, MODE_FILE = 0x8497;      // as the game's own save has them
const u32 FAT_END = 0xffffffff, FAT_USED = 0x80000000;

void put16(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8); }
void put32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
void putf(u8 *p, float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    put32(p, v);
}

// a Unix time as the card's 8 bytes of time of day, in Japan's time as the PS2 keeps it
void put_tod(u8 *p, int64_t t)
{
    t += 9 * 3600;
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) secs += 86400, days--;
    // H. Hinnant's civil_from_days
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    const int y = (int)(yoe + era * 400 + (m <= 2));
    p[0] = 0;
    p[1] = (u8)(secs % 60);
    p[2] = (u8)(secs / 60 % 60);
    p[3] = (u8)(secs / 3600);
    p[4] = (u8)d;
    p[5] = (u8)m;
    put16(p + 6, (u32)y);
}

// icon.sys as the game makes it (cDBFileSys::SetIconData): the title NFS:HP2 in full-width Shift-JIS on one line, the background,
// the three lights, and nfs6.ico for the icon of every state
Bytes make_icon_sys()
{
    Bytes b(964, 0);
    memcpy(&b[0], "PS2D", 4);
    put16(&b[6], 14);                       // where the title's second line would begin: after all of it
    put32(&b[12], 128);                     // the background's opacity
    const u32 bg[4][4] = {{128, 0, 0, 0}, {0, 128, 0, 0}, {0, 0, 128, 0}, {128, 128, 128, 0}};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) put32(&b[0x10 + 16 * i + 4 * j], bg[i][j]);
    const float lights[7][4] = {{0.5f, 0.5f, 0.5f, 0}, {0, -0.4f, -0.1f, 0}, {-0.5f, -0.5f, 0.5f, 0},     // directions
                                {0.48f, 0.48f, 0.03f, 0}, {0.5f, 0.33f, 0.2f, 0}, {0.14f, 0.14f, 0.38f, 0},  // colours
                                {0.5f, 0.5f, 0.5f, 0}};                                                       // ambient
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 4; j++) putf(&b[0x50 + 16 * i + 4 * j], lights[i][j]);
    static const u8 title[] = {0x82, 0x6d, 0x82, 0x65, 0x82, 0x72, 0x81, 0x46, 0x82, 0x67, 0x82, 0x6f, 0x82, 0x51};   // ＮＦＳ：ＨＰ２
    memcpy(&b[0xc0], title, sizeof title);
    for (int o : {0x104, 0x144, 0x184}) memcpy(&b[(size_t)o], "nfs6.ico", 8);
    return b;
}

struct OutSave {
    std::string dir;
    int64_t when = 0;
    std::vector<SaveFile> files;            // in the game's order: the save, icon.sys, nfs6.ico
};

// the card's files of a profile, or why there are none
bool make_out_save(const fs::path &game, const std::string &profile, const Disc &disc, OutSave &out, std::string &err)
{
    const fs::path save = game / "saves" / from_u8(profile) / "save.dat", icon = game / "data" / "NFS6.ICO";
    Bytes s, ico;
    if (!read_file(save, s)) {
        err = "the profile " + profile + " has no save yet (play and save once)";
        return false;
    }
    if (s.size() != kSaveSize || rd32(s.data()) != kSaveSize) {
        err = "the profile's save.dat is " + std::to_string(s.size()) + " bytes, not the game's " + std::to_string(kSaveSize);
        return false;
    }
    if (!read_file(icon, ico) || ico.size() < 20 || rd32(ico.data()) != 0x10000) {
        err = "the game's icon " + u8str(icon) + " is missing or damaged";
        return false;
    }
    out.dir = disc.dir;
    out.when = file_unix(save);
    if (out.when <= 0) out.when = (int64_t)time(nullptr);
    out.files = {{disc.dir, std::move(s)}, {"icon.sys", make_icon_sys()}, {"nfs6.ico", std::move(ico)}};
    return true;
}

// ---- into a card image ----------------------------------------------------------------------------------
// The image is changed in memory and written as a new file: the card's FAT gets the clusters, the root directory the save's folder
// (in the first deleted entry, else at its end), the folder its ".", ".." and files as the game makes them, and every page written
// its ECC again (3 bytes of Hamming code a 128 bytes, as mymc+ and PCSX2 have it). A save of the game there already, a card that is
// full or damaged: nothing is written.
struct CardWriter {
    Bytes d;
    u32 page = 0, ppc = 0, raw = 0, cs = 0, epc = 0, alloc_off = 0, alloc_end = 0, root = 0, limit = 0;
    u32 ifc[32] = {};
    std::vector<bool> dirty;                // pages written

    void open()
    {
        Card c(d);
        c.open();
        page = c.page, ppc = c.ppc, raw = c.raw, cs = c.cluster_size, epc = c.epc, alloc_off = c.alloc_off, alloc_end = c.alloc_end, root = c.root;
        memcpy(ifc, c.ifc, sizeof ifc);
        const u32 ppeb = rd16(&d[0x2c]), good1 = rd32(&d[0x40]), good2 = rd32(&d[0x44]);
        limit = alloc_end;
        if (ppeb && good1 && good2) limit = std::min<u32>(limit, std::min(good1, good2) * ppeb / ppc - alloc_off);
        dirty.assign(d.size() / raw, false);
    }

    u8 *page_at(u32 abs_page)
    {
        const u64 at = (u64)abs_page * raw;
        if (at + page > d.size()) throw Bad("the card image is cut short");
        return &d[(size_t)at];
    }
    // a byte of an absolute cluster
    u8 *at(u32 abs_cluster, u32 off, bool write)
    {
        const u32 p = abs_cluster * ppc + off / page;
        if (write) dirty[p] = true;
        return page_at(p) + off % page;
    }

    u32 fat_slot(u32 n, u32 &abs, u32 &off)
    {
        if (n >= alloc_end) throw Bad("the card's FAT is damaged");
        const u32 fi = n / epc, ii = fi / epc;
        if (ii >= 32) throw Bad("the card's FAT is damaged");
        abs = rd32(at(ifc[ii], 4 * (fi % epc), false));
        off = 4 * (n % epc);
        return rd32(at(abs, off, false));
    }
    u32 fat(u32 n)
    {
        u32 a, o;
        return fat_slot(n, a, o);
    }
    void set_fat(u32 n, u32 v)
    {
        u32 a, o;
        fat_slot(n, a, o);
        put32(at(a, o, true), v);
    }

    u32 free_clusters()
    {
        u32 n = 0;
        for (u32 i = 0; i < limit; i++) n += !(fat(i) & FAT_USED);
        return n;
    }
    u32 alloc()
    {
        for (u32 i = 0; i < limit; i++)
            if (!(fat(i) & FAT_USED)) {
                set_fat(i, FAT_END);
                return i;
            }
        throw Bad("the card is full");
    }

    std::vector<u32> chain(u32 first)
    {
        std::vector<u32> out;
        for (u32 c = first;;) {
            if (out.size() > limit) throw Bad("the card's FAT is damaged");
            out.push_back(c);
            const u32 e = fat(c);
            if (e == FAT_END || !(e & FAT_USED)) return out;
            c = e & 0x7fffffff;
        }
    }

    // data into new clusters, chained; the first (FAT_END for none)
    u32 write_new(const Bytes &data, u32 at_least = 0)
    {
        const u32 n = std::max<u32>(at_least, (u32)((data.size() + cs - 1) / cs));
        u32 first = FAT_END, prev = FAT_END;
        for (u32 i = 0; i < n; i++) {
            const u32 c = alloc();
            if (prev == FAT_END) first = c;
            else set_fat(prev, FAT_USED | c);
            for (u32 off = 0; off < cs; off++) *at(c + alloc_off, off, true) = (size_t)i * cs + off < data.size() ? data[(size_t)i * cs + off] : 0;
            prev = c;
        }
        return first;
    }

    // a directory entry, 512 bytes
    static void entry(u8 *e, u16 mode, u32 length, const u8 *created, u32 cluster, u32 dir_entry, const u8 *modified, const std::string &name)
    {
        memset(e, 0, 512);
        put16(e, mode);
        put32(e + 4, length);
        memcpy(e + 8, created, 8);
        put32(e + 0x10, cluster);
        put32(e + 0x14, dir_entry);
        memcpy(e + 0x18, modified, 8);
        memcpy(e + 0x40, name.data(), std::min<size_t>(name.size(), 32));
    }

    void add(const OutSave &s)
    {
        // the root: its entries, the save's folder not among them
        std::vector<u32> rc = chain(root);
        const u32 per = cs / 512;
        auto root_entry = [&](u32 i, bool write) -> u8 * {
            if (i / per >= rc.size()) throw Bad("the card's root directory is damaged");
            return at(rc[i / per] + alloc_off, i % per * 512, write);
        };
        const u32 count = rd32(root_entry(0, false) + 4);
        if (count < 2 || count > 65536) throw Bad("the card's root directory is damaged");
        u32 slot = count;
        for (u32 i = 2; i < count; i++) {
            const u8 *e = root_entry(i, false);
            const u16 mode = rd16(e);
            if (!(mode & DF_EXISTS)) {
                if (slot == count) slot = i;
                continue;
            }
            if (cstr(e + 0x40, 32) == s.dir) throw Bad("the card has a save of the game in " + s.dir + " already");
        }
        // room: the folder's entries, the files, and the root one cluster longer if the folder's entry is past its end
        const u32 nent = 2 + (u32)s.files.size();
        u32 need = (nent * 512 + cs - 1) / cs + (slot / per >= rc.size() ? 1 : 0);
        for (const SaveFile &f : s.files) need += (u32)((f.data.size() + cs - 1) / cs);
        const u32 have = free_clusters();
        if (need > have)
            throw Bad("the card has " + std::to_string(have * cs / 1024) + " KB free, the save needs " + std::to_string(need * cs / 1024) + " KB");

        u8 now[8];
        put_tod(now, s.when);
        u8 root_dot[512];
        memcpy(root_dot, root_entry(0, false), 512);
        // the files, then the folder that lists them
        Bytes dir((size_t)nent * 512, 0);
        std::vector<u32> firsts;
        for (const SaveFile &f : s.files) firsts.push_back(f.data.empty() ? FAT_END : write_new(f.data));
        entry(&dir[0], MODE_DIR, 0, now, root, slot, now, ".");
        entry(&dir[512], MODE_DIR, 0, root_dot + 8, 0, 0, root_dot + 0x18, "..");
        for (size_t i = 0; i < s.files.size(); i++)
            entry(&dir[(i + 2) * 512], MODE_FILE, (u32)s.files[i].data.size(), now, firsts[i], 0, now, s.files[i].name);
        const u32 dir_first = write_new(dir);
        // its entry in the root, a cluster more for the root if needed, and the root's count
        if (slot / per >= rc.size()) {
            const u32 c = write_new(Bytes(cs, 0));
            set_fat(rc.back(), FAT_USED | c);
            rc.push_back(c);
        }
        entry(root_entry(slot, true), MODE_DIR, nent, now, dir_first, 0, now, s.dir);
        if (slot == count) put32(root_entry(0, true) + 4, count + 1);
    }

    void free_chain(u32 first)
    {
        if (first == FAT_END) return;
        for (u32 c : chain(first)) set_fat(c, 0x7fffffff);
    }

    // A folder of the root and its files gone, as the PS2's mcman deletes: the clusters free in the FAT, the entry kept with its
    // "exists" bit off (the next save takes it). False: there is no such folder. A folder in it (no save has one) is refused.
    bool remove_dir(const std::string &name)
    {
        const std::vector<u32> rc = chain(root);
        const u32 per = cs / 512;
        const u32 count = rd32(at(rc[0] + alloc_off, 4, false));
        for (u32 i = 2; i < count; i++) {
            if (i / per >= rc.size()) throw Bad("the card's root directory is damaged");
            u8 *e = at(rc[i / per] + alloc_off, i % per * 512, false);
            if (!(rd16(e) & DF_EXISTS) || !mode_dir(rd16(e)) || cstr(e + 0x40, 32) != name) continue;
            const u32 first = rd32(e + 0x10), n = rd32(e + 4);
            if (n > 4096) throw Bad("the save's folder on the card is damaged");
            const std::vector<u32> dc = chain(first);
            for (u32 j = 2; j < n; j++) {
                if (j / per >= dc.size()) throw Bad("the save's folder on the card is damaged");
                const u8 *f = at(dc[j / per] + alloc_off, j % per * 512, false);
                const u16 mode = rd16(f);
                if (!(mode & DF_EXISTS)) continue;
                if (mode & DF_DIR) throw Bad("the save's folder has a folder in it; it is left as it is");
                if (rd32(f + 4)) free_chain(rd32(f + 0x10));
            }
            free_chain(first);
            put16(at(rc[i / per] + alloc_off, i % per * 512, true), rd16(e) & ~DF_EXISTS);
            return true;
        }
        return false;
    }

    // every page written gets its ECC: 3 bytes a 128 bytes of the page, at the start of the 16 spare bytes
    void ecc()
    {
        if (raw == page) return;
        static u8 par[256], colmask[256];
        static bool made = false;
        if (!made) {
            const u8 cpm[7] = {0x55, 0x33, 0x0f, 0x00, 0xaa, 0xcc, 0xf0};
            for (int b = 0; b < 256; b++) {
                int x = b;
                x ^= x >> 4, x ^= x >> 2, x ^= x >> 1;
                par[b] = (u8)(x & 1);
            }
            for (int b = 0; b < 256; b++) {
                u8 m = 0;
                for (int i = 0; i < 7; i++) m |= (u8)(par[b & cpm[i]] << i);
                colmask[b] = m;
            }
            made = true;
        }
        for (size_t p = 0; p < dirty.size(); p++) {
            if (!dirty[p]) continue;
            u8 *pg = page_at((u32)p), *spare = pg + page;
            for (u32 k = 0; k < page / 128; k++) {
                u8 cp = 0x77, l0 = 0x7f, l1 = 0x7f;
                for (u32 i = 0; i < 128; i++) {
                    const u8 b = pg[k * 128 + i];
                    cp ^= colmask[b];
                    if (par[b]) l0 ^= (u8)~i, l1 ^= (u8)i;
                }
                spare[k * 3] = cp, spare[k * 3 + 1] = l0 & 0x7f, spare[k * 3 + 2] = l1;
            }
            memset(spare + page / 128 * 3, 0, raw - page - page / 128 * 3);
        }
    }
};

// a file written whole and new: the bytes to <path>.new, checked, then put in place; false with err
bool write_whole(const fs::path &path, const Bytes &data, bool replace, std::string &err)
{
    std::error_code ec;
    fs::path tmp = path;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char *)data.data(), (std::streamsize)data.size());
        if (!f.good()) {
            err = "cannot write " + u8str(tmp);
            fs::remove(tmp, ec);
            return false;
        }
    }
#ifdef _WIN32
    const bool ok = MoveFileExW(tmp.c_str(), path.c_str(), (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool ok = replace ? rename(tmp.c_str(), path.c_str()) == 0 : !fs::exists(path, ec) && rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!ok) {
        err = "cannot put " + u8str(path) + " in place" + (replace ? " (is it open in an emulator?)" : " (there is such a file)");
        fs::remove(tmp, ec);
    }
    return ok;
}

// the card's copy as it was, beside it: <card>.bak, or .bak2, .bak3... the first name that is free
fs::path backup_name(const fs::path &card)
{
    std::error_code ec;
    for (int i = 1;; i++) {
        fs::path b = card;
        b += i == 1 ? std::string(".bak") : ".bak" + std::to_string(i);
        if (!fs::exists(b, ec)) return b;
    }
}

// the folder's save on a card image: is it there, and when was it saved
bool card_has(const fs::path &card, const std::string &dir, int64_t *when)
{
    Bytes d;
    if (!read_file(card, d)) return false;
    try {
        for (const SaveDir &x : card_saves(d))
            if (x.name == dir) {
                if (when) *when = x.when;
                return true;
            }
    } catch (const Bad &) {
    }
    return false;
}

// a file or folder to the Recycle Bin, where it can be had back from
bool recycle(const fs::path &p, std::string &err)
{
    std::error_code ec;
    const fs::path full = fs::absolute(p, ec);
#ifdef _WIN32
    std::wstring from = full.wstring();
    from.push_back(L'\0');                 // the list ends with a second NUL
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted || fs::exists(full, ec)) {
        err = "cannot move " + u8str(full) + " to the Recycle Bin";
        return false;
    }
    return true;
#else
    if (fs::remove_all(full, ec) == 0 || ec) {
        err = "cannot delete " + u8str(full);
        return false;
    }
    return true;
#endif
}

// the image read, changed in memory, read back, then put in place with the copy as it was beside it
bool rewrite_card(const fs::path &card, const std::function<void(CardWriter &)> &change, const std::function<bool(const Bytes &)> &check,
                  std::string &done, std::string &err)
{
    CardWriter w;
    if (!read_file(card, w.d)) {
        err = "cannot read " + u8str(card);
        return false;
    }
    try {
        w.open();
        change(w);
        w.ecc();
        if (!check(w.d)) throw Bad("the card as written could not be read back as it should (nothing was changed)");
    } catch (const Bad &e) {
        err = e.what();
        return false;
    }
    std::error_code ec;
    const fs::path bak = backup_name(card);
    if (!fs::copy_file(card, bak, ec)) {
        err = "cannot make the backup " + u8str(bak) + " (nothing was changed)";
        return false;
    }
    if (!write_whole(card, w.d, true, err)) return false;
    done = "the card's copy as it was is " + u8str(bak.filename());
    return true;
}

bool export_to_card(const fs::path &card, const OutSave &s, bool overwrite, std::string &done, std::string &err)
{
    return rewrite_card(
        card,
        [&](CardWriter &w) {
            if (overwrite) w.remove_dir(s.dir);
            w.add(s);
        },
        [&](const Bytes &d) {
            for (const SaveDir &x : card_saves(d))
                if (x.name == s.dir)
                    for (const SaveFile &f : x.files)
                        if (f.name == s.dir && f.data == s.files[0].data) return true;
            return false;
        },
        done, err);
}

// ---- into a folder: a PCSX2 folder card, or anywhere the save's folder should go ---------------------------------
void set_mtime(const fs::path &p, int64_t t)
{
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const u64 ft = ((u64)t + 11644473600ull) * 10000000ull;
    FILETIME f = {(DWORD)ft, (DWORD)(ft >> 32)};
    SetFileTime(h, &f, nullptr, &f);
    CloseHandle(h);
#else
    struct utimbuf u = {(time_t)t, (time_t)t};
    utime(p.c_str(), &u);
#endif
}

bool export_to_folder(const fs::path &folder, const OutSave &s, bool overwrite, std::string &err)
{
    std::error_code ec;
    const fs::path dir = folder / s.dir;
    if (overwrite && fs::exists(dir, ec) && !recycle(dir, err)) return false;
    if (!fs::create_directory(dir, ec)) {
        err = ec ? "cannot make " + u8str(dir) + ": " + ec.message() : "there is a save of the game in " + u8str(dir) + " already";
        return false;
    }
    for (const SaveFile &f : s.files) {
        const fs::path p = dir / f.name;
        std::ofstream o(p, std::ios::binary);
        o.write((const char *)f.data.data(), (std::streamsize)f.data.size());
        if (!o.good()) {
            err = "cannot write " + u8str(p);
            o.close();
            for (const SaveFile &g : s.files) fs::remove(dir / g.name, ec);     // what this made, nothing else
            fs::remove(dir, ec);
            return false;
        }
        o.close();
        set_mtime(p, s.when);
    }
    return true;
}

// ---- save files ------------------------------------------------------------------------------------------
Bytes make_psu(const OutSave &s)
{
    u8 now[8];
    put_tod(now, s.when);
    Bytes out(3 * 512, 0);
    CardWriter::entry(&out[0], MODE_DIR, 2 + (u32)s.files.size(), now, 0, 0, now, s.dir);
    CardWriter::entry(&out[512], MODE_DIR, 0, now, 0, 0, now, ".");
    CardWriter::entry(&out[1024], MODE_DIR, 0, now, 0, 0, now, "..");
    for (const SaveFile &f : s.files) {
        const size_t at = out.size();
        out.resize(at + 512 + (f.data.size() + 1023) / 1024 * 1024, 0);
        CardWriter::entry(&out[at], MODE_FILE, (u32)f.data.size(), now, 0, 0, now, f.name);
        memcpy(&out[at + 512], f.data.data(), f.data.size());
    }
    return out;
}

u32 crc32(const u8 *p, size_t n, u32 crc = 0)
{
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// Okumura's LZARI.C encoder, the twin of Lzari above: the window's binary tree finds the longest match, the arithmetic coder writes
// the symbols and positions.
struct LzariEnc {
    enum { N = Lzari::N, F = Lzari::F, THRESHOLD = Lzari::THRESHOLD, N_CHAR = Lzari::N_CHAR, M = Lzari::M, NIL = N };
    static constexpr u64 Q1 = Lzari::Q1, Q2 = Lzari::Q2, Q3 = Lzari::Q3, Q4 = Lzari::Q4, MAX_CUM = Lzari::MAX_CUM;
    Bytes out;
    unsigned mask = 128, buffer = 0;
    u64 low = 0, high = Q4;
    int shifts = 0;
    int char_to_sym[N_CHAR], sym_to_char[N_CHAR + 1];
    unsigned sym_freq[N_CHAR + 1], sym_cum[N_CHAR + 1], position_cum[N + 1];
    u8 text_buf[N + F - 1];
    int match_position = 0, match_length = 0, lson[N + 1], rson[N + 257], dad[N + 1];

    void put_bit(int bit)
    {
        if (bit) buffer |= mask;
        if ((mask >>= 1) == 0) {
            out.push_back((u8)buffer);
            buffer = 0;
            mask = 128;
        }
    }
    void output(int bit)
    {
        put_bit(bit);
        for (; shifts > 0; shifts--) put_bit(!bit);
    }

    void start_model()
    {
        sym_cum[N_CHAR] = 0;
        for (int sym = N_CHAR; sym >= 1; sym--) {
            const int ch = sym - 1;
            char_to_sym[ch] = sym;
            sym_to_char[sym] = ch;
            sym_freq[sym] = 1;
            sym_cum[sym - 1] = sym_cum[sym] + sym_freq[sym];
        }
        sym_freq[0] = 0;
        position_cum[N] = 0;
        for (int i = N; i >= 1; i--) position_cum[i - 1] = position_cum[i] + 10000 / (i + 200);
    }

    void update_model(int sym)
    {
        if (sym_cum[0] >= MAX_CUM) {
            unsigned c = 0;
            for (int i = N_CHAR; i > 0; i--) {
                sym_cum[i] = c;
                c += (sym_freq[i] = (sym_freq[i] + 1) >> 1);
            }
            sym_cum[0] = c;
        }
        int i;
        for (i = sym; sym_freq[i] == sym_freq[i - 1]; i--) {
        }
        if (i < sym) {
            const int ch_i = sym_to_char[i], ch_sym = sym_to_char[sym];
            sym_to_char[i] = ch_sym;
            sym_to_char[sym] = ch_i;
            char_to_sym[ch_i] = sym;
            char_to_sym[ch_sym] = i;
        }
        sym_freq[i]++;
        while (--i >= 0) sym_cum[i]++;
    }

    void narrow(u64 hi_cum, u64 lo_cum, u64 total)
    {
        const u64 range = high - low;
        high = low + range * hi_cum / total;
        low += range * lo_cum / total;
        for (;;) {
            if (high <= Q2) {
                output(0);
            } else if (low >= Q2) {
                output(1);
                low -= Q2;
                high -= Q2;
            } else if (low >= Q1 && high <= Q3) {
                shifts++;
                low -= Q1;
                high -= Q1;
            } else {
                break;
            }
            low += low;
            high += high;
        }
    }
    void encode_char(int ch)
    {
        const int sym = char_to_sym[ch];
        narrow(sym_cum[sym - 1], sym_cum[sym], sym_cum[0]);
        update_model(sym);
    }
    void encode_position(int position) { narrow(position_cum[position], position_cum[position + 1], position_cum[0]); }

    void init_tree()
    {
        for (int i = N + 1; i <= N + 256; i++) rson[i] = NIL;
        for (int i = 0; i < N; i++) dad[i] = NIL;
    }
    void insert_node(int r)
    {
        int cmp = 1, p = N + 1 + text_buf[r];
        const u8 *key = &text_buf[r];
        rson[r] = lson[r] = NIL;
        match_length = 0;
        for (;;) {
            if (cmp >= 0) {
                if (rson[p] != NIL) p = rson[p];
                else {
                    rson[p] = r;
                    dad[r] = p;
                    return;
                }
            } else {
                if (lson[p] != NIL) p = lson[p];
                else {
                    lson[p] = r;
                    dad[r] = p;
                    return;
                }
            }
            int i;
            for (i = 1; i < F; i++)
                if ((cmp = key[i] - text_buf[p + i]) != 0) break;
            if (i > THRESHOLD) {
                if (i > match_length) {
                    match_position = (r - p) & (N - 1);
                    if ((match_length = i) >= F) break;
                } else if (i == match_length) {
                    const int temp = (r - p) & (N - 1);
                    if (temp < match_position) match_position = temp;
                }
            }
        }
        dad[r] = dad[p];
        lson[r] = lson[p];
        rson[r] = rson[p];
        dad[lson[p]] = r;
        dad[rson[p]] = r;
        if (rson[dad[p]] == p) rson[dad[p]] = r;
        else lson[dad[p]] = r;
        dad[p] = NIL;
    }
    void delete_node(int p)
    {
        int q;
        if (dad[p] == NIL) return;
        if (rson[p] == NIL) q = lson[p];
        else if (lson[p] == NIL) q = rson[p];
        else {
            q = lson[p];
            if (rson[q] != NIL) {
                do q = rson[q];
                while (rson[q] != NIL);
                rson[dad[q]] = lson[q];
                dad[lson[q]] = dad[q];
                lson[q] = lson[p];
                dad[lson[p]] = q;
            }
            rson[q] = rson[p];
            dad[rson[p]] = q;
        }
        dad[q] = dad[p];
        if (rson[dad[p]] == p) rson[dad[p]] = q;
        else lson[dad[p]] = q;
        dad[p] = NIL;
    }

    Bytes run(const Bytes &in)
    {
        size_t next = 0;
        auto getc_ = [&]() { return next < in.size() ? (int)in[next++] : -1; };
        start_model();
        init_tree();
        int s = 0, r = N - F, len, c, i;
        memset(text_buf, ' ', N - F);
        for (len = 0; len < F && (c = getc_()) != -1; len++) text_buf[r + len] = (u8)c;
        if (len == 0) return {};
        for (i = 1; i <= F; i++) insert_node(r - i);
        insert_node(r);
        do {
            if (match_length > len) match_length = len;
            if (match_length <= THRESHOLD) {
                match_length = 1;
                encode_char(text_buf[r]);
            } else {
                encode_char(255 - THRESHOLD + match_length);
                encode_position(match_position - 1);
            }
            const int last_match_length = match_length;
            for (i = 0; i < last_match_length && (c = getc_()) != -1; i++) {
                delete_node(s);
                text_buf[s] = (u8)c;
                if (s < F - 1) text_buf[s + N] = (u8)c;
                s = (s + 1) & (N - 1);
                r = (r + 1) & (N - 1);
                insert_node(r);
            }
            while (i++ < last_match_length) {
                delete_node(s);
                s = (s + 1) & (N - 1);
                r = (r + 1) & (N - 1);
                if (--len) insert_node(r);
            }
        } while (len > 0);
        shifts++;
        output(low < Q1 ? 0 : 1);
        for (i = 0; i < 7; i++) put_bit(0);
        return std::move(out);
    }
};

Bytes lzari_encode(const Bytes &in)
{
    auto z = std::make_unique<LzariEnc>();
    return z->run(in);
}

// Max Drive: the header, a CRC over it and the compressed data, then LZARI of (length, name, bytes) per file at 16n + 8
Bytes make_max(const OutSave &s)
{
    Bytes body;
    for (const SaveFile &f : s.files) {
        const size_t at = body.size();
        body.resize(at + 36, 0);
        put32(&body[at], (u32)f.data.size());
        memcpy(&body[at + 4], f.name.data(), std::min<size_t>(f.name.size(), 32));
        body.insert(body.end(), f.data.begin(), f.data.end());
        body.resize((body.size() + 8 + 15) / 16 * 16 - 8, 0);
    }
    const Bytes comp = lzari_encode(body);
    Bytes out(0x5c, 0);
    memcpy(&out[0], "Ps2PowerSave", 12);
    memcpy(&out[0x10], s.dir.data(), std::min<size_t>(s.dir.size(), 32));
    memcpy(&out[0x30], "NFS:HP2", 7);
    put32(&out[0x50], (u32)comp.size() + 4);
    put32(&out[0x54], (u32)s.files.size());
    put32(&out[0x58], (u32)body.size());
    put32(&out[0x0c], crc32(comp.data(), comp.size(), crc32(out.data(), out.size())));
    out.insert(out.end(), comp.begin(), comp.end());
    return out;
}

// zlib with stored blocks: every inflater reads it, and a save is small
Bytes zlib_stored(const Bytes &in)
{
    Bytes out = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, in.size() - pos);
        out.push_back(pos + n >= in.size() ? 1 : 0);
        out.push_back((u8)n), out.push_back((u8)(n >> 8)), out.push_back((u8)~n), out.push_back((u8)(~n >> 8));
        out.insert(out.end(), in.begin() + pos, in.begin() + pos + n);
        pos += n;
    } while (pos < in.size());
    u32 a = 1, b = 0;
    for (u8 c : in) a = (a + c) % 65521, b = (b + a) % 65521;
    const u32 adler = b << 16 | a;
    for (int k = 3; k >= 0; k--) out.push_back((u8)(adler >> (8 * k)));
    return out;
}

// CodeBreaker: the header as mymc+ reads it (the header 0x128 bytes long), the body RC4 over zlib
Bytes make_cbs(const OutSave &s)
{
    u8 now[8];
    put_tod(now, s.when);
    Bytes body;
    for (const SaveFile &f : s.files) {
        const size_t at = body.size();
        body.resize(at + 64, 0);
        memcpy(&body[at], now, 8);
        memcpy(&body[at + 8], now, 8);
        put32(&body[at + 16], (u32)f.data.size());
        put16(&body[at + 20], MODE_FILE);
        memcpy(&body[at + 32], f.name.data(), std::min<size_t>(f.name.size(), 32));
        body.insert(body.end(), f.data.begin(), f.data.end());
    }
    Bytes comp = zlib_stored(body);
    u8 st[256];
    memcpy(st, kCbsRc4, 256);
    unsigned j = 0;
    for (size_t k = 0; k < comp.size(); k++) {
        const unsigned i = (unsigned)((k + 1) & 255);
        j = (j + st[i]) & 255;
        std::swap(st[i], st[j]);
        comp[k] ^= st[(st[i] + st[j]) & 255];
    }
    const u32 hlen = 0x128;
    Bytes out(hlen, 0);
    memcpy(&out[0], "CFU\0", 4);
    put32(&out[4], 0x1f40);
    put32(&out[8], hlen);
    put32(&out[12], (u32)body.size());
    put32(&out[16], (u32)comp.size());
    memcpy(&out[20], s.dir.data(), std::min<size_t>(s.dir.size(), 32));
    memcpy(&out[52], now, 8);
    memcpy(&out[60], now, 8);
    put32(&out[76], MODE_DIR);
    memcpy(&out[92], "Need for Speed Hot Pursuit 2", 28);
    out.insert(out.end(), comp.begin(), comp.end());
    return out;
}

// Deleting a save found: on a card image, its folder and files (the card's copy as it was kept beside it); in a folder card, or a save's
// own folder, the folder to the Recycle Bin. A save file (.psu, .max, .cbs) is not deleted here.
bool delete_save(const Found &f, std::string &done, std::string &err)
{
    switch (f.src) {
    case SRC_CARD:
        return rewrite_card(
            f.source,
            [&](CardWriter &w) {
                if (!w.remove_dir(f.dir)) throw Bad("there is no " + f.dir + " on the card");
            },
            [&](const Bytes &d) {
                for (const SaveDir &x : card_saves(d))
                    if (x.name == f.dir) return false;
                return true;
            },
            done, err);
    case SRC_FOLDER_CARD:
        if (!recycle(f.source / from_u8(f.dir), err)) return false;
        done = "its folder is in the Recycle Bin";
        return true;
    case SRC_SAVE_FOLDER:
        if (!recycle(f.source, err)) return false;
        done = "its folder is in the Recycle Bin";
        return true;
    default:
        err = "a save file is not deleted here: it is the file " + u8str(f.source.filename()) + " itself";
        return false;
    }
}

enum ExportKind { EX_CARD, EX_FOLDER, EX_PSU, EX_MAX, EX_CBS, EX_NONE };

std::string lower_ext(const fs::path &p)
{
    std::string e = u8str(p.extension());
    for (char &c : e) c = (char)tolower((unsigned char)c);
    return e;
}

// what a target is: a folder, a save file to make (by its extension), or a card image there already
ExportKind export_kind(const fs::path &p)
{
    std::error_code ec;
    if (fs::is_directory(p, ec)) return EX_FOLDER;
    const std::string e = lower_ext(p);
    if (e == ".psu") return EX_PSU;
    if (e == ".max") return EX_MAX;
    if (e == ".cbs") return EX_CBS;
    Bytes head;
    if (fs::is_regular_file(p, ec)) {
        std::ifstream f(p, std::ios::binary);
        char m[28] = {};
        f.read(m, 28);
        if (f.gcount() == 28 && !memcmp(m, kCardMagic, 28)) return EX_CARD;
    }
    return EX_NONE;
}

// the export itself; done: what to tell besides "written", err: why not
bool export_save(const fs::path &game, const std::string &profile, const Disc &disc, const fs::path &target, bool replace_file, bool overwrite,
                 std::string &done, std::string &err)
{
    OutSave s;
    if (!make_out_save(game, profile, disc, s, err)) return false;
    std::error_code ec;
    switch (export_kind(target)) {
    case EX_CARD:
        return export_to_card(target, s, overwrite, done, err);
    case EX_FOLDER:
        return export_to_folder(target, s, overwrite, err);
    case EX_PSU:
    case EX_MAX:
    case EX_CBS: {
        if (!replace_file && fs::exists(target, ec)) {
            err = u8str(target) + " is there already";
            return false;
        }
        const ExportKind k = export_kind(target);
        const Bytes b = k == EX_PSU ? make_psu(s) : k == EX_MAX ? make_max(s) : make_cbs(s);
        if (!write_whole(target, b, replace_file, err)) return false;
        set_mtime(target, s.when);
        return true;
    }
    default:
        err = u8str(target) + " is neither a PS2 memory card image, a folder, nor a .psu, .max or .cbs file to make";
        return false;
    }
}

// ---- where to look by default ---------------------------------------------------------------------------
// PCSX2's card folders, all in them; this program's own folder, its card and save files only
std::vector<fs::path> default_sources()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(exe_dir(), ec))
        if (e.is_regular_file(ec) && card_like(e.path())) out.push_back(e.path());
    for (const fs::path &dir : pcsx2_card_folders())
        for (const auto &e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) || e.is_directory(ec)) out.push_back(e.path());
    return out;
}

void scan(const std::vector<fs::path> &sources, std::vector<Found> &found, std::vector<Note> &notes)
{
    found.clear();
    notes.clear();
    for (const fs::path &p : sources) look_in(p, found, notes);
}

const Found *pick(const std::vector<Found> &found, int n)
{
    for (const Found &f : found)
        if (f.problem.empty() && --n == 0) return &f;
    return nullptr;
}

// ---- the command line -----------------------------------------------------------------------------------
void show(const std::vector<Found> &found, const std::vector<Note> &notes)
{
    int n = 0;
    for (const Found &f : found)
        if (f.problem.empty()) printf("  %2d. %s, saved %s\n      from %s\n", ++n, f.what.c_str(), local_time(f.when).c_str(), f.where.c_str());
    bool header = false;
    for (const Found &f : found)
        if (!f.problem.empty()) {
            if (!header) printf("\n  Found but cannot be imported:\n");
            header = true;
            printf("    - %s from %s: %s\n", f.what.c_str(), f.where.c_str(), f.problem.c_str());
        }
    if (!notes.empty()) printf("\n  Also looked at:\n");
    for (const Note &x : notes) printf("    - %s: %s\n", x.where.c_str(), x.text.c_str());
    if (!n) printf("\n  No save to import.\n");
}

const char kVersion[] = "0.1";         // the program's own (savestransfer.rc), not the game's

void usage()
{
    printf("NFS Hot Pursuit 2 - PS2 Memory card game saves manager %s (Saves Manager)\n\n"
           "savestransfer [card or save files, folders...]       the window\n"
           "savestransfer [files...] --list [--game <folder>]    what is found\n"
           "savestransfer [files...] --import <n> --name <profile> [--game <folder>]\n"
           "savestransfer --export <profile> --to <card image | folder | file.psu/.max/.cbs> [--disc usa|europe|asia|korea] [--overwrite] [--game <folder>]\n"
           "savestransfer --delete <save's folder, e.g. BASLUS-20362HP2> --from <card image | folder card>\n\n"
           "Makes saves of the game from PS2 memory cards (.ps2/.mcd images, PCSX2 folder cards) and save files (.psu, .max, .cbs)\n"
           "new profiles of the port. Profiles there already are never changed.\n"
           "Saves from the discs: USA (SLUS-20362), Europe v1.00 and v2.00 (SLES-50731), Asia (SLPM-65181), Korea (SLPM-67527).\n",
           kVersion);
}

int run_cli(const std::vector<fs::path> &sources, fs::path game, bool list, int import_n, std::string name, const std::string &export_profile,
            const fs::path &export_to, const std::string &disc_key, bool overwrite, const std::string &delete_dir, const fs::path &delete_from)
{
    if (!delete_dir.empty()) {
        std::vector<Found> found;
        std::vector<Note> notes;
        if (!delete_from.empty()) look_in(delete_from, found, notes);
        for (const Found &f : found)
            if (f.dir == delete_dir) {
                std::string done, err;
                if (!delete_save(f, done, err)) {
                    printf("Not deleted: %s.\n", err.c_str());
                    return 1;
                }
                printf("Deleted %s from %s; %s.\n", delete_dir.c_str(), u8str(delete_from).c_str(), done.c_str());
                return 0;
            }
        printf("Not deleted: %s.\n", delete_from.empty() ? "name the card with --from" : ("no save in " + delete_dir + " there").c_str());
        return 1;
    }
    if (game.empty()) game = find_game();
    if (game.empty() || !is_game_folder(game)) {
        if (game.empty()) printf("The game's folder is not found: name it with --game.\n");
        else printf("%s is not the game's folder (it has no data\\SLUS_203.62).\n", u8str(game).c_str());
        return 1;
    }
    if (!export_profile.empty()) {
        const Disc *disc = nullptr;
        for (const Disc &d : kDiscs)
            if (disc_key == d.key) disc = &d;
        if (!disc) {
            printf("--disc is usa, europe, asia or korea.\n");
            return 1;
        }
        std::string done, err;
        if (export_to.empty() || !export_save(game, export_profile, *disc, export_to, false, overwrite, done, err)) {
            printf("Not exported: %s.\n", export_to.empty() ? "name where with --to" : err.c_str());
            return 1;
        }
        printf("Exported the profile %s for %s to %s%s%s.\n", export_profile.c_str(), disc->name, u8str(export_to).c_str(), done.empty() ? "" : "; ",
               done.c_str());
        return 0;
    }
    const fs::path saves = game / "saves";
    printf("Game: %s\n\n", u8str(game).c_str());
    std::vector<Found> found;
    std::vector<Note> notes;
    scan(sources.empty() ? default_sources() : sources, found, notes);
    show(found, notes);
    if (list) return 0;
    const Found *f = pick(found, import_n);
    if (!f) {
        printf("There is no save number %d.\n", import_n);
        return 1;
    }
    const std::string why = check_name(name, saves);
    if (!why.empty()) {
        printf("%s\n", why.c_str());
        return 1;
    }
    std::string err;
    if (!make_profile(saves, name, *f, err)) {
        printf("Not imported: %s.\n", err.c_str());
        return 1;
    }
    printf("Made the profile %s.\n", name.c_str());
    return 0;
}

#ifdef _WIN32
// ---- the window --------------------------------------------------------------------------------------
std::wstring widen(const std::string &s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? (size_t)n : 0);
    return w;
}

std::string narrow(const std::wstring &w)
{
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(len > 0 ? len : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring window_text(HWND h)
{
    std::wstring w((size_t)GetWindowTextLengthW(h) + 1, L'\0');
    w.resize((size_t)GetWindowTextW(h, &w[0], (int)w.size()));
    return w;
}

enum { ID_GAME_BROWSE = 101, ID_LIST, ID_ADD_FILES, ID_ADD_FOLDER, ID_REFRESH, ID_NAME, ID_IMPORT, ID_CLOSE, ID_PROFILES, ID_HELP, ID_ABOUT, ID_EXPORT, ID_DELETE };
enum { ST_PLAIN, ST_GOOD, ST_BAD };

struct Ui {
    HWND wnd = nullptr, icon = nullptr, title = nullptr, subtitle = nullptr, help = nullptr, about = nullptr, export_btn = nullptr, del = nullptr, game_label = nullptr, game_edit = nullptr, game_browse = nullptr,
         list_label = nullptr, list = nullptr, add_files = nullptr, add_folder = nullptr, refresh = nullptr, prof_label = nullptr,
         profiles = nullptr, name_label = nullptr, name = nullptr, import = nullptr, status = nullptr, close = nullptr;
    HFONT font = nullptr, heading = nullptr, sub = nullptr;
    int dpi = 96;
    fs::path game;
    std::vector<fs::path> added;            // files and folders the user gave: looked in again with the defaults
    std::vector<Found> found;
    std::vector<Note> notes;
    int status_kind = ST_PLAIN;
    int disc = 0;                       // the disc chosen last for an export
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int id = 0, DWORD ex = 0)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    return c;
}

void set_status(const std::string &text, int kind)
{
    g.status_kind = kind;
    SetWindowTextW(g.status, widen(text).c_str());
    InvalidateRect(g.status, nullptr, TRUE);
}

fs::path saves_dir() { return g.game / "saves"; }

bool game_ok() { return !g.game.empty() && is_game_folder(g.game); }

// the row selected: an index into found, or -1 (none, or a row of a note)
// rows: lParam >= 0 an index into found, <= -2 into notes (-2 - i)
int selected()
{
    const int i = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    if (i < 0) return -1;
    LVITEMW it = {};
    it.mask = LVIF_PARAM;
    it.iItem = i;
    ListView_GetItem(g.list, &it);
    return (int)it.lParam;
}

void update_import()
{
    const int s = selected();
    EnableWindow(g.del, s >= 0 && g.found[(size_t)s].src != SRC_FILE);
    EnableWindow(g.import, game_ok() && s >= 0 && g.found[(size_t)s].problem.empty() && !trim(narrow(window_text(g.name))).empty());
}

void update_export() { EnableWindow(g.export_btn, game_ok() && SendMessageW(g.profiles, LB_GETCURSEL, 0, 0) != LB_ERR); }

void fill_profiles()
{
    SendMessageW(g.profiles, LB_RESETCONTENT, 0, 0);
    if (g.export_btn) EnableWindow(g.export_btn, FALSE);
    if (!game_ok()) return;
    for (const std::string &n : profiles(saves_dir())) SendMessageW(g.profiles, LB_ADDSTRING, 0, (LPARAM)widen(n).c_str());
}

void fill_list()
{
    ListView_DeleteAllItems(g.list);
    int row = 0, first_ready = -1;
    auto put = [&](const std::string &a, const std::string &b, const std::string &c, const std::string &d, LPARAM param) {
        std::wstring wa = widen(a), wb = widen(b), wc = widen(c), wd = widen(d);
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = &wa[0];
        it.lParam = param;
        ListView_InsertItem(g.list, &it);
        ListView_SetItemText(g.list, row, 1, &wb[0]);
        ListView_SetItemText(g.list, row, 2, &wc[0]);
        ListView_SetItemText(g.list, row, 3, &wd[0]);
        row++;
    };
    // the ones to import first, then the others with their reason; files and cards without a save of the game are not rows
    for (size_t i = 0; i < g.found.size(); i++)
        if (g.found[i].problem.empty()) {
            if (first_ready < 0) first_ready = row;
            put(g.found[i].what, local_time(g.found[i].when), "Ready", g.found[i].short_where, (LPARAM)i);
        }
    for (size_t i = 0; i < g.found.size(); i++)
        if (!g.found[i].problem.empty()) put(g.found[i].what, local_time(g.found[i].when), g.found[i].problem, g.found[i].short_where, (LPARAM)i);
    if (first_ready >= 0) {
        ListView_SetItemState(g.list, first_ready, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, first_ready, FALSE);
    }
    int ready = 0;
    for (const Found &f : g.found) ready += f.problem.empty();
    char buf[200];
    if (ready) snprintf(buf, sizeof buf, "%d save%s to import. Select one, give the new profile a name and press Import.", ready, ready == 1 ? "" : "s");
    else snprintf(buf, sizeof buf, "No save to import yet. Add a memory card or a save file, or drop it on this window.");
    set_status(buf, ST_PLAIN);
    update_import();
}

void rescan()
{
    HCURSOR old = SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_WAIT));
    std::vector<fs::path> sources = default_sources();
    sources.insert(sources.end(), g.added.begin(), g.added.end());
    try {
        scan(sources, g.found, g.notes);
    } catch (const std::exception &e) {
        g.notes.push_back({"looking for saves", e.what()});
    }
    SetCursor(old);
    fill_list();
}

// Files and folders the user gives. One that is looked in anyway (a PCSX2 card) or was given before is not added twice; one without any
// save of the game is not kept at all: why is said once, in the status line.
void add_sources(const std::vector<fs::path> &paths)
{
    std::error_code ec;
    const std::vector<fs::path> defaults = default_sources();
    std::string why;
    for (const fs::path &p : paths) {
        bool seen = false;
        for (const std::vector<fs::path> *list : {&defaults, (const std::vector<fs::path> *)&g.added})
            for (const fs::path &q : *list) seen = seen || p == q || fs::equivalent(p, q, ec);
        if (seen) continue;
        std::vector<Found> found;
        std::vector<Note> notes;
        try {
            look_in(p, found, notes);
        } catch (const std::exception &e) {
            notes.push_back({u8str(p), e.what(), u8str(p.filename())});
        }
        if (!found.empty()) g.added.push_back(p);
        else if (why.empty()) why = (notes.empty() ? u8str(p.filename()) + ": no Hot Pursuit 2 save in it" : notes[0].short_where + ": " + notes[0].text) + ".";
    }
    rescan();
    if (!why.empty()) set_status(why, ST_BAD);
}

void set_game(const fs::path &p)
{
    g.game = p;
    SetWindowTextW(g.game_edit, game_ok() ? p.wstring().c_str() : L"Not found: choose the folder the game is installed in.");
    fill_profiles();
    update_import();
}

// Explorer's dialog: files (several) or a folder
std::vector<fs::path> choose(bool folders, bool many, const wchar_t *title)
{
    std::vector<fs::path> out;
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) | (many ? FOS_ALLOWMULTISELECT : 0));
    d->SetTitle(title);
    if (!folders) {
        COMDLG_FILTERSPEC types[] = {{L"Memory cards and save files", L"*.ps2;*.mcd;*.mc2;*.psu;*.max;*.cbs"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, types);
    }
    if (SUCCEEDED(d->Show(g.wnd))) {
        IShellItemArray *items = nullptr;
        if (SUCCEEDED(d->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem *item = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out.push_back(path);
                if (path) CoTaskMemFree(path);
                if (item) item->Release();
            }
            items->Release();
        }
    }
    d->Release();
    return out;
}

void do_import()
{
    const int s = selected();
    if (s < 0 || !game_ok() || !g.found[(size_t)s].problem.empty()) return;
    std::string name = narrow(window_text(g.name));
    const std::string why = check_name(name, saves_dir());
    if (!why.empty()) {
        set_status(why, ST_BAD);
        SetFocus(g.name);
        return;
    }
    std::string err;
    if (!make_profile(saves_dir(), name, g.found[(size_t)s], err)) {
        set_status("Not imported: " + err + ".", ST_BAD);
        return;
    }
    fill_profiles();
    const LRESULT at = SendMessageW(g.profiles, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)widen(name).c_str());
    if (at != LB_ERR) SendMessageW(g.profiles, LB_SETTOPINDEX, (WPARAM)at, 0);
    SetWindowTextW(g.name, L"");
    set_status("Made the profile " + name + ". Start the game and choose it on the profiles screen.", ST_GOOD);
}

// Positions for a client area w x h: the header, the game's folder, the saves (left) and the profiles (right), the buttons under the
// saves, the status, then the name and Import.
void layout(int w, int h)
{
    const int m = S(16), pw = S(180), gap = S(12), row = S(26);
    auto at = [](HWND c, int x, int y, int cw, int ch) { SetWindowPos(c, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE); };
    const int tw = w - 2 * m - S(60) - S(220);
    at(g.icon, m, m, S(48), S(48));
    at(g.title, m + S(60), m - S(3), tw, S(30));
    at(g.subtitle, m + S(60), m + S(26), tw, S(24));
    at(g.help, w - m - S(208), m, S(100), S(30));
    at(g.about, w - m - S(100), m, S(100), S(30));
    int y = m + S(66);
    at(g.game_label, m, y + S(4), S(90), S(20));
    at(g.game_edit, m + S(92), y, w - 2 * m - S(92) - S(108), S(24));
    at(g.game_browse, w - m - S(100), y - S(1), S(100), row);
    y += S(40);
    const int lw = w - 2 * m - pw - gap;
    at(g.list_label, m, y, lw, S(20));
    at(g.prof_label, w - m - pw, y, pw, S(20));
    y += S(22);
    const int bottom = S(8) + row + S(12) + S(20) + S(10) + S(28) + m;
    const int lh = std::max(S(80), h - y - bottom);
    at(g.list, m, y, lw, lh);
    at(g.profiles, w - m - pw, y, pw, lh);
    y += lh + S(8);
    at(g.export_btn, w - m - pw, y, pw, row);
    at(g.add_files, m, y, S(150), row);
    at(g.add_folder, m + S(158), y, S(120), row);
    at(g.refresh, m + S(286), y, S(110), row);
    at(g.del, m + S(406), y, S(150), row);
    y += row + S(12);
    at(g.status, m, y, w - 2 * m, S(20));
    y = h - m - S(28);
    at(g.name_label, m, y + S(5), S(122), S(20));
    at(g.name, m + S(124), y + S(2), S(180), S(24));
    at(g.import, m + S(314), y, S(170), S(28));
    at(g.close, w - m - S(100), y, S(100), S(28));
    // the columns share the list's width: the save, the time, the state, where it is from
    const int cw = lw - GetSystemMetrics(SM_CXVSCROLL) - S(4);
    const int c0 = cw * 30 / 100, c1 = cw * 17 / 100, c2 = cw * 23 / 100;
    ListView_SetColumnWidth(g.list, 0, c0);
    ListView_SetColumnWidth(g.list, 1, c1);
    ListView_SetColumnWidth(g.list, 2, c2);
    ListView_SetColumnWidth(g.list, 3, cw - c0 - c1 - c2);
    RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void show_help();
void show_about();
void do_export();
void do_delete();
HRESULT CALLBACK about_proc(HWND h, UINT n, WPARAM, LPARAM, LONG_PTR);

// ---- Export, in the window ---------------------------------------------------------------------------------
bool pcsx2_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        std::wstring n = pe.szExeFile;
        for (wchar_t &c : n) c = (wchar_t)towlower(c);
        found = n.find(L"pcsx2") != std::wstring::npos;
    }
    CloseHandle(snap);
    return found;
}

fs::path choose_save_file(const std::string &dir)
{
    fs::path out;
    IFileSaveDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    COMDLG_FILTERSPEC types[] = {{L"EMS / uLaunchELF save (*.psu)", L"*.psu"}, {L"Max Drive save (*.max)", L"*.max"}, {L"CodeBreaker save (*.cbs)", L"*.cbs"}};
    d->SetFileTypes(3, types);
    d->SetDefaultExtension(L"psu");
    d->SetFileName(widen(dir).c_str());
    d->SetTitle(L"Save the profile's save as");
    if (SUCCEEDED(d->Show(g.wnd))) {
        IShellItem *item = nullptr;
        PWSTR path = nullptr;
        UINT type = 1;
        d->GetFileTypeIndex(&type);
        if (SUCCEEDED(d->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = path;
            const std::string e = lower_ext(out);
            if (e != ".psu" && e != ".max" && e != ".cbs") out += type == 2 ? L".max" : type == 3 ? L".cbs" : L".psu";
        }
        if (path) CoTaskMemFree(path);
        if (item) item->Release();
    }
    d->Release();
    return out;
}

// a question in the window's style: a warning, one button that goes on, Cancel the default
bool ask_go(const std::wstring &title, const std::wstring &text, const wchar_t *go)
{
    TASKDIALOGCONFIG w = {};
    w.cbSize = sizeof w;
    w.hwndParent = g.wnd;
    w.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    w.pszWindowTitle = L"Saves Manager";
    w.pszMainIcon = TD_WARNING_ICON;
    w.pszMainInstruction = title.c_str();
    w.pszContent = text.c_str();
    const TASKDIALOG_BUTTON b[] = {{IDYES, go}};
    w.cButtons = 1;
    w.pButtons = b;
    w.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    w.nDefaultButton = IDCANCEL;
    if (getenv("SAVESTRANSFER_SHOT")) {             // drawn for checking, then cancelled
        w.dwFlags |= TDF_CALLBACK_TIMER;
        w.pfCallback = about_proc;
    }
    int pressed = 0;
    return SUCCEEDED(TaskDialogIndirect(&w, &pressed, nullptr, nullptr)) && pressed == IDYES;
}

const wchar_t kPcsx2Warning[] = L"\n\nPCSX2 is running. If this card is inserted in it, close PCSX2 (or eject the card) first: while it has the card it "
                                L"may write its own copy over the change.";

// Delete: the save selected, from its card (or folder card), after a question
void do_delete()
{
    const int s = selected();
    if (s < 0) return;
    const Found f = g.found[(size_t)s];
    if (f.src == SRC_FILE) return;
    std::wstring text = widen(f.what) + L", saved " + widen(local_time(f.when)) + L", in " + widen(f.short_where) + L".\n\n";
    if (f.src == SRC_CARD)
        text += L"It is taken off the card. The card's copy as it was is kept beside it (" + backup_name(f.source).filename().wstring() + L").";
    else
        text += L"Its folder goes to the Recycle Bin, where it can be had back from.";
    if (f.src == SRC_CARD && pcsx2_running()) text += kPcsx2Warning;
    if (!ask_go(L"Delete this save from the card?", text, L"Delete")) return;
    std::string done, err;
    if (!delete_save(f, done, err)) {
        set_status("Not deleted: " + err + ".", ST_BAD);
        return;
    }
    rescan();
    set_status("Deleted " + f.what + " from " + u8str(f.source.filename()) + "; " + done + ".", ST_GOOD);
}

// Export: the profile selected, which disc the save is for, where it goes (a card image, a folder card, a save file); then written.
void do_export()
{
    const LRESULT sel = SendMessageW(g.profiles, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !game_ok()) return;
    std::wstring wname((size_t)SendMessageW(g.profiles, LB_GETTEXTLEN, (WPARAM)sel, 0) + 1, L'\0');
    wname.resize((size_t)SendMessageW(g.profiles, LB_GETTEXT, (WPARAM)sel, (LPARAM)&wname[0]));
    const std::string name = narrow(wname);
    std::error_code ec;
    if (!fs::exists(saves_dir() / wname / L"save.dat", ec)) {
        set_status("The profile " + name + " has no save yet: play and save once, then export it.", ST_BAD);
        return;
    }
    const std::wstring title = L"Export the profile " + wname;
    TASKDIALOG_BUTTON radios[4];
    std::wstring rtext[4];
    for (int i = 0; i < 4; i++) {
        rtext[i] = L"For the disc " + widen(kDiscs[i].name) + L"  (folder " + widen(kDiscs[i].dir) + L")";
        radios[i] = {100 + i, rtext[i].c_str()};
    }
    const TASKDIALOG_BUTTON buttons[] = {
        {201, L"Into a memory card image\n.ps2 or .mcd of PCSX2, AetherSX2, NetherSX2. The card's copy as it was is kept beside it (.bak)."},
        {202, L"Into a PCSX2 folder card\nOr any folder: the save's folder is made in it."},
        {203, L"As a save file\n.psu (uLaunchELF, EMS), .max (Max Drive) or .cbs (CodeBreaker)."}};
    TASKDIALOGCONFIG c = {};
    c.cbSize = sizeof c;
    c.hwndParent = g.wnd;
    c.hInstance = GetModuleHandleW(nullptr);
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = L"Export - Saves Manager";
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = title.c_str();
    c.pszContent = L"The save goes on the card as the game itself saves it, with its icon. The profile's name stays in the port.\n"
                   L"A game reads its saves from the folder of its own disc: choose the disc the card is played with.";
    c.cRadioButtons = 4;
    c.pRadioButtons = radios;
    c.nDefaultRadioButton = 100 + g.disc;
    c.cButtons = 3;
    c.pButtons = buttons;
    if (getenv("SAVESTRANSFER_SHOT")) {
        c.dwFlags |= TDF_CALLBACK_TIMER;
        c.pfCallback = about_proc;
    }
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int button = 0, radio = 100;
    if (FAILED(TaskDialogIndirect(&c, &button, &radio, nullptr)) || button < 201) return;
    g.disc = std::min(3, std::max(0, radio - 100));
    const Disc &disc = kDiscs[g.disc];

    fs::path target;
    bool overwrite = false;
    if (button == 201) {
        const auto p = choose(false, false, L"The memory card image to write the save on");
        if (p.empty()) return;
        target = p[0];
        if (export_kind(target) != EX_CARD) {
            set_status(u8str(target.filename()) + " is not a PS2 memory card image (a formatted .ps2 or .mcd).", ST_BAD);
            return;
        }
        int64_t when = 0;
        const bool running = pcsx2_running();
        if (card_has(target, disc.dir, &when)) {
            if (!ask_go(L"The card has a save for this disc already",
                        L"In " + widen(disc.dir) + L", saved " + widen(local_time(when)) + L".\n\nWrite the profile " + wname +
                            L"'s save over it? The card's copy as it was is kept beside it (" + backup_name(target).filename().wstring() + L")." +
                            (running ? kPcsx2Warning : L""),
                        L"Write over it"))
                return;
            overwrite = true;
        } else if (running && !ask_go(L"PCSX2 is running", std::wstring(kPcsx2Warning + 2), L"Write anyway")) {
            return;
        }
    } else if (button == 202) {
        const auto p = choose(true, false, L"The folder card (or folder) to put the save's folder in");
        if (p.empty()) return;
        target = p[0];
        if (fs::exists(target / disc.dir, ec)) {
            if (!ask_go(L"There is a save for this disc there already",
                        L"The folder " + widen(disc.dir) + L" is there.\n\nWrite the profile " + wname +
                            L"'s save over it? The save there now goes to the Recycle Bin.",
                        L"Write over it"))
                return;
            overwrite = true;
        }
    } else {
        target = choose_save_file(disc.dir);
        if (target.empty()) return;
    }
    std::string done, err;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_WAIT));
    const bool ok = export_save(g.game, name, disc, target, button == 203, overwrite, done, err);
    SetCursor(old);
    if (!ok) {
        set_status("Not exported: " + err + ".", ST_BAD);
        return;
    }
    rescan();
    set_status("Exported " + name + " for " + disc.name + " to " + u8str(target.filename()) + (done.empty() ? "." : ". " + done + "."), ST_GOOD);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g.list) layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r = {0, 0, S(720), S(460)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        ((MINMAXINFO *)lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        COLORREF c = GetSysColor(COLOR_WINDOWTEXT);
        if ((HWND)lp == g.status && g.status_kind == ST_GOOD) c = RGB(0x1a, 0x7f, 0x37);
        else if ((HWND)lp == g.status && g.status_kind == ST_BAD) c = RGB(0xc4, 0x2b, 0x1c);
        SetTextColor(dc, c);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_GAME_BROWSE: {
            const auto p = choose(true, false, L"The folder the game is installed in (with speed.exe)");
            if (p.empty()) break;
            if (is_game_folder(p[0])) {
                set_game(p[0]);
                set_status("Game folder: " + u8str(p[0]), ST_PLAIN);
            } else {
                set_status(u8str(p[0]) + " is not the game's folder: it has no data\\SLUS_203.62.", ST_BAD);
            }
            break;
        }
        case ID_ADD_FILES: {
            const auto p = choose(false, true, L"Memory cards and save files");
            if (!p.empty()) add_sources(p);
            break;
        }
        case ID_ADD_FOLDER: {
            const auto p = choose(true, false, L"A folder card, or a save's folder");
            if (!p.empty()) add_sources(p);
            break;
        }
        case ID_REFRESH:
            rescan();
            fill_profiles();
            break;
        case ID_NAME:
            if (HIWORD(wp) == EN_CHANGE) update_import();
            break;
        case ID_PROFILES:
            if (HIWORD(wp) == LBN_SELCHANGE) update_export();
            else if (HIWORD(wp) == LBN_DBLCLK) do_export();
            break;
        case ID_EXPORT:
            do_export();
            break;
        case ID_DELETE:
            do_delete();
            break;
        case ID_IMPORT:
        case IDOK:
            if (IsWindowEnabled(g.import)) do_import();
            break;
        case ID_HELP:
            show_help();
            break;
        case ID_ABOUT:
            show_about();
            break;
        case ID_CLOSE:
        case IDCANCEL:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nm = (const NMHDR *)lp;
        if (nm->hwndFrom != g.list) break;
        if (nm->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *lv = (const NMLISTVIEW *)lp;
            if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
                const int s = selected();
                if (s >= 0 && !g.found[(size_t)s].problem.empty()) set_status("This one cannot be imported: " + g.found[(size_t)s].problem + ".", ST_BAD);
                else if (s >= 0) set_status("Give the new profile a name and press Import. Profiles there already are never changed.", ST_PLAIN);
            }
            update_import();
        } else if (nm->code == NM_DBLCLK) {
            if (IsWindowEnabled(g.import)) do_import();
            else SetFocus(g.name);
        } else if (nm->code == NM_CUSTOMDRAW) {
            // the rows that cannot be imported in grey
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const int i = (int)cd->nmcd.lItemlParam;
                if (i < 0 || !g.found[(size_t)i].problem.empty()) cd->clrText = GetSysColor(COLOR_GRAYTEXT);
                return CDRF_DODEFAULT;
            }
        } else if (nm->code == LVN_GETINFOTIPW) {
            // the whole of it: the state and where from, with the full path
            NMLVGETINFOTIPW *tip = (NMLVGETINFOTIPW *)lp;
            LVITEMW it = {};
            it.mask = LVIF_PARAM;
            it.iItem = tip->iItem;
            ListView_GetItem(g.list, &it);
            const int i = (int)it.lParam;
            std::string text;
            if (i >= 0) text = (g.found[(size_t)i].problem.empty() ? std::string("Ready to import") : g.found[(size_t)i].problem) + "\nFrom " + g.found[(size_t)i].where;
            else if (i <= -2 && (size_t)(-2 - i) < g.notes.size()) text = g.notes[(size_t)(-2 - i)].text + "\n" + g.notes[(size_t)(-2 - i)].where;
            const std::wstring w = widen(text);
            if (tip->pszText && tip->cchTextMax > 0) {
                wcsncpy(tip->pszText, w.c_str(), (size_t)tip->cchTextMax - 1);
                tip->pszText[tip->cchTextMax - 1] = 0;
            }
        }
        break;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        std::vector<fs::path> paths;
        const UINT n = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            std::wstring w((size_t)DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            w.resize(DragQueryFileW(drop, i, &w[0], (UINT)w.size()));
            paths.push_back(w);
        }
        DragFinish(drop);
        add_sources(paths);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// the window as drawn, to a 32-bit .bmp (SAVESTRANSFER_SHOT=<file>: the window is shown, drawn, written and closed; for checking it)
void save_shot(HWND wnd, const std::wstring &path)
{
    RECT r;
    GetWindowRect(wnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC wdc = GetWindowDC(wnd), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(wnd, mdc, 2);
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
    if (FILE *f = _wfopen(path.c_str(), L"wb")) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, 4, (size_t)w * h, f);
        fclose(f);
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(wnd, wdc);
}

// ---- Help and About -------------------------------------------------------------------------------------
const wchar_t kHelpText[] =
    L"Saves Manager takes your Need for Speed: Hot Pursuit 2 saves from PlayStation 2 memory cards and save files, and makes each one "
    L"a new profile of the PC port. The profiles there already are never changed or written over, and the cards and files it reads "
    L"are only read.\r\n"
    L"\r\n"
    L"SAVES FROM THE DISCS\r\n"
    L"    USA (SLUS-20362)\r\n"
    L"    Europe v1.00 and v2.00 (SLES-50731)\r\n"
    L"    Asia (SLPM-65181)\r\n"
    L"    Korea (SLPM-67527)\r\n"
    L"\r\n"
    L"WHAT IT READS\r\n"
    L"    Memory card images .ps2 and .mcd (PCSX2, AetherSX2, NetherSX2), with or without ECC\r\n"
    L"    PCSX2 folder memory cards, and a save's own folder (BASLUS-20362HP2 and its files)\r\n"
    L"    Save files .psu (uLaunchELF, EMS), .max (Max Drive), .cbs (CodeBreaker)\r\n"
    L"\r\n"
    L"HOW TO USE IT\r\n"
    L"1. Game folder. It is found by itself when this program is next to speed.exe, or where the port's setup installed the game. "
    L"If not, press Browse... and choose the folder with speed.exe.\r\n"
    L"\r\n"
    L"2. Saves. PCSX2's memory cards are looked in by themselves (Documents\\PCSX2\\memcards and the folder set in PCSX2). Other cards "
    L"and save files: Add cards or files..., Add a folder..., or drop them on the window. Refresh looks at all of them again.\r\n"
    L"\r\n"
    L"3. Choose a save marked Ready. Grey rows cannot be imported: the State column and the tip under the mouse say why (a save of a "
    L"pre-release version, a damaged file, a card without a Hot Pursuit 2 save).\r\n"
    L"\r\n"
    L"4. Type a name for the new profile, up to 15 letters as in the game, and press Import as new profile (or Enter). A name a "
    L"profile has already, in any case of its letters, is refused: choose another.\r\n"
    L"\r\n"
    L"5. Start the game and choose the profile on the profiles screen.\r\n"
    L"\r\n"
    L"BACK TO A MEMORY CARD\r\n"
    L"Select a profile in Profiles in the game and press Export to a card or file... Choose the disc the card is played with "
    L"(the game looks for its save in its own disc's folder), then where the save goes: into a memory card image (.ps2, .mcd), "
    L"into a PCSX2 folder card, or as a .psu, .max or .cbs file. The save is written as the game writes it, with its icon "
    L"(taken from the game's folder). Only the save goes: the profile's name stays in the port.\r\n"
    L"If the card has a save for that disc already, you are asked whether to write over it. Before a card is changed its copy as "
    L"it was is kept beside it (.bak). Close PCSX2, or eject the card, before writing on it.\r\n"
    L"\r\n"
    L"DELETING A SAVE FROM A CARD\r\n"
    L"Select the save in the list and press Delete from card..., then confirm. On a memory card image the save is taken off the card "
    L"and the card's copy as it was is kept beside it (.bak); on a PCSX2 folder card its folder goes to the Recycle Bin. Saves in "
    L".psu, .max and .cbs files are not deleted here: they are the files themselves.\r\n"
    L"\r\n"
    L"FROM A COMMAND LINE\r\n"
    L"    savestransfer [files] --list\r\n"
    L"    savestransfer [files] --import <n> --name <profile>\r\n"
    L"    savestransfer --export <profile> --to <card, folder or file> [--disc usa|europe|asia|korea] [--overwrite]\r\n"
    L"    savestransfer --delete <save's folder> --from <card or folder card>\r\n"
    L"    --game <folder> names the game's folder.\r\n";

LRESULT CALLBACK help_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        EnableWindow(g.wnd, TRUE);
        SetActiveWindow(g.wnd);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// the Help window over the main one, which waits for it
HWND make_help()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = help_proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"HP2SavesHelp";
        RegisterClassW(&wc);
        registered = true;
    }
    const int cw = S(620), ch = S(580), m = S(16);
    RECT r = {0, 0, cw, ch};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr;
    GetWindowRect(g.wnd, &pr);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"HP2SavesHelp", L"Help - Saves Manager", style,
                               (pr.left + pr.right - w) / 2, (pr.top + pr.bottom - h) / 2, w, h, g.wnd, nullptr, inst, nullptr);
    auto child = [&](const wchar_t *cls, const wchar_t *text, DWORD st, int x, int y, int cw2, int ch2, int id, HFONT f) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, x, y, cw2, ch2, wnd, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    HWND icon = child(L"STATIC", nullptr, SS_ICON | SS_REALSIZECONTROL, m, m, S(48), S(48), 0, g.font);
    SendMessageW(icon, STM_SETICON, (WPARAM)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0), 0);
    child(L"STATIC", L"NFS Hot Pursuit 2", SS_NOPREFIX, m + S(60), m - S(3), cw - 2 * m - S(60), S(30), 0, g.heading);
    child(L"STATIC", L"How to use the saves manager", SS_NOPREFIX, m + S(60), m + S(26), cw - 2 * m - S(60), S(24), 0, g.sub);
    HWND text = child(L"EDIT", kHelpText, ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_BORDER, m, m + S(64), cw - 2 * m, ch - 2 * m - S(64) - S(40), 0,
                      g.font);
    SendMessageW(text, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(8), S(8)));
    HWND ok = child(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, cw - m - S(100), ch - m - S(28), S(100), S(28), IDOK, g.font);
    ShowWindow(wnd, SW_SHOW);
    SetFocus(ok);
    return wnd;
}

void show_help()
{
    HWND wnd = make_help();
    EnableWindow(g.wnd, FALSE);
    MSG msg;
    while (IsWindow(wnd)) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (IsDialogMessageW(wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// About: the name, the version, who made it and when. About and Export drawn for checking: SAVESTRANSFER_SHOT with SAVESTRANSFER_SHOT_OF
// about or export (the dialog written to the picture, then closed)
HRESULT CALLBACK about_proc(HWND h, UINT n, WPARAM, LPARAM, LONG_PTR)
{
    static int ticks = 0;
    if (n == TDN_TIMER && getenv("SAVESTRANSFER_SHOT") && ++ticks == 3) {
        save_shot(h, widen(getenv("SAVESTRANSFER_SHOT")));
        SendMessageW(h, TDM_CLICK_BUTTON, IDCANCEL, 0);
    }
    return S_OK;
}

void show_about()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const std::wstring content = L"Version " + widen(kVersion) + L"\nMade by ZloiKILLER, 2026";
    TASKDIALOGCONFIG c = {};
    c.cbSize = sizeof c;
    c.hwndParent = g.wnd;
    c.hInstance = inst;
    c.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_CALLBACK_TIMER;
    c.pszWindowTitle = L"About - Saves Manager";
    c.hMainIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    c.pszMainInstruction = L"NFS Hot Pursuit 2\nPS2 Memory card game saves manager";
    c.pszContent = content.c_str();
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    c.pfCallback = about_proc;
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}


int run_window(const std::vector<fs::path> &sources, const fs::path &game)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW big = ncm.lfMessageFont;
    big.lfHeight = big.lfHeight * 16 / 9;
    big.lfWeight = FW_SEMIBOLD;
    g.heading = CreateFontIndirectW(&big);
    LOGFONTW mid = ncm.lfMessageFont;
    mid.lfHeight = mid.lfHeight * 4 / 3;
    g.sub = CreateFontIndirectW(&mid);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"HP2SavesTransfer";
    RegisterClassW(&wc);
    RECT r = {0, 0, S(860), S(580)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g.wnd = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Saves Manager",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww,
                            wh, nullptr, nullptr, inst, nullptr);

    g.icon = add(L"STATIC", nullptr, SS_ICON | SS_REALSIZECONTROL);
    SendMessageW(g.icon, STM_SETICON, (WPARAM)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0), 0);
    g.title = add(L"STATIC", L"NFS Hot Pursuit 2", SS_NOPREFIX);
    SendMessageW(g.title, WM_SETFONT, (WPARAM)g.heading, TRUE);
    g.subtitle = add(L"STATIC", L"PS2 Memory card game saves manager", SS_NOPREFIX);
    SendMessageW(g.subtitle, WM_SETFONT, (WPARAM)g.sub, TRUE);
    // Help and About with Windows' own icons for them; the discs, the formats and the guide are in Help
    g.help = add(L"BUTTON", L" Help", BS_PUSHBUTTON | WS_TABSTOP, ID_HELP);
    g.about = add(L"BUTTON", L" About", BS_PUSHBUTTON | WS_TABSTOP, ID_ABOUT);
    for (auto [b, id] : {std::pair<HWND, SHSTOCKICONID>{g.help, SIID_HELP}, {g.about, SIID_INFO}}) {
        SHSTOCKICONINFO si = {sizeof si};
        if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_SMALLICON, &si))) SendMessageW(b, BM_SETIMAGE, IMAGE_ICON, (LPARAM)si.hIcon);
    }
    g.game_label = add(L"STATIC", L"Game folder:", 0);
    g.game_edit = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY);
    g.game_browse = add(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_GAME_BROWSE);
    g.list_label = add(L"STATIC", L"Saves found (PCSX2's memory cards are looked in by themselves; drop other cards and save files on the window):", SS_NOPREFIX | SS_ENDELLIPSIS);
    g.list = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    const wchar_t *cols[] = {L"Save", L"Saved", L"State", L"From"};
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i];
        c.cx = S(100);
        ListView_InsertColumn(g.list, i, &c);
    }
    g.add_files = add(L"BUTTON", L"Add cards or files...", BS_PUSHBUTTON | WS_TABSTOP, ID_ADD_FILES);
    g.add_folder = add(L"BUTTON", L"Add a folder...", BS_PUSHBUTTON | WS_TABSTOP, ID_ADD_FOLDER);
    g.refresh = add(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, ID_REFRESH);
    g.del = add(L"BUTTON", L"Delete from card...", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_DELETE);
    g.prof_label = add(L"STATIC", L"Profiles in the game:", SS_NOPREFIX);
    g.profiles = add(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_TABSTOP, ID_PROFILES);
    g.export_btn = add(L"BUTTON", L"Export to a card or file...", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_EXPORT);
    g.status = add(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS);
    g.name_label = add(L"STATIC", L"New profile name:", 0);
    g.name = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_NAME);
    SendMessageW(g.name, EM_SETLIMITTEXT, 15, 0);
    SendMessageW(g.name, EM_SETCUEBANNER, TRUE, (LPARAM)L"up to 15 letters");
    g.import = add(L"BUTTON", L"Import as new profile", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_IMPORT);
    g.close = add(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, ID_CLOSE);

    RECT cr;
    GetClientRect(g.wnd, &cr);
    layout(cr.right, cr.bottom);
    set_game(game.empty() ? find_game() : game);
    rescan();
    if (!sources.empty()) add_sources(sources);
    if (!game_ok()) set_status("The game's folder is not found: press Browse... and choose the folder the game is installed in.", ST_BAD);
    if (const char *name = getenv("SAVESTRANSFER_NAME")) SetWindowTextW(g.name, widen(name).c_str());
    ShowWindow(g.wnd, SW_SHOW);
    UpdateWindow(g.wnd);

    MSG msg;
    if (const char *shot = getenv("SAVESTRANSFER_SHOT")) {
        for (int i = 0; i < 20; i++) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(20);
        }
        // SAVESTRANSFER_SHOT_SIZE=<w>x<h>: the window drawn first as it opens, then made that size (a smaller one: what a resize leaves)
        int sw = 0, sh = 0;
        if (const char *size = getenv("SAVESTRANSFER_SHOT_SIZE"); size && sscanf(size, "%dx%d", &sw, &sh) == 2) {
            SetWindowPos(g.wnd, nullptr, 0, 0, sw, sh, SWP_NOMOVE | SWP_NOZORDER);
            for (int i = 0; i < 20; i++) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                Sleep(20);
            }
        }
        // SAVESTRANSFER_SHOT_OF: help or about, else the main window
        const char *of = getenv("SAVESTRANSFER_SHOT_OF");
        if (of && !strcmp(of, "about")) {
            show_about();
        } else if (of && !strcmp(of, "delete")) {
            for (int i = 0; i < ListView_GetItemCount(g.list); i++) {
                ListView_SetItemState(g.list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                if (selected() >= 0 && g.found[(size_t)selected()].src == SRC_CARD) break;
            }
            do_delete();
        } else if (of && !strcmp(of, "export")) {
            // the first profile with a save
            const auto names = profiles(saves_dir());
            for (size_t i = 0; i < names.size(); i++) {
                std::error_code ec;
                if (fs::exists(saves_dir() / from_u8(names[i]) / "save.dat", ec)) {
                    SendMessageW(g.profiles, LB_SETCURSEL, i, 0);
                    break;
                }
            }
            do_export();
        } else if (of && !strcmp(of, "help")) {
            HWND hw = make_help();
            for (int i = 0; i < 20; i++) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                Sleep(20);
            }
            save_shot(hw, widen(shot));
            DestroyWindow(hw);
        } else {
            save_shot(g.wnd, widen(shot));
        }
        DestroyWindow(g.wnd);
        return 0;
    }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g.wnd, &msg)) continue;        // Tab between the controls, Enter on Import, Esc closes
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
#endif

int run(int argc, char **argv)
{
    std::vector<fs::path> sources;
    fs::path game;
    bool list = false, cli = false;
    int import_n = 0;
    std::string import_name, export_profile, disc_key = "usa", delete_dir;
    fs::path export_to, delete_from;
    bool overwrite = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if ((a == "--game" || a == "--import" || a == "--name" || a == "--export" || a == "--to" || a == "--disc" || a == "--delete" || a == "--from") && i + 1 >= argc) {
            usage();
            return 2;
        }
        if (a == "--game") game = from_u8(argv[++i]);
        else if (a == "--list") list = cli = true;
        else if (a == "--import") import_n = atoi(argv[++i]), cli = true;
        else if (a == "--name") import_name = argv[++i];
        else if (a == "--export") export_profile = argv[++i], cli = true;
        else if (a == "--to") export_to = from_u8(argv[++i]);
        else if (a == "--disc") disc_key = argv[++i];
        else if (a == "--overwrite") overwrite = true;
        else if (a == "--delete") delete_dir = argv[++i], cli = true;
        else if (a == "--from") delete_from = from_u8(argv[++i]);
        else if (a == "--help" || a == "-h" || a == "/?") {
            usage();
            return 0;
        } else sources.push_back(from_u8(a));
    }
    try {
#ifdef _WIN32
        if (!cli) return run_window(sources, game);
#endif
        return run_cli(sources, game, list, import_n, import_name, export_profile, export_to, disc_key, overwrite, delete_dir, delete_from);
    } catch (const std::exception &e) {
        printf("\nStopped: %s\n", e.what());
        return 1;
    }
}

}   // namespace

#ifdef _WIN32
// A window program: from a command line (--list, --import, --help) it writes to the console it was started from, or to the pipe it was given.
int wmain(int argc, wchar_t **wargv)
{
    bool cli = false;
    for (int i = 1; i < argc; i++)
        cli = cli || !wcscmp(wargv[i], L"--list") || !wcscmp(wargv[i], L"--import") || !wcscmp(wargv[i], L"--export") || !wcscmp(wargv[i], L"--delete") || !wcscmp(wargv[i], L"--help") || !wcscmp(wargv[i], L"-h") ||
              !wcscmp(wargv[i], L"/?");
    if (cli) {
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if ((!out || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            printf("\n");
        }
        SetConsoleOutputCP(CP_UTF8);
    }
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(narrow(wargv[i]));
    std::vector<char *> argv;
    for (std::string &s : args) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    return run(argc, argv.data());
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
