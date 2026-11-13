// The native renderer, the game's side: the draw lists and the strips (native.h). Addresses and formats
// were checked on the retail: handoff.md, "Нативный рендер: списки Ecstasy".
#include "native.h"

#include "hw.h"
#include "pcconfig.h"
#include "pcmenu.h"
#include "memwatch.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>

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
    if (d.tex) d.tex_mode = native_tex_mode(*d.tex), d.tex_upscaled = d.tex->upscaled, d.tex_min_linear = d.tex->min_linear;
}
static float g_clip[16];
static bool g_clip_stale;                   // the target, the pack or the local-world changed since g_clip was made (update_clip)
static void clip_now();
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
// The race's aspect for the frame being made: the setting's, no wider than the window (pc_aspect). Taken once a frame, as the last one is
// handed over: the window goes fullscreen and back on its own thread, and a frame's lists and its picture must have one aspect.
static int g_aspect_now = -1;
int native_aspect()
{
    if (g_aspect_now < 0) g_aspect_now = pc_aspect();
    return g_aspect_now;
}

float native_narrow()
{
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    if (exact) return 1.0f;
    const int aspect = native_aspect();
    return aspect == 2 ? (4.0f / 3.0f) / (16.0f / 9.0f) : aspect == 1 ? (4.0f / 3.0f) / (16.0f / 10.0f) : 1.0f;
}

// The packets of a Flailer object and its pieces: a text goes as one thing, not glyph by glyph. One object can hold pieces far apart
// that share a texture (the last lights under the mirror and the panel by the right edge), and each of them goes on its own.
struct StripSpan {
    float x0, x1, whole;                                    // whole: the middle of the line of text the span is part of
};
static std::unordered_map<u32, std::vector<StripSpan>> g_packet_spans;
static std::vector<StripSpan> g_strip_spans;

// The 2D of a wide frame. The game lays it out for 4:3; on a wider picture every piece is narrowed by the same factor, and what matters is
// where: the HUD's left part keeps to the left edge, its right part to the right edge, the middle (the mirror and its lights, 160..351, and
// the messages) to the middle. A piece is placed by the middle of the line it belongs to, never by its own: letters of one line placed one
// by one come apart where the line crosses from one part to the next ("Led          a lap").
//
// The lines are found from what was drawn, at the frame's end (resolve_wide_2d), not from the game's objects: a message that slides or grows
// in is drawn by its animation, outside the objects' own lists, and the lines made from those lists (find_lines) never saw it. Until then the
// layer's quads are only written down.
struct Wide2D {
    u32 first, count;                                       // into g_frame.vertices
    float x0, x1, y0, y1, ox;                               // x without the target's offset
    u32 packet, mark;                                       // the packet (its strip's spans), and what the quad shows: its texture and corner
    bool edges;
};
static std::vector<Wide2D> g_wide_2d;

static void unstretch_2d(u32 first, u32 data, float ox)
{
    float f = native_narrow();
    u32 id = LW(g_target_ptr);
    if (id == 7 || id >= 8) return;
    size_t n = g_frame.vertices.size() - first;
    if (!n) return;
    bool layer = id == 0 || id == 3;
    u32 radial = LW(g_pc_ctx->r[28].w[0] - 25124);          // pTextureInfoRadialBlur
    if (!layer && g_texture && (g_texture == radial || g_texture == g_prev_texture)) return;    // the whole view, cut in four
    bool edges = pc_hud_up();
    size_t group = n % 6 ? n : 6;
    for (size_t at = 0; at < n; at += group) {
        NativeVertex *v = &g_frame.vertices[first + at];
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (size_t i = 1; i < group; i++) {
            x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
            y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
        }
        x0 -= ox, x1 -= ox;
        // across the whole screen: a plain colour (a fade, a letterbox, a line) stays across the wide screen too; a picture keeps its shape
        if (x1 - x0 >= 400 && (!g_texture || LH(g_texture + 0x24) <= 64)) continue;
        if (layer) {
            u32 st[2];
            memcpy(st, &v[0].s, 8);
            g_wide_2d.push_back({(u32)(first + at), (u32)group, x0, x1, y0, y1, ox, data, g_texture ^ st[0] * 31 ^ st[1] * 131, edges});
            continue;
        }
        float mid = (x0 + x1) / 2;                          // in the view (the sun, a spark): narrowed where it is
        for (size_t i = 0; i < group; i++) v[i].x = ox + mid + (v[i].x - ox - mid) * f;
    }
}

// Where the middle of a line goes. Standing still, by the part of the screen it is in. A line that moves (a message sliding in from an
// edge) would jump as its middle passes from one part to the next, so while it moves the two parts' places are blended over 64 pixels.
static float wide_place(float m, bool edges, bool moving, float f)
{
    const float centre = 256 + (m - 256) * f;
    if (!edges) return centre;
    const float left = m * f, right = 512 - (512 - m) * f;
    if (!moving) return m < 150 ? left : m > 352 ? right : centre;
    if (m < 118) return left;
    if (m < 182) return left + (centre - left) * (m - 118) / 64;
    if (m <= 320) return centre;
    if (m <= 384) return centre + (right - centre) * (m - 320) / 64;
    return right;
}

static bool dumping();
static void resolve_wide_2d()
{
    if (g_wide_2d.empty()) return;
    const float f = native_narrow();
    const size_t n = g_wide_2d.size();
    // a letter's quad: the HUD's fonts are under 40 pixels a glyph, a message growing in can be larger for a moment
    auto letter = [](const Wide2D &q) { return q.x1 - q.x0 <= 64 && q.y1 - q.y0 <= 64; };
    std::vector<int> order(n), up(n);
    for (size_t i = 0; i < n; i++) order[i] = up[i] = (int)i;
    std::sort(order.begin(), order.end(), [](int a, int b) { return g_wide_2d[a].x0 < g_wide_2d[b].x0; });
    auto root = [&](int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    // letters within a word's gap of each other across, and on the same line (or touching the next): one line. The gap goes by the letters'
    // height, so that a large font's wider spaces are still spaces. Only letters make a line: a panel or a bar is no word of one.
    for (size_t a = 0; a < n; a++) {
        const Wide2D &i = g_wide_2d[order[a]];
        if (!letter(i)) continue;
        for (size_t b = a + 1; b < n; b++) {
            const Wide2D &j = g_wide_2d[order[b]];
            if (j.x0 - i.x1 >= 64) break;
            if (!letter(j) || j.y0 > i.y1 + 4 || i.y0 > j.y1 + 4) continue;
            if (j.x0 - i.x1 >= std::max(32.0f, 0.9f * std::max(i.y1 - i.y0, j.y1 - j.y0))) continue;
            up[root(order[b])] = root(order[a]);
        }
    }
    struct Line {
        float lo = 1e9f, hi = -1e9f, top = 1e9f;
        int count = 0;
        u32 mark = 0;
        float mark_x = 1e9f;
        bool moving = false;
    };
    std::vector<Line> lines(n);
    for (size_t i = 0; i < n; i++) {
        const Wide2D &q = g_wide_2d[i];
        Line &l = lines[root((int)i)];
        l.lo = std::min(l.lo, q.x0), l.hi = std::max(l.hi, q.x1), l.top = std::min(l.top, q.y0);
        l.count++;
        if (q.x0 < l.mark_x) l.mark_x = q.x0, l.mark = q.mark;          // the line's first letter
    }
    // Does the line move? It is known from one frame to the next by its first letter, its length and its row; once seen moving it is a
    // moving line for as long as it is there, so that it does not jump where it comes to rest either.
    struct Seen {
        float mid;
        bool moving;
    };
    static std::unordered_map<u64, Seen> seen, now;
    now.clear();
    for (size_t i = 0; i < n; i++) {
        Line &l = lines[i];
        if (l.count < 2 || root((int)i) != (int)i) continue;
        const u64 key = (u64)l.mark << 32 | (u32)l.count << 8 | ((u32)(l.top / 64) & 255);
        const float mid = (l.lo + l.hi) / 2;
        auto was = seen.find(key);
        l.moving = was != seen.end() && (was->second.moving || std::fabs(was->second.mid - mid) > 0.25f);
        now[key] = {mid, l.moving};
    }
    seen.swap(now);
    for (size_t i = 0; i < n; i++) {
        const Wide2D &q = g_wide_2d[i];
        const Line &l = lines[root((int)i)];
        const float mid = (q.x0 + q.x1) / 2;
        float whole = mid;
        if (l.count >= 2) {
            whole = (l.lo + l.hi) / 2;
        } else {
            // a piece on its own: its strip's span, as the game's objects have it (a panel in several parts)
            auto spans = g_packet_spans.find(q.packet);
            if (spans != g_packet_spans.end())
                for (const StripSpan &sp : spans->second)
                    if (mid >= sp.x0 && mid <= sp.x1) whole = sp.whole;
        }
        const float place = wide_place(whole, q.edges, l.moving, f);
        NativeVertex *v = &g_frame.vertices[q.first];
        for (u32 k = 0; k < q.count; k++) v[k].x = q.ox + place + (v[k].x - q.ox - whole) * f;
        if (dumping())
            fprintf(g_hle_log, "dump 2D quad %zu x %.1f..%.1f y %.1f..%.1f %s line %d of %d, %.1f..%.1f%s -> middle %.1f at %.1f\n", i, q.x0, q.x1, q.y0, q.y1,
                    letter(q) ? "letter" : "piece", root((int)i), l.count, l.lo, l.hi, l.moving ? " moving" : "", whole, place);
    }
    g_wide_2d.clear();
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
// The cache's table: open addressing in one block, a slot 32 bytes, so a strip seen before costs one miss of the CPU's cache where the
// standard map cost two or three (a heavy frame asks for some 60 thousand strips). A slot is in use while its generation is the table's:
// emptying the cache is one increment.
struct StripSlot {
    StripKey key;
    CachedStrip strip;
    u32 generation;
};
static std::vector<StripSlot> g_strip_slots;
static u32 g_strip_generation = 1, g_strip_count;
static std::unordered_map<StripKey, std::vector<NativeVertex>, StripKeyHash> g_strip_check;
static u32 g_cache_top, g_cache_cap;
static u64 g_cache_hits, g_cache_misses, g_cache_flushes, g_cache_check_bad;
static const bool g_cache_off = getenv("HP2_NOVCACHE") != nullptr, g_cache_checking = getenv("HP2_VCACHECHECK") != nullptr;

static StripSlot *strip_slot(const StripKey &key)
{
    if (g_strip_slots.empty()) g_strip_slots.resize(1u << 17);
    const size_t mask = g_strip_slots.size() - 1;
    for (size_t i = StripKeyHash()(key) >> 20 & mask;; i = (i + 1) & mask) {
        StripSlot &s = g_strip_slots[i];
        if (s.generation != g_strip_generation || s.key == key) return &s;
    }
}

static const CachedStrip *strip_find(const StripKey &key)
{
    const StripSlot *s = strip_slot(key);
    return s->generation == g_strip_generation ? &s->strip : nullptr;
}

static void strip_put(const StripKey &key, const CachedStrip &strip)
{
    if ((g_strip_count + 1) * 2 > g_strip_slots.size()) {       // half full: twice the room, the strips put again
        std::vector<StripSlot> old(g_strip_slots.empty() ? (size_t)1 << 16 : g_strip_slots.size() * 2);
        old.swap(g_strip_slots);
        for (const StripSlot &o : old)
            if (o.generation == g_strip_generation) *strip_slot(o.key) = o;
    }
    StripSlot *s = strip_slot(key);
    if (s->generation != g_strip_generation) g_strip_count++;
    *s = {key, strip, g_strip_generation};
}

// The key: two 64-bit hashes of the packet's bytes, each of every word. Each goes in four lanes, a lane a word of every four, so the
// multiplications of a step do not wait for one another (a packet is a multiple of 16 bytes); the lanes meet at the end.
static inline u64 strip_mix_a(u64 h, u64 w)
{
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    return h ^ h >> 29;
}
static inline u64 strip_mix_b(u64 h, u64 w)
{
    h = (h + w) * 0xBF58476D1CE4E5B9ull;
    return h ^ h >> 31;
}
static StripKey strip_key(u32 data, u32 qwords)
{
    const u8 *p = host(data);
    const size_t n = (size_t)qwords * 16;
    u64 a[4] = {0x9E3779B97F4A7C15ull ^ n, 0xD6E8FEB86659FD93ull, 0xA0761D6478BD642Full, 0xE7037ED1A0B428DBull};
    u64 b[4] = {0xC2B2AE3D27D4EB4Full + n, 0x8EBC6AF09C88C6E3ull, 0x589965CC75374CC3ull, 0x1D8E4E27C47D124Full};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 w[4];
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) a[k] = strip_mix_a(a[k], w[k]), b[k] = strip_mix_b(b[k], w[k]);
    }
    if (i < n) {
        u64 w[2];
        memcpy(w, p + i, 16);
        for (int k = 0; k < 2; k++) a[k] = strip_mix_a(a[k], w[k]), b[k] = strip_mix_b(b[k], w[k]);
    }
    u64 ha = a[0], hb = b[0];
    for (int k = 1; k < 4; k++) ha = strip_mix_a(ha, a[k]), hb = strip_mix_b(hb, b[k]);
    return {ha, hb};
}

