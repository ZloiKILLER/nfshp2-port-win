// The native renderer, the game's side: the draw lists and the strips (native.h). Addresses and formats
// were checked on the retail: handoff.md, "Нативный рендер: списки Ecstasy".
#include "native.h"

#include "hw.h"
#include "pcconfig.h"
#include "pcmenu.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>

extern FILE *g_hle_log;

// ---- matrices --------------------------------------------------------------------------------
static void mul(float out[16], const float a[16], const float b[16])      // columns: out = a * b
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

static void read_quads(u32 addr, float *out, int quads) { memcpy(out, host(addr), (size_t)quads * 16); }

// vuiLoadMatricies: the pack's first four quads times the local-world matrix, then each column scaled by
// the fifth quad and offset by the sixth times its w. x/w and y/w are GS pixels, z/w is the Z buffer's.
// Here they are moved to Vulkan's clip space around the target's middle (the sixth quad's x and y).
// The GS clamps Z to its buffer's top. The mirror's and the envmap views' buffers are 16 bits, MaxZ 65535: their depth runs over all of
// Vulkan's range, and the depth clamp does the rest. The 2D they overlay sits at 65535 and has to pass over what clamped there.
static float depth_scale(u32 target)
{
    float top;
    memcpy(&top, host(target + 0x74), 4);
    return top > 0 && top < 1e6f ? 16777216.0f / top * 1.000001f : 1.0f;
}

static void clip_matrix(float out[16], u32 pack, u32 local, int half_w, int half_h, float z_scale)
{
    float p[16], l[16], m[16], q[8];
    read_quads(pack, p, 4);
    read_quads(pack + 64, q, 2);
    read_quads(local, l, 4);
    mul(m, p, l);
    for (int c = 0; c < 4; c++) {
        float *col = m + c * 4, w = col[3];
        // X/W is a GS pixel (the centre, q[4]/q[7], is 2048): out of the middle by half the target.
        // Z/W is in [2, 4): the VU sends the float's bits to XYZF2, whose Z is above the low four: Z = (Z/W - 2) * 2^22 / 16, larger is
        // nearer. Vulkan's depth is that Z / 2^24, which keeps the fog's Z >> 8 and the 2D's and the particles' Z on the same scale.
        float cw = q[3] * col[3] + q[7] * w;
        out[c * 4 + 0] = q[0] * col[0] / (float)half_w;
        out[c * 4 + 1] = q[1] * col[1] / (float)half_h;
        out[c * 4 + 2] = (q[2] * col[2] + q[6] * w - 2 * cw) / 64 * z_scale;
        out[c * 4 + 3] = cw;
    }
}

// ---- the strip -------------------------------------------------------------------------------
// VIF codes run into a copy of VU memory, as solid-geometry.md has it; addresses 0 header, 1 ADC bits,
// 2.. XYZ (four vertices to three quads), 32.. ST, 52.. colour, 92.. normals.
static const int VU_QUADS = 128;

static s32 sext(u32 v, int bits) { return (s32)(v << (32 - bits)) >> (32 - bits); }

// the vectors a strip is made of, without looking at the format for each element
template <typename T> static inline void unpack_vectors(const u8 *p, u32 n, u32 elems, u32 (*vu)[4])
{
    for (u32 i = 0; i < n; i++)
        for (u32 e = 0; e < elems; e++) {
            T v;
            memcpy(&v, p + (i * elems + e) * sizeof(T), sizeof(T));
            vu[i][e] = (u32)(s32)v;
        }
}

static bool unpack_strip(u32 addr, u32 qwords, u32 vu[VU_QUADS][4])
{
    const u8 *p = host(addr), *end = p + (size_t)qwords * 16;
    while (p + 4 <= end) {
        u32 code;
        memcpy(&code, p, 4);
        p += 4;
        u32 cmd = code >> 24 & 0x7f, num = code >> 16 & 0xff, imm = code & 0xffff;
        if ((cmd & 0x60) != 0x60) {
            if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) break;  // MSCAL, MSCALF, MSCNT: the packet's end
            continue;
        }
        u32 vn = cmd >> 2 & 3, vl = cmd & 3, usn = imm >> 14 & 1, at = imm & 0x3ff, n = num ? num : 256;
        u32 elems = vn + 1, bits = vl == 3 ? 16 : 32 >> vl;
        size_t bytes = (((size_t)n * elems * bits + 31) / 32) * 4;
        if (vl == 3) bytes = ((size_t)n * 2 + 3) / 4 * 4;
        if (p + bytes > end) return false;
        if (vn != 0 && vl != 3) {
            if (at + n > VU_QUADS) return false;
            if (vn == 3 && vl == 0) memcpy(vu[at], p, (size_t)n * 16);
            else if (vl == 0) unpack_vectors<u32>(p, n, elems, vu + at);
            else if (vl == 1 && usn) unpack_vectors<u16>(p, n, elems, vu + at);
            else if (vl == 1) unpack_vectors<s16>(p, n, elems, vu + at);
            else if (usn) unpack_vectors<u8>(p, n, elems, vu + at);
            else unpack_vectors<s8>(p, n, elems, vu + at);
            p += bytes;
            continue;
        }
        for (u32 i = 0; i < n; i++, at++) {
            if (at >= VU_QUADS) return false;
            if (vn == 0 && vl != 3)         // S-32, S-16, S-8 fill the quad
                vu[at][1] = vu[at][2] = vu[at][3] = 0;
            for (u32 e = 0; e < elems; e++) {
                if (vl == 3) {              // V4-5: RGBA5551, one halfword a quad
                    u16 h;
                    memcpy(&h, p + i * 2, 2);
                    u32 sh = e * 5, v = e == 3 ? (h >> 15) << 7 : ((h >> sh) & 31) << 3;
                    vu[at][e] = v;
                } else if (bits == 32) {
                    memcpy(&vu[at][e], p + (i * elems + e) * 4, 4);
                } else if (bits == 16) {
                    u16 h;
                    memcpy(&h, p + (i * elems + e) * 2, 2);
                    vu[at][e] = usn ? h : (u32)sext(h, 16);
                } else {
                    u8 b = p[i * elems + e];
                    vu[at][e] = usn ? b : (u32)sext(b, 8);
                }
            }
            if (vn == 0 && vl != 3) vu[at][1] = vu[at][2] = vu[at][3] = vu[at][0];
        }
        p += bytes;
    }
    return true;
}

// vuiReferenceStripper: the XYZ quads' ADC bit is bit 15 of VU1's random register, stepped before each vertex. The strip's words at
// address 1 seed it: x and again for the 1st of 12 calls of four vertices, z for the 5th, w for the 9th. A strip runs the last NumLoops + 1
// of the calls, and the first it makes covers seven vertices, as the program looks ahead.
static void strip_adc(const u32 vu[VU_QUADS][4], u32 loops, u32 n, bool out[30])
{
    u32 r = vu[1][0] & 0x7fffff, v = 0;
    for (int call = 11 - (int)loops, first = 1; call <= 11; call++, first = 0) {
        if (call == 0) r = vu[1][0] & 0x7fffff;
        if (call == 4) r = vu[1][2] & 0x7fffff;
        if (call == 8) r = vu[1][3] & 0x7fffff;
        for (int i = first ? 7 : 4; i > 0 && v < n; i--) {
            r = (r << 1 | ((r >> 22 ^ r >> 4) & 1)) & 0x7fffff;
            out[v++] = (r >> 15 & 1) != 0;
        }
    }
    while (v < n) out[v++] = true;
}

static NativeFrame g_frame;
static u32 g_light_garbage;              // directions out of range cleared (HP2_NATIVELOG)
static u32 g_mode;                     // the texture's, from EDMA_SETTARGET
static u32 g_texture;                  // the TextureInfo of AttachTextureEnvDirect
static u32 g_frame_no;

// The state of a draw from the texture and the mode. AlphaBits is ALPHA_1's low byte: A, B, C, D by two bits,
// (A - B) C + D with 0 Cs, 1 Cd, 2 zero for A, B, D and 0 As, 1 Ad, 2 FIX for C.
static u32 g_alpha_seen[256];

static u32 g_target_ptr;                 // the eRenderTarget of the last SETTARGET
static u32 g_light_ctx, g_light_mat;
static int g_light = -1;
static u32 g_strip_flags;
static u32 g_draw_target;

static u32 g_rvm_texture, g_rvm_target;  // the mirror's TextureInfo and target
static u32 g_prev_texture, g_prev_target;  // the screen buffer that holds the last frame

// ALPHA_1's low byte: A, B, C, D by two bits, (A - B) C + D with 0 Cs, 1 Cd, 2 zero for A, B, D and 0 As, 1 Ad, 2 FIX for C.
static void set_blend(NativeDraw &d, u32 bits, u32 fix)
{
    u32 a = bits & 3, b = bits >> 2 & 3, cc = bits >> 4 & 3, dd = bits >> 6 & 3;
    d.blend = NB_OPAQUE;
    if (dd == 1) {
        if (a == b) d.blend = NB_KEEP;
        else if (a == 0 && b == 1) d.blend = cc == 1 ? NB_LERP_DEST : NB_ALPHA;
        else if (a == 0 && b == 2) d.blend = cc == 1 ? NB_DEST : NB_ADD;
        else if (a == 2 && b == 0) d.blend = NB_SUB;
    }
    d.blend_fix = cc == 2 ? fix / 128.0f : -1;
}

