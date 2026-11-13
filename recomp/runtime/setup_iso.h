#pragma once
// What the first start reads a disc image with (setup.cpp, install.cpp) and what loads the game's program (ps2.cpp): SHA-1, the CRC-32 of zlib,
// ISO 9660 without mounting.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block[64];
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }
    void run()
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)block[i * 4] << 24 | block[i * 4 + 1] << 16 | block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void add(const void *p, size_t n)
    {
        const uint8_t *s = (const uint8_t *)p;
        total += n;
        while (n--) {
            block[used++] = *s++;
            if (used == 64) run(), used = 0;
        }
    }
    std::string hex()
    {
        uint64_t bits = total * 8;
        uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (used != 56) add(&zero, 1);
        for (int i = 7; i >= 0; i--) {
            uint8_t b = (uint8_t)(bits >> (i * 8));
            add(&b, 1);
        }
        char out[41];
        for (int i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

inline std::string sha1_hex(const void *p, size_t n)
{
    Sha1 s;
    s.add(p, n);
    return s.hex();
}

// The discs the port knows, by the program SYSTEM.CNF boots and its SHA-1. The European v1.00 disc has the very program of the USA one (and
// the same archive, but for the empty PAL.TAG that turns on the PAL video mode, left out by install.cpp): the code recompiled from the one
// runs the other. The European v2.00 disc's program differs in 22 instructions, the stack frames of the two reward screens' constructors
// (128 bytes more for a text); the recompiled code has those larger frames for every disc (tools/recomp/gen.py), and nothing else of the
// program is other, its data included, so the one recompiled code runs all three. Its archive is the same files built again (147 differ
// by a few bytes), less two of the demo's recordings, plus HOTCHUNK.TAG, which the program never asks for. Whatever the disc, the program
// goes to data\SLUS_203.62.
// The Asia and Korea discs (one disc under two numbers: the programs differ in 5 bytes, the archives in none) have the program linked
// again with 24 bytes less of data, one disc number and one memory card code where the USA program has two each, and so with every
// address after them 8 or 16 lower: the recompiled code cannot run on it. The installer turns it into the USA program by a patch the exe
// carries (tools/recomp/make_disc_patch.py, the resource named here) and checks the result by the USA program's SHA-1. Their archive is
// the USA one less two of the demo's recordings, 46 files differing by a few bytes.
struct KnownDisc {
    const char *elf_sha1, *boot, *name;
    const wchar_t *patch;       // the resource that makes the USA program of this one, null where it runs as it is
};
static const KnownDisc kKnownDiscs[] = {
    {"6397d4425d55056678ed5a405cc81542f7d4d2ee", "SLUS_203.62", "Need for Speed: Hot Pursuit 2 (USA), SLUS-20362", nullptr},
    {"6397d4425d55056678ed5a405cc81542f7d4d2ee", "SLES_507.31", "Need for Speed: Hot Pursuit 2 (Europe) v1.00, SLES-50731", nullptr},
    {"e8983ff7e3517fd6fce5c1ca3673b71619c777f5", "SLES_507.31", "Need for Speed: Hot Pursuit 2 (Europe) v2.00, SLES-50731", nullptr},
    {"ffcfeedaba5deeec3b80c9f700739c2e080343f5", "SLPM_651.81", "Need for Speed: Hot Pursuit 2 (Asia), SLPM-65181", L"PATCH_SLPM_65181"},
    {"548fbe75a6096231a72254aea539fd0bea4406dc", "SLPM_675.27", "Need for Speed: Hot Pursuit 2 (Korea), SLPM-67527", L"PATCH_SLPM_67527"},
};

inline uint32_t crc32_update(uint32_t crc, const void *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint8_t *s = (const uint8_t *)p;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *s++) & 255] ^ (crc >> 8);
    return ~crc;
}

#ifdef _WIN32
// ISO 9660, its files by path; 2048-byte sectors
struct Iso {
    static const uint32_t SECTOR = 2048;
    struct File {
        std::string path;       // /NFSHP2/ZZDATA.BIN, without ;1
        uint32_t lba = 0;
        uint64_t size = 0;
    };
    FILE *f = nullptr;
    std::vector<File> files;
    // The other source, the disc itself in a drive (or its image mounted as one): root is the drive, "E:", and the files are read as the
    // files Windows shows there, one open at a time.
    std::wstring root;
    FILE *cur = nullptr;
    const File *cur_of = nullptr;
    ~Iso()
    {
        if (f) fclose(f);
        if (cur) fclose(cur);
    }

