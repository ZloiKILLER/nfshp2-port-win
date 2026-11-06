#pragma once
// What the first start reads a disc image with (setup.cpp, install.cpp) and what loads the game's program (ps2.cpp): SHA-1, the CRC-32 of zlib,
// ISO 9660 without mounting.
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// the game's program on the discs the port knows
struct KnownDisc {
    const char *elf_sha1, *name;
};
static const KnownDisc kKnownDiscs[] = {
    {"6397d4425d55056678ed5a405cc81542f7d4d2ee", "Need for Speed: Hot Pursuit 2 (USA), SLUS-20362"},
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
    ~Iso() { if (f) fclose(f); }

    bool read(uint64_t at, void *dst, size_t n) { return _fseeki64(f, (int64_t)at, SEEK_SET) == 0 && fread(dst, 1, n, f) == n; }
    bool read_file(const File &x, uint64_t at, void *dst, size_t n) { return read((uint64_t)x.lba * SECTOR + at, dst, n); }

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
};
#endif