static void set_draw_state(NativeDraw &d)
{
    d.rt = -1;
    d.snap_x = d.snap_y = 0;
    d.view[0] = d.view[1] = 0;
    d.view[2] = 512;
    d.view[3] = 448;
    if (g_target_ptr) {
        d.view[0] = (float)(s32)LW(g_target_ptr + 0x14);
        d.view[1] = (float)(s32)LW(g_target_ptr + 0x18);
        d.view[2] = (float)(s32)LW(g_target_ptr + 0x1c);
        d.view[3] = (float)(s32)LW(g_target_ptr + 0x20);
    }
    d.tfx = 0;
    d.lod_l = 0;
    d.depth_test = !g_target_ptr || LW(g_target_ptr + 0x44) != 1;
    d.target = (u8)g_draw_target;
    d.flags = (u16)g_strip_flags;
    d.light = g_light;
    d.depth_write = (g_mode & 2) != 0;
    d.alpha_test = !(g_mode & 1);
    d.blend = NB_OPAQUE;
    d.blend_fix = -1;
    d.scroll_s = d.scroll_t = 0;
    d.tex = nullptr;
    if (!g_texture) return;
    d.tfx = LB(g_texture + 0x4e);
    d.lod_l = LB(g_texture + 0x71);
    u32 bits = LB(g_texture + 118);
    g_alpha_seen[bits]++;
    set_blend(d, bits, LB(g_texture + 119));
    // TextureInfo::GetScroll: time, ScrollType +0x79, speeds +0x7c, +0x80, step +0x84
    if (LB(g_texture + 121)) {
        u32 tw = LW(g_pc_ctx->r[28].w[0] - 19576), scale = LW(g_pc_ctx->r[28].w[0] - 32704);
        float t, speed[2], step;
        memcpy(&t, &scale, 4);
        t *= (float)(s32)tw;
        memcpy(&speed[0], host(g_texture + 124), 8);
        memcpy(&step, host(g_texture + 132), 4);
        if (LB(g_texture + 121) == 2 && step != 0) t = (float)(int)(t / step);
        float us = t * speed[0], ut = t * speed[1];
        d.scroll_s = us - (float)(int)us;
        d.scroll_t = ut - (float)(int)ut;
    }
    if (g_texture == g_rvm_texture) d.rt = NATIVE_RT_MIRROR;
    else if (g_texture == g_prev_texture) d.rt = NATIVE_RT_PREVIOUS;
    else d.tex = native_texture(g_texture, g_frame_no);
}
static float g_clip[16];
static bool g_clip_new = true;
static struct { u64 strips, bad, verts, tris, lists; } g_stat;

// what the strip's lighter reads, once per pair in a frame
static void update_light()
{
    if (!g_light_ctx || !g_light_mat) {
        g_light = -1;
        return;
    }
    static u32 last_ctx, last_mat, last_frame = ~0u;
    if (g_light >= 0 && last_frame == g_frame_no && last_ctx == g_light_ctx && last_mat == g_light_mat) return;
    last_ctx = g_light_ctx;
    last_mat = g_light_mat;
    last_frame = g_frame_no;
    NativeLight l;
    memcpy(l.q[0], host(g_light_ctx), 18 * 16);
    memcpy(l.q[18], host(g_light_mat), 4 * 16);
    g_frame.lights.push_back(l);
    g_light = (int)g_frame.lights.size() - 1;
}

// The PC's own: what the PS2's picture has and nobody wants. HP2_NATIVEEXACT=1 leaves them, to hold the picture against the old path's.
static bool pc_fixes()
{
    static const bool on = !getenv("HP2_NATIVEEXACT");
    return on;
}

// A font's cells are cut a column too wide: a glyph's quad can take in the edge of its neighbour in the atlas, a dark line beside the
// character. Such a column shows by a gap of nothing between it and the glyph. It goes, with half the next for the filter's sake.
static void trim_glyph(u32 first)
{
    if (g_frame.vertices.size() != first + 6 || g_frame.draws.empty()) return;
    const NativeTexture *t = g_frame.draws.back().tex;
    if (!t || t->font_alpha.empty()) return;
    NativeVertex *v = &g_frame.vertices[first];
    float x0 = v[0].x, x1 = v[0].x, s0 = v[0].s, s1 = v[0].s, t0 = v[0].t, t1 = v[0].t;
    for (int i = 1; i < 6; i++) {
        x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
        s0 = std::min(s0, v[i].s), s1 = std::max(s1, v[i].s);
        t0 = std::min(t0, v[i].t), t1 = std::max(t1, v[i].t);
    }
    int y0 = (int)(t0 * t->h), y1 = std::min((int)(t1 * t->h), t->h - 1);
    int c0 = (int)(s0 * t->w), c1 = (int)(s1 * t->w - 0.01f);
    if (c1 - c0 < 3 || x1 <= x0 || c0 < 0 || c1 >= t->w || y0 < 0) return;
    auto alone = [&](int edge, int inner) {
        bool any = false;
        for (int y = y0; y <= y1; y++) {
            const u8 *row = &t->font_alpha[(size_t)y * t->w];
            if (row[edge] > 8) {
                if (row[inner] > 8) return false;
                any = true;
            }
        }
        return any;
    };
    float per_texel = (x1 - x0) / ((s1 - s0) * t->w), cut = 1.5f;
    if (alone(c1, c1 - 1))
        for (int i = 0; i < 6; i++)
            if (v[i].x == x1) v[i].x -= cut * per_texel, v[i].s -= cut / t->w;
    if (alone(c0, c0 + 1))
        for (int i = 0; i < 6; i++)
            if (v[i].x == x0) v[i].x += cut * per_texel, v[i].s += cut / t->w;
}

// The mirror's frame starts a column in on its target and its texture's last column is not clean, so the target's first two columns are a
// line beside the frame. They are the right edge of the mirror on the screen, past the frame: the quad stops short of them.
static void trim_mirror(u32 first)
{
    if (g_frame.vertices.size() != first + 6) return;
    NativeVertex *v = &g_frame.vertices[first];
    float x0 = v[0].x, x1 = v[0].x;
    for (int i = 1; i < 6; i++) x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
    float at_right = 0, at_left = 0;
    for (int i = 0; i < 6; i++) (v[i].x == x1 ? at_right : at_left) = v[i].s;
    if (x1 <= x0 || at_right == at_left) return;
    float part = 2.0f / (float)LW(g_rvm_target + 0x1c);     // of the target
    float width = x1 - x0;
    for (int i = 0; i < 6; i++)
        if (v[i].x == x1) {
            v[i].s += (at_left - at_right) * part;
            v[i].x -= width * part;
        }
}

// ---- the wide race ---------------------------------------------------------------------------
// The 3D is widened by the game's own view (HOOK_CreateViewMatricies): the target keeps its 512 pixels across, and the picture
// shows them wider, so what the game places in those pixels in 2D would come out stretched. So 2D is narrowed again, as a whole:
// the HUD's panels keep to the screen's left and right edges, the rest to its middle, nothing changes shape.
float native_narrow()
{
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    if (exact) return 1.0f;
    const float f = [] {
        return g_cfg.aspect == 2 ? (4.0f / 3.0f) / (16.0f / 9.0f) : g_cfg.aspect == 1 ? (4.0f / 3.0f) / (16.0f / 10.0f) : 1.0f;
    }();
    return f;
}

// The packets of a Flailer object and its pieces: a text goes as one thing, not glyph by glyph. One object can hold pieces far apart
// that share a texture (the last lights under the mirror and the panel by the right edge), and each of them goes on its own.
struct StripSpan {
    float x0, x1;
};
static std::unordered_map<u32, std::vector<StripSpan>> g_packet_spans;
static std::vector<StripSpan> g_strip_spans;

static void unstretch_2d(u32 first, u32 data, float ox)
{
    float f = native_narrow();
    u32 id = LW(g_target_ptr);
    if (id == 7 || id >= 8) return;
    size_t n = g_frame.vertices.size() - first;
    if (!n) return;
    auto spans = g_packet_spans.find(data);
    bool layer = id == 0 || id == 3;
    u32 radial = LW(g_pc_ctx->r[28].w[0] - 25124);          // pTextureInfoRadialBlur
    if (!layer && g_texture && (g_texture == radial || g_texture == g_prev_texture)) return;    // the whole view, cut in four
    bool edges = pc_hud_up();
    size_t group = n % 6 ? n : 6;
    for (size_t at = 0; at < n; at += group) {
        NativeVertex *v = &g_frame.vertices[first + at];
        float x0 = v[0].x, x1 = v[0].x;
        for (size_t i = 1; i < group; i++) x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
        x0 -= ox, x1 -= ox;
        // across the whole screen: a plain colour (a fade, a letterbox, a line) stays across the wide screen too; a picture keeps its shape
        if (x1 - x0 >= 400 && (!g_texture || LH(g_texture + 0x24) <= 64)) continue;
        float mid = (x0 + x1) / 2;
        float anchor = mid;                                 // in the view (the sun, a spark): narrowed where it is
        if (layer) {
            float whole = mid;
            if (spans != g_packet_spans.end())
                for (const StripSpan &s : spans->second)
                    if (mid >= s.x0 && mid <= s.x1) whole = (s.x0 + s.x1) / 2;
            anchor = !edges ? 256 : whole < 150 ? 0 : whole > 352 ? 512 : 256;  // the mirror and its lights are 160..351
        }
        for (size_t i = 0; i < group; i++) v[i].x = ox + anchor + (v[i].x - ox - anchor) * f;
    }
}

