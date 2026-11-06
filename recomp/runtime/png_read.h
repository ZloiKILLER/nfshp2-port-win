// A PNG as RGBA8, with nothing else needed: for the texture replacements (native_tex.cpp). Every colour type, 8 and 16 bits and the paletted
// and grey depths below 8; not interlaced ones, which no one saves a texture as. 16 bits keep the high byte.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace png_read {

struct Bits {
    const uint8_t *p, *end;
    uint32_t acc = 0;
    int n = 0;
    bool bad = false;
    uint32_t get(int count)
    {
        while (n < count) {
            if (p >= end) {
                bad = true;
                return 0;
            }
            acc |= (uint32_t)*p++ << n;
            n += 8;
        }
        const uint32_t v = acc & ((1u << count) - 1);
        acc >>= count;
        n -= count;
        return count ? v : 0;
    }
};

// canonical Huffman: the short codes from a table, the rest a bit at a time by the counts of each length
struct Huff {
    enum { FAST = 10 };
    uint16_t count[16] = {}, symbol[320] = {};
    uint16_t fast[1 << FAST] = {};      // by the next bits: symbol << 4 | length, 0 for a longer code
    void build(const uint8_t *lengths, int n)
    {
        memset(count, 0, sizeof count);
        memset(fast, 0, sizeof fast);
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        count[0] = 0;
        uint16_t offs[16] = {};
        int next[16] = {}, code = 0;
        for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + count[i];
        for (int i = 1; i < 16; i++) {
            next[i] = code;
            code = (code + count[i]) << 1;
        }
        for (int i = 0; i < n; i++) {
            const int len = lengths[i];
            if (!len) continue;
            symbol[offs[len]++] = (uint16_t)i;
            const int c = next[len]++;
            if (len > FAST) continue;
            int rev = 0;                // the bits come low first
            for (int b = 0; b < len; b++) rev |= ((c >> b) & 1) << (len - 1 - b);
            for (int at = rev; at < 1 << FAST; at += 1 << len) fast[at] = (uint16_t)(i << 4 | len);
        }
    }
    int decode(Bits &b) const
    {
        while (b.n < FAST && b.p < b.end) {
            b.acc |= (uint32_t)*b.p++ << b.n;
            b.n += 8;
        }
        const uint16_t f = fast[b.acc & ((1u << FAST) - 1)];
        if (f && (f & 15) <= b.n) {
            b.acc >>= f & 15;
            b.n -= f & 15;
            return f >> 4;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= (int)b.get(1);
            if (b.bad) return -1;
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

inline bool inflate(const uint8_t *src, size_t size, std::vector<uint8_t> &out)
{
    static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    if (size < 2) return false;
    Bits b = {src + 2, src + size};     // past the zlib header
    bool last = false;
    while (!last) {
        last = b.get(1);
        const uint32_t type = b.get(2);
        if (b.bad || type == 3) return false;
        if (type == 0) {
            b.acc = 0;
            b.n = 0;
            if (b.end - b.p < 4) return false;
            const size_t len = b.p[0] | b.p[1] << 8;
            b.p += 4;
            if ((size_t)(b.end - b.p) < len) return false;
            out.insert(out.end(), b.p, b.p + len);
            b.p += len;
            continue;
        }
        Huff lit, dist;
        uint8_t lengths[320];
        if (type == 1) {
            for (int i = 0; i < 288; i++) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            lit.build(lengths, 288);
            for (int i = 0; i < 30; i++) lengths[i] = 5;
            dist.build(lengths, 30);
        } else {
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = b.get(5) + 257, ndist = b.get(5) + 1, ncode = b.get(4) + 4;
            memset(lengths, 0, sizeof lengths);
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)b.get(3);
            Huff codes;
            codes.build(lengths, 19);
            for (int i = 0; i < nlen + ndist;) {
                const int s = codes.decode(b);
                if (s < 0) return false;
                if (s < 16) {
                    lengths[i++] = (uint8_t)s;
                    continue;
                }
                int prev = 0, rep;
                if (s == 16) {
                    if (!i) return false;
                    prev = lengths[i - 1];
                    rep = 3 + b.get(2);
                } else {
                    rep = s == 17 ? 3 + b.get(3) : 11 + b.get(7);
                }
                if (i + rep > nlen + ndist) return false;
                while (rep--) lengths[i++] = (uint8_t)prev;
            }
            lit.build(lengths, nlen);
            dist.build(lengths + nlen, ndist);
        }
        for (;;) {
            const int s = lit.decode(b);
            if (s < 0) return false;
            if (s < 256) {
                out.push_back((uint8_t)s);
            } else if (s == 256) {
                break;
            } else {
                if (s > 285) return false;
                const int len = len_base[s - 257] + b.get(len_extra[s - 257]);
                const int d = dist.decode(b);
                if (d < 0 || d > 29) return false;
                const size_t back = dist_base[d] + b.get(dist_extra[d]);
                if (b.bad || back > out.size()) return false;
                size_t from = out.size() - back;
                for (int i = 0; i < len; i++) out.push_back(out[from++]);
            }
        }
    }
    return true;
}

inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// rgba: w * h * 4 bytes. On failure why is set.
inline bool load(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h, const char *&why)
{
    static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        why = "cannot open";
        return false;
    }
    std::vector<uint8_t> file;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + n);
    fclose(f);
    if (file.size() < 33 || memcmp(file.data(), magic, 8)) {
        why = "not a PNG";
        return false;
    }
    int depth = 0, type = 0;
    std::vector<uint8_t> idat, palette, trns;
    for (size_t p = 8; p + 12 <= file.size();) {
        const uint32_t len = be32(&file[p]);
        if (len > file.size() - p - 12) break;
        const uint8_t *tag = &file[p + 4], *body = &file[p + 8];
        if (!memcmp(tag, "IHDR", 4) && len >= 13) {
            w = (int)be32(body);
            h = (int)be32(body + 4);
            depth = body[8];
            type = body[9];
            if (body[12]) {
                why = "interlaced";
                return false;
            }
        } else if (!memcmp(tag, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!memcmp(tag, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!memcmp(tag, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        }
        p += 12 + (size_t)len;
    }
    const int channels = type == 0 || type == 3 ? 1 : type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (!channels || w <= 0 || h <= 0 || w > 16384 || h > 16384 || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) {
        why = "a kind of PNG not read here";
        return false;
    }
    const size_t bits = (size_t)channels * depth, stride = ((size_t)w * bits + 7) / 8, step = bits < 8 ? 1 : bits / 8;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * h);
    if (!inflate(idat.data(), idat.size(), raw) || raw.size() < (stride + 1) * h) {
        why = "broken data";
        return false;
    }
    for (int y = 0; y < h; y++) {
        uint8_t *row = &raw[(stride + 1) * y + 1];
        const uint8_t *up = y ? row - stride - 1 : nullptr;
        const int filter = row[-1];
        for (size_t x = 0; x < stride; x++) {
            const int a = x >= step ? row[x - step] : 0, b = up ? up[x] : 0, c = up && x >= step ? up[x - step] : 0;
            int add = 0;
            if (filter == 1) add = a;
            else if (filter == 2) add = b;
            else if (filter == 3) add = (a + b) / 2;
            else if (filter == 4) {
                const int pp = a + b - c, pa = pp > a ? pp - a : a - pp, pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp;
                add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            row[x] = (uint8_t)(row[x] + add);
        }
    }
    rgba.resize((size_t)w * h * 4);
    const int scale = depth < 8 ? 255 / ((1 << depth) - 1) : 1;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = &raw[(stride + 1) * y + 1];
        uint8_t *out = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++, out += 4) {
            if (depth < 8) {
                const size_t at = (size_t)x * depth;
                const int v = (row[at / 8] >> (8 - depth - (at & 7))) & ((1 << depth) - 1);
                if (type == 3) {
                    out[0] = (size_t)v * 3 + 2 < palette.size() ? palette[v * 3] : 0;
                    out[1] = (size_t)v * 3 + 2 < palette.size() ? palette[v * 3 + 1] : 0;
                    out[2] = (size_t)v * 3 + 2 < palette.size() ? palette[v * 3 + 2] : 0;
                    out[3] = (size_t)v < trns.size() ? trns[v] : 255;
                } else {
                    out[0] = out[1] = out[2] = (uint8_t)(v * scale);
                    out[3] = 255;
                }
                continue;
            }
            const uint8_t *s = row + (size_t)x * step;
            const int d = depth / 8;
            if (type == 3) {
                const int v = s[0];
                out[0] = (size_t)v * 3 + 2 < palette.size() ? palette[v * 3] : 0;
                out[1] = (size_t)v * 3 + 2 < palette.size() ? palette[v * 3 + 1] : 0;
                out[2] = (size_t)v * 3 + 2 < palette.size() ? palette[v * 3 + 2] : 0;
                out[3] = (size_t)v < trns.size() ? trns[v] : 255;
            } else if (type == 0 || type == 4) {
                out[0] = out[1] = out[2] = s[0];
                out[3] = type == 4 ? s[d] : 255;
            } else {
                out[0] = s[0];
                out[1] = s[d];
                out[2] = s[d * 2];
                out[3] = type == 6 ? s[d * 3] : 255;
            }
        }
    }
    return true;
}

}  // namespace png_read