// A strip by its address. The key above costs a read of the whole packet, every frame, and a heavy frame has 40 MB of packets; but nearly
// all of them are the track's models, which nothing writes once they are loaded. So a packet whose key came out the same a few frames
// running has its pages watched (memwatch.h), the key made once more under the watch, and from then on, while the pages' generations
// stand, its place in the cache is taken from here by the address alone. One slot an address: a packet that shares its slot with
// another seen as often just goes the long way. HP2_NOMEMWATCH: all of them do.
struct alignas(64) StripSeen {              // one line of the CPU's cache
    u32 data, qwords;
    StripKey key;
    CachedStrip strip;
    u64 stamp;                              // memwatch_stamp when the key was made under the watch
    u32 generation;                         // the cache's, when strip was its
    u8 same;                                // frames running the packet gave this key
    bool fast;
};
static std::vector<StripSeen> g_strip_seen;
static u64 g_seen_hits;
static inline StripSeen *strip_seen(u32 data)
{
    if (g_strip_seen.empty()) g_strip_seen.resize(1u << 18);
    // by the address itself, 128 bytes a slot over the 32 MB: a model's strips lie one after another in memory, so their slots do in
    // the table, and the CPU's own read-ahead sees the run. Two strips in one 128 bytes share a slot (the long way for one of them)
    return &g_strip_seen[data >> 7 & ((1u << 18) - 1)];
}

static void cache_flush()
{
    g_strip_generation++;
    g_strip_count = 0;
    g_strip_check.clear();
    g_cache_top = 0;
    g_cache_flushes++;
}

static void add_strip(u32 data, u32 qwords, u32 flags)
{
    const bool caching = g_cache_cap && !g_cache_off;
    StripKey key = {};
    const CachedStrip *hit = nullptr;
    StripSeen *seen = caching ? strip_seen(data) : nullptr;
    const bool known = seen && seen->fast && seen->data == data && seen->qwords == qwords && seen->generation == g_strip_generation &&
                       seen->stamp == memwatch_stamp(data, qwords * 16);
    if (known) {
        key = seen->key;
        hit = &seen->strip;
        g_seen_hits++;
    } else if (caching) {
        key = strip_key(data, qwords);
        hit = strip_find(key);
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
        clip_now();
        memcpy(d.clip, g_clip, sizeof d.clip);
        d.first = (u32)g_frame.indices.size();
        d.count = 0;
        d.indexed = true;
        set_draw_state(d);
        if (dumping())
            fprintf(g_hle_log, "dump draw %zu target %u tex %s flags %x blend %d fix %g tfx %d atest %d zw %d light %d mode %u a %d mmag %d mmin %d levels %d\n",
                    g_frame.draws.size(), g_draw_target, g_texture ? (const char *)host(g_texture + 8) : "-", flags, d.blend, d.blend_fix, d.tfx, d.alpha_test,
                    d.depth_write, g_light, g_mode, tri_n ? tri[0].a : -1, g_texture ? LB(g_texture + 0x6f) : -1, g_texture ? LB(g_texture + 0x70) : -1,
                    d.tex ? d.tex->levels : 0);
        if (dumping() && g_light >= 0 && (flags & 0x100)) {      // the light a reflecting strip has: colours, ambient, material
            const NativeLight &l = g_frame.lights[g_light];
            float f[22][4];
            memcpy(f, l.q, sizeof f);
            fprintf(g_hle_log, "dump light %d frame %u vertex colour %d %d %d:", g_light, g_frame_no, tri_n ? tri[0].r : -1, tri_n ? tri[0].g : -1, tri_n ? tri[0].b : -1);
            for (int q : {0, 1, 2, 3, 18, 19, 20, 21}) fprintf(g_hle_log, " [%d] %.4g %.4g %.4g %.4g", q, f[q][0], f[q][1], f[q][2], f[q][3]);
            fputc(10, g_hle_log);
            if (g_light_mat) {                      // eLightMaterial: its head, then the frontend's and the race's data (20 floats each)
                const u32 m = g_light_mat - (LW(g_pc_ctx->r[28].w[0] - 21424) ? 256 : 192);
                fprintf(g_hle_log, "dump material %08x head", m);
                for (u32 i = 0; i < 8; i++) fprintf(g_hle_log, " %08x", LW(m + i * 4));
                for (u32 blk : {0x20u, 0x70u}) {
                    fprintf(g_hle_log, " | +%x:", blk);
                    for (u32 i = 0; i < 20; i++) fprintf(g_hle_log, " %.4g", *(float *)host(m + blk + i * 4));
                }
                fputc(10, g_hle_log);
            }
        }
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
        strip_put(key, {base, tri_n, n});
        if (g_cache_checking) g_strip_check[key].assign(tri, tri + tri_n);
        g_cache_misses++;
    } else {
        // no cache, or no room in it this frame: the frame's own vertices
        base = g_frame.dyn_base + (u32)g_frame.vertices.size();
        g_frame.vertices.insert(g_frame.vertices.end(), tri, tri + tri_n);
    }
    // what the address is remembered by: the key that keeps coming out the same gets its pages watched
    if (seen && !known && (hit || (caching && base < g_cache_cap))) {
        const bool again = seen->data == data && seen->qwords == qwords && seen->key == key;
        seen->same = again ? (u8)std::min<u32>(seen->same + 1, 255) : 0;
        seen->data = data;
        seen->qwords = qwords;
        seen->key = key;
        seen->strip = {base, tri_n, n};
        seen->generation = g_strip_generation;
        seen->fast = false;
        if (seen->same >= 2 && memwatch_watch(data, qwords * 16)) {
            const u64 stamp = memwatch_stamp(data, qwords * 16);
            if (strip_key(data, qwords) == key) {
                seen->stamp = stamp;
                seen->fast = true;
            }
        }
    }
    {   // the strip's indices, one after another: room first (not cleared), then a loop the compiler can do several at a time
        u32 *out = g_frame.indices.grow(tri_n);
        for (u32 i = 0; i < tri_n; i++) out[i] = base + i;
    }
    d.count += tri_n;
    g_stat.tris += tri_n / 3;
    g_stat.strips++;
    g_stat.verts += n;
}

// ---- the lists -------------------------------------------------------------------------------
enum { EDMA_STRIP = 0, EDMA_SETTARGET = 1, EDMA_LIGHTMATERIAL = 2, EDMA_LIGHTCONTEXT = 3, EDMA_DIRECT = 4, EDMA_POLY3D = 5,
       EDMA_SET_MATRIX_PACK = 9, EDMA_SET_LOCAL_WORLD = 12 };

static u32 g_target, g_pack, g_local;
static const bool g_native_stats = getenv("HP2_NATIVESTATS") != nullptr;
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
static std::atomic<u32> g_dump_at{~0u};
void native_dump_next()
{
    g_dump_at = g_frame_no + 2;
    fprintf(g_hle_log, "native: F11, dumping frame %u\n", g_frame_no + 2);
}
bool native_dumping_now() { return g_dump_at == g_frame_no; }
static bool dumping_now();
// asked for at every new draw: the answer is a frame's, made once for it
static bool dumping()
{
    static u32 frame = ~0u;
    static bool now;
    if (frame != g_frame_no) frame = g_frame_no, now = dumping_now();
    return now;
}
static bool dumping_now()
{
    static const int at = getenv("HP2_NATIVEDUMP") ? atoi(getenv("HP2_NATIVEDUMP")) : -1;
    // HP2_NATIVEDUMPEVERY=<n>[,<from>]: every n-th frame from that one on
    static const char *every_env = getenv("HP2_NATIVEDUMPEVERY");
    static const int every = every_env ? atoi(every_env) : 0, from = every_env && strchr(every_env, ',') ? atoi(strchr(every_env, ',') + 1) : 0;
    if (every > 0 && (int)g_frame_no >= from && g_frame_no % every == 0) return true;
    return (at >= 0 && (int)g_frame_no == at) || g_dump_at == g_frame_no;
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
    {   // TEMP: HP2_MIRRORFXLOG=1, the particles in the mirror
        static const bool on = getenv("HP2_MIRRORFXLOG") != nullptr;
        if ((on || dumping()) && LW(g_target) == 7 && g_texture && strncmp((const char *)host(g_texture + 8), "RVM", 3)) {
            size_t n = g_frame.vertices.size() - first_vertex;
            float best = 0, z0 = 1e30f, z1 = -1e30f, cover = 0;
            for (size_t at = 0; at + 6 <= n; at += 6) {
                const NativeVertex *v = &g_frame.vertices[first_vertex + at];
                float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
                for (int i = 0; i < 6; i++) {
                    x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x), y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
                    z0 = std::min(z0, v[i].z), z1 = std::max(z1, v[i].z);
                }
                float cx0 = std::max(x0 - ox, 0.0f), cx1 = std::min(x1 - ox, w), cy0 = std::max(y0 - oy, 0.0f), cy1 = std::min(y1 - oy, h);
                float f = cx1 > cx0 && cy1 > cy0 ? (cx1 - cx0) * (cy1 - cy0) / (w * h) : 0;
                best = std::max(best, f);
                cover += f;
            }
            fprintf(g_hle_log, "mfx: frame %u %s sprites %zu biggest %.2f of the mirror, sum %.2f, z %g..%g, alpha %d\n", g_frame_no,
                    (const char *)host(g_texture + 8), n / 6, best, cover, z0, z1, n ? g_frame.vertices[first_vertex].a : -1);
        }
    }
    g_clip_new = true;
}