static s8 lod_k(u32 v)
{
    s32 k = sext(v, 12);
    return (s8)(k > 127 ? 127 : k < -127 ? -127 : k);       // past 7.9 levels is the last level anyway
}

// ---- the GPU's vertex cache: a strip's triangles are a function of its packet's bytes alone (unpack_strip reads nothing else), so a packet
// seen before is drawn from what the GPU already has, by indices; the bus carries 4 bytes a vertex instead of 32. Keyed by a 128-bit hash of
// the bytes. Emptied when it nears full, at a frame's start, and when a frame did not reach the GPU. HP2_NOVCACHE: none. HP2_VCACHECHECK:
// every hit made again and compared.
struct StripKey {
    u64 a, b;
    bool operator==(const StripKey &o) const { return a == o.a && b == o.b; }
};
struct StripKeyHash {
    size_t operator()(const StripKey &k) const { return (size_t)(k.a ^ k.b * 0x9E3779B97F4A7C15ull); }
};
struct CachedStrip {
    u32 base, count, n;
};
static std::unordered_map<StripKey, CachedStrip, StripKeyHash> g_strip_cache;
static std::unordered_map<StripKey, std::vector<NativeVertex>, StripKeyHash> g_strip_check;
static u32 g_cache_top, g_cache_cap;
static u64 g_cache_hits, g_cache_misses, g_cache_flushes, g_cache_check_bad;
static const bool g_cache_off = getenv("HP2_NOVCACHE") != nullptr, g_cache_checking = getenv("HP2_VCACHECHECK") != nullptr;

static StripKey strip_key(u32 data, u32 qwords)
{
    const u8 *p = host(data);
    const size_t n = (size_t)qwords * 16;
    u64 a = 0x9E3779B97F4A7C15ull ^ n, b = 0xC2B2AE3D27D4EB4Full + n;
    for (size_t i = 0; i < n; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        a = (a ^ w) * 0x9E3779B97F4A7C15ull;
        a ^= a >> 29;
        b = (b + w) * 0xBF58476D1CE4E5B9ull;
        b ^= b >> 31;
    }
    return {a, b};
}

static void cache_flush()
{
    g_strip_cache.clear();
    g_strip_check.clear();
    g_cache_top = 0;
    g_cache_flushes++;
}

static bool dumping();
static void add_strip(u32 data, u32 qwords, u32 flags)
{
    const bool caching = g_cache_cap && !g_cache_off;
    StripKey key = {};
    const CachedStrip *hit = nullptr;
    if (caching) {
        key = strip_key(data, qwords);
        auto found = g_strip_cache.find(key);
        if (found != g_strip_cache.end()) hit = &found->second;
    }
    NativeVertex tri[84];                   // 28 triangles at most
    u32 tri_n = 0, n = 0;
    if (!hit || g_cache_checking) {
    static thread_local u32 vu[VU_QUADS][4];
    memset(vu, 0, sizeof vu);
    if (!unpack_strip(data, qwords, vu)) {
        g_stat.bad++;
        return;
    }
    n = vu[0][0] & 0xff;
    const u32 loops = vu[0][1] & 0xff;
    if (n < 3 || n > 30) {
        g_stat.bad++;
        return;
    }
    NativeVertex in[30];
    s8 k = lod_k(vu[1][1]);                 // VU1's "lq.y vf01y,1(vi12)", stored into TEX1's upper word
    for (u32 v = 0; v < n; v++) {
        u32 g = 2 + 3 * (v / 4), l = v % 4;
        memcpy(&in[v].x, &vu[g][l], 4);
        memcpy(&in[v].y, &vu[g + 1][l], 4);
        memcpy(&in[v].z, &vu[g + 2][l], 4);
        in[v].r = (u8)vu[52 + v][0];
        in[v].g = (u8)vu[52 + v][1];
        in[v].b = (u8)vu[52 + v][2];
        in[v].a = (u8)vu[52 + v][3];
        u32 ng = 92 + 3 * (v / 4);
        in[v].nx = (s8)vu[ng][l];
        in[v].ny = (s8)vu[ng + 1][l];
        in[v].nz = (s8)vu[ng + 2][l];
        in[v].k = k;
        in[v].w = 1;
        memcpy(&in[v].s, &vu[32 + (v >> 1)][v & 1 ? 3 : 0], 4);
        memcpy(&in[v].t, &vu[32 + (v >> 1)][v & 1 ? 2 : 1], 4);
    }
    bool adc[30];
    strip_adc(vu, loops, n, adc);
    u32 restart = 0;
    for (u32 v = 2; v < n; v++) {
        if (adc[v]) {
            restart = v;
            continue;
        }
        bool odd = (v - restart) & 1;
        tri[tri_n++] = in[v - 2];
        tri[tri_n++] = in[odd ? v : v - 1];
        tri[tri_n++] = in[odd ? v - 1 : v];
    }
    }
    if (hit && g_cache_checking) {
        const std::vector<NativeVertex> &was = g_strip_check[key];
        if (was.size() != tri_n || (tri_n && memcmp(was.data(), tri, tri_n * sizeof(NativeVertex)))) {
            if (g_cache_check_bad++ < 10)
                fprintf(g_hle_log, "native: vcachecheck: a strip at %08x (%u qwords) is not what the cache holds for its bytes\n", data, qwords);
        }
    }
    update_light();
    const NativeDraw *prev = g_frame.draws.empty() ? nullptr : &g_frame.draws.back();
    if (g_clip_new || !prev || !prev->indexed || prev->flags != (u16)flags || prev->light != g_light || prev->target != (u8)g_draw_target) {
        g_strip_flags = flags;
        NativeDraw d;
        memcpy(d.clip, g_clip, sizeof d.clip);
        d.first = (u32)g_frame.indices.size();
        d.count = 0;
        d.indexed = true;
        set_draw_state(d);
        if (dumping())
            fprintf(g_hle_log, "dump draw %zu target %u tex %s flags %x blend %d fix %g tfx %d atest %d zw %d light %d mode %u a %d\n", g_frame.draws.size(),
                    g_draw_target, g_texture ? (const char *)host(g_texture + 8) : "-", flags, d.blend, d.blend_fix, d.tfx, d.alpha_test, d.depth_write,
                    g_light, g_mode, tri_n ? tri[0].a : -1);
        d.snap_x = (float)(LW(g_target_ptr + 0x1c) / 2);
        d.snap_y = (float)(LW(g_target_ptr + 0x20) / 2);
        g_frame.draws.push_back(d);
        g_clip_new = false;
    }
    NativeDraw &d = g_frame.draws.back();
    u32 base;
    if (hit) {
        base = hit->base;
        tri_n = hit->count;
        n = hit->n;
        g_cache_hits++;
    } else if (caching && g_cache_top + tri_n <= g_cache_cap) {
        // new to the cache: uploaded with this frame, a run with the strip before it if they follow each other
        base = g_cache_top;
        g_cache_top += tri_n;
        if (!g_frame.fresh_runs.empty() && g_frame.fresh_runs.back().dst + g_frame.fresh_runs.back().count == base)
            g_frame.fresh_runs.back().count += tri_n;
        else
            g_frame.fresh_runs.push_back({base, (u32)g_frame.fresh.size(), tri_n});
        g_frame.fresh.insert(g_frame.fresh.end(), tri, tri + tri_n);
        g_strip_cache[key] = {base, tri_n, n};
        if (g_cache_checking) g_strip_check[key].assign(tri, tri + tri_n);
        g_cache_misses++;
    } else {
        // no cache, or no room in it this frame: the frame's own vertices
        base = g_frame.dyn_base + (u32)g_frame.vertices.size();
        g_frame.vertices.insert(g_frame.vertices.end(), tri, tri + tri_n);
    }
    for (u32 i = 0; i < tri_n; i++) g_frame.indices.push_back(base + i);
    d.count += tri_n;
    g_stat.tris += tri_n / 3;
    g_stat.strips++;
    g_stat.verts += n;
}

// ---- the lists -------------------------------------------------------------------------------
enum { EDMA_STRIP = 0, EDMA_SETTARGET = 1, EDMA_LIGHTMATERIAL = 2, EDMA_LIGHTCONTEXT = 3, EDMA_DIRECT = 4, EDMA_POLY3D = 5,
       EDMA_SET_MATRIX_PACK = 9, EDMA_SET_LOCAL_WORLD = 12 };

static u32 g_target, g_pack, g_local;
static u32 g_nodes_seen[16], g_prims_seen[8];
static struct { u64 strips, direct; u32 pack, target; } g_by_target[16];