    bool read(uint64_t at, void *dst, size_t n) { return _fseeki64(f, (int64_t)at, SEEK_SET) == 0 && fread(dst, 1, n, f) == n; }
    bool read_file(const File &x, uint64_t at, void *dst, size_t n)
    {
        if (root.empty()) return read((uint64_t)x.lba * SECTOR + at, dst, n);
        if (cur_of != &x) {
            if (cur) fclose(cur);
            cur = _wfopen((root + std::wstring(x.path.begin(), x.path.end())).c_str(), L"rb");
            cur_of = &x;
        }
        return cur && _fseeki64(cur, (int64_t)at, SEEK_SET) == 0 && fread(dst, 1, n, cur) == n;
    }

    // the files of the disc's root and of the game's two folders, with their sizes
    bool open_drive(const std::wstring &drive, std::string &why)
    {
        root = drive;
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        for (const wchar_t *dir : {L"", L"/NFSHP2", L"/NFSHP2/IRX253"}) {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(root + dir + L"/", ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string path;
                for (wchar_t ch : std::wstring(dir) + L"/" + it->path().filename().wstring()) path += (char)ch;
                files.push_back({path, 0, (uint64_t)it->file_size(ec)});
            }
        }
        if (files.empty()) return why = "The disc in the drive cannot be read.", false;
        return true;
    }

    void walk(uint32_t lba, uint32_t size, const std::string &dir, int depth)
    {
        if (depth > 8 || size > (16u << 20)) return;
        std::vector<uint8_t> d(size);
        if (!read((uint64_t)lba * SECTOR, d.data(), size)) return;
        for (uint32_t pos = 0; pos < size;) {
            uint8_t len = d[pos];
            if (!len) {                     // records never cross a sector
                pos = (pos / SECTOR + 1) * SECTOR;
                continue;
            }
            if (pos + 33 > size || pos + len > size) break;
            uint32_t ext, n;
            memcpy(&ext, &d[pos + 2], 4);
            memcpy(&n, &d[pos + 10], 4);
            uint8_t flags = d[pos + 25], nlen = d[pos + 32];
            std::string name((const char *)&d[pos + 33], nlen);
            pos += len;
            if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;     // . and ..
            size_t semi = name.find(';');
            if (semi != std::string::npos) name.resize(semi);
            if (flags & 2) walk(ext, n, dir + "/" + name, depth + 1);
            else files.push_back({dir + "/" + name, ext, n});
        }
    }

    bool open(const std::wstring &path, std::string &why)
    {
        f = _wfopen(path.c_str(), L"rb");
        if (!f) return why = "The file cannot be opened.", false;
        uint8_t pvd[SECTOR];
        for (uint32_t s = 16; s < 32; s++) {
            if (!read((uint64_t)s * SECTOR, pvd, SECTOR)) break;
            if (memcmp(pvd + 1, "CD001", 5) || pvd[0] == 255 || pvd[0] != 1) continue;
            uint32_t root, rsize;
            memcpy(&root, pvd + 156 + 2, 4);
            memcpy(&rsize, pvd + 156 + 10, 4);
            walk(root, rsize, "", 0);
            if (files.empty()) return why = "The disc image has no files.", false;
            return true;
        }
        return why = "This is not a disc image (no ISO 9660 volume).", false;
    }

    const File *find(const char *path) const
    {
        for (const File &x : files)
            if (!_stricmp(x.path.c_str(), path)) return &x;
        return nullptr;
    }

    // the program the disc boots: SYSTEM.CNF's BOOT2 = cdrom0:\SLES_507.31;1
    const File *boot(std::string &name)
    {
        const File *cnf = find("/SYSTEM.CNF");
        if (!cnf || cnf->size > 4096) return nullptr;
        std::string text((size_t)cnf->size, '\0');
        if (!read_file(*cnf, 0, &text[0], text.size())) return nullptr;
        size_t at = text.find("BOOT2"), slash = text.find('\\', at == std::string::npos ? 0 : at);
        if (at == std::string::npos || slash == std::string::npos) return nullptr;
        size_t end = text.find_first_of(";\r\n", slash);
        name = text.substr(slash + 1, end == std::string::npos ? std::string::npos : end - slash - 1);
        return find(("/" + name).c_str());
    }
};
#endif