// The matrix a strip is drawn with, for the target, pack and local-world in force. Each of them comes as a node of its own and most
// objects change the last only, but a list has runs of such nodes with no strip between them (an object culled after its matrix was set,
// the other views), so the matrix is made when a strip is about to use it (clip_now), not at each node. g_clip_new is raised at the
// node as before: it is what starts a new draw.
static void update_clip()
{
    if (!g_target || !g_pack || !g_local) return;
    g_clip_stale = true;
    g_clip_new = true;
}

static void clip_now()
{
    if (!g_clip_stale) return;
    g_clip_stale = false;
    clip_matrix(g_clip, g_pack, g_local, LW(g_target + 0x1c) / 2, LW(g_target + 0x20) / 2, depth_scale(g_target));
}

void native_read_list(u32 list)
{
    g_stat.lists++;
    // A strip is known by its address from a table of 16 MB (strip_seen), a line of the CPU's cache a strip, and a heavy frame asks for
    // 65 thousand of them all over it: each was a wait for memory. So the line of the strip four nodes on is asked for now, and the
    // wait is over by the strip's turn. (RenderStrips makes a node a strip.)
    const bool by_address = g_cache_cap && !g_cache_off;
    u32 ahead = LW(list);
    const auto ask = [&] {
        if (!ahead || !by_address) return;
        const u32 t = LH(ahead + 4);
        if (t == EDMA_STRIP || t == EDMA_POLY3D) __builtin_prefetch(strip_seen(LW(ahead + 12)));
    };
    ask();
    for (int i = 0; i < 3 && ahead; i++) ahead = LW(ahead), ask();
    for (u32 node = LW(list); node; node = LW(node)) {
        if (ahead) ahead = LW(ahead), ask();
        u32 type = LH(node + 4), data = LW(node + 12);
        if (g_native_stats) g_nodes_seen[type & 15]++;
        switch (type) {
        case EDMA_SETTARGET:
            g_target = data;
            g_target_ptr = data;
            g_mode = LH(node + 6);
            if (dumping())                          // the target's GS registers, as the game sends them
                if (u32 block = LW(data + 0x7c + (g_mode & 3) * 4)) {
                    fprintf(g_hle_log, "dump target %u mode %u regs", LW(data), g_mode);
                    for (u32 q = 1; q < 14; q++) fprintf(g_hle_log, " %02x=%08x%08x", LB(block + q * 16 + 8), LW(block + q * 16 + 4), LW(block + q * 16));
                    fputc(10, g_hle_log);
                }
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
            if (g_native_stats && g_target && LW(g_target) < 16) {      // HP2_NATIVESTATS's numbers
                auto &t = g_by_target[LW(g_target)];
                t.strips += LH(node + 8);
                t.pack = g_pack;
                t.target = g_target;
            }
            if (type == EDMA_POLY3D && dumping()) {
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
                // The packets are all over the game's memory, and a heavy frame reads some 40 MB of them: the next strip's first lines and
                // the next node are asked for while this strip is worked on, so the memory's wait runs beside the work, not before it
                if (const u32 next = LW(node))
                    if (const u8 *p = host(next)) __builtin_prefetch(p);
                for (u32 i = 0; i < count; i++) {
                    if (i + 1 < count)
                        if (const u8 *p = host(LW(node + 12 + (i + 1) * 8))) {
                            __builtin_prefetch(p);
                            __builtin_prefetch(p + 64);
                            __builtin_prefetch(p + 128);
                            __builtin_prefetch(p + 192);
                        }
                    add_strip(LW(node + 12 + i * 8), LH(node + 10 + i * 8), LH(node + 6));
                }
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

// ---- the renderer's thread ----------------------------------------------------------------------------
// The lists are read on the game's thread, as they are handed over (native_read_list): they point all over the game's live data. What
// comes of them, the frame (NativeFrame: vertices, indices, draws, the textures decoded for it), is all the Vulkan side needs, and it
// reads nothing of the game's. So the finished frame goes to a thread that records and submits it while the game's thread is already at
// the next one. One frame in hand at most: the game's thread waits for the renderer's before it hands over another, and before it asks
// for the last frame's depth (the sun's flare). HP2_RENDERSYNC=1: drawn on the game's thread, as it was.
static NativeFrame g_out;                   // what the renderer's thread has: g_frame as it was at the hand-over
static std::mutex g_render_mx;
static std::condition_variable g_render_cv;
static bool g_render_busy, g_render_started;
static const bool g_render_sync = getenv("HP2_RENDERSYNC") != nullptr;
static u32 g_seen_epoch;                    // g_native_epoch as this thread last took note of it
void ee_float_mode_here();
void pc_heap_report(u32 frame);

static void render_thread()
{
    ee_float_mode_here();                   // the Vulkan side's own sums as they were on the game's thread: cut toward zero
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_render_mx);
            g_render_cv.wait(lock, [] { return g_render_busy; });
        }
        native_vk_draw(g_out);
        {
            std::lock_guard<std::mutex> lock(g_render_mx);
            g_render_busy = false;
        }
        g_render_cv.notify_all();
    }
}

void native_render_wait()
{
    if (!g_render_started) return;
    std::unique_lock<std::mutex> lock(g_render_mx);
    g_render_cv.wait(lock, [] { return !g_render_busy; });
}

// the frame to the renderer; g_frame is then the one before it, to be emptied and filled again
static void hand_over()
{
    if (g_render_sync) {
        native_vk_draw(g_frame);
        return;
    }
    native_render_wait();
    std::swap(g_frame, g_out);
    if (!g_render_started) {
        g_render_started = true;
        std::thread(render_thread).detach();
    }
    {
        std::lock_guard<std::mutex> lock(g_render_mx);
        g_render_busy = true;
    }
    g_render_cv.notify_all();
}

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
    g_frame.road_mode = g_frame.road_specular ? native_tex_mode(*g_frame.road_specular) : 0;
    // eDisplayFrame's ESTAGE_FECARS: in the frontend (CurrentGameFlowState 3) the game downloads pTextureInfoCarSelectEnvMap to VRAM 10240 every
    // frame and the cars' environment pass reads it, not the sphere map a race left behind
    u32 car_env = LW(0x330544) == 3 ? LW(0x32f6d8) : 0;      // CurrentGameFlowState (gp-21420), pTextureInfoCarSelectEnvMap (gp-25112)
    g_frame.env_override = car_env ? native_texture(car_env, g_frame_no) : nullptr;
    g_frame.env_mode = g_frame.env_override ? native_tex_mode(*g_frame.env_override) : 0;
    u32 env = pc_call(0x105848, 8);
    u32 env_info = env ? pc_call(0x105aa0, env) : 0;
    g_frame.env_linear = !env_info || LB(env_info + 0x6f) != 0;
    memcpy(g_frame.samples, host(0x2d7130), sizeof g_frame.samples);            // ReflectionSamples
    // The rects StuffSun asked for this frame: gp-24684 is this frame's queue by now (ExecuteRenderStageQueue swapped them)
    g_frame.depth_rects.clear();
    u32 stage_head = LW(g_pc_ctx->r[28].w[0] - 24684) + 2128;
    for (u32 n = LW(stage_head); n != stage_head && g_frame.depth_rects.size() < 16; n = LW(n))
        g_frame.depth_rects.push_back({(int)LH(n + 20), (int)LH(n + 22), (int)LH(n + 24), (int)LH(n + 26)});
    memcpy(g_frame.back_samples, host(0x2d9158), sizeof g_frame.back_samples);  // BackReflectionSamples
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
    resolve_wide_2d();
    window_cursor_frame();
    native_texture_take_uploads(g_frame.uploads);
    g_frame.number = g_frame_no;
    g_frame.dumping = native_dumping_now();
    g_frame.epoch = g_seen_epoch;
    hand_over();
    memwatch_frame();
    pc_heap_report(g_frame_no);             // HP2_HEAPLOG
    g_aspect_now = pc_aspect();
    g_frame.list_ms = 0;
    // the GPU's cache is not what this thread took it for (a frame did not reach it): this side of it emptied. The frames made
    // meanwhile are not drawn (NativeFrame::epoch).
    if (const u32 epoch = g_native_epoch.load(); epoch != g_seen_epoch) {
        cache_flush();
        g_seen_epoch = epoch;
    }
    // the next frame's cache: emptied before it can fill up within a frame
    const u32 cap = native_vk_cache_vertices();
    const u32 margin = std::min<u32>(1536 * 1024, cap / 2);
    if (cap != g_cache_cap || g_cache_top + margin > cap) {
        if (g_cache_top || cap != g_cache_cap) cache_flush();
        g_cache_cap = cap;
    }
    g_frame.dyn_base = g_cache_cap;
    g_frame.uploads.clear();
    g_frame.indices.clear();
    g_frame.fresh.clear();
    g_frame.fresh_runs.clear();
    if (g_frame_no % 600 == 0 && getenv("HP2_NATIVELOG")) {
        fprintf(g_hle_log, "native: vertex cache %u of %u vertices used, %llu hits, %llu new, %llu emptyings%s\n", g_cache_top, g_cache_cap,
                (unsigned long long)g_cache_hits, (unsigned long long)g_cache_misses, (unsigned long long)g_cache_flushes, g_cache_off ? " (off)" : "");
        u64 faults;
        u32 watched, busy;
        memwatch_stats(&faults, &watched, &busy);
        fprintf(g_hle_log, "native: strips by address: %llu of the hits; %u pages watched, %u left alone, %llu watches taken off by writes\n",
                (unsigned long long)g_seen_hits, watched, busy, (unsigned long long)faults);
        u64 tex_vouched, tex_summed, tex_bad;
        native_texture_watch_stats(&tex_vouched, &tex_summed, &tex_bad);
        fprintf(g_hle_log, "native: textures asked for: %llu known unchanged by their pages, %llu summed", (unsigned long long)tex_vouched, (unsigned long long)tex_summed);
        if (getenv("HP2_TEXWATCHCHECK")) fprintf(g_hle_log, "; texwatchcheck: %llu not what their sum says", (unsigned long long)tex_bad);
        fputc(10, g_hle_log);
        if (g_cache_checking) fprintf(g_hle_log, "native: vcachecheck: %llu hits not what they should be\n", (unsigned long long)g_cache_check_bad);
    }
    g_frame_no++;
    g_frame.vertices.clear();
    g_frame.draws.clear();
    g_frame.lights.clear();
    g_frame.fx.clear();
    g_packet_spans.clear();
    g_wide_2d.clear();
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
        native_render_wait();               // the last frame is the one handed over a moment ago: till it is with the GPU
        if (!words.empty() && native_vk_depth(r, words.data())) memcpy(host(LW(n + 8)), words.data(), words.size() * 4);
    }
}