// the main view: anything else (env map views, shadows, the mirror) comes with later steps
static const u32 MAIN_TARGET = 1;           // TARGET_PLAYER1
static bool main_target()
{
    if (!g_target) return false;
    u32 id = LW(g_target);
    // the 2D layer, the player's view with its halves, its mirror, the six envmap views
    return id <= 4 || id == 6 || id == 7 || (id >= 8 && id <= 13);
}

// ---- GIF packets: what ePoly::Render, eAttachDirectData and the 2D layer send -----------------
// PACKED tags with the registers the game uses, screen space. Depth is the Z buffer's over 2^24, as a strip's.
// XYOFFSET comes out of the target's register block (DrawContextData, A+D pairs).
static void target_offset(u32 target, u32 mode, float &ox, float &oy)
{
    ox = 2048 - (float)LW(target + 0x1c) / 2;
    oy = 2048 - (float)LW(target + 0x20) / 2;
    u32 block = LW(target + 0x7c + (mode & 3) * 4);
    if (!block) return;
    for (u32 q = 1; q < 14; q++)
        if (LB(block + q * 16 + 8) == 0x18) {
            u32 v = LW(block + q * 16);
            ox = (v & 0xffff) / 16.0f;
            oy = LH(block + q * 16 + 4) / 16.0f;
            return;
        }
}

// HP2_NATIVEDUMP=<frame>: that frame's 2D packets and POLY3D, for what does not line up with the old path
static bool dumping()
{
    static const int at = getenv("HP2_NATIVEDUMP") ? atoi(getenv("HP2_NATIVEDUMP")) : -1;
    return at >= 0 && (int)g_frame_no == at;
}

static void add_direct(u32 data, u32 qwords)
{
    if (!g_target) return;
    // The stones from under the wheels, dark blots on the dust behind the car. The player does not see them on the main view: they fly backwards.
    if (pc_fixes() && LW(g_target) == 7 && g_texture && !strncmp((const char *)host(g_texture + 8), "XETT_GRAVEL", 11)) return;
    if (dumping()) {
        fprintf(g_hle_log, "dump DIRECT target %u mode %u tex %s %dx%d tw %d th %d ai %02x semi %d qw %u:", LW(g_target), g_mode, g_texture ? (const char *)host(g_texture + 8) : "-",
                g_texture ? LH(g_texture + 0x24) : 0, g_texture ? LH(g_texture + 0x26) : 0, g_texture ? LB(g_texture + 0x48) : 0, g_texture ? LB(g_texture + 0x49) : 0,
                g_texture ? LB(g_texture + 118) : 0, g_texture ? (s8)LB(g_texture + 0x4f) : 0, qwords);
        for (u32 i = 0; i < qwords * 4 && i < 60; i++) fprintf(g_hle_log, " %08x", LW(data + i * 4));
        fprintf(g_hle_log, "\n");
    }
    float ox, oy, w = (float)LW(g_target + 0x1c), h = (float)LW(g_target + 0x20);
    target_offset(g_target, g_mode, ox, oy);
    const float clip[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, depth_scale(g_target), 0, -1 - 2 * ox / w, -1 - 2 * oy / h, 0, 1};
    const u8 *p = host(data), *end = p + (size_t)qwords * 16;
    u32 first_vertex = (u32)g_frame.vertices.size();
    u32 prim = 0, alpha = g_texture ? LB(g_texture + 118) : 0, alpha_fix = g_texture ? LB(g_texture + 119) : 0x80;
    NativeVertex cur = {}, ring[3];
    cur.r = cur.g = cur.b = cur.a = 0x80;
    cur.w = 1;
    if (g_texture) cur.k = lod_k(LH(g_texture + 0x72));
    float q = 1;
    int kept = 0;
    bool state_new = true;
    // a render target's picture here is the target alone; its TextureInfo may be larger (the screen buffers are 512 x 512)
    float scale_s = 1, scale_t = 1;
    u32 of = g_texture == g_rvm_texture ? g_rvm_target : g_texture == g_prev_texture ? g_prev_target : 0;
    if (g_texture && of) {
        scale_s = (float)(1 << LB(g_texture + 0x48)) / (float)LW(of + 0x1c);
        scale_t = (float)(1 << LB(g_texture + 0x49)) / (float)LW(of + 0x20);
    }

    auto emit = [&](NativeVertex a, NativeVertex b, const NativeVertex &c) {
        if (!(prim & 8)) {                  // IIP off: flat, the colour of the vertex that kicked
            a.r = b.r = c.r, a.g = b.g = c.g, a.b = b.b = c.b, a.a = b.a = c.a;
        }
        if (state_new || g_frame.draws.empty()) {
            NativeDraw d;
            memcpy(d.clip, clip, sizeof clip);
            d.first = (u32)g_frame.vertices.size();
            d.count = 0;
            g_strip_flags = 0;
            u32 save_tex = g_texture;
            if (!(prim & 0x10)) g_texture = 0;      // TME off
            set_draw_state(d);
            g_texture = save_tex;
            d.light = -1;
            d.blend = NB_OPAQUE;
            d.blend_fix = -1;
            if (prim & 0x40) set_blend(d, alpha, alpha_fix);
            g_frame.draws.push_back(d);
            g_by_target[g_draw_target & 15].direct++;
            g_by_target[g_draw_target & 15].target = g_target;
            state_new = false;
        }
        g_frame.vertices.push_back(a);
        g_frame.vertices.push_back(b);
        g_frame.vertices.push_back(c);
        g_frame.draws.back().count += 3;
    };
    auto kick = [&](const NativeVertex &v) {
        g_prims_seen[prim & 7]++;
        switch (prim & 7) {
        case 3:                             // triangles
            ring[kept++] = v;
            if (kept == 3) {
                emit(ring[0], ring[1], ring[2]);
                kept = 0;
            }
            break;
        case 4:                             // strip
            if (kept < 2) {
                ring[kept++] = v;
                break;
            }
            emit(ring[0], ring[1], v);
            ring[0] = ring[1];
            ring[1] = v;
            break;
        case 5:                             // fan
            if (kept < 2) {
                ring[kept++] = v;
                break;
            }
            emit(ring[0], ring[1], v);
            ring[1] = v;
            break;
        case 6: {                           // sprite: two corners
            if (kept == 0) {
                ring[kept++] = v;
                break;
            }
            // the old path takes a sprite's pixel at its corner, a triangle's at its middle: half a GS pixel, for any picture size
            NativeVertex a = ring[0], b = v;
            a.x += 0.5f, a.y += 0.5f, b.x += 0.5f, b.y += 0.5f;
            a.z = b.z;
            a.w = b.w = 1;                  // and its ST is straight
            NativeVertex c = a, d = a;
            c.x = b.x;
            c.s = b.s;
            d.y = b.y;
            d.t = b.t;
            c.r = d.r = b.r, c.g = d.g = b.g, c.b = d.b = b.b, c.a = d.a = b.a;
            a.r = b.r, a.g = b.g, a.b = b.b, a.a = b.a;
            emit(a, c, d);
            emit(d, c, b);
            kept = 0;
            break;
        }
        default:
            break;
        }
    };
    auto set_reg = [&](u32 reg, const u8 *q16) {
        u32 lo, hi, w2;
        memcpy(&lo, q16, 4);
        memcpy(&hi, q16 + 4, 4);
        memcpy(&w2, q16 + 8, 4);
        switch (reg) {
        case 0x0:                           // PRIM
            if (lo != prim) state_new = true;
            prim = lo;
            kept = 0;
            break;
        case 0x1:                           // RGBAQ
            cur.r = q16[0];
            cur.g = q16[4];
            cur.b = q16[8];
            cur.a = q16[12];
            break;
        case 0x2: {                         // ST, Q
            float st[3];
            memcpy(st, q16, 12);
            q = st[2] != 0 ? st[2] : 1;
            cur.s = st[0] / q * scale_s;
            cur.t = st[1] / q * scale_t;
            cur.w = 1 / q;
            break;
        }
        case 0x3:                           // UV: texels, 4 fractional bits
            if (g_texture) {
                cur.s = (lo & 0x3fff) / 16.0f / (float)(1 << LB(g_texture + 0x48)) * scale_s;
                cur.t = (hi & 0x3fff) / 16.0f / (float)(1 << LB(g_texture + 0x49)) * scale_t;
            }
            break;
        case 0x4:                           // XYZF2: Z is above four bits of nothing when PACKED
        case 0x5:                           // XYZ2
            cur.x = (lo & 0xffff) / 16.0f;
            cur.y = (hi & 0xffff) / 16.0f;
            cur.z = (float)(reg == 4 ? w2 >> 4 : w2) / 16777216.0f;
            kick(cur);
            break;
        default:
            break;
        }
    };
    while (p + 16 <= end) {
        u64 tag0;
        u64 regs;
        memcpy(&tag0, p, 8);
        memcpy(&regs, p + 8, 8);
        p += 16;
        u32 nloop = (u32)(tag0 & 0x7fff), flg = (u32)(tag0 >> 58 & 3), nreg = (u32)(tag0 >> 60 & 15);
        bool eop = (tag0 >> 15 & 1) != 0, pre = (tag0 >> 46 & 1) != 0;
        if (nreg == 0) nreg = 16;
        if (pre) {
            prim = (u32)(tag0 >> 47 & 0x7ff);
            kept = 0;
            state_new = true;
        }
        if (flg == 0) {                     // PACKED
            for (u32 l = 0; l < nloop; l++)
                for (u32 r = 0; r < nreg && p + 16 <= end; r++, p += 16) {
                    u32 reg = (u32)(regs >> (r * 4) & 15);
                    if (reg == 0xe) {       // A+D
                        u32 ad = p[8];
                        if (ad == 0x42) {
                            alpha = p[0];
                            alpha_fix = p[4];
                            state_new = true;
                        } else if (ad <= 5) {
                            u8 q16[16] = {};
                            if (ad == 1) {
                                q16[0] = p[0], q16[4] = p[1], q16[8] = p[2], q16[12] = p[3];
                            } else {
                                memcpy(q16, p, 8);
                            }
                            set_reg(ad, q16);
                        }
                    } else if (reg == 0xf) {
                        // NOP
                    } else {
                        set_reg(reg, p);
                    }
                }
        } else if (flg == 1) {              // REGLIST: the registers as they are, RenderEffects' sprites
            size_t words = (size_t)nloop * nreg;
            for (size_t i = 0; i < words && p + 8 <= end; i++, p += 8) {
                u32 reg = (u32)(regs >> ((i % nreg) * 4) & 15), lo, hi;
                memcpy(&lo, p, 4);
                memcpy(&hi, p + 4, 4);
                u8 q16[16] = {};
                u32 w2 = 0;
                switch (reg) {
                case 0x1:                   // RGBAQ: this is where Q comes from here
                    q16[0] = p[0], q16[4] = p[1], q16[8] = p[2], q16[12] = p[3];
                    memcpy(&q, p + 4, 4);
                    break;
                case 0x2:                   // ST
                    memcpy(q16, p, 8);
                    memcpy(q16 + 8, &q, 4);
                    break;
                case 0x4:                   // XYZF2: X, Y, Z 24, F 8
                case 0x5:                   // XYZ2: X, Y, Z 32
                    memcpy(q16, &lo, 2);
                    memcpy(q16 + 4, p + 2, 2);
                    w2 = reg == 4 ? (hi & 0xffffff) << 4 : hi;
                    memcpy(q16 + 8, &w2, 4);
                    break;
                default:
                    memcpy(q16, p, 8);
                    break;
                }
                set_reg(reg, q16);
            }
            if (words & 1) p += 8;
        } else {                            // IMAGE and the rest: not geometry
            p += (size_t)nloop * 16;
        }
        if (eop) break;
    }
    if (pc_fixes()) {
        trim_glyph(first_vertex);
        if (g_texture && g_texture == g_rvm_texture && LW(g_target) != 7) trim_mirror(first_vertex);
    }
    if (g_frame.wide) unstretch_2d(first_vertex, data, ox);
    g_clip_new = true;
}

static void update_clip()
{
    if (!g_target || !g_pack || !g_local) return;
    clip_matrix(g_clip, g_pack, g_local, LW(g_target + 0x1c) / 2, LW(g_target + 0x20) / 2, depth_scale(g_target));
    g_clip_new = true;
}

void native_read_list(u32 list)
{
    g_stat.lists++;
    for (u32 node = LW(list); node; node = LW(node)) {
        u32 type = LH(node + 4), data = LW(node + 12);
        g_nodes_seen[type & 15]++;
        switch (type) {
        case EDMA_SETTARGET:
            g_target = data;
            g_target_ptr = data;
            g_mode = LH(node + 6);
            update_clip();
            g_clip_new = true;
            break;
        case EDMA_SET_MATRIX_PACK:
            g_pack = data;
            update_clip();
            break;
        case EDMA_SET_LOCAL_WORLD:
            g_local = data;
            update_clip();
            break;
        case EDMA_LIGHTCONTEXT:
            g_light_ctx = data;
            break;
        case EDMA_LIGHTMATERIAL:
            // eLightMaterial: the frontend's data at +192, the race's at +256 by InGameFlag
            g_light_mat = data ? data + (LW(g_pc_ctx->r[28].w[0] - 21424) ? 256 : 192) : 0;
            break;
        case EDMA_DIRECT:
            if (main_target()) {
                g_draw_target = LW(g_target);
                add_direct(data, LH(node + 10));
            }
            break;
        case EDMA_POLY3D:                   // the same packet as a strip's: ePoly::Render3D, eEndStrip
        case EDMA_STRIP:
            if (g_target && LW(g_target) < 16) {
                auto &t = g_by_target[LW(g_target)];
                t.strips += LH(node + 8);
                t.pack = g_pack;
                t.target = g_target;
            }
            if (dumping() && type == EDMA_POLY3D) {
                float m[16], pq[24];
                read_quads(g_local, m, 4);
                read_quads(g_pack, pq, 6);
                fprintf(g_hle_log, "dump POLY3D target %u mode %u tex %s n %u param %x\n local", LW(g_target), g_mode, g_texture ? (const char *)host(g_texture + 8) : "-", LH(node + 8), LH(node + 6));
                for (float f : m) fprintf(g_hle_log, " %g", f);
                fprintf(g_hle_log, "\n pack");
                for (float f : pq) fprintf(g_hle_log, " %g", f);
                fprintf(g_hle_log, "\n");
                u32 d0 = LW(node + 12);
                for (u32 i = 0; i < LH(node + 10) * 4 && i < 40; i++) fprintf(g_hle_log, " %08x", LW(d0 + i * 4));
                fprintf(g_hle_log, "\n");
            }
            if (main_target() && g_pack && g_local) {
                g_draw_target = LW(g_target);
                u32 count = LH(node + 8);
                for (u32 i = 0; i < count; i++)
                    add_strip(LW(node + 12 + i * 8), LH(node + 10 + i * 8), LH(node + 6));
            }
            break;
        }
    }
}

// HP2_NATIVELOG: what the frame had and the numbers step 1 rests on
static void log_frame()
{
    static int frames;
    static const bool on = getenv("HP2_NATIVESTATS") != nullptr;    // the lists' numbers every 120 frames: their writes cost a frame
    if (!on || ++frames % 120) return;
    fprintf(g_hle_log, "native: light garbage cleared so far: %u\n", g_light_garbage);
    fprintf(g_hle_log, "native: frame %d lists %llu strips %llu bad %llu verts %llu tris %llu draws %zu\n", frames,
            (unsigned long long)g_stat.lists, (unsigned long long)g_stat.strips, (unsigned long long)g_stat.bad,
            (unsigned long long)g_stat.verts, (unsigned long long)g_stat.tris, g_frame.draws.size());
    for (auto &t : g_by_target) {
        if (!t.target) continue;
        fprintf(g_hle_log, "native: target %u %s scissor %d %d %d %d strips %llu direct %llu +44 %x +74 %g\n", LW(t.target), pc_str(LW(t.target + 4)).c_str(),
                LW(t.target + 0x14), LW(t.target + 0x18), LW(t.target + 0x1c), LW(t.target + 0x20), (unsigned long long)t.strips,
                (unsigned long long)t.direct, LW(t.target + 0x44), *(float *)host(t.target + 0x74));
        for (u32 m = 0; m < 4; m++) {
            u32 block = LW(t.target + 0x7c + m * 4);
            if (!block) continue;
            fprintf(g_hle_log, "native:   mode %u regs", m);
            for (u32 q = 1; q < 14; q++)
                if (LB(block + q * 16 + 8) == 0x47 || LB(block + q * 16 + 8) == 0x4e || LB(block + q * 16 + 8) == 0x4f)
                    fprintf(g_hle_log, " %02x=%x", LB(block + q * 16 + 8), LW(block + q * 16));
            fprintf(g_hle_log, "\n");
        }
        float q[32];
        if (t.pack) read_quads(t.pack, q, 8);
        for (int i = 0; t.pack && i < 8; i++)
            fprintf(g_hle_log, "native:   pack[%d] %g %g %g %g\n", i, q[i * 4], q[i * 4 + 1], q[i * 4 + 2], q[i * 4 + 3]);
    }
    if (on) {
        fprintf(g_hle_log, "native: list nodes by type:");
        for (int i = 0; i < 16; i++)
            if (g_nodes_seen[i]) fprintf(g_hle_log, " %d:%u", i, g_nodes_seen[i]);
        fprintf(g_hle_log, "\nnative: GIF prims:");
        for (int i = 0; i < 8; i++)
            if (g_prims_seen[i]) fprintf(g_hle_log, " %d:%u", i, g_prims_seen[i]);
        fprintf(g_hle_log, "\nnative: AlphaBits draws:");
        for (int i = 0; i < 256; i++)
            if (g_alpha_seen[i]) fprintf(g_hle_log, " %02x:%u", i, g_alpha_seen[i]);
        fprintf(g_hle_log, "\n");
    }
    fflush(g_hle_log);
}

static int g_lod_flip_now = -1;            // HP2_LODFLIP's: 0 Original, 1 Maximum, -1 the setting's