// A Flailer poly's box: the polys are a list at +36 of a strip, their vertices' x and y at +0x10 and +0x14 of each 16 bytes
struct PolyBox {
    u32 poly;
    float x0, x1, y0, y1;
};
static PolyBox poly_box(u32 p)
{
    PolyBox b = {p, 1e9f, -1e9f, 1e9f, -1e9f};
    for (int i = 0; i < 4; i++) {
        float x, y;
        memcpy(&x, host(p + 0x10 + i * 16), 4);
        memcpy(&y, host(p + 0x14 + i * 16), 4);
        b.x0 = std::min(b.x0, x), b.x1 = std::max(b.x1, x);
        b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y);
    }
    return b;
}

// The lines of text on the screen: one message is often several objects, a word each ("LED" "A LAP" "+1000"), and each is a strip of
// its own. Narrowed one by one, the words of a line sat at different anchors (the screen's middle and its right edge) and came apart. So
// the polys of all strips, of every layer, are put together into lines first, as a frame's first RenderLayer comes: polys within
// 32 pixels across and touching up and down are one line, and every poly gets the middle of its line.
static std::unordered_map<u32, float> g_poly_line;

// a letter's quad: the HUD's fonts are well under 40 pixels a glyph
static bool glyph(const PolyBox &b) { return b.x1 - b.x0 <= 40 && b.y1 - b.y0 <= 40; }
static u32 g_lines_frame = ~0u;

static void find_lines(u32 manager)
{
    std::vector<PolyBox> boxes;
    for (int layer = 0; layer < 29; layer++) {
        u32 head = manager + layer * 8;
        int strips = 0;
        for (u32 st = LW(head); st != head && st && strips < 4096; st = LW(st), strips++) {
            u32 polys = st + 36;
            int n = 0;
            for (u32 p = LW(polys); p != polys && p && n < 4096; p = LW(p), n++) boxes.push_back(poly_box(p));
        }
    }
    // union-find over the boxes, sorted by x so that only neighbours across are looked at
    std::sort(boxes.begin(), boxes.end(), [](const PolyBox &a, const PolyBox &b) { return a.x0 < b.x0; });
    std::vector<int> up(boxes.size());
    for (size_t i = 0; i < up.size(); i++) up[i] = (int)i;
    auto root = [&](int i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    for (size_t i = 0; i < boxes.size(); i++)
        for (size_t j = i + 1; j < boxes.size() && boxes[j].x0 - boxes[i].x1 < 32; j++) {
            // a gap in x under 32 (or overlap), and in y no more than 4: the same line, or the line under it
            if (boxes[j].y0 > boxes[i].y1 + 4 || boxes[i].y0 > boxes[j].y1 + 4) continue;
            // only letters make a line: a backdrop, a bar or a panel is no word of one, and through it the lights under the mirror took the
            // right edge's anchor from the panel beside them
            if (!glyph(boxes[i]) || !glyph(boxes[j])) continue;
            up[root((int)j)] = root((int)i);
        }
    std::vector<float> lo(boxes.size(), 1e9f), hi(boxes.size(), -1e9f);
    for (size_t i = 0; i < boxes.size(); i++) {
        int r = root((int)i);
        lo[r] = std::min(lo[r], boxes[i].x0), hi[r] = std::max(hi[r], boxes[i].x1);
    }
    g_poly_line.clear();
    for (size_t i = 0; i < boxes.size(); i++) {
        if (!glyph(boxes[i])) continue;                     // the rest keeps its strip's own span, as before
        int r = root((int)i);
        g_poly_line[boxes[i].poly] = (lo[r] + hi[r]) / 2;
    }
}

void HOOK_RenderLayer__20FlailerRenderManageriP5eView(Ctx &c, void (*orig)(Ctx &))
{
    if (g_lines_frame != g_frame_no) {
        g_lines_frame = g_frame_no;
        if (g_frame.wide) find_lines(c.r[4].w[0]);
        else g_poly_line.clear();
    }
    orig(c);
}

void HOOK_Render__12FlailerStripP5eView(Ctx &c, void (*orig)(Ctx &))
{
    u32 strip = c.r[4].w[0], head = strip + 36;
    std::vector<StripSpan> polys;
    for (u32 p = LW(head); p != head && p; p = LW(p)) {
        PolyBox b = poly_box(p);
        auto line = g_poly_line.find(p);
        polys.push_back({b.x0, b.x1, line != g_poly_line.end() ? line->second : 1e9f});
    }
    std::sort(polys.begin(), polys.end(), [](const StripSpan &a, const StripSpan &b) { return a.x0 < b.x0; });
    g_strip_spans.clear();
    for (const StripSpan &s : polys) {
        // the gaps inside a text are well under this, the lights and the right panel are 60 pixels apart
        if (!g_strip_spans.empty() && s.x0 - g_strip_spans.back().x1 < 32) {
            StripSpan &last = g_strip_spans.back();
            last.x1 = std::max(last.x1, s.x1);
            if (last.whole > 1e8f) last.whole = s.whole;
        } else {
            g_strip_spans.push_back(s);
        }
    }
    for (StripSpan &s : g_strip_spans)
        if (s.whole > 1e8f) s.whole = (s.x0 + s.x1) / 2;   // a strip the lines did not see (drawn outside RenderLayer)
    orig(c);
    g_strip_spans.clear();
}

static u32 g_added_strips;                  // strips handed to the lists: HP2_LODLOG takes a car's share of them

void HOOK_AddData__12eDmaDataListUsPvUsUs(Ctx &c, void (*orig)(Ctx &))
{
    if ((c.r[5].w[0] & 0xffff) == EDMA_STRIP) g_added_strips++;
    if (!g_strip_spans.empty() && (c.r[5].w[0] & 0xffff) == EDMA_DIRECT) g_packet_spans[c.r[6].w[0]] = g_strip_spans;
    orig(c);
}

// Wider than the PS2's: the projection takes less of x for the same pixels, and the clip volume, which the game culls by, takes more.
// ---- eModel::RenderStrips (0x10dd58) ---------------------------------------------------------------
// The game's loop that hands a model's visible strips to the lists, a strip at a time: on a far view with every object at its most
// detailed model that is 65 thousand strips a frame, and as generated code (registers in memory, a call for each list entry) it was the
// heaviest function of the game's thread. Here it is as the port's own code, statement for statement what the game's does: the same
// reads, the same writes in the same order, the game's own functions called for everything that is not the loop itself (the textures'
// lists, the alpha keys and their depth, the blended copies). eDmaDataList's AddData, SetLocalWorldMatrix, SetLightContext and
// SetLightMaterial (0x104a20, 0x104b88, 0x104c18, 0x104bd0) are done in place, with their counters and frame memory as theirs.
// HP2_NORSNATIVE=1: the game's code. HP2_RSCHECK=<n>: every n-th call runs both on the same memory and compares all of it after
// (run it with HP2_NOMEMWATCH=1: the check writes every page back).
void F_0010c878(Ctx &c);                    // eModel::ApplyReplacementTextureTable
void F_00105838(Ctx &c);                    // eGetCurrentRenderTarget
void F_00107ca8(Ctx &c);                    // eGetCurrentView
void F_00116a68(Ctx &c);                    // elGetDefaultLightMaterial
void F_00104df0(Ctx &c);                    // eCreateActiveTexture(TextureInfo *)
void F_00105158(Ctx &c);                    // eNewAlphaSortKey
void F_0010cbf8(Ctx &c);                    // GetAABBZDepth(out, min, max, local world, view)
void F_00250288(Ctx &c);                    // bMemCpy
void F_0010d778(Ctx &c);                    // DoBlendingOnStrip(solid, strip, copy, matrix)

static u64 g_rs_alpha, g_rs_blend, g_rs_made, g_rs_replaced, g_rs_no_key;    // HP2_RSCHECK: the rarer ways through, so far
static inline void rs_arg(Ctx &c, int r, u32 v) { c.r[r].d[0] = (u64)(s64)(s32)v; }

// eDmaDataList::AddData, with what HOOK_AddData counts
static inline void rs_add_data(u32 gp, u32 list, u32 type, u32 data, u32 qwords, u32 parameter)
{
    if (type == EDMA_STRIP) g_added_strips++;
    const u32 pos = LW(gp - 19388), end = LW(gp - 19384), next = pos + 16;
    if (!((u64)(s64)(s32)next < (u64)(s64)(s32)end)) {       // FRAME_MALLOC's failure
        const u32 failed = LW(gp - 19368);
        SW(gp - 19372, 1);
        SW(gp - 19368, failed + 16);
        return;
    }
    SW(gp - 19388, next);
    const u32 tail = LW(list + 4);
    SH(pos + 4, (u16)type);
    SW(tail, pos);
    SH(pos + 6, (u16)parameter);
    SH(pos + 8, 1);
    SH(pos + 10, (u16)qwords);
    SW(pos + 12, data);
    SW(list + 4, pos);
    SW(pos, 0);
}

// SetLocalWorldMatrix, SetLightContext, SetLightMaterial: the list's field at `field`, the call counted at `stat` and the hit at +4
static inline void rs_set(u32 gp, u32 list, u32 field, u32 stat, u32 value, u32 type, u32 qwords, u32 parameter)
{
    const u32 calls = LW(stat), was = LW(list + field);
    SW(stat, calls + 1);
    if (was == value) {
        SW(stat + 4, LW(stat + 4) + 1);
        return;
    }
    SW(list + field, value);
    rs_add_data(gp, list, type, value, qwords, parameter);
}

static void render_strips_native(Ctx &c)
{
    const u32 self = c.r[4].w[0], local_world = c.r[5].w[0], light_context = c.r[6].w[0], flags = c.r[7].w[0], blend = c.r[8].w[0];
    const u32 gp = c.r[28].w[0], sp = c.r[29].w[0] - 256;
    const u64 ra = c.r[31].d[0];
    const u32 solid = LW(self + 12);
    if (!solid) {
        c.r[2].d[0] = 0;
        return;
    }
    rs_arg(c, 29, sp);
    u32 mask = 0;
    if (!LW(gp - 25040) || (flags & 1)) mask = 256;
    if (flags & 2) mask |= 128;
    if (!light_context) mask |= 448;
    if (LW(self + 16)) {
        g_rs_replaced++;
        rs_arg(c, 4, self);
        c.r[31].d[0] = 0x10de00;
        F_0010c878(c);
    }
    c.r[31].d[0] = 0x10de08;
    F_00105838(c);
    const u32 target = c.r[2].w[0], target_id = LW(target);
    const u32 opaque_off = (flags & 16) ? 1 : LW(target + 84);      // with it, a strip of an alpha texture is left out
    const u32 alpha_only = (flags & 8) ? 1 : 0;                     // with it, a strip of any other
    c.r[31].d[0] = 0x10de48;
    F_00107ca8(c);
    const u32 view_matrix = LW(c.r[2].w[0] + 68);
    c.r[31].d[0] = 0x10de54;
    F_00116a68(c);
    const u32 default_material = c.r[2].w[0];
    const u32 textures = LW(solid + 184);
    u32 run = 0x2d0c80, strip = LW(run), count = 0, last_active = 0;
    const u32 alpha_list = 0x2c94a8 + target_id * 12, list_off = (target_id << 5) + 16;
    while (strip) {
        const u32 info = LW(textures + ((u32)(s32)(s8)LB(strip) << 3) + 4);
        const u32 data = LW(strip + 8);
        const u32 strip_flags = LH(strip + 14), qwords = LH(strip + 12);
        const u32 left = (u32)LH(run + 6) - 1;
        const bool alpha = (s8)LB(info + 77) != 0;
        const u32 tex_bit = (s8)LB(info + 116) ? 4 : 0;
        const u32 pass = LH(run + 4);
        SH(run + 6, (u16)left);
        u32 next;
        if (left & 0xffff) {
            next = strip + 64;
        } else {
            run += 8;
            next = LW(run);
        }
        bool draw = !(strip_flags & 1024);
        if (opaque_off && alpha) draw = false;
        if (alpha_only && !alpha) draw = false;
        if (draw) {
            u32 active = LW(info + 152);
            if (!active) {
                g_rs_made++;
                rs_arg(c, 4, info);
                c.r[31].d[0] = 0x10df6c;
                F_00104df0(c);
                active = c.r[2].w[0];
            }
            u32 list = active + list_off;
            if (alpha) {
                c.r[31].d[0] = 0x10df84;
                F_00105158(c);
                const u32 key = c.r[2].w[0];
                if (key) {
                    g_rs_alpha++;
                    rs_arg(c, 4, sp);
                    rs_arg(c, 5, strip + 16);
                    rs_arg(c, 6, strip + 32);
                    rs_arg(c, 7, local_world);
                    rs_arg(c, 8, view_matrix);
                    c.r[31].d[0] = 0x10dfa4;
                    F_0010cbf8(c);
                    const u32 tail = LW(alpha_list + 4), keys = LW(alpha_list + 8), depth = LW(sp);
                    SW(tail, key);
                    SW(key + 12, depth);
                    SW(alpha_list + 8, keys + 1);
                    list = LW(key + 4);
                    SW(key + 8, active);
                    SW(key, 0);
                    SW(alpha_list + 4, key);
                } else {
                    g_rs_no_key++;
                    list = 0;
                }
            }
            if (list) {
                if (active != last_active || alpha) {
                    rs_set(gp, list, 16, 0x2c85f0, local_world, EDMA_SET_LOCAL_WORLD, 4, 840);
                    if (light_context) rs_set(gp, list, 12, 0x2c85e0, light_context, EDMA_LIGHTCONTEXT, 18, 860);
                    last_active = active;
                }
                u32 material = default_material;
                if (const u32 materials = LW(solid + 188)) {
                    const s32 index = (s8)LB(strip + 31);
                    if (index >= 0 && index < (s32)(s16)LH(solid + 174)) material = LW(materials + ((u32)index << 3) + 4);
                }
                rs_set(gp, list, 8, 0x2c85d0, material, EDMA_LIGHTMATERIAL, 4, 852);
                const u32 parameter = (strip_flags | tex_bit | (pass == 1 ? 512 : 0)) & ~mask;
                if (blend) {
                    // a copy of the strip in the frame's memory, blended there
                    const u32 bytes = qwords << 4, pos = LW(gp - 19388), end = LW(gp - 19384), top = pos + bytes;
                    if ((u64)(s64)(s32)top < (u64)(s64)(s32)end) {
                        g_rs_blend++;
                        SW(gp - 19388, top);
                        rs_arg(c, 4, pos);
                        rs_arg(c, 5, data);
                        rs_arg(c, 6, bytes);
                        c.r[31].d[0] = 0x10e0cc;
                        F_00250288(c);
                        rs_arg(c, 4, solid);
                        rs_arg(c, 5, strip);
                        rs_arg(c, 6, pos);
                        rs_arg(c, 7, blend);
                        c.r[31].d[0] = 0x10e0e0;
                        F_0010d778(c);
                        rs_add_data(gp, list, EDMA_STRIP, pos, qwords, parameter);
                    } else {
                        const u32 failed = LW(gp - 19368);
                        SW(gp - 19372, 1);
                        SW(gp - 19368, failed + bytes);
                    }
                } else {
                    rs_add_data(gp, list, EDMA_STRIP, data, qwords, parameter);
                }
            }
        }
        strip = next;
        count++;
        if ((s32)count > 999998) break;
    }
    rs_arg(c, 29, sp + 256);
    c.r[31].d[0] = ra;
    rs_arg(c, 2, count);
}

// Both on the same memory: the game's first, then everything put back and the port's, and the two outcomes compared, the game's 32 MB
// and the PC's memory past them whole. The dead stack below the frame is the one thing that may differ (the callees save other
// registers there). The game goes on with its own outcome.
extern bool g_eie;
static void render_strips_check(Ctx &c, void (*orig)(Ctx &))
{
    static std::vector<u8> before, after;
    static u8 spr_before[0x4000], spr_after[0x4000];
    static u64 checks, bad;
    const size_t total = 0x02000000 + (size_t)g_ext_size;
    before.resize(total);
    after.resize(total);
    const Ctx c0 = c;
    const u32 strips0 = g_added_strips, sp = c.r[29].w[0];
    const bool eie = g_eie;
    g_eie = false;                          // no interrupt's handler in between: it would be in one outcome and not the other
    memcpy(before.data(), g_ram, total);
    memcpy(spr_before, g_spr, sizeof spr_before);
    orig(c);
    const Ctx c1 = c;
    memcpy(after.data(), g_ram, total);
    memcpy(spr_after, g_spr, sizeof spr_after);
    memcpy(g_ram, before.data(), total);
    memcpy(g_spr, spr_before, sizeof spr_before);
    c = c0;
    g_added_strips = strips0;
    render_strips_native(c);
    checks++;
    // the stack's dead part: below the caller's sp, wherever the stack is
    const u8 *stack = host(sp);
    auto dead = [&](const u8 *p) { return stack && p < stack && p >= stack - 0x1800; };
    u32 differ = 0;
    auto compare = [&](const u8 *mine, const u8 *game, size_t n, u32 guest, const char *name) {
        if (!memcmp(mine, game, n)) return;
        for (size_t i = 0; i < n; i += 4) {
            if (!memcmp(mine + i, game + i, 4) || dead(mine + i)) continue;
            if (differ++ < 8) {
                u32 a, b;
                memcpy(&a, mine + i, 4), memcpy(&b, game + i, 4);
                fprintf(g_hle_log, "rscheck: %s %08x: the port's %08x, the game's %08x\n", name, guest + (u32)i, a, b);
            }
        }
    };
    compare(g_ram, after.data(), total, 0, "memory");
    compare(g_spr, spr_after, sizeof spr_after, 0x70000000, "scratchpad");
    bool regs = c.r[2].d[0] == c1.r[2].d[0];
    for (int r : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31}) regs = regs && c.r[r].d[0] == c1.r[r].d[0];
    if (!regs) {
        differ++;
        fprintf(g_hle_log, "rscheck: registers: the port's count %08x sp %08x, the game's %08x %08x\n", c.r[2].w[0], c.r[29].w[0], c1.r[2].w[0], c1.r[29].w[0]);
    }
    if (differ) bad++;
    if (differ || checks % 50 == 0)
        fprintf(g_hle_log, "rscheck: %llu calls compared, %llu not the same (this one: %u words, %u strips); so far %llu alpha strips, %llu blended, "
                           "%llu textures' lists made, %llu calls with replaced textures, %llu without a key\n",
                (unsigned long long)checks, (unsigned long long)bad, differ, c1.r[2].w[0], (unsigned long long)g_rs_alpha, (unsigned long long)g_rs_blend,
                (unsigned long long)g_rs_made, (unsigned long long)g_rs_replaced, (unsigned long long)g_rs_no_key),
            fflush(g_hle_log);
    memcpy(g_ram, after.data(), total);
    memcpy(g_spr, spr_after, sizeof spr_after);
    c = c1;
    g_eie = eie;
}