void native_end_frame()
{
    log_frame();
    // for the next frame's lists: eGetRenderTarget, eRenderTarget::GetTextureInfo. gp-24920 has swapped by now
    g_rvm_target = pc_call(0x105848, 7);
    g_rvm_texture = g_rvm_target ? pc_call(0x105aa0, g_rvm_target) : 0;
    g_prev_target = LW(g_pc_ctx->r[28].w[0] - 24920);
    g_prev_texture = g_prev_target ? pc_call(0x105aa0, g_prev_target) : 0;
    u32 falloff = LW(0x32f6d0);             // pTextureInfoSpecularFalloff
    g_frame.road_specular = falloff ? native_texture(falloff, g_frame_no) : nullptr;
    // eDisplayFrame's ESTAGE_FECARS: in the frontend (CurrentGameFlowState 3) the game downloads pTextureInfoCarSelectEnvMap to VRAM 10240 every
    // frame and the cars' environment pass reads it, not the sphere map a race left behind
    u32 car_env = LW(0x330544) == 3 ? LW(0x32f6d8) : 0;      // CurrentGameFlowState (gp-21420), pTextureInfoCarSelectEnvMap (gp-25112)
    g_frame.env_override = car_env ? native_texture(car_env, g_frame_no) : nullptr;
    u32 env = pc_call(0x105848, 8);
    u32 env_info = env ? pc_call(0x105aa0, env) : 0;
    g_frame.env_linear = !env_info || LB(env_info + 0x6f) != 0;
    g_frame.samples = host(0x2d7130);       // ReflectionSamples
    // The rects StuffSun asked for this frame: gp-24684 is this frame's queue by now (ExecuteRenderStageQueue swapped them)
    g_frame.depth_rects.clear();
    u32 stage_head = LW(g_pc_ctx->r[28].w[0] - 24684) + 2128;
    for (u32 n = LW(stage_head); n != stage_head && g_frame.depth_rects.size() < 16; n = LW(n))
        g_frame.depth_rects.push_back({(int)LH(n + 20), (int)LH(n + 22), (int)LH(n + 24), (int)LH(n + 26)});
    g_frame.back_samples = host(0x2d9158);  // BackReflectionSamples
    native_texture_take_time(g_frame.tex_ms, g_frame.tex_decoded);
    if (g_lod_flip_now >= 0 && pc_racing()) {
        static double verts[2], draws[2];
        static int n[2];
        verts[g_lod_flip_now] += (double)(g_frame.indices.size() + g_frame.vertices.size());
        draws[g_lod_flip_now] += (double)g_frame.draws.size();
        n[g_lod_flip_now]++;
        if (n[0] + n[1] >= 600 && n[0] && n[1]) {
            fprintf(g_hle_log, "native: lodflip: Original %.0f vertices, %.0f draws; Maximum %.0f vertices, %.0f draws\n", verts[0] / n[0], draws[0] / n[0],
                    verts[1] / n[1], draws[1] / n[1]);
            verts[0] = verts[1] = draws[0] = draws[1] = 0;
            n[0] = n[1] = 0;
        }
    }
    native_vk_draw(g_frame);
    g_frame.list_ms = 0;
    if (!g_frame.uploaded) cache_flush();
    // the next frame's cache: emptied before it can fill up within a frame
    const u32 cap = native_vk_cache_vertices();
    const u32 margin = std::min<u32>(1536 * 1024, cap / 2);
    if (cap != g_cache_cap || g_cache_top + margin > cap) {
        if (g_cache_top || cap != g_cache_cap) cache_flush();
        g_cache_cap = cap;
    }
    g_frame.dyn_base = g_cache_cap;
    g_frame.uploaded = false;
    g_frame.indices.clear();
    g_frame.fresh.clear();
    g_frame.fresh_runs.clear();
    if (g_frame_no % 600 == 0 && getenv("HP2_NATIVELOG")) {
        fprintf(g_hle_log, "native: vertex cache %u of %u vertices used, %llu hits, %llu new, %llu emptyings%s\n", g_cache_top, g_cache_cap,
                (unsigned long long)g_cache_hits, (unsigned long long)g_cache_misses, (unsigned long long)g_cache_flushes, g_cache_off ? " (off)" : "");
        if (g_cache_checking) fprintf(g_hle_log, "native: vcachecheck: %llu hits not what they should be\n", (unsigned long long)g_cache_check_bad);
    }
    g_frame_no++;
    g_frame.vertices.clear();
    g_frame.draws.clear();
    g_frame.lights.clear();
    g_frame.fx.clear();
    g_packet_spans.clear();
    g_light_ctx = g_light_mat = 0;
    g_light = -1;
    g_target = g_pack = g_local = g_mode = g_texture = 0;
    g_clip_new = true;
    memset(&g_stat, 0, sizeof g_stat);
    memset(g_by_target, 0, sizeof g_by_target);
}

void HOOK_AttachDmaDataListMain__FP9eDmaChainP12eDmaDataListP8DMAState(Ctx &c, void (*orig)(Ctx &))
{
    // What the game does next is to turn the list into packets for VU1 and the GS, and nothing reads those: the list is what is drawn.
    // HP2_KEEPPACKETS=1 builds them all the same.
    const auto t0 = std::chrono::steady_clock::now();
    native_read_list(c.r[5].w[0]);
    g_frame.list_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static const bool keep = getenv("HP2_KEEPPACKETS") != nullptr;
    if (keep) orig(c);
}

// The body's flicker, on both renderers. In a race with fewer than two point lights near, retail leaves
// light_positions[1] and [2] unwritten: the lighter gets directions from the stack, up to 1e29. Their colour is 0,
// and on VU1 0 times anything is 0; on a PC the same product can be a NaN. So a light without a colour gets no direction.
// eLightContext: LocalColourMatrix +0, LocalDirectionMatrix +0x40 (a light's direction down lane i), LocalLightPositions +0x80.
void HOOK_elSetupLightContext__FP13eLightContextP8bMatrix4T1P8bVector4P5eView(Ctx &c, void (*orig)(Ctx &))
{
    u32 lc = c.r[4].w[0];
    orig(c);
    if (!lc) return;
    static const bool log = getenv("HP2_NATIVELOG") != nullptr;
    for (u32 i = 1; i < 3; i++) {
        if (LW(lc + i * 16) || LW(lc + i * 16 + 4) || LW(lc + i * 16 + 8)) continue;
        for (u32 row = 0; row < 3; row++) {
            if (log) {
                u32 bits = LW(lc + 0x40 + row * 16 + i * 4);
                float v;
                memcpy(&v, &bits, 4);
                if (!(v >= -2.0f && v <= 2.0f)) g_light_garbage++;
            }
            SW(lc + 0x40 + row * 16 + i * 4, 0);
        }
        for (u32 k = 0; k < 3; k++) SW(lc + 0x80 + i * 16 + k * 4, 0);
    }
}

void HOOK_AttachTextureEnvDirect__FP9eDmaChainP11TextureInfo(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    g_texture = c.r[5].w[0];
    g_clip_new = true;
    orig(c);
}

// ---- what is laid over the view ---------------------------------------------------------------
static const u32 kFogPalette = 0x2da780, kFocusPalettes = 0x2db380, kDrawFog = 0x32faa0, kFogMode1 = 0x32fa98, kFogMode2 = 0x32fa9c;

// eGetView: a player's view, if it and its target are active
static u32 active_view_target(u32 id)
{
    u32 view = pc_call(0x107c90, id);
    if (!view || !LB(view + 4)) return 0;
    u32 target = LW(view + 48);
    return target && LW(target + 8) ? target : 0;
}

static NativeFx &add_fx(int kind, u32 target)
{
    g_frame.fx.emplace_back();
    NativeFx &f = g_frame.fx.back();
    f.at = (u32)g_frame.draws.size();
    f.kind = (u8)kind;
    f.darken = false;
    f.rect[0] = (float)(s32)LW(target + 0x14);
    f.rect[1] = (float)(s32)LW(target + 0x18);
    f.rect[2] = (float)(s32)LW(target + 0x1c);
    f.rect[3] = (float)(s32)LW(target + 0x20);
    return f;
}

// RemapLinearIntoClut8's order: of each 32 entries, the second 8 and the third 8 trade places
static void read_palette(NativeFx &f, u32 addr)
{
    for (u32 k = 0; k < 256; k++)
        f.palette[k] = LW(addr + ((k & ~0x18u) | (k & 8) << 1 | (k & 0x10) >> 1) * 4);
}

void HOOK_eAttachFog__FP9eDmaChainUi(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    orig(c);
    if (!LW(kDrawFog)) return;
    for (u32 id = 1; id <= 2; id++)
        if (u32 target = active_view_target(id)) {
            NativeFx &f = add_fx(FX_FOG, target);
            f.darken = LW(id == 1 ? kFogMode1 : kFogMode2) == 1;
            read_palette(f, kFogPalette + (id - 1) * 1024);
        }
}

void HOOK_eAttachDepthOfField__FP9eDmaChainUiUi(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    orig(c);
    if (!c.r[2].w[0]) return;                           // nothing out of focus
    for (u32 id = 1; id <= 2; id++)
        if (u32 target = active_view_target(id)) read_palette(add_fx(FX_DOF, target), kFocusPalettes + (id - 1) * 1024);
}

void HOOK_eAttachBlackAndWhite__FP9eDmaChainUiUi(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    orig(c);
    u32 force = LW(c.r[28].w[0] - 24048);               // ForceBlackAndWhite
    for (u32 id = 1; id <= 2; id++) {
        u32 view = pc_call(0x107c90, id);
        if (!view) continue;
        u32 target = LW(view + 48);
        if (!target) continue;
        if (force || (LB(view + 4) && LW(target + 8) && LW(view + 116))) add_fx(FX_GREY, target);
    }
}

// The sun's flare looks at the Z buffer over the sun: the queue's rects go to where the game wants them, from the last frame's depth, as the
// GS's frame was the last one by then too. The rest is FetchVRAM's own: the stage moves to the list of those done.
void HOOK_FetchVRAM__FP12eRenderStage(Ctx &c, void (*orig)(Ctx &))
{
    (void)orig;
    u32 stage = c.r[4].w[0], head = stage + 80, done = stage + 88;
    while (LW(head) != head) {
        u32 n = LW(head), next = LW(n), prev = LW(n + 4);
        SW(prev, next);
        SW(next + 4, prev);
        u32 tail = LW(done + 4);
        SW(tail, n);
        SW(done + 4, n);
        SW(n + 4, tail);
        SW(n, done);
        NativeDepthRect r = {(int)LH(n + 20), (int)LH(n + 22), (int)LH(n + 24), (int)LH(n + 26)};
        static std::vector<uint32_t> words;
        words.resize((size_t)r.w * r.h);
        if (!words.empty() && native_vk_depth(r, words.data())) memcpy(host(LW(n + 8)), words.data(), words.size() * 4);
    }
}

void HOOK_Render__12FlailerStripP5eView(Ctx &c, void (*orig)(Ctx &))
{
    // the polys are a list at +36, their vertices' x at +0x10 of each 16 bytes
    u32 strip = c.r[4].w[0], head = strip + 36;
    std::vector<StripSpan> polys;
    for (u32 p = LW(head); p != head && p; p = LW(p)) {
        StripSpan s = {1e9f, -1e9f};
        for (int i = 0; i < 4; i++) {
            float x;
            memcpy(&x, host(p + 0x10 + i * 16), 4);
            s.x0 = std::min(s.x0, x), s.x1 = std::max(s.x1, x);
        }
        polys.push_back(s);
    }
    std::sort(polys.begin(), polys.end(), [](const StripSpan &a, const StripSpan &b) { return a.x0 < b.x0; });
    g_strip_spans.clear();
    for (const StripSpan &s : polys) {
        // the gaps inside a text are well under this, the lights and the right panel are 60 pixels apart
        if (!g_strip_spans.empty() && s.x0 - g_strip_spans.back().x1 < 32) g_strip_spans.back().x1 = std::max(g_strip_spans.back().x1, s.x1);
        else g_strip_spans.push_back(s);
    }
    orig(c);
    g_strip_spans.clear();
}

void HOOK_AddData__12eDmaDataListUsPvUsUs(Ctx &c, void (*orig)(Ctx &))
{
    if (!g_strip_spans.empty() && (c.r[5].w[0] & 0xffff) == EDMA_DIRECT) g_packet_spans[c.r[6].w[0]] = g_strip_spans;
    orig(c);
}

// Wider than the PS2's: the projection takes less of x for the same pixels, and the clip volume, which the game culls by, takes more.
void HOOK_CreateViewMatricies__FP5eViewP8bMatrix4T1T1T1T1T1fff(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 view = c.r[4].w[0], target = view ? LW(view + 48) : 0, id = target ? LW(target) : 99;
    float f = native_narrow();
    if (f >= 1 || id > 2 || !pc_racing()) {
        orig(c);
        return;
    }
    u32 width = LW(target + 0x1c);
    float aspect;
    memcpy(&aspect, host(target + 0x58), 4);
    float narrowed = aspect * f;
    SW(target + 0x1c, (u32)((float)width / f + 0.5f));
    memcpy(host(target + 0x58), &narrowed, 4);
    orig(c);
    SW(target + 0x1c, width);
    memcpy(host(target + 0x58), &aspect, 4);
}

// The race's frame rate. eDisplayFrame holds a race to two vertical blanks a frame while the word at gp-25272 is set, and nothing else reads
// that word. Everything else goes by the time a frame took, from 1/60 s up (PrepareRealTimestep), so 0 there is a race at 60.
static void set_frame_rate(Ctx &c)
{
    static const u32 retail = LW(c.r[28].w[0] - 25272);
    SW(c.r[28].w[0] - 25272, g_cfg.frame_rate == 60 ? 0 : retail);
}

// HP2_SPEEDTRACE=<m/s>: the frames around a car passing that speed. Car: Position +96, Velocity +112, wheels down +956, ForwardSpeed +960, gear +968, throttle +972, rpm +976
static void speed_trace(Ctx &c, u32 world, int n, float step, float wt)
{
    static const char *env = getenv("HP2_SPEEDTRACE");
    if (!env) return;
    static const float limit = (float)atof(env);
    struct Row { float wt, step, v, vx, vy, vz, z, thr, rpm; int wheels, gear; };
    enum { BEFORE = 45, AFTER = 45 };
    static Row ring[14][BEFORE];
    static u32 at[14];
    static int after[14], events;
    static float prev[14];
    const auto f = [](u32 a) { float x; memcpy(&x, host(a), 4); return x; };
    for (int i = 0; i < n; i++) {
        const u32 car = LW(world + 44 + i * 4);
        if (!car) continue;
        Row r{wt, step, f(car + 960), f(car + 112), f(car + 116), f(car + 120), f(car + 104), f(car + 972), f(car + 976), (int)LW(car + 956), (int)LW(car + 968)};
        const auto print = [&](const Row &x, char tag) {
            fprintf(stderr, "trace %d %c w %.3f dt %.4f v %.1f vel %.1f %.1f %.1f z %.1f thr %.2f rpm %.0f gear %d wheels %d\n", i, tag, x.wt, x.step, x.v, x.vx, x.vy, x.vz, x.z, x.thr, x.rpm, x.gear, x.wheels);
        };
        if (after[i] > 0) {
            print(r, '+');
            after[i]--;
        } else if (r.v > limit && prev[i] <= limit && events < 300) {
            events++;
            after[i] = AFTER;
            for (u32 k = 0; k < BEFORE; k++) print(ring[i][(at[i] + k) % BEFORE], '-');
            print(r, '*');
        }
        ring[i][at[i]++ % BEFORE] = r;
        prev[i] = r.v;
    }
}

// HP2_SPEEDLOG=1: every car once a second, for comparing frame rates: the distance it has gone, its top speed, its sharpest change of speed,
// and any speed that is not a number. World's table: count +28, cars +44; a car's ForwardSpeed +960 (tPursuit::GetSpeedMPHOrKPH)
static void speed_log(Ctx &c)
{
    static const bool on = getenv("HP2_SPEEDLOG") != nullptr;
    if (!on || !pc_racing()) return;
    const u32 gp = c.r[28].w[0], world = LW(gp - 22120);
    if (!world) return;
    const int n = (int)LW(world + 28);
    if (n <= 0 || n > 14) return;
    struct Track { u32 car; float last, dist, top, jolt; u32 bad; };
    static Track t[14];
    u32 ue = LW(gp - 19596);
    float step;
    memcpy(&step, &ue, 4);
    // paused, the step goes on but the world does not
    u32 uw = LW(gp - 19572);
    float wt;
    memcpy(&wt, &uw, 4);
    static float last_wt;
    const bool moved = wt != last_wt;
    last_wt = wt;
    if (moved) speed_trace(c, world, n, step, wt);
    // HP2_FORCEFINISH=<world seconds>: the player's car finishes there, second, as the game's debug menu does it (Race +1608 the place, +1612 the player)
    static const char *finish = getenv("HP2_FORCEFINISH");
    static bool finished;
    if (finish && !finished && wt > (float)atof(finish) && wt < (float)atof(finish) + 2 && LW(gp - 22272)) {   // not the last race's clock, before this one's starts
        finished = true;
        SW(LW(gp - 22272) + 1608, 2);
        SW(LW(gp - 22272) + 1612, 0);
        fprintf(stderr, "speedlog: forced finish at world %.2f\n", wt);
    }
    for (int i = 0; i < n; i++) {
        const u32 car = LW(world + 44 + i * 4);
        if (!car) continue;
        Track &k = t[i];
        if (k.car != car) k = {car, 0, 0, 0, 0, 0};
        float v;
        memcpy(&v, host(car + 960), 4);
        if (v != v || v > 1e6f || v < -1e6f) {
            k.bad++;
            continue;
        }
        const float a = v < 0 ? -v : v;
        if (moved) k.dist += a * step;
        if (a > k.top) k.top = a;
        if (step > 0) {
            const float d = (v - k.last) / step, j = d < 0 ? -d : d;
            if (j > k.jolt) k.jolt = j;
        }
        k.last = v;
    }
    static u32 last_real;
    const u32 real = LW(gp - 19608);
    if (real - last_real < 4000) return;        // RealTimer: 1/4000 s
    last_real = real;
    // ReplayManager (gp-22440): +0 it plays back, +16 paused, +20 slow motion (World::GetTimestep)
    const u32 rm = LW(gp - 22440);
    float boost[14] = {};
    int mover[14] = {};
    for (int i = 0; i < n; i++)
        if (const u32 car = LW(world + 44 + i * 4)) {
            memcpy(&boost[i], host(car + 992), 4);
            const u32 m = LW(car + 48), vt = m ? LW(m + 4) : 0;     // the mover: 1 the belt, 2 real physics, 3 mellow
            mover[i] = vt == 0x2bb090 ? 1 : vt == 0x2aeac8 ? 2 : vt == 0x2bc730 ? 3 : 0;
        }
    fprintf(stderr, "speedlog: fps %d replay %u paused %u slow %u real %.2f world %.2f step %.4f n %d", g_cfg.frame_rate, rm ? LW(rm) : 0, rm ? LW(rm + 16) : 0, rm ? LW(rm + 20) : 0,
            real / 4000.0, wt, step, n);
    for (int i = 0; i < n; i++) fprintf(stderr, " | %.0f %.1f %.1f %.0f %u %.2f %d", t[i].dist, t[i].last, t[i].top, t[i].jolt, t[i].bad, boost[i], mover[i]);
    fprintf(stderr, "\n");
    // the player's speed over the driving line's, the term of the catch-up (Player +160 its Average, +8 the value; +168 the peak)
    if (const u32 p = LW(gp - 20936)) {
        const u32 avg = LW(p + 160);
        float ratio = 0, peak = 0;
        if (avg) memcpy(&ratio, host(avg + 8), 4);
        memcpy(&peak, host(p + 168), 4);
        fprintf(stderr, "speedratio: world %.2f ratio %.3f peak %.3f\n", wt, ratio, peak);
    }
    // the race as the game counts it (Race: count +24, pRacingCars +1572; RacingCar: car +0, position +10, finished +12, lap +20)
    if (const u32 race = LW(gp - 22272)) {
        fprintf(stderr, "speedrace: world %.2f", wt);
        for (int i = 0; i < (int)LW(race + 24) && i < 8; i++) {
            const u32 rc = LW(race + 1572 + i * 4);
            if (!rc) continue;
            float lap;
            memcpy(&lap, host(rc + 20), 4);
            int index = -1;
            for (int k = 0; k < n; k++)
                if (LW(world + 44 + k * 4) == LW(rc)) index = k;
            fprintf(stderr, " | %d pos %d lap %.3f done %d", index, (int)(s8)LB(rc + 10), lap, (int)(s8)LB(rc + 12));
        }
        fprintf(stderr, "\n");
    }
}