void HOOK_RenderStrips__6eModelP8bMatrix4P13eLightContextUiT1(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NORSNATIVE") != nullptr;
    static const int check = getenv("HP2_RSCHECK") ? atoi(getenv("HP2_RSCHECK")) : 0;
    if (off) {
        orig(c);
        return;
    }
    if (check > 0) {
        static u64 calls;
        if (++calls % (u64)check == 0) {
            render_strips_check(c, orig);
            return;
        }
    }
    render_strips_native(c);
}

void HOOK_CreateViewMatricies__FP5eViewP8bMatrix4T1T1T1T1T1fff(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    u32 view = c.r[4].w[0], target = view ? LW(view + 48) : 0, id = target ? LW(target) : 99;
    // The mirror's far plane. Its camera has 192 m (RearViewMirrorCameraMover::Update, 0x1590e4: near 0.75, far 192), the players' have
    // 10000, so the mirror lost everything past 192 m to the frustum whatever its size, and the draw distance changed nothing in it. The
    // far plane goes with the setting: the function's own farz (f13), which overrides the camera's when it is not 0. Camera: FarZ +128.
    if (g_cfg.draw_distance != 100 && view && LW(view) == 3 && c.f[13].u << 1 == 0 && LW(view + 56)) {
        float far_z;
        memcpy(&far_z, host(LW(view + 56) + 128), 4);
        far_z = far_z * (float)g_cfg.draw_distance / 100.0f;
        if (far_z > 0 && far_z < 10000.0f) memcpy(&c.f[13].u, &far_z, 4);
    }
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
static s32 g_think_seen = -1;               // the world's loop the count below was last made in
static s64 g_think_tick, g_think_prev;      // the clock in thirtieths of a second at that loop, and at the one before
long long native_think_state() { return g_think_tick; }
void native_think_restore(long long tick)
{
    g_think_tick = tick;
    g_think_seen = -1;                      // the next think makes the count anew from it, as the one after the snapshot did
}

// The player's steering at 60 frames. RealDriver::DoSteering moves the steering toward the stick by 9 a second times a factor that
// falls with the car's speed (1 down to 0.34), times the step, but never by less than 0.173 a step (the float at gp-31548, which
// nothing else reads). At the PS2's 30 steps a second that least step matters only above some 300 km/h; at 60 the steps are half as
// long and it is always the larger, so the steering turned in faster than made, by 15 % at low speed and 80 % at 250 km/h, the same
// at every speed. Here the least step is as long as the step is against a thirtieth of a second, which gives the same turn a second
// as at 30. Only at 60 frames and never for the attract demo (a PS2 recording); at 30 the function runs untouched.
void HOOK_DoSteering__10RealDriverf12JoystickPort(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rc = LW(gp - 22216);
    static const bool off = getenv("HP2_NOSTEERFIX") != nullptr;
    if (off || g_cfg.frame_rate != 60 || (rc && LW(rc) == 9)) {
        orig(c);
        return;
    }
    const u32 at = gp - 31548, was = LW(at);
    float least;
    memcpy(&least, &was, 4);
    const float scaled = least * (c.f[12].f * 30.0f);
    if (scaled < least) {
        u32 w;
        memcpy(&w, &scaled, 4);
        SW(at, w);
    }
    static bool told;
    if (!told && scaled < least) {
        told = true;
        fprintf(stderr, "steering: 60 frames, the least step %.4f for a step of %.5f s (%.4f at 30 frames)\n", scaled, c.f[12].f, least);
    }
    orig(c);
    SW(at, was);
}

void HOOK_Think__15AICarControllerf(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rc = LW(gp - 22216);
    // the attract demo (RCSTATE_REPLAY_DEMO_MODE) is a PS2 recording, its racers' thinking that of 30 frames; a replay made here thinks as it
    // was made
    if (g_cfg.frame_rate != 60 || (rc && LW(rc) == 9)) {
        orig(c);
        return;
    }
    const u32 world = LW(gp - 22120), car = LW(c.r[4].w[0]);
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
    // Whose turn: the thirtieths of a second the world's clock has passed since the frame before. That frame's count is kept
    // (g_think_tick), and a replay must go on from the count the race had there: a replay starts from a snapshot, and with the count
    // left from wherever the race had got to, its first step gave a racer a turn the recording had not given, or took one away. A
    // racer that thinks at another step drives a little differently, the world leaves the recording, and the game closes the replay.
    // So the count is kept with each snapshot of the race and put back when a replay starts from it (native_think_state,
    // native_think_restore; the snapshot hooks of pcplatform.cpp).
    if (real != g_think_seen) {
        g_think_seen = real;
        g_think_prev = g_think_tick;
        g_think_tick = (s64)(wt * 30.0f);
    }
    const s64 tick = g_think_tick, prev = g_think_prev;
    bool turn = false;
    if (tick < prev) turn = tick % 8 == me;             // the clock went back: another race
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

// TEMP (HP2_LODLOG=1): what is still drawn below its best model. Scenery: the kinds with no first model, which go by their size as on the
// PS2 (each kind once, and the counts every 600 frames). Cars: every 20th frame each car of the player's view, with its size on the screen
// (the game's own GetPixelSize for a sphere of 3), the strips its Render handed over and the two Force tweakables as Render found them.
static bool max_detail_now() { return g_lod_flip_now >= 0 ? g_lod_flip_now != 0 : g_cfg.max_detail != 0; }
static const bool g_lod_log = getenv("HP2_LODLOG") != nullptr;
static u64 g_lod_scenery, g_lod_scenery_bare;

// A car under 18 pixels of the GS's is drawn without its reflections: CarRenderInfo::Render (0x121258) hands eModel::Render the flag 1,
// which RenderStrips (0x10ddac) turns into the mask 0x100, the sphere map's pass. Those are the PS2's pixels: at 1080 lines such a car is 43
// high, and it goes dull in plain sight. With Level of Detail at Maximum the flag is taken off again, unless the caller of Render asked
// for it itself (its own flags, the tenth argument).
void F_00107600(Ctx &c);
static bool g_in_car_render, g_car_asked_plain;
static bool g_car_size_next;                // the next GetPixelSize is CarRenderInfo::Render's own, of the car

// A car is not drawn under 4 pixels of the GS's (0x120f90). The draw distance takes the size as larger for the players' views, as it does
// for the scenery, so a car shows as far as the things around it. Its levels of detail are forced or go the better way with it.
void HOOK_GetPixelSize__5eViewPC8bVector3f(Ctx &c, void (*orig)(Ctx &))
{
    const bool car = g_car_size_next;
    g_car_size_next = false;
    const u32 view = c.r[4].w[0];
    orig(c);
    if (!car || g_cfg.draw_distance == 100 || !view) return;
    const u32 id = LW(view);
    if (id < 1 || id > 3) return;
    c.r[2].d[0] = (u64)(s64)((s64)(s32)c.r[2].w[0] * g_cfg.draw_distance / 100);
}
static u64 g_lod_kept_reflections;

void HOOK_Render__6eModelP8bMatrix4P13eLightContextUi(Ctx &c, void (*orig)(Ctx &))
{
    if (g_in_car_render && !g_car_asked_plain && (c.r[7].w[0] & 1) && max_detail_now()) {
        c.r[7].d[0] = c.r[7].w[0] & ~1u;
        g_lod_kept_reflections++;
    }
    orig(c);
}

void HOOK_Render__13CarRenderInfoP5eViewPC8bVector3PC8bMatrix4P8bMatrix4T4Uii(Ctx &c, void (*orig)(Ctx &))
{
    const bool was_in = g_in_car_render, was_plain = g_car_asked_plain;
    g_in_car_render = true;
    g_car_asked_plain = (c.r[10].w[0] & 1) != 0;
    struct Leave {
        bool in, plain;
        ~Leave() { g_in_car_render = in, g_car_asked_plain = plain; }
    } leave{was_in, was_plain};
    if (!g_lod_log) {
        g_car_size_next = true;
        orig(c);
        g_car_size_next = false;
        return;
    }
    const u32 info = c.r[4].w[0], view = c.r[5].w[0], pos = c.r[6].w[0];
    const u32 id = view ? LW(view) : 0, type = LW(info + 8), car = LW(info);
    const s32 force_car = (s32)LW(0x32fc60), force_tire = (s32)LW(0x32fc64);
    int size = -1;
    if (id == 1 && g_frame_no % 20 == 0) {
        Ctx t = c;
        t.r[4].d[0] = view;
        t.r[5].d[0] = pos;
        t.f[12].f = 3.0f;
        g_car_size_next = false;
        F_00107600(t);                      // through the hook above, as any other caller's: the game's own size
        size = (int)t.r[2].w[0];
    }
    const u32 before = g_added_strips;
    g_car_size_next = true;
    orig(c);
    g_car_size_next = false;
    if (size >= 0 || (id == 1 && (force_car != 0 || force_tire != 0))) {
        float p[3];
        memcpy(p, host(pos), 12);
        fprintf(g_hle_log, "lodlog: car frame %u info %08x type %08x car %08x size %d strips %u force %d %d kept %llu at %.0f %.0f %.0f\n", g_frame_no, info, type, car, size,
                g_added_strips - before, force_car, force_tire, (unsigned long long)g_lod_kept_reflections, p[0], p[1], p[2]);
    }
}

// ---- ScenerySectionHeader::DrawAScenery (0x14fe18) ---------------------------------------------------
// Called for every object of the scenery in sight: is it large enough on the screen, which of its models, and its matrix into the
// frame's memory. As generated code it was a few per cent of the game's thread, and twice that again in the EE's exact square root and
// division it makes for the size in pixels, both of the slow kind (a table of 2 MB, the divider's 25 steps).
// Here: the same statements for the vectors (the same VU0 and FPU helpers), the port's own code for the rest, and the size in pixels
// by das_pixels below. HP2_NODASNATIVE=1: the game's code. HP2_DASCHECK=<n>: every n-th call runs both on the same memory and
// compares it all (with HP2_NOMEMWATCH=1), and every size in pixels is made both ways and compared.
void F_0010ccb0(Ctx &c);                    // ClassifyAABB(min, max, matrix)
void F_0014fda8(Ctx &c);                    // CreateFacadeMatrix(out, position, camera)

static inline s32 das_cvt(float v) { return (s32)(v >= 2147483647.0f ? 0x7FFFFFFFu : v <= -2147483648.0f ? 0x80000000u : (u32)(s32)v); }
static inline bool das_normal(float f) { return (ee_bits(f) >> 23 & 255) - 1 < 254; }
static u64 g_das_quick, g_das_exact, g_das_wrong;

// The size as the game has it: (int)(h * reach / (sqrt(len2) - reach)), or (int)h when the object is nearer than twice its reach, with
// the EE's root, subtraction, multiplication and division.
static s32 das_pixels_exact(float len2, float reach, float h)
{
    const float away = ee_sub(ee_sqrt(len2), reach);
    return das_cvt(EE_LT(reach, away) ? ee_div(ee_mul(reach, h), away) : h);
}

// The same number without the EE's root and divider. Each of the two gives the host's result cut toward zero or one unit in the last
// place more (eefloat.h), so there are at most four values the EE's size can be, all cheap; the size is a whole number, and unless one
// of them falls on the other side of a whole number (once in tens of thousands) they all say the same. Then that is the answer, and
// else the exact way is taken.
static s32 das_pixels(float len2, float reach, float h)
{
    static const bool verify = getenv("HP2_DASCHECK") != nullptr;
    s32 quick = 0;
    bool sure = das_normal(len2) && das_normal(reach) && das_normal(h);
    if (sure) {
        const float root = std::sqrt(std::fabs(len2));     // this thread rounds toward zero
        bool first = true;
        auto take = [&](s32 v) {
            if (first) quick = v, first = false;
            else if (v != quick) sure = false;
        };
        for (u32 up = 0; up < 2 && sure; up++) {
            const float away = ee_sub(ee_float(ee_bits(root) + up), reach);
            if (!EE_LT(reach, away)) {
                take(das_cvt(h));
                continue;
            }
            const float top = ee_mul(reach, h);
            if (!das_normal(away) || !das_normal(top)) {
                sure = false;
                break;
            }
            const float q = top / away;
            if (!das_normal(q) || !das_normal(ee_float(ee_bits(q) + 1))) {
                sure = false;
                break;
            }
            take(das_cvt(q));
            take(das_cvt(ee_float(ee_bits(q) + 1)));
        }
    }
    if (sure && !verify) {
        g_das_quick++;
        return quick;
    }
    const s32 exact = das_pixels_exact(len2, reach, h);
    if (sure) {
        g_das_quick++;
        if (exact != quick && g_das_wrong++ < 20)
            fprintf(g_hle_log, "dascheck: size %d, the exact way %d (len2 %08x reach %08x h %08x)\n", quick, exact, ee_bits(len2), ee_bits(reach), ee_bits(h));
    } else {
        g_das_exact++;
    }
    return exact;
}

static void draw_a_scenery_native(Ctx &c)
{
    const u32 header = c.r[4].w[0], index = c.r[5].w[0], cull = c.r[6].w[0];
    const u32 gp = c.r[28].w[0], sp = c.r[29].w[0] - 128;
    const u64 ra = c.r[31].d[0];
    u32 state = c.r[7].w[0];
    const bool classify = c.r[7].d[0] == 1;
    const u32 inst = LW(header + 28) + index * 48;
    rs_arg(c, 29, sp);
    auto leave = [&] {
        rs_arg(c, 29, sp + 128);
        c.r[31].d[0] = ra;
    };
    if (classify) {
        // the object's box against the view: 1 partly in sight, 2 wholly, 0 not
        for (u32 i = 0; i < 3; i++) {
            const float lo = (float)(s32)(s16)LH(inst + i * 2), hi = (float)(s32)(s16)LH(inst + 6 + i * 2);
            SW(sp + i * 4, ee_bits(lo));
            SW(sp + 16 + i * 4, ee_bits(hi));
        }
        rs_arg(c, 4, sp);
        rs_arg(c, 5, sp + 16);
        rs_arg(c, 6, cull + 64);
        c.r[31].d[0] = 0x14fee0;
        F_0010ccb0(c);
        state = c.r[2].w[0];
        if (!c.r[2].d[0]) return leave();
    }
    const u32 kind = LW(header + 20) + (u32)((s32)(s16)LH(inst + 12) * 40);
    const u32 flags = (u32)(s32)(s16)LH(inst + 14);
    if (flags & LW(cull + 132) & 255) return leave();
    // the way from the camera to the object, along the view and in length: the game's statements
    op_lqc2(c, 1, cull + 160);
    op_lqc2(c, 3, inst + 16);
    c.f[1].u = LW(kind + 36);
    VU_vsub(c, 0x4be118ecu);
    c.f[0].u = 0x40800000u;
    op_lqc2(c, 2, cull + 176);
    SQ(sp, c.vf[3].q);
    FPU_add_s(c, 2, 1, 0);
    op_lqc2(c, 3, sp);
    VU_vmul(c, 0x4bc218aau);
    VU_vaddy(c, 0x4b021041u);
    VU_vaddz(c, 0x4b020842u);
    FPU_neg_s(c, 1, 2, 0);
    c.f[0].u = ee_bits(c.vf[1].f[0]);
    FPU_c_lt_s(c, 0, 0, 1);
    s32 pixels = 0;
    if (!(c.fcr31 & FCR31_C)) {
        VU_vmul(c, 0x4bc3186au);
        VU_vaddy(c, 0x4b010881u);
        VU_vaddz(c, 0x4b011082u);
        pixels = das_pixels(c.vf[2].f[0], ee_float(c.f[2].u), ee_float(LW(cull + 192)));
    }
    if (pixels < 2) return leave();
    // the first of the three models the object is large enough for, and has
    u32 model = 0;
    s32 lod = -1;
    if (!(pixels < (s32)(s16)LH(kind + 12)) && LW(kind + 20)) {
        model = LW(kind + 20);
        lod = 0;
    } else {
        for (s32 i = 1; i < 3; i++) {
            if (pixels < (s32)(s16)LH(kind + 12 + (u32)i * 2)) continue;
            const u32 m = LW(kind + 20 + (u32)i * 4);
            if (!m) continue;
            model = m;
            lod = i;
            break;
        }
    }
    if (!model) return leave();
    auto less = [](u32 a, u32 b) { return (u64)(s64)(s32)a < (u64)(s64)(s32)b; };
    if (LH(inst + 14) & 512) {              // drawn where it stands, without a matrix of its own
        const u32 slot = LW(cull + 140);
        if (less(slot, LW(cull + 144))) {
            SW(cull + 140, slot + 8);
            SW(slot, model + state);
            SW(slot + 4, 0);
        }
        return leave();
    }
    const u32 pos = LW(gp - 19388), top = pos + 64;
    if (!less(top, LW(gp - 19384))) {       // FRAME_MALLOC's failure
        const u32 failed = LW(gp - 19368);
        SW(gp - 19372, 1);
        SW(gp - 19368, failed + 64);
        return leave();
    }
    SW(gp - 19388, top);
    const u32 matrix = pos;
    if ((s8)LB(kind + (u32)lod + 32)) {
        op_lqc2(c, 1, inst + 16);
        SQ(sp, c.vf[1].q);
        rs_arg(c, 4, matrix);
        rs_arg(c, 5, sp);
        rs_arg(c, 6, cull + 160);
        c.r[31].d[0] = 0x1500d0;
        F_0014fda8(c);
    } else {
        const float unit = ee_float(0x38800000u);       // 2^-14
        for (u32 row = 0; row < 3; row++) {
            SW(matrix + row * 16 + 12, 0);
            for (u32 col = 0; col < 3; col++)
                SW(matrix + row * 16 + col * 4, ee_bits(ee_mul((float)(s32)(s16)LH(inst + 28 + row * 6 + col * 2), unit)));
        }
    }
    op_lqc2(c, 1, inst + 16);
    SQ(matrix + 48, c.vf[1].q);
    SW(matrix + 60, 0x3f800000u);
    if ((u32)(s32)(s16)LH(inst + 14) & LW(cull + 132) & 256) SW(matrix + 56, ee_bits(ee_add(ee_float(LW(matrix + 56)), 2.0f)));
    const u32 slot = LW(cull + 140);
    if (less(slot, LW(cull + 144))) {
        SW(cull + 140, slot + 8);
        SW(slot + 4, matrix);
        SW(slot, model + state);
    }
    leave();
}

// Both ways on the same memory, as render_strips_check: the game's code first, everything put back, the port's, and the game's 32 MB,
// the PC's memory past them and the scratchpad compared whole but for the dead stack under the frame.
static void native_ab_check(Ctx &c, void (*orig)(Ctx &), void (*mine)(Ctx &), const char *name)
{
    static std::vector<u8> before, after;
    static u8 spr_before[0x4000], spr_after[0x4000];
    static u64 checks, bad;
    const size_t total = 0x02000000 + (size_t)g_ext_size;
    before.resize(total);
    after.resize(total);
    const Ctx c0 = c;
    const u32 sp = c.r[29].w[0];
    const bool eie = g_eie;
    g_eie = false;
    memcpy(before.data(), g_ram, total);
    memcpy(spr_before, g_spr, sizeof spr_before);
    orig(c);
    const Ctx c1 = c;
    memcpy(after.data(), g_ram, total);
    memcpy(spr_after, g_spr, sizeof spr_after);
    memcpy(g_ram, before.data(), total);
    memcpy(g_spr, spr_before, sizeof spr_before);
    c = c0;
    mine(c);
    checks++;
    const u8 *stack = host(sp);
    auto dead = [&](const u8 *p) { return stack && p < stack && p >= stack - 0x1800; };
    u32 differ = 0;
    auto compare = [&](const u8 *got, const u8 *game, size_t n, u32 guest, const char *what) {
        if (!memcmp(got, game, n)) return;
        for (size_t i = 0; i < n; i += 4) {
            if (!memcmp(got + i, game + i, 4) || dead(got + i)) continue;
            if (differ++ < 8) {
                u32 a, b;
                memcpy(&a, got + i, 4), memcpy(&b, game + i, 4);
                fprintf(g_hle_log, "abcheck: %s: %s %08x: the port's %08x, the game's %08x\n", name, what, guest + (u32)i, a, b);
            }
        }
    };
    compare(g_ram, after.data(), total, 0, "memory");
    compare(g_spr, spr_after, sizeof spr_after, 0x70000000, "scratchpad");
    for (int r : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
        if (c.r[r].d[0] != c1.r[r].d[0]) {
            differ++;
            fprintf(g_hle_log, "abcheck: %s: register %d: the port's %08x, the game's %08x\n", name, r, c.r[r].w[0], c1.r[r].w[0]);
        }
    if (differ) bad++;
    if (differ || checks % 200 == 0)
        fprintf(g_hle_log, "abcheck: %s: %llu calls compared, %llu not the same; sizes in pixels: %llu the quick way, %llu the exact, %llu wrong\n", name,
                (unsigned long long)checks, (unsigned long long)bad, (unsigned long long)g_das_quick, (unsigned long long)g_das_exact,
                (unsigned long long)g_das_wrong),
            fflush(g_hle_log);
    memcpy(g_ram, after.data(), total);
    memcpy(g_spr, spr_after, sizeof spr_after);
    c = c1;
    g_eie = eie;
}

static void draw_a_scenery(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NODASNATIVE") != nullptr;
    static const int check = getenv("HP2_DASCHECK") ? atoi(getenv("HP2_DASCHECK")) : 0;
    if (off) {
        orig(c);
        return;
    }
    if (check > 0) {
        static u64 calls;
        if (!calls) {
            // the quick size against the exact one on numbers of every kind, the near ones (the object almost at its reach) most of all:
            // das_pixels compares the two itself and counts
            u32 seed = 12345;
            auto rnd = [&] { return seed = seed * 1664525u + 1013904223u; };
            auto in = [&](float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) * (1.0f / 16777216.0f); };
            const u64 quick0 = g_das_quick, exact0 = g_das_exact, wrong0 = g_das_wrong;
            for (int i = 0; i < 6000000; i++) {
                const float reach = in(0.3f, 400.0f), h = in(50.0f, 6000.0f);
                const int kind = i % 4;
                const float away = kind == 0 ? in(0.0f, 4000.0f) : kind == 1 ? reach * in(0.99f, 1.01f) : kind == 2 ? reach * in(1.0f, 3.0f) : in(0.0f, 30.0f);
                const float dist = reach + away;
                das_pixels(dist * dist, reach, h);
            }
            fprintf(g_hle_log, "dascheck: 6000000 made-up sizes: %llu the quick way, %llu the exact, %llu wrong\n", (unsigned long long)(g_das_quick - quick0),
                    (unsigned long long)(g_das_exact - exact0), (unsigned long long)(g_das_wrong - wrong0));
            fflush(g_hle_log);
        }
        if (++calls % (u64)check == 0) {
            native_ab_check(c, orig, draw_a_scenery_native, "DrawAScenery");
            return;
        }
    }
    draw_a_scenery_native(c);
}

void HOOK_DrawAScenery__20ScenerySectionHeaderiP15SceneryCullInfoi(Ctx &c, void (*orig)(Ctx &))
{
    if (!max_detail_now()) {
        draw_a_scenery(c, orig);
        return;
    }
    const u32 header = c.r[4].w[0], index = c.r[5].w[0];
    const u32 instances = LW(header + 28), infos = LW(header + 20);
    const u32 info = instances && infos ? infos + 40 * (u32)LH(instances + 48 * index + 12) : 0;
    if (g_lod_log) {
        g_lod_scenery++;
        if (info && !LW(info + 20)) {
            g_lod_scenery_bare++;
            static std::vector<u32> told;
            if (std::find(told.begin(), told.end(), info) == told.end() && told.size() < 4000) {
                told.push_back(info);
                fprintf(g_hle_log, "lodlog: scenery kind %08x section %u has no first model: names %08x %08x %08x, far clips %d %d %d, models %08x %08x %08x\n", info,
                        LW(header + 8), LW(info), LW(info + 4), LW(info + 8), (s16)LH(info + 12), (s16)LH(info + 14), (s16)LH(info + 16), LW(info + 20),
                        LW(info + 24), LW(info + 28));
            }
        }
        if (g_lod_scenery % 200000 == 0)
            fprintf(g_hle_log, "lodlog: scenery frame %u: %llu drawn so far, %llu of them of a kind with no first model\n", g_frame_no,
                    (unsigned long long)g_lod_scenery, (unsigned long long)g_lod_scenery_bare);
    }
    if (!info || !LW(info + 20)) {
        draw_a_scenery(c, orig);
        return;
    }
    const u16 was = LH(info + 12);
    SH(info + 12, 0);
    draw_a_scenery(c, orig);
    SH(info + 12, was);
}

// The cars' reflections (Graphics, Reflections). The game draws the six views round the car by a table: a row of six words a phase at
// 0x2D6FD0, the frame's number modulo 4 picks the row with one view on the screen (the six rows from the table's start) and modulo 6 on
// a split screen (the six rows 144 bytes on); eEnvMap::UpdateCameras (0x1175c8) makes a view active where its word is above 0. The
// game's rows: ahead and below every frame with one of the other four in turn, and one view a frame on a split screen. Advanced
// and High: all six in every row. Our fold takes the views a frame drew, so nothing else is to change.
static void reflections_schedule()
{
    static const u32 table = 0x2d6fd0;
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    static u32 game[72];
    static int applied = -1;
    if (applied < 0)
        for (u32 i = 0; i < 72; i++) game[i] = LW(table + i * 4);
    const int want = g_cfg.reflections && !exact ? 1 : 0;
    if (want == applied) return;
    for (u32 i = 0; i < 72; i++) SW(table + i * 4, want ? 1 : game[i]);
    applied = want;
}

// ForceCarLOD, ForceTireLOD (carrender.c): -1 the game's choice by the car's size, else that level. The car screens set and put them back
// themselves, around their own drawing.
static void car_detail()
{
    static bool ours;
    const u32 car = 0x32fc60, tire = 0x32fc64;
    static const bool flip = getenv("HP2_LODFLIP") != nullptr;
    if (flip) g_lod_flip_now = (int)(g_frame_no / 60 % 2);
    // 0x32fc30 (gp-0x5cc0), read only by CarRenderInfo::Render (0x121acc): not zero, and a car that renders its driver does so at any size,
    // not only over 50, 75 or 100 pixels of the GS's. The driver's own level of detail still goes by the size.
    SW(0x32fc30, max_detail_now() ? 1 : 0);
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

// ---- the scenery's draw table ------------------------------------------------------------------
// What the culling keeps of the scenery goes into one table for all the views, SceneryDrawInfoTable at 0x2f0ca8 up to 0x2f8fe8: 4200
// entries of 8 bytes. DoCulling hands it to the views in turn, the player's first, the mirror's after; DrawAScenery adds nothing once a
// view's pCurrentDrawInfo (+140 of its SceneryCullInfo) reaches pTopDrawInfo (+144). The PS2's distance fitted. The draw distance and the
// most detailed models let the player's view take all of it in the thicker places, and the mirror kept only its sky (which is not
// scenery): no road, no objects. So the first view of a frame starts the table again in the PC's memory past 32 MB, with room for a
// million entries, and each next view goes on where the last stopped, as in DoCulling; StuffScenery reads each view's own first..current.
static const u32 kOldDrawTable = 0x2f0ca8, kDrawTableOff = 0x0C000000, kDrawTableEntries = 1u << 20;
static u32 g_draw_table_peak;

void HOOK_CullView__20GrandSceneryCullInfoP15SceneryCullInfo(Ctx &c, void (*orig)(Ctx &))
{
    static const bool ready = ext_commit(kDrawTableOff, kDrawTableEntries * 8);
    const u32 cull = c.r[5].w[0], table = EXT_BASE + kDrawTableOff, end = table + kDrawTableEntries * 8;
    if (ready) {
        if (LW(cull + 140) == kOldDrawTable) {              // the frame's first view
            SW(cull + 136, table);
            SW(cull + 140, table);
        }
        const u32 cur = LW(cull + 140);
        if (cur >= table && cur < end) SW(cull + 144, end);
    }
    const u32 before_cur = LW(cull + 140);
    orig(c);
    const u32 cur = LW(cull + 140);
    {   // TEMP: HP2_VIEWLOG=1, each view's planes and what the culling kept for it
        static const bool on = getenv("HP2_VIEWLOG") != nullptr;
        const u32 view = LW(cull + 128);
        if (on && view && g_frame_no % 120 == 0) {
            float h, hi, nz, fz, fov;
            u32 w;
            w = LW(view + 8), memcpy(&h, &w, 4);
            w = LW(cull + 192), memcpy(&hi, &w, 4);
            w = LW(view + 12), memcpy(&nz, &w, 4);
            w = LW(view + 16), memcpy(&fz, &w, 4);
            w = LW(view + 24), memcpy(&fov, &w, 4);
            fprintf(g_hle_log, "view: frame %u id %u H %g (info %g) near %g far %g fov %g kept %u flags %x\n", g_frame_no, LW(view), h, hi, nz, fz, fov,
                    (cur - before_cur) / 8, LW(cull + 196));
        }
    }
    if (cur >= table && cur <= end) {
        const u32 used = (cur - table) / 8;
        if (used > g_draw_table_peak) g_draw_table_peak = used;
        if (cur == end) {
            static bool told;
            if (!told) fprintf(g_hle_log, "native: the scenery's draw table is full (%u entries)\n", kDrawTableEntries);
            told = true;
        }
    }
}

// ---- the frame malloc buffers ---------------------------------------------------------------
// eDmaDataList::AddData and the rest of a frame's lists take their nodes from one of two halves (bMalloc'd, 1.6 MB each on the PS2);
// when a half is full, AddData adds nothing (FrameMallocFailed). The draw distance and the most detailed models take a race to 1.3 MB of
// it at 200 %, more at 400 %. So the halves live in the PC's memory past 32 MB (ps2.h): 64 MB each, and a frame that still ran out
// doubles them for the next ones, up to 96 MB each. A half grows in place (each has its 96 MB of addresses), so what the other half's
// frame holds does not move. The last 32 MB of that memory are the scenery's draw table (HOOK_CullView).
static const u32 kFrameHalfStart = 64u << 20, kFrameHalfMax = 96u << 20;
static bool g_frame_ext;

static bool frame_halves(u32 size)
{
    return ext_commit(0, size) && ext_commit(kFrameHalfMax, size);
}

void HOOK_eAllocateFrameMallocBuffers__FUi(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], wanted = c.r[4].w[0];
    if (!frame_halves(kFrameHalfStart)) {
        fprintf(g_hle_log, "native: no memory past 32 MB, the frame buffers are the game's (%u bytes)\n", wanted);
        orig(c);
        return;
    }
    const u32 a = EXT_BASE, b = EXT_BASE + kFrameHalfMax;
    SW(gp - 19320, a);                                      // FrameMemoryBuffer[0]
    SW(gp - 19316, b);                                      // FrameMemoryBuffer[1]
    SW(gp - 19392, kFrameHalfStart);                        // FrameMemoryBufferSize
    SW(gp - 19388, a);                                      // CurrentBufferPos
    SW(gp - 19384, a + kFrameHalfStart);                    // CurrentBufferEnd
    SW(gp - 19376, 0);                                      // FrameMallocAllocNum
    g_frame_ext = true;
    fprintf(g_hle_log, "native: frame buffers 2 x %u MB past 32 MB (the game asked for %u bytes)\n", kFrameHalfStart >> 20, wanted);
}

void HOOK_eFreeFrameMallocBuffers__Fv(Ctx &c, void (*orig)(Ctx &))
{
    if (!g_frame_ext) {
        orig(c);
        return;
    }
    const u32 gp = c.r[28].w[0];
    for (int off : {19320, 19316, 19392, 19388, 19384, 19376}) SW(gp - (u32)off, 0);
    g_frame_ext = false;                                    // the memory stays committed for the next allocation
}

void HOOK_eSwapFrameMallocBuffers__Fv(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0];
    if (g_frame_ext && LW(gp - 19372)) {                    // FrameMallocFailed: this frame lost some of its lists
        const u32 size = LW(gp - 19392), short_by = LW(gp - 19368);
        u32 grown = size;
        while (grown < kFrameHalfMax && grown < size + short_by * 2) grown = std::min(grown * 2, kFrameHalfMax);
        if (grown > size && frame_halves(grown)) {
            SW(gp - 19392, grown);
            fprintf(g_hle_log, "native: a frame's lists ran %u bytes past %u MB, the frame buffers are now 2 x %u MB\n", short_by, size >> 20,
                    grown >> 20);
        } else {
            static bool told;
            if (!told) fprintf(g_hle_log, "native: a frame's lists ran %u bytes past %u MB, the frame buffers cannot grow\n", short_by, size >> 20);
            told = true;
        }
    }
    orig(c);
}

void HOOK_eDisplayFrame__Fv(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    set_frame_rate(c);
    speed_log(c);
    pc_paint_buttons();         // the device changes in a race too
    g_frame.wide = native_narrow() < 1 && pc_racing();
    car_detail();
    reflections_schedule();
    // TEMP diagnostics (HP2_FRAMEMEM=1): the frame malloc buffer and the mirror's draws, for the mirror that shows only the sky
    static const bool diag = getenv("HP2_FRAMEMEM") != nullptr;
    if (diag) {
        const u32 gp = c.r[28].w[0];
        static u32 peak, fails, last_fail_frame;
        u32 used = LW(gp - 19312), failed = LW(gp - 19372), fail = LW(gp - 19368);
        if (used > peak) peak = used;
        if (failed) {
            fails++;
            if (g_frame_no - last_fail_frame >= 30) {
                fprintf(g_hle_log, "framemem: FAILED frame %u: %u bytes short (frames failed so far %u), used %u of %u\n", g_frame_no, fail, fails,
                        LW(gp - 19388) - LW(gp - 19320), LW(gp - 19392));
                fflush(g_hle_log);
                last_fail_frame = g_frame_no;
            }
        }
        if (g_frame_no % 300 == 0) {
            fprintf(g_hle_log, "framemem: frame %u half %u used %u peak %u failed frames %u maxfail %u free %u largest %u dd %d detail %d, scenery table peak %u (the PS2's 4200)\n", g_frame_no,
                    LW(gp - 19392), used, peak, fails, LW(gp - 19364), pc_call(0x257988, 0), pc_call(0x2579b0, 0), g_cfg.draw_distance, g_cfg.max_detail, g_draw_table_peak);
            fflush(g_hle_log);
        }
    }
    orig(c);
    if (diag && pc_racing()) {
        u32 draws = 0, verts = 0;
        for (const NativeDraw &d : g_frame.draws)
            if (d.target == 7) draws++, verts += d.count;
        static u32 last_low;
        if ((draws < 12 && g_frame_no - last_low >= 30) || g_frame_no % 300 == 0) {
            fprintf(g_hle_log, "mirror: frame %u draws %u vertices %u%s\n", g_frame_no, draws, verts, draws < 12 ? "  <-- few" : "");
            fflush(g_hle_log);
            if (draws < 12) last_low = g_frame_no;
        }
    }
    native_end_frame();
}