// The racers think in turn: WorldLoopCounter % 8 is the place in the world's table of the one whose turn it is (AICarController::Think, ai-car-controller.md).
// So a racer thinks every 8 frames, every 0.27 s at the 30 frames the game was made for. A racer far from the players rides a BeltMover, which each think
// starts again from the car's speed and gains speed by it. At 60 frames there are twice the thinks. Keep the 30 turns a second, by the world's clock,
// whatever the frame rate. Think alone sees the counter so made; the collisions and the replay use the real one.
void HOOK_Think__15AICarControllerf(Ctx &c, void (*orig)(Ctx &))
{
    if (g_cfg.frame_rate != 60) {
        orig(c);
        return;
    }
    const u32 gp = c.r[28].w[0], world = LW(gp - 22120), car = LW(c.r[4].w[0]);
    const int first = (int)LW(world + 32), count = (int)LW(world + 36) - first;
    int me = -1;
    for (int i = 0; i < count && i < 8; i++)
        if (LW(world + 44 + (u32)(first + i) * 4) == car) me = i;
    if (me < 0) {                   // traffic and cops: their own timing
        orig(c);
        return;
    }
    const s32 real = (s32)LW(gp - 19552);
    u32 uw = LW(gp - 19572);
    float wt;
    memcpy(&wt, &uw, 4);
    static s32 seen = -1;
    static s64 tick, prev;
    if (real != seen) {
        seen = real;
        prev = tick;
        tick = (s64)(wt * 30.0f);
    }
    bool turn = false;
    if (tick < prev) turn = tick % 8 == me;             // a replay went back
    else if (tick - prev >= 8) turn = true;
    else
        for (s64 t = prev + 1; t <= tick; t++)
            if (t % 8 == me) turn = true;
    const int want = turn ? me : (me + 1) % 8;          // past the last racer, or another's place: no one's or not mine
    s32 fake = real - (((real % 8) - want + 8) % 8);
    if (fake < 0) fake += 8;
    SW(gp - 19552, (u32)fake);
    orig(c);
    SW(gp - 19552, (u32)real);
}

// The draw distance. An object of the scenery shows while it is 2 pixels of the GS's or more, and its levels of detail go by the same size, H * r / distance
// (scenerycull.md, DrawAScenery). So the size is taken with a larger H for the players' views, and far things stay, on their better levels. The maps of the
// environment (views 4 to 9) keep what they had. SceneryCullInfo: pView +128, H +192; eView: ID +0.
static bool player_view(u32 info)
{
    const u32 view = LW(info + 128), id = view ? LW(view) : 0;
    return id >= 1 && id <= 3;
}

void HOOK_TreeCull__20ScenerySectionHeaderP15SceneryCullInfo(Ctx &c, void (*orig)(Ctx &))
{
    const u32 info = c.r[5].w[0];
    if (g_cfg.draw_distance == 100 || !player_view(info)) {
        orig(c);
        return;
    }
    const u32 h = LW(info + 192);
    float f;
    memcpy(&f, &h, 4);
    f *= g_cfg.draw_distance * 0.01f;
    u32 scaled;
    memcpy(&scaled, &f, 4);
    SW(info + 192, scaled);
    orig(c);
    SW(info + 192, h);
}

// The level of detail (Graphics, Level of Detail: Maximum, the default). DrawAScenery takes the first LOD k with size >= FarClipSize[k] that has a model
// (scenerycull.md): FarClipSize[0] at 0 for the call gives every object that is drawn its LOD 0. ScenerySectionHeader: pSceneryInfo +20,
// pSceneryInstance +28; SceneryInstance 48 bytes, the kind at +12; SceneryInfo 40 bytes, FarClipSize[3] int16 at +12, pModel[3] at +20.
// HP2_LODFLIP: Maximum and Original by turns, 60 frames each, and the vertices and draws of each in the log: the setting measured on the
// same scenes
static bool max_detail_now() { return g_lod_flip_now >= 0 ? g_lod_flip_now != 0 : g_cfg.max_detail != 0; }

void HOOK_DrawAScenery__20ScenerySectionHeaderiP15SceneryCullInfoi(Ctx &c, void (*orig)(Ctx &))
{
    if (!max_detail_now()) {
        orig(c);
        return;
    }
    const u32 header = c.r[4].w[0], index = c.r[5].w[0];
    const u32 instances = LW(header + 28), infos = LW(header + 20);
    const u32 info = instances && infos ? infos + 40 * (u32)LH(instances + 48 * index + 12) : 0;
    if (!info || !LW(info + 20)) {
        orig(c);
        return;
    }
    const u16 was = LH(info + 12);
    SH(info + 12, 0);
    orig(c);
    SH(info + 12, was);
}

// ForceCarLOD, ForceTireLOD (carrender.c): -1 the game's choice by the car's size, else that level. The car screens set and put them back
// themselves, around their own drawing.
static void car_detail()
{
    static bool ours;
    const u32 car = 0x32fc60, tire = 0x32fc64;
    static const bool flip = getenv("HP2_LODFLIP") != nullptr;
    if (flip) g_lod_flip_now = (int)(g_frame_no / 60 % 2);
    if (max_detail_now()) {
        if ((s32)LW(car) == -1 && (s32)LW(tire) == -1) {
            SW(car, 0);
            SW(tire, 0);
            ours = true;
        }
    } else if (ours) {
        SW(car, (u32)-1);
        SW(tire, (u32)-1);
        ours = false;
    }
}

// A visible zone lists the sections that can be seen from it at the PS2's distance. With more, all that are loaded (0x2f8fe8, by number) go through the frustum and the size.
void HOOK_WhatSectionsShouldWeDraw__20GrandSceneryCullInfoPsiP15SceneryCullInfo(Ctx &c, void (*orig)(Ctx &))
{
    const u32 out = c.r[5].w[0], info = c.r[7].w[0];
    const int max = (int)c.r[6].w[0];
    orig(c);
    if (g_cfg.draw_distance == 100 || !player_view(info)) return;
    int count = (int)c.r[2].w[0];
    bool listed[100] = {};
    for (int i = 0; i < count; i++) {
        const int n = (int)(s16)LH(out + (u32)i * 2);
        if (n >= 0 && n < 100) listed[n] = true;
    }
    for (int n = 0; n < 90 && count < max; n++)
        if (!listed[n] && LW(0x2f8fe8 + (u32)n * 4)) {
            SH(out + (u32)count * 2, (u16)n);
            count++;
        }
    c.r[2].d[0] = (u64)(s64)count;
}

void HOOK_eDisplayFrame__Fv(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    set_frame_rate(c);
    speed_log(c);
    pc_paint_buttons();         // the device changes in a race too
    g_frame.wide = native_narrow() < 1 && pc_racing();
    car_detail();
    orig(c);
    native_end_frame();
}
