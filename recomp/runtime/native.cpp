// The native renderer, the game's side: the draw lists and the strips (native.h). Addresses and formats
// were checked on the retail: handoff.md, "Нативный рендер: списки Ecstasy".
#include "native.h"
#include "stall.h"
#include "phase.h"
#include <intrin.h>
#include "cores.h"

#include "hw.h"
#include "pcconfig.h"
#include "pcmenu.h"
#include "memwatch.h"
#include "mirror_glass.h"
#include "track_roofs.h"

#include <array>
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

// the same from the pack's four quads p, its two quads q and the local-world matrix l (null: none) as the port has read them
static void clip_matrix_host(float out[16], const float p[16], const float q[8], const float *l, int half_w, int half_h, float z_scale)
{
    float m[16];
    if (l) mul(m, p, l);
    else memcpy(m, p, sizeof m);
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

// the same in doubles (mid_direct)
static void clip_matrix_d(double out[16], const float p[16], const float q[8], const float *l, int half_w, int half_h, float z_scale)
{
    double m[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            m[c * 4 + r] = l ? (double)p[r] * l[c * 4] + (double)p[4 + r] * l[c * 4 + 1] + (double)p[8 + r] * l[c * 4 + 2] + (double)p[12 + r] * l[c * 4 + 3] : (double)p[c * 4 + r];
    for (int c = 0; c < 4; c++) {
        const double *col = m + c * 4, w = col[3];
        const double cw = q[3] * col[3] + q[7] * w;
        out[c * 4 + 0] = q[0] * col[0] / half_w;
        out[c * 4 + 1] = q[1] * col[1] / half_h;
        out[c * 4 + 2] = (q[2] * col[2] + q[6] * w - 2 * cw) / 64 * z_scale;
        out[c * 4 + 3] = cw;
    }
}

// local 0: the pack alone, the world to the screen
static void clip_matrix(float out[16], u32 pack, u32 local, int half_w, int half_h, float z_scale)
{
    float p[16], l[16], q[8];
    read_quads(pack, p, 4);
    read_quads(pack + 64, q, 2);
    if (local) read_quads(local, l, 4);
    clip_matrix_host(out, p, q, local ? l : nullptr, half_w, half_h, z_scale);
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
struct MidOwner;
static const MidOwner *g_owner;             // the car whose node the list's reader is at (mid_owner_of), or null: the world's

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
    d.car = g_owner != nullptr;
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

// ---- a strip draw's state once a list (2026-11-24) ------------------------------------------------------------------------------
// set_draw_state reads the target's rectangle and depth test, the texture's blend, filter and scroll, and asks for the texture: the
// same answers for every draw of strips of one list (a list is one texture's, read in one go with nothing of the game running in
// between), and a list of the scenery's has hundreds of draws. So the first draw's state is kept and the others take it: while the
// list, the texture, the target, its mode and the picture are what they were. Every field set_draw_state sets is copied (a field
// added there is to be added here); the flags and the light are each draw's own. HP2_STATEKEEP=0: asked for every draw, as before.
static const bool g_state_keep = !(getenv("HP2_STATEKEEP") && atoi(getenv("HP2_STATEKEEP")) == 0);
static u32 g_list_serial;                   // counted up by every list read
static struct {
    bool have;
    u32 list, texture, target, mode, draw_target, bits;
    NativeDraw d;
} g_state_kept;

static void strip_state(NativeDraw &d)
{
    auto &k = g_state_kept;
    if (g_state_keep && k.have && k.list == g_list_serial && k.texture == g_texture && k.target == g_target_ptr && k.mode == g_mode &&
        k.draw_target == (u32)g_draw_target) {
        const NativeDraw &s = k.d;
        d.rt = s.rt;
        d.snap_x = s.snap_x, d.snap_y = s.snap_y;
        memcpy(d.view, s.view, sizeof d.view);
        d.tfx = s.tfx;
        d.lod_l = s.lod_l;
        d.depth_test = s.depth_test;
        d.target = s.target;
        d.depth_write = s.depth_write;
        d.alpha_test = s.alpha_test;
        d.blend = s.blend;
        d.blend_fix = s.blend_fix;
        d.scroll_s = s.scroll_s, d.scroll_t = s.scroll_t;
        d.tex = s.tex;
        d.tex_mode = s.tex_mode, d.tex_upscaled = s.tex_upscaled, d.tex_min_linear = s.tex_min_linear;
        d.flags = (u16)g_strip_flags;
        d.car = g_owner != nullptr;
        d.light = g_light;
        if (g_texture) g_alpha_seen[k.bits]++;          // (HP2_NATIVESTATS counts the draws by their blend)
        return;
    }
    set_draw_state(d);
    k.have = true;
    k.list = g_list_serial, k.texture = g_texture, k.target = g_target_ptr, k.mode = g_mode, k.draw_target = (u32)g_draw_target;
    k.bits = g_texture ? LB(g_texture + 118) : 0;
    k.d = d;
}
static float g_clip[16];
static bool g_clip_stale;                   // the target, the pack or the local-world changed since g_clip was made (update_clip)
static void clip_now();
static bool g_clip_new = true;
static struct { u64 strips, bad, verts, tris, lists; } g_stat;

static u32 g_target, g_pack, g_local;       // the list's state: the eRenderTarget, the matrix pack, the local-world matrix
// a 4 x 4 matrix inverted (by its cofactors, in doubles: the view's matrix has entries from 1e-17 to 2); false if it has no inverse
static bool invert4d(const double *a, double *out)
{
    double inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}
static bool invert4(const float *m, float *out)
{
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    if (!invert4d(a, inv)) return false;
    for (int i = 0; i < 16; i++) out[i] = (float)inv[i];
    return true;
}

// The matrix pack the scenery of a target is drawn with (DrawAScenery's strips: the world's own, no local matrix of a model's), by
// the target: the view's projection is in it (mid_direct)
static struct {
    u32 target, pack;
} g_view_pack[16];
static void note_view_pack(u32 target, u32 pack)
{
    for (auto &v : g_view_pack)
        if (v.target == target || !v.target) {
            v.target = target;
            v.pack = pack;
            return;
        }
}

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
    // any shape wider than 4:3 (2026-11-17: 21:9, 32:9 and the rest; it was 16:10 and 16:9 alone)
    const int aspect = native_aspect();
    return aspect > 13334 ? (4.0f / 3.0f) / ((float)aspect * 0.0001f) : 1.0f;
}

// The packets of a Flailer object and its pieces: a text goes as one thing, not glyph by glyph. One object can hold pieces far apart
// that share a texture (the last lights under the mirror and the panel by the right edge), and each of them goes on its own.
struct StripSpan {
    float x0, x1, whole;                                    // whole: the middle of the line of text the span is part of
};
static std::unordered_map<u32, std::vector<StripSpan>> g_packet_spans;
static std::vector<StripSpan> g_strip_spans;

// Whose a packet of the 2D layer is (the HUD in a picture between two frames, hud_between): taken from the game where the packet is
// made, kept by the packet's address till the lists are read.
//  - A Flailer object's (a text, a bitmap; the HUD is made of them): its strip is being rendered (FlailerStrip::Render, 0x145950).
//    The strip is made anew whenever the object changes, but it has its object at +8 (the constructor, 0x145758), and the object
//    stays. The group is the object and which of its strips this frame it is (an object rendered several times a frame, a list's
//    rows through one text object, has a strip each time, in the same order every frame); the index is the poly's place in the
//    strip, the strips chained to it (+32, another texture of the same object) counted on.
//  - The track map's (TrackMap::Render, 0x1bade8: the map's pieces and the cars' markers, straight through ePoly::Render): the
//    group is the map, the index the poly's place among all it draws.
// Anything else of the layer has no owner and is drawn in every picture as the frame has it.
struct HudWho {
    u64 group;
    u32 index;
    float x;                                // the object's own place across as its strip was rendered (FlailerObjectChunk +48)
};
static std::unordered_map<u32, HudWho> g_packet_who;
static u64 g_hud_group;                     // whose packets are being made now; 0: nobody's
static u32 g_hud_index;
static float g_hud_x;
static int g_hud_strips;                    // FlailerStrip::Render inside itself (a chained strip)
static std::unordered_map<u32, u32> g_hud_times;        // a Flailer object's strips rendered this frame so far
struct HudDraw {
    u32 draw;                               // in g_frame.draws
    u64 group;
    u32 index;
    u32 tex;                                // the game's TextureInfo, for HP2_HUDAUDIT's names
};
static std::vector<HudDraw> g_hud_draws;    // this frame's draws of packets that have an owner
static std::unordered_map<std::string, int> g_hud_strangers;       // HP2_HUDAUDIT: the layer's packets with none, by texture
static inline u64 hud_group(u32 kind, u32 who, u32 time) { return ((u64)kind << 60) ^ ((u64)time << 32) ^ who; }

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
    u64 who;                                                // the game's object that drew it (g_packet_who's group), 0: none known
    u32 tex;                                                // its TextureInfo (for the log's names)
};
static std::vector<Wide2D> g_wide_2d;
// The developers' readout (the Debug menu's frame rates, the car's place, the distance: "C: 79fps", "G: 4934fps", "0.215 km") is
// the screen's printf: FlailerRenderManager::RenderScreenPrintf (0x146190) renders the text objects of a screen of its own
// (0xd1d43ff1) while any were printed this frame. On a wide picture its lines keep to the picture's left edge whatever else is
// up: with the pause menu over the race the 2D is narrowed to the middle as a whole, and the readout stood in the menu's rows
// (the user's picture, 2026-11-24: "can the debug data go by the edges of the aspect in use too, so that they are at the edge").
// The packets made inside that call are marked by their address, as the HUD's owners are (g_packet_who).
static bool g_printf_now;
static std::unordered_map<u32, bool> g_packet_edge;

// The part of the picture a HUD object is put by, known from the layout itself (the user, 2026-11-26, after two evenings of
// pieces put by what happened to be drawn near them: "why not settle it at the root?", and of the ways offered: "B"). The
// HUD is Flailer objects of a file, each at a place the layout gives it; tools/recomp/make_hud_anchors.py works out from that
// file which part of a wide picture each belongs to (by the middle of its box, with the few the numbers do not say right
// named there; drawn for the user's eye at 16:9, 21:9 and 32:9 before it was built), pcmenu.cpp gives every object of a loaded
// screen its part, and a quad of such an object is put here at once, by that part and nothing else: it keeps it for as long
// as it lives, whatever is drawn beside it, so there is nothing to jump.
//
// The game moves its objects: an animation writes the object's own place and size frame by frame (HudAnimator::DoAnimation,
// 0x23b788: +48, +52, +56, +60), and plain code puts some where the layout does not have them at all (the user, the same
// day: "can you see by the code how the elements' animations go, and write it all into the anchors?"). By the code: a
// pursuit's star comes from 300 to the left of its place, four times its size (UpdatePursuitMeter, 0x17c380); a score's
// words stand at 530, 236 and leave by 90, -100 (StartScore), on the two players' screens as well, where the layout has
// them in the middle; the place number's and the speed's second copies grow or drop from where the first stands; the
// wrong way sign shows at 258 and parks 105, 95 or 240 to the left (HudStartWrongWay, 0x17c888); a gauge's needle and
// backing, which the layout keeps off the screen at the left, are put on the car's own gauge (SetHudFeatures); the marks
// of the cars behind run from 120 to 520 (DrawTracker, 0x17c278). So an object's number is its part's and what the
// parts' blend (hud_blend: the same places as a moving line's, wide_place) changes by between where the layout has it and
// where it is now, the object's own place read as its strip is rendered. Standing where the layout has it, that is the
// part itself; moved inside one part, still the part; taken from the middle to a side, it goes there evenly with the
// way; put by code in another part, it is in that part. One number for all the object's quads, and no step in it
// anywhere. Two kinds the table names apart: one that keeps its part wherever it is (the marks of the cars behind), one
// with two places and a part for each (the wrong way sign: the middle where it shows, the left where it parks).
// What has no part (drawn outside the Flailer's strips: the mirror, the track's map; the screens that are no HUD) goes the
// old way, by the lines of each frame (resolve_wide_2d). hp2.ini's hud_anchors=0: everything the old way.
static std::unordered_map<u32, NativeHudPart> g_hud_part;
void native_hud_part(uint32_t object, const NativeHudPart *part)
{
    if (!object || !part) g_hud_part.clear();
    else g_hud_part[object] = *part;
}

// the parts' numbers blended, by a middle in the layout's 640: 0 to the left of 147.5, 0.5 from 227.5 to 400, 1 past 480
static float hud_blend(float m)
{
    return m < 147.5f ? 0.0f : m < 227.5f ? (m - 147.5f) / 160.0f : m <= 400.0f ? 0.5f : m <= 480.0f ? 0.5f + (m - 400.0f) / 160.0f : 1.0f;
}

static float wide_place(float m, bool edges, bool moving, float f);
static bool dumping();
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
    bool edges = pc_hud_up() || g_packet_edge.count(data) != 0;
    const auto owner = g_packet_who.find(data);
    const u64 who = owner != g_packet_who.end() ? owner->second.group : 0;
    // (the object's part, if the layout's table has one for it: a Flailer object's strip, with the HUD up)
    float part = -1.0f;
    if (layer && edges && who >> 60 == 1 && g_cfg.hud_anchors && !g_hud_part.empty())
        if (const auto known = g_hud_part.find((u32)who); known != g_hud_part.end()) {
            const NativeHudPart &p = known->second;
            const float x = owner->second.x;
            part = p.k;
            if (p.kind == 2 && p.ax != p.bx) {
                const float t = (x - p.ax) / (p.bx - p.ax);
                part = p.ak + (p.bk - p.ak) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
            } else if (p.kind == 0 && std::fabs(x - p.x) > 0.01f) {
                part = p.k + hud_blend(p.mid + x - p.x) - hud_blend(p.mid);
                part = part < 0.0f ? 0.0f : part > 1.0f ? 1.0f : part;
            }
        }
    size_t group = n % 6 ? n : 6;
    for (size_t at = 0; at < n; at += group) {
        NativeVertex *v = &g_frame.vertices[first + at];
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (size_t i = 1; i < group; i++) {
            x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
            y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
        }
        x0 -= ox, x1 -= ox;
        if (dumping() && x1 - x0 >= 200)
            fprintf(g_hle_log, "dump wide quad target %u x %.1f..%.1f y %.1f..%.1f texture %08x %dx%d colour %02x%02x%02x%02x packet %08x%c", id, x0, x1, y0, y1, g_texture,
                    g_texture ? LH(g_texture + 0x24) : 0, g_texture ? LH(g_texture + 0x26) : 0, v[0].r, v[0].g, v[0].b, v[0].a, data, 10);
        // across the whole screen: a plain colour (a fade, a letterbox, a line) stays across the wide screen too; a picture keeps its shape
        if (x1 - x0 >= 400 && (!g_texture || LH(g_texture + 0x24) <= 64)) {
            // Left as it is, such a piece keeps its 4:3 numbers on the wide picture: its right end is at the right edge or past
            // it, and its left end as far in as 4:3 has it. A window over the race (a question of the pause menu, the screens
            // after a race) has its sheet, its red line and its white one so, from 73 of 512 on: with the title's bar now across
            // the whole picture (resolve_wide_2d, a band) they began a seventh of the way in (the user's picture, 2026-11-26:
            // "the backdrop with the white line must go to the end too"). The same band: from the left edge. Not with the HUD up.
            if (layer && !edges && x0 > 0.5f && x1 >= 508.0f) {
                const float to = std::max(512.0f, x1) / (x1 - x0);
                for (size_t i = 0; i < group; i++) v[i].x = ox + (v[i].x - ox - x0) * to;
            }
            continue;
        }
        // A sheet over the 2D layer from its left edge to its right one is a backdrop, whatever its texture: the film lines that darken
        // the picture under Busted, a ticket and the starting grid (NISOverlayManager's screen: three quads 0..512 of a 640x304 texture).
        // Narrowed like a picture it left the wide screen's sides lighter than its middle; a backdrop covers the wide screen too.
        // Not the replay's bar of buttons (REPLAYBOTTOM_NEW, 640 texels shown across the 512): it is the screen's controls, and
        // stretched over a wide picture its symbols and keys were a third wider and further apart than the places the pointer
        // presses them at, which go by the 4:3 block (the user, 2026-11-23: "the buttons' places do not match the mouse").
        // It is narrowed and centred with the rest of the replay's 2D.
        const bool replay_bar = g_texture && !strcmp((const char *)host(g_texture + 8), "REPLAYBOTTOM_NEW");
        if (layer && x0 <= 0.5f && x1 >= 511.5f && !replay_bar) continue;
        if (part >= 0.0f) {
            const float shift = part * 512.0f * (1.0f - f);
            for (size_t i = 0; i < group; i++) v[i].x = ox + (v[i].x - ox) * f + shift;
            continue;
        }
        if (layer) {
            u32 st[2];
            memcpy(st, &v[0].s, 8);
            g_wide_2d.push_back({(u32)(first + at), (u32)group, x0, x1, y0, y1, ox, data, g_texture ^ st[0] * 31 ^ st[1] * 131, edges, who, g_texture});
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

// What holds a piece of the HUD in its place (the user, 2026-11-25: "when the player gets the stars of a pursuit the table at
// the top left jerks and loses its place ... let us close the HUD's problems once and for all, this one and whatever others
// there may be"; the song's name at a race's start was the same kind of thing a day before).
//
// A piece's place across a wide picture is one number: x' = x * f + k * 512 * (1 - f), k 0 by the left edge, 0.5 in the
// middle, 1 by the right edge, and between them for what is on its way. That number was worked out anew in every frame, for
// every piece, from what happened to be drawn around it: the lines are found by what lies near what (below), so a thing
// passing by (a pursuit's star flying from the middle of the screen to its panel on the right) made one line of two for a
// few frames, the line's middle was somewhere else, and the pieces that had stood there all along were put by it: the list
// of racers went towards the middle and back.
//
// So the number is the game object's, kept from frame to frame (g_wide_held, by the Flailer object whose strip made the
// quad: g_packet_who, as the pictures between two frames know their objects):
//  - an object that stands where it stood keeps its number, whatever is drawn around it now. Standing: its span across is what
//    it was; or it is a text that got longer or shorter (as tall as it was, one end moved, or both apart: its middle by no
//    more than half the change of its width). NOT one that grows or shrinks, taller or lower as well as wider: the count
//    before a race, 3 2 1, comes in large and shrinks to its place with its left edge where it is, so the 1 was born with
//    its middle in the right part of the screen and, held to that, stayed by the right edge, far from the 2 (the user's
//    picture of the same evening: "the 1 is too far"). What changes its size is put anew in each frame until it rests;
//  - an object that is moving (both ends of its span went the same way since the frame before) is no part of the lines of what
//    stands, neither one of them nor a bridge between two; the moving ones make lines among themselves (a message of several
//    words slides in as one) and are put by their line's middle, with the parts' places blended, as a moving line always was.
//    When it comes to rest it keeps the number it arrived with;
//  - an object that is new (or was put somewhere else at once) gets its number from the lines, as before; in a line with
//    objects that stand and agree on theirs it takes theirs (a new row of the list, a new letter of a number), with ones that
//    disagree it goes by its own middle;
//  - an object whose pieces are in different parts of the screen (one strip's pieces far apart) has no number of its own: its
//    pieces go by the lines, as before. So does everything with no object (drawn outside the Flailer's strips).
// A text on a panel is taken into the panel's line only if most of the text's own span lies on the panel: a thing passing
// over a wide backdrop is not written on it.
// hp2.ini's hud_steady=0: every piece by the lines of each frame, as it was.
struct WideHeld {
    float lo, hi, top, bottom, k;           // the object's span a frame ago, across and down, and its number
    u32 frame;
    bool split;
    u8 rule;                                // how it got the number: 0 new (by the lines), 1 held, 2 moving, 3 split
    u32 told;                               // the frame speed.log was last told of it
};
static std::unordered_map<u64, WideHeld> g_wide_held;

static bool dumping();
static void resolve_wide_2d()
{
    if (g_wide_2d.empty()) return;
    const float f = native_narrow();
    const size_t n = g_wide_2d.size();
    const float room = 512.0f * (1.0f - f);
    const bool hold = g_cfg.hud_steady && room > 0.01f;
    // the objects of this frame: their spans, and which of them stand, which move
    struct Obj {
        float lo = 1e9f, hi = -1e9f, top = 1e9f, bottom = -1e9f, k = 0.5f, kmin = 1e9f, kmax = -1e9f;
        bool moving = false, standing = false, split = false;
        u32 tex = 0;                                                    // (for the log: its first quad's texture and line)
        int line = -1;
    };
    std::unordered_map<u64, Obj> objs;
    std::vector<Obj *> obj_of(n, nullptr);
    if (hold) {
        for (const Wide2D &q : g_wide_2d)
            if (q.who && q.edges) {
                Obj &o = objs[q.who];
                o.lo = std::min(o.lo, q.x0), o.hi = std::max(o.hi, q.x1);
                o.top = std::min(o.top, q.y0), o.bottom = std::max(o.bottom, q.y1);
                if (!o.tex) o.tex = q.tex;
            }
        for (size_t i = 0; i < n; i++)
            if (g_wide_2d[i].who && g_wide_2d[i].edges) obj_of[i] = &objs[g_wide_2d[i].who];
        for (auto &e : objs) {
            Obj &o = e.second;
            const auto was = g_wide_held.find(e.first);
            if (was == g_wide_held.end()) continue;
            const WideHeld &h = was->second;
            const float dlo = o.lo - h.lo, dhi = o.hi - h.hi;
            const bool same = std::fabs(dlo) <= 0.25f && std::fabs(dhi) <= 0.25f;
            const bool as_tall = std::fabs((o.bottom - o.top) - (h.bottom - h.top)) <= 0.5f;
            const bool text_changed = as_tall && std::fabs((o.lo + o.hi) - (h.lo + h.hi)) / 2 <= std::fabs((o.hi - o.lo) - (h.hi - h.lo)) / 2 + 0.5f;
            if (g_frame_no - h.frame <= 2 && std::fabs(dlo) > 0.25f && std::fabs(dhi) > 0.25f && (dlo > 0) == (dhi > 0)) o.moving = true;
            else if (!h.split && (same || text_changed)) o.standing = true, o.k = h.k;
        }
    }
    std::vector<char> mover(n, 0);
    for (size_t i = 0; i < n; i++) mover[i] = obj_of[i] && obj_of[i]->moving;
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
    // height, so that a large font's wider spaces are still spaces. Only letters make a line: a panel or a bar is no word of one. What
    // moves and what stands make no line together.
    for (size_t a = 0; a < n; a++) {
        const Wide2D &i = g_wide_2d[order[a]];
        if (!letter(i)) continue;
        for (size_t b = a + 1; b < n; b++) {
            const Wide2D &j = g_wide_2d[order[b]];
            if (j.x0 - i.x1 >= 64) break;
            if (!letter(j) || j.y0 > i.y1 + 4 || i.y0 > j.y1 + 4) continue;
            if (j.x0 - i.x1 >= std::max(32.0f, 0.9f * std::max(i.y1 - i.y0, j.y1 - j.y0))) continue;
            if (mover[order[a]] != mover[order[b]]) continue;
            up[root(order[b])] = root(order[a]);
        }
    }
    // A panel and what is written on it are one thing. A backdrop (a piece that is no letter) takes in every letter whose middle
    // lies on it, and with the letters their lines: the song's name as a race begins is four lines on a panel that fades out
    // far to the right, so the lines' middles were in the screen's left part and the panel's own in the middle one, and on a
    // wide picture the text went to the left edge while its panel stayed where 4:3 has it (the user's picture, 2026-11-25:
    // "the text is shifted"). The whole is placed by its letters, the panel with them. Not a sheet 400 wide or more: that is no
    // panel of one thing, and what lies on it keeps its own places. A letter of an object most of which is off the panel is
    // not written on it (above).
    for (size_t p = 0; p < n; p++) {
        const Wide2D &panel = g_wide_2d[p];
        if (letter(panel) || panel.x1 - panel.x0 >= 400) continue;
        for (size_t j = 0; j < n; j++) {
            const Wide2D &q = g_wide_2d[j];
            if (!letter(q) || mover[p] != mover[j]) continue;
            const float mx = (q.x0 + q.x1) / 2, my = (q.y0 + q.y1) / 2;
            if (mx < panel.x0 || mx > panel.x1 || my < panel.y0 || my > panel.y1) continue;
            if (const Obj *o = obj_of[j]) {
                const float on = std::min(o->hi, panel.x1) - std::max(o->lo, panel.x0);
                if (on < 0.75f * (o->hi - o->lo)) continue;
            }
            up[root((int)j)] = root((int)p);
        }
    }
    struct Line {
        float lo = 1e9f, hi = -1e9f, top = 1e9f;
        int count = 0, panels = 0;                                      // its letters; the backdrops they lie on
        u32 mark = 0;
        float mark_x = 1e9f;
        bool moving = false;
        int seniors = 0;                                                // standing objects in it: 0 none, 1 all of one number, 2 of several
        float senior_k = 0;
    };
    std::vector<Line> lines(n);
    for (size_t i = 0; i < n; i++) {
        const Wide2D &q = g_wide_2d[i];
        Line &l = lines[root((int)i)];
        if (const Obj *o = obj_of[i]; o && o->standing) {
            if (!l.seniors) l.seniors = 1, l.senior_k = o->k;
            else if (std::fabs(l.senior_k - o->k) > 0.01f) l.seniors = 2;
        }
        if (mover[i]) l.moving = true;
        if (!letter(q)) {
            l.panels++;                                                 // (placed by the letters, its own span is no part of theirs)
            continue;
        }
        l.lo = std::min(l.lo, q.x0), l.hi = std::max(l.hi, q.x1), l.top = std::min(l.top, q.y0);
        l.count++;
        if (q.x0 < l.mark_x) l.mark_x = q.x0, l.mark = q.mark;          // the line's first letter
    }
    // Does the line move? What has an object says so itself (above). The rest is known from one frame to the next by its first letter,
    // its length and its row; once seen moving it is a moving line for as long as it is there, so that it does not jump where it comes
    // to rest either.
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
        l.moving = l.moving || (was != seen.end() && (was->second.moving || std::fabs(was->second.mid - mid) > 0.25f));
        now[key] = {mid, l.moving};
    }
    seen.swap(now);
    // every quad's number by the lines of this frame
    std::vector<float> shift(n), middle(n);
    for (size_t i = 0; i < n; i++) {
        const Wide2D &q = g_wide_2d[i];
        const Line &l = lines[root((int)i)];
        const float mid = (q.x0 + q.x1) / 2;
        float whole = mid;
        if (l.count >= 2 || (l.count == 1 && l.panels)) {
            whole = (l.lo + l.hi) / 2;                                  // a line, or what is written on a panel and the panel itself
        } else {
            // a piece on its own: its strip's span, as the game's objects have it (a panel in several parts)
            auto spans = g_packet_spans.find(q.packet);
            if (spans != g_packet_spans.end())
                for (const StripSpan &sp : spans->second)
                    if (mid >= sp.x0 && mid <= sp.x1) whole = sp.whole;
        }
        middle[i] = whole;
        shift[i] = wide_place(whole, q.edges, l.moving, f) - whole * f;
        if (Obj *o = obj_of[i]) {
            const float k = shift[i] / room;
            o->kmin = std::min(o->kmin, k), o->kmax = std::max(o->kmax, k);
            if (o->line < 0) o->line = root((int)i);
        }
    }
    // the objects' own numbers
    if (hold) {
        // (a new object in a line with standing ones: all its lines' standing objects agree, or they do not)
        std::unordered_map<Obj *, int> agree;                           // 1 one number, 2 several
        std::unordered_map<Obj *, float> agreed;
        for (size_t i = 0; i < n; i++) {
            Obj *o = obj_of[i];
            if (!o || o->standing || o->moving) continue;
            const Line &l = lines[root((int)i)];
            if (!l.seniors) continue;
            int &a = agree[o];
            if (l.seniors == 2 || (a == 1 && std::fabs(agreed[o] - l.senior_k) > 0.01f)) a = 2;
            else if (!a) a = 1, agreed[o] = l.senior_k;
        }
        for (auto &e : objs) {
            Obj &o = e.second;
            if (!o.standing) {
                const auto a = agree.find(&o);
                if (o.kmax - o.kmin > 0.01f) o.split = true;            // (its pieces are in different lines, with different numbers)
                else if (a != agree.end() && a->second == 1) o.k = agreed[&o];
                else if (a != agree.end()) o.k = (wide_place((o.lo + o.hi) / 2, true, true, f) - (o.lo + o.hi) / 2 * f) / room;
                else o.k = o.kmin;
            }
            // speed.log: an object on the screen that was put somewhere else than a frame ago (the user, 2026-11-26, after a
            // word of a message came in at a wrong place and nobody had a picture of it: "yes, add it"). Its number changed by
            // more than 2 of the picture's 512 while it did not simply go on moving: what it is (the Flailer object's name,
            // a text's words, the texture), where it stood and stands in the game's 4:3 numbers, the numbers and how each was
            // come by, and the line it was put with. An object once in 20 frames, 600 lines a run.
            const u8 rule = o.split ? 3 : o.moving ? 2 : o.standing ? 1 : 0;
            u32 told = 0;
            if (const auto was = g_wide_held.find(e.first); was != g_wide_held.end()) {
                const WideHeld &h = was->second;
                told = h.told;
                static int lines_told;
                const float jump = (o.k - h.k) * room;
                if (!h.split && !o.split && g_frame_no - h.frame <= 2 && !(h.rule == 2 && rule == 2) && std::fabs(jump) > 2.0f &&
                    g_frame_no - h.told >= 20 && lines_told < 600) {
                    lines_told++;
                    told = g_frame_no;
                    static const char *const kRule[4] = {"new, by the lines", "held", "moving", "split"};
                    const u32 kind = (u32)(e.first >> 60), object = (u32)e.first;
                    char name[25] = "", words[41] = "";
                    if (kind == 1 && object >= 0x100000 && object < 0x2000000 - 160) {
                        for (int c = 0; c < 24; c++) {
                            const u8 ch = *host(object + 16 + (u32)c);
                            if (!ch) break;
                            name[c] = ch >= 32 && ch < 127 ? (char)ch : '?', name[c + 1] = 0;
                        }
                        if ((s8)*host(object + 40) == 1)
                            for (int c = 0; c < 40; c++) {
                                const u8 ch = *host(object + 100 + (u32)c);
                                if (!ch) break;
                                words[c] = ch >= 32 && ch < 127 ? (char)ch : '?', words[c + 1] = 0;
                            }
                    }
                    const Line *l = o.line >= 0 ? &lines[(size_t)o.line] : nullptr;
                    fprintf(stderr, "hud: frame %u: %s%s%s%s (texture %s) was put %.1f of 512 %s: x %.1f..%.1f -> %.1f..%.1f, y %.1f..%.1f -> %.1f..%.1f, "
                                    "number %.3f (%s) -> %.3f (%s), line of %d letters %.1f..%.1f%s%s, narrowed %.3f\n",
                            g_frame_no, kind == 2 ? "the track map" : name[0] ? name : "an object", words[0] ? " \"" : "", words, words[0] ? "\"" : "",
                            o.tex ? (const char *)host(o.tex + 8) : "-", std::fabs(jump), jump > 0 ? "to the right" : "to the left", h.lo, h.hi, o.lo, o.hi,
                            h.top, h.bottom, o.top, o.bottom, h.k, kRule[h.rule & 3], o.k, kRule[rule], l ? l->count : 0, l ? l->lo : 0.0f, l ? l->hi : 0.0f,
                            l && l->moving ? ", moving" : "", l && l->seniors ? (l->seniors == 1 ? ", with held ones of one number" : ", with held ones of several numbers") : "", f);
                }
            }
            g_wide_held[e.first] = {o.lo, o.hi, o.top, o.bottom, o.k, g_frame_no, o.split, rule, told};
        }
        if ((g_frame_no & 511) == 0)
            for (auto it = g_wide_held.begin(); it != g_wide_held.end();) it = g_frame_no - it->second.frame > 3600 ? g_wide_held.erase(it) : std::next(it);
    }
    // A band of a screen that is over the race (the windows after a race and the questions of the pause menu: a title's bar with
    // its red line, the sheet under the text): a backdrop that runs off the picture's right edge in 4:3. Narrowed to the
    // middle with the rest of the screen it began a long way in from the wide picture's left edge, and a title's bar, which
    // ends at the 4:3 edge, stopped short of the right one too (the user's pictures, 2026-11-25: "from edge to edge, with no
    // empty room on the left and on the right"). Such a band goes across the whole wide picture: from its left end at the
    // left edge to its right end where it was, or at the right edge. A band: every quad of one object (or of one packet,
    // where no object is known), none of them a letter's size, 200 across or more together, ending within 4 of the right
    // edge or past it. Not while the HUD is up: its parts keep to their edges. The texts on a band stay with the screen.
    struct Band {
        float lo = 1e9f, hi = -1e9f;
        bool plain = true;
    };
    std::unordered_map<u64, Band> bands;
    const auto band_of = [](const Wide2D &q) { return q.who ? q.who : (u64)q.packet | 1ull << 63; };
    for (const Wide2D &q : g_wide_2d) {
        if (q.edges) continue;
        Band &b = bands[band_of(q)];
        b.lo = std::min(b.lo, q.x0), b.hi = std::max(b.hi, q.x1);
        if (letter(q)) b.plain = false;
    }
    for (size_t i = 0; i < n; i++) {
        const Wide2D &q = g_wide_2d[i];
        const Line &l = lines[root((int)i)];
        const Obj *o = obj_of[i];
        const float s = o && !o->split ? o->k * room : shift[i];
        if (!q.edges) {
            const Band &b = bands[band_of(q)];
            // (not one that begins at the left edge: a sheet across the whole 4:3 picture that is narrowed on purpose, the
            // replay's bar of buttons, stays as it is)
            if (b.plain && b.lo > 0.5f && b.hi >= 508.0f && b.hi - b.lo >= 200.0f) {
                const float to = std::max(512.0f, b.hi) / (b.hi - b.lo);
                NativeVertex *v = &g_frame.vertices[q.first];
                for (u32 k = 0; k < q.count; k++) v[k].x = q.ox + (v[k].x - q.ox - b.lo) * to;
                if (dumping())
                    fprintf(g_hle_log, "dump 2D quad %zu x %.1f..%.1f y %.1f..%.1f a band %.1f..%.1f -> across the picture\n", i, q.x0, q.x1, q.y0, q.y1, b.lo, b.hi);
                continue;
            }
        }
        {   // TEMP HP2_HUDLOG=1: a piece of the top right corner that is not put by the right edge
            static const bool on = getenv("HP2_HUDLOG") != nullptr;
            static int told;
            static u32 last_frame;
            if (on && q.y0 < 90 && q.x0 > 352 && room - s > 2 && told < 200 && pc_racing()) {
                if (last_frame != g_frame_no) told++, last_frame = g_frame_no;
                fprintf(g_hle_log, "hudlog: frame %u quad x %.1f..%.1f y %.1f..%.1f line %.1f..%.1f of %d%s edges %d mode %u: middle %.1f, shift %.1f of %.1f%s%c", g_frame_no, q.x0, q.x1,
                        q.y0, q.y1, l.lo, l.hi, l.count, l.moving ? " moving" : "", q.edges, pc_input_mode(), middle[i], s, room,
                        !o ? "" : o->standing ? " (held)" : o->moving ? " (its object moves)" : " (new)", 10);
            }
        }
        NativeVertex *v = &g_frame.vertices[q.first];
        for (u32 k = 0; k < q.count; k++) v[k].x = q.ox + (v[k].x - q.ox) * f + s;
        if (dumping())
            fprintf(g_hle_log, "dump 2D quad %zu x %.1f..%.1f y %.1f..%.1f %s line %d of %d, %.1f..%.1f%s -> middle %.1f, shift %.1f%s\n", i, q.x0, q.x1, q.y0, q.y1,
                    letter(q) ? "letter" : "piece", root((int)i), l.count, l.lo, l.hi, l.moving ? " moving" : "", middle[i], s,
                    !o ? "" : o->standing ? " held" : o->moving ? " moving object" : o->split ? " split object" : " new object");
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

// ---- the strips' triangles by indices (change 3, 2026-11-24) -------------------------------------------------------------------
// A strip of n vertices is up to n - 2 triangles, and the cache held each triangle's three vertices: 84 vertices of 32 bytes for a
// strip of 30, every vertex there about three times. A far view at the most detailed models had 13 million of them in the GPU's
// memory (430 MB) and gave the GPU 1.5 million vertices a frame to work on. Now the cache holds the strip's own vertices once and,
// apart, the triangles as indices: 4 bytes where a vertex's copy was 32, and the GPU works a vertex once however many triangles
// share it. The same triangles of the same vertices in the same order: the picture is the same by construction.
// The cache is one buffer of the GPU's still, counted in vertices (32 bytes, a "slot"). The vertices fill it from the bottom as
// before (g_cache_top). The indices take chunks from its top down (kIdxChunk slots each, 8 indices a slot) and fill a chunk upwards,
// so strips cached one after another have their indices one after another, and a draw's strips are still a few ranges (now of
// indices). A range's first is an index's place in the buffer read as 32-bit words from its start; an index is its vertex's slot.
// The indices go to the GPU the way the vertices do, as slots of the frame's "fresh" ones: the chunk being filled is kept here
// (g_idx_mirror) and the slots touched in a frame are sent whole at its end. A strip the cache has no room for (or the mirror's
// glass, which is never cached) goes into the frame's own vertices the same way: its vertices, then its indices in slots after them.
// HP2_GEOINDEX=0: every triangle's vertices, as before (and so with HP2_INDEXED=1, or on a GPU that takes no 32-bit index).
static const bool kGeoDefault = true;       // on since 2026-11-24: the five demos in step, 939 thousand strips read back the same
static const u32 kIdxChunk = 512;           // slots: 4096 indices
static bool g_geo;                          // this frame's strips are made that way (set between frames only: geo_frame)
static u32 g_idx_low;                       // the lowest slot the indices have taken; 0: none yet
static u32 g_idx_at, g_idx_end;             // the next index's place, and the chunk's end
static u32 g_idx_mirror[kIdxChunk * 8];
static u32 g_idx_dirty_lo, g_idx_dirty_hi;  // the chunk's indices not sent to the GPU yet

static u32 cache_used() { return g_cache_top + (g_idx_low ? g_cache_cap - g_idx_low : 0); }

// the chunk's indices written since the last time, as whole slots of the frame's fresh ones
static void idx_send()
{
    if (g_idx_dirty_hi <= g_idx_dirty_lo) return;
    const u32 chunk = g_idx_end - kIdxChunk * 8, slot_lo = g_idx_dirty_lo / 8, slots = (g_idx_dirty_hi + 7) / 8 - slot_lo;
    const size_t src = g_frame.fresh.size();
    g_frame.fresh.resize(src + slots);
    memcpy((void *)&g_frame.fresh[src], g_idx_mirror + (slot_lo * 8 - chunk), (size_t)slots * sizeof(NativeVertex));
    g_frame.fresh_runs.push_back({slot_lo, (u32)src, slots});
    g_idx_dirty_lo = g_idx_dirty_hi = 0;
}

// room in the cache for a strip of n vertices and count indices; a new chunk of indices is begun here when the one in use is full
static bool geo_room(u32 n, u32 count)
{
    const bool chunk = !g_idx_low || g_idx_at + count > g_idx_end;
    u32 low = g_idx_low ? g_idx_low : g_cache_cap;
    if (chunk) {
        if (low < kIdxChunk) return false;
        low -= kIdxChunk;
    }
    if (g_cache_top + n > low) return false;
    if (chunk) {
        idx_send();
        g_idx_low = low;
        g_idx_at = low * 8;
        g_idx_end = (low + kIdxChunk) * 8;
        memset(g_idx_mirror, 0, sizeof g_idx_mirror);
    }
    return true;
}

// HP2_GEOCHECK=1. The cache as the GPU is to have it, kept on this side: every frame's fresh slots are put into it the way the
// renderer's copy puts them (and must never write a slot that is in use with anything else), and every strip put into the cache
// or the frame's own vertices that frame is read back the way a draw reads it, index by index, against its triangles.
static const bool g_geo_check = getenv("HP2_GEOCHECK") != nullptr;
struct GeoPut {
    u32 base, count, data;
    NativeVertex tri[84];
};
static std::vector<NativeVertex> g_geo_copy;
static std::vector<u8> g_geo_set;           // a slot of the copy has been written since the cache was emptied
static std::vector<GeoPut> g_geo_puts;
static u64 g_geo_read, g_geo_bad, g_geo_over;

static void geo_check_frame()
{
    if (!g_geo_check || !g_geo) {
        g_geo_puts.clear();
        return;
    }
    if (g_geo_copy.size() != g_cache_cap) g_geo_copy.assign(g_cache_cap, NativeVertex{}), g_geo_set.assign(g_cache_cap, 0);
    for (const NativeFreshRun &r : g_frame.fresh_runs)
        for (u32 i = 0; i < r.count; i++) {
            const u32 slot = r.dst + i;
            if (slot >= g_cache_cap) {
                g_geo_over++;
                continue;
            }
            const NativeVertex &now = g_frame.fresh[r.src + i];
            // a slot written again: only the indices' last slot is, filled further, and what it had stays
            if (g_geo_set[slot]) {
                u32 was[8], is[8];
                memcpy(was, &g_geo_copy[slot], 32), memcpy(is, &now, 32);
                bool grown = slot >= g_idx_low && g_idx_low;
                for (int k = 0; k < 8 && grown; k++) grown = !was[k] || was[k] == is[k];
                if (!grown) g_geo_over++;
            }
            g_geo_copy[slot] = now;
            g_geo_set[slot] = 1;
        }
    const u32 dyn = g_frame.dyn_base, own = (u32)g_frame.vertices.size();
    const auto slot_of = [&](u32 slot) -> const NativeVertex * {
        if (slot < g_cache_cap && slot < dyn) return g_geo_set[slot] ? &g_geo_copy[slot] : nullptr;
        if (slot >= dyn && slot - dyn < own) return &g_frame.vertices[slot - dyn];
        return nullptr;
    };
    for (const GeoPut &put : g_geo_puts) {
        bool same = true;
        for (u32 k = 0; k < put.count && same; k++) {
            const u32 at = put.base + k;
            const NativeVertex *words = slot_of(at / 8);
            u32 index = 0;
            if (words) memcpy(&index, (const char *)words + (at % 8) * 4, 4);
            const NativeVertex *v = words ? slot_of(index) : nullptr;
            same = v && !memcmp(v, &put.tri[k], sizeof(NativeVertex));
        }
        g_geo_read++;
        if (!same && g_geo_bad++ < 10)
            fprintf(g_hle_log, "geocheck: a strip at %08x put at index %u (%u of them) does not read back as its triangles\n", put.data, put.base, put.count);
    }
    g_geo_puts.clear();
    if (g_frame_no % 600 == 0)
        fprintf(g_hle_log, "geocheck: %llu strips read back through their indices, %llu not the same; %llu slots written over\n", (unsigned long long)g_geo_read,
                (unsigned long long)g_geo_bad, (unsigned long long)g_geo_over),
            fflush(g_hle_log);
}

static void cache_flush()
{
    std::fill(g_geo_set.begin(), g_geo_set.end(), (u8)0);
    g_strip_generation++;
    g_strip_count = 0;
    g_strip_check.clear();
    g_cache_top = 0;
    g_idx_low = g_idx_at = g_idx_end = 0;
    g_idx_dirty_lo = g_idx_dirty_hi = 0;
    g_cache_flushes++;
}

// A strip looked up ahead of the list's reading (look_ahead): known, with its place in the cache, or not found that way.
struct StripAhead {
    CachedStrip strip;
    bool known;
};

static bool g_double_now;                   // 120 pictures a second: this frame is to be drawn twice (mid_frame)
static void mid_matrix(NativeDraw &d, u32 strip);
static const MidOwner *g_strip_owner;       // of the last draw of strips made
static void mid_direct(NativeDraw &d);
static int shadows_level();
static void shadow_caster();
static bool shadow_poly(u32 node);
static int shadow_car(Ctx &c, u32 info, const float pose[16], bool moves, const float move[16]);
static void shadow_frame();
static void shadow_end();
static void ao_end();
static bool mid_particle_data(u32 data);
static bool port_sprites_emit(u32 data);
static void port_sprites_check(u32 from, u32 to);
static void port_sprites_frame();
static void mid_particles(size_t first_draw);

// The strips of a draw as ranges of the GPU's vertex buffer (NativeRange), not as indices made and sent every frame. HP2_INDEXED=1:
// the indices, as it was, for comparing.
static const bool g_ranged = getenv("HP2_INDEXED") == nullptr;

// ---- the player's side mirrors ------------------------------------------------------------------------------------------
// The user, 2026-11-24: "for the fun of it: the side mirrors of the driver's car show a small copy of the HUD's rear-view
// mirror; every car that has such a glass, the bonus ones too; where there is none, touch nothing; not on a split screen".
// The game makes one reflection: the rear-view mirror of the HUD, its own view of the world behind (target 7). A car's side
// mirrors are parts of their own, <CAR>_SIDE_MIRROR_LEFT / _RIGHT, on all 29 cars that can be driven, and one strip of each is
// the glass (mirror_glass.h, made from the car files by tools/recomp/make_mirror_glass.py, which says how it is told from the
// housing; checked on a sheet of all 58, work/nshots/mirrors). The glass has no picture laid out on it: the whole strip is one
// texel of the car's texture.
//
// Here that strip, of the first player's car in the player's own view, is drawn with the mirror's picture instead: as a draw of
// its own, its vertices' texture coordinates made from where each lies on the glass's plane, unlit and with no reflection
// pass (the strip's own flags are for the car's paint). The left mirror shows the picture's part about three quarters across,
// the right one about a quarter: the mirror's view looks backward, so the car's left is the picture's right; the HUD turns it
// round as it shows it, and the glass, seen from behind the car, takes it the same way. As tall as the picture, and as wide
// as the glass's own shape makes that.
//
// Only while the mirror's view is drawn (the frame before had draws of target 7: the HUD's mirror is on), with one player, and
// for a strip whose first vertex and count are a row of the table: a car bent by a crash, a car of a mod, another car's
// mirrors, the car in the mirror's own view or in the reflections' views are all left as they are. hp2.ini side_mirrors=0: off.
//
// The user, 2026-11-24, having seen it ("works fine in a race"): it goes by the Reflections setting, never at the PS2's, on at
// the others; the picture on the glass at a quarter of the mirror's resolution (first a half, then "let us try four times"),
// the HUD's own mirror as it is; the side mirrors aspheric, their outer third showing more ("the second variant, on the side
// ones only"); and the glass of the mirror inside the car, where a car has one as a plate of its own (side 0 in the table:
// four open cars), shows the picture's middle. All of that is native.frag's sample_glass (NativeDraw::tex_mode 128): the
// vertices carry the place on the glass (s: across it, -1 to 1 toward the car's left; t: the picture's own) and, as their
// colour, the glass's numbers (r the side, g the part's width, b its middle), and the shader makes the picture's place of them
// for every pixel: a glass has a dozen vertices, all on its outline, so a curve made at the vertices would be no curve.
//
// And the mirror's view is there before the HUD's mirror is (the user: "the glass is empty until the race starts, then it
// fills at once"): pcmenu.cpp keeps the mirror's camera on for the whole race (mirror_view_frame), and the HUD's own picture
// of it is not drawn while the game does not want it.
static bool g_rvm_now, g_rvm_was;           // the mirror's view has draws this frame; it had the frame before
struct GlassSeen {
    u32 qwords, generation;
    int row;                                // of kMirrorGlass, -1: not a glass
};
static std::unordered_map<u32, GlassSeen> g_glass_seen;        // by the strip's address: asked of the table once

static u32 owner_car(const MidOwner *o);    // the Car a car's nodes are of
static int glass_row(u32 data, u32 qwords)
{
    static thread_local u32 vu[VU_QUADS][4];
    memset(vu, 0, sizeof vu);
    if (!unpack_strip(data, qwords, vu)) return -1;
    const int n = (int)(vu[0][0] & 0xff);
    // (cars that share a mirror's shape have rows that are the same: the first is as good as the other)
    for (size_t i = 0; i < sizeof kMirrorGlass / sizeof kMirrorGlass[0]; i++) {
        const MirrorGlass &m = kMirrorGlass[i];
        if (m.n == n && m.first[0] == vu[2][0] && m.first[1] == vu[3][0] && m.first[2] == vu[4][0]) return (int)i;
    }
    return -1;
}

static const float kGlassCurve = 1.0f;     // native.frag's GLASS_CURVE
static bool g_pcar_now, g_pcar_was;         // the first player's car was drawn this frame; the frame before
bool native_player_car_drawn() { return g_pcar_was; }
static const MirrorGlass *mirror_glass_now(u32 data, u32 qwords)
{
    if (!g_cfg.side_mirrors || !g_cfg.reflections || !g_rvm_was || g_draw_target != 1 || !g_owner) return nullptr;
    const u32 pcar = owner_car(g_owner);
    if (!pcar || pcar != pc_player_car() || pc_two_player_race()) return nullptr;
    // What an address was found to be is kept while the car is being drawn: after a pause in that (another race is loaded, and
    // another car may lie at the same place) all is asked again. A strip found to be a glass is asked every time: two a frame.
    static u32 last_frame;
    if (g_frame_no - last_frame > 30) g_glass_seen.clear();
    last_frame = g_frame_no;
    auto it = g_glass_seen.find(data);
    if (it == g_glass_seen.end() || it->second.qwords != qwords || it->second.generation != g_strip_generation || it->second.row >= 0) {
        if (g_glass_seen.size() > 4096) g_glass_seen.clear();
        const int row = glass_row(data, qwords);
        static int told;
        if (row >= 0 && it == g_glass_seen.end() && told < 8)
            told++, fprintf(g_hle_log, "native: side mirrors: a glass of the player's car at %08x, row %d of the table\n", data, row);
        g_glass_seen[data] = GlassSeen{qwords, g_strip_generation, row};
        return row >= 0 ? &kMirrorGlass[row] : nullptr;
    }
    return nullptr;
}

// what g_clip was made of: the pack's quads and the local-world matrix, for the picture between two frames (mid_matrix)
static float g_now_p[16], g_now_q[8], g_now_l[16];

// ---- the clip matrix made later (change 4, 2026-11-24) ----------------------------------------------------------------------
// Every draw of strips has a clip matrix: the view's matrix pack times the draw's local-world matrix, scaled to the target. The
// game's thread made it as the draw began, several thousand products a frame (clip_now was 1.4 % of the thread's samples), though
// nothing on that thread reads it but the pictures between frames. So it now hands over what the matrix is made of: the draw keeps
// its local-world matrix where the product will be, and the number of the view's pack among the frame's (NativePack); the
// renderer's thread makes the product before it draws (native_frame_clips). The same function on the same numbers, on a thread
// that rounds the same way (ee_float_mode_here): the same matrix to the bit.
// Not at more than 60 pictures a second: the picture between needs the product at once (mid_matrix), and so do the shadows'
// casters, which ask clip_now themselves. HP2_CLIPLATER=0: made at once, as before. HP2_CLIPCHECK=1: made at once as well, and at
// the frame's end the two are compared.
static bool g_clip_made;                    // g_clip is the product of what g_now_p, g_now_q and g_now_l hold
static const bool g_clip_later = getenv("HP2_CLIPLATER") ? atoi(getenv("HP2_CLIPLATER")) != 0 : true;
static const bool g_clip_check = getenv("HP2_CLIPCHECK") != nullptr;
struct ClipKept {
    u32 draw;
    float clip[16];
};
static std::vector<ClipKept> g_clip_kept;
static u64 g_clip_laters, g_clip_checked, g_clip_bad;

// What a clip matrix is made of, read; the product not made. The view's pack is read again only when a list's node has named the
// target or the pack anew, or another list has begun (g_pack_stale): between two draws of a list it is most often only the
// local-world matrix that is another, and the pack's 96 bytes were read and compared with the frame's last for each of them.
static bool g_pack_stale = true;
static int g_pack_slot = -1;                // the pack's place among the frame's (frame_pack); -1: not looked for yet

static void clip_read()
{
    if (!g_clip_stale) return;
    g_clip_stale = false;
    g_clip_made = false;
    if (g_pack_stale || !g_state_keep) {
        g_pack_stale = false;
        g_pack_slot = -1;
        read_quads(g_pack, g_now_p, 4);
        read_quads(g_pack + 64, g_now_q, 2);
    }
    read_quads(g_local, g_now_l, 4);
}

static void clip_now()
{
    clip_read();
    if (g_clip_made) return;
    g_clip_made = true;
    clip_matrix_host(g_clip, g_now_p, g_now_q, g_now_l, LW(g_target + 0x1c) / 2, LW(g_target + 0x20) / 2, depth_scale(g_target));
}

// the view's pack as the frame's NativePack: the last one again while it is the same
static int frame_pack()
{
    if (g_pack_slot >= 0 && (size_t)g_pack_slot < g_frame.packs.size()) return g_pack_slot;
    NativePack k;
    memcpy(k.p, g_now_p, sizeof k.p);
    memcpy(k.q, g_now_q, sizeof k.q);
    k.half_w = (int)(LW(g_target + 0x1c) / 2);
    k.half_h = (int)(LW(g_target + 0x20) / 2);
    k.z_scale = depth_scale(g_target);
    if (!g_frame.packs.empty() && !memcmp(&g_frame.packs.back(), &k, sizeof k)) return g_pack_slot = (int)g_frame.packs.size() - 1;
    if (g_frame.packs.size() >= 65535) return -1;
    g_frame.packs.push_back(k);
    return g_pack_slot = (int)g_frame.packs.size() - 1;
}

void native_frame_clips(NativeFrame &frame)
{
    for (NativeDraw &d : frame.draws) {
        if (!d.clip_local) continue;
        d.clip_local = false;
        if (d.pack >= frame.packs.size()) continue;
        const NativePack &k = frame.packs[d.pack];
        float local[16];
        memcpy(local, d.clip, sizeof local);
        clip_matrix_host(d.clip, k.p, k.q, local, k.half_w, k.half_h, k.z_scale);
    }
}

// HP2_CLIPCHECK: the frame's draws left to the renderer, each against the matrix made at once when the draw began
static void clip_check_frame()
{
    for (const ClipKept &kept : g_clip_kept) {
        if (kept.draw >= g_frame.draws.size()) continue;
        const NativeDraw &d = g_frame.draws[kept.draw];
        float made[16] = {};
        bool same = d.clip_local && d.pack < g_frame.packs.size();
        if (same) {
            const NativePack &k = g_frame.packs[d.pack];
            clip_matrix_host(made, k.p, k.q, d.clip, k.half_w, k.half_h, k.z_scale);
            same = !memcmp(made, kept.clip, sizeof made);
        }
        g_clip_checked++;
        if (!same && g_clip_bad++ < 10) fprintf(g_hle_log, "clipcheck: draw %u of frame %u: the matrix made later is not the one made at once\n", kept.draw, g_frame_no);
    }
    g_clip_kept.clear();
    if (g_clip_check && g_frame_no % 600 == 0)
        fprintf(g_hle_log, "clipcheck: %llu matrices compared, %llu not the same; %llu left to the renderer so far, %zu packs this frame\n",
                (unsigned long long)g_clip_checked, (unsigned long long)g_clip_bad, (unsigned long long)g_clip_laters, g_frame.packs.size()),
            fflush(g_hle_log);
}

static u32 g_strip_last_base, g_strip_last_tris;      // add_strip's last: where its vertices are (HP2_DIRECTCHECK)

// A strip's draw: the one before it goes on with this strip, or a new one begins here (the last of g_frame.draws either way). tri: the
// strip's vertices where they were unpacked, for a frame's dump only.
static void strip_draw(u32 flags, u32 data, const NativeVertex *tri, u32 tri_n)
{
    update_light();
    const NativeDraw *prev = g_frame.draws.empty() ? nullptr : &g_frame.draws.back();
    if (g_clip_new || !prev || !prev->indexed || prev->flags != (u16)flags || prev->light != g_light || prev->target != (u8)g_draw_target ||
        g_owner != g_strip_owner) {
        g_strip_owner = g_owner;
        g_strip_flags = flags;
        NativeDraw d;
        int pack = -1;
        if (g_clip_later && !g_double_now) {
            clip_read();
            pack = frame_pack();
        }
        if (pack >= 0) {
            // the product is the renderer's thread's to make (native_frame_clips)
            memcpy(d.clip, g_now_l, sizeof d.clip);
            d.clip_local = true;
            d.pack = (uint16_t)pack;
            g_clip_laters++;
            if (g_clip_check) {
                clip_now();
                g_clip_kept.emplace_back();
                g_clip_kept.back().draw = (u32)g_frame.draws.size();
                memcpy(g_clip_kept.back().clip, g_clip, sizeof g_clip);
            }
        } else {
            clip_now();
            memcpy(d.clip, g_clip, sizeof d.clip);
        }
        note_view_pack(g_target_ptr, g_pack);
        d.has_mid = false;
        if (g_double_now) mid_matrix(d, data);
        d.first = g_ranged ? (u32)g_frame.ranges.size() : (u32)g_frame.indices.size();
        d.count = 0;
        d.indexed = true;
        d.ranges = 0;
        strip_state(d);
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
        if (g_draw_target == 7) g_rvm_now = true;
        g_frame.draws.push_back(d);
        g_clip_new = false;
        if (g_owner) shadow_caster();
    }
}

static void add_strip(u32 data, u32 qwords, u32 flags, const StripAhead *ahead = nullptr)
{
    g_strip_last_tris = 0;
    // (a side mirror's glass of the player's car is not the cache's: its vertices get coordinates of their own, below)
    const MirrorGlass *const glass = mirror_glass_now(data, qwords);
    const bool caching = g_cache_cap && !g_cache_off && !glass;
    StripKey key = {};
    const CachedStrip *hit = nullptr;
    // Known ahead: the table by address is not asked again. (Its slot may have gone to another strip meanwhile, read earlier in this
    // list; the strip is then still in the cache under its key, at the same place, so what is drawn is the same either way.)
    const bool ahead_known = ahead && ahead->known && !glass;
    StripSeen *seen = caching && !ahead_known ? strip_seen(data) : nullptr;
    const bool known = ahead_known || (seen && seen->fast && seen->data == data && seen->qwords == qwords && seen->generation == g_strip_generation &&
                                       seen->stamp == memwatch_stamp(data, qwords * 16));
    if (ahead_known) {
        hit = &ahead->strip;
        g_seen_hits++;
        static const bool check = getenv("HP2_AHEADCHECK") != nullptr;
        if (check) {
            static u64 checked, bad;
            const CachedStrip *long_way = strip_find(strip_key(data, qwords));
            checked++;
            if (!long_way || long_way->base != hit->base || long_way->count != hit->count || long_way->n != hit->n)
                if (bad++ < 10) fprintf(g_hle_log, "aheadcheck: a strip at %08x (%u qwords) known ahead as %u+%u is %s the long way\n", data, qwords, hit->base, hit->count,
                                        long_way ? "another place" : "not in the cache");
            if (checked % 2000000 == 0) fprintf(g_hle_log, "aheadcheck: %llu strips known ahead checked the long way, %llu differ\n", (unsigned long long)checked, (unsigned long long)bad);
        }
    } else if (known) {
        key = seen->key;
        hit = &seen->strip;
        g_seen_hits++;
    } else if (caching) {
        key = strip_key(data, qwords);
        hit = strip_find(key);
    }
    NativeVertex tri[84];                   // 28 triangles at most
    NativeVertex in[30];                    // the strip's own vertices
    u8 ix[84];                              // and each triangle's corner as a number among them
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
        ix[tri_n] = (u8)(v - 2), ix[tri_n + 1] = (u8)(odd ? v : v - 1), ix[tri_n + 2] = (u8)(odd ? v - 1 : v);
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
    if (glass) {
        // the mirror's picture is half the 4:3 width by a seventh of the height: 14 / 3 wide for 1 tall
        const float wide = std::min(glass->half_w / glass->half_h * (3.0f / 14.0f), 0.9f);
        const float part = glass->side > 0 ? 0.75f : glass->side < 0 ? 0.25f : 0.5f;
        // a side mirror's outer third reaches further (sample_glass): by the curve times (2 / 3) squared of half the part
        const float reach = glass->side ? kGlassCurve * 4.0f / 9.0f : 0.0f;
        const float lo = 0.02f + wide / 2 * (1 + (glass->side < 0 ? reach : 0.0f)), hi = 0.98f - wide / 2 * (1 + (glass->side > 0 ? reach : 0.0f));
        const float middle = std::min(std::max(part, lo), std::max(lo, hi));
        const u8 r = glass->side > 0 ? 255 : glass->side < 0 ? 0 : 128;
        const u8 g = (u8)std::min(wide * 255.0f + 0.5f, 255.0f), b = (u8)(middle * 255.0f + 0.5f);
        const auto on_glass = [&](NativeVertex &v) {
            const float rx = v.x - glass->centre[0], ry = v.y - glass->centre[1], rz = v.z - glass->centre[2];
            const float up = (rx * glass->up[0] + ry * glass->up[1] + rz * glass->up[2]) / glass->half_h;
            v.s = (rx * glass->across[0] + ry * glass->across[1] + rz * glass->across[2]) / glass->half_w;
            v.t = 0.5f - up * 0.47f;
            v.r = r, v.g = g, v.b = b, v.a = 128;
        };
        for (u32 i = 0; i < tri_n; i++) on_glass(tri[i]);
        for (u32 i = 0; i < n; i++) on_glass(in[i]);
        flags = 0;                          // unlit, no reflection pass: the picture as it is
        g_clip_new = true;                  // a draw of its own
    }
    strip_draw(flags, data, tri, tri_n);
    NativeDraw &d = g_frame.draws.back();
    if (glass) {                            // the mirror's picture, laid on as it is (DECAL), opaque
        d.rt = NATIVE_RT_MIRROR;
        d.tex = nullptr;
        d.blend = NB_OPAQUE;
        d.blend_fix = -1;
        d.alpha_test = false;
        d.tfx = 1;
        d.lod_l = 0;
        d.tex_mode = 128;                   // native.frag: sample_glass
        d.scroll_s = d.scroll_t = 0;
        g_clip_new = true;                  // and the strip after it starts another
    }
    u32 base;
    bool in_cache = false;
    if (hit) {
        in_cache = true;
        base = hit->base;
        tri_n = hit->count;
        n = hit->n;
        g_cache_hits++;
    } else if (g_geo && caching && geo_room(n, tri_n)) {
        // new to the cache, by indices: the strip's own vertices at the cache's top, the triangles' indices in the chunk
        const u32 first = g_cache_top;
        g_cache_top += n;
        if (!g_frame.fresh_runs.empty() && g_frame.fresh_runs.back().dst + g_frame.fresh_runs.back().count == first)
            g_frame.fresh_runs.back().count += n;
        else
            g_frame.fresh_runs.push_back({first, (u32)g_frame.fresh.size(), n});
        g_frame.fresh.insert(g_frame.fresh.end(), in, in + n);
        base = g_idx_at;
        u32 *out = g_idx_mirror + (base - (g_idx_end - kIdxChunk * 8));
        for (u32 i = 0; i < tri_n; i++) out[i] = first + ix[i];
        if (g_idx_dirty_hi <= g_idx_dirty_lo) g_idx_dirty_lo = base;
        g_idx_at += tri_n;
        g_idx_dirty_hi = g_idx_at;
        in_cache = true;
        strip_put(key, {base, tri_n, n});
        if (g_cache_checking) g_strip_check[key].assign(tri, tri + tri_n);
        g_cache_misses++;
    } else if (g_geo) {
        // no cache, or no room in it this frame: the frame's own vertices, the strip's and then its indices in slots of their own
        const u32 slots = (tri_n + 7) / 8;
        if (caching) g_frame.cache_short += n + slots;
        const u32 first = g_frame.dyn_base + (u32)g_frame.vertices.size();
        g_frame.vertices.insert(g_frame.vertices.end(), in, in + n);
        u32 words[88] = {};
        for (u32 i = 0; i < tri_n; i++) words[i] = first + ix[i];
        const size_t at = g_frame.vertices.size();
        g_frame.vertices.resize(at + slots);
        memcpy((void *)&g_frame.vertices[at], words, (size_t)slots * sizeof(NativeVertex));
        base = (g_frame.dyn_base + (u32)at) * 8;
    } else if (caching && g_cache_top + tri_n <= g_cache_cap) {
        // new to the cache: uploaded with this frame, a run with the strip before it if they follow each other
        in_cache = true;
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
        if (caching) g_frame.cache_short += tri_n;
        base = g_frame.dyn_base + (u32)g_frame.vertices.size();
        g_frame.vertices.insert(g_frame.vertices.end(), tri, tri + tri_n);
    }
    if (g_geo_check && g_geo && !hit && g_geo_puts.size() < 200000) {
        g_geo_puts.emplace_back();
        GeoPut &put = g_geo_puts.back();
        put.base = base, put.count = tri_n, put.data = data;
        memcpy((void *)put.tri, tri, (size_t)tri_n * sizeof(NativeVertex));
    }
    // what the address is remembered by: the key that keeps coming out the same gets its pages watched
    if (seen && !known && in_cache) {
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
    if (g_phases_on) {      // TEMP (HP2_PHASES): how the strips of a draw lie in the vertex cache, for draws by ranges instead of indices
        static u64 strips, joined, draws, cached, verts, frame_at;
        static u32 last_end, last_draw = ~0u;
        const u32 this_draw = (u32)g_frame.draws.size();
        strips++;
        verts += tri_n;
        if (in_cache) cached++;
        if (this_draw == last_draw && d.count && base == last_end) joined++;
        if (!d.count) draws++;
        last_end = base + tri_n;
        last_draw = this_draw;
        if (g_frame_no - frame_at >= 900 && pc_racing()) {
            fprintf(g_hle_log, "ranges: %u frames: a frame has %llu strips in %llu draws, %llu vertices; %.1f %% of the strips are in the cache, %.1f %% follow the strip before "
                               "them in the cache: %llu ranges a frame, %.2f a draw\n",
                    (unsigned)(g_frame_no - frame_at), strips / 900, draws / 900, verts / 900, 100.0 * cached / strips, 100.0 * joined / strips,
                    (strips - joined) / 900, (double)(strips - joined) / (double)(draws ? draws : 1));
            strips = joined = draws = cached = verts = 0;
            frame_at = g_frame_no;
        }
    }
    if (g_ranged) {
        // the strip's vertices as a range of the GPU's buffer: one range with the strip before it when it lies right after it there
        NativeRange *last = d.ranges ? g_frame.ranges.p + g_frame.ranges.n - 1 : nullptr;
        if (last && last->first + last->count == base) {
            last->count += tri_n;
        } else {
            *g_frame.ranges.grow(1) = {base, tri_n};
            d.ranges++;
        }
    } else {   // the strip's indices, one after another: room first (not cleared), then a loop the compiler can do several at a time
        u32 *out = g_frame.indices.grow(tri_n);
        for (u32 i = 0; i < tri_n; i++) out[i] = base + i;
    }
    d.count += tri_n;
    g_frame.strip_vertices += tri_n;
    g_stat.tris += tri_n / 3;
    g_stat.strips++;
    g_stat.verts += n;
    g_strip_last_base = base;
    g_strip_last_tris = tri_n;
}

// ---- the direct output: a model's strips straight to the frame (2026-11-24) -----------------------------------------------------
// eModel::RenderStrips (render_strips_native, below) handed every strip to the lists as a node of its own, and the lists' reader
// (native_read_list) then asked of every node what add_strip asks: where the strip is in the GPU's vertex cache, and whether a new
// draw begins with it. For a strip known by its address (StripSeen) the first answer is there when the strip is handed out, and the
// second is the same for all the strips handed out one after another to one list with nothing between them. So such strips go out
// as a run: one node of the port's own kind (EDMA_RUN) in the list, where the strips' nodes would be, and here the run's ranges of
// the vertex cache, put together already. The reader makes the draw's decision once and appends the ranges.
// The frame is the same as before by construction. What the reader would have found is what the run holds, or the run is not used:
// each strip was known by its pages' generations when it was handed out (sum: of the very numbers memwatch_stamp gave then), and
// the reader makes that sum again; with another one, or another generation of the cache, the strips go through add_strip one by
// one, as before. A car's strips are never runs (the mirrors' glass, the shadows, the body that moves as one: all by the node), nor
// are alpha strips (a list each) or blended ones (copies in the frame's memory).
// HP2_DIRECT=0: none, every strip a node; =1: on. HP2_DIRECTCHECK=1: every run goes the long way and is compared with what it holds.
struct RunStrip {
    u32 data, qwords;
};
struct DirectRun {
    u32 range0, ranges;                     // g_run_ranges
    u32 strip0, strips;                     // g_run_strips
    u32 tris, verts;                        // the vertices drawn; the strips' own (the statistics)
    u32 generation;                         // the cache's
    u64 sum;                                // of the strips' pages' generations
};
static std::vector<DirectRun> g_runs;       // a frame's, emptied at its end
static std::vector<NativeRange> g_run_ranges;
static std::vector<RunStrip> g_run_strips;
static u64 g_direct_runs, g_direct_strips, g_direct_slow, g_direct_stale, g_direct_checked, g_direct_bad;
static const bool kDirectDefault = true;    // on since 2026-11-24: the five demos in step, 16.7 million runs the same the long way
static const bool g_direct_check = getenv("HP2_DIRECTCHECK") != nullptr;
static bool g_in_car_render, g_car_asked_plain;     // CarRenderInfo::Render is running (its hook, below)
static u32 g_car_now;                               // and whose it is (CarRenderInfo::pCar)

static bool direct_on()
{
    // not with what reads the lists as the game's (HP2_KEEPPACKETS) or holds the output against the game's own (HP2_RSCHECK)
    static const bool on = (getenv("HP2_DIRECT") ? atoi(getenv("HP2_DIRECT")) != 0 : kDirectDefault) && g_ranged && !g_cache_off && !g_cache_checking &&
                           !getenv("HP2_KEEPPACKETS") && !getenv("HP2_RSCHECK");
    return on && g_cache_cap;
}

// a run's node of a list, read
static void add_run(const DirectRun &r, u32 flags)
{
    const RunStrip *strips = &g_run_strips[r.strip0];
    const NativeRange *in = &g_run_ranges[r.range0];
    u64 sum = 0;
    for (u32 i = 0; i < r.strips; i++) {
        const u64 stamp = memwatch_stamp(strips[i].data, strips[i].qwords * 16);
        sum += (stamp >> 32) + (u32)stamp;
    }
    const bool stands = sum == r.sum && r.generation == g_strip_generation;
    if (!stands || g_direct_check) {
        // the long way, a strip at a time; and with the run still standing it must come to what the run holds
        if (!stands) g_direct_slow++;
        bool same = true;
        u32 at = 0, used = 0, tris = 0;
        for (u32 i = 0; i < r.strips; i++) {
            const size_t draws = g_frame.draws.size();
            add_strip(strips[i].data, strips[i].qwords, flags);
            if (i && g_frame.draws.size() != draws) same = false;       // a new draw inside the run
            if (at < r.ranges && used == in[at].count) at++, used = 0;
            if (at >= r.ranges || in[at].first + used != g_strip_last_base) same = false;
            used += g_strip_last_tris;
            tris += g_strip_last_tris;
        }
        if (stands) {
            g_direct_checked++;
            if (!same || tris != r.tris || at + 1 != r.ranges || used != in[at].count)
                if (g_direct_bad++ < 10)
                    fprintf(g_hle_log, "directcheck: a run of %u strips from %08x (%u ranges, %u vertices) is not what the long way gives (%u vertices)\n", r.strips,
                            strips[0].data, r.ranges, r.tris, tris);
            if (g_direct_checked % 500000 == 0)
                fprintf(g_hle_log, "directcheck: %llu runs gone the long way too, %llu not the same\n", (unsigned long long)g_direct_checked, (unsigned long long)g_direct_bad),
                    fflush(g_hle_log);
        }
        return;
    }
    strip_draw(flags, strips[0].data, nullptr, 0);
    NativeDraw &d = g_frame.draws.back();
    u32 k = 0;
    if (d.ranges) {         // one range with the draw's last where the run begins right after it
        NativeRange &last = g_frame.ranges.p[g_frame.ranges.n - 1];
        if (last.first + last.count == in[0].first) last.count += in[0].count, k = 1;
    }
    if (k < r.ranges) {
        memcpy(g_frame.ranges.grow(r.ranges - k), in + k, (size_t)(r.ranges - k) * sizeof(NativeRange));
        d.ranges += r.ranges - k;
    }
    d.count += r.tris;
    g_frame.strip_vertices += r.tris;
    g_stat.tris += r.tris / 3;
    g_stat.strips += r.strips;
    g_stat.verts += r.verts;
    g_cache_hits += r.strips;
    g_seen_hits += r.strips;
}

// ---- the lists -------------------------------------------------------------------------------
enum { EDMA_STRIP = 0, EDMA_SETTARGET = 1, EDMA_LIGHTMATERIAL = 2, EDMA_LIGHTCONTEXT = 3, EDMA_DIRECT = 4, EDMA_POLY3D = 5,
       EDMA_SET_MATRIX_PACK = 9, EDMA_SET_LOCAL_WORLD = 12,
       EDMA_RUN = 14 };     // the port's own (the direct output, above): +6 the strips' parameter, +8 their number, +10 the frame, +12 the run

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
            // The register places the target in the GS's frame: Player 2's view is the lower half of it (its scissor starts at line
            // 224, its offset is 224 lines less than Player 1's), and a packet's vertices are places in the frame. The draw's box is
            // the target's own (NativeDraw::view), so the target's corner comes off. Without this every packet of Player 2's view
            // was drawn below it and cut away: no smoke, sparks, rockets or sun for the second player (a tester's report of 0.13).
            ox += (float)(s32)LW(target + 0x14);
            oy += (float)(s32)LW(target + 0x18);
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
    const size_t first_draw = g_frame.draws.size();
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
    if (const u32 id = LW(g_target); id == 0 || id == 3) {          // the 2D layer: whose the packet is (hud_between)
        auto who = g_packet_who.find(data);
        if (who != g_packet_who.end()) {
            for (size_t i = first_draw; i < g_frame.draws.size(); i++)
                g_hud_draws.push_back({(u32)i, who->second.group, who->second.index * 16 + (u32)std::min<size_t>(i - first_draw, 15), g_texture});
        } else {
            static const bool audit = getenv("HP2_HUDAUDIT") != nullptr;
            if (audit && g_frame.draws.size() > first_draw) g_hud_strangers[g_texture ? (const char *)host(g_texture + 8) : "(no texture)"]++;
        }
    }
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
    if (g_double_now) {
        if (mid_particle_data(data)) mid_particles(first_draw);
        else
            for (size_t i = first_draw; i < g_frame.draws.size(); i++) mid_direct(g_frame.draws[i]);
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


// ---- 120 pictures a second: a picture between each two of the race's ------------------------------
// Options > Graphics > Frame Rate above 60 (g_cfg.pictures). The game still makes 60 frames a second, its physics and its logic untouched.
// The renderer draws each frame twice: first "between", with every draw of strips halfway from where the frame before had it, then as
// the frame is; 1/120 s apart (native_vk.cpp). So the picture a player sees is half a frame behind what 60 would show.
// Halfway for a draw is the mean of two clip matrices: this frame's (g_clip), and one made of the camera of the frame before and
// the model's place in the frame before.
//  - The camera of the frame before: a target's matrix packs are made anew in the frame's memory, so a pack is known by its target
//    and its turn among that target's packs in the frame (the first, the second ...), and its quads as the last frame's lists found
//    them are kept under that (pack_seen). A pack that changed by a quarter of its size or more is a cut (another camera): no
//    halfway from that.
//  - The model's place in the frame before: a car's by the car (MidOwner), the rest by mid_resolve once the frame's lists are all
//    read: a draw is known by the address of its first strip and its target, and its local-world matrix then is the model's place
//    then. All that has no past found stands where it stands now, as the scenery does.
// The particles are places on the screen with a depth: moved with the world (mid_direct). The 2D is drawn the same both times.
struct PackSeen {
    u32 frame;
    float p[16], q[8], prev_p[16], prev_q[8];
    bool has_prev;
};
static PackSeen g_packs_seen[16][4];        // by the target's number and the pack's turn on it
static struct {
    u32 frame, n;
    u32 addr[4];
} g_target_packs[16];                       // a target's packs this frame, in the order the lists came to them
struct MidKey {
    u32 strip;
    u8 target;
    u8 taken;                               // a draw of the frame after stands at this very place: it is that draw a frame before
    u8 poly;                                // HP2_MIDAUDIT's: a POLY3D node's
    u32 draw;                               // its draw in g_frame.draws
    u32 texture;                            // HP2_MIDAUDIT's
    const PackSeen *pack;                   // its view, with the view of the frame before; null at a cut
    int half_w, half_h;
    float z_scale;
    float local[16];
};
static std::vector<MidKey> g_mid_now, g_mid_prev;
static std::vector<u32> g_mid_order_now, g_mid_order_prev;      // the same in the order of mid_less
static inline u64 mid_key(const MidKey &k) { return (u64)k.strip << 8 | k.target; }
// by the bits of the place, not its numbers: an order is all that is wanted, and one that holds whatever the floats are
static inline int mid_place_cmp(const MidKey &a, const MidKey &b)
{
    u32 x[3], y[3];
    memcpy(x, a.local + 12, 12);
    memcpy(y, b.local + 12, 12);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
static inline bool mid_less(const MidKey &a, const MidKey &b)
{
    const u64 ka = mid_key(a), kb = mid_key(b);
    return ka != kb ? ka < kb : mid_place_cmp(a, b) < 0;
}
static u64 g_mid_draws, g_mid_made, g_mid_matched, g_mid_still, g_mid_cuts, g_mid_crowds;
static bool g_node_poly;                    // the node being read is a POLY3D (HP2_MIDAUDIT)

// HP2_MIDAUDIT=1: what the picture between two frames does with every draw of a race, by the kind of thing and its texture, summed
// and written every 30 s. One run lists all that is left standing while the rest moves (the user, 2026-11-14: "some objects of
// the track still jerk; analyse all such at once").
enum { MA_CAR, MA_CAR_NEW, MA_STILL, MA_TURNED, MA_MOVED, MA_NEW, MA_CUT, MA_PARTICLES, MA_PARTICLES_LOST, MA_WORLD, MA_SUN, MA_SHEET, MA_FLAT, MA_2D, MA_OTHER_VIEW, MA_NO_VIEW, MA_KINDS };
static const char *const kMidAudit[MA_KINDS] = {"car: moved with the car",
                                                "car: new, as it stands",
                                                "strips: at the very place of the frame before",
                                                "strips: turned where it stands, halfway",
                                                "strips: halfway from where it was",
                                                "strips: no frame before, as it stands",
                                                "strips: a cut, the frame's own picture",
                                                "particles: each sprite by its particle",
                                                "particles: sprites with no particle found, moved with the world",
                                                "packets: moved with the world",
                                                "packets: a sheet at the lens, turned with the camera",
                                                "packets: a sheet at the lens, NOT MOVED",
                                                "packets: no depth test, NOT MOVED",
                                                "packets: the 2D layer, not moved",
                                                "packets: another view (mirror, reflections), once a frame",
                                                "packets: no view known, NOT MOVED"};
struct MidAuditRow {
    char name[25];
    u8 target, kind, poly;
    u64 n;
    float most, most2;
};
static std::unordered_map<u64, MidAuditRow> g_mid_audit;
static const bool g_mid_auditing = getenv("HP2_MIDAUDIT") != nullptr;
static void mid_audit(u32 texture, u32 target, int kind, bool poly, float amount = 0, float amount2 = 0)
{
    const u64 key = (u64)texture << 16 | (u64)(target & 15) << 8 | (u64)kind << 1 | (poly ? 1 : 0);
    auto it = g_mid_audit.find(key);
    if (it == g_mid_audit.end()) {
        MidAuditRow row = {};
        const char *name = texture ? (const char *)host(texture + 8) : "-";
        if (name) strncpy(row.name, name, 24);
        row.target = (u8)target, row.kind = (u8)kind, row.poly = poly;
        it = g_mid_audit.emplace(key, row).first;
    }
    it->second.n++;
    it->second.most = std::max(it->second.most, amount);
    it->second.most2 = std::max(it->second.most2, amount2);
}
static void mid_audit_write()
{
    std::vector<const MidAuditRow *> rows;
    for (const auto &r : g_mid_audit) rows.push_back(&r.second);
    std::sort(rows.begin(), rows.end(), [](const MidAuditRow *a, const MidAuditRow *b) {
        if (a->kind != b->kind) return a->kind < b->kind;
        if (a->n != b->n) return a->n > b->n;
        return strcmp(a->name, b->name) < 0;
    });
    fprintf(g_hle_log, "midaudit: table at frame %u, %zu rows\n", g_frame_no, rows.size());
    for (const MidAuditRow *r : rows)
        fprintf(g_hle_log, "midaudit: | %s | target %u | %s%s | %llu draws | %.3f | %.3f\n", kMidAudit[r->kind], r->target, r->name, r->poly ? " (poly)" : "",
                (unsigned long long)r->n, r->most, r->most2);
}

static const PackSeen *pack_seen(u32 pack)
{
    const u32 target = g_draw_target & 15;
    auto &t = g_target_packs[target];
    if (t.frame != g_frame_no) t.frame = g_frame_no, t.n = 0;
    u32 turn = 0;
    while (turn < t.n && t.addr[turn] != pack) turn++;
    if (turn == t.n) {
        if (t.n >= 4) return nullptr;
        t.addr[t.n++] = pack;
    }
    PackSeen *s = &g_packs_seen[target][turn];
    if (s->frame != g_frame_no) {
        const bool follows = s->frame + 1 == g_frame_no;
        if (follows) memcpy(s->prev_p, s->p, sizeof s->p), memcpy(s->prev_q, s->q, sizeof s->q);
        memcpy(s->p, g_now_p, sizeof s->p);
        memcpy(s->q, g_now_q, sizeof s->q);
        s->has_prev = follows;
        if (follows) {
            float differ = 0, size = 0;
            for (int i = 0; i < 16; i++) differ += fabsf(s->p[i] - s->prev_p[i]), size += fabsf(s->p[i]);
            if (differ > size * 0.25f) s->has_prev = false, g_mid_cuts++;
        }
        s->frame = g_frame_no;
    }
    return s;
}

// A car's draws (2026-11-14, the user's report of the first version: the body, the shadows and the particles jerk). What
// CarRenderInfo::Render puts into the lists is the car's: its body and wheels, and also its shadow and the flares of its lamps, which
// are polygons made anew every frame with the world's own matrix (POLY3D) and so had no "frame before" to be found by: they stood
// where the frame has them while the car was drawn halfway back, the shadow jumping under the car 60 times a second. And a wheel
// turns half a turn a frame at speed: the mean of its two matrices is a flattened wheel.
// So all a car's render adds moves as one body with the car: the nodes it adds lie in the frame's memory between the allocator's
// place before the call and after it (MidOwner, by the node's address as the lists are read), and the car's place a frame before is
// kept by its CarRenderInfo. No search for a car's draws at all.
struct MidOwner {
    u32 from, to;                           // the frame's memory its nodes are in
    u32 pcar = 0;                           // the Car it is (CarRenderInfo::pCar)
    bool moves;                             // it was drawn the frame before, and not put somewhere else since
    float move[16];                         // where a point of the car's was a frame before: its place then times its place now inverted
    int car = -1;                           // Shadows: High: the car among g_shadow_cars
};
static std::vector<MidOwner> g_owners;      // this frame's, in the memory's order
struct CarPose {
    u32 info;
    float m[16];
};
static std::vector<CarPose> g_poses_now, g_poses_prev;
struct CarMove {
    u32 car;                                // the Car, as an AcidActiveGroup has it
    float move[16];                         // MidOwner::move
};
static std::vector<CarMove> g_car_moves;    // this frame's cars that were drawn the frame before too
static u64 g_mid_owned, g_mid_direct;
static u32 owner_car(const MidOwner *o) { return o->pcar; }
static const MidOwner *g_owner_last;        // mid_owner_of's last answer: the next node is most often the same car's

// Ambient Occlusion is on in this race (Options > Graphics > Advanced, or HP2_AO)
static bool ao_on()
{
    static const char *env = getenv("HP2_AO");
    return (env ? atoi(env) : g_cfg.ao) && pc_racing();
}

// CarRenderInfo::Render is about to run: where the frame's memory is now, and the car's place (the matrix with the position in it)
static u32 mid_car_begin(Ctx &c, float pose[16])
{
    const u32 pos = c.r[6].w[0], orient = c.r[7].w[0];
    // (the side mirrors' picture needs to know the player's car too: mirror_glass_now; Ambient Occlusion which draws are a car's,
    // NativeDraw::car)
    if ((!g_double_now && !shadows_level() && !(g_cfg.side_mirrors && g_cfg.reflections) && !ao_on()) || !pos || !orient) return 0;
    read_quads(orient, pose, 4);
    memcpy(pose + 12, host(pos), 12);
    pose[3] = pose[7] = pose[11] = 0;
    pose[15] = 1;
    return LW(c.r[28].w[0] - 19388);
}

static void mid_car_end(Ctx &c, u32 info, u32 from, const float pose[16])
{
    if (!from) return;
    const u32 to = LW(c.r[28].w[0] - 19388);
    if (to <= from) return;
    MidOwner o = {};
    o.from = from;
    o.to = to;
    o.pcar = LW(info);
    if (o.pcar && o.pcar == pc_player_car()) g_pcar_now = true;
    for (const CarPose &was : g_poses_prev)
        if (was.info == info) {
            const float dx = was.m[12] - pose[12], dy = was.m[13] - pose[13], dz = was.m[14] - pose[14];
            float back[16];
            if (dx * dx + dy * dy + dz * dz < 64.0f && invert4(pose, back)) mul(o.move, was.m, back), o.moves = true;
            break;
        }
    // the frame's memory only grows within a frame, but the list is kept in order whatever comes
    if (o.moves) {
        const u32 car = LW(info);           // CarRenderInfo::pCar
        bool known = false;
        for (const CarMove &m : g_car_moves) known |= m.car == car;
        if (!known && car) {
            g_car_moves.emplace_back();
            g_car_moves.back().car = car;
            memcpy(g_car_moves.back().move, o.move, sizeof o.move);
        }
    }
    if (shadows_level()) o.car = shadow_car(c, info, pose, o.moves, o.move);
    auto at = g_owners.end();
    while (at != g_owners.begin() && (at - 1)->from > from) --at;
    g_owners.insert(at, o);
    g_owner_last = nullptr;
    bool known = false;
    for (const CarPose &now : g_poses_now) known |= now.info == info;       // a car is drawn once for each view: one pose
    if (!known) {
        g_poses_now.emplace_back();
        g_poses_now.back().info = info;
        memcpy(g_poses_now.back().m, pose, sizeof g_poses_now.back().m);
    }
}

// whose node this is
static const MidOwner *mid_owner_of(u32 node)
{
    const MidOwner *&last = g_owner_last;
    if (g_owners.empty()) return last = nullptr;
    if (last && node >= last->from && node < last->to) return last;
    size_t lo = 0, hi = g_owners.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_owners[mid].to <= node) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_owners.size() && node >= g_owners[lo].from) return last = &g_owners[lo];
    return nullptr;
}

// A target's view a frame before and now, the world to the screen: for what has no matrix of its own (mid_direct)
static const PackSeen *pack_seen_known(u32 target, u32 pack)
{
    const auto &t = g_target_packs[target & 15];
    if (t.frame != g_frame_no) return nullptr;
    for (u32 turn = 0; turn < t.n; turn++)
        if (t.addr[turn] == pack) {
            const PackSeen *s = &g_packs_seen[target & 15][turn];
            return s->frame == g_frame_no && s->has_prev ? s : nullptr;
        }
    return nullptr;
}

// A draw of GIF packets on a player's view that is tested against the depth: smoke, dust, sparks. Their vertices are places on the
// screen with a depth, so each is a place in the world, and the picture between two frames wants it where the camera halfway back
// sees that place (the particles of the first version stood still on the screen while the world behind them moved). The screen now
// goes to the world by the view's matrix inverted and back to the screen by the mean of the view's two matrices: a matrix laid
// over the draw's own. A car's (g_owner) goes with the car besides.
static void mid_direct(NativeDraw &d)
{
    const u32 id = g_draw_target;
    // the draw's box on the screen and its nearest depth
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, nearest = 0;
    for (u32 i = 0; i < d.count; i++) {
        const NativeVertex &v = g_frame.vertices[d.first + i];
        x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
        nearest = std::max(nearest, v.z);
    }
    const float view_w = (float)LW(g_target_ptr + 0x1c), view_h = (float)LW(g_target_ptr + 0x20);
    const float cover = d.count && view_w > 0 && view_h > 0 ? (x1 - x0) * (y1 - y0) / (view_w * view_h) : 0;
    const auto audit = [&](int kind) {
        if (g_mid_auditing) mid_audit(g_texture, id, kind, false, cover, nearest);
    };
    if (id < 1 || id > 4) {
        audit(id == 0 ? MA_2D : MA_OTHER_VIEW);
        return;
    }
    if (!d.depth_test) {
        audit(MA_FLAT);
        return;
    }
    u32 pack = 0;
    for (const auto &v : g_view_pack)
        if (v.target == g_target_ptr) pack = v.pack;
    const PackSeen *seen = pack ? pack_seen_known(id, pack) : nullptr;
    if (!seen) {
        audit(MA_NO_VIEW);
        return;
    }
    // The sun's rays and halo are drawn at the nearest depth there is (0.999999): a sheet over the lens, not a place in the world,
    // and taken as one it is the camera's own place, which the view of a frame before throws anywhere. Smoke 30 m off is at 0.0005;
    // 0.25 is a few centimetres from the camera.
    // The sun's own sheets (its rays, its halo) are laid round where the sun is seen, and the sun is a direction: they turn with
    // the camera as a place infinitely far would, and keep their depth. Any other sheet is left as the frame has it.
    const bool sheet = nearest > 0.25f;
    const bool sun = sheet && g_texture && !strncmp((const char *)host(g_texture + 8), "SUN", 3);
    if (sheet && !sun) {
        audit(MA_SHEET);
        return;
    }
    audit(sun ? MA_SUN : MA_WORLD);
    const int half_w = LW(g_target_ptr + 0x1c) / 2, half_h = LW(g_target_ptr + 0x20) / 2;
    const float z_scale = depth_scale(g_target_ptr);
    // In doubles from the pack's own floats: the view's matrix has entries from 1e-17 to 2, and what is wanted is the small
    // difference of two of them that are nearly the same. (In floats the sun's sprites, far off, lost their rays in the picture between.)
    double now[16], prev[16], back[16];
    clip_matrix_d(now, seen->p, seen->q, nullptr, half_w, half_h, z_scale);
    clip_matrix_d(prev, seen->prev_p, seen->prev_q, g_owner && g_owner->moves ? g_owner->move : nullptr, half_w, half_h, z_scale);
    if (!invert4d(now, back)) return;
    double over[16];                        // (the view before times the view now inverted, plus one) over two: all but the unit matrix
    for (int col = 0; col < 4; col++)
        for (int r = 0; r < 4; r++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) sum += prev[k * 4 + r] * back[col * 4 + k];
            over[col * 4 + r] = 0.5 * (sum + (col == r ? 1.0 : 0.0));
        }
    double own[16];                         // the draw's own matrix: the screen's pixels to the view's clip space
    for (int i = 0; i < 16; i++) own[i] = d.clip[i];
    if (sun) {
        // every vertex at the depth of what is infinitely far: z = a w + b there, and w has no end, so z over w is a
        int k = 0;
        for (int i = 1; i < 3; i++)
            if (fabs(now[3 + 4 * i]) > fabs(now[3 + 4 * k])) k = i;
        if (now[3 + 4 * k] == 0) return;
        own[8] = own[9] = own[10] = own[11] = 0;
        own[14] = now[2 + 4 * k] / now[3 + 4 * k];
    }
    double out[16];
    for (int col = 0; col < 4; col++)
        for (int r = 0; r < 4; r++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) sum += over[k * 4 + r] * own[col * 4 + k];
            out[col * 4 + r] = sum;
        }
    if (sun)                                // and back at the lens: its depth as it was, whatever w came out
        for (int col = 0; col < 4; col++) out[col * 4 + 2] = (double)nearest * (double)d.clip[10] * out[col * 4 + 3];
    for (int i = 0; i < 16; i++) d.clip_mid[i] = (float)out[i];
    d.has_mid = true;
    g_mid_direct++;
}

// ---- the particles, each by its own move (2026-11-14, the user: "take the particles and the smoke out of the zone of risk") ----
// A particle is a place in the world that moves: smoke drifts, sparks and gravel fly, and a puff is born at the wheel of a car that
// is itself drawn half a step back in the picture between. Moved "with the world" as one sheet, a spark stood still between two
// frames and a new puff showed half a car's step ahead of its wheel. Now each sprite goes halfway from where its particle was:
//  - AcidEffectSystem::RenderEffects (0x128da0) makes a packet of sprites for each group of particles in the frame's memory and
//    attaches it (eAttachDirectData): what lies in the memory that call took is a packet of particles (mid_particle_data), with
//    no names asked;
//  - the systems' particles are read as RenderEffects begins, once a frame: each one's place (AcidParticle +16), its life (+8) and
//    its group's car (AcidActiveGroup +16), kept by the particle's address till the next frame;
//  - a sprite's middle on the screen with its depth is a place in the world; the nearest particle to it is its particle. If that
//    one was there a frame before (the same slot, its life not gone up), the sprite goes halfway from its place then. Born since,
//    on a car: from where the car had that place a frame before (it moves with the car). Born since, on nothing: where it is;
//  - the sprites' vertices for the picture between are a second copy after the frame's own (NativeDraw::mid_first), shifted on
//    the screen; the draw's matrix stays.
struct MidParticle {
    u32 at;                                 // the AcidParticle
    float x, y, z;
    s16 life;
    u32 car;
};
static std::vector<MidParticle> g_parts_now, g_parts_prev;      // the frame before's in the order of their addresses
static u32 g_parts_systems[4], g_parts_systems_n;               // the systems read this frame
struct MidRange {
    u32 from, to;
};
static std::vector<MidRange> g_particle_ranges;                 // the frame's memory RenderEffects took, this frame
static u64 g_mid_sprites, g_mid_sprites_tracked, g_mid_sprites_born, g_mid_sprites_lost;

// RenderEffects is about to run for a view: the system's particles (once a frame), and where the frame's memory is
static u32 g_fx_system, g_fx_view;       // TEMP HP2_FXLOG
static u32 g_fx_from;                    // where the frame's memory stood as RenderEffects began (HP2_PARTCHECK)
u32 native_particles_begin(Ctx &c)
{
    g_fx_system = c.r[4].w[0], g_fx_view = c.r[5].w[0];
    g_fx_from = LW(c.r[28].w[0] - 19388);
    if (!g_double_now) return 0;
    const u32 system = c.r[4].w[0];
    bool known = false;
    for (u32 i = 0; i < g_parts_systems_n; i++) known |= g_parts_systems[i] == system;
    if (!known && g_parts_systems_n < 4 && system) {
        g_parts_systems[g_parts_systems_n++] = system;
        // AcidEffectSystem: ActiveGroupList at +0 (a bTList: the list is its own last node); AcidActiveGroup: pCar +16, ParticleList +20
        u32 groups = 0;
        for (u32 group = LW(system); group && group != system && groups < 1024; group = LW(group), groups++) {
            const u32 car = LW(group + 16), list = group + 20;
            u32 n = 0;
            for (u32 part = LW(list); part && part != list && n < 2048; part = LW(part), n++) {
                MidParticle m;
                m.at = part;
                memcpy(&m.x, host(part + 16), 12);
                m.life = (s16)LH(part + 8);
                m.car = car;
                g_parts_now.push_back(m);
            }
        }
    }
    return LW(c.r[28].w[0] - 19388);
}

void native_particles_end(Ctx &c, u32 from)
{
    {   // TEMP HP2_FXLOG=1: what RenderEffects did for each view
        static const bool on = getenv("HP2_FXLOG") != nullptr;
        if (on && g_frame_no % 120 == 0) {
            const u32 system = g_fx_system, view = g_fx_view;
            u32 groups = 0, with_car = 0, parts = 0, mask = 0xffffffff, first_car = 0;
            for (u32 group = LW(system); group && group != system && groups < 1024; group = LW(group), groups++) {
                const u32 car = LW(group + 16), list = group + 20;
                if (car) with_car++, mask &= LW(car + 1300), first_car = first_car ? first_car : car;
                for (u32 part = LW(list); part && part != list && parts < 4096; part = LW(part)) parts++;
            }
            fprintf(g_hle_log, "fxlog: frame %u system %08x view %u (target %u): %u groups (%u on cars, their view masks and-ed %08x), %u particles; frame memory at %08x, visible particles %d%c",
                    g_frame_no, system, view ? LW(view) : 99, LW(pc_call(0x105838)) ? LW(LW(pc_call(0x105838))) : 99, groups, with_car, mask, parts, LW(c.r[28].w[0] - 19388),
                    (int)LW(system + 88), 10);
        }
    }
    port_sprites_check(from ? from : g_fx_from, LW(c.r[28].w[0] - 19388));
    if (!from) return;
    const u32 to = LW(c.r[28].w[0] - 19388);
    if (to > from) g_particle_ranges.push_back({from, to});
}

static bool mid_particle_data(u32 data)
{
    for (const MidRange &r : g_particle_ranges)
        if (data >= r.from && data < r.to) return true;
    return false;
}

// the draws from first_draw on are one packet of particles' sprites
static void mid_particles(size_t first_draw)
{
    const u32 id = g_draw_target;
    if ((id < 1 || id > 4) && id != 7) {
        if (g_mid_auditing)
            for (size_t i = first_draw; i < g_frame.draws.size(); i++) mid_audit(g_texture, id, MA_OTHER_VIEW, false);
        return;
    }
    u32 pack = 0;
    for (const auto &v : g_view_pack)
        if (v.target == g_target_ptr) pack = v.pack;
    const PackSeen *seen = pack ? pack_seen_known(id, pack) : nullptr;
    if (!seen) {
        if (g_mid_auditing)
            for (size_t i = first_draw; i < g_frame.draws.size(); i++) mid_audit(g_texture, id, MA_NO_VIEW, false);
        return;
    }
    const int half_w = LW(g_target_ptr + 0x1c) / 2, half_h = LW(g_target_ptr + 0x20) / 2;
    const float z_scale = depth_scale(g_target_ptr);
    double now[16], prev[16], back[16];
    clip_matrix_d(now, seen->p, seen->q, nullptr, half_w, half_h, z_scale);
    clip_matrix_d(prev, seen->prev_p, seen->prev_q, nullptr, half_w, half_h, z_scale);
    if (!invert4d(now, back)) return;
    const auto apply = [](const double *m, const double *v, double *out) {
        for (int r = 0; r < 4; r++) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    };
    // Where this view has each particle on the screen, made when a sprite first needs it: for a sprite whose depth says nothing
    // (the mirror's dust is all at depth 0, the lightning far off is at the far end), its particle is the one drawn where it is.
    static std::vector<float> on_screen;
    bool screened = false;
    const size_t draws = g_frame.draws.size();
    for (size_t di = first_draw; di < draws; di++) {
        NativeDraw &d = g_frame.draws[di];
        if (!d.count || d.count % 6 || d.clip[0] == 0 || d.clip[5] == 0 || d.clip[10] == 0) {
            mid_direct(d);                  // not sprites as two triangles each: as one sheet, with the world
            continue;
        }
        const u32 m0 = (u32)g_frame.vertices.size();
        g_frame.vertices.resize(m0 + d.count);
        const NativeVertex *in = &g_frame.vertices[d.first];
        NativeVertex *out = &g_frame.vertices[m0];
        memcpy(out, in, d.count * sizeof(NativeVertex));
        u32 tracked = 0, born = 0, lost = 0;
        for (u32 at = 0; at + 6 <= d.count; at += 6) {
            // the sprite's middle: of its box on the screen, at the mean of its depths
            float x0 = in[at].x, x1 = x0, y0 = in[at].y, y1 = y0, zsum = 0;
            for (int k = 0; k < 6; k++) {
                x0 = std::min(x0, in[at + k].x), x1 = std::max(x1, in[at + k].x);
                y0 = std::min(y0, in[at + k].y), y1 = std::max(y1, in[at + k].y);
                zsum += in[at + k].z;
            }
            const double mid[4] = {0.5 * (x0 + x1), 0.5 * (y0 + y1), zsum / 6.0, 1.0};
            const double clip[4] = {d.clip[0] * mid[0] + d.clip[12], d.clip[5] * mid[1] + d.clip[13], d.clip[10] * mid[2] + d.clip[14], 1.0};
            double w4[4];
            apply(back, clip, w4);
            if (fabs(w4[3]) < 1e-30) continue;
            double here[3] = {w4[0] / w4[3], w4[1] / w4[3], w4[2] / w4[3]};
            // its particle: the nearest, within 2 m, or 5 % of how far off it is (the depth buffer's steps grow with the distance)
            const MidParticle *mine = nullptr;
            bool by_screen = false;         // found by where it is drawn, not by its depth: the sprite keeps its depth
            const double off = 0.05 / fabs(w4[3]);      // the clip space's w of the place: about its distance
            double least = std::max(4.0, std::min(off * off, 400.0));
            for (const MidParticle &part : g_parts_now) {
                const double dx = part.x - here[0], dy = part.y - here[1], dz = part.z - here[2], far = dx * dx + dy * dy + dz * dz;
                if (far < least) least = far, mine = &part;
            }
            if (!mine) {
                if (!screened) {
                    screened = true;
                    on_screen.resize(g_parts_now.size() * 2);
                    for (size_t i = 0; i < g_parts_now.size(); i++) {
                        const double at4[4] = {g_parts_now[i].x, g_parts_now[i].y, g_parts_now[i].z, 1.0};
                        double c4[4];
                        apply(now, at4, c4);
                        const bool ahead = c4[3] > 1e-6;
                        on_screen[i * 2] = ahead ? (float)((c4[0] / c4[3] - d.clip[12]) / d.clip[0]) : 1e9f;
                        on_screen[i * 2 + 1] = ahead ? (float)((c4[1] / c4[3] - d.clip[13]) / d.clip[5]) : 1e9f;
                    }
                }
                float nearest = 2.25f;      // a pixel and a half of the GS's
                for (size_t i = 0; i < g_parts_now.size(); i++) {
                    const float dx = on_screen[i * 2] - (float)mid[0], dy = on_screen[i * 2 + 1] - (float)mid[1], far = dx * dx + dy * dy;
                    if (far < nearest) nearest = far, mine = &g_parts_now[i];
                }
                if (mine) here[0] = mine->x, here[1] = mine->y, here[2] = mine->z, by_screen = true;
            }
            double was[4] = {here[0], here[1], here[2], 1.0};
            if (!mine) {
                lost++;
                if (g_mid_auditing) mid_audit(g_texture, id, MA_PARTICLES_LOST, false, (float)(x1 - x0), (float)mid[2]);
            } else {
                const MidParticle *before = nullptr;
                size_t lo = 0, hi = g_parts_prev.size();
                while (lo < hi) {
                    const size_t m = (lo + hi) / 2;
                    if (g_parts_prev[m].at < mine->at) lo = m + 1;
                    else hi = m;
                }
                if (lo < g_parts_prev.size() && g_parts_prev[lo].at == mine->at && g_parts_prev[lo].life >= mine->life) before = &g_parts_prev[lo];
                if (before) {
                    const double mx = before->x - mine->x, my = before->y - mine->y, mz = before->z - mine->z;
                    if (mx * mx + my * my + mz * mz < 9.0) was[0] += mx, was[1] += my, was[2] += mz, tracked++;    // 3 m a frame and more: another one in its slot
                    else before = nullptr;
                }
                if (!before) {
                    born++;
                    if (mine->car)
                        for (const CarMove &cm : g_car_moves)
                            if (cm.car == mine->car) {
                                const double p3[3] = {here[0], here[1], here[2]};
                                for (int r = 0; r < 3; r++) was[r] = cm.move[r] * p3[0] + cm.move[4 + r] * p3[1] + cm.move[8 + r] * p3[2] + cm.move[12 + r];
                                break;
                            }
                }
            }
            // halfway: the mean of where the view before had its place then and where this view has it now
            // (both in the views' own clip space, w and all: the mean of the two is what a draw of strips gets from its two matrices.
            // The first try took this view's side from the sprite's corner, where w is 1, and the mean was all the frame before's.)
            double then[4], here_now[4];
            const double here4[4] = {here[0], here[1], here[2], 1.0};
            apply(prev, was, then);
            apply(now, here4, here_now);
            const double half[4] = {0.5 * (then[0] + here_now[0]), 0.5 * (then[1] + here_now[1]), 0.5 * (then[2] + here_now[2]), 0.5 * (then[3] + here_now[3])};
            if (half[3] < 1e-6 || here_now[3] < 1e-6) continue;     // behind the camera: as the frame has it
            // the sprite moves on the screen by as much as its place does
            const float sx = (float)((half[0] / half[3] - here_now[0] / here_now[3]) / d.clip[0]), sy = (float)((half[1] / half[3] - here_now[1] / here_now[3]) / d.clip[5]);
            const float sz = by_screen ? 0.0f : (float)((half[2] / half[3] - here_now[2] / here_now[3]) / d.clip[10]);
            if (!(fabsf(sx) < 4096.0f && fabsf(sy) < 4096.0f)) continue;
            for (int k = 0; k < 6; k++) {
                out[at + k].x += sx, out[at + k].y += sy;
                out[at + k].z = std::max(0.0f, std::min(1.0f, out[at + k].z + sz));
            }
        }
        d.mid_first = m0;
        d.mid_verts = true;
        g_mid_direct++;
        g_mid_sprites += d.count / 6, g_mid_sprites_tracked += tracked, g_mid_sprites_born += born, g_mid_sprites_lost += lost;
        if (g_mid_auditing) mid_audit(g_texture, id, MA_PARTICLES, false, (float)tracked, (float)born);
    }
}

// ---- Particles: Advanced and High (Options > Graphics > Advanced; the user, 2026-11-14) ---------------------------------
// The PS2's: AcidEffectSystem::RenderEffects (0x128da0) makes a sprite of every particle a view shows and hands the view a packet of
// them for each group (eAttachDirectData, 0x110378). A sprite's middle is cut to a whole pixel of the 512 across and its corners to
// sixteenths: at ten times the PS2's lines smoke comes on in steps, and the picture between two frames has to find each sprite's
// particle again by where it was drawn (mid_particles).
// Advanced: the port makes the sprites instead, by the same rules read from that function (which groups a view shows, the place on the
// screen by the view's own matrices, the size and the spin and the colour by the emitter's curves over the particle's life, the cap
// in pixels, the far limit) with nothing cut to a grid, and knows each sprite's particle. It still asks the game to attach a packet for
// each group, an empty one: the game sorts it among the rest that is transparent and sets its texture as it would its own, and
// the list's node, known by the packet's address, is where the sprites go in (port_sprites_emit).
// High (the menu's Advanced since 2026-11-17): particles of the port's own, as many and from as far as the game's, made into sprites
// by the GPU ("Particles: High" further down).
// The game's code for the particles runs at every level, untouched; they touch nothing else in the game.
// The setting has two (the user, 2026-11-17): Original, the game's own sprites as its own code makes them, and Advanced: the game's
// own particles, every one of them and nothing else, made into sprites by the port (level 1, port_sprites_build) and drawn with
// soft edges (the ball of smoke: native.frag's SOFT). For half a day Advanced was level 2, particles of the port's own that the
// GPU moves ("High" below): as many and from as far as the game's, and still not the game's - its dust and its tyres' smoke raced
// each other where the game's do not (the user: "the shader cannot do the original's logic one to one; the soft edges are the
// one change I want"). Level 2 is left for HP2_PARTICLES=2.
static int particles_level()
{
    static const char *env = getenv("HP2_PARTICLES");
    return env ? atoi(env) : g_cfg.particles ? 1 : 0;
}
// How many of the port's own particles are born and from how far, by the game's own: as many and from as far. They were twice
// both (till 2026-11-17), and far off, where the game never showed a particle, the dust and the smoke of a car were a few pixels
// each of two colours over one another and never still (the user: "the particles race"; at the game's own numbers they do not).
static const float kPartDensity = 1.0f, kPartFar = 1.0f;
// NativeDraw::flags of a draw of the port's sprites of the game's particles: its vertices have the soft sprite's own (native.vert,
// SOFT_2D: the corner in the normal's place, the radius in metres in 1 / w's) and its light the view's (port_sprites_build)
static const u32 kSoft2dFlag = 0x40000;

struct PortBatch {
    u32 marker;                             // the empty packet in the frame's memory; 0: not attached (HP2_PARTCHECK)
    u32 first, count;                       // sprites: six of g_port_verts and one of g_port_parts each
    u32 view, group;
    int gpu = -1, light = -1;               // High: the port's own group (g_gpu_groups) and its block among the frame's lights
};
static std::vector<PortBatch> g_port_batches;
static bool gpu_sprites_emit(const PortBatch &b);
static void gpu_build(Ctx &c, u32 system, u32 view);
static void gpu_frame();
static std::vector<NativeVertex> g_port_verts;
static std::vector<MidParticle> g_port_parts;
static u64 g_port_sprites_n, g_port_groups_n;
static bool g_port_checking;
static u32 g_check_batches, g_check_verts, g_check_parts;

static void port_sprites_frame()
{
    gpu_frame();
    if (g_port_groups_n && g_frame_no % 600 == 0 && getenv("HP2_NATIVELOG"))
        fprintf(g_hle_log, "native: particles, level %d: %llu sprites in %llu packets of the port's so far%c", particles_level(), (unsigned long long)g_port_sprites_n,
                (unsigned long long)g_port_groups_n, 10);
    g_port_batches.clear();
    g_port_verts.clear();
    g_port_parts.clear();
}

// a matrix of the game's by a place, w 1 (eMulVector, eRotTransPers, eGetClipFlags)
static inline void part_mul(const float *m, const float *p, float *out)
{
    for (int i = 0; i < 4; i++) out[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
}

static inline s32 part_int(float v)         // cvt.w.s, with what cannot be an integer left at 0
{
    return v > -2.0e9f && v < 2.0e9f ? (s32)v : 0;
}

static void attach_direct(u32 tex, float key, u32 data, u32 qwords)     // eAttachDirectData(TextureInfo *, float, void *, ushort)
{
    Ctx &c = *g_pc_ctx;
    Ctx saved = c;
    c.r[4].d[0] = (u64)(s64)(s32)tex;
    c.r[5].d[0] = (u64)(s64)(s32)data;
    c.r[6].d[0] = qwords;
    c.f[12].f = key;
    c.r[29].d[0] -= 0x200;
    ps2_call(c, 0x110378);
    c = saved;
}

// whether a view shows a group that is on a car (RenderEffects, 0x128f20 on; the rules are spelled out below)
static bool part_group_shown(u32 car, u32 ed, u32 view_id, u32 camera, u32 hidden)
{
    if (!car) return true;
    if (!((LW(car + 1300) >> (view_id & 31)) & 1)) return false;
    const u32 place = LW(ed + 88) - 1;
    const u32 rule = place < 26 ? LW(0x2ad3b0 + place * 4) : 0;
    if (rule == 0x128f78) return !(view_id == 3 || hidden == car);
    if (rule == 0x128f94) return !(view_id != 3 && hidden == car);
    if (rule == 0x128fb0) return !(hidden == car && LW(camera + 448));
    return true;
}

// the empty packet in the frame's memory that stands for a group of the port's in the game's lists: one tag, no loops, the end of
// the packet. 0: no room.
static u32 part_marker(u32 gp)
{
    const u32 cur = (LW(gp - 19388) + 15) & ~15u, end = LW(gp - 19384);
    if (cur < 16 || end < cur + 256) return 0;
    SW(cur, 0x8000), SW(cur + 4, 0), SW(cur + 8, 0), SW(cur + 12, 0);
    SW(gp - 19388, cur + 16);
    return cur;
}

// One view's sprites, a batch for each group of particles it shows. RenderEffects' rules:
//  - a group on a car is shown where the car's word +1300 has the view's bit, and by where on the car it is (EmitterData +88, a
//    table of 26 at 0x2ad3b0): not in the mirror nor on the car the camera sits in (0x128f78); in the mirror, elsewhere not on
//    that car (0x128f94); on that car only while the camera's word +448 is clear (0x128fb0); the rest always;
//  - a particle: its place by the view's matrix +72 inside the view (eGetClipFlags), its depth z by the matrix +68, its place on
//    the screen by the matrix +76 (eRotTransPers); its age 1 - life / the emitter's Life; size and spin by the curve at +320,
//    colour by the one at +256 (hermite_parameter: the matrix by t^3, t^2, t, 1); size in pixels = size * H / depth, H from the
//    camera's angle (CalculateH, 0x104448), no more than MaxPixelSize (+140, 0 is 512), shown above one pixel and nearer than
//    FarClip (+128); a sprite is its pixels wide and 7/8 of them high, turned by the particle's angle plus or minus the spin
//    (the angle's low bit says which); its picture is one cell of the texture's sheet (+25, +26: powers of two) by the frame +31;
//  - the depth is the bits of the float z the matrix +76 gives, cut as the game cuts them (28 bits, shifted by gp -23440).
static void port_sprites_build(Ctx &c, u32 system, u32 view, bool attach)
{
    const u32 gp = c.r[28].w[0];
    const u32 view_id = LW(view), camera = LW(view + 56);
    const u32 a_view = LW(view + 68), a_clip = LW(view + 72), a_screen = LW(view + 76);
    if (!camera || !a_view || !a_clip || !a_screen) return;
    float m_view[16], m_clip[16], m_screen[16], turn, quarter;
    memcpy(m_view, host(a_view), 64);
    memcpy(m_clip, host(a_clip), 64);
    memcpy(m_screen, host(a_screen), 64);
    memcpy(&turn, host(gp - 32080), 4);
    memcpy(&quarter, host(gp - 32076), 4);
    const float tangent = tanf((float)(LH(camera + 132) >> 1) * (6.28318530718f / 65536.0f));
    const float focal = tangent == 0 ? 10.0f : (float)((s32)LW(gp - 25108) >> 1) / tangent;
    const u32 hidden = LW(camera + 444);
    const u32 only = (s8)LB(system + 77) ? LW(system + 64) : 0;
    const u32 shift = LW(gp - 23440) & 31;
    const float far_k = particles_level() >= 2 ? kPartFar : 1.0f;
    // The soft sprite (native.frag, SOFT): what the shaders need of this view, as one of the frame's lights, and of each sprite in
    // its vertices (which corner, and its radius in metres where the strips have their 1 / w: kSoft2dFlag says so). The screen's
    // rows for Z and W go by the distance ahead alone: each is the view's row for that distance times a number, plus a number.
    int soft_light = -1;
    if (!g_port_checking && g_frame.lights.size() < kNativeLightsMost) {
        int axis = 0;
        for (int i = 1; i < 3; i++)
            if (fabsf(m_view[i * 4 + 2]) > fabsf(m_view[axis * 4 + 2])) axis = i;
        const float ahead = m_view[axis * 4 + 2];
        if (fabsf(ahead) > 1e-12f) {
            const float kz = m_screen[axis * 4 + 2] / ahead, kw = m_screen[axis * 4 + 3] / ahead;
            const float cz = m_screen[14] - kz * m_view[14], cw = m_screen[15] - kw * m_view[14];
            const float nearest = 4.0f * kw - kz;       // Z / W is 4 at the nearest the GS's depth holds
            soft_light = (int)g_frame.lights.size();
            g_frame.lights.emplace_back();
            float *q = &g_frame.lights[soft_light].q[0][0];
            memset(q, 0, sizeof(NativeLight));
            q[0] = kz, q[1] = cz, q[2] = kw, q[3] = cw;
            q[4] = fabsf(nearest) > 1e-20f ? std::max((cz - 4.0f * cw) / nearest, 0.0f) : 0.0f;
            q[5] = (float)(1u << (shift + 2)), q[6] = (float)shift;
        }
    }
    u32 groups = 0;
    for (u32 group = LW(system); group && group != system && groups < 8192; group = LW(group), groups++) {
        const u32 car = LW(group + 16), ed = LW(group + 8);
        if (!ed) continue;
        if (!part_group_shown(car, ed, view_id, camera, hidden)) continue;
        const u32 tex = LW(ed + 20);
        if (!tex) continue;
        if (only && LW(group + 12) != only) continue;
        float life_all, max_px, far;
        memcpy(&life_all, host(ed + 56), 4);
        memcpy(&max_px, host(ed + 140), 4);
        memcpy(&far, host(ed + 128), 4);
        if (max_px == 0) max_px = 512.0f;
        far *= far_k;
        float colour[16], extra[16];
        memcpy(colour, host(ed + 256), 64);
        memcpy(extra, host(ed + 320), 64);
        const u32 pow_w = LB(ed + 25) & 31, pow_h = LB(ed + 26) & 31;
        const float du = 1.0f / (float)(1u << pow_w), dv = 1.0f / (float)(1u << pow_h);
        const u32 list = group + 20, first = (u32)g_port_parts.size();
        double depth_sum = 0;
        u32 n = 0;
        for (u32 part = LW(list); part && part != list && n < 16384; part = LW(part), n++) {
            const float age = 1.0f - (float)(s16)LH(part + 8) * (1.0f / 1024.0f) * (1.0f / life_all);
            float pos[3], v4[4], cam[4], scr[4];
            memcpy(pos, host(part + 16), 12);
            part_mul(m_clip, pos, v4);
            // The game drops a particle whose middle is outside the view. A sprite of smoke near the camera is hundreds of pixels
            // across and went out all at once as its middle crossed the picture's edge. The port's sprites are dropped by their
            // depth alone and the picture's edge cuts them (the check against the game's packets keeps the game's rule)
            const float lim = fabsf(v4[3]);
            if (!(fabsf(v4[2]) <= lim)) continue;
            if (g_port_checking && !(fabsf(v4[0]) <= lim && fabsf(v4[1]) <= lim)) continue;
            part_mul(m_view, pos, cam);
            part_mul(m_screen, pos, scr);
            if (scr[3] == 0) continue;
            const float q = 1.0f / scr[3], sx = scr[0] * q, sy = scr[1] * q, sz = scr[2] * q;
            if (!(fabsf(sx) < 16384.0f && fabsf(sy) < 16384.0f)) continue;
            const float t2 = age * age, t3 = t2 * age;
            const float size = extra[0] * t3 + extra[4] * t2 + extra[8] * age + extra[12];
            const float spin = extra[1] * t3 + extra[5] * t2 + extra[9] * age + extra[13];
            float pixels = size > 0 ? size * focal / cam[2] : 0;
            if (max_px < pixels) pixels = max_px;
            if (!(pixels > 1.0f) || !(cam[2] < far)) continue;
            const u32 angle = LH(part + 10), delta = (u32)part_int(spin) & 0xffff;
            const u32 rot = ((angle & 1) ? angle - delta : angle + delta) & 0xffff;
            float corner[4][2];
            if (!rot) {
                const float hx = pixels * 0.5f, hy = pixels * 0.4375f;
                for (int i = 0; i < 4; i++) corner[i][0] = (i & 1) ? hx : -hx, corner[i][1] = (i & 2) ? hy : -hy;
            } else {
                const float th = (float)rot * turn, sn = sinf(th), cs = sinf(th + quarter);
                for (int i = 0; i < 4; i++) {
                    const float px = (i & 1) ? pixels : -pixels, py = (i & 2) ? pixels : -pixels;
                    corner[i][0] = (cs * px - sn * py) * 0.5f;
                    corner[i][1] = (sn * px + cs * py) * 0.4375f;
                }
            }
            u8 rgba[4];
            for (int k = 0; k < 4; k++) {
                const float v = colour[k] * t3 + colour[4 + k] * t2 + colour[8 + k] * age + colour[12 + k];
                rgba[k] = v > 0 ? (v < 255.0f ? (u8)(int)v : 255) : 0;
            }
            const u32 frame = LB(part + 31);
            const float u0 = (float)(frame & ((1u << pow_w) - 1)) * du, v0 = (float)((frame >> pow_w) & ((1u << pow_h) - 1)) * dv;
            u32 zbits;
            memcpy(&zbits, &sz, 4);
            const float z = (float)(((zbits & 0x0fffffff) >> shift) & 0xffffff) / 16777216.0f;
            NativeVertex quad[4];
            for (int i = 0; i < 4; i++) {
                NativeVertex &v = quad[i];
                v = NativeVertex{};
                v.x = sx + corner[i][0], v.y = sy + corner[i][1], v.z = z;
                v.r = rgba[0], v.g = rgba[1], v.b = rgba[2], v.a = rgba[3];
                v.s = u0 + ((i & 1) ? du : 0), v.t = v0 + ((i & 2) ? dv : 0);
                v.w = 1;
                if (soft_light >= 0) {
                    v.nx = (i & 1) ? 127 : -127, v.ny = (i & 2) ? 127 : -127;
                    v.w = 0.5f * pixels * cam[2] / focal;
                }
            }
            static const int kOrder[6] = {0, 1, 2, 1, 2, 3};
            for (int i : kOrder) g_port_verts.push_back(quad[i]);
            MidParticle m;
            m.at = part;
            m.x = pos[0], m.y = pos[1], m.z = pos[2];
            m.life = (s16)LH(part + 8);
            m.car = car;
            g_port_parts.push_back(m);
            depth_sum += (double)part_int(cam[2]);
        }
        const u32 count = (u32)g_port_parts.size() - first;
        if (!count) continue;
        PortBatch b = {0, first, count, view, group};
        b.light = soft_light;
        if (attach) {
            const u32 cur = part_marker(gp);
            if (!cur) {
                g_port_parts.resize(first);
                g_port_verts.resize((size_t)first * 6);
                break;
            }
            b.marker = cur;
            g_port_batches.push_back(b);
            attach_direct(tex, (float)(depth_sum / (double)count), cur, 1);
        } else {
            g_port_batches.push_back(b);
        }
    }
}

// the picture between: each sprite halfway from where its particle was a frame before, as mid_particles has it, the particle known
static void port_sprites_mid(const PortBatch &b, NativeDraw &d)
{
    const u32 id = g_draw_target;
    if ((id < 1 || id > 4) && id != 7) return;
    u32 pack = 0;
    for (const auto &v : g_view_pack)
        if (v.target == g_target_ptr) pack = v.pack;
    const PackSeen *seen = pack ? pack_seen_known(id, pack) : nullptr;
    if (!seen) return;
    const int half_w = LW(g_target_ptr + 0x1c) / 2, half_h = LW(g_target_ptr + 0x20) / 2;
    const float z_scale = depth_scale(g_target_ptr);
    double now[16], prev[16];
    clip_matrix_d(now, seen->p, seen->q, nullptr, half_w, half_h, z_scale);
    clip_matrix_d(prev, seen->prev_p, seen->prev_q, nullptr, half_w, half_h, z_scale);
    if (d.clip[0] == 0 || d.clip[5] == 0 || d.clip[10] == 0) return;
    const auto apply = [](const double *m, const double *v, double *out) {
        for (int r = 0; r < 4; r++) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    };
    const u32 m0 = (u32)g_frame.vertices.size();
    g_frame.vertices.resize(m0 + d.count);
    const NativeVertex *in = &g_frame.vertices[d.first];
    NativeVertex *out = &g_frame.vertices[m0];
    memcpy(out, in, d.count * sizeof(NativeVertex));
    u32 tracked = 0, born = 0;
    for (u32 i = 0; i < b.count; i++) {
        const MidParticle &mine = g_port_parts[b.first + i];
        const double here4[4] = {mine.x, mine.y, mine.z, 1.0};
        double was[4] = {mine.x, mine.y, mine.z, 1.0};
        const MidParticle *before = nullptr;
        size_t lo = 0, hi = g_parts_prev.size();
        while (lo < hi) {
            const size_t m = (lo + hi) / 2;
            if (g_parts_prev[m].at < mine.at) lo = m + 1;
            else hi = m;
        }
        if (lo < g_parts_prev.size() && g_parts_prev[lo].at == mine.at && g_parts_prev[lo].life >= mine.life) before = &g_parts_prev[lo];
        if (before) {
            const double mx = before->x - mine.x, my = before->y - mine.y, mz = before->z - mine.z;
            if (mx * mx + my * my + mz * mz < 9.0) was[0] += mx, was[1] += my, was[2] += mz, tracked++;    // 3 m a frame and more: another one in its slot
            else before = nullptr;
        }
        if (!before) {
            born++;
            if (mine.car)
                for (const CarMove &cm : g_car_moves)
                    if (cm.car == mine.car) {
                        for (int r = 0; r < 3; r++) was[r] = cm.move[r] * here4[0] + cm.move[4 + r] * here4[1] + cm.move[8 + r] * here4[2] + cm.move[12 + r];
                        break;
                    }
        }
        double then[4], here_now[4];
        apply(prev, was, then);
        apply(now, here4, here_now);
        const double half[4] = {0.5 * (then[0] + here_now[0]), 0.5 * (then[1] + here_now[1]), 0.5 * (then[2] + here_now[2]), 0.5 * (then[3] + here_now[3])};
        if (half[3] < 1e-6 || here_now[3] < 1e-6) continue;
        const float sx = (float)((half[0] / half[3] - here_now[0] / here_now[3]) / d.clip[0]), sy = (float)((half[1] / half[3] - here_now[1] / here_now[3]) / d.clip[5]);
        const float sz = (float)((half[2] / half[3] - here_now[2] / here_now[3]) / d.clip[10]);
        if (!(fabsf(sx) < 4096.0f && fabsf(sy) < 4096.0f)) continue;
        for (int k = 0; k < 6; k++) {
            NativeVertex &v = out[(size_t)i * 6 + k];
            v.x += sx, v.y += sy;
            v.z = std::max(0.0f, std::min(1.0f, v.z + sz));
        }
    }
    d.mid_first = m0;
    d.mid_verts = true;
    g_mid_direct++;
    g_mid_sprites += b.count, g_mid_sprites_tracked += tracked, g_mid_sprites_born += born;
    if (g_mid_auditing) mid_audit(g_texture, id, MA_PARTICLES, false, (float)tracked, (float)born);
}

// the list's node of an empty packet of the port's: its sprites, drawn as the game's packet would be (add_direct)
static bool port_sprites_emit(u32 data)
{
    if (g_port_batches.empty()) return false;
    const PortBatch *b = nullptr;
    for (const PortBatch &x : g_port_batches)
        if (x.marker == data) {
            b = &x;
            break;
        }
    if (!b || !data) return false;
    if (b->gpu >= 0) return gpu_sprites_emit(*b);
    float ox, oy;
    const float w = (float)LW(g_target + 0x1c), h = (float)LW(g_target + 0x20);
    target_offset(g_target, g_mode, ox, oy);
    const float clip[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, depth_scale(g_target), 0, -1 - 2 * ox / w, -1 - 2 * oy / h, 0, 1};
    NativeDraw d;
    memcpy(d.clip, clip, sizeof clip);
    d.first = (u32)g_frame.vertices.size();
    d.count = b->count * 6;
    g_strip_flags = 0;
    set_draw_state(d);
    d.light = -1;
    if (b->light >= 0 && (size_t)b->light < g_frame.lights.size()) d.light = b->light, d.flags = kSoft2dFlag;
    d.blend = NB_OPAQUE;
    d.blend_fix = -1;
    set_blend(d, g_texture ? LB(g_texture + 118) : 0, g_texture ? LB(g_texture + 119) : 0x80);
    const s8 k = g_texture ? lod_k(LH(g_texture + 0x72)) : 0;
    // on the wide picture a sprite is narrowed where it stands, as the game's are (unstretch_2d); not the mirror's nor the reflections'
    const u32 id = LW(g_target_ptr);
    const float f = g_frame.wide && id != 7 && id < 8 ? native_narrow() : 1.0f;
    g_frame.vertices.insert(g_frame.vertices.end(), g_port_verts.begin() + (size_t)b->first * 6, g_port_verts.begin() + ((size_t)b->first + b->count) * 6);
    NativeVertex *out = &g_frame.vertices[d.first];
    for (u32 i = 0; i < d.count; i += 6) {
        const float mid = (out[i].x + out[i + 5].x) * 0.5f;
        for (int j = 0; j < 6; j++) {
            out[i + j].k = k;
            if (f != 1.0f) out[i + j].x = mid + (out[i + j].x - mid) * f;
        }
    }
    g_frame.draws.push_back(d);
    g_by_target[g_draw_target & 15].direct++;
    g_by_target[g_draw_target & 15].target = g_target;
    if (g_double_now) port_sprites_mid(*b, g_frame.draws.back());
    g_clip_new = true;
    g_port_sprites_n += b->count, g_port_groups_n++;
    return true;
}

// TEMP HP2_PARTCHECK=1 (with Particles at Advanced or High): the port's sprites are made and not drawn, and held against the game's
// packets of the same call, sprite by sprite: the middle (the game's is cut down to a whole pixel), the corners about the middle,
// the colour, the depth, the cell of the sheet. The game's are the ones drawn.
static void port_sprites_check(u32 from, u32 to)
{
    if (!g_port_checking) return;
    static u64 packets, sprites, bad_count, bad_colour, bad_depth, bad_cell, bad_middle, bad_shape, told;
    static double worst_middle, worst_shape;
    size_t batch = g_check_batches;
    for (u32 a = from; from && a + 16 <= to; batch++) {
        const u32 n = LW(a) & 0x7fff;
        if (!(LW(a) & 0x8000) || LW(a + 8) != 0x42424210 || LW(a + 12) != 0x42 || !n) {
            // not a packet: what eAttachDirectData took from the frame's memory for its own nodes lies between two of them
            a += 16;
            batch--;
            continue;
        }
        packets++;
        const PortBatch *b = batch < g_port_batches.size() ? &g_port_batches[batch] : nullptr;
        if (!b || b->count != n) {
            bad_count++;
            if (told++ < 40) fprintf(g_hle_log, "partcheck: frame %u view %u: the game's packet has %u sprites, the port's batch %d%c", g_frame_no, LW(g_fx_view), n, b ? (int)b->count : -1, 10);
        } else {
            for (u32 i = 0; i < n; i++) {
                const u32 at = a + 16 + i * 80;
                const NativeVertex *mine = &g_port_verts[((size_t)b->first + i) * 6];
                static const int kCorner[4] = {0, 1, 2, 5};
                sprites++;
                float gx[4], gy[4], gs[4], gt[4], gcx = 0, gcy = 0, mcx = 0, mcy = 0;
                for (int k = 0; k < 4; k++) {
                    memcpy(&gs[k], host(at + 16 + k * 16), 4);
                    memcpy(&gt[k], host(at + 20 + k * 16), 4);
                    const u32 xy = LW(at + 24 + k * 16);
                    gx[k] = (float)(xy & 0xffff) / 16.0f, gy[k] = (float)(xy >> 16) / 16.0f;
                    gcx += gx[k] * 0.25f, gcy += gy[k] * 0.25f;
                    mcx += mine[kCorner[k]].x * 0.25f, mcy += mine[kCorner[k]].y * 0.25f;
                }
                const u32 rgba = LW(at + 8), zw = LW(at + 28) & 0xffffff;
                const NativeVertex &m0 = mine[0];
                bool fail = false;
                const int dc = std::max(std::max(abs((int)m0.r - (int)(rgba & 255)), abs((int)m0.g - (int)(rgba >> 8 & 255))),
                                        std::max(abs((int)m0.b - (int)(rgba >> 16 & 255)), abs((int)m0.a - (int)(rgba >> 24))));
                if (dc > 1) bad_colour++, fail = true;
                if (fabsf(m0.z - (float)zw / 16777216.0f) > 1.0f / 16777216.0f) {
                    static int told_depth;
                    bad_depth++;
                    if (told_depth++ < 12) fprintf(g_hle_log, "partcheck: depth: the game's %06x, the port's %06x (shift %u)%c", zw, (u32)(m0.z * 16777216.0f), LW(g_pc_ctx->r[28].w[0] - 23440), 10);
                }
                // the port's middle is the place itself; the game's is that cut down to a whole pixel
                const double dx = mcx - gcx, dy = mcy - gcy, off = std::max(std::max(-dx, dx - 1.0), std::max(-dy, dy - 1.0));
                worst_middle = std::max(worst_middle, off);
                if (off > 0.15) bad_middle++, fail = true;
                double shape = 0;
                bool cells = true;
                for (int k = 0; k < 4; k++) {
                    int found = -1;
                    for (int j = 0; j < 4; j++)
                        if (fabsf(mine[kCorner[j]].s - gs[k]) < 1e-4f && fabsf(mine[kCorner[j]].t - gt[k]) < 1e-4f) found = j;
                    if (found < 0) {
                        cells = false;
                        continue;
                    }
                    shape = std::max(shape, (double)std::max(fabsf((mine[kCorner[found]].x - mcx) - (gx[k] - gcx)), fabsf((mine[kCorner[found]].y - mcy) - (gy[k] - gcy))));
                }
                if (!cells) bad_cell++, fail = true;
                worst_shape = std::max(worst_shape, shape);
                if (shape > 0.2) bad_shape++, fail = true;
                if (fail && told++ < 40) {
                    fprintf(g_hle_log, "partcheck: frame %u view %u sprite %u of %u: the game's %08x z %06x", g_frame_no, LW(g_fx_view), i, n, rgba, zw);
                    for (int k = 0; k < 4; k++) fprintf(g_hle_log, " (%.2f %.2f / %.3f %.3f)", gx[k], gy[k], gs[k], gt[k]);
                    fprintf(g_hle_log, "; the port's %02x%02x%02x%02x z %06x", m0.a, m0.b, m0.g, m0.r, (u32)(m0.z * 16777216.0f));
                    for (int k = 0; k < 4; k++) fprintf(g_hle_log, " (%.2f %.2f / %.3f %.3f)", mine[kCorner[k]].x, mine[kCorner[k]].y, mine[kCorner[k]].s, mine[kCorner[k]].t);
                    fputc(10, g_hle_log);
                }
            }
        }
        a += 16 + n * 80;
    }
    if (g_port_batches.size() > g_check_batches) {
        if (batch < g_port_batches.size()) {
            bad_count += g_port_batches.size() - batch;
            if (told++ < 40) fprintf(g_hle_log, "partcheck: frame %u view %u: %zu batches of the port's with no packet of the game's%c", g_frame_no, LW(g_fx_view), g_port_batches.size() - batch, 10);
        }
        g_port_batches.resize(g_check_batches);
        g_port_verts.resize(g_check_verts);
        g_port_parts.resize(g_check_parts);
    }
    static u32 last;
    if (g_frame_no >= last + 600 && packets) {
        last = g_frame_no;
        fprintf(g_hle_log, "partcheck: %llu packets, %llu sprites; unlike: %llu packets by their count, sprites: %llu colour, %llu depth, %llu cell, %llu middle (worst %.3f), %llu shape (worst %.3f)%c",
                (unsigned long long)packets, (unsigned long long)sprites, (unsigned long long)bad_count, (unsigned long long)bad_colour, (unsigned long long)bad_depth,
                (unsigned long long)bad_cell, (unsigned long long)bad_middle, worst_middle, (unsigned long long)bad_shape, worst_shape, 10);
    }
}

u32 native_particles_begin(Ctx &c);
bool native_particles_draw(Ctx &c)
{
    static const bool check = getenv("HP2_PARTCHECK") && atoi(getenv("HP2_PARTCHECK")) == 1;
    g_port_checking = false;
    if (particles_level() < 1) return false;
    g_pc_ctx = &c;
    const u32 system = c.r[4].w[0], view = c.r[5].w[0];
    if (!system || !view) return false;
    if (check) {
        g_port_checking = true;
        g_check_batches = (u32)g_port_batches.size(), g_check_verts = (u32)g_port_verts.size(), g_check_parts = (u32)g_port_parts.size();
        port_sprites_build(c, system, view, false);
        return false;
    }
    if (particles_level() >= 2) {
        gpu_build(c, system, view);         // High: the port's own particles, the shader's work
        return true;
    }
    native_particles_begin(c);              // the particles' places this frame, for the picture between the next two
    port_sprites_build(c, system, view, true);
    return true;
}

// ---- Particles: High: the port's own particles, the GPU's work (the user, 2026-11-14) ---------------------------------
// The game's particles stay the game's at every level, pools and all; at High they are simply not drawn. The port keeps particles
// of its own beside them:
//  - born by the game's own code. After an emitter has given the game its particles (TriggerEmitter, 0x127700) the same function
//    runs once more for the port (gpu_emit): with the port's own seed and countdown for that emitter in place of the game's, twice
//    the time to cover (twice the particles) and twice the far limit, into a system and two pools that are the port's, in the PC's
//    memory past the game's 32 MB. What it made there is taken as the port's new particles, and every word of the game's that
//    call touched (the emitter's seed, countdown and group, the far limit, the two pools' addresses, the registers) is put back;
//  - kept as it was born and nothing more: place, direction and speed, life, angle, the frame of its sheet, and the system's
//    clock then (the game's 1/1024 s, counted as DoTimeStep counts them). Six vertices carry that (gpu_add: NativeVertex's
//    fields under other names) and the vertex shader does the rest for each picture (native.vert, particle()): the game's steps
//    from its birth to the picture's time, its age, size, spin, colour and frame, where the view has it, the sprite's corner.
//    The picture between two steps is the same particles at another time: nothing to pair, nothing kept from the frame before;
//  - the game's step rounds: after each one the speed is cut to a word and the direction to three bytes (0x128c4c on), and that
//    is most of why its smoke slows down as it does. The shader takes the very steps the game took since the particle's birth,
//    each of the length DoTimeStep was given, with the same cuts: the clock keeps the lengths of the last 2048 (GpuClock::steps)
//    and the frame carries them among its lights (gpu_table);
//  - what the shader needs of the view and of the emitter is a block among the frame's lights (gpu_block: two of them), one for
//    each group a view shows: the view's matrices as RenderEffects uses them, now and a step before, the emitter's two curves
//    and constants, the clock now and a step before, the car's way for the particles that go with their car (MotionLive);
//  - the game still sorts: an empty packet is attached for each group a view shows, with the group's depth, as at Advanced.
// Leaving High drops the port's particles. Nothing of the game's was changed for them, so there is nothing to put back.
static const u32 kGpuSteps = 2048;                  // the steps a clock remembers: 34 s of sixtieths, more than a particle lives
static const u32 kGpuMost = 24000;                  // the port's particles at once
static const u32 kPartFlag = 0x10000;               // NativeDraw::flags: native.vert's PARTICLE. Above the strips' own 16 bits (vi07): a car's
                                                    // glass has 0xc180 there, and 0x8000 for this made the shader take glass for particles
static const u32 kGpuArenaOff = 0x0C800000, kGpuArenaBytes = 0x40000;       // past the draw table's (kDrawTableOff)
static const u32 kGpuArena = EXT_BASE + kGpuArenaOff;
static const u32 kGpuPoolP = kGpuArena, kGpuPoolG = kGpuArena + 0x40, kGpuSystem = kGpuArena + 0x80;
static const u32 kGpuGroups = kGpuArena + 0x200, kGpuParts = kGpuArena + 0x400;
static const u32 kGpuArenaParts = 4096, kGpuArenaGroups = 8;

struct GpuPart {
    u32 birth, life;                        // the clock then; its ticks to live
};
struct GpuGroup {
    u32 id;                                 // its own, never 0: an emitter knows its last group by it (GpuSeed::last)
    u32 system, emitter, car, ed;
    bool follows;                           // MotionLive on a car: its particles go with the car, each by its share
    double way[3], way_shown[3], way_prev[3];       // the car's way since the group began: now, as this frame shows it, the frame before
    std::vector<GpuPart> parts;
    std::vector<NativeVertex> verts;        // six a particle
    double sum[3], sum_share;               // of its particles: their places at birth (less their share of the car's way till then), and
                                            // their shares: the middle of them all, for the game's sort (gpu_build)
    u32 frame, frame_first;                 // its vertices are in the frame g_frame_no + 1 == frame, from there
};
struct GpuClock {
    u32 system, ticks, shown, prev;         // now; as this frame shows it; as the frame before did
    u32 count, fresh, since;                // steps in all; since a frame was last shown; of those the frame now shown has
    u32 frame;                              // the table is among the lights of the frame g_frame_no + 1 == frame, at `light`
    int light;
    float steps[kGpuSteps];                 // the lengths DoTimeStep was given, in seconds: steps[(count - 1 - n) % kGpuSteps] is n steps back
};
struct GpuSeed {
    u32 seed, countdown;                    // AcidEmitter's RandomSeed and EmissionCountdown, the port's
    u32 last;                               // and its LastActiveGroup: a GpuGroup's id, 0 none
};
struct GpuView {
    u32 frame;
    bool has;
    float clip[16], screen[16], zrow[4], focal, p_clip[16], p_screen[16], p_zrow[4], p_focal;
};
static std::vector<GpuGroup> g_gpu_groups;
static GpuClock g_gpu_clocks[4];
static u32 g_gpu_clocks_n;
static std::unordered_map<u32, GpuSeed> g_gpu_emitters;
static GpuView g_gpu_views[16];
static u32 g_gpu_total;
static u64 g_gpu_born, g_gpu_drawn, g_gpu_draws, g_gpu_full;

static GpuClock &gpu_clock(u32 system)
{
    for (u32 i = 0; i < g_gpu_clocks_n; i++)
        if (g_gpu_clocks[i].system == system) return g_gpu_clocks[i];
    GpuClock &k = g_gpu_clocks[g_gpu_clocks_n < 4 ? g_gpu_clocks_n++ : 3];
    memset(&k, 0, sizeof k);
    k.system = system;
    return k;
}

static void gpu_forget(u32 system = 0)      // the port's particles, all of them or one system's
{
    if (!system) {
        g_gpu_groups.clear();
        g_gpu_emitters.clear();
        g_gpu_total = 0;
        return;
    }
    size_t w = 0;
    for (size_t i = 0; i < g_gpu_groups.size(); i++) {
        if (g_gpu_groups[i].system == system) {
            g_gpu_total -= (u32)g_gpu_groups[i].parts.size();
            continue;
        }
        if (w != i) g_gpu_groups[w] = std::move(g_gpu_groups[i]);
        w++;
    }
    g_gpu_groups.resize(w);
}

// a pool of the port's as SlotPool::Malloc and Free know one (0x258200, 0x258238): the free slots chained from +16, the counts at
// +24 and +28; flags 0, so it neither grows nor clears. The first `used` slots get their links again.
static void gpu_pool_reset(u32 pool, u32 slots, u32 size, u32 n, u32 used)
{
    for (u32 i = 0; i < used && i < n; i++) SW(slots + i * size, i + 1 < n ? slots + (i + 1) * size : 0);
    SW(pool + 16, slots), SW(pool + 20, 0), SW(pool + 24, 0), SW(pool + 28, 0), SW(pool + 36, n), SW(pool + 40, size), SW(pool + 44, n);
}

static bool gpu_arena()
{
    static const bool ok = ext_commit(kGpuArenaOff, kGpuArenaBytes);
    static bool made;
    if (ok && !made) {
        made = true;
        memset(host(kGpuArena), 0, kGpuArenaBytes);
        gpu_pool_reset(kGpuPoolP, kGpuParts, 32, kGpuArenaParts, kGpuArenaParts);
        gpu_pool_reset(kGpuPoolG, kGpuGroups, 32, kGpuArenaGroups, kGpuArenaGroups);
    }
    return ok;
}

// The group an emitter's new particles go into, by the game's own rule (TriggerEmitter, 0x1278f0 on): the emitter's last group
// while that is this car's, else a new one, which is then its last. An emitter is one for all the cars (the dust is one, the
// tyres' smoke another), so with several cars raising dust each call makes a new group: many small ones, a car's dust of a moment
// each, and the game sorts every one by its own depth among the rest that is transparent. Till 2026-11-17 the port had one group
// for all an emitter ever gave a car: the car's whole trail of dust and its whole trail of smoke, two groups with nearly one depth
// that changed places as wholes from frame to frame (the user: "the smoke of the dust and of the wheels race each other").
static const u32 kGpuGroupsMost = 400;      // the game's pool of groups (afxInit): none past it
static u32 g_gpu_group_ids;
static GpuGroup *gpu_group(GpuSeed &mine, u32 system, u32 emitter, u32 car, u32 ed)
{
    if (mine.last)
        for (GpuGroup &g : g_gpu_groups)
            if (g.id == mine.last) {
                if (g.car == car && g.ed == ed && g.system == system && g.emitter == emitter) return &g;
                break;
            }
    if (g_gpu_groups.size() >= kGpuGroupsMost) return nullptr;
    g_gpu_groups.emplace_back();
    GpuGroup &g = g_gpu_groups.back();
    if (!++g_gpu_group_ids) ++g_gpu_group_ids;
    g.id = mine.last = g_gpu_group_ids;
    g.system = system, g.emitter = emitter, g.car = car, g.ed = ed;
    float inherit;
    memcpy(&inherit, host(ed + 80), 4);
    g.follows = (s8)LB(ed + 30) && car && inherit > 0;      // DoTimeStep, 0x128a44 on
    for (int i = 0; i < 3; i++) g.way[i] = g.way_shown[i] = g.way_prev[i] = 0;
    g.sum[0] = g.sum[1] = g.sum[2] = g.sum_share = 0;
    g.frame = g.frame_first = 0;
    return &g;
}

// an AcidParticle the game's code has just made, as one of the port's
static void gpu_add(GpuGroup &g, u32 p, u32 birth, u32 gp)
{
    const s32 life = (s16)LH(p + 8);
    if (life <= 0) return;
    float pos[3];
    memcpy(pos, host(p + 16), 12);
    const u32 inherit = LB(p + 14), angle = LH(p + 10);
    if (g.follows) {
        float share;
        memcpy(&share, host(gp - 32084), 4);                // 1 / 255
        for (int i = 0; i < 3; i++) pos[i] -= (float)(g.way[i] * (double)((float)inherit * share));
        g.sum_share += (double)inherit * (1.0 / 255.0);
    }
    for (int i = 0; i < 3; i++) g.sum[i] += (double)pos[i];
    NativeVertex v = NativeVertex{};
    v.x = pos[0], v.y = pos[1], v.z = pos[2];
    v.s = (float)(birth & 0xffff), v.t = (float)life, v.w = (float)LH(p + 12);
    v.r = (u8)angle, v.g = (u8)(angle >> 8), v.b = (u8)inherit, v.a = LB(p + 31);
    v.nx = (s8)LB(p + 28), v.ny = (s8)LB(p + 29), v.nz = (s8)LB(p + 30);
    static const s8 kOrder[6] = {0, 1, 2, 1, 2, 3};
    for (s8 corner : kOrder) {
        v.k = corner;
        g.verts.push_back(v);
    }
    g.parts.push_back(GpuPart{birth, (u32)life});
    g_gpu_total++, g_gpu_born++;
}

// TriggerEmitter once more, for the port. `in` is the call as the game made it; c is as the game's own call left it.
static void gpu_emit(Ctx &c, const Ctx &in, void (*orig)(Ctx &))
{
    const u32 system = in.r[4].w[0], emitter = in.r[5].w[0], gp = in.r[28].w[0];
    const u32 ed = emitter ? LW(emitter + 28) : 0;
    if (!system || !ed || !gpu_arena()) return;
    if (g_gpu_total >= kGpuMost) {
        g_gpu_full++;
        return;
    }
    GpuSeed &mine = g_gpu_emitters[emitter];
    if (!mine.seed) mine.seed = (LW(emitter + 36) * 0x9e3779b1u) | 1, mine.countdown = 0, mine.last = 0;     // never 0: that one is drawn from the game's
    const Ctx out = c;
    const u32 was_seed = LW(emitter + 36), was_count = LW(emitter + 24), was_group = LW(emitter + 32), was_far = LW(ed + 128);
    const u32 was_parts = LW(gp - 23528), was_groups = LW(gp - 23524);
    // the port's system: the game's interest points (+16: where the cameras are), and no groups
    memcpy(host(kGpuSystem), host(system), 96);
    SW(kGpuSystem, kGpuSystem), SW(kGpuSystem + 4, kGpuSystem), SW(kGpuSystem + 8, kGpuSystem + 8), SW(kGpuSystem + 12, kGpuSystem + 8);
    SW(emitter + 36, mine.seed), SW(emitter + 24, mine.countdown), SW(emitter + 32, 0);
    float far_limit;
    memcpy(&far_limit, &was_far, 4);
    far_limit *= kPartFar;
    u32 far_bits;
    memcpy(&far_bits, &far_limit, 4);
    SW(ed + 128, far_bits);
    SW(gp - 23528, kGpuPoolP), SW(gp - 23524, kGpuPoolG);
    c = in;
    c.r[4].d[0] = (u64)(s64)(s32)kGpuSystem;
    c.f[12].f = in.f[12].f * kPartDensity;
    orig(c);
    mine.seed = LW(emitter + 36), mine.countdown = LW(emitter + 24);
    const u32 birth = gpu_clock(system).ticks;
    for (u32 group = LW(kGpuSystem), gn = 0; group && group != kGpuSystem && gn < kGpuArenaGroups; group = LW(group), gn++) {
        const u32 list = group + 20;
        GpuGroup *mine_group = nullptr;
        for (u32 p = LW(list), n = 0; p && p != list && n < kGpuArenaParts && g_gpu_total < kGpuMost; p = LW(p), n++) {
            if (!mine_group) mine_group = gpu_group(mine, system, emitter, LW(group + 16), LW(group + 8));
            if (!mine_group) break;         // no group left, as the game's pool of them runs out
            gpu_add(*mine_group, p, birth, gp);
        }
    }
    SW(emitter + 36, was_seed), SW(emitter + 24, was_count), SW(emitter + 32, was_group), SW(ed + 128, was_far);
    SW(gp - 23528, was_parts), SW(gp - 23524, was_groups);
    gpu_pool_reset(kGpuPoolP, kGpuParts, 32, kGpuArenaParts, LW(kGpuPoolP + 28));
    gpu_pool_reset(kGpuPoolG, kGpuGroups, 32, kGpuArenaGroups, LW(kGpuPoolG + 28));
    c = out;
}

void HOOK_TriggerEmitter__16AcidEffectSystemP11AcidEmitterfP3Car(Ctx &c, void (*orig)(Ctx &))
{
    if (particles_level() < 2) {
        orig(c);
        return;
    }
    const Ctx in = c;
    orig(c);                                // the game's own, as at every level
    gpu_emit(c, in, orig);
}

// the game's step for one particle as the shader takes it (native.vert, particle()): for HP2_PARTCHECK=2
static void gpu_step(float pos[3], int d[3], int &sp, float gravity, float drag, float c127, float dt)
{
    float v[3], l = 0;
    const float mag = (float)sp * (1.0f / 128.0f) * c127;
    for (int i = 0; i < 3; i++) {
        v[i] = (float)d[i] * mag;
        float acc = (v[i] < 0 ? 1.0f : v[i] > 0 ? -1.0f : 0.0f) * drag * (v[i] * v[i]);
        if (i == 2) acc -= gravity;
        v[i] += acc * dt;
        pos[i] += v[i] * dt;
        l += v[i] * v[i];
    }
    l = sqrtf(l);
    sp = (int)(l * 128.0f) & 0xffff;
    for (int i = 0; i < 3; i++) d[i] = l > 0 ? (int)(v[i] * (1.0f / l) * 127.0f) : 0;
}

// TEMP HP2_PARTCHECK=2: the shader's step held against the game's own particles, each from where it was first seen: after every
// step of the game's, where the port's steps have it and where the game does, and whether its speed and direction are the game's
static void gpu_check_motion(u32 system, u32 gp, float elapsed)
{
    struct Track {
        float pos[3], way[3];
        int d[3], sp;
        s32 life;
        u32 stamp, system;
    };
    static std::unordered_map<u32, Track> tracks;
    static std::unordered_map<u32, u32> stamps;         // each system's own count of steps
    u32 &stamp = stamps[system];
    static u64 steps, off_state, started;
    static double worst, sum;
    static float worst_at[8], least = 1e9f, most;
    stamp++;
    if (elapsed < least) least = elapsed;
    if (elapsed > most) most = elapsed;
    float c127, share;
    memcpy(&c127, host(gp - 32088), 4);
    memcpy(&share, host(gp - 32084), 4);
    u32 groups = 0;
    for (u32 group = LW(system); group && group != system && groups < 8192; group = LW(group), groups++) {
        const u32 ed = LW(group + 8), car = LW(group + 16), list = group + 20;
        if (!ed) continue;
        float gravity, drag, inherit, vel[3] = {0, 0, 0};
        memcpy(&gravity, host(ed + 132), 4);
        memcpy(&drag, host(ed + 136), 4);
        memcpy(&inherit, host(ed + 80), 4);
        const bool follows = (s8)LB(ed + 30) && car && inherit > 0;
        if (follows) memcpy(vel, host(car + 112), 12);
        u32 n = 0;
        for (u32 p = LW(list); p && p != list && n < 16384; p = LW(p), n++) {
            const s32 life = (s16)LH(p + 8);
            float now[3];
            memcpy(now, host(p + 16), 12);
            auto it = tracks.find(p);
            if (it != tracks.end() && it->second.stamp + 1 == stamp && life < it->second.life) {
                Track &t = it->second;
                gpu_step(t.pos, t.d, t.sp, gravity, drag, c127, elapsed);
                if (follows)
                    for (int i = 0; i < 3; i++) t.way[i] += elapsed * vel[i] * ((float)LB(p + 14) * share);
                double e = 0;
                for (int i = 0; i < 3; i++) e += ((double)t.pos[i] + t.way[i] - now[i]) * ((double)t.pos[i] + t.way[i] - now[i]);
                e = sqrt(e);
                steps++, sum += e;
                if (e > worst) {
                    worst = e;
                    worst_at[0] = gravity, worst_at[1] = drag, worst_at[2] = (float)t.sp, worst_at[3] = (float)LH(p + 12), worst_at[4] = (float)follows;
                    worst_at[5] = elapsed;
                }
                if (t.sp != (int)LH(p + 12) || t.d[0] != (s8)LB(p + 28) || t.d[1] != (s8)LB(p + 29) || t.d[2] != (s8)LB(p + 30)) off_state++;
                t.life = life, t.stamp = stamp;
                continue;
            }
            Track t;
            memcpy(t.pos, now, 12);
            t.way[0] = t.way[1] = t.way[2] = 0;
            t.d[0] = (s8)LB(p + 28), t.d[1] = (s8)LB(p + 29), t.d[2] = (s8)LB(p + 30);
            t.sp = LH(p + 12);
            t.life = life, t.stamp = stamp, t.system = system;
            tracks[p] = t;
            started++;
        }
    }
    if (stamp % 600 == 0) {
        for (auto it = tracks.begin(); it != tracks.end();) it = it->second.system == system && it->second.stamp + 2 < stamp ? tracks.erase(it) : std::next(it);
        fprintf(g_hle_log, "partcheck: motion: %llu steps of %llu particles held against the game's: %.6f m apart on average, %.6f at worst "
                           "(gravity %.3f drag %.3f, speed %.0f against the game's %.0f, with its car %.0f, the step %.6f s); %llu steps with the speed or "
                           "the direction not the game's; the game's steps were %.6f to %.6f s%c",
                (unsigned long long)steps, (unsigned long long)started, steps ? sum / (double)steps : 0.0, worst, worst_at[0], worst_at[1], worst_at[2],
                worst_at[3], worst_at[4], worst_at[5], (unsigned long long)off_state, least, most, 10);
    }
}

// AcidEffectSystem::DoTimeStep(float) (phase.cpp): the system's clock and its steps, the cars' ways, and the port's particles whose
// time was up a step ago (one that ends in this step is still on its way there in the picture between)
void native_particles_step(Ctx &c, void (*orig)(Ctx &))
{
    static const bool check = getenv("HP2_PARTCHECK") && atoi(getenv("HP2_PARTCHECK")) == 2;
    const u32 system = c.r[4].w[0], gp = c.r[28].w[0];
    const float elapsed = c.f[12].f;
    orig(c);                                // its automatic emitters give in there, at the clock before this step
    if (!system) return;
    const s32 ticks = part_int(elapsed * 1024.0f);      // as the game counts a particle's life down (0x128750)
    if (ticks <= 0) return;
    if (check) gpu_check_motion(system, gp, elapsed);
    if (particles_level() < 2) return;
    GpuClock &k = gpu_clock(system);
    const u32 before = k.ticks;
    k.steps[k.count++ % kGpuSteps] = elapsed;
    k.fresh++;
    k.ticks += (u32)ticks;
    size_t kept = 0;
    for (size_t gi = 0; gi < g_gpu_groups.size(); gi++) {
        GpuGroup &g = g_gpu_groups[gi];
        if (g.system == system) {
            if (g.follows) {
                float vel[3];
                memcpy(vel, host(g.car + 112), 12);
                for (int i = 0; i < 3; i++) g.way[i] += (double)(elapsed * vel[i]);
            }
            size_t w = 0;
            for (size_t i = 0; i < g.parts.size(); i++) {
                if (before - g.parts[i].birth >= g.parts[i].life) {
                    const NativeVertex &gone = g.verts[i * 6];
                    g.sum[0] -= (double)gone.x, g.sum[1] -= (double)gone.y, g.sum[2] -= (double)gone.z;
                    if (g.follows) g.sum_share -= (double)gone.b * (1.0 / 255.0);
                    continue;
                }
                if (w != i) {
                    g.parts[w] = g.parts[i];
                    memcpy(&g.verts[w * 6], &g.verts[i * 6], 6 * sizeof(NativeVertex));
                }
                w++;
            }
            g_gpu_total -= (u32)(g.parts.size() - w);
            g.parts.resize(w);
            g.verts.resize(w * 6);
            if (!w) continue;
        }
        if (kept != gi) g_gpu_groups[kept] = std::move(g);
        kept++;
    }
    g_gpu_groups.resize(kept);
}

// AcidEffectSystem::KillAllActiveEffects (0x1276a8): the port's go with the game's
void HOOK_KillAllActiveEffects__16AcidEffectSystem(Ctx &c, void (*orig)(Ctx &))
{
    const u32 system = c.r[4].w[0];
    orig(c);
    gpu_forget(system);
}

// a frame is about to be shown: what it shows of the clocks and the ways, and what the frame before did
static void gpu_frame()
{
    if (particles_level() < 2) {
        if (!g_gpu_groups.empty() || !g_gpu_emitters.empty()) gpu_forget();
        return;
    }
    for (u32 i = 0; i < g_gpu_clocks_n; i++) {
        GpuClock &k = g_gpu_clocks[i];
        k.prev = k.shown, k.shown = k.ticks;
        k.since = k.fresh, k.fresh = 0;
    }
    for (GpuGroup &g : g_gpu_groups)
        for (int i = 0; i < 3; i++) g.way_prev[i] = g.way_shown[i], g.way_shown[i] = g.way[i];
    if (g_frame_no % 600 == 0 && getenv("HP2_NATIVELOG"))
        fprintf(g_hle_log, "native: particles on the GPU: %u alive in %zu groups; %llu born, %llu drawn in %llu draws so far; %llu times an emitter was "
                           "refused for want of room%c",
                g_gpu_total, g_gpu_groups.size(), (unsigned long long)g_gpu_born, (unsigned long long)g_gpu_drawn, (unsigned long long)g_gpu_draws,
                (unsigned long long)g_gpu_full, 10);
}

// the clock's steps among the frame's lights, the latest first: once a frame. The light they start at, -1: no room.
static int gpu_table(GpuClock &k)
{
    if (k.frame == g_frame_no + 1) return k.light;
    static const u32 kLights = (kGpuSteps / 4 + 21) / 22;
    const int at = (int)g_frame.lights.size();
    k.frame = g_frame_no + 1;
    k.light = -1;
    if (at + kLights > kNativeLightsMost) return -1;    // the most a frame may have (native.h)
    g_frame.lights.resize((size_t)at + kLights);
    float *q = &g_frame.lights[at].q[0][0];
    memset(q, 0, kLights * sizeof(NativeLight));
    const u32 have = k.count < kGpuSteps ? k.count : kGpuSteps;
    for (u32 n = 0; n < have; n++) q[n] = k.steps[(k.count - 1 - n) % kGpuSteps];
    return k.light = at;
}

// the block the shader reads for a group in a view: two of the frame's lights, 44 quads (native.vert, particle(): L[b + ...])
static int gpu_block(const GpuView &v, GpuClock &k, const GpuGroup &g, u32 gp)
{
    const int table = gpu_table(k);
    if (table < 0) return -1;
    const int at = (int)g_frame.lights.size();
    if (at + 2 > kNativeLightsMost) return -1;          // the most a frame may have (native.h)
    g_frame.lights.resize((size_t)at + 2);
    float *q = &g_frame.lights[at].q[0][0];
    memset(q, 0, 2 * sizeof(NativeLight));
    const u32 ed = g.ed;
    memcpy(q, v.p_clip, 64), memcpy(q + 16, v.p_screen, 64), memcpy(q + 32, v.p_zrow, 16);
    memcpy(q + 36, v.clip, 64), memcpy(q + 52, v.screen, 64), memcpy(q + 68, v.zrow, 16);
    memcpy(q + 72, host(ed + 256), 64);     // ColourBasis
    memcpy(q + 88, host(ed + 320), 64);     // ExtraBasis: the size, the spin
    float max_px, far_limit;
    memcpy(&max_px, host(ed + 140), 4);
    memcpy(&far_limit, host(ed + 128), 4);
    q[104] = v.p_focal, q[105] = v.focal, q[106] = max_px == 0 ? 512.0f : max_px, q[107] = far_limit * kPartFar;
    memcpy(q + 108, host(ed + 56), 4);      // Life
    memcpy(q + 109, host(ed + 132), 8);     // Gravity, Drag
    q[111] = 1;                             // how much narrower on the wide picture: gpu_sprites_emit
    const s32 fps = (s8)LB(ed + 27);
    const u32 pow_w = LB(ed + 25) & 31, pow_h = LB(ed + 26) & 31;
    q[112] = (float)(k.prev & 0xffff), q[113] = (float)(s32)(k.shown - k.prev);
    q[114] = fps ? (float)part_int(1.0f / (float)fps * 1024.0f) : 0.0f;         // DoTimeStep, 0x1289e0 on: the sheet's ticks a frame
    q[115] = (float)((1u << pow_w << pow_h) - 1);
    q[116] = (float)pow_w, q[117] = (float)pow_h, q[118] = (float)(LW(gp - 23440) & 31), q[119] = 0;   // TEX1's K: gpu_sprites_emit
    if (g.follows)
        for (int i = 0; i < 3; i++) q[120 + i] = (float)g.way_prev[i], q[124 + i] = (float)g.way_shown[i];
    memcpy(q + 123, host(gp - 32080), 4);   // an angle's unit in radians
    memcpy(q + 127, host(gp - 32076), 4);   // a quarter turn
    q[128] = (float)(table * 22), q[129] = (float)(k.count < kGpuSteps ? k.count : kGpuSteps), q[130] = (float)k.since;
    memcpy(q + 131, host(gp - 32088), 4);   // 1 / 127
    memcpy(q + 132, host(gp - 32084), 4);   // 1 / 255
    return at;
}

// One view's groups of the port's particles: a block and an empty packet for each it shows, by RenderEffects' rules for a group
// (part_group_shown). The particles themselves are not looked at here: the shader finds which of them the view has.
static void gpu_build(Ctx &c, u32 system, u32 view)
{
    const u32 gp = c.r[28].w[0];
    const u32 view_id = LW(view), camera = LW(view + 56);
    const u32 a_view = LW(view + 68), a_clip = LW(view + 72), a_screen = LW(view + 76);
    if (!camera || !a_view || !a_clip || !a_screen) return;
    GpuView &v = g_gpu_views[view_id & 15];
    if (v.frame != g_frame_no + 1) {
        const bool follows = v.has && v.frame == g_frame_no;        // it was drawn the frame before
        if (follows) memcpy(v.p_clip, v.clip, 64), memcpy(v.p_screen, v.screen, 64), memcpy(v.p_zrow, v.zrow, 16), v.p_focal = v.focal;
        float m_view[16];
        memcpy(m_view, host(a_view), 64);
        memcpy(v.clip, host(a_clip), 64);
        memcpy(v.screen, host(a_screen), 64);
        for (int i = 0; i < 4; i++) v.zrow[i] = m_view[i * 4 + 2];
        const float tangent = tanf((float)(LH(camera + 132) >> 1) * (6.28318530718f / 65536.0f));
        v.focal = tangent == 0 ? 10.0f : (float)((s32)LW(gp - 25108) >> 1) / tangent;       // CalculateH, 0x104448
        bool cut = !follows;
        if (follows) {                      // another camera: no way from the one to the other (as pack_seen has it)
            float differ = 0, size = 0;
            for (int i = 0; i < 16; i++) differ += fabsf(v.screen[i] - v.p_screen[i]), size += fabsf(v.screen[i]);
            cut = differ > size * 0.25f;
        }
        if (cut) memcpy(v.p_clip, v.clip, 64), memcpy(v.p_screen, v.screen, 64), memcpy(v.p_zrow, v.zrow, 16), v.p_focal = v.focal;
        v.frame = g_frame_no + 1, v.has = true;
    }
    const u32 hidden = LW(camera + 444);
    const u32 only = (s8)LB(system + 77) ? LW(system + 64) : 0;
    GpuClock &k = gpu_clock(system);
    for (size_t gi = 0; gi < g_gpu_groups.size(); gi++) {
        GpuGroup &g = g_gpu_groups[gi];
        if (g.system != system || g.parts.empty()) continue;
        if (!part_group_shown(g.car, g.ed, view_id, camera, hidden)) continue;
        const u32 tex = LW(g.ed + 20);
        if (!tex) continue;
        if (only && g.emitter != only) continue;
        // The group's depth for the game's sort. It has to be steady: a wide emitter's newest particle is born anywhere in its
        // volume, and by its place the groups changed places with each other and with what else is transparent from one frame to
        // the next (the user, 2026-11-15: smoke that flickers, the lava's glow through it). The game has the mean depth of the
        // particles the view shows, and so it is here (2026-11-17), by where each was born and its share of the car's way: those
        // inside the view; none there, those ahead of it within the emitter's reach. The middle of them all, as it was, had the
        // half of a long trail that is behind the camera or past the reach in it, and stood far from the smoke on the picture.
        // None ahead at all: the middle of them all, and nowhere near the view is not this view's
        float far_limit;
        memcpy(&far_limit, host(g.ed + 128), 4);
        const float reach = far_limit * kPartFar;
        double in_sum = 0, ahead_sum = 0;
        u32 in_n = 0, ahead_n = 0;
        for (size_t i = 0; i < g.verts.size(); i += 6) {
            const NativeVertex &p = g.verts[i];
            float at[3] = {p.x, p.y, p.z};
            if (g.follows) {
                const double share = (double)p.b * (1.0 / 255.0);
                for (int a = 0; a < 3; a++) at[a] += (float)(g.way_shown[a] * share);
            }
            const float d = v.zrow[0] * at[0] + v.zrow[1] * at[1] + v.zrow[2] * at[2] + v.zrow[3];
            if (!(d > 0 && d < reach)) continue;
            ahead_sum += (double)d, ahead_n++;
            const float cx = v.clip[0] * at[0] + v.clip[4] * at[1] + v.clip[8] * at[2] + v.clip[12];
            const float cy = v.clip[1] * at[0] + v.clip[5] * at[1] + v.clip[9] * at[2] + v.clip[13];
            const float cw = v.clip[3] * at[0] + v.clip[7] * at[1] + v.clip[11] * at[2] + v.clip[15];
            if (fabsf(cx) <= fabsf(cw) && fabsf(cy) <= fabsf(cw)) in_sum += (double)d, in_n++;
        }
        float depth;
        if (in_n) {
            depth = (float)(in_sum / (double)in_n);
        } else if (ahead_n) {
            depth = (float)(ahead_sum / (double)ahead_n);
        } else {
            float mid[3];
            for (int i = 0; i < 3; i++) mid[i] = (float)((g.sum[i] + g.way_shown[i] * g.sum_share) / (double)g.parts.size());
            depth = v.zrow[0] * mid[0] + v.zrow[1] * mid[1] + v.zrow[2] * mid[2] + v.zrow[3];
            if (!(depth > -150.0f && depth < reach + 150.0f)) continue;
        }
        const u32 marker = part_marker(gp);
        if (!marker) break;
        const int light = gpu_block(v, k, g, gp);
        if (light < 0) break;
        PortBatch b = {marker, 0, (u32)g.parts.size(), view, 0};
        b.gpu = (int)gi, b.light = light;
        g_port_batches.push_back(b);
        // the key of the game's sort is the game's own: the group's mean depth, as it is (for a day, 2026-11-17, a car's groups
        // went in under one key in one order; it was not what made them flicker, and it was not the game's way)
        float key = depth;
        if (!(key > -1.0e9f && key < 1.0e9f)) key = 0;
        attach_direct(tex, key, marker, 1);
    }
}

// the list's node of a group of the port's own particles: one draw of its vertices, which the shader makes sprites of
static bool gpu_sprites_emit(const PortBatch &b)
{
    if ((size_t)b.gpu >= g_gpu_groups.size()) return true;
    GpuGroup &g = g_gpu_groups[b.gpu];
    if (g.verts.empty()) return true;
    if (g.frame != g_frame_no + 1) {        // its vertices go into the frame once, whatever views show it
        g.frame = g_frame_no + 1;
        g.frame_first = (u32)g_frame.vertices.size();
        g_frame.vertices.insert(g_frame.vertices.end(), g.verts.begin(), g.verts.end());
    }
    float ox, oy;
    const float w = (float)LW(g_target + 0x1c), h = (float)LW(g_target + 0x20);
    target_offset(g_target, g_mode, ox, oy);
    const float clip[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, depth_scale(g_target), 0, -1 - 2 * ox / w, -1 - 2 * oy / h, 0, 1};
    NativeDraw d;
    memcpy(d.clip, clip, sizeof clip);
    d.first = g.frame_first;
    d.count = (u32)g.verts.size();
    g_strip_flags = 0;
    set_draw_state(d);
    d.flags = kPartFlag;
    d.light = b.light;
    d.blend = NB_OPAQUE;
    d.blend_fix = -1;
    set_blend(d, g_texture ? LB(g_texture + 118) : 0, g_texture ? LB(g_texture + 119) : 0x80);
    // on the wide picture a sprite is narrowed where it stands, as the game's are; not the mirror's nor the reflections'
    const u32 id = LW(g_target_ptr);
    float *q = &g_frame.lights[b.light].q[0][0];
    q[111] = g_frame.wide && id != 7 && id < 8 ? native_narrow() : 1.0f;
    q[119] = g_texture ? (float)lod_k(LH(g_texture + 0x72)) : 0.0f;
    g_frame.draws.push_back(d);
    g_by_target[g_draw_target & 15].direct++;
    g_by_target[g_draw_target & 15].target = g_target;
    g_clip_new = true;
    g_gpu_drawn += b.count, g_gpu_draws++;
    return true;
}

// d's clip_mid, for a draw of strips that starts with the strip at `strip`. g_clip and g_now_* are the draw's.
// A car's draw goes with the car. Any other is taken to stand where it stands (the scenery: the camera alone moves), and is noted
// for mid_resolve, which looks at the whole frame once it is all there.
static void mid_matrix(NativeDraw &d, u32 strip)
{
    g_mid_draws++;
    const PackSeen *pack = pack_seen(g_pack);
    const bool view = pack && pack->has_prev;
    const float *before = g_now_l;
    float moved[16];
    if (g_owner) {
        // a car's: as one body with the car
        if (g_owner->moves) mul(moved, g_owner->move, g_now_l), before = moved;
        g_mid_owned++;
        if (g_mid_auditing) mid_audit(g_texture, g_draw_target, !view ? MA_CUT : g_owner->moves ? MA_CAR : MA_CAR_NEW, g_node_poly);
    } else {
        g_mid_now.emplace_back();
        MidKey &key = g_mid_now.back();
        key.strip = strip;
        key.target = (u8)g_draw_target;
        key.taken = 0;
        key.poly = g_node_poly;
        key.draw = (u32)g_frame.draws.size();
        key.texture = g_texture;
        key.pack = view ? pack : nullptr;
        key.half_w = LW(g_target + 0x1c) / 2;
        key.half_h = LW(g_target + 0x20) / 2;
        key.z_scale = depth_scale(g_target);
        memcpy(key.local, g_now_l, sizeof key.local);
    }
    if (!view) return;
    float prev[16];
    clip_matrix_host(prev, pack->prev_p, pack->prev_q, before, LW(g_target + 0x1c) / 2, LW(g_target + 0x20) / 2, depth_scale(g_target));
    for (int i = 0; i < 16; i++) d.clip_mid[i] = 0.5f * (prev[i] + d.clip[i]);
    d.has_mid = true;
    g_mid_made++;
}

static inline float mid_far2(const float *a, const float *b)
{
    const float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
    return dx * dx + dy * dy + dz * dz;
}
// the most an axis turned between two matrices, in degrees
static float mid_turn(const float *a, const float *b)
{
    float least = 1;
    for (int c = 0; c < 3; c++) {
        const float *x = a + c * 4, *y = b + c * 4;
        const float lx = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]), ly = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        if (lx < 1e-12f || ly < 1e-12f) continue;
        least = std::min(least, (x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) / (lx * ly));
    }
    return acosf(std::max(-1.0f, std::min(1.0f, least))) * 57.29578f;
}

// The frame's lists are all read: which of the world's draws moved since the frame before, and from where (the third version,
// 2026-11-14; the user: "some objects of the track still jerk").
// The second version looked for each draw's strip in the frame before as the draw came, first in the lists' order and then the
// nearest of the kind, and took what it found within 8 m for the same thing a frame earlier. Scenery is full of the same model many
// times over a few metres apart (posts, fence spans, barriers, trees): whenever one of them came into sight or left it, the
// others were matched each with its NEIGHBOUR and drawn halfway to it for a picture. Passing a fence did that all the time.
// Now nothing is taken for a move that can be anything else:
//  - a draw whose strip was drawn the frame before with the very same matrix stands still, and that draw of the frame before
//    is then nobody else's past;
//  - one drawn at the very same place with another matrix turned where it stands (the trees of the desert sway in the wind,
//    hundreds of one model): from that one;
//  - of what is left on both sides, a draw moved from the nearest one of the frame before within 3 m, if no other draw left
//    is nearer to that one (a new post next to a post that moves is no move of the post);
//  - all else stands where it stands: new in sight, or its past is not known. For the scenery that is right whatever happens.
static u64 g_mid_resolve_ns, g_mid_resolves;
static void mid_resolve()
{
    if (!g_double_now) return;
    struct Timed {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { g_mid_resolve_ns += (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), g_mid_resolves++; }
    } timed;
    const size_t n = g_mid_now.size();
    std::vector<u32> &on = g_mid_order_now, &op = g_mid_order_prev;
    on.resize(n);
    for (u32 i = 0; i < (u32)n; i++) on[i] = i;
    std::sort(on.begin(), on.end(), [](u32 a, u32 b) { return mid_less(g_mid_now[a], g_mid_now[b]); });
    if (op.size() != g_mid_prev.size()) g_mid_prev.clear(), op.clear();
    const size_t m = op.size();
    static std::vector<u32> left_now, left_prev;
    const auto move_from = [](const MidKey &now, const MidKey &was) {
        if (!now.pack || now.draw >= g_frame.draws.size()) return;
        NativeDraw &d = g_frame.draws[now.draw];
        float prev[16];
        clip_matrix_host(prev, now.pack->prev_p, now.pack->prev_q, was.local, now.half_w, now.half_h, now.z_scale);
        for (int k = 0; k < 16; k++) d.clip_mid[k] = 0.5f * (prev[k] + d.clip[k]);
        g_mid_matched++;
    };
    size_t i = 0, j = 0;
    while (i < n) {
        const u64 key = mid_key(g_mid_now[on[i]]);
        size_t i_end = i;
        while (i_end < n && mid_key(g_mid_now[on[i_end]]) == key) i_end++;
        while (j < m && mid_key(g_mid_prev[op[j]]) < key) j++;
        size_t j_end = j;
        while (j_end < m && mid_key(g_mid_prev[op[j_end]]) == key) j_end++;
        left_now.clear();
        left_prev.clear();
        // the same place: both runs are in the order of their places
        size_t b = j;
        for (size_t a = i; a < i_end; a++) {
            const MidKey &now = g_mid_now[on[a]];
            while (b < j_end && mid_place_cmp(g_mid_prev[op[b]], now) < 0) b++;
            bool still = false;
            MidKey *turned = nullptr;       // at the same place with another matrix: the first that is nobody's yet, else any
            for (size_t e = b; e < j_end && mid_place_cmp(g_mid_prev[op[e]], now) == 0; e++) {
                MidKey &was = g_mid_prev[op[e]];
                if (!memcmp(was.local, now.local, sizeof now.local)) still = true, was.taken = 1;
                else if (!turned || (turned->taken && !was.taken)) turned = &was;
            }
            if (still) {
                g_mid_still++;
                if (g_mid_auditing) mid_audit(now.texture, now.target, now.pack ? MA_STILL : MA_CUT, now.poly);
            } else if (turned) {
                // Turned by more than a right angle in a frame it is another thing put at the same place (a pole and its mirrored
                // twin, as one goes out of sight and the other comes in): it stands, and that one is nobody's past.
                const float turn = mid_turn(turned->local, now.local);
                turned->taken = 1;
                if (g_mid_auditing) mid_audit(now.texture, now.target, !now.pack ? MA_CUT : turn <= 90.0f ? MA_TURNED : MA_NEW, now.poly, 0, turn);
                if (turn <= 90.0f) move_from(now, *turned);
            } else {
                left_now.push_back(on[a]);
            }
        }
        for (b = j; b < j_end; b++)
            if (!g_mid_prev[op[b]].taken) left_prev.push_back(op[b]);
        // a crowd of the same thing all moving (never seen): left standing rather than a frame spent on pairing them
        const bool crowd = left_now.size() + left_prev.size() > 1024;
        if (crowd && !left_prev.empty() && !left_now.empty()) {
            if (g_mid_crowds++ < 8 && getenv("HP2_NATIVELOG")) {
                const MidKey &k = g_mid_now[left_now[0]];
                fprintf(g_hle_log, "native: 120: a crowd at frame %u: %zu draws now and %zu of the frame before with the strip at %08x on target %u (%s), unpaired\n", g_frame_no,
                        left_now.size(), left_prev.size(), k.strip, k.target, k.texture ? (const char *)host(k.texture + 8) : "-");
            }
        }
        for (u32 a : left_now) {
            const MidKey &now = g_mid_now[a];
            const MidKey *was = nullptr;
            // 3 m a frame is 650 km/h: nothing of the game's moves so fast (a car at its best, a sign it knocked off: 1.4 m). Further
            // off it is another one of the kind: a span of a fence leaving the picture as the next but one comes into it (the
            // spans are 5 to 8 m apart; at 8 m here some 700 draws of a 100 s race were taken for moves that way).
            float least = 9.0f;
            if (!crowd)
                for (u32 q : left_prev) {
                    const float far = mid_far2(g_mid_prev[q].local, now.local);
                    if (far < least) least = far, was = &g_mid_prev[q];
                }
            if (was)
                for (u32 other : left_now)
                    if (other != a && mid_far2(g_mid_now[other].local, was->local) < least) {
                        was = nullptr;
                        break;
                    }
            if (g_mid_auditing)
                mid_audit(now.texture, now.target, !now.pack ? MA_CUT : was ? MA_MOVED : MA_NEW, now.poly, was ? sqrtf(least) : 0, was ? mid_turn(was->local, now.local) : 0);
            if (was) move_from(now, *was);
        }
        i = i_end;
        j = j_end;
    }
}

// a frame begins: whether it is one to draw twice, and the last frame's draws to match this one's with
static void mid_frame()
{
    static const bool off = getenv("HP2_NO120") != nullptr;
    g_double_now = !off && pc_picture_rate() > 60 && pc_racing();
    g_mid_prev.swap(g_mid_now);
    g_mid_now.clear();
    g_mid_order_prev.swap(g_mid_order_now);
    g_mid_order_now.clear();
    g_poses_prev.swap(g_poses_now);
    g_poses_now.clear();
    g_car_moves.clear();
    g_rvm_was = g_rvm_now;
    g_rvm_now = false;
    g_pcar_was = g_pcar_now;
    g_pcar_now = false;
    shadow_frame();
    g_parts_prev.swap(g_parts_now);
    g_parts_now.clear();
    std::sort(g_parts_prev.begin(), g_parts_prev.end(), [](const MidParticle &a, const MidParticle &b) { return a.at < b.at; });
    g_parts_systems_n = 0;
    g_particle_ranges.clear();
    port_sprites_frame();
    g_owners.clear();
    g_owner = g_strip_owner = g_owner_last = nullptr;
    if (g_double_now && g_frame_no % 600 == 0 && getenv("HP2_NATIVELOG"))
        fprintf(g_hle_log, "native: 120: %llu draws of strips so far, %llu with a halfway matrix, %llu a car's, %llu at the place of the frame before, %llu moved, "
                           "%llu cuts, %llu crowds; %llu draws of packets moved; %zu cars the last frame; the pairing took %.3f ms a frame; "
                           "%llu sprites of particles: %llu from their place a frame before, %llu born since, %llu with no particle found\n",
                (unsigned long long)g_mid_draws, (unsigned long long)g_mid_made, (unsigned long long)g_mid_owned, (unsigned long long)g_mid_still,
                (unsigned long long)g_mid_matched, (unsigned long long)g_mid_cuts, (unsigned long long)g_mid_crowds, (unsigned long long)g_mid_direct,
                g_poses_prev.size(), g_mid_resolves ? (double)g_mid_resolve_ns / (double)g_mid_resolves * 1e-6 : 0.0, (unsigned long long)g_mid_sprites,
                (unsigned long long)g_mid_sprites_tracked, (unsigned long long)g_mid_sprites_born, (unsigned long long)g_mid_sprites_lost);
    if (g_mid_auditing && g_double_now && g_frame_no % 1800 == 0) mid_audit_write();
}

// The list's strips looked up ahead, by the workers (cores.h). A strip seen before is found by its address in a table of 16 MB, a line
// of the CPU's cache a strip, and a heavy frame has 65 thousand strips: the waits for memory were most of the reading. They are
// all independent, and the lookup writes nothing (the game's thread stands meanwhile, so nothing writes the game's memory either):
// the workers do it for the whole list at once, and the reading, still one strip after another in the list's order, takes each
// answer from g_ahead. A list of fewer than 2048 strips is not worth waking them.
// OFF unless HP2_LOOKAHEAD=1. Measured 2026-11-14 (demo 4, the user's settings, two pairs of runs one after another): the lists cost
// 1.37 and 1.18 ms a frame with it, 1.14 and 1.06 without, and the game's thread 75 to 77 bn cycles against 73. The waits for memory
// were already overlapped by the reading's own prefetch (four nodes ahead), so what the workers save is less than waking them and
// walking the list twice cost. Checked to give the same strips (HP2_AHEADCHECK=1: 10 million, none differ); kept for a slower
// processor to be measured on.
static std::vector<StripAhead> g_ahead;
static u32 g_ahead_at;                      // the next strip's place in it, as the list is read
static bool g_ahead_on;
static u64 g_ahead_lists, g_ahead_strips, g_ahead_known;
static void look_ahead(u32 list)
{
    static const bool off = getenv("HP2_LOOKAHEAD") == nullptr;
    g_ahead_on = false;
    if (off || !cores_workers() || !g_cache_cap || g_cache_off || g_cache_checking) return;
    struct Packet {
        u32 data, qwords;
    };
    static std::vector<Packet> strips;
    strips.clear();
    for (u32 node = LW(list); node; node = LW(node)) {
        const u32 type = LH(node + 4);
        if (type != EDMA_STRIP && type != EDMA_POLY3D) continue;
        const u32 count = LH(node + 8);
        for (u32 i = 0; i < count; i++) strips.push_back({LW(node + 12 + i * 8), LH(node + 10 + i * 8)});
    }
    const size_t n = strips.size();
    if (n < 2048) return;
    if (g_ahead.size() < n) g_ahead.resize(n);
    if (g_strip_seen.empty()) strip_seen(0);        // the table made here, not by two workers at once
    const u32 generation = g_strip_generation;
    const size_t piece = 1024;
    jobs_each((int)((n + piece - 1) / piece), [&](int at, int) {
        const size_t from = (size_t)at * piece, to = std::min(n, from + piece);
        for (size_t i = from; i < to; i++) {
            if (i + 8 < to) __builtin_prefetch(strip_seen(strips[i + 8].data));
            const u32 data = strips[i].data, qwords = strips[i].qwords;
            const StripSeen *seen = strip_seen(data);
            StripAhead &a = g_ahead[i];
            a.known = seen->fast && seen->data == data && seen->qwords == qwords && seen->generation == generation &&
                      seen->stamp == memwatch_stamp(data, qwords * 16);
            if (a.known) a.strip = seen->strip;
        }
    });
    g_ahead_at = 0;
    g_ahead_on = true;
    g_ahead_lists++;
    g_ahead_strips += n;
    if (g_native_stats)
        for (size_t i = 0; i < n; i++) g_ahead_known += g_ahead[i].known;
}

void native_read_list(u32 list)
{
    g_stat.lists++;
    g_list_serial++;                        // a draw's state and the view's pack are kept within a list only (strip_state, clip_read)
    g_pack_stale = true;
    look_ahead(list);
    // A strip is known by its address from a table of 16 MB (strip_seen), a line of the CPU's cache a strip, and a heavy frame asks for
    // 65 thousand of them all over it: each was a wait for memory. So the line of the strip four nodes on is asked for now, and the
    // wait is over by the strip's turn. (RenderStrips makes a node a strip.)
    const bool by_address = g_cache_cap && !g_cache_off && !g_ahead_on;
    u32 ahead = LW(list);
    const auto ask = [&] {
        if (!ahead || !by_address) return;
        const u32 t = LH(ahead + 4);
        if (t == EDMA_STRIP || t == EDMA_POLY3D) {
            __builtin_prefetch(strip_seen(LW(ahead + 12)));
        } else if (t == EDMA_RUN) {         // a run's record, wherever it is among the frame's
            const u32 at = LW(ahead + 12);
            if (at < g_runs.size()) __builtin_prefetch(&g_runs[at]);
        }
    };
    ask();
    for (int i = 0; i < 3 && ahead; i++) ahead = LW(ahead), ask();
    const bool owners = !g_owners.empty();
    g_owner = nullptr;
    for (u32 node = LW(list); node; node = LW(node)) {
        if (ahead) ahead = LW(ahead), ask();
        u32 type = LH(node + 4), data = LW(node + 12);
        // the next node's run: its record was asked for three nodes ago, now its strips and its ranges
        if (by_address)
            if (const u32 next = LW(node))
                if (LH(next + 4) == EDMA_RUN) {
                    const u32 at = LW(next + 12);
                    if (at < g_runs.size()) {
                        const DirectRun &r = g_runs[at];
                        if (r.strip0 < g_run_strips.size()) __builtin_prefetch(&g_run_strips[r.strip0]);
                        if (r.range0 < g_run_ranges.size()) __builtin_prefetch(&g_run_ranges[r.range0]);
                    }
                }
        if (owners) g_owner = mid_owner_of(node);
        if (g_native_stats) g_nodes_seen[type & 15]++;
        switch (type) {
        case EDMA_SETTARGET:
            g_target = data;
            g_target_ptr = data;
            g_mode = LH(node + 6);
            g_pack_stale = true;
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
            g_pack_stale = true;
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
            {   // TEMP HP2_FXLOG: the packets as the lists have them
                static const bool on = getenv("HP2_FXLOG") != nullptr;
                if (on && g_frame_no % 120 == 0)
                    fprintf(g_hle_log, "fxlog: frame %u list %08x packet for target %u (%s) tex %s, %u qwords, data %08x%s%c", g_frame_no, list, g_target ? LW(g_target) : 99,
                            g_target ? pc_str(LW(g_target + 4)).c_str() : "-", g_texture ? (const char *)host(g_texture + 8) : "-", LH(node + 10), data,
                            mid_particle_data(data) ? " (particles)" : "", 10);
            }
            if (main_target()) {
                g_draw_target = LW(g_target);
                if (!port_sprites_emit(data)) add_direct(data, LH(node + 10));
            }
            break;
        case EDMA_RUN: {                    // strips handed out as a run (the direct output)
            if (g_native_stats && g_target && LW(g_target) < 16) {
                auto &t = g_by_target[LW(g_target)];
                t.strips += LH(node + 8);
                t.pack = g_pack;
                t.target = g_target;
            }
            if (data >= g_runs.size() || LH(node + 10) != (u16)g_frame_no) {      // not this frame's: nothing of it is left
                if (g_direct_stale++ < 5) fprintf(g_hle_log, "native: a run's node of another frame in list %08x (frame %u), not drawn\n", list, g_frame_no);
                break;
            }
            if (main_target() && g_pack && g_local) {
                g_draw_target = LW(g_target);
                g_node_poly = false;
                add_run(g_runs[data], LH(node + 6));
            }
            break;
        }
        case EDMA_POLY3D:                   // the same packet as a strip's: ePoly::Render3D, eEndStrip
        case EDMA_STRIP: {
            // this node's strips in g_ahead: counted for every node of the kind, drawn or not, as look_ahead counted them
            const u32 ahead_at = g_ahead_at;
            if (g_ahead_on) g_ahead_at += LH(node + 8);
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
                if (type == EDMA_POLY3D && shadow_poly(node)) break;       // Shadows: High: one of a car's two, drawn the port's way
                g_draw_target = LW(g_target);
                u32 count = LH(node + 8);
                g_node_poly = type == EDMA_POLY3D;
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
                    add_strip(LW(node + 12 + i * 8), LH(node + 10 + i * 8), LH(node + 6), g_ahead_on ? &g_ahead[ahead_at + i] : nullptr);
                }
            }
            break;
        }
        }
    }
    g_ahead_on = false;
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

static void native_scenery_log();           // the scenery's own loop (below): its numbers
static void depth_settle();                 // the sun's depth the game asked for, now (below, by HOOK_FetchVRAM)

// ---- the renderer's thread ----------------------------------------------------------------------------
// The lists are read on the game's thread, as they are handed over (native_read_list): they point all over the game's live data. What
// comes of them, the frame (NativeFrame: vertices, indices, draws, the textures decoded for it), is all the Vulkan side needs, and it
// reads nothing of the game's. So the finished frame goes to a thread that records and submits it while the game's thread is already at
// the next one. One frame in hand at most: the game's thread waits for the renderer's before it hands over another, and before it asks
// for the last frame's depth (the sun's flare). HP2_RENDERSYNC=1: drawn on the game's thread, as it was.
// A queue of one (2026-11-18, the user: "let the game stop waiting for the renderer"). With VSync and a rate that is not a whole
// number of pictures a frame (144, 165, 75 on such a screen) a frame takes the renderer two refreshes or three by turns, and the
// game, waiting for it at the hand-over and again for the sun's depth, had its own frames of uneven length: on a model of it
// (work/nshots/t5/sim_queue.py) 2300 of 5800 steps were held at 144 Hz. There a frame may now wait in a place of its own while
// the renderer finishes the one before, and the game's thread goes on; it waits only while that place is taken. The renderer,
// when a frame has waited for it more than 12.5 ms, leaves one picture out and so catches up (native_vk.cpp): a screen is never
// exactly its nominal rate, and the queue would otherwise fill and stay full. Everywhere else it is as it was: a rate of 60, 120,
// 240 has the same pictures every frame and holds the game evenly at the screen's own pace, where a queue would only add a frame
// of delay; without VSync the renderer keeps to the frame's own time by itself. HP2_NOQUEUE=1: never.
static NativeFrame g_out;                   // what the renderer's thread has: g_frame as it was at the hand-over
static NativeFrame g_next;                  // the queue's place: handed over while g_out was still in hand
static std::mutex g_render_mx;
static std::condition_variable g_render_cv;
static bool g_render_busy, g_render_started;
static bool g_next_full;                    // (under g_render_mx, as g_render_busy is)
static bool g_queue_now;                    // the game's thread: the frame handed over last went by the queue's rules
static const bool g_render_sync = getenv("HP2_RENDERSYNC") != nullptr;
static u32 g_seen_epoch;                    // g_native_epoch as this thread last took note of it
void ee_float_mode_here();
void pc_heap_report(u32 frame);

static void render_thread()
{
    stall_render_thread();
    phase_thread("renderer");
    cores_thread(CORE_RENDER);
    ee_float_mode_here();                   // the Vulkan side's own sums as they were on the game's thread: cut toward zero
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_render_mx);
            g_render_cv.wait(lock, [] { return g_render_busy; });
        }
        native_vk_draw(g_out);
        {
            std::lock_guard<std::mutex> lock(g_render_mx);
            if (g_next_full) {              // the one that waited: in hand at once
                std::swap(g_out, g_next);
                g_next_full = false;
            } else {
                g_render_busy = false;
            }
        }
        g_render_cv.notify_all();
    }
}

void native_render_wait()
{
    if (!g_render_started) return;
    Phase timed(PH_WAIT_RENDER);
    std::unique_lock<std::mutex> lock(g_render_mx);
    g_render_cv.wait(lock, [] { return !g_render_busy && !g_next_full; });
}

// The sun's depth, waited for no longer than it has to be (2026-11-26, the user: "the exact way"). See HOOK_FetchVRAM below.
static uint32_t g_hand_count;               // the game's thread: frames handed over (NativeFrame::handed)
static uint32_t g_depth_sent_no;            // (under g_render_mx) the last frame whose depth rects the GPU has on their own fence
void native_depth_sent(uint32_t handed)
{
    {
        std::lock_guard<std::mutex> lock(g_render_mx);
        g_depth_sent_no = handed;
    }
    g_render_cv.notify_all();
}
// The renderer made again for other settings (2026-11-26, the user's item 3): it took 0.1 to 4 s, the game's thread waited at the
// hand-over all that time, and the sound, which it feeds, went round the last two seconds of the music. Now, while it is at it, the
// game goes on: a frame it hands over then is not drawn (it would not be anyway: the renderer's cache and textures are gone, and the
// frames made before the game hears of that, NativeFrame::epoch, are not drawn either), its textures' uploads wait for the next
// frame that is handed over, and the picture stands till the renderer is back. Not while the player drives in a race (pc_car_input:
// the pause menu and the race's other menus are not driving): there the game is held as before, rather than run on with no picture.
// HP2_REBUILDHOLD=1: held always, as it was.
static bool g_rebuilding;                   // (under g_render_mx) the renderer's thread is in rebuild()
static std::vector<NativeTexUpload> g_carried;  // the game's thread: uploads of the frames not handed over, in the order they came
void native_rebuild_mark(bool on)
{
    {
        std::lock_guard<std::mutex> lock(g_render_mx);
        g_rebuilding = on;
    }
    g_render_cv.notify_all();
}

// the game's thread: till that frame's depth is with the GPU on its own, or the renderer has no frame in hand (a frame it did not
// draw, or one with its depth at the end as before: then all of it, as native_render_wait). False: the renderer is being made again,
// and the frame's depth is not there to read (that frame is not drawn: the words stay as they are, as they did when it was waited for)
static bool native_depth_wait(uint32_t handed)
{
    if (!g_render_started) return true;
    Phase timed(PH_WAIT_RENDER);
    std::unique_lock<std::mutex> lock(g_render_mx);
    g_render_cv.wait(lock, [handed] { return g_depth_sent_no == handed || (!g_render_busy && !g_next_full) || g_rebuilding; });
    return g_depth_sent_no == handed || (!g_render_busy && !g_next_full);
}

// A frame may wait for the renderer in the queue's place: VSync, pictures between the frames, and not the same number of them every
// frame (the rate as the renderer will have it, native_vk.cpp's picture_rate: no more than the screen's).
static bool queue_wanted()
{
    static const bool off = getenv("HP2_NOQUEUE") != nullptr;
    if (off || !g_frame.doubled || !g_cfg.vsync) return false;
    int rate = g_frame.pictures;
    if (rate >= 1000) return false;
    const int hz = native_vk_screen_hz();
    if (hz && rate > hz) rate = hz;
    return rate > 60 && rate % 60 != 0;
}

// the frame to the renderer; g_frame is then one of those before it, to be emptied and filled again
static void hand_over()
{
    g_frame.handed = ++g_hand_count;
    if (g_render_sync) {
        native_vk_draw(g_frame);
        return;
    }
    const bool queue = queue_wanted();
    g_queue_now = queue;
    g_frame.queued = queue;
    g_frame.made_s = native_vk_clock();
    // the uploads of frames that were not handed over, before this one's (see g_rebuilding)
    static u32 not_drawn;
    if (!g_carried.empty()) {
        g_carried.insert(g_carried.end(), std::make_move_iterator(g_frame.uploads.begin()), std::make_move_iterator(g_frame.uploads.end()));
        g_frame.uploads.swap(g_carried);
        g_carried.clear();
    }
    static const bool hold = getenv("HP2_REBUILDHOLD") != nullptr;
    const bool may_go_on = !hold && !(pc_racing() && pc_car_input());
    bool dropped = false;
    {
        Phase timed(PH_WAIT_RENDER);
        std::unique_lock<std::mutex> lock(g_render_mx);
        auto room = [queue] { return queue ? !g_next_full : !g_render_busy && !g_next_full; };
        g_render_cv.wait(lock, [&] { return room() || (may_go_on && g_rebuilding); });
        if (!room()) {
            dropped = true;                 // the renderer is being made again: the game goes on
        } else if (!g_render_busy) {
            std::swap(g_frame, g_out);
            g_render_busy = true;
        } else {
            std::swap(g_frame, g_next);
            g_next_full = true;
        }
    }
    if (dropped) {
        g_carried.swap(g_frame.uploads);    // (g_frame is emptied and filled again, as one handed over would be)
        g_frame.uploads.clear();
        not_drawn++;
        return;
    }
    if (not_drawn) {
        fprintf(g_hle_log, "native: %u frames went on while the renderer was made again (not drawn); their textures went with this one\n", not_drawn);
        not_drawn = 0;
    }
    if (!g_render_started) {
        g_render_started = true;
        std::thread(render_thread).detach();
    }
    g_render_cv.notify_all();
}

// The 2D layer in a picture between two frames (the user, 2026-11-15: "the HUD's lines"). The game draws it 60 times a second: a
// message sliding in, the needle of the rev counter, a dot on the map stood still in the pictures between and jumped with the frame.
// A draw of the layer has the game's object that drew it and its place among that object's (g_hud_draws, from g_packet_who): the
// same object's same poly a frame before is where it comes from, and the picture between has its vertices on the way (a second
// copy halfway, NativeDraw::mid_verts, which the renderer puts on the line for any picture). An object goes as a whole or not at
// all: one that was not there a frame before, that has another number of polys now (a text grown longer), or that went further than
// kHudJump in the frame (put somewhere else, not moved there) is drawn as the frame has it.
// It was done by looks till 2026-11-18: a draw was "the same" as one of the frame before with the same texture, vertex count and
// places in the texture within 40 pixels. But the HUD's text comes a letter a packet, so that was "the same letter near by": where a
// text changed, its letters came from the same letters of the next place or the next row (a tester on a 144 Hz screen: the labels
// in the list of racers blink). hp2.ini's hud_smooth=0: the layer as the game draws it, in every picture.
struct HudSeen {
    u64 group;
    u32 index, first, count;                // first: in g_hud_prev (the frame before) or in the frame's own list (this one)
    u32 draw, tex;
};
static std::vector<HudSeen> g_hud_seen;     // the frame before, by group and index
static std::vector<NativeVertex> g_hud_prev;
static const float kHudJump = 64.0f;
static void hud_between()
{
    static std::vector<HudSeen> now;
    static std::vector<NativeVertex> now_verts;
    static const bool audit = getenv("HP2_HUDAUDIT") != nullptr;
    now.clear();
    now_verts.clear();
    if (!g_double_now || !g_cfg.hud_smooth) {
        g_hud_seen.clear();
        g_hud_prev.clear();
        g_hud_draws.clear();
        g_hud_strangers.clear();
        return;
    }
    for (const HudDraw &h : g_hud_draws) {
        if (h.draw >= g_frame.draws.size()) continue;
        const NativeDraw &d = g_frame.draws[h.draw];
        if ((d.target != 0 && d.target != 3) || !d.count || d.indexed || d.ranges || d.mid_verts || (d.flags & kPartFlag)) continue;
        now.push_back({h.group, h.index, (u32)now_verts.size(), d.count, h.draw, h.tex});
        now_verts.insert(now_verts.end(), g_frame.vertices.begin() + d.first, g_frame.vertices.begin() + d.first + d.count);
    }
    std::stable_sort(now.begin(), now.end(), [](const HudSeen &a, const HudSeen &b) { return a.group != b.group ? a.group < b.group : a.index < b.index; });
    int groups = 0, moved = 0, still = 0, fresh = 0, other = 0, jumped = 0, moved_draws = 0;
    float furthest = 0;
    const bool tell = audit && g_frame_no % 120 == 0;
    std::string told;                       // HP2_HUDAUDIT: what became of each object that is not standing still
    for (size_t a = 0; a < now.size();) {
        size_t b = a;
        while (b < now.size() && now[b].group == now[a].group) b++;
        groups++;
        // the same object a frame before
        const auto less = [](const HudSeen &h, u64 group) { return h.group < group; };
        const auto was = std::lower_bound(g_hud_seen.begin(), g_hud_seen.end(), now[a].group, less);
        size_t n = 0;
        while (was + n != g_hud_seen.end() && was[n].group == now[a].group) n++;
        bool same = n == b - a;
        for (size_t k = 0; same && k < n; k++) same = was[k].index == now[a + k].index && was[k].count == now[a + k].count;
        float far = 0;
        if (same)
            for (size_t k = 0; k < n; k++)
                for (u32 i = 0; i < was[k].count; i++) {
                    const NativeVertex &p = g_hud_prev[was[k].first + i], &q = now_verts[now[a + k].first + i];
                    far = std::max(far, std::max(fabsf(p.x - q.x), fabsf(p.y - q.y)));
                }
        if (tell && !(same && far < 0.01f)) {
            char line[160];
            snprintf(line, sizeof line, " [%s x%zu %s %.1f]", now[a].tex ? (const char *)host(now[a].tex + 8) : "-", b - a,
                     !same ? (n ? "resized" : "new") : far > kHudJump ? "jumped" : "moved", far);
            told += line;
        }
        if (!same) (n ? other : fresh)++;
        else if (far < 0.01f) still++;
        else if (far > kHudJump) jumped++;
        else {
            moved++;
            furthest = std::max(furthest, far);
            for (size_t k = 0; k < n; k++) {
                NativeDraw &d = g_frame.draws[now[a + k].draw];
                const u32 m0 = (u32)g_frame.vertices.size();
                g_frame.vertices.resize((size_t)m0 + d.count);
                for (u32 i = 0; i < d.count; i++) {
                    const NativeVertex &p = g_hud_prev[was[k].first + i];
                    NativeVertex v = g_frame.vertices[d.first + i];
                    v.x = 0.5f * (p.x + v.x), v.y = 0.5f * (p.y + v.y);
                    g_frame.vertices[m0 + i] = v;
                }
                d.mid_first = m0;
                d.mid_verts = true;
                moved_draws++;
            }
        }
        a = b;
    }
    if (tell) {
        int layer = 0;
        for (const NativeDraw &d : g_frame.draws) layer += (d.target == 0 || d.target == 3) && d.count;
        fprintf(g_hle_log, "hudaudit: frame %u: the layer has %d draws, %zu of them owned, in %d objects: %d moved (%d draws, %.1f pixels at the most), %d still, "
                           "%d new, %d of another size, %d jumped; with no owner:", g_frame_no, layer, now.size(), groups, moved, moved_draws, furthest, still, fresh, other, jumped);
        for (const auto &t : g_hud_strangers) fprintf(g_hle_log, " %s %d", t.first.c_str(), t.second);
        fprintf(g_hle_log, "; not still:%s%c", told.c_str(), 10);
    }
    g_hud_strangers.clear();
    g_hud_draws.clear();
    g_hud_seen.swap(now);
    g_hud_prev.swap(now_verts);
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
    // The blend of the race's sphere map pass over a gloss map (VU1's kick 0x31c) is five words the game keeps (0x32f70c on:
    // A, B, C, D, FIX), put into ALPHA by eDisplayFrame's header (0x10b0a0). As the disc has them: 0, 2, 1, 1, 0x10, which is
    // 0x58, Cs Ad + Cd: the map added by the gloss the first pass left. The Chrome Cars cheat (the game's render mode 12, a list
    // of words it overwrites: pcsettings.cpp, cheat_words) makes them 2, 2, 1, 0, 0x80: (0 - 0) Ad + Cs, the map itself over the
    // paint. The pass is the renderer's own here, so it has to ask (the user, 2026-11-24: "I see nothing of Chrome Cars").
    g_frame.chrome = LW(0x32f70c) == LW(0x32f710) && LW(0x32f718) == 0;
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
        verts[g_lod_flip_now] += (double)(g_frame.strip_vertices + g_frame.vertices.size());
        draws[g_lod_flip_now] += (double)g_frame.draws.size();
        n[g_lod_flip_now]++;
        if (n[0] + n[1] >= 600 && n[0] && n[1]) {
            fprintf(g_hle_log, "native: lodflip: Original %.0f vertices, %.0f draws; Maximum %.0f vertices, %.0f draws\n", verts[0] / n[0], draws[0] / n[0],
                    verts[1] / n[1], draws[1] / n[1]);
            verts[0] = verts[1] = draws[0] = draws[1] = 0;
            n[0] = n[1] = 0;
        }
    }
    mid_resolve();
    resolve_wide_2d();
    hud_between();
    shadow_end();
    ao_end();
    window_cursor_frame();
    native_texture_take_uploads(g_frame.uploads);
    g_frame.number = g_frame_no;
    {   // the frame's length as the game has it: its real time step (gp-19596, set by PrepareRealTimestep from the ticker)
        const u32 bits = LW(g_pc_ctx->r[28].w[0] - 19596);
        memcpy(&g_frame.step_s, &bits, 4);
    }
    g_frame.dumping = native_dumping_now();
    g_frame.epoch = g_seen_epoch;
    idx_send();
    geo_check_frame();
    clip_check_frame();
    g_frame.geo_indexed = g_geo;
    g_frame.cache_top = cache_used();
    depth_settle();                         // the sun's depth of the frame before, if StuffSun did not take it: before that frame's rects go
    hand_over();
    memwatch_frame();
    pc_heap_report(g_frame_no);
    stall_test();             // HP2_HEAPLOG
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
    // emptied for want of room (not for a new size): the renderer hears of it with the next frame, and asks for a larger buffer
    const u32 for_room = cap && cap == g_cache_cap && cache_used() + margin > cap ? cache_used() : 0;
    // the strips by indices or not: by the setting and the GPU, changed between frames only, with the cache emptied
    static const bool geo_wanted = (getenv("HP2_GEOINDEX") ? atoi(getenv("HP2_GEOINDEX")) != 0 : kGeoDefault) && g_ranged;
    const bool geo = geo_wanted && native_vk_geo_ok();
    if (cap != g_cache_cap || cache_used() + margin > cap || geo != g_geo) {
        if (g_cache_top || cap != g_cache_cap || geo != g_geo) cache_flush();
        g_geo = geo;
        g_cache_cap = cap;
    }
    g_frame.dyn_base = g_cache_cap;
    g_frame.uploads.clear();
    g_frame.indices.clear();
    g_frame.ranges.clear();
    g_runs.clear();
    g_run_ranges.clear();
    g_run_strips.clear();
    g_frame.strip_vertices = 0;
    g_frame.cache_short = for_room;
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
        fprintf(g_hle_log, "native: direct output%s: %llu strips in %llu runs so far, %llu runs gone the long way, %llu nodes of another frame\n", direct_on() ? "" : " (off)",
                (unsigned long long)g_direct_strips, (unsigned long long)g_direct_runs, (unsigned long long)g_direct_slow, (unsigned long long)g_direct_stale);
        if (g_direct_check) fprintf(g_hle_log, "native: directcheck: %llu runs compared, %llu not the same\n", (unsigned long long)g_direct_checked, (unsigned long long)g_direct_bad);
        native_scenery_log();
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
    g_frame.packs.clear();
    g_frame.casters.clear();
    g_frame.shadows.clear();
    g_frame.lights.clear();
    g_frame.fx.clear();
    g_packet_spans.clear();
    g_packet_who.clear();
    g_packet_edge.clear();
    g_hud_times.clear();
    g_hud_draws.clear();
    g_wide_2d.clear();
    g_light_ctx = g_light_mat = 0;
    g_light = -1;
    g_target = g_pack = g_local = g_mode = g_texture = 0;
    g_pack_stale = true;
    g_pack_slot = -1;
    g_state_kept.have = false;
    g_clip_new = true;
    memset(&g_stat, 0, sizeof g_stat);
    memset(g_by_target, 0, sizeof g_by_target);
}

void HOOK_AttachDmaDataListMain__FP9eDmaChainP12eDmaDataListP8DMAState(Ctx &c, void (*orig)(Ctx &))
{
    // What the game does next is to turn the list into packets for VU1 and the GS, and nothing reads those: the list is what is drawn.
    // HP2_KEEPPACKETS=1 builds them all the same.
    const auto t0 = std::chrono::steady_clock::now();
    Phase timed(PH_LISTS);
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

// ---- Shadows: High, Very High and Ultra (Options > Graphics > Shadows; the user, 2026-11-15) -----------------------------
// The game's two shadows of a car are polygons of CarRenderInfo::Render (0x120ef8), made anew every frame in the world's own space:
//  - from the sun (texture SHADOWRAMP; 0x121d70 on): the corners of the car's collision box moved along the sun's ray to the car's
//    height, their convex outline, each point of it dropped onto the terrain, the corners rounded (0x120c00), a fan and a soft rim.
//    Its strength is 90 by the sunlight where the car is (the car's light context +212: none below 0.4). Not in the mirror, not
//    for a car at its third level of detail or lower, fading at the second by the car's size in the PS2's pixels;
//  - the patch under the car (texture <car>_SHADOWIG; 0x122454 on): a grid of 4 x 4 over the terrain.
// At High the port draws both itself, where the frame's lists have them:
//  - the patch is laid over the picture by the picture's own depth (NativeShadow, kind 0): every pixel whose place in the world is
//    under the polygon, a little above or below it, takes the texture there. It lies on the kerb, the grass, another car;
//  - the sun's shadow is the car's own outline (kind 1): the car's opaque draws are drawn once more as the sun sees them into the
//    car's cell of a map that keeps the furthest of them along the ray (NativeShadowCaster; the ray is the game's own, 0x120d60),
//    and every pixel of the picture further along that ray than the car is in its shadow. It is made for every car a view draws,
//    the mirror's too, whatever its level of detail, and fades by the car's size on the real picture (the renderer's part);
//  - the game's polygons of both kinds are then left out of the players' views and the mirror. A car there is no cell for (more
//    than 32 at once), or one whose body was not drawn, keeps the game's.
// The players' views get them at the place of the first such polygon in the lists (an effect over the picture, FX_SHADOW, as the
// fog is one); the mirror after all the rest of it. Very High (Maximum till 2026-11-26) is High with a map of twice the size and the
// tunnels' lamps; Ultra twice that again.
static int shadows_level()
{
    static const char *env = getenv("HP2_SHADOWS");
    // a race's only: the showroom and the menus keep the game's own (the user, 2026-11-17: the port's shadow is not wanted there)
    if (!pc_racing()) return 0;
    return env ? atoi(env) : g_cfg.shadows;
}

static const int kShadowCells = 32;                 // the map is 8 x 4 cells, a car each (native_vk.cpp, kShadowAcross)
// A cell is so many metres either way of the car, and along the ray, at the least: a car's own are larger if the car is (shadow_car;
// they were these for every car till 2026-11-17, and a bus or a truck was cut at the cell's edge and at its depth)
static const double kShadowReach = 3.4, kShadowDeep = 4.0;
// The soft edge (native_shadow.frag): it reaches so many metres either way of the outline for each metre, along the ray, that the
// car is above the place, no less than kShadowSoftLeast and no more than kShadowSoftMost. (It was 0.05 everywhere.) hp2.ini's
// shadow_softness, a percent, makes the first and the last wider or narrower.
static const double kShadowSoftRise = 0.07, kShadowSoftLeast = 0.012, kShadowSoftMost = 0.12;
struct ShadowCar {
    u32 info;                               // the CarRenderInfo
    float pose[16];
    double light[16];                       // the world to the car's cell: x and y -1..1, the third 0..1 away from the sun
    double reach, deep;                     // the cell is so many metres either way of the car, and twice so many along the ray
    int cell;                               // -1: none left
    u32 cast_verts[16];                     // by the target's number: the vertices of the car's draws there that can cast
    float strength;                         // 90 by the sunlight, over 128; below 0: not known yet
    u32 made;                               // by the target's number: its shadow from the sun is among the frame's
    int patch[16];                          // by the target's number: its patch among the frame's shadows, -1: none
    bool moves;                             // MidOwner's: where the car was a frame before
    float move[16];
    double radius;                          // a ball about the car's middle (pose + half a metre up) that holds all of it
    struct Lamp {                           // Very High and Ultra: one of its tunnel lamps ("the tunnels' lamps" below)
        double clip[16];                    // the world to the lamp's cell: x, y -1..1 over w, the third K (z - n) over w = z
        double n, k, tn, fade;              // the near distance, K, the half-width's tangent; where its shadow has faded out
        double size;                        // how wide the lamp is, metres: the soft edge
        double scale;                       // how much of its light counts (lamps_pick)
        double at[3];                       // where it is
        double cut[4];                      // where its shadow ends (lamp_cut), in the world: a x + b y + c z <= d; 0, 0, 0, 1: nowhere
        int cell;                           // of the lamps' cells, 0..kLampCells-1
    } lamp[4];                              // kLampsPerCar
    int block;                              // its lamps' block in the frame's lights (lamps_pick); -1: none
    int lamps;                              // how many of lamp[] there are
    u32 lamp_made;                          // by the target's number: its lamps' shadows are among the frame's
    float tunnel;                           // how much the sun is hidden from it, 0..1 (lamps_tunnel): its shadow is so much less
    float roofed;                           // how much it is under a roof: the lamps' shadows are so much
    bool roof_known;                        // its ground was found (else the lamps' shadows stay as they were)
};
static std::vector<ShadowCar> g_shadow_cars;        // this frame's, a car once whatever views draw it
// Whose each of g_frame.casters is: the car and the view its draw is of. A car goes into its cell of the map once, by one view's
// draws of it, and that used to be the first view that drew it. With two players the first is player 1's view, where player
// 2's car and the traffic about it are far off: drawn in fewer triangles, or not at all in one frame and there in the next. So
// the outline of the shadows in player 2's view changed with what player 1's view happened to draw (the user, 2026-11-23:
// "in split screen the shadow flickers, for player 2"). Every view's draws are written down now, and at the frame's end the
// map keeps, for each car, those of the view that drew most of it (shadow_end): the one it is nearest in.
struct CasterOf {
    u16 car;
    u8 target;
};
static std::vector<CasterOf> g_caster_of;
static bool g_shadow_fx[16];                        // by the target's number: its FX_SHADOW is queued
static u32 g_shadow_rvm_at = ~0u;                   // the mirror: the draw its first shadow polygon stood before; ~0: none yet
// A shadow's own space to the picture (the inverse of NativeShadow::to, made beside it in doubles), for the part of the view the
// shadow can fall on at all (shadow_end): g_frame.shadows' own order
struct ShadowFrom {
    double now[16], mid[16];
    bool has_mid;
};
static std::vector<ShadowFrom> g_shadow_from;
// the patch whose polygons are being read: all its corners so far, for the plane through them (shadow_poly)
static struct {
    int shadow;                             // among g_frame.shadows; -1: none
    u32 n;
    float x[256], y[256], z[256];
    double map[6];                          // u and v by x and y: u = map[0] x + map[1] y + map[2], v the next three
} g_patch = {-1};

static void shadow_frame()
{
    g_shadow_cars.clear();
    g_caster_of.clear();
    g_shadow_from.clear();
    g_patch.shadow = -1;
    g_shadow_rvm_at = ~0u;
    memset(g_shadow_fx, 0, sizeof g_shadow_fx);
}

static inline void mul_d(double out[16], const double a[16], const double b[16])       // columns: out = a * b
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

// ---- The tunnels' lamps (Shadows: Very High and Ultra; the user, 2026-11-25, Very High since 2026-11-26) ------------------------
// A car in a tunnel gets the shadows of the lamps that light it most, made the way the sun's is: the car's draws once more as
// the lamp sees them, now in perspective, into a cell of the map's own rows for the lamps (native_vk.cpp), and the picture's pixels
// further from the lamp than the car are in its shadow (native_shadow.frag, lamps). The sun's shadow is gone under a roof (lamps_tunnel).
// The lamps are the ones on the tunnels' ceilings that one sees: the track's flares (LoaderLightList, 0x114f38, links each pack of
// them into the list at gp - 24272: the count at +48, the flares listed from +56, 80 bytes each, its name at +16 and x y z at +48),
// those named for a tunnel (EF_TunnelA, EF_TunnEL_A). They all face down. The game's own lights of kind 2 (the list at gp - 24264:
// the pack's header has them at +20 and their count at +24, 96 bytes each, the kind at +36, the name at +0, x y z at +48) are no
// lamps one sees: points the game lights a car's body by, two at most, 1 to 4 metres below and aside of the lamps, on two of the
// locations one between the two lamps of a pair across the tunnel; and they reach 7 to 13 metres, so a shadow by their weight was
// there only with the lamp all but over the car, falling under it (the user, 2026-11-25: "still very faint"). They are the lamps
// only where a track has no tunnel flares.
// How the shadows of several lamps lie over each other (the user, 2026-11-26: "the shadow jumps from one lamp to the next and
// makes an ugly fan"; the shadows are to change fast, as in the tunnels of Hot Pursuit 2010, and lie over each other rightly).
// A shadow is the light of one lamp that does not reach a place. Each place of the road has the light off the walls and the light
// of every lamp near; the car hides some of them from it, and the place is darker by what it lost over what it had:
//     darker by  lost / (walls + all the lamps' light),  lost = the sum of each lamp's light there times how much of it is hidden
// So every lamp near casts its shadow at once, each as strong as its light at that place (how far the lamp is from the place, and
// how the place faces it), where one lamp of three is hidden the place is a little darker, under the car where all are hidden it
// is dark, and a lamp that comes into the set or leaves it has next to no light there then: no shadow jumps. One pass a car does
// it all (native_shadow.frag, lamps): with its four lamps (the most light where the car is) the place's light is all theirs.
// Before (2026-11-25): one shadow from the lamp of the most light, a second one only while they handed over, a pair of lamps across
// the tunnel as one source in their middle; before that each lamp's shadow by its share of the light at the car alone.
static const int kLampsPerCar = 4;
static const int kLampCells = 64;                   // the map's rows for the lamps: 16 x 4 cells of half a car's cell (native_vk.cpp)
static const double kLampAmbient = 0.0165;        // the light off the walls, as a lamp's at 7.8 metres: one lamp 4.5 metres over the
                                                    // road casts a shadow 0.75 dark under the car (the sun's is 90 / 128)
static const double kLampNear = 8.0, kLampFar = 20.0;
static const double kLampSize = 0.4;                // how wide a lamp is, in metres: its shadow's soft edge
static const double kLampPair = 0.0;                // two lamps nearer than this, each the other's nearest, are one: none now, each
                                                    // lamp of a pair casts its own (it was 10.5 till 2026-11-26)
static const double kLampFade = 12.0;               // how far beyond the car the shadow lies before it is gone, metres along the ray
struct TunnelLamp {
    double at[3];
};
static std::vector<TunnelLamp> g_lamps;
struct LampSource {
    double at[3];
    double size;                                    // how wide, metres
};
static std::vector<LampSource> g_sources;           // what casts the shadows: g_lamps, the pairs as one
// The roofs a track draws over its tunnels' road and its ground has not (track_roofs.h, made by tools/recomp/make_track_roofs.py:
// location 4's cave, TRN_SECTION70_CAVERN and _MINE_PROP, over 17% of its tunnel road; the user, 2026-11-25): the set of the
// location whose tunnel lamp the loaded track has. A triangle: its corners, its box across the ground, its plane (a x + b y + c z = d)
struct DrawnRoof {
    float v[9];
    float box[4];
    double plane[4];
};
static std::vector<DrawnRoof> g_drawn_roofs;
static u32 g_lamps_sig = 0, g_lamps_frame = ~0u;

static bool lamps_on()
{
    static const char *env = getenv("HP2_LAMPSHADOWS");
    return shadows_level() >= 2 && !(env && atoi(env) == 0);     // Very High and Ultra (the user, 2026-11-26: Very High too)
}

static bool tunnel_name(u32 at)
{
    char name[33] = {};
    memcpy(name, host(at), 32);
    for (int k = 0; k + 5 <= 32 && name[k]; k++)
        if (!_strnicmp(name + k, "unnel", 5)) return true;
    return false;
}

// the lamps of the packs the game has: read anew when the packs change
static void lamps_read(u32 gp)
{
    if (g_lamps_frame == g_frame_no) return;
    g_lamps_frame = g_frame_no;
    const auto ok = [](u32 a, u32 n) { return a >= 0x100000 && a < 0x2000000 && n < 0x2000000 - a && !(a & 3); };
    const u32 lights = gp - 24264, flares = gp - 24272;
    if (!ok(flares, 16)) return;
    u32 sig = 0x9e3779b9u;
    int guard = 0;
    for (u32 h = LW(lights); h != lights && ok(h, 28) && guard < 64; h = LW(h), guard++) sig = sig * 31 + h + LW(h + 24) * 7;
    guard = 0;
    for (u32 h = LW(flares); h != flares && ok(h, 64) && guard < 64; h = LW(h), guard++) sig = sig * 31 + h + LW(h + 48) * 13;
    if (sig == g_lamps_sig) return;
    g_lamps_sig = sig;
    g_lamps.clear();
    const auto real = [](float v) { return v == v && fabsf(v) < 1.0e6f; };
    guard = 0;
    for (u32 h = LW(flares); h != flares && ok(h, 64) && guard < 64; h = LW(h), guard++) {
        const u32 count = LW(h + 48), list = h + 56;
        u32 node = LW(list);
        for (u32 i = 0; i < count && i < 4096 && node != list && ok(node, 80); i++, node = LW(node)) {
            float p[3];
            memcpy(p, host(node + 48), 12);
            if (real(p[0]) && real(p[1]) && real(p[2]) && tunnel_name(node + 16)) g_lamps.push_back({{p[0], p[1], p[2]}});
        }
    }
    const size_t flared = g_lamps.size();
    guard = 0;
    for (u32 h = LW(lights); !flared && h != lights && ok(h, 28) && guard < 64; h = LW(h), guard++) {
        const u32 at = LW(h + 20), count = LW(h + 24);
        if (count > 8192 || !ok(at, count * 96)) continue;
        for (u32 i = 0; i < count; i++) {
            const u32 r = at + i * 96;
            if (*(const u8 *)host(r + 36) != 2 || !tunnel_name(r)) continue;
            float p[3];
            memcpy(p, host(r + 48), 12);
            if (real(p[0]) && real(p[1]) && real(p[2])) g_lamps.push_back({{p[0], p[1], p[2]}});
        }
    }
    // the sources: a pair across is one, in its middle
    g_sources.clear();
    std::vector<int> nearest(g_lamps.size(), -1);
    std::vector<double> nearest_d(g_lamps.size(), 1e30);
    for (size_t i = 0; i < g_lamps.size(); i++)
        for (size_t j = 0; j < g_lamps.size(); j++) {
            if (j == i) continue;
            const double dx = g_lamps[j].at[0] - g_lamps[i].at[0], dy = g_lamps[j].at[1] - g_lamps[i].at[1], dz = g_lamps[j].at[2] - g_lamps[i].at[2];
            const double e = sqrt(dx * dx + dy * dy + dz * dz);
            if (e < nearest_d[i]) nearest_d[i] = e, nearest[i] = (int)j;
        }
    for (size_t i = 0; i < g_lamps.size(); i++) {
        const int j = nearest[i];
        const bool pair = j >= 0 && nearest[j] == (int)i && nearest_d[i] < kLampPair;
        if (pair && j < (int)i) continue;               // made with the first of the two
        LampSource s;
        s.size = kLampSize;
        for (int k = 0; k < 3; k++) s.at[k] = g_lamps[i].at[k];
        if (pair) {
            for (int k = 0; k < 3; k++) s.at[k] = 0.5 * (g_lamps[i].at[k] + g_lamps[j].at[k]);
            s.size = std::max(kLampSize, nearest_d[i]);
        }
        g_sources.push_back(s);
    }
    // the drawn roofs of this location, if any
    g_drawn_roofs.clear();
    for (int k = 0; k < kTrackRoofSets; k++) {
        bool here = false;
        for (const TunnelLamp &l : g_lamps)
            if (fabs(l.at[0] - kTrackRoofs[k].lamp[0]) < 0.5 && fabs(l.at[1] - kTrackRoofs[k].lamp[1]) < 0.5 && fabs(l.at[2] - kTrackRoofs[k].lamp[2]) < 0.5)
                here = true;
        if (!here) continue;
        for (int t = 0; t < kTrackRoofs[k].triangles; t++) {
            DrawnRoof r;
            memcpy(r.v, kTrackRoofs[k].corners + t * 9, sizeof r.v);
            r.box[0] = std::min(std::min(r.v[0], r.v[3]), r.v[6]), r.box[1] = std::min(std::min(r.v[1], r.v[4]), r.v[7]);
            r.box[2] = std::max(std::max(r.v[0], r.v[3]), r.v[6]), r.box[3] = std::max(std::max(r.v[1], r.v[4]), r.v[7]);
            const double e1[3] = {r.v[3] - (double)r.v[0], r.v[4] - (double)r.v[1], r.v[5] - (double)r.v[2]};
            const double e2[3] = {r.v[6] - (double)r.v[0], r.v[7] - (double)r.v[1], r.v[8] - (double)r.v[2]};
            r.plane[0] = e1[1] * e2[2] - e1[2] * e2[1], r.plane[1] = e1[2] * e2[0] - e1[0] * e2[2], r.plane[2] = e1[0] * e2[1] - e1[1] * e2[0];
            r.plane[3] = r.plane[0] * r.v[0] + r.plane[1] * r.v[1] + r.plane[2] * r.v[2];
            const double l = sqrt(r.plane[0] * r.plane[0] + r.plane[1] * r.plane[1] + r.plane[2] * r.plane[2]);
            if (!(l > 1e-9)) continue;
            for (double &c : r.plane) c /= l;
            g_drawn_roofs.push_back(r);
        }
    }
    if (getenv("HP2_NATIVELOG"))
        fprintf(stderr, "[native] tunnel lamps: %zu (%s), %zu sources, %zu drawn roofs\n", g_lamps.size(), flared ? "the flares" : "the lights",
                g_sources.size(), g_drawn_roofs.size());
}

// What is over the car, for its shadows (Very High and Ultra; the user, 2026-11-25: "the sun's shadow is cast in the tunnels too", "for every
// car, the traffic too"). The game's lighting has no roofs: the sunlight its zones give a car (ELAmbient_..., what the sun's shadow
// goes by) is 0.7 in a tunnel as on the open road. The game's ground has them. Every car keeps the track's collision about it in
// its TopologyCoordinate (the Car's +1312; the Car is the CarRenderInfo's first word): the box it was fetched for at +0 (x, y
// least) and +8 (x, y most; GetElevation fetches anew outside it), and SetInterestBBox (0x151d30) fills it with up to 160 polygons
// from +32, the ground first (flags at the polygon's +10 with neither 2 nor 8; their count at +676), the walls next (2; +680), what
// is overhead last (8, the tunnels' ceilings; +684), all of them +672. A polygon (as GetElevation, 0x152070, reads it): its
// corners' count at +9, its box at +20 (x, y least) and +28 (x, y most), its corners at +48 (x, y, z, w; 16 bytes each), its plane
// at +112 (a, b, c, d: a x + b y + c z = d). A place is in it when no side has it outside: (y1 - y0)(x - x0) - (x1 - x0)(y - y0)
// <= 0 for the sides 0-1, 1-2, 3-0, and 2-3 with four corners; a ceiling goes round the other way, >= 0 (the first build missed
// that and found no roof in any tunnel). The ground under a place is the highest of the ground's polygons there no more than a
// metre above the car (the game's own rule), and more the further the place is from the car, for a road going up (kRoofRise).
// Only read: GetElevation itself keeps the polygon it found last (+688) and fetches the ground anew outside its box, so it is not
// called here for cars the game does not call it for. A car whose polygons do not reach has no ground found and keeps its shadows.
// The tracks' files (2026-11-25, all 30): on every route through a tunnel its ceilings are over all the road under its lamps but on
// location 4's (93%); a route that goes round a tunnel has no road under its lamps at all.
//  - The sun's shadow: a ray from a metre over the ground towards the sun (the ray the car's own shadow is cast along, 0x120d60),
//    and anything of the ground or a ceiling it meets kRoofLeast metres or more up hides the sun. So at a tunnel's mouth the
//    shadow goes on as far in as the sun shines, and under a bridge it is out only where the bridge's own shadow is. Where the ray
//    leaves the car's box before it is as high as the roof straight above, what is past the box is not known: the roof straight
//    above says then.
//  - The lamps' shadows: only under a roof, straight above (the tunnel's), for out under the sky the day lights the road a hundred
//    to a thousand times over what a lamp does (road luminance in sun some 3,000 to 8,000 cd/m2; a tunnel's interior 2 to 10).
//    A tunnel lamp right over a place stands in for a ceiling, only for a car that has no ceilings at all in its box.
// Five places, the car's middle and two metres to either side of it along both its axes, and each car's results eased in over a
// sixth of a second, so a shadow fades as the car goes in and out and takes no steps.
// Before this, the same day: the tunnels' lamps (the sun's shadow out within 6 to 12 metres of their chain; it went out a few
// metres before a mouth, and not under a bridge), and the game's own reverb areas of its SoundScape (0x317c80, 124 bytes each, the
// count at gp - 18084; dropped before a build: half of location 1's tunnels and all of location 2's lie in none, their outlines are
// a few corners over hundreds of metres, some are under the open sky, and location 6's SA02_Tunnel has no triangles at all).
static const double kRoofLeast = 2.5;               // metres above the ground: a car is no taller than that
static const double kRoofMost = 60.0;               // and no roof is further up than this
static const double kRoofLamp = 5.0;               // a tunnel lamp within this across the ground, over the place, is a roof too
static const double kRoofRise = 0.25;              // the ground may be so much higher for each metre from the car (a road uphill)
static const double kRoofEase = 0.1;               // of the way to the new result each frame: a sixth of a second at 60
static struct RoofProbe {
    u32 list = 0, all = 0, ground = 0, walls = 0, over = 0;
    double carx = 0, cary = 0, carz = 0;
    float box[4] = {};                              // what the car's polygons are known for: x, y least, x, y most
    bool valid = false;
    // a polygon's height at x, y, if x, y is in it
    static bool at(u32 p, double x, double y, double &h)
    {
        float box[4], v[16], plane[4];
        memcpy(box, host(p + 20), 16);
        if (x < box[0] || x > box[2] || y < box[1] || y > box[3]) return false;
        memcpy(v, host(p + 48), 64);
        memcpy(plane, host(p + 112), 16);
        const int corners = *(const s8 *)host(p + 9);
        const auto side = [&](int i, int j) {
            return ((double)v[j * 4 + 1] - v[i * 4 + 1]) * (x - v[i * 4]) - ((double)v[j * 4] - v[i * 4]) * (y - v[i * 4 + 1]);
        };
        const double s0 = side(0, 1), s1 = side(1, 2), s2 = side(3, 0), s3 = corners == 4 ? side(2, 3) : s0;
        const double most = std::max(std::max(s0, s1), std::max(s2, s3)), least = std::min(std::min(s0, s1), std::min(s2, s3));
        if (!(most <= 0.0 || least >= 0.0) || !(fabsf(plane[2]) > 0.05f)) return false;
        h = ((double)plane[3] - plane[0] * x - plane[1] * y) / plane[2];
        return h == h && fabs(h) < 1e6;
    }
    u32 polygon(u32 i) const { return LW(list + i * 4); }
    // a drawn roof's height at x, y, if x, y is in it (and the roof is not standing up)
    static bool drawn_at(const DrawnRoof &r, double x, double y, double &h)
    {
        if (x < r.box[0] || x > r.box[2] || y < r.box[1] || y > r.box[3] || !(fabs(r.plane[2]) > 0.05)) return false;
        const float *v = r.v;
        const double d1 = ((double)v[3] - v[0]) * (y - v[1]) - ((double)v[4] - v[1]) * (x - v[0]);
        const double d2 = ((double)v[6] - v[3]) * (y - v[4]) - ((double)v[7] - v[4]) * (x - v[3]);
        const double d3 = ((double)v[0] - v[6]) * (y - v[7]) - ((double)v[1] - v[7]) * (x - v[6]);
        if (!((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0))) return false;
        h = (r.plane[3] - r.plane[0] * x - r.plane[1] * y) / r.plane[2];
        return true;
    }
    bool wall(u32 i) const { return i >= ground && i < ground + walls; }
    // the ground under x, y; false: none known there
    bool floor_at(double x, double y, double &floor) const
    {
        if (!valid) return false;
        const double rise = 1.0 + kRoofRise * sqrt((x - carx) * (x - carx) + (y - cary) * (y - cary));
        floor = -1e30;
        double h;
        for (u32 i = 0; i < ground; i++)
            if (at(polygon(i), x, y, h) && h < carz + rise && h > floor) floor = h;
        return floor > -1e29;
    }
    // how high a roof is straight over x, y (the lowest), above the ground there; 0: none. `lamps`: a tunnel lamp right over it
    // counts (only for a car with no ceilings in its box)
    double roof_at(double x, double y, double floor, bool lamps) const
    {
        double lowest = 0, h;
        for (u32 i = 0; i < all; i++)
            if (!wall(i) && at(polygon(i), x, y, h) && h >= floor + kRoofLeast && h <= floor + kRoofMost && (!lowest || h - floor < lowest))
                lowest = h - floor;
        for (const DrawnRoof &r : g_drawn_roofs)
            if (drawn_at(r, x, y, h) && h >= floor + kRoofLeast && h <= floor + kRoofMost && (!lowest || h - floor < lowest)) lowest = h - floor;
        if (lamps && !over)
            for (const TunnelLamp &l : g_lamps) {
                const double dx = l.at[0] - x, dy = l.at[1] - y, dz = l.at[2] - floor;
                if (dx * dx + dy * dy < kRoofLamp * kRoofLamp && dz >= kRoofLeast && dz <= 12.0 && (!lowest || dz < lowest)) lowest = dz;
            }
        return lowest;
    }
    // 1 or 0: whether the sun is hidden from a metre over the ground at x, y, along `sun` (towards it, unit)
    int sun_hidden(double x, double y, double floor, const double sun[3]) const
    {
        const double o[3] = {x, y, floor + 1.0};
        for (u32 i = 0; i < all; i++) {
            if (wall(i)) continue;
            const u32 p = polygon(i);
            float plane[4];
            memcpy(plane, host(p + 112), 16);
            const double towards = plane[0] * sun[0] + plane[1] * sun[1] + plane[2] * sun[2];
            if (!(fabs(towards) > 1e-6)) continue;
            const double t = (plane[3] - (plane[0] * o[0] + plane[1] * o[1] + plane[2] * o[2])) / towards;
            if (!(t > 0.2)) continue;
            const double hx = o[0] + sun[0] * t, hy = o[1] + sun[1] * t, hz = o[2] + sun[2] * t, up = hz - floor;
            double h;
            if (up >= kRoofLeast && up <= kRoofMost && at(p, hx, hy, h)) return 1;
        }
        for (const DrawnRoof &r : g_drawn_roofs) {
            const double towards = r.plane[0] * sun[0] + r.plane[1] * sun[1] + r.plane[2] * sun[2];
            if (!(fabs(towards) > 1e-6)) continue;
            const double t = (r.plane[3] - (r.plane[0] * o[0] + r.plane[1] * o[1] + r.plane[2] * o[2])) / towards;
            if (!(t > 0.2)) continue;
            const double hx = o[0] + sun[0] * t, hy = o[1] + sun[1] * t, up = o[2] + sun[2] * t - floor;
            if (!(up >= kRoofLeast && up <= kRoofMost) || hx < r.box[0] || hx > r.box[2] || hy < r.box[1] || hy > r.box[3]) continue;
            // the place the ray meets the triangle's plane: in the triangle? (by its own corners, so a wall of the cave counts too)
            const float *v = r.v;
            const double c1[3] = {v[3] - (double)v[0], v[4] - (double)v[1], v[5] - (double)v[2]};
            const double c2[3] = {v[6] - (double)v[0], v[7] - (double)v[1], v[8] - (double)v[2]};
            const double q[3] = {hx - v[0], hy - v[1], o[2] + sun[2] * t - v[2]};
            const double d00 = c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2], d01 = c1[0] * c2[0] + c1[1] * c2[1] + c1[2] * c2[2];
            const double d11 = c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2];
            const double d20 = q[0] * c1[0] + q[1] * c1[1] + q[2] * c1[2], d21 = q[0] * c2[0] + q[1] * c2[1] + q[2] * c2[2];
            const double den = d00 * d11 - d01 * d01;
            if (!(fabs(den) > 1e-12)) continue;
            const double b1 = (d11 * d20 - d01 * d21) / den, b2 = (d00 * d21 - d01 * d20) / den;
            if (b1 >= 0 && b2 >= 0 && b1 + b2 <= 1) return 1;
        }
        // none met: but past the car's box nothing is known. Where the ray leaves it lower than the roof straight above, that says
        const double roof = roof_at(x, y, floor, false);
        if (!roof || !(sun[2] > 1e-3)) return 0;
        const double across = sqrt(sun[0] * sun[0] + sun[1] * sun[1]);
        if (across < 1e-6) return 0;
        double leave = 1e30;                        // how far across the ground to the box's edge
        for (int k = 0; k < 2; k++) {
            if (sun[k] > 1e-9) leave = std::min(leave, (box[2 + k] - o[k]) / sun[k] * across);
            else if (sun[k] < -1e-9) leave = std::min(leave, (box[k] - o[k]) / sun[k] * across);
        }
        const double height = 1.0 + std::max(leave, 0.0) * sun[2] / across;
        return height < roof ? 1 : 0;
    }
} g_probe;

// each car's results, eased (kRoofEase): by the CarRenderInfo
struct RoofEased {
    float lamps, sun;
    u32 frame;
};
static std::unordered_map<u32, RoofEased> g_roof_eased;

// How much the car is under a roof (`lamps`, for the lamps' shadows) and out of the sun (the return, for the sun's), both 0..1.
// `known`: its ground was found (else the lamps' shadows stay as they were).
static float lamps_tunnel(Ctx &c, u32 info, const float pose[16], const double away[3], float &lamps, bool &known)
{
    known = false;
    lamps = 0.0f;
    g_probe.valid = false;
    if (!lamps_on()) return 0.0f;
    lamps_read(c.r[28].w[0]);
    const auto ok = [](u32 a, u32 n) { return a >= 0x100000 && a < 0x2000000 && n < 0x2000000 - a && !(a & 3); };
    if (!ok(info, 4)) return 0.0f;
    const u32 car = LW(info);
    if (!ok(car, 1312 + 692)) return 0.0f;
    const u32 topo = car + 1312;
    const u32 all = LW(topo + 672), ground = LW(topo + 676), walls = LW(topo + 680), over = LW(topo + 684);
    if (all > 160 || ground + walls + over != all || !all) return 0.0f;
    float carpos[4];
    memcpy(carpos, host(car + 96), 16);
    if (!(fabsf(carpos[0]) < 1e6f && fabsf(carpos[1]) < 1e6f && fabsf(carpos[2]) < 1e6f)) return 0.0f;
    const u32 list = topo + 32;
    for (u32 i = 0; i < all; i++)
        if (!ok(LW(list + i * 4), 128)) return 0.0f;
    RoofProbe &g = g_probe;
    g.list = list, g.all = all, g.ground = ground, g.walls = walls, g.over = over;
    g.carx = carpos[0], g.cary = carpos[1], g.carz = carpos[2];
    memcpy(g.box, host(topo), 16);
    g.valid = true;
    const double sun[3] = {-away[0], -away[1], -away[2]};
    // the car's axes across the ground
    double ax[2] = {pose[0], pose[1]}, ay[2] = {pose[4], pose[5]};
    for (double *a : {ax, ay}) {
        const double l = sqrt(a[0] * a[0] + a[1] * a[1]);
        if (l > 1e-3) a[0] /= l, a[1] /= l;
        else a[0] = 0, a[1] = 0;
    }
    const double off[5][2] = {{0, 0}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
    int roofed = 0, hidden = 0, places = 0;
    for (const auto &o : off) {
        const double x = carpos[0] + ax[0] * o[0] + ay[0] * o[1], y = carpos[1] + ax[1] * o[0] + ay[1] * o[1];
        double floor;
        if (!g.floor_at(x, y, floor)) continue;         // no ground known here
        places++;
        roofed += g.roof_at(x, y, floor, true) > 0;
        hidden += g.sun_hidden(x, y, floor, sun);
    }
    if (!places) return 0.0f;
    known = true;
    float now_lamps = (float)roofed / places, now_sun = (float)hidden / places;
    // eased by the car
    auto it = g_roof_eased.find(info);
    if (it == g_roof_eased.end() || g_frame_no - it->second.frame > 30) {
        g_roof_eased[info] = {now_lamps, now_sun, g_frame_no};
    } else if (g_frame_no != it->second.frame) {
        const float k = 1.0f - powf(1.0f - (float)kRoofEase, (float)(g_frame_no - it->second.frame));
        it->second.lamps += (now_lamps - it->second.lamps) * k;
        it->second.sun += (now_sun - it->second.sun) * k;
        it->second.frame = g_frame_no;
        now_lamps = it->second.lamps, now_sun = it->second.sun;
    } else {
        now_lamps = it->second.lamps, now_sun = it->second.sun;
    }
    if (g_frame_no % 600 == 0)
        for (auto e = g_roof_eased.begin(); e != g_roof_eased.end();)
            e = g_frame_no - e->second.frame > 300 ? g_roof_eased.erase(e) : std::next(e);
    lamps = now_lamps;
    return now_sun;
}

// Where a lamp's shadow comes out from under the roof (the user, 2026-11-25: the lamps' shadows are not to stick out of the
// tunnel; out there the day outshines a lamp a hundred times and more). It ends at the roof's edge: a wall standing up from the
// ground across the way from the lamp through the car, at the first place on that way that has no roof (native_shadow.frag fades
// it out over 0.3 metres either side). Only the ceilings and the ground count here, not the lamps standing in for one, and only
// when the car's own place has a roof: else there is no edge to go by. The car's polygons reach some 8 metres about it; an edge
// further than that is not found, and the shadow is as before.
static bool lamp_cut(const double mid[3], const double from[3], double plane[4])
{
    double d[2] = {mid[0] - from[0], mid[1] - from[1]};
    const double l = sqrt(d[0] * d[0] + d[1] * d[1]);
    double floor;
    if (l < 0.5 || !g_probe.floor_at(mid[0], mid[1], floor) || !(g_probe.roof_at(mid[0], mid[1], floor, false) > 0)) return false;
    d[0] /= l, d[1] /= l;
    // -1: no ground known; 0: open; 1: a roof
    const auto test = [&](double t) {
        const double x = mid[0] + d[0] * t, y = mid[1] + d[1] * t;
        double f;
        if (!g_probe.floor_at(x, y, f)) return -1;
        return g_probe.roof_at(x, y, f, false) > 0 ? 1 : 0;
    };
    double roof = 0, open = -1;
    for (double t = 1.0; t <= 14.0; t += 1.0) {
        const int r = test(t);
        if (r == 0) {
            open = t;
            break;
        }
        if (r == 1) roof = t;
    }
    if (open < 0) return false;
    for (int i = 0; i < 4; i++) {                       // the edge to a quarter of a metre
        const double t = 0.5 * (roof + open);
        if (test(t) == 0) open = t;
        else roof = t;
    }
    const double t = 0.5 * (roof + open);
    plane[0] = d[0], plane[1] = d[1], plane[2] = 0;
    plane[3] = d[0] * (mid[0] + d[0] * t) + d[1] * (mid[1] + d[1] * t);
    return true;
}

// The car's lamps (kLampsPerCar at most) and each one's cell: how the lamp sees the ball that holds the car; and the block of the
// frame's lights the shader reads them from (native_shadow.frag, lamps). The block, two NativeLights from s.block on, in quads:
//   0      the count of lamps, the light off the walls (kLampAmbient), how much the car is under a roof, 0
//   1 + 8 i, lamp i:  +0..+3 the world to its cell (u, v 0..1 over w, the third K (z - n) over w = z: the columns)
//                     +4 its cell's number among the lamps' (native_vk.cpp), K, n, the tangent of the cell's half-width
//                     +5 its width (the soft edge), where its shadow has faded out (metres along its ray), how much of it there
//                        is (its scale, below), 0
//                     +6 where it is, 0;  +7 where its shadow ends, the roof's edge (lamp_cut): a x + b y + c z <= d, or
//                        0, 0, 0, 1 for nowhere
static void lamps_pick(Ctx &c, ShadowCar &s, int index)
{
    s.lamps = 0;
    s.block = -1;
    if (!lamps_on() || s.cell < 0 || index >= kLampCells / kLampsPerCar) return;
    lamps_read(c.r[28].w[0]);
    if (g_sources.empty()) return;
    // only under a roof (the user, 2026-11-25: the lamps' shadows are not to come out of the tunnel; a car on the road over a tunnel
    // has its lamps below it and no roof); a car whose ground is not known keeps them
    const double roofed = s.roof_known ? s.roofed : 1.0;
    if (!(roofed > 0.01)) return;
    const double mid[3] = {s.pose[12], s.pose[13], s.pose[14] + 0.5};
    // the ones that give the car the most light where it is, and one more
    int top[kLampsPerCar + 1];
    double light[kLampsPerCar + 1];
    for (int k = 0; k <= kLampsPerCar; k++) top[k] = -1, light[k] = 0;
    for (size_t i = 0; i < g_sources.size(); i++) {
        const double *at = g_sources[i].at;
        const double dx = mid[0] - at[0], dy = mid[1] - at[1], dz = mid[2] - at[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (!(d2 < kLampFar * kLampFar)) continue;
        const double t = std::min(std::max((sqrt(d2) - kLampNear) / (kLampFar - kLampNear), 0.0), 1.0);
        const double l = (1.0 - t * t * (3.0 - 2.0 * t)) / std::max(d2, 9.0);
        for (int k = 0; k <= kLampsPerCar; k++) {
            if (l <= light[k]) continue;
            for (int j = kLampsPerCar; j > k; j--) light[j] = light[j - 1], top[j] = top[j - 1];
            light[k] = l, top[k] = (int)i;
            break;
        }
    }
    for (int k = 0; k < kLampsPerCar; k++) {
        if (top[k] < 0 || !(light[k] > 0)) continue;
        // A lamp is in the set while it is among the nearest, and goes as the next one comes in: so much of it is there as its
        // light here is more than the one left out's. At the swap it is none, so its shadow is gone before it is dropped
        double scale = (light[k] - light[kLampsPerCar]) / light[k];
        const double *from = g_sources[top[k]].at;
        double f[3] = {mid[0] - from[0], mid[1] - from[1], mid[2] - from[2]};
        const double dist = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]), rs = s.radius;
        if (!(dist > rs * 1.15) || !(dist < 200.0)) continue;      // the lamp all but in the car: no shadow that means anything
        // and fading in from there, so the shadow does not jump in or out as the lamp comes that near
        const double near = std::min((dist - rs * 1.15) / (rs * 0.25), 1.0);
        scale *= near * near * (3.0 - 2.0 * near);
        if (!(scale > 0.02)) continue;
        for (double &v : f) v /= dist;
        double r[3] = {f[1], -f[0], 0};                         // across the ray, level
        double rl = sqrt(r[0] * r[0] + r[1] * r[1]);
        if (rl < 1e-3) r[0] = 1, r[1] = 0, rl = 1;
        r[0] /= rl, r[1] /= rl;
        const double up[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        ShadowCar::Lamp &L = s.lamp[s.lamps];
        L.tn = rs / sqrt(dist * dist - rs * rs);
        L.n = dist - rs;
        const double far = dist + rs;
        L.k = far / (far - L.n);
        L.fade = far + kLampFade;
        L.scale = scale;
        L.size = g_sources[top[k]].size;
        for (int j = 0; j < 3; j++) L.at[j] = from[j];
        L.cut[0] = L.cut[1] = L.cut[2] = 0, L.cut[3] = 1;
        if (!lamp_cut(mid, from, L.cut)) L.cut[0] = L.cut[1] = L.cut[2] = 0, L.cut[3] = 1;
        L.cell = index * kLampsPerCar + s.lamps;
        const double sr = r[0] * from[0] + r[1] * from[1] + r[2] * from[2];
        const double su = up[0] * from[0] + up[1] * from[1] + up[2] * from[2];
        const double sf = f[0] * from[0] + f[1] * from[1] + f[2] * from[2];
        double *m = L.clip;
        for (int j = 0; j < 3; j++) {
            m[j * 4 + 0] = r[j] / L.tn;
            m[j * 4 + 1] = up[j] / L.tn;
            m[j * 4 + 2] = L.k * f[j];
            m[j * 4 + 3] = f[j];
        }
        m[12] = -sr / L.tn;
        m[13] = -su / L.tn;
        m[14] = L.k * (-sf - L.n);
        m[15] = -sf;
        s.lamps++;
    }
    if (!s.lamps) return;
    // the block
    const size_t first = g_frame.lights.size();
    g_frame.lights.resize(first + 2);
    float *q = &g_frame.lights[first].q[0][0];
    memset(q, 0, 2 * sizeof(NativeLight));
    q[0] = (float)s.lamps, q[1] = (float)kLampAmbient, q[2] = (float)roofed;
    for (int i = 0; i < s.lamps; i++) {
        const ShadowCar::Lamp &L = s.lamp[i];
        float *o = q + 4 * (1 + 8 * i);
        for (int k = 0; k < 4; k++) {               // the column k of the world to the cell
            o[k * 4 + 0] = (float)(0.5 * L.clip[k * 4 + 0] + 0.5 * L.clip[k * 4 + 3]);
            o[k * 4 + 1] = (float)(0.5 * L.clip[k * 4 + 1] + 0.5 * L.clip[k * 4 + 3]);
            o[k * 4 + 2] = (float)L.clip[k * 4 + 2];
            o[k * 4 + 3] = (float)L.clip[k * 4 + 3];
        }
        o[16] = (float)L.cell, o[17] = (float)L.k, o[18] = (float)L.n, o[19] = (float)L.tn;
        o[20] = (float)L.size, o[21] = (float)L.fade, o[22] = (float)L.scale;
        o[24] = (float)L.at[0], o[25] = (float)L.at[1], o[26] = (float)L.at[2];
        for (int k = 0; k < 4; k++) o[28 + k] = (float)L.cut[k];
    }
    s.block = (int)first;
}

// CarRenderInfo::Render has run for a view (mid_car_end): the car's place among this frame's, and the sun's ray for it
static int shadow_car(Ctx &c, u32 info, const float pose[16], bool moves, const float move[16])
{
    for (size_t i = 0; i < g_shadow_cars.size(); i++)
        if (g_shadow_cars[i].info == info) return (int)i;
    if (g_shadow_cars.size() >= 64) return -1;
    ShadowCar s = {};
    s.info = info;
    memcpy(s.pose, pose, sizeof s.pose);
    s.cell = (int)g_shadow_cars.size() < kShadowCells ? (int)g_shadow_cars.size() : -1;
    s.strength = -1;
    for (int &p : s.patch) p = -1;
    s.moves = moves;
    if (moves) memcpy(s.move, move, sizeof s.move);
    // the ray: the game's own (0x120d60 takes the car's place, leaves the place less the sun's at 0x332920, never steeper... than
    // 4600 of 65536 of a turn above the ground). Its argument goes below the stack, where pc_call's own frame does not reach
    const u32 at = (c.r[29].w[0] - 0x100) & ~15u;
    float place[4] = {pose[12], pose[13], pose[14], 1.0f};
    memcpy(host(at), place, 16);
    g_pc_ctx = &c;
    pc_call(0x120d60, at);
    float ray[3];
    memcpy(ray, host(0x332920), 12);
    double l[3] = {ray[0], ray[1], ray[2]};
    double len = sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!(len > 1e-6) || !(len < 1e12)) l[0] = 0.5, l[1] = 0, l[2] = -1, len = sqrt(1.25);
    for (double &v : l) v /= len;
    if (l[2] > 0) l[0] = -l[0], l[1] = -l[1], l[2] = -l[2];             // away from the sun is down
    // across the ray: right is level, up is the third
    double right[3] = {-l[1], l[0], 0};                                 // the ray by the world's up
    double rl = sqrt(right[0] * right[0] + right[1] * right[1]);
    if (rl < 1e-6) right[0] = 1, right[1] = 0, rl = 1;
    right[0] /= rl, right[1] /= rl;
    const double up[3] = {right[1] * l[2] - right[2] * l[1], right[2] * l[0] - right[0] * l[2], right[0] * l[1] - right[1] * l[0]};
    const double mid[3] = {pose[12], pose[13], pose[14] + 0.5};
    // The cell's size by the car's own: the points of its collision box, which the game's own shadow is made of (Render, 0x121e30
    // on: the Car is the CarRenderInfo's first word, its body at +52, the body's box at +8; the box has its count of points at +16
    // and the points at +32, 20 bytes each with x, y, z at +4 in the car's own space). Never smaller than it was for every car.
    double reach = kShadowReach, deep = kShadowDeep;
    double car_across = 2.5, car_tall = 1.0;        // a car's, when its box cannot be read
    {
        const auto in_game = [](u32 a) { return a >= 0x100000 && a < 0x2000000 - 64 && !(a & 3); };
        const u32 car = in_game(info) ? LW(info) : 0, body = in_game(car) ? LW(car + 52) : 0, box = in_game(body) ? LW(body + 8) : 0;
        const s32 count = in_game(box) ? (s16)LH(box + 16) : 0;
        const u32 points = in_game(box) ? LW(box + 32) : 0;
        if (count >= 4 && count <= 64 && in_game(points) && in_game(points + (u32)count * 20)) {
            double across = 0, tall = 0;
            bool sane = true;
            for (s32 i = 0; i < count; i++) {
                float at[3];
                memcpy(at, host(points + (u32)i * 20 + 4), 12);
                if (!(fabsf(at[0]) < 40.0f && fabsf(at[1]) < 40.0f && fabsf(at[2]) < 40.0f)) sane = false;
                across = std::max(across, sqrt((double)at[0] * at[0] + (double)at[1] * at[1]));
                tall = std::max(tall, fabs((double)at[2] - 0.5));
            }
            if (sane) {
                car_across = across, car_tall = tall;
                reach = std::min(std::max(reach, across + tall + 0.3), 16.0);
                deep = std::min(std::max(deep, sqrt(across * across + tall * tall) + 1.2), 18.0);
            }
        }
    }
    s.reach = reach, s.deep = deep;
    s.radius = std::min(sqrt(car_across * car_across + car_tall * car_tall) + 0.3, 12.0);
    double *m = s.light;
    for (int k = 0; k < 3; k++) {
        m[k * 4 + 0] = right[k] / reach;
        m[k * 4 + 1] = up[k] / reach;
        m[k * 4 + 2] = l[k] / (2 * deep);
        m[k * 4 + 3] = 0;
    }
    m[12] = -(mid[0] * right[0] + mid[1] * right[1] + mid[2] * right[2]) / reach;
    m[13] = -(mid[0] * up[0] + mid[1] * up[1] + mid[2] * up[2]) / reach;
    m[14] = (-(mid[0] * l[0] + mid[1] * l[1] + mid[2] * l[2]) + deep) / (2 * deep);
    m[15] = 1;
    s.tunnel = lamps_tunnel(c, info, pose, l, s.roofed, s.roof_known);
    lamps_pick(c, s, (int)g_shadow_cars.size());
    g_shadow_cars.push_back(s);
    return (int)g_shadow_cars.size() - 1;
}

// A shadow's matrices: `space` takes the world to the shadow's own (u, v, the third), and the picture's (x, y, depth, 1) goes to
// the world by the view's matrix inverted. For the picture between two frames the view is the one halfway and the car is where it
// was halfway (as its draws are: the mean of the matrices).
static bool shadow_matrices(NativeShadow &out, const ShadowCar &s, const double space[16], u32 id, ShadowFrom &from)
{
    const int half_w = LW(g_target + 0x1c) / 2, half_h = LW(g_target + 0x20) / 2;
    const float z_scale = depth_scale(g_target);
    if (half_w <= 0 || half_h <= 0) return false;
    clip_now();
    double view[16], back[16], to[16], space_back[16];
    clip_matrix_d(view, g_now_p, g_now_q, nullptr, half_w, half_h, z_scale);
    if (!invert4d(view, back) || !invert4d(space, space_back)) return false;
    mul_d(to, space, back);
    for (int i = 0; i < 16; i++) out.to[i] = (float)to[i];
    mul_d(from.now, view, space_back);
    from.has_mid = false;
    out.has_mid = false;
    // how large the car is in the view: its length over the view's width, where it stands
    const double w = view[3] * s.pose[12] + view[7] * s.pose[13] + view[11] * s.pose[14] + view[15];
    const double across = sqrt(view[0] * view[0] + view[4] * view[4] + view[8] * view[8]);
    out.size = w > 1e-6 ? (float)(4.5 * across / w * 0.5) : 1.0f;
    out.view[0] = (float)(s32)LW(g_target + 0x14), out.view[1] = (float)(s32)LW(g_target + 0x18);
    out.view[2] = (float)(s32)LW(g_target + 0x1c), out.view[3] = (float)(s32)LW(g_target + 0x20);
    out.target = (u8)id;
    if (!g_double_now) return true;
    u32 pack = 0;
    for (const auto &v : g_view_pack)
        if (v.target == g_target_ptr) pack = v.pack;
    const PackSeen *seen = pack ? pack_seen_known(id, pack) : nullptr;
    if (!seen || !seen->has_prev) return true;
    double before[16], halfway[16], car[16], car_back[16], t1[16];
    clip_matrix_d(before, seen->prev_p, seen->prev_q, nullptr, half_w, half_h, z_scale);
    for (int i = 0; i < 16; i++) halfway[i] = 0.5 * (before[i] + view[i]);
    if (!invert4d(halfway, back)) return true;
    for (int i = 0; i < 16; i++) car[i] = (i % 5 == 0 ? 0.5 : 0.0) + (s.moves ? 0.5 * s.move[i] : (i % 5 == 0 ? 0.5 : 0.0));
    if (!invert4d(car, car_back)) return true;
    mul_d(t1, space, car_back);
    mul_d(to, t1, back);
    for (int i = 0; i < 16; i++) out.to_mid[i] = (float)to[i];
    out.has_mid = true;
    mul_d(t1, halfway, car);
    mul_d(from.mid, t1, space_back);
    from.has_mid = true;
    return true;
}

// the players' views: the shadows go over the picture here, at the first of the game's own
static void shadow_fx(u32 id)
{
    // The mirror: where its first shadow polygon stood among its draws. Its shadows went over all the rest of it, the smoke and
    // the sparks too (the user, 2026-11-17: the shadow shows above the effects in the mirror, which makes no sense); now they go
    // where the game's own lie, under what is transparent and comes after (native_vk.cpp, draw_env_views).
    if (id == 7 && g_shadow_rvm_at == ~0u) g_shadow_rvm_at = (u32)g_frame.draws.size();
    if (id > 2 || g_shadow_fx[id]) return;
    g_shadow_fx[id] = true;
    NativeFx &f = add_fx(FX_SHADOW, g_target);
    f.target = (u8)id;
}

// this view's shadow of the car from the sun, once
static void shadow_sun(ShadowCar &s, u32 id)
{
    if ((s.made >> id) & 1 || s.cell < 0 || s.strength < 0) return;
    s.made |= 1u << id;
    if (s.strength <= 0) return;
    double space[16];
    for (int k = 0; k < 4; k++) {
        space[k * 4 + 0] = 0.5 * s.light[k * 4 + 0] + 0.5 * s.light[k * 4 + 3];
        space[k * 4 + 1] = 0.5 * s.light[k * 4 + 1] + 0.5 * s.light[k * 4 + 3];
        space[k * 4 + 2] = s.light[k * 4 + 2];
        space[k * 4 + 3] = s.light[k * 4 + 3];
    }
    NativeShadow n = {};
    n.kind = 1;
    n.cell = (u16)s.cell;
    n.colour[3] = s.strength;
    const double softer = std::min(std::max(g_cfg.shadow_softness, 25), 200) * 0.01;
    n.soft = (float)(kShadowSoftMost * softer / (2.0 * s.reach));
    n.low = (float)(kShadowSoftLeast / (2.0 * s.reach));
    n.high = (float)(kShadowSoftRise * softer * s.deep / s.reach);      // a metre along the ray is 1 / (2 deep) of the third
    n.metres[0] = (float)(2.0 * s.reach), n.metres[1] = (float)(2.0 * s.deep);   // the cell in metres (native_shadow.frag: what faces the sun)
    ShadowFrom from;
    if (!shadow_matrices(n, s, space, id, from)) {
        s.made &= ~(1u << id);
        return;
    }
    g_frame.shadows.push_back(n);
    g_shadow_from.push_back(from);
}

static bool shadow_box(const double from[16], double z0, double z1, double box[4]);

// this view's shadow of the car's tunnel lamps, once: the players' views and the mirror. Its `to` takes the picture to the world;
// where it can fall is worked out here: the car and kLampFade metres about it, from 3 metres under it to 4 over
static void shadow_lamps(ShadowCar &s, u32 id)
{
    if (s.block < 0 || (id != 1 && id != 2 && id != 7) || (s.lamp_made >> id) & 1) return;
    s.lamp_made |= 1u << id;
    static const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    NativeShadow n = {};
    n.kind = 3;
    n.block = (u32)s.block * 22;
    ShadowFrom from;
    if (!shadow_matrices(n, s, identity, id, from)) return;
    const double r = s.radius + kLampFade, lo[3] = {s.pose[12] - r, s.pose[13] - r, s.pose[14] - 3.0};
    double cube[16] = {2 * r, 0, 0, 0, 0, 2 * r, 0, 0, 0, 0, 7.0, 0, lo[0], lo[1], lo[2], 1};       // the unit cube to that box
    double now_m[16], mid_m[16], now[4], mid[4];
    n.box[0] = n.box[1] = -1, n.box[2] = n.box[3] = 1;
    mul_d(now_m, from.now, cube);
    if (shadow_box(now_m, 0.0, 1.0, now)) {
        bool good = true;
        if (from.has_mid) {
            mul_d(mid_m, from.mid, cube);
            good = shadow_box(mid_m, 0.0, 1.0, mid);
            for (int k = 0; k < 4 && good; k++) {
                const double before = 2 * mid[k] - now[k];
                now[k] = k < 2 ? std::min(now[k], std::min(mid[k], before)) : std::max(now[k], std::max(mid[k], before));
            }
        }
        if (good) {
            const double mx = 0.02 + 0.05 * (now[2] - now[0]), my = 0.02 + 0.05 * (now[3] - now[1]);
            n.box[0] = (float)std::max(now[0] - mx, -1.0), n.box[1] = (float)std::max(now[1] - my, -1.0);
            n.box[2] = (float)std::min(now[2] + mx, 1.0), n.box[3] = (float)std::min(now[3] + my, 1.0);
        }
    }
    g_frame.shadows.push_back(n);
    g_shadow_from.push_back(from);
}

// a draw has just been made (add_strip): a car's opaque one goes into the car's cell of the map, and the car's shadow from the sun
// into this view
static void shadow_caster()
{
    if (!g_owner || g_owner->car < 0 || g_node_poly || (size_t)g_owner->car >= g_shadow_cars.size()) return;
    const u32 id = g_draw_target;
    if (id != 1 && id != 2 && id != 7) return;
    ShadowCar &s = g_shadow_cars[g_owner->car];
    if (s.strength < 0 && g_light_ctx) {            // the car's light context +212: the sunlight where it is (0x121fd8 on)
        float sunlight;
        memcpy(&sunlight, host(g_light_ctx + 212), 4);
        const float amount = !(sunlight >= 0.4f) ? 0.0f : sunlight <= 1.0f ? (sunlight - 0.4f) / 0.6f : 1.0f;
        s.strength = 90.0f * amount / 128.0f * (1.0f - s.tunnel);      // none in a tunnel (Very High and Ultra: lamps_tunnel)
    }
    shadow_sun(s, id);
    shadow_lamps(s, id);
    const NativeDraw &d = g_frame.draws.back();
    // Glass (2026-11-17, the user: the shadow could take the light through the glass into account). What blends over the picture
    // by its alpha is the car's glass: it casts too, into a half of the map of its own, and where a ray meets nothing of the car
    // but glass the ground has a faint shadow, the glass's tint (native_shadow.frag). Where a ray meets the body as well, the
    // body's shadow is all there is, as before.
    const bool glass = d.blend == NB_ALPHA;
    if (s.cell < 0 || (d.blend != NB_OPAQUE && !glass)) return;
    g_caster_of.push_back({(u16)g_owner->car, (u8)id});      // (the draw has no vertices yet: counted at the frame's end)
    NativeShadowCaster k = {};
    k.draw = (u32)g_frame.draws.size() - 1;
    k.cell = (u16)s.cell;
    k.glass = glass ? 1 : 0;
    double local[16], clip[16];
    for (int i = 0; i < 16; i++) local[i] = g_now_l[i];
    mul_d(clip, s.light, local);
    for (int i = 0; i < 16; i++) k.clip[i] = (float)clip[i];
    g_frame.casters.push_back(k);
    // and into the cells of its lamps (the body alone: no glass in them)
    if (glass) return;
    for (int i = 0; i < s.lamps; i++) {
        g_caster_of.push_back({(u16)g_owner->car, (u8)id});
        NativeShadowCaster q = {};
        q.draw = k.draw;
        q.cell = (u16)(kShadowCells + s.lamp[i].cell);
        q.glass = 0;
        mul_d(clip, s.lamp[i].clip, local);
        for (int j = 0; j < 16; j++) q.clip[j] = (float)clip[j];
        g_frame.casters.push_back(q);
    }
}

// A test on the user's word (2026-11-26: "take away the static shadow's texture, to see what is there" under the car, with the
// Ambient Occlusion on): the patch under a car (<car>_SHADOWIG) is left out of the race's views at every Shadows level, neither the
// game's polygon nor the port's own. The sun's shadow stays. Back to false when the user says so (he did, the same day: the patch is
// back; AO had given no shade under the car, so it was taken off round the cars and kept on their bodies, ПОРА111).
static const bool kNoPatchTest = false;

// A POLY3D node of the lists: one of a car's two shadows? True: the port draws it its own way, the polygon is left out.
static bool shadow_poly(u32 node)
{
    if (kNoPatchTest && g_texture && g_target && pc_racing()) {
        const u32 view = LW(g_target);
        const char *name = (const char *)host(g_texture + 8);
        const size_t len = strnlen(name, 40);
        if ((view == 1 || view == 2 || view == 7) && len > 9 && len < 40 && !strcmp(name + len - 9, "_SHADOWIG")) return true;
    }
    if (!shadows_level() || !g_texture || !g_target || !g_pack || !g_owner || g_owner->car < 0 || (size_t)g_owner->car >= g_shadow_cars.size()) return false;
    const u32 id = LW(g_target);
    if (id != 1 && id != 2 && id != 7) return false;
    const char *name = (const char *)host(g_texture + 8);
    const size_t len = strnlen(name, 40);
    const bool ramp = !strcmp(name, "SHADOWRAMP");
    const bool patch = len > 9 && len < 40 && !strcmp(name + len - 9, "_SHADOWIG");
    if (!ramp && !patch) return false;
    ShadowCar &s = g_shadow_cars[g_owner->car];
    if (ramp) {
        if (!((s.made >> id) & 1)) return false;    // no outline of this car in this view: the game's own
        shadow_fx(id);
        return true;
    }
    // the patch: the polygon's corners say where the texture lies on the ground (u and v go straight with x and y) and how high
    float xs[64], ys[64], zs[64], us[64], vs[64];
    u32 n = 0, rgba = 0x80808080;
    const u32 strips = LH(node + 8);
    for (u32 i = 0; i < strips && n < 64; i++) {
        static thread_local u32 vu[VU_QUADS][4];
        memset(vu, 0, sizeof vu);
        if (!unpack_strip(LW(node + 12 + i * 8), LH(node + 10 + i * 8), vu)) continue;
        const u32 count = vu[0][0] & 0xff;
        if (count < 3 || count > 30) continue;
        if (!n) rgba = (vu[52][0] & 255) | (vu[52][1] & 255) << 8 | (vu[52][2] & 255) << 16 | (vu[52][3] & 255) << 24;
        for (u32 v = 0; v < count && n < 64; v++, n++) {
            const u32 g = 2 + 3 * (v / 4), l = v % 4;
            memcpy(&xs[n], &vu[g][l], 4);
            memcpy(&ys[n], &vu[g + 1][l], 4);
            memcpy(&zs[n], &vu[g + 2][l], 4);
            memcpy(&us[n], &vu[32 + (v >> 1)][v & 1 ? 3 : 0], 4);
            memcpy(&vs[n], &vu[32 + (v >> 1)][v & 1 ? 2 : 1], 4);
        }
    }
    if (n < 3) return false;
    // More of the same patch (its grid comes as several polygons), or its first. The heights are taken from the plane through all
    // the corners read so far, not from a level (2026-11-17): by a level the patch of a car on a slope was a slab as thick as
    // the slope under the car is high, and lay on the car's own sills and bumper at its lower end. The plane is fitted over the
    // whole grid, since each corner was dropped onto the terrain by itself and one on a kerb would tilt a plane through three.
    const bool more = s.patch[id] >= 0 && (size_t)s.patch[id] < g_frame.shadows.size() && g_shadow_from.size() == g_frame.shadows.size();
    if (more && g_patch.shadow != s.patch[id]) {    // not the one being read: it stays as it is
        shadow_fx(id);
        return true;
    }
    if (!more) {
        // where the texture lies on the ground: u and v go straight with x and y. Three corners not in a line: the first, the
        // furthest from it, the furthest from the line through those two
        u32 b = 1, c3 = 2;
        double far = -1;
        for (u32 i = 1; i < n; i++) {
            const double dx = xs[i] - xs[0], dy = ys[i] - ys[0];
            if (dx * dx + dy * dy > far) far = dx * dx + dy * dy, b = i;
        }
        far = -1;
        for (u32 i = 1; i < n; i++) {
            const double cross = fabs((double)(xs[b] - xs[0]) * (ys[i] - ys[0]) - (double)(xs[i] - xs[0]) * (ys[b] - ys[0]));
            if (cross > far) far = cross, c3 = i;
        }
        const double x1 = xs[b] - xs[0], y1 = ys[b] - ys[0], x2 = xs[c3] - xs[0], y2 = ys[c3] - ys[0];
        const double det = x1 * y2 - x2 * y1;
        if (fabs(det) < 1e-4) return false;
        const double ua = ((us[b] - us[0]) * y2 - (us[c3] - us[0]) * y1) / det, ub = (x1 * (us[c3] - us[0]) - x2 * (us[b] - us[0])) / det;
        const double va = ((vs[b] - vs[0]) * y2 - (vs[c3] - vs[0]) * y1) / det, vb = (x1 * (vs[c3] - vs[0]) - x2 * (vs[b] - vs[0])) / det;
        g_patch.shadow = -1;
        g_patch.n = 0;
        g_patch.map[0] = ua, g_patch.map[1] = ub, g_patch.map[2] = us[0] - ua * xs[0] - ub * ys[0];
        g_patch.map[3] = va, g_patch.map[4] = vb, g_patch.map[5] = vs[0] - va * xs[0] - vb * ys[0];
    }
    for (u32 i = 0; i < n && g_patch.n < 256; i++, g_patch.n++)
        g_patch.x[g_patch.n] = xs[i], g_patch.y[g_patch.n] = ys[i], g_patch.z[g_patch.n] = zs[i];
    // the plane: z = cz + pa (x - cx) + pb (y - cy), by least squares about the corners' middle
    const u32 all = g_patch.n;
    double cx = 0, cy = 0, cz = 0;
    for (u32 i = 0; i < all; i++) cx += g_patch.x[i], cy += g_patch.y[i], cz += g_patch.z[i];
    cx /= all, cy /= all, cz /= all;
    double sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
    for (u32 i = 0; i < all; i++) {
        const double dx = g_patch.x[i] - cx, dy = g_patch.y[i] - cy, dz = g_patch.z[i] - cz;
        sxx += dx * dx, sxy += dx * dy, syy += dy * dy, sxz += dx * dz, syz += dy * dz;
    }
    double pa = 0, pb = 0;
    const double pdet = sxx * syy - sxy * sxy;
    if (pdet > 1e-6 * (sxx + syy) * (sxx + syy) && pdet > 1e-9) {
        pa = (sxz * syy - syz * sxy) / pdet;
        pb = (syz * sxx - sxz * sxy) / pdet;
        if (!(fabs(pa) < 1.0 && fabs(pb) < 1.0)) pa = pb = 0;       // steeper than any road: a level through the middle
    }
    double low = 0, high = 0;               // the corners themselves about the plane
    for (u32 i = 0; i < all; i++) {
        const double r = g_patch.z[i] - (cz + pa * (g_patch.x[i] - cx) + pb * (g_patch.y[i] - cy));
        low = std::min(low, r), high = std::max(high, r);
    }
    double space[16] = {};
    space[0] = g_patch.map[0], space[4] = g_patch.map[1], space[12] = g_patch.map[2];
    space[1] = g_patch.map[3], space[5] = g_patch.map[4], space[13] = g_patch.map[5];
    space[2] = -pa, space[6] = -pb, space[10] = 1, space[14] = -(cz - pa * cx - pb * cy);
    space[15] = 1;
    NativeShadow p = {};
    if (more) {
        p = g_frame.shadows[s.patch[id]];
    } else {
        p.kind = 0;
        for (int k = 0; k < 4; k++) p.colour[k] = (float)(rgba >> (k * 8) & 255) / 128.0f;
        p.tex = native_texture(g_texture, g_frame_no);
        if (!p.tex) return false;
    }
    p.base = (float)cz;
    p.low = (float)low - 1.0f;
    p.high = (float)high + 0.10f;
    // the patch's size on the ground, for the shader to tell the ground from a wall by (native_shadow.frag, flatness)
    {
        const double lu = sqrt(g_patch.map[0] * g_patch.map[0] + g_patch.map[1] * g_patch.map[1]);
        const double lv = sqrt(g_patch.map[3] * g_patch.map[3] + g_patch.map[4] * g_patch.map[4]);
        p.metres[0] = lu > 1e-6 ? (float)(1.0 / lu) : 0.0f;
        p.metres[1] = lv > 1e-6 ? (float)(1.0 / lv) : 0.0f;
    }
    ShadowFrom from;
    if (!shadow_matrices(p, s, space, id, from)) {
        if (!more) return false;
        shadow_fx(id);                      // the patch stays as its polygons before this one made it
        return true;
    }
    if (more) {
        g_frame.shadows[s.patch[id]] = p;
        g_shadow_from[s.patch[id]] = from;
    } else {
        s.patch[id] = (int)g_frame.shadows.size();
        g_patch.shadow = s.patch[id];
        g_frame.shadows.push_back(p);
        g_shadow_from.push_back(from);
    }
    shadow_fx(id);
    return true;
}

// Where in its view a shadow can fall at all: the corners of its own space (u and v 0..1, the third from z0 to z1) on the picture,
// left, top, right, bottom in the view's -1..1. False: a corner is behind the camera, and it may be anywhere.
static bool shadow_box(const double from[16], double z0, double z1, double box[4])
{
    box[0] = box[1] = 1e30, box[2] = box[3] = -1e30;
    for (int k = 0; k < 8; k++) {
        const double u = k & 1 ? 1.0 : 0.0, v = k & 2 ? 1.0 : 0.0, z = k & 4 ? z1 : z0;
        const double x = from[0] * u + from[4] * v + from[8] * z + from[12], y = from[1] * u + from[5] * v + from[9] * z + from[13];
        const double w = from[3] * u + from[7] * v + from[11] * z + from[15];
        if (!(w > 0)) return false;
        box[0] = std::min(box[0], x / w), box[2] = std::max(box[2], x / w);
        box[1] = std::min(box[1], y / w), box[3] = std::max(box[3], y / w);
    }
    return true;
}

// The frame is all read. A player's view that has shadows and none of the game's polygons to say where they go (every car far off)
// gets them after the last of its draws.
static void shadow_end()
{
    g_frame.shadow_level = (u8)shadows_level();
    // each car into the map by one view's draws: the view with the most of it (g_caster_of); a player's view before the mirror
    if (g_caster_of.size() == g_frame.casters.size()) {
        // A draw is written down as it begins and gets its vertices after: they are counted here, with the frame all read.
        // (Counted at the draw's start they were all 0: no view had "the most", every car's draws were dropped and no car
        // had a shadow from the sun at all - the user, 2026-11-23, after the first version of this.)
        for (ShadowCar &car : g_shadow_cars) memset(car.cast_verts, 0, sizeof car.cast_verts);
        for (size_t i = 0; i < g_frame.casters.size(); i++) {
            const CasterOf &of = g_caster_of[i];
            const u32 draw = g_frame.casters[i].draw;
            if (of.car < g_shadow_cars.size() && of.target < 16 && draw < g_frame.draws.size())
                g_shadow_cars[of.car].cast_verts[of.target] += g_frame.draws[draw].count;
        }
        size_t kept = 0;
        for (size_t i = 0; i < g_frame.casters.size(); i++) {
            const CasterOf &of = g_caster_of[i];
            if (of.car >= g_shadow_cars.size()) continue;
            const ShadowCar &s = g_shadow_cars[of.car];
            u32 best = 0;
            for (u32 id : {1u, 2u, 7u})
                if (s.cast_verts[id] > (best ? s.cast_verts[best] : 0u)) best = id;
            if (of.target == best || !best) g_frame.casters[kept++] = g_frame.casters[i];
        }
        g_frame.casters.resize(kept);
    }
    // the mirror's shadows: before the draw its first shadow polygon stood at; with no such polygon, before the first of its draws
    // that blends and writes no depth (the smoke, the flares, the glass); with none of those, after all of it
    g_frame.rvm_shadow_at = g_shadow_rvm_at;
    if (g_frame.rvm_shadow_at == ~0u) {
        g_frame.rvm_shadow_at = (u32)g_frame.draws.size();
        for (size_t i = 0; i < g_frame.draws.size(); i++) {
            const NativeDraw &d = g_frame.draws[i];
            if (d.target == 7 && d.count && d.blend != NB_OPAQUE && !d.depth_write) {
                g_frame.rvm_shadow_at = (u32)i;
                break;
            }
        }
    }
    if (g_frame.shadows.empty()) return;
    // Each shadow went over its whole view, pixel by pixel, to find the few it falls on: with a dozen cars that is two dozen
    // pictures' worth of pixels (2026-11-17). Its own space is a box about the car, and the box's corners on the picture say where
    // to look. For the pictures between two frames: the box halfway too, and as far again before that (a picture rate that is
    // no multiple of 60 has pictures earlier than halfway, shown by going on past it: native_vk.cpp, draw_shadows).
    const bool known = g_shadow_from.size() == g_frame.shadows.size();
    for (size_t i = 0; i < g_frame.shadows.size(); i++) {
        NativeShadow &s = g_frame.shadows[i];
        if (s.kind == 3) continue;                  // the lamps': shadow_lamps has it
        s.box[0] = s.box[1] = -1, s.box[2] = s.box[3] = 1;
        if (!known) continue;
        const ShadowFrom &f = g_shadow_from[i];
        const double z0 = s.kind == 1 ? 0.0 : (double)s.low, z1 = s.kind == 1 ? 1.75 : (double)s.high;
        double now[4], mid[4];
        if (!shadow_box(f.now, z0, z1, now)) continue;
        if (f.has_mid) {
            if (!shadow_box(f.mid, z0, z1, mid)) continue;
            for (int k = 0; k < 4; k++) {
                const double before = 2 * mid[k] - now[k];
                now[k] = k < 2 ? std::min(now[k], std::min(mid[k], before)) : std::max(now[k], std::max(mid[k], before));
            }
        }
        const double mx = 0.02 + 0.05 * (now[2] - now[0]), my = 0.02 + 0.05 * (now[3] - now[1]);
        s.box[0] = (float)std::max(now[0] - mx, -1.0), s.box[1] = (float)std::max(now[1] - my, -1.0);
        s.box[2] = (float)std::min(now[2] + mx, 1.0), s.box[3] = (float)std::min(now[3] + my, 1.0);
    }
    for (u32 id = 1; id <= 2; id++) {
        if (g_shadow_fx[id]) continue;
        bool any = false;
        for (const NativeShadow &s : g_frame.shadows) any |= s.target == id;
        if (!any) continue;
        u32 at = 0;
        for (size_t i = 0; i < g_frame.draws.size(); i++)
            if (g_frame.draws[i].target == id && g_frame.draws[i].count) at = (u32)i + 1;
        if (!at) continue;
        NativeFx f = {};
        f.at = at;
        f.kind = FX_SHADOW;
        f.target = (u8)id;
        size_t where = 0;
        while (where < g_frame.fx.size() && g_frame.fx[where].at <= at) where++;
        g_frame.fx.insert(g_frame.fx.begin() + where, f);
    }
}

// ---- Ambient Occlusion (Options > Graphics > Advanced; the user, 2026-11-17: once more, on and off, done the better way) -------
// A race's views are shaded where surfaces meet, from the picture's own depth (native_ao.frag). It goes over the picture after the
// last of a view's draws that are solid, before all that is transparent and before the cars' shadows: the smoke, the glass and
// the shadows' edges are drawn over it, untouched by it. The first one (removed 2026-11-14) went over the finished picture.
// Cars shade nothing round them (the user, 2026-11-26, code word ПОРА111: "a big halo round the car"; their bodies keep their
// shade): at a view's first draw of a car's (NativeDraw::car) its depth is copied, FX_AO_WORLD, and the road and the walls are
// shaded by that, a world with no car in it; a car's own pixels by the whole depth (native_ao.frag).
static void ao_end()
{
    if (!ao_on()) return;
    for (u32 id = 1; id <= 2; id++) {
        u32 at = 0;
        for (size_t i = 0; i < g_frame.draws.size(); i++) {
            const NativeDraw &d = g_frame.draws[i];
            if (d.target == id && d.count && d.depth_write && d.blend == NB_OPAQUE) at = (u32)i + 1;
        }
        if (!at) continue;
        for (const NativeFx &other : g_frame.fx)    // never over the shadows, wherever a solid draw may come in the lists
            if (other.kind == FX_SHADOW && other.target == id && other.at < at) at = other.at;
        u32 target = 0, pack = 0;
        for (const auto &v : g_view_pack)
            if (v.target && v.pack && LW(v.target) == id) target = v.target, pack = v.pack;
        if (!target) continue;
        const int half_w = LW(target + 0x1c) / 2, half_h = LW(target + 0x20) / 2;
        if (half_w <= 0 || half_h <= 0) continue;
        float p[16], q[8];
        read_quads(pack, p, 4);
        read_quads(pack + 64, q, 2);
        double view[16], back[16];
        clip_matrix_d(view, p, q, nullptr, half_w, half_h, depth_scale(target));
        if (!invert4d(view, back)) continue;
        // the camera: where the picture's rays meet, which the view takes to (0, 0, z, 0). The world is taken about it: what the
        // shader works with is then metres from the camera, not places thousands of metres from the track's corner.
        if (!(fabs(back[11]) > 1e-30)) continue;
        const double eye[3] = {back[8] / back[11], back[9] / back[11], back[10] / back[11]};
        const double shift[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eye[0], -eye[1], -eye[2], 1};
        double to[16];
        mul_d(to, shift, back);
        NativeFx f = {};
        f.at = at;
        f.kind = FX_AO;
        f.target = (u8)id;
        f.rect[0] = (float)(s32)LW(target + 0x14);
        f.rect[1] = (float)(s32)LW(target + 0x18);
        f.rect[2] = (float)(s32)LW(target + 0x1c);
        f.rect[3] = (float)(s32)LW(target + 0x20);
        for (int i = 0; i < 16; i++) f.m[i] = (float)to[i];
        // the world's depth, before the view's first car (a world draw after it counts as a car's: it is shaded, it shades nothing)
        for (u32 i = 0; i < at; i++) {
            const NativeDraw &d = g_frame.draws[i];
            if (d.target != id || !d.count || !d.car) continue;
            NativeFx w = {};
            w.at = i;
            w.kind = FX_AO_WORLD;
            w.target = (u8)id;
            memcpy(w.rect, f.rect, sizeof w.rect);
            size_t before = 0;
            while (before < g_frame.fx.size() && g_frame.fx[before].at < i) before++;
            g_frame.fx.insert(g_frame.fx.begin() + before, w);
            f.world = true;
            break;
        }
        size_t where = 0;                   // before what else is laid over the picture at that draw: the shadows, the fog
        while (where < g_frame.fx.size() && g_frame.fx[where].at < at) where++;
        g_frame.fx.insert(g_frame.fx.begin() + where, f);
    }
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
//
// When, 2026-11-26. On the PS2 FetchVRAM came from the render stage queue's interrupt, once the GS had drawn the frame, and the game
// was at the next one meanwhile. Here it comes at the hand-over, and it waited there for the renderer's thread and then for the
// GPU to finish the whole frame: the game and the card took turns, a frame's time was the one's and the other's added. But the words
// are read by one function only, StuffSun (0x1252b0: it counts the sky's pixels of its view's 32 x 32 at 0x2e03d0 + 4112 a view,
// then asks for the next rects; nothing else addresses that buffer), at the start of the player's view of the NEXT frame. So
// FetchVRAM now only notes which rect goes where, and the words are fetched when StuffSun is about to read them (HOOK_StuffSun),
// or at the latest before the next hand-over (native_end_frame: after it the renderer would put the next frame's rects in their
// place). The same rects of the same frame into the same words as before, only later: the world's step, the culling, the
// reflections' views run while the GPU draws. And the GPU says when the rects are copied, before the frame's last passes
// (native_vk.cpp, depth_send_early), so that wait is shorter still.
// HP2_DEPTHLATE=0: fetched here, as it was. The queue (hand_over) keeps its own way: no wait, the last frame the renderer has done.
static const bool g_depth_late = !(getenv("HP2_DEPTHLATE") && atoi(getenv("HP2_DEPTHLATE")) == 0);
struct DepthWant {
    u32 to;                                 // where the game wants the words
    NativeDepthRect r;
};
static std::vector<DepthWant> g_depth_wants;
static u32 g_depth_want_of;                 // the frame they are of (NativeFrame::handed)

static void depth_fill(const DepthWant &w, bool idle)
{
    static std::vector<uint32_t> words;
    words.resize((size_t)w.r.w * w.r.h);
    if (!words.empty() && native_vk_depth(w.r, words.data(), idle)) memcpy(host(w.to), words.data(), words.size() * 4);
}

static void depth_settle()
{
    if (g_depth_wants.empty()) return;
    if (native_depth_wait(g_depth_want_of))
        for (const DepthWant &w : g_depth_wants) depth_fill(w, true);
    g_depth_wants.clear();
}

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
        const DepthWant w = {LW(n + 8), {(int)LH(n + 20), (int)LH(n + 22), (int)LH(n + 24), (int)LH(n + 26)}};
        // With the queue (hand_over) the game does not wait for the renderer here: the depth is that of the last frame the renderer
        // has done, a frame older
        if (g_queue_now) {
            depth_fill(w, false);
        } else if (g_depth_late) {
            if (g_depth_wants.empty()) g_depth_want_of = g_hand_count;
            else if (g_depth_want_of != g_hand_count) depth_settle(), g_depth_want_of = g_hand_count;     // (not to be: settled at the hand-over)
            g_depth_wants.push_back(w);
        } else {
            // the last frame is the one handed over a moment ago: till it is with the GPU
            native_render_wait();
            depth_fill(w, true);
        }
    }
}

// the words StuffSun is about to read: fetched now (HOOK_FetchVRAM)
void HOOK_StuffSun__FP5eView(Ctx &c, void (*orig)(Ctx &))
{
    depth_settle();
    orig(c);
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
    Phase timed(PH_HUD);
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
    // whose packets these are (g_packet_who): the strip's object; a strip chained to this one renders inside this call and counts on
    const u64 group_was = g_hud_group;
    const u32 index_was = g_hud_index;
    const bool outer = g_hud_strips++ == 0;
    if (outer) {
        const u32 object = LW(strip + 8);
        g_hud_group = hud_group(1, object, g_hud_times[object]++);
        g_hud_index = 0;
        const u32 across = object ? LW(object + 48) : 0;
        memcpy(&g_hud_x, &across, 4);
    }
    orig(c);
    g_hud_strips--;
    if (outer) g_hud_group = group_was, g_hud_index = index_was;
    g_strip_spans.clear();
}

// TrackMap::Render (0x1bade8): the map on the HUD, its pieces and the cars' markers. Its packets are the map's (g_packet_who).
void HOOK_Render__8TrackMap(Ctx &c, void (*orig)(Ctx &))
{
    const u64 group_was = g_hud_group;
    const u32 index_was = g_hud_index;
    g_hud_group = hud_group(2, c.r[4].w[0], 0);
    g_hud_index = 0;
    orig(c);
    g_hud_group = group_was, g_hud_index = index_was;
}

static u32 g_added_strips;                  // strips handed to the lists: HP2_LODLOG takes a car's share of them

void HOOK_AddData__12eDmaDataListUsPvUsUs(Ctx &c, void (*orig)(Ctx &))
{
    if ((c.r[5].w[0] & 0xffff) == EDMA_STRIP) g_added_strips++;
    if (!g_strip_spans.empty() && (c.r[5].w[0] & 0xffff) == EDMA_DIRECT) g_packet_spans[c.r[6].w[0]] = g_strip_spans;
    if (g_hud_group && (c.r[5].w[0] & 0xffff) == EDMA_DIRECT) g_packet_who[c.r[6].w[0]] = {g_hud_group, g_hud_index++, g_hud_x};
    if (g_printf_now && (c.r[5].w[0] & 0xffff) == EDMA_DIRECT) g_packet_edge[c.r[6].w[0]] = true;
    orig(c);
}

void HOOK_RenderScreenPrintf(Ctx &c, void (*orig)(Ctx &))
{
    g_printf_now = true;
    orig(c);
    g_printf_now = false;
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

// a run's node: AddData's, of the port's kind. False: no room in the frame's memory
static inline bool rs_add_run(u32 gp, u32 list, u32 run, u32 strips, u32 parameter)
{
    const u32 pos = LW(gp - 19388), end = LW(gp - 19384), next = pos + 16;
    if (!((u64)(s64)(s32)next < (u64)(s64)(s32)end)) return false;
    SW(gp - 19388, next);
    const u32 tail = LW(list + 4);
    SH(pos + 4, (u16)EDMA_RUN);
    SW(tail, pos);
    SH(pos + 6, (u16)parameter);
    SH(pos + 8, (u16)strips);
    SH(pos + 10, (u16)g_frame_no);
    SW(pos + 12, run);
    SW(list + 4, pos);
    SW(pos, 0);
    return true;
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
    // The direct output (above): the strips known by their address, one after another into one list, are held back as a run (out)
    // and handed out as one node when anything else is to go into that list, or a function of the game's is to run.
    // A car's strips too (2026-11-24; HP2_DIRECTCARS=0: a node a strip, as before). All a car's draws need of the list is the
    // node's address (whose car: mid_owner_of) and the draw itself (its shadow, its move between two pictures), and a run's node
    // lies in the car's part of the frame's memory as its strips' nodes did. Not the player's own car in the player's view while
    // its mirrors may show the mirror's picture: a glass is told from the other strips one by one as the list is read
    // (mirror_glass_now).
    static const bool cars_direct = !(getenv("HP2_DIRECTCARS") && atoi(getenv("HP2_DIRECTCARS")) == 0);
    const bool glass_car = g_in_car_render && g_cfg.side_mirrors && g_cfg.reflections && target_id == 1 && g_car_now && g_car_now == pc_player_car() &&
                           !pc_two_player_race();
    const bool direct = direct_on() && !blend && (!g_in_car_render || (cars_direct && !glass_car));
    DirectRun out = {};
    u32 out_list = 0, out_parameter = 0;
    const auto hand_out = [&] {
        if (!out.strips) return;
        if (rs_add_run(gp, out_list, (u32)g_runs.size(), out.strips, out_parameter)) {
            g_runs.push_back(out);
            g_added_strips += out.strips;
            g_direct_strips += out.strips;
            g_direct_runs++;
        } else {                            // no room for the node: a node a strip, which says so the game's way
            for (u32 i = 0; i < out.strips; i++) rs_add_data(gp, out_list, EDMA_STRIP, g_run_strips[out.strip0 + i].data, g_run_strips[out.strip0 + i].qwords, out_parameter);
            g_run_strips.resize(out.strip0);
            g_run_ranges.resize(out.range0);
        }
        out = {};
    };
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
        if (direct && next) __builtin_prefetch(strip_seen(LW(next + 8)));       // the next strip's line of the table by address
        bool draw = !(strip_flags & 1024);
        if (opaque_off && alpha) draw = false;
        if (alpha_only && !alpha) draw = false;
        if (draw) {
            u32 active = LW(info + 152);
            if (!active || alpha) hand_out();
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
                if (out.strips && list != out_list) hand_out();
                if (active != last_active || alpha) {
                    if (out.strips && (LW(list + 16) != local_world || (light_context && LW(list + 12) != light_context))) hand_out();
                    rs_set(gp, list, 16, 0x2c85f0, local_world, EDMA_SET_LOCAL_WORLD, 4, 840);
                    if (light_context) rs_set(gp, list, 12, 0x2c85e0, light_context, EDMA_LIGHTCONTEXT, 18, 860);
                    last_active = active;
                }
                u32 material = default_material;
                if (const u32 materials = LW(solid + 188)) {
                    const s32 index = (s8)LB(strip + 31);
                    if (index >= 0 && index < (s32)(s16)LH(solid + 174)) material = LW(materials + ((u32)index << 3) + 4);
                }
                if (out.strips && LW(list + 8) != material) hand_out();
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
                } else if (direct && !alpha) {
                    const StripSeen *seen = strip_seen(data);
                    const u64 stamp = memwatch_stamp(data, qwords * 16);
                    if (seen->fast && seen->data == data && seen->qwords == qwords && seen->generation == g_strip_generation && seen->stamp == stamp) {
                        if (out.strips && (out_parameter != parameter || out.strips >= 60000)) hand_out();
                        if (!out.strips) {
                            out.range0 = (u32)g_run_ranges.size();
                            out.strip0 = (u32)g_run_strips.size();
                            out.generation = g_strip_generation;
                            out_list = list;
                            out_parameter = parameter;
                        }
                        const CachedStrip &at = seen->strip;
                        if (out.ranges && g_run_ranges.back().first + g_run_ranges.back().count == at.base) {
                            g_run_ranges.back().count += at.count;
                        } else {
                            g_run_ranges.push_back({at.base, at.count});
                            out.ranges++;
                        }
                        g_run_strips.push_back({data, qwords});
                        out.strips++;
                        out.tris += at.count;
                        out.verts += at.n;
                        out.sum += (stamp >> 32) + (u32)stamp;
                    } else {
                        hand_out();
                        rs_add_data(gp, list, EDMA_STRIP, data, qwords, parameter);
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
    hand_out();
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
    Phase timed(PH_STRIPS);
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
    SW(c.r[28].w[0] - 25272, pc_world_rate() > 30 ? 0 : retail);
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
bool stall_world(unsigned &clock_bits, unsigned &paused, unsigned &slow)
{
    if (!g_pc_ctx || !pc_racing() || pc_input_mode() != 2 || pc_race_menu() || pc_replay_playing()) return false;
    const u32 gp = g_pc_ctx->r[28].w[0], rm = LW(gp - 22440);
    clock_bits = LW(gp - 19572);            // WorldTimeSeconds
    paused = rm ? LW(rm + 16) : 0, slow = rm ? LW(rm + 20) : 0;
    return true;
}

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
// the world's clock in thirtieths of a second at this loop of the world, and at the loop before: made once a loop, by whichever
// thinker asks first (HOOK_Think__15AICarControllerf has why it is kept and how a replay gets it back)
static void think_ticks(u32 gp, s64 &tick, s64 &prev)
{
    const s32 real = (s32)LW(gp - 19552);
    u32 uw = LW(gp - 19572);
    float wt;
    memcpy(&wt, &uw, 4);
    if (real != g_think_seen) {
        g_think_seen = real;
        g_think_prev = g_think_tick;
        g_think_tick = (s64)(wt * 30.0f);
    }
    tick = g_think_tick, prev = g_think_prev;
}
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
// The zones: the game itself slows or stops the world's time for a moment (World::SetTimeScale, 0x14d620: the scale, for how long, and
// what to call then) when Player::RequestZone (0x177c88) grants one, asked by the player's buttons (Zone Freeze, Zone Zoom) or by the
// race (a spike belt, a warhead, the pursuit's logic, the player's own state). With the picture standing for half a second now and
// then in a race, the world standing while frames and sound go on, these are what the log is asked: each zone granted and each
// change of the time's scale, with the address it was asked from.
void HOOK_RequestZone__6Playeribbb(Ctx &c, void (*orig)(Ctx &))
{
    const u32 kind = c.r[5].w[0], f0 = c.r[6].w[0], f1 = c.r[7].w[0], f2 = c.r[8].w[0], from = c.r[31].w[0];
    orig(c);
    static int told;
    if (c.r[2].w[0] && told < 400) told++, fprintf(stderr, "zone: kind %u granted (%u %u %u), asked from %08x\n", kind, f0, f1, f2, from);
}

void HOOK_SetTimeScale__5WorldffPFi_vi(Ctx &c, void (*orig)(Ctx &))
{
    static int told;
    if (told < 400) told++, fprintf(stderr, "zone: the world's time at %.2f for %.2f s, asked from %08x\n", c.f[12].f, c.f[13].f, c.r[31].w[0]);
    orig(c);
}

// The second thing that goes by the step and not by the time is the car's own steering scale (CarTypeInfo +768: 0.8 to 1.25, 1.0 for
// some; the cars' table is chunk 0x34600 of GLOBALB.BUN). DoSteering keeps the wheel's angle as a word (RealDriver +528) WITH the
// scale in it, and starts each step by reading it back as a part of the lock WITHOUT the scale. So every step pushes the steering
// out by the scale (above 1: toward the lock; below 1: back toward the middle) before it moves it toward the stick, and what a
// second of driving gives depends on how many steps it had. At 60 a car of scale 1.25 let go at speed returns to the middle up to
// twice as slowly and above some 260 km/h does not return at all (each step's push is as large as the step back); a car of 0.8
// never reaches its full lock. At 30 the same push comes half as often, and that is how the cars were tuned. So at 60 the word is
// handed to every second step with the scale taken out (the steps of the world's count that are odd, a count that replays keep):
// the push comes 30 times a second again. A step with the scale taken out and the half-size step toward the stick are together
// what one step of the PS2 is. HP2_NOSTEERSCALE=1: without this part.
static void steer_step_60(Ctx &c, void (*orig)(Ctx &), bool second, bool with_scale)
{
    const u32 gp = c.r[28].w[0], driver = c.r[4].w[0];
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
    if (second && with_scale) {
        const u32 car = LW(driver + 560), info = car ? LW(car + 12) : 0;
        if (info) {
            const u32 bits = LW(info + 768);
            float scale;
            memcpy(&scale, &bits, 4);
            if (scale > 0.1f && scale < 10.0f && scale != 1.0f) {
                const int word = (s16)LH(driver + 528);
                SH(driver + 528, (u16)(s16)((float)word / scale));
                static bool told_scale;
                if (!told_scale) {
                    told_scale = true;
                    fprintf(stderr, "steering: 60 frames, the car's steering scale %.2f is fed back on every second step only\n", scale);
                }
            }
        }
    }
    orig(c);
    SW(at, was);
}

// HP2_STEERTEST=1: at the first step of a player's steering, the game's own function run over manoeuvres of two seconds for every
// steering scale the table of cars has (CarTypeInfo, 64 of 1376 bytes at gp-20616), at 36 to 360 km/h, going straight and sliding
// 20 degrees either way (the countersteering's branch): at 30 steps, at 60 with the least step alone (as it was), and at 60 with the
// scale's part too. For each scale the log gets how far the wheel at 60 was from the wheel at 30 (the worst and the mean of all
// thirtieths of a second, in parts of the lock) and in how many runs the wheel let go did not come back. HP2_STEERTEST=2: each run's
// line as well. Everything touched is put back.
static float steer_move(int move, float t)
{
    switch (move) {
    case 0: return t < 1.0f ? 1.0f : 0.0f;                              // full lock for a second, let go
    case 1: return t < 0.6f ? 1.0f : t < 1.2f ? -1.0f : 0.0f;           // one way, the other, let go
    case 2: return t < 1.4f && (int)(t / 0.2f) % 2 == 0 ? -1.0f : 0.0f; // taps of a fifth of a second, the other way
    case 3: return t < 1.0f ? 0.5f : 0.0f;                              // half a stick
    default: return t < 1.0f ? t : t < 1.5f ? 1.0f - (t - 1.0f) * 2.0f : 0.0f;     // a stick pushed slowly over and back
    }
}

static void steer_selftest(Ctx &c, void (*orig)(Ctx &))
{
    static const bool lines = atoi(getenv("HP2_STEERTEST")) >= 2;
    const Ctx saved = c;
    const u32 gp = c.r[28].w[0], driver = c.r[4].w[0], car = LW(driver + 560), info = car ? LW(car + 12) : 0, table = LW(gp - 20616);
    if (!info || !table) return;
    const u16 word0 = LH(driver + 528);
    const u32 counter0 = LW(driver + 584), stick0 = LW(driver + 596), speed0 = LW(car + 960), scale0 = LW(info + 768), weight0 = LW(gp - 22528);
    u8 velocity0[16];
    memcpy(velocity0, host(car + 112), 16);
    auto put = [](u32 a, float f) {
        u32 w;
        memcpy(&w, &f, 4);
        SW(a, w);
    };
    auto get = [](u32 a) {
        const u32 w = LW(a);
        float f;
        memcpy(&f, &w, 4);
        return f;
    };
    // the scales the cars have
    std::vector<float> scales;
    for (int i = 0; i < 64; i++) {
        const float sc = get(table + (u32)i * 1376 + 768);
        if (sc > 0.1f && sc < 10.0f && std::find(scales.begin(), scales.end(), sc) == scales.end()) scales.push_back(sc);
    }
    std::sort(scales.begin(), scales.end());
    // a velocity that is a slide of some 20 degrees either way: found by asking the game's own CarState::GetYawAlways (0x1c08d0)
    float slide[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    float found[3] = {0, 0, 0};
    put(car + 960, 50.0f);
    for (int deg = 0; deg < 360; deg++) {
        const float vx = cosf(deg * 0.0174533f), vy = sinf(deg * 0.0174533f);
        put(car + 112, vx * 50.0f), put(car + 116, vy * 50.0f), put(car + 120, 0.0f), put(car + 124, 0.0f);
        c = saved;
        c.r[4].d[0] = car + 64;
        ps2_call(c, 0x1c08d0);
        const float yaw = c.f[0].f * 57.29578f;
        for (int k = 1; k < 3; k++) {
            const float want = k == 1 ? 20.0f : -20.0f;
            if (found[k] == 0 || fabsf(yaw - want) < fabsf(found[k] - want)) found[k] = yaw, slide[k][0] = vx, slide[k][1] = vy;
        }
    }
    fprintf(stderr, "steertest: %d scales in the table; the slides found are %.1f and %.1f degrees\n", (int)scales.size(), found[1], found[2]);
    struct Sum {
        double worst[2] = {0, 0}, mean[2] = {0, 0};
        int n = 0, stuck[3] = {0, 0, 0}, runs = 0;
        float worst_speed[2] = {0, 0};
        int worst_move[2] = {0, 0}, worst_slide[2] = {0, 0};
    };
    for (float scale : scales) {
        Sum sum;
        for (int k = 0; k < 3; k++)
            for (int sp = 10; sp <= 100; sp += 10)
                for (int move = 0; move < 5; move++) {
                    float series[3][60];
                    for (int mode = 0; mode < 3; mode++) {      // 30; 60 with the least step; 60 with the scale's part too
                        const int fps = mode ? 60 : 30;
                        SH(driver + 528, 0);
                        SW(driver + 584, 0);
                        put(car + 112, slide[k][0] * sp), put(car + 116, slide[k][1] * sp), put(car + 120, 0.0f), put(car + 124, 0.0f);
                        put(car + 960, (float)sp);
                        put(info + 768, scale);
                        for (int step = 0; step < fps * 2; step++) {
                            put(driver + 596, steer_move(move, (step + 0.001f) / fps));
                            c = saved;
                            c.f[12].f = 1.0f / fps;
                            if (mode == 0) orig(c);
                            else steer_step_60(c, orig, (step & 1) != 0, mode == 2);
                            if (fps == 30 || (step & 1)) series[mode][fps == 30 ? step : step / 2] = (s16)LH(driver + 528) * (360.0f / 65536.0f);
                        }
                    }
                    float lock = 0.01f;
                    for (float v : series[0]) lock = std::max(lock, fabsf(v));
                    sum.runs++;
                    for (int mode = 0; mode < 3; mode++)
                        if (fabsf(series[mode][59]) > 0.1f * lock) sum.stuck[mode]++;
                    for (int m = 0; m < 2; m++) {
                        double w = 0;
                        for (int i = 0; i < 60; i++) {
                            const double diff = fabs(series[m + 1][i] - series[0][i]) / lock;
                            w = std::max(w, diff);
                            sum.mean[m] += diff;
                        }
                        if (w > sum.worst[m]) sum.worst[m] = w, sum.worst_speed[m] = (float)sp, sum.worst_move[m] = move, sum.worst_slide[m] = k;
                    }
                    sum.n += 60;
                    if (lines) {
                        fprintf(stderr, "steertest: run scale %.2f slide %d speed %d move %d lock %.2f |", scale, k, sp, move, lock);
                        for (int mode = 0; mode < 3; mode++) {
                            fprintf(stderr, " [%s]", mode == 0 ? "30" : mode == 1 ? "60" : "60fix");
                            for (int i = 0; i < 60; i += 1) fprintf(stderr, " %.2f", series[mode][i]);
                        }
                        fprintf(stderr, "\n");
                    }
                }
        fprintf(stderr,
                "steertest: scale %.2f, %d runs | 60 as it was: worst %.0f%% of the lock (at %.0f km/h, move %d, slide %d), mean %.1f%%, not back in %d | "
                "60 with the scale's part: worst %.0f%% (at %.0f km/h, move %d, slide %d), mean %.1f%%, not back in %d | at 30 not back in %d\n",
                scale, sum.runs, sum.worst[0] * 100, sum.worst_speed[0] * 3.6f, sum.worst_move[0], sum.worst_slide[0], sum.mean[0] / sum.n * 100, sum.stuck[1],
                sum.worst[1] * 100, sum.worst_speed[1] * 3.6f, sum.worst_move[1], sum.worst_slide[1], sum.mean[1] / sum.n * 100, sum.stuck[2], sum.stuck[0]);
    }
    SH(driver + 528, word0);
    SW(driver + 584, counter0);
    SW(driver + 596, stick0);
    SW(car + 960, speed0);
    SW(info + 768, scale0);
    SW(gp - 22528, weight0);
    memcpy(host(car + 112), velocity0, 16);
    c = saved;
}

// TEMP HP2_FORCECAR=<type>: every car of a race is that type (a number of the cars' table: 4 the Crown Victoria, 13 the TS50, 47 the
// Murcielago), with its first skin. For driving one car at 30 and at 60 frames by the same hands (HP2_TURNTEST).
void HOOK_BuildRideInfo__18FrontendPlayerInfoP8RideInfo7CarTypeUii(Ctx &c, void (*orig)(Ctx &))
{
    static const char *force = getenv("HP2_FORCECAR");
    if (force) c.r[6].d[0] = (u32)atoi(force);
    orig(c);
}

// TEMP HP2_TRAFFICLOG=1 (the user, 2026-11-14: "traffic is sometimes off its lane toward the shoulder; is that a bug?"). After a
// traffic driver's Think: its action (AIAction::Type: 1 avoid cars, 5 just drive, 6 pull over, 7 get unstuck, 9 just got hit, 10
// accident recovery), its place across the route point it is at (to the right of the point's direction, metres: TrackRoutePoint::
// GetPosition's own sign) beside where its driving line is there (DrivingLineOffsets[line] / 256, DrivingLocation::GetLinePosition),
// and the road's two widths. TrafficDriver: +0 the Car, +504 its DrivingLocation (route, point, line), +532 its AIAction (+588 type).
// ---- the AI drivers' averages at more than 30 steps a second (2026-11-14; on at the user's word after the measurements) ----
// Average (0x18fbf0) is a ring of N samples: +0 N, +1 the next slot, +2 how many are in, +4 their sum, +8 their mean, +12 the
// floats. PidError (two of them: +0 the integral's, +16 the derivative's; +32 the value now, +36 before) records once a call
// (PidError::Record, 0x18ff30: the integral's ring gets step x the mean of the two values, the derivative's the change over the
// step), and PsychoDriver::Drive calls it every frame for the heading, the velocity's heading (15 and 5 samples each) and the speed
// (6 and 3). So a window is N frames: half a second and a sixth at the PS2's 30, half of that at 60, and the integral's share of
// the steering (its sum times a gain) is half what it was for the same error.
// The rings PsychoDriver's constructor makes get room for eight times their N here, with N kept in the pad byte (+3). A ring is
// N long times the world's steps a second over 30 (pc_world_rate: 2 N at 60 and at 120, the same time as N at 30); for the
// attract demo, a PS2's recording, N. A ring found at another length is made over from its newest samples. The length is in the
// game's snapshots of an Average (its DoSnapshot saves +0 and that many floats), so a replay keeps it. HP2_PIDFRAMES=1: N always,
// as the game has it. Measured (handoff.md): racers in the soft physics kept their line at 60 with a median 1.4 to 1.9 m off it
// against 0.7 to 1.1 at 30; with this 0.9 to 1.1.
void HOOK___7Averagei(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0], n = c.r[5].w[0], ra = c.r[31].w[0];
    if (ra < 0x139760 || ra >= 0x1398c8 || n < 1 || n > 31) return orig(c);        // not PsychoDriver's
    c.r[5].d[0] = n * 8;
    orig(c);
    SB(self, (u8)n);
    SB(self + 3, (u8)n);
}

static void average_resize(u32 a, u32 want)
{
    const u32 slots = LB(a), cur = LB(a + 1), n = LB(a + 2), data = LW(a + 12), room = LB(a + 3) * 8u;
    if (!slots || !data || want > room) return;
    float old[256], kept[256];
    memcpy(old, host(data), slots * 4);
    u32 k = 0;
    const u32 take = n < want ? n : want;
    for (u32 i = 0; i < take; i++) {            // the newest `take`, oldest first: the newest is the slot before `cur`
        const u32 back = take - 1 - i;
        kept[k++] = old[(cur + slots * 2 - 1 - back) % slots];
    }
    float total = 0;
    for (u32 i = 0; i < k; i++) total += kept[i];
    memset(host(data), 0, room * 4);
    memcpy(host(data), kept, k * 4);
    const float mean = k ? total / (float)k : 0.0f;
    SB(a, (u8)want);
    SB(a + 1, (u8)(k % want));
    SB(a + 2, (u8)k);
    memcpy(host(a + 4), &total, 4);
    memcpy(host(a + 8), &mean, 4);
}

void HOOK_Record__8PidErrorff(Ctx &c, void (*orig)(Ctx &))
{
    static const bool by_frames = getenv("HP2_PIDFRAMES") != nullptr;
    const u32 self = c.r[4].w[0], rc = LW(c.r[28].w[0] - 22216);
    const u32 times = by_frames || (rc && LW(rc) == 9) ? 1u : (u32)std::min(std::max(pc_world_rate() / 30, 1), 8);
    for (u32 a : {self, self + 16}) {
        const u32 base = LB(a + 3);
        if (!base) continue;
        const u32 want = base * times;
        if (LB(a) != want) average_resize(a, want);
    }
    orig(c);
}

// TEMP HP2_TRAFFICLOG / HP2_AILOG: a thinker's car across its driving line (see HOOK_Think__13TrafficDriverf), every sixth frame
static void ai_line_log(Ctx &c, u32 driver, const char *who)
{
    if (g_frame_no % 6) return;
    const u32 car = LW(driver), loc = LW(driver + 504), action = LW(driver + 532);
    const u32 route = loc ? LW(loc) : 0, point = loc ? LW(loc + 4) : 0, line = loc ? LW(loc + 8) : 0;
    if (!car || !route || !point || line > 20) return;
    float pos[3], p[2], d[2], widths[2], v[3];
    memcpy(pos, host(car + 80), 12);
    memcpy(p, host(point), 8);
    memcpy(d, host(point + 12), 8);
    memcpy(widths, host(point + 24), 8);
    memcpy(v, host(car + 96), 12);
    const float across = (pos[0] - p[0]) * d[1] - (pos[1] - p[1]) * d[0], along = (pos[0] - p[0]) * d[0] + (pos[1] - p[1]) * d[1];
    const float authored = (float)(s16)LH(point + 48 + 2 * line) / 256.0f;
    fprintf(g_hle_log, "%s: frame %u t %.2f car %08x mode %u action %d%s route %d line %u across %.2f line's %.2f off %.2f widths %.1f %.1f along %.1f at %.1f %.1f%c", who, g_frame_no,
            (double)LW(c.r[28].w[0] - 19576) / 4000.0, car, LW(car + 36), action ? (int)LW(action + 588) : -1,
            action && LW(action + 588) == 6 ? (LW(action + 620) ? " to the edge" : " holding") : "", (int)(s16)LH(route + 10), line, across, authored, across - authored, widths[0],
            widths[1], along, pos[0], pos[1], 10);
}

void HOOK_Think__13TrafficDriverf(Ctx &c, void (*orig)(Ctx &))
{
    const u32 driver = c.r[4].w[0];
    // The traffic's turn to think at 60 frames (2026-11-14, the user's word after the measurements in handoff.md, "Трафик"). A traffic
    // car (DriverInfo's class 3: car +20, class +8) thinks when WorldLoopCounter % 12 is odd and half of it is the car's place
    // among the traffic (the cars of World's table from +40 to +28, table at +44): once in 12 loops of the world, 0.4 s on the
    // PS2. At 60 frames the loops come twice as fast, the plan of points is made anew every 0.2 s, and the traffic keeps its line
    // worse: twice the time more than a metre off it (measured). So at 60 the turn goes by thirtieths of a second of the world's
    // clock, as the racers' does: the count the function sees is the car's own phase on the loop where its thirtieth comes, and
    // nobody's otherwise. The attract demo is a PS2's recording and thinks as it was made. HP2_TRAFFICFRAMES=1: by loops, as before.
    {
        const u32 gp = c.r[28].w[0], rc = LW(gp - 22216), car = LW(driver), info = car ? LW(car + 20) : 0;
        static const bool by_frames = getenv("HP2_TRAFFICFRAMES") != nullptr;
        int me = -1;
        if (pc_world_rate() > 30 && !(rc && LW(rc) == 9) && !by_frames && info && LW(info + 8) == 3) {
            const u32 world = LW(gp - 22120);
            const int from = (int)LW(world + 40), count = (int)LW(world + 28) - from;
            for (int i = 0; i < count && i < 6; i++)
                if (LW(world + 44 + (u32)(from + i) * 4) == car) me = i;
        }
        if (me < 0) {
            orig(c);
        } else {
            const s32 real = (s32)LW(gp - 19552);
            s64 tick, prev;
            think_ticks(gp, tick, prev);
            const int mine = me * 2 + 1;
            bool turn = false;
            if (tick < prev) turn = tick % 12 == mine;          // the clock went back: another race
            else if (tick - prev >= 12) turn = true;
            else
                for (s64 t = prev + 1; t <= tick; t++)
                    if (t % 12 == mine) turn = true;
            const int want = turn ? mine : 0;                   // an even phase is nobody's
            s32 fake = real - (((real % 12) - want + 12) % 12);
            if (fake < 0) fake += 12;
            SW(gp - 19552, (u32)fake);
            orig(c);
            SW(gp - 19552, (u32)real);
        }
    }
    static const bool on = getenv("HP2_TRAFFICLOG") != nullptr;
    if (on) ai_line_log(c, driver, "traffic");
}

// TEMP HP2_FORCETRACK=<n>: the quick race's track is that one, for the same road in two runs; the log says what was picked.
void HOOK_PickTrackNumberForExpressGame__Fv(Ctx &c, void (*orig)(Ctx &))
{
    static const char *force = getenv("HP2_FORCETRACK");
    orig(c);
    const u32 desc = c.r[2].w[0];
    fprintf(stderr, "quick race: track %d (and %d) picked%s\n", (int)LW(desc + 8), (int)LW(desc + 12), force ? ", another is forced" : "");
    if (force) {
        SW(desc + 8, (u32)atoi(force));
        if (const char *second = strchr(force, ',')) SW(desc + 12, (u32)atoi(second + 1));
    }
}

// TEMP HP2_TURNTEST=<m/s>: the port's own hands in a live race, by the world's clock and the car's speed, so that the same drive can
// be had at 30 and at 60 frames: full throttle from the moment the car is the player's; at that speed full left with the throttle
// kept, for 1.5 s; then nothing. native_turntest_pad gives the buttons (1 throttle, 2 left); the steering's hook writes the car's
// state after each of its steps while the test runs.
static int g_turn_phase;                // 0 before the speed, 1 turning, 2 done
static float g_turn_from;               // the world's time the turn began at

static float turn_fl(u32 a)
{
    const u32 w = LW(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

int native_turntest_pad()
{
    static const char *want = getenv("HP2_TURNTEST");
    if (!want || !g_pc_ctx || !pc_car_input()) return 0;
    const u32 gp = g_pc_ctx->r[28].w[0], world = LW(gp - 22120), car = world ? LW(world + 44) : 0;
    if (!car) return 0;
    const float speed = turn_fl(car + 960), now = turn_fl(gp - 19572);
    if (g_turn_phase == 0 && speed >= (float)atof(want)) g_turn_phase = 1, g_turn_from = now;
    const char *hold = strchr(want, ',');           // HP2_TURNTEST=<m/s>,<seconds of the turn>
    if (g_turn_phase == 1 && now - g_turn_from >= (hold ? (float)atof(hold + 1) : 1.5f)) g_turn_phase = 2;
    return g_turn_phase == 0 ? 1 : g_turn_phase == 1 ? 3 : 0;
}

static void turntest_log(Ctx &c)
{
    static const bool on = getenv("HP2_TURNTEST") != nullptr;
    static int after;
    if (!on || !pc_car_input()) return;
    if (g_turn_phase == 2 && ++after > 60) return;
    const u32 gp = c.r[28].w[0], driver = c.r[4].w[0], car = LW(driver + 560);
    static bool named;
    if (!named) {
        named = true;
        const u32 info = LW(car + 12);
        fprintf(stderr, "turntest: the car is %s, steering scale %.2f, kind %u\n", (const char *)host(info), turn_fl(info + 768), LW(info + 1336));
    }
    Ctx yaw = c;
    yaw.r[4].d[0] = car + 64;
    ps2_call(yaw, 0x1c08d0);            // CarState::GetYawAlways: the slide's angle
    fprintf(stderr, "turntest: t %.4f phase %d since %.4f speed %.3f pos %.3f %.3f %.3f vel %.3f %.3f %.3f slip %.3f word %d stick %.2f step %.5f\n",
            turn_fl(gp - 19572), g_turn_phase, g_turn_phase ? turn_fl(gp - 19572) - g_turn_from : 0.0f, turn_fl(car + 960), turn_fl(car + 96), turn_fl(car + 100),
            turn_fl(car + 104), turn_fl(car + 112), turn_fl(car + 116), turn_fl(car + 120), yaw.f[0].f * 57.29578f, (int)(s16)LH(driver + 528), turn_fl(driver + 596),
            c.f[12].f);
}

static void steering_hook(Ctx &c, void (*orig)(Ctx &));

void HOOK_DoSteering__10RealDriverf12JoystickPort(Ctx &c, void (*orig)(Ctx &))
{
    const Ctx before = c;
    steering_hook(c, orig);
    Ctx at = before;
    turntest_log(at);
}

static void steering_hook(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rc = LW(gp - 22216);
    static const bool off = getenv("HP2_NOSTEERFIX") != nullptr, scale_off = getenv("HP2_NOSTEERSCALE") != nullptr;
    static const bool test = getenv("HP2_STEERTEST") != nullptr;
    static bool tested;
    if (test && !tested) {
        tested = true;
        steer_selftest(c, orig);
    }
    if (off || pc_world_rate() == 30 || (rc && LW(rc) == 9)) {
        orig(c);
        return;
    }
    if (test) {
        static int said;
        if (said < 16) said++, fprintf(stderr, "steertest: a real step, the world's count %u, the step %.5f s, driver %08x\n", LW(gp - 19552), c.f[12].f, c.r[4].w[0]);
    }
    // Which steps get the car's scale fed back as the game does it: one in each thirtieth of a second of the world's clock, the step
    // on which the thirtieth begins (think_ticks: the count a replay keeps); every other step has it taken out. At 60 steady steps
    // that is every second step, as it was by the count's last bit; at any other rate, and when a step is long, it is still 30
    // times a second. HP2_STEERPARITY=1: by the count's last bit, as before 2026-11-14.
    static const bool parity = getenv("HP2_STEERPARITY") != nullptr;
    s64 tick, prev;
    think_ticks(gp, tick, prev);
    steer_step_60(c, orig, parity ? (LW(gp - 19552) & 1) != 0 : tick == prev, !scale_off);
}

// The driver's animation at 60 frames. AnimatedDriver::BuildQuaternionTable (0x1aa2f0, from the car's drawing) takes the pose in whole
// frames of the animations: the hands by the frame that CalculateSteeringFrameNumber makes of the steering (15 to 45, cut to a whole
// number), and each playing clip (a gear change, a hit, a look back) by AnimationPlayer::GetFrameNumber, one frame for two sixtieths of
// the world's clock. The animation's own functions take a frame as a float and cut it too. So at 60 frames a second a clip stood still
// every other frame and the hands moved in steps. Here, at 60 only, the part of a frame that those two cut off is kept, and the three
// functions that read the animation give the pose between the frame and the next by it: each bone's rotation turned the short way and
// made of length one again, the root's place in a straight line. Nothing but the picture reads the pose. At 30 nothing is touched;
// HP2_NODRIVER60=1: as the game does it.
static bool g_drv_on;           // inside BuildQuaternionTable, at 60 frames
static float g_drv_part;        // how far past its whole frame the pose that is taken next is: 0 to 1
static bool g_drv_told;

void HOOK_BuildQuaternionTable__14AnimatedDriverP10eAnimationP11bQuaternionP8bVector3(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NODRIVER60") != nullptr;
    g_drv_on = !off && pc_world_rate() > 30;
    g_drv_part = 0;
    orig(c);
    g_drv_on = false;
    g_drv_part = 0;
}

void HOOK_CalculateSteeringFrameNumber__14AnimatedDriver(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0];
    orig(c);
    if (!g_drv_on) return;
    const u32 w = LW(LW(self + 28));        // the steering's frame as it is kept, before it is cut
    float kept;
    memcpy(&kept, &w, 4);
    const float part = kept - (float)(s32)c.r[2].w[0];
    g_drv_part = part > 0.0f && part < 1.0f ? part : 0.0f;
}

void HOOK_GetFrameNumber__15AnimationPlayer(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0], gp = c.r[28].w[0];
    orig(c);
    if (!g_drv_on) return;
    const s32 past = (s32)(LW(gp - 19568) - LW(self + 4)), last = (s32)LW(self + 12), frame = (s32)c.r[2].w[0];
    g_drv_part = past > 0 && (past & 1) && (last == -1 || frame < last) ? 0.5f : 0.0f;
    if (g_drv_part > 0 && !g_drv_told) {
        g_drv_told = true;
        fprintf(stderr, "driver: a clip between its frames %d and %d (60 frames)\n", frame, frame + 1);
    }
}

// q = a turned toward b by t, the short way, of length one
static void drv_blend(float *q, const float *a, const float *b, float t)
{
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = dot < 0.0f ? -1.0f : 1.0f;
    float n = 0.0f;
    for (int k = 0; k < 4; k++) q[k] = a[k] + (b[k] * s - a[k]) * t, n += q[k] * q[k];
    if (n > 1e-12f) {
        n = 1.0f / sqrtf(n);
        for (int k = 0; k < 4; k++) q[k] *= n;
    } else memcpy(q, a, 16);
}

// The scenes before a race (NisManager) at 60 frames, the same way. NisManager::Render (0x1b2c90) has SetupForRendering (0x1b13f0) give
// the scene's characters their frame, GetFrameNumber (0x1b2c70): one for two ticks of the clock at gp-19564 since the scene began,
// kept as a float in each eAnimation (+48) and cut to a whole number by the functions below. On an odd tick the frame kept is made
// half a frame more here (unless it is the animation's last), and while the scene is drawn those functions give the pose between.
static bool g_nis_on;           // inside NisManager::Render, at 60 frames
static bool g_nis_told;

void HOOK_Render__10NisManagerP5eView(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NODRIVER60") != nullptr;
    g_nis_on = !off && pc_world_rate() > 30;
    orig(c);
    g_nis_on = false;
}

void HOOK_SetupForRendering__10NisManager(Ctx &c, void (*orig)(Ctx &))
{
    const u32 self = c.r[4].w[0], gp = c.r[28].w[0];
    orig(c);
    if (!g_nis_on) return;
    const s32 past = (s32)(LW(gp - 19564) - LW(self + 72));
    if (past <= 0 || !(past & 1)) return;
    for (u32 at : {self + 128, self + 176}) {
        const u32 anim = LW(at), solid = anim ? LW(anim + 40) : 0;
        if (!solid) continue;
        const u32 w = LW(anim + 48);
        float frame;
        memcpy(&frame, &w, 4);
        if (frame < 0.0f || (int)frame + 1 >= (int)LH(solid + 68)) continue;
        frame = (float)(int)frame + 0.5f;
        u32 nw;
        memcpy(&nw, &frame, 4);
        SW(anim + 48, nw);
        if (!g_nis_told) {
            g_nis_told = true;
            fprintf(stderr, "driver: a scene's character between its frames %d and %d (60 frames)\n", (int)frame, (int)frame + 1);
        }
    }
}

// The frame after the one asked for is in the animation: there is something to go toward, and how far. For the driver the part is
// the one kept (g_drv_part); in a scene it is what the frame itself has past its whole number.
static bool drv_between(u32 solid, float frame, int &whole, float &part)
{
    whole = (int)frame;
    part = g_drv_on ? g_drv_part : g_nis_on ? frame - (float)whole : 0.0f;
    return part > 0.0f && part < 1.0f && whole >= 0 && whole + 1 < (int)LH(solid + 68);
}

void HOOK_GetRotation__15eAnimationSolidP11bQuaternionif(Ctx &c, void (*orig)(Ctx &))
{
    const u32 solid = c.r[4].w[0], out = c.r[5].w[0], bone = c.r[6].w[0];
    int whole;
    float part;
    if (!drv_between(solid, c.f[12].f, whole, part)) return orig(c);
    float a[4], b[4], q[4];
    orig(c);
    memcpy(a, host(out), 16);
    c.r[4].d[0] = solid, c.r[5].d[0] = out, c.r[6].d[0] = bone, c.f[12].f = (float)(whole + 1);
    orig(c);
    memcpy(b, host(out), 16);
    drv_blend(q, a, b, part);
    memcpy(host(out), q, 16);
}

void HOOK_GetRotations__15eAnimationSolidP11bQuaternionf(Ctx &c, void (*orig)(Ctx &))
{
    const u32 solid = c.r[4].w[0], out = c.r[5].w[0], bones = LH(solid + 70);
    int whole;
    float part;
    static float a[256][4];
    if (!drv_between(solid, c.f[12].f, whole, part) || bones > 256) return orig(c);
    orig(c);
    memcpy(a, host(out), bones * 16);
    c.r[4].d[0] = solid, c.r[5].d[0] = out, c.f[12].f = (float)(whole + 1);
    orig(c);
    float *b = (float *)host(out);
    for (u32 i = 0; i < bones; i++) {
        float q[4], next[4];
        memcpy(next, b + i * 4, 16);
        drv_blend(q, a[i], next, part);
        memcpy(b + i * 4, q, 16);
    }
}

void HOOK_GetTranslation__15eAnimationSolidP8bVector3f(Ctx &c, void (*orig)(Ctx &))
{
    const u32 solid = c.r[4].w[0], out = c.r[5].w[0];
    int whole;
    float part;
    if (!drv_between(solid, c.f[12].f, whole, part)) return orig(c);
    float a[3], b[3];
    orig(c);
    memcpy(a, host(out), 12);
    c.r[4].d[0] = solid, c.r[5].d[0] = out, c.f[12].f = (float)(whole + 1);
    orig(c);
    memcpy(b, host(out), 12);
    for (int k = 0; k < 3; k++) a[k] += (b[k] - a[k]) * part;
    memcpy(host(out), a, 12);
}

// tCopManager::IsCopNearby (0x225d30) of a car, at the distance tPursuit's check of the player asks it with for a slow car: the manager
// at gp-15620, the distance the float at gp-15592. It only reads.
static bool cop_near(Ctx &c, u32 cops, u32 car)
{
    Ctx saved = c;
    c.r[4].d[0] = (u64)(s64)(s32)cops;
    c.r[5].d[0] = (u64)(s64)(s32)car;
    c.f[12].u = LW(c.r[28].w[0] - 15592);
    c.r[29].d[0] -= 0x200;
    ps2_call(c, 0x225d30);
    const bool near = c.r[2].w[0] != 0;
    c = saved;
    return near;
}

// A reset during a chase (after spartaque12's pull request 10 to the release repository). RealDriver::JoyHandler_ResetCar puts the car
// back on the road (Car::ResetToRoutePoint, 0x132f20), which marks it as just reset, car+2140 = 2, counted down by Car::Drive.
// tPursuit::IsChasedCarStoppable (0x22bea0) takes a blinking car with that mark for busted, with no cop near or speed asked, so on the
// PS2 a reset is an arrest for as long as the chase lasts, the cops long out of sight. Here it is an arrest only with a cop within the
// bust's distance when the reset is asked; else the mark is put back as it was: the car still goes back and blinks, the chase goes on.
// The mark is read by nothing else but the replay's snapshot. The cops are asked only when the handler is going to reset: a press (a2's
// low byte), nothing at driver+572, a car with a player (car+24), a cop manager; whether the player may reset (Player::IsAllowedToReset)
// is left to the handler, as without a reset the mark stays as it was anyway. HP2_PS2RESET: the PS2's way. Never in the attract demo
// (RCSTATE_REPLAY_DEMO_MODE, a PS2 recording).
void HOOK_JoyHandler_ResetCar__10RealDriver12JoystickPort8JoyEventUcPv(Ctx &c, void (*orig)(Ctx &))
{
    const u32 gp = c.r[28].w[0], rc = LW(gp - 22216), drv = c.r[7].w[0], cops = LW(gp - 15620);
    static const bool ps2 = getenv("HP2_PS2RESET") != nullptr;
    const u32 car = drv ? LW(drv + 560) : 0;
    if (ps2 || (rc && LW(rc) == 9) || !cops || !car || LW(drv + 572) || !(c.r[6].w[0] & 255) || !LW(car + 24) || cop_near(c, cops, car)) {
        orig(c);
        return;
    }
    const u32 was = LW(car + 2140);
    orig(c);
    SW(car + 2140, was);
}

void HOOK_Think__15AICarControllerf(Ctx &c, void (*orig)(Ctx &))
{
    struct Logged {                         // TEMP HP2_AILOG=1: the racers' (and whoever else thinks here) places across their lines
        Ctx &c;
        u32 driver;
        ~Logged()
        {
            static const bool on = getenv("HP2_AILOG") != nullptr;
            if (on) ai_line_log(c, driver, "ai");
        }
    } logged{c, c.r[4].w[0]};
    const u32 gp = c.r[28].w[0], rc = LW(gp - 22216);
    // the attract demo (RCSTATE_REPLAY_DEMO_MODE) is a PS2 recording, its racers' thinking that of 30 frames; a replay made here thinks as it
    // was made
    if (pc_world_rate() == 30 || (rc && LW(rc) == 9)) {
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
    s64 tick, prev;
    think_ticks(gp, tick, prev);
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
    const u32 was_car = g_car_now;
    g_in_car_render = true;
    g_car_asked_plain = (c.r[10].w[0] & 1) != 0;
    g_car_now = c.r[4].w[0] ? LW(c.r[4].w[0]) : 0;
    struct Leave {
        bool in, plain;
        u32 car;
        ~Leave() { g_in_car_render = in, g_car_asked_plain = plain, g_car_now = car; }
    } leave{was_in, was_plain, was_car};
    // 120 pictures a second: what this call adds to the lists is this car's (mid_car_end)
    struct Owned {
        Ctx &c;
        u32 info, from;
        float pose[16];
        ~Owned() { mid_car_end(c, info, from, pose); }
    } owned{c, c.r[4].w[0], 0, {}};
    owned.from = mid_car_begin(c, owned.pose);
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
static thread_local bool t_das_count = true;    // a worker does not count: the counters are the game's thread's

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
        if (t_das_count) g_das_quick++;
        return quick;
    }
    const s32 exact = das_pixels_exact(len2, reach, h);
    if (!t_das_count) return exact;
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
    // (and on a picture wider than 16:9, whatever the distance: the zone's list is what the PS2's view could see from it, and a
    // view twice as wide looks past its sides; the user, 2026-11-17: the sides drawn as the draw distance draws the far)
    const bool ultrawide = pc_racing() && native_narrow() < 0.74f;
    if ((g_cfg.draw_distance == 100 && !ultrawide) || !player_view(info)) return;
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

// ---- a view's culling by the port's own code, its sections on the workers ------------------------------
// CullView (0x150b10) asks WhatSectionsShouldWeDraw for the view's sections and walks each one's tree of boxes (TreeCull, 0x150378):
// a box wholly out of sight is dropped with all under it, an object in sight goes through DrawAScenery (0x14fe18), which measures
// its size on the screen, picks its level of detail and adds it to the view's table of what to draw, with a matrix from the frame's
// memory. The sections do not depend on one another, and all the deciding (the boxes, the sizes, the levels) only reads: so the
// sections of a player's view are decided by the workers, each into a list of its own, and then the game's thread adds the lists to
// the table one after another in the sections' order, taking the matrices from the frame's memory as it goes. The table and the
// memory are then what the game's own walk leaves, to the byte (HP2_CULLCHECK=<n>: every n-th view is culled both ways and the
// whole memory compared). The other views (the mirror, the six of the reflections: a few dozen objects) are decided the same way
// on the game's thread. HP2_NOCULLNATIVE=1: the game's walk, as it was.
u32 hot_classify_aabb(const float lo[3], const float hi[3], u32 matrix);
void F_00150950(Ctx &c);                    // GrandSceneryCullInfo::WhatSectionsShouldWeDraw(out, max, cull info)

struct CullKept {
    u32 inst, model_state;                  // the SceneryInstance, and the table's first word: the model's address + 1 or 2
    u8 how;                                 // 0 drawn where it stands; 1 with its own matrix; 2 with a facade's (CreateFacadeMatrix)
};
struct CullSection {
    u32 header;
    std::vector<CullKept> kept;
};
static std::vector<CullSection> g_cull_sections;
static Ctx g_cull_ctx[16];                  // a worker's registers for the game's vector arithmetic: one a slot of jobs_run

// One section: TreeCull's walk and DrawAScenery's decisions, into s.kept in the game's order. w is the caller's own registers.
static void cull_section(Ctx &w, CullSection &s, u32 cull, u32 h_bits, bool max_detail)
{
    const u32 header = s.header, tree = LW(header + 36), instances = LW(header + 28), kinds = LW(header + 20);
    const u32 hide = LW(cull + 132);
    s.kept.clear();
    u32 nodes[128];
    u8 states[128];
    int top = 0;
    nodes[0] = tree;
    states[0] = 1;
    top = 1;
    while (top) {
        top--;
        const u32 node = nodes[top];
        u32 state = states[top];
        if (state == 1) {
            float lo[3], hi[3];
            for (u32 i = 0; i < 3; i++) lo[i] = (float)(s32)(s16)LH(node + i * 2), hi[i] = (float)(s32)(s16)LH(node + 6 + i * 2);
            state = hot_classify_aabb(lo, hi, cull + 64) & 255;
        }
        if (!state) continue;
        const s32 count = (s32)(s16)LH(node + 12);
        for (s32 i = 0; i < count; i++) {
            const s32 child = (s32)(s16)LH(node + 14 + (u32)i * 2);
            if (child < 0) {
                if (top < 128) {
                    nodes[top] = tree + (u32)((s32)(s16)(-child) * 24);
                    states[top] = (u8)state;
                    top++;
                }
                continue;
            }
            // DrawAScenery(header, child, cull, state)
            const u32 inst = instances + (u32)child * 48;
            u32 st = state;
            if (st == 1) {
                float lo[3], hi[3];
                for (u32 k = 0; k < 3; k++) lo[k] = (float)(s32)(s16)LH(inst + k * 2), hi[k] = (float)(s32)(s16)LH(inst + 6 + k * 2);
                st = hot_classify_aabb(lo, hi, cull + 64);
                if (!st) continue;
            }
            const u32 kind = kinds + (u32)((s32)(s16)LH(inst + 12) * 40);
            const u32 flags = (u32)(s32)(s16)LH(inst + 14);
            if (flags & hide & 255) continue;
            // the way from the camera to the object, along the view and in length: the game's statements, on this thread's registers.
            // (The game stores vf3 on its stack after the subtraction and reads it back: the same value, so not here.)
            op_lqc2(w, 1, cull + 160);
            op_lqc2(w, 3, inst + 16);
            w.f[1].u = LW(kind + 36);
            VU_vsub(w, 0x4be118ecu);
            w.f[0].u = 0x40800000u;
            op_lqc2(w, 2, cull + 176);
            FPU_add_s(w, 2, 1, 0);
            VU_vmul(w, 0x4bc218aau);
            VU_vaddy(w, 0x4b021041u);
            VU_vaddz(w, 0x4b020842u);
            FPU_neg_s(w, 1, 2, 0);
            w.f[0].u = ee_bits(w.vf[1].f[0]);
            FPU_c_lt_s(w, 0, 0, 1);
            s32 pixels = 0;
            if (!(w.fcr31 & FCR31_C)) {
                VU_vmul(w, 0x4bc3186au);
                VU_vaddy(w, 0x4b010881u);
                VU_vaddz(w, 0x4b011082u);
                pixels = das_pixels(w.vf[2].f[0], ee_float(w.f[2].u), ee_float(h_bits));
            }
            if (pixels < 2) continue;
            // the first of the three models the object is large enough for, and has. Level of Detail at Maximum: the first model at
            // any size, where the kind has one (HOOK_DrawAScenery sets its FarClipSize[0] to 0 for the call)
            const s32 far0 = max_detail && LW(kind + 20) ? 0 : (s32)(s16)LH(kind + 12);
            u32 model = 0;
            s32 lod = -1;
            if (!(pixels < far0) && LW(kind + 20)) {
                model = LW(kind + 20);
                lod = 0;
            } else {
                for (s32 k = 1; k < 3; k++) {
                    if (pixels < (s32)(s16)LH(kind + 12 + (u32)k * 2)) continue;
                    const u32 m = LW(kind + 20 + (u32)k * 4);
                    if (!m) continue;
                    model = m;
                    lod = k;
                    break;
                }
            }
            if (!model) continue;
            const u8 how = (LH(inst + 14) & 512) ? 0 : (s8)LB(kind + (u32)lod + 32) ? 2 : 1;
            s.kept.push_back({inst, model + st, how});
        }
    }
}

// What DrawAScenery writes for an object it keeps: its matrix in the frame's memory and its place in the view's table. On the game's
// thread, in the game's order. sp: scratch on the game's stack, as DrawAScenery's own frame.
static void cull_keep(Ctx &c, u32 cull, const CullKept &k, u32 sp)
{
    const u32 gp = c.r[28].w[0], inst = k.inst;
    auto less = [](u32 a, u32 b) { return (u64)(s64)(s32)a < (u64)(s64)(s32)b; };
    if (k.how == 0) {
        const u32 slot = LW(cull + 140);
        if (less(slot, LW(cull + 144))) {
            SW(cull + 140, slot + 8);
            SW(slot, k.model_state);
            SW(slot + 4, 0);
        }
        return;
    }
    const u32 pos = LW(gp - 19388), top = pos + 64;
    if (!less(top, LW(gp - 19384))) {       // FRAME_MALLOC's failure
        const u32 failed = LW(gp - 19368);
        SW(gp - 19372, 1);
        SW(gp - 19368, failed + 64);
        return;
    }
    SW(gp - 19388, top);
    const u32 matrix = pos;
    if (k.how == 2) {
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
        SW(slot, k.model_state);
    }
}

static u64 g_cull_views, g_cull_parallel, g_cull_sections_n, g_cull_kept;

// ---- which strips of the scenery's models are in sight, worked out ahead by the workers ----------------
// StuffScenery (0x150eb8) goes through the view's table of what to draw and renders each model (eModel::Render, 0x10dc80), and
// Render first asks DetermineVisibleStrips (0x10d390) which of the model's strips are in sight: the model's box against the view,
// and where the box is across the view's edge, each strip's own box (CullVisibleStrips, 0x10cf50). The answer is a table of runs of
// strips at 0x2d0c80, {first strip, class, count}, ended by a zero, which RenderStrips reads at once. The table is one for the
// whole game, so the game's own function cannot run for two models at a time; but the answer depends on nothing but the model, its
// matrix and the view. So before StuffScenery starts, the answers for its models are worked out by the workers into lists of
// their own, and DetermineVisibleStrips, when the game calls it, copies the model's list into the table.
// Only the plain way through the function is taken over: no rigid or vertex animation, no back-face cone (the solid's flag 0x1000,
// which goes through fInvertMatrix), the model not known to be wholly in sight (that way is two stores). Any other model, and any
// call not found here, goes to the game's function. HP2_VISCHECK=1: the game's function runs as well, every time, and its table
// is compared with the list. HP2_NOVISAHEAD=1: the game's function alone.
u32 hot_classify_aabb_host(const float lo[3], const float hi[3], const float matrix[16]);
void hot_mul_matrix(float out[16], u32 a_addr, u32 b_addr);

struct VisAhead {
    u32 model, matrix;                      // what DetermineVisibleStrips is called with
    u32 first, count;                       // its runs, in its worker's g_vis_runs
    u8 slot;
    u8 result;                              // what the function returns: 1 when it leaves a table (even an empty one)
    bool ready;
};
struct VisRun {
    u32 strip;
    u16 klass, count;
};
static std::vector<VisAhead> g_vis;
static std::vector<VisRun> g_vis_runs[16];
static size_t g_vis_at;                     // the next one StuffScenery is to ask for
static bool g_vis_on;                       // inside StuffScenery, with answers ready
static u64 g_vis_used, g_vis_missed, g_vis_checked, g_vis_bad;

static void vis_ahead(VisAhead &v, u32 view, int slot)
{
    std::vector<VisRun> &runs = g_vis_runs[slot];
    v.slot = (u8)slot;
    v.first = (u32)runs.size();
    v.count = 0;
    v.result = 0;
    const u32 solid = LW(v.model + 12);
    float lo[3], hi[3], screen[16];
    memcpy(lo, host(solid + 64), 12);
    memcpy(hi, host(solid + 80), 12);
    hot_mul_matrix(screen, v.matrix, LW(view + 80));        // CalculateLocalToScreenClip
    u32 klass = hot_classify_aabb_host(lo, hi, screen);
    if (klass == 1) {
        float clip[16];
        hot_mul_matrix(clip, v.matrix, LW(view + 72));       // CalculateLocalToClip
        klass = hot_classify_aabb_host(lo, hi, clip);
    }
    const s32 n = (s32)(s16)LH(solid + 52);
    if (klass == 2) {
        // SetAllStripsVisible: one run of them all
        runs.push_back({n ? LW(solid + 176) : 0, 2, (u16)n});
        v.result = 1;
    } else if (klass == 1) {
        // CullVisibleStrips: each strip's box against the screen's matrix; a strip across the edge is of the caller's class, 1.
        // (The game's loop means to join strips of one class into a run and never does: a run a strip.)
        u32 strip = LW(solid + 176);
        for (s32 i = 0; i < n; i++, strip += 64) {
            float slo[3], shi[3];
            memcpy(slo, host(strip + 16), 12);
            memcpy(shi, host(strip + 32), 12);
            const u32 c = hot_classify_aabb_host(slo, shi, screen);
            if (c) runs.push_back({strip, (u16)c, 1});
        }
        v.result = 1;
    }
    v.count = (u32)runs.size() - v.first;
    v.ready = true;
}

// ---- the scenery handed out by the port's own loop, the models' strips worked out by the workers (change 2, 2026-11-24) ----------
// StuffScenery (0x150eb8) goes through a view's draw table, an entry a model: a light context made for a lit one, then eModel::Render,
// which asks which strips are in sight (DetermineVisibleStrips) and hands them to the lists (RenderStrips). Three thousand models a
// frame, one after another on the game's thread, though what a model hands out depends on nothing the others do.
// So the loop is the port's here, in three steps. The table is read (the models that can be done this way: no animation, no back
// face cone, no replaced textures). Then the workers make each model's plan, reading only: the strips in sight, as vis_ahead does,
// and what render_strips_native would do with each, as items in the strips' order: a run of strips known by their address (the
// direct output's, with its ranges of the vertex cache), a strip not known yet, an alpha strip. Then the game's thread goes through
// the table as the game does, makes the light contexts, and for a planned model puts the plan's items into the lists: the list of
// each item's texture, the state's nodes, the node. Everything the game's memory gets is what the game's loop with the direct output
// leaves there, to the byte (HP2_STUFFCHECK=<n>: every n-th call is done both ways on the same memory and compared whole).
// The other models (animated ones, a few) go through the game's eModel::Render where they stand in the table.
// HP2_SCENERY=0: the game's loop. HP2_THREADS=0 or a short table: the plans are made on the game's thread, by the same code.
void F_0010dc80(Ctx &c);                    // eModel::Render(matrix, light context, flags)
void F_00115978(Ctx &c);                    // elSetupLightContext(context, matrix, world lights, view's position, view)

enum { PLAN_RUN, PLAN_STRIP, PLAN_ALPHA };
struct PlanItem {
    u8 kind;
    u16 parameter;                          // the node's
    u32 info, material;                     // the strips' TextureInfo and eLightMaterial
    u32 strip, data, qwords;                // a strip of its own: its record, its packet
    DirectRun run;                          // a run: range0 and strip0 are places in the slot's arrays
};
struct PlanSlot {                           // a worker's own, or the game's thread's
    std::vector<PlanItem> items;
    std::vector<NativeRange> ranges;
    std::vector<RunStrip> strips;
    std::vector<VisRun> vis;
};
struct ScModel {
    u32 model, matrix, flags;               // the table's entry: the matrix 0 is the world's own, the flags eModel::Render's
    bool lit, planned;
    u8 result;                              // DetermineVisibleStrips's
    u16 slot;
    u32 item0, items, vis0, vis_n, strips;
};
struct PlanEnv {
    u32 view, culling_off, opaque_off, default_material;
};
static std::vector<PlanSlot> g_plan;
static std::vector<ScModel> g_sc;
static u64 g_sc_calls, g_sc_models, g_sc_planned, g_sc_checked, g_sc_bad;
static const bool kSceneryDefault = true;   // on since 2026-11-24: the five demos in step, 455 calls the same as the game's loop to the byte

static bool scenery_on()
{
    static const bool on = getenv("HP2_SCENERY") ? atoi(getenv("HP2_SCENERY")) != 0 : kSceneryDefault;
    return on && direct_on();
}

// A model's plan. Reads the game's memory and the tables by address, writes its slot only.
static void plan_model(ScModel &m, const PlanEnv &env, int slot)
{
    PlanSlot &ps = g_plan[(size_t)slot];
    m.slot = (u16)slot;
    m.item0 = (u32)ps.items.size();
    m.vis0 = (u32)ps.vis.size();
    m.items = m.vis_n = m.strips = 0;
    m.result = 0;
    const u32 solid = LW(m.model + 12);
    const s32 n = (s32)(s16)LH(solid + 52);
    if (m.flags & 4) {
        // DetermineVisibleStrips(.., 2): SetAllStripsVisible, one run of them all
        ps.vis.push_back({n ? LW(solid + 176) : 0, 2, (u16)n});
        m.result = 1;
    } else {
        // DetermineVisibleStrips(.., 1), as vis_ahead has it
        const u32 matrix = m.matrix ? m.matrix : 0x2dc380u;
        float lo[3], hi[3], screen[16];
        memcpy(lo, host(solid + 64), 12);
        memcpy(hi, host(solid + 80), 12);
        hot_mul_matrix(screen, matrix, LW(env.view + 80));
        u32 klass = hot_classify_aabb_host(lo, hi, screen);
        if (klass == 1) {
            float clip[16];
            hot_mul_matrix(clip, matrix, LW(env.view + 72));
            klass = hot_classify_aabb_host(lo, hi, clip);
        }
        if (klass == 2) {
            ps.vis.push_back({n ? LW(solid + 176) : 0, 2, (u16)n});
            m.result = 1;
        } else if (klass == 1) {
            u32 strip = LW(solid + 176);
            for (s32 i = 0; i < n; i++, strip += 64) {
                float slo[3], shi[3];
                memcpy(slo, host(strip + 16), 12);
                memcpy(shi, host(strip + 32), 12);
                const u32 c = hot_classify_aabb_host(slo, shi, screen);
                if (c) ps.vis.push_back({strip, (u16)c, 1});
            }
            m.result = 1;
        }
    }
    m.vis_n = (u32)ps.vis.size() - m.vis0;
    if (!m.result) return;
    // RenderStrips, what it decides of each strip (render_strips_native, statement for statement; the flags here are 0 or 4, no blend)
    u32 mask = env.culling_off ? 256 : 0;
    if (!m.lit) mask |= 448;
    const u32 textures = LW(solid + 184), materials = LW(solid + 188);
    const s32 material_n = (s32)(s16)LH(solid + 174);
    for (u32 r = 0; r < m.vis_n; r++) {
        const VisRun run = ps.vis[m.vis0 + r];
        if (!run.strip) break;
        for (u32 k = 0; k < run.count; k++) {
            const u32 strip = run.strip + k * 64;
            m.strips++;
            if (k + 1 < run.count) __builtin_prefetch(strip_seen(LW(strip + 64 + 8)));
            const u32 info = LW(textures + ((u32)(s32)(s8)LB(strip) << 3) + 4);
            const u32 data = LW(strip + 8);
            const u32 strip_flags = LH(strip + 14), qwords = LH(strip + 12);
            const bool alpha = (s8)LB(info + 77) != 0;
            const u32 tex_bit = (s8)LB(info + 116) ? 4 : 0;
            if (strip_flags & 1024) continue;
            if (env.opaque_off && alpha) continue;
            u32 material = env.default_material;
            if (materials) {
                const s32 index = (s8)LB(strip + 31);
                if (index >= 0 && index < material_n) material = LW(materials + ((u32)index << 3) + 4);
            }
            const u32 parameter = (strip_flags | tex_bit | (run.klass == 1 ? 512 : 0)) & ~mask;
            PlanItem it = {};
            it.parameter = (u16)parameter;
            it.info = info;
            it.material = material;
            it.strip = strip;
            it.data = data;
            it.qwords = qwords;
            if (alpha) {
                it.kind = PLAN_ALPHA;
                ps.items.push_back(it);
                continue;
            }
            const StripSeen *seen = strip_seen(data);
            const u64 stamp = memwatch_stamp(data, qwords * 16);
            if (!(seen->fast && seen->data == data && seen->qwords == qwords && seen->generation == g_strip_generation && seen->stamp == stamp)) {
                it.kind = PLAN_STRIP;
                ps.items.push_back(it);
                continue;
            }
            PlanItem *last = ps.items.size() > m.item0 ? &ps.items.back() : nullptr;
            if (!last || last->kind != PLAN_RUN || last->info != info || last->material != material || last->parameter != (u16)parameter || last->run.strips >= 60000) {
                it.kind = PLAN_RUN;
                it.run.range0 = (u32)ps.ranges.size();
                it.run.strip0 = (u32)ps.strips.size();
                it.run.generation = g_strip_generation;
                ps.items.push_back(it);
                last = &ps.items.back();
            }
            DirectRun &out = last->run;
            const CachedStrip &at = seen->strip;
            if (out.ranges && ps.ranges.back().first + ps.ranges.back().count == at.base) {
                ps.ranges.back().count += at.count;
            } else {
                ps.ranges.push_back({at.base, at.count});
                out.ranges++;
            }
            ps.strips.push_back({data, qwords});
            out.strips++;
            out.tris += at.count;
            out.verts += at.n;
            out.sum += (stamp >> 32) + (u32)stamp;
        }
    }
    m.items = (u32)ps.items.size() - m.item0;
}

// A planned model into the lists: what DetermineVisibleStrips and RenderStrips leave in the game's memory for it.
static void replay_model(Ctx &c, const ScModel &m, u32 local_world, u32 light_context, u32 target_id, u32 view_matrix, u32 sp)
{
    const u32 gp = c.r[28].w[0];
    const PlanSlot &ps = g_plan[m.slot];
    // the table of the strips in sight, as RenderStrips leaves it: each run gone through, its count at 0
    const u32 table = 0x2d0c80;
    if (!m.result) {
        SW(table, 0);
        return;
    }
    const VisRun *vis = ps.vis.data() + m.vis0;
    for (u32 i = 0; i < m.vis_n; i++) {
        SW(table + i * 8, vis[i].strip);
        SH(table + i * 8 + 4, vis[i].klass);
        SH(table + i * 8 + 6, vis[i].strip ? 0 : vis[i].count);
    }
    SW(table + m.vis_n * 8, 0);
    const u32 alpha_list = 0x2c94a8 + target_id * 12, list_off = (target_id << 5) + 16;
    u32 last_active = 0;
    const PlanItem *items = ps.items.data() + m.item0;
    for (u32 i = 0; i < m.items; i++) {
        const PlanItem &it = items[i];
        const bool alpha = it.kind == PLAN_ALPHA;
        u32 active = LW(it.info + 152);
        if (!active) {
            g_rs_made++;
            rs_arg(c, 4, it.info);
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
                rs_arg(c, 5, it.strip + 16);
                rs_arg(c, 6, it.strip + 32);
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
        if (!list) continue;
        if (active != last_active || alpha) {
            rs_set(gp, list, 16, 0x2c85f0, local_world, EDMA_SET_LOCAL_WORLD, 4, 840);
            if (light_context) rs_set(gp, list, 12, 0x2c85e0, light_context, EDMA_LIGHTCONTEXT, 18, 860);
            last_active = active;
        }
        rs_set(gp, list, 8, 0x2c85d0, it.material, EDMA_LIGHTMATERIAL, 4, 852);
        if (it.kind != PLAN_RUN) {
            rs_add_data(gp, list, EDMA_STRIP, it.data, it.qwords, it.parameter);
            continue;
        }
        // the run's other strips asked for the same material, each a call and a hit of SetLightMaterial's counters
        const u32 more = it.run.strips - 1;
        SW(0x2c85d0, LW(0x2c85d0) + more);
        SW(0x2c85d4, LW(0x2c85d4) + more);
        const RunStrip *strips = ps.strips.data() + it.run.strip0;
        if (rs_add_run(gp, list, (u32)g_runs.size(), it.run.strips, it.parameter)) {
            DirectRun run = it.run;
            run.range0 = (u32)g_run_ranges.size();
            run.strip0 = (u32)g_run_strips.size();
            g_run_ranges.insert(g_run_ranges.end(), ps.ranges.begin() + it.run.range0, ps.ranges.begin() + it.run.range0 + it.run.ranges);
            g_run_strips.insert(g_run_strips.end(), strips, strips + it.run.strips);
            g_runs.push_back(run);
            g_added_strips += it.run.strips;
            g_direct_strips += it.run.strips;
            g_direct_runs++;
        } else {
            for (u32 k = 0; k < it.run.strips; k++) rs_add_data(gp, list, EDMA_STRIP, strips[k].data, strips[k].qwords, it.parameter);
        }
    }
}

static void stuff_scenery_native(Ctx &c)
{
    const u32 self = c.r[4].w[0], view = c.r[5].w[0], gp = c.r[28].w[0], sp0 = c.r[29].w[0];
    const u64 ra = c.r[31].d[0];
    g_sc_calls++;
    // the view's part of the table: the cull info whose view this is, the first of them
    u32 from = 0, to = 0;
    const s32 infos = (s32)LW(self + 2080);
    for (s32 i = 0; i < infos; i++) {
        const u32 info = self + (u32)i * 208;
        if (LW(info + 128) != view) continue;
        from = LW(info + 136);
        to = LW(info + 140);
        break;
    }
    if (from >= to) return;
    rs_arg(c, 29, sp0 - 160);               // the game's frame: what is called stands under it
    c.r[31].d[0] = 0x10de08;
    F_00105838(c);                          // eGetCurrentRenderTarget
    const u32 target = c.r[2].w[0], target_id = LW(target);
    c.r[31].d[0] = 0x10de48;
    F_00107ca8(c);                          // eGetCurrentView
    const u32 view_matrix = LW(c.r[2].w[0] + 68);
    c.r[31].d[0] = 0x10de54;
    F_00116a68(c);                          // elGetDefaultLightMaterial
    PlanEnv env;
    env.view = view;
    env.culling_off = !LW(gp - 25040);
    env.opaque_off = LW(target + 84);
    env.default_material = c.r[2].w[0];
    const u32 world_light = LW(view + 112), eye = LW(view + 56);
    // 1. the table
    g_sc.clear();
    for (u32 e = from; e < to; e += 8) {
        const u32 word = LW(e);
        ScModel m = {};
        m.model = word & ~3u;
        m.matrix = LW(e + 4);
        m.flags = (word & 3) == 2 ? 4 : 0;
        const u32 solid = LW(m.model + 12);
        m.lit = !m.matrix ? world_light != 0 : solid && (LH(solid + 50) & 0x1c0);
        m.planned = solid && !(LW(solid + 168) && LH(m.model + 22)) && !LW(solid + 44) && !(LH(solid + 50) & 4096) && !LW(m.model + 16);
        g_sc.push_back(m);
    }
    // 2. the plans
    const size_t n = g_sc.size(), slots = (size_t)cores_workers() + 1;
    if (g_plan.size() < slots) g_plan.resize(slots);
    for (PlanSlot &ps : g_plan) ps.items.clear(), ps.ranges.clear(), ps.strips.clear(), ps.vis.clear();
    if (g_strip_seen.empty()) strip_seen(0);        // the table made here, not by two workers at once
    if (n >= 48 && cores_workers()) {
        const size_t piece = 24;
        jobs_each((int)((n + piece - 1) / piece), [&](int at, int slot) {
            for (size_t k = (size_t)at * piece, end = std::min(n, k + piece); k < end; k++)
                if (g_sc[k].planned) plan_model(g_sc[k], env, slot);
        });
    } else {
        for (ScModel &m : g_sc)
            if (m.planned) plan_model(m, env, 0);
    }
    // The textures the plans name that nothing has asked for lately (a race's first frame: every texture of the track at once,
    // decoded one after another as the lists were read, 50 ms for 179 of them): decoded by the workers now (native_tex.cpp)
    {
        static std::vector<uint32_t> fresh;
        fresh.clear();
        for (const ScModel &m : g_sc) {
            if (!m.planned) continue;
            const PlanItem *items = g_plan[m.slot].items.data() + m.item0;
            for (u32 i = 0; i < m.items; i++) {
                const u32 info = items[i].info;
                if (native_texture_recent(info, g_frame_no) || fresh.size() >= 1024) continue;
                bool had = false;
                for (size_t k = fresh.size(); k-- > 0 && !had;) had = fresh[k] == info;
                if (!had) fresh.push_back(info);
            }
        }
        if (fresh.size() >= 8) native_textures_ahead(fresh.data(), fresh.size(), g_frame_no);
    }
    // 3. the table's order: the light context of a lit model with a matrix of its own, then the model
    for (const ScModel &m : g_sc) {
        u32 local = m.matrix, light = 0;
        if (!m.matrix) {
            local = 0x2dc380;
            light = world_light;
        } else if (m.lit) {
            const u32 pos = LW(gp - 19388), end = LW(gp - 19384), next = pos + 288;
            if (!((u64)(s64)(s32)next < (u64)(s64)(s32)end)) {      // FRAME_MALLOC's failure: the model is left out
                SW(gp - 19372, 1);
                SW(gp - 19368, LW(gp - 19368) + 288);
                continue;
            }
            SW(gp - 19388, next);
            if (!pos) continue;
            rs_arg(c, 4, pos);
            rs_arg(c, 5, m.matrix);
            rs_arg(c, 6, 0x2d70f0);
            rs_arg(c, 7, eye + 64);
            rs_arg(c, 8, view);
            c.r[31].d[0] = 0x150fcc;
            F_00115978(c);
            light = pos;
        }
        g_sc_models++;
        if (m.planned) {
            g_sc_planned++;
            rs_arg(c, 29, sp0 - 160 - 80 - 256);        // RenderStrips's frame under eModel::Render's: its room for a depth
            replay_model(c, m, local, light, target_id, view_matrix, sp0 - 160 - 80 - 256);
            rs_arg(c, 29, sp0 - 160);
        } else {
            rs_arg(c, 4, m.model);
            rs_arg(c, 5, local);
            rs_arg(c, 6, light);
            rs_arg(c, 7, m.flags);
            c.r[31].d[0] = 0x150fe0;
            F_0010dc80(c);
        }
    }
    rs_arg(c, 29, sp0);
    c.r[31].d[0] = ra;
}

static void native_scenery_log()
{
    fprintf(g_hle_log, "native: scenery by the port's loop%s: %llu calls, %llu models, %llu of them planned (workers: %d); stuffcheck: %llu compared, %llu not the same\n",
            scenery_on() ? "" : " (off)", (unsigned long long)g_sc_calls, (unsigned long long)g_sc_models, (unsigned long long)g_sc_planned, cores_workers(),
            (unsigned long long)g_sc_checked, (unsigned long long)g_sc_bad);
}

// Both loops on the same memory (as render_strips_check): the game's with the direct output first, everything put back, then the
// port's; the game's 32 MB, the PC's memory past them, the scratchpad and the runs they made compared whole.
// The game's memory as it was, the pages that differ only: a page written though it is the same would lose its watch (memwatch.h),
// and every strip on it would be unknown by its address to whatever runs next.
static void put_back(const u8 *from, size_t total)
{
    for (size_t at = 0; at < total; at += 4096) {
        const size_t n = std::min<size_t>(4096, total - at);
        if (memcmp(g_ram + at, from + at, n)) memcpy(g_ram + at, from + at, n);
    }
}

static void stuff_scenery_check(Ctx &c, void (*orig)(Ctx &))
{
    static std::vector<u8> before, after;
    static u8 spr_before[0x4000], spr_after[0x4000];
    const size_t total = 0x02000000 + (size_t)g_ext_size;
    before.resize(total);
    after.resize(total);
    const Ctx c0 = c;
    const u32 sp = c.r[29].w[0], strips0 = g_added_strips;
    const size_t runs0 = g_runs.size(), ranges0 = g_run_ranges.size(), rstrips0 = g_run_strips.size();
    const u64 dstrips0 = g_direct_strips, druns0 = g_direct_runs;
    const bool eie = g_eie;
    g_eie = false;
    memcpy(before.data(), g_ram, total);
    memcpy(spr_before, g_spr, sizeof spr_before);
    orig(c);
    const Ctx c1 = c;
    memcpy(after.data(), g_ram, total);
    memcpy(spr_after, g_spr, sizeof spr_after);
    const std::vector<DirectRun> runs(g_runs.begin() + (long long)runs0, g_runs.end());
    const std::vector<NativeRange> ranges(g_run_ranges.begin() + (long long)ranges0, g_run_ranges.end());
    const std::vector<RunStrip> rstrips(g_run_strips.begin() + (long long)rstrips0, g_run_strips.end());
    const u32 strips1 = g_added_strips;
    const u64 dstrips1 = g_direct_strips, druns1 = g_direct_runs;
    put_back(before.data(), total);
    memcpy(g_spr, spr_before, sizeof spr_before);
    g_runs.resize(runs0);
    g_run_ranges.resize(ranges0);
    g_run_strips.resize(rstrips0);
    g_added_strips = strips0;
    g_direct_strips = dstrips0;
    g_direct_runs = druns0;
    c = c0;
    stuff_scenery_native(c);
    g_sc_checked++;
    const u8 *stack = host(sp);
    auto dead = [&](const u8 *p) { return stack && p < stack && p >= stack - 0x1800; };
    u32 differ = 0;
    auto compare = [&](const u8 *mine, const u8 *game, size_t bytes, u32 guest, const char *name) {
        if (!memcmp(mine, game, bytes)) return;
        for (size_t i = 0; i < bytes; i += 4) {
            if (!memcmp(mine + i, game + i, 4) || dead(mine + i)) continue;
            if (differ++ < 8) {
                u32 a, b;
                memcpy(&a, mine + i, 4), memcpy(&b, game + i, 4);
                fprintf(g_hle_log, "stuffcheck: %s %08x: the port's %08x, the game's %08x\n", name, guest + (u32)i, a, b);
            }
        }
    };
    compare(g_ram, after.data(), total, 0, "memory");
    compare(g_spr, spr_after, sizeof spr_after, 0x70000000, "scratchpad");
    for (int r : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
        if (c.r[r].d[0] != c1.r[r].d[0]) differ++, fprintf(g_hle_log, "stuffcheck: register %d: the port's %08x, the game's %08x\n", r, c.r[r].w[0], c1.r[r].w[0]);
    bool same = g_runs.size() - runs0 == runs.size() && g_run_ranges.size() - ranges0 == ranges.size() && g_run_strips.size() - rstrips0 == rstrips.size() &&
                g_added_strips == strips1 && g_direct_strips == dstrips1 && g_direct_runs == druns1;
    for (size_t i = 0; same && i < runs.size(); i++) {
        const DirectRun &a = g_runs[runs0 + i], &b = runs[i];
        same = a.range0 == b.range0 && a.ranges == b.ranges && a.strip0 == b.strip0 && a.strips == b.strips && a.tris == b.tris && a.verts == b.verts &&
               a.generation == b.generation && a.sum == b.sum;
    }
    for (size_t i = 0; same && i < ranges.size(); i++) same = g_run_ranges[ranges0 + i].first == ranges[i].first && g_run_ranges[ranges0 + i].count == ranges[i].count;
    for (size_t i = 0; same && i < rstrips.size(); i++) same = g_run_strips[rstrips0 + i].data == rstrips[i].data && g_run_strips[rstrips0 + i].qwords == rstrips[i].qwords;
    if (!same) differ++, fprintf(g_hle_log, "stuffcheck: the runs: the port's %zu with %zu ranges, the game's loop's %zu with %zu\n", g_runs.size() - runs0,
                                 g_run_ranges.size() - ranges0, runs.size(), ranges.size());
    if (differ) g_sc_bad++;
    if (differ || g_sc_checked % 200 == 0)
        fprintf(g_hle_log, "stuffcheck: %llu calls compared, %llu not the same (this one: %u differences, %zu models, %zu runs)\n", (unsigned long long)g_sc_checked,
                (unsigned long long)g_sc_bad, differ, g_sc.size(), runs.size()),
            fflush(g_hle_log);
    // the game goes on with its own loop's outcome
    put_back(after.data(), total);
    memcpy(g_spr, spr_after, sizeof spr_after);
    g_runs.resize(runs0);
    g_run_ranges.resize(ranges0);
    g_run_strips.resize(rstrips0);
    g_runs.insert(g_runs.end(), runs.begin(), runs.end());
    g_run_ranges.insert(g_run_ranges.end(), ranges.begin(), ranges.end());
    g_run_strips.insert(g_run_strips.end(), rstrips.begin(), rstrips.end());
    g_added_strips = strips1;
    g_direct_strips = dstrips1;
    g_direct_runs = druns1;
    c = c1;
    g_eie = eie;
}

// ---- do the cars depend on one another within a phase of the world's step? (a diagnostic, 2026-11-24) -------------------------
// World::DoTimestep (0x14dcd8) runs each of its phases as a loop over the cars: Car::Think, Drive, BeginMovement, FinishMovement.
// Whether such a loop could be given to several threads is first of all whether its calls depend on their order. HP2_ORDERCHECK=<n>:
// every n-th step, when a phase's loop comes to its first car, the whole loop is run here in the game's order, the memory put back,
// run again in the reverse order, and the two outcomes compared: every word that differs is something one car's call leaves for
// another's, or a place they all write (a scratch buffer, a counter). The memory is put back once more and the game runs its loop
// as ever. The words are counted by address and written to the log at the end of each 20 comparisons.
// The port's own state outside the game's memory (the hooks' statics, the sound's queue) is not put back: the run is a
// diagnostic's, its replay may go out of step.
void native_order_check(Ctx &c, void (*orig)(Ctx &), int which)
{
    static const int every = getenv("HP2_ORDERCHECK") ? atoi(getenv("HP2_ORDERCHECK")) : 0;
    if (every <= 0) return;
    static bool inside;
    if (inside) return;
    const u32 world = c.r[17].w[0];         // the caller's s1
    if (!world || world >= 0x02000000) return;
    // the cars of the phase, in the loop's order: Think goes by four parts of the list (+32, +36, +40), the others from 0 to +28
    const s32 all = (s32)LW(world + 28), a = (s32)LW(world + 32), b = (s32)LW(world + 36), d = (s32)LW(world + 40);
    if (all < 2 || all > 256) return;
    std::vector<u32> cars;
    auto take = [&](s32 from, s32 to) {
        for (s32 i = from; i < to; i++) cars.push_back(LW(world + 44 + (u32)i * 4));
    };
    if (which == 0) take(a, b), take(d, all), take(0, a), take(b, d);
    else take(0, all);
    if (cars.size() < 2 || c.r[4].w[0] != cars[0]) return;
    static const char *const kName[4] = {"Think", "Drive", "BeginMovement", "FinishMovement"};
    static u64 steps[4], done[4], differed[4];
    if (steps[which]++ % (u64)every) return;
    static std::unordered_map<u32, u32> places[4];
    static std::vector<u8> before, forward;
    static u8 spr_before[0x4000], spr_forward[0x4000];
    const size_t total = 0x02000000 + (size_t)g_ext_size;
    before.resize(total);
    forward.resize(total);
    const Ctx c0 = c;
    const bool eie = g_eie;
    g_eie = false;
    inside = true;
    memcpy(before.data(), g_ram, total);
    memcpy(spr_before, g_spr, sizeof spr_before);
    for (size_t i = 0; i < cars.size(); i++) {
        c = c0;
        c.r[4].d[0] = (u64)(s64)(s32)cars[i];
        orig(c);
    }
    memcpy(forward.data(), g_ram, total);
    memcpy(spr_forward, g_spr, sizeof spr_forward);
    put_back(before.data(), total);
    memcpy(g_spr, spr_before, sizeof spr_before);
    for (size_t i = cars.size(); i-- > 0;) {
        c = c0;
        c.r[4].d[0] = (u64)(s64)(s32)cars[i];
        orig(c);
    }
    const u8 *stack = host(c0.r[29].w[0]);
    u32 differ = 0;
    for (size_t at = 0; at < total; at += 4096) {
        if (!memcmp(g_ram + at, forward.data() + at, std::min<size_t>(4096, total - at))) continue;
        for (size_t i = at; i < at + 4096 && i < total; i += 4) {
            if (!memcmp(g_ram + i, forward.data() + i, 4)) continue;
            if (stack && g_ram + i < stack && g_ram + i >= stack - 0x4000) continue;       // the dead stack under the caller
            differ++;
            if (places[which].size() < 20000 || places[which].count((u32)i)) places[which][(u32)i]++;
        }
    }
    for (size_t i = 0; i < sizeof spr_before; i += 4)
        if (memcmp(g_spr + i, spr_forward + i, 4) && !(stack && g_spr + i < stack && g_spr + i >= stack - 0x4000)) differ++, places[which][0x70000000u + (u32)i]++;
    done[which]++;
    differed[which] += differ != 0;
    put_back(before.data(), total);
    memcpy(g_spr, spr_before, sizeof spr_before);
    c = c0;
    g_eie = eie;
    inside = false;
    if (done[which] % 20 == 0) {
        fprintf(g_hle_log, "ordercheck: %s: %llu loops of %zu cars run both ways, %llu came out differently; %zu places in all (this loop: %u words)\n", kName[which],
                (unsigned long long)done[which], cars.size(), (unsigned long long)differed[which], places[which].size(), differ);
        std::vector<std::pair<u32, u32>> top(places[which].begin(), places[which].end());
        std::sort(top.begin(), top.end(), [](const std::pair<u32, u32> &x, const std::pair<u32, u32> &y) { return x.second > y.second; });
        const u32 gp = c.r[28].w[0];
        for (size_t i = 0; i < top.size() && i < 60; i++) {
            const u32 at = top[i].first;
            char what[64] = "";
            for (size_t k = 0; k < cars.size(); k++)
                if (at >= cars[k] && at < cars[k] + 0x900) snprintf(what, sizeof what, " (car %zu +%u)", k, at - cars[k]);
            if (!what[0] && at + 0x8000 >= gp && at < gp + 0x8000) snprintf(what, sizeof what, " (gp%+d)", (int)(at - gp));
            fprintf(g_hle_log, "ordercheck: %s:   %08x%s  %u times\n", kName[which], at, what, top[i].second);
        }
        fflush(g_hle_log);
    }
}

void HOOK_StuffScenery__20GrandSceneryCullInfoP5eView(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NOVISAHEAD") != nullptr;
    Phase timed(PH_SCENERY);
    g_vis_on = false;
    if (scenery_on()) {
        static const int check = getenv("HP2_STUFFCHECK") ? atoi(getenv("HP2_STUFFCHECK")) : 0;
        static u64 calls;
        if (check > 0 && ++calls % (u64)check == 0) stuff_scenery_check(c, orig);
        else stuff_scenery_native(c);
        return;
    }
    if (!off && cores_workers()) {
        const u32 self = c.r[4].w[0], view = c.r[5].w[0], gp = c.r[28].w[0];
        g_vis.clear();
        // the view's part of the table, as StuffScenery finds it: the cull info whose view this is
        const s32 infos = (s32)LW(self + 2080);
        for (s32 i = 0; i < infos; i++) {
            const u32 info = self + (u32)i * 208;
            if (LW(info + 128) != view) continue;
            for (u32 e = LW(info + 136), end = LW(info + 140); e < end; e += 8) {
                const u32 word = LW(e), model = word & ~3u;
                if ((word & 3) == 2) continue;              // wholly in sight: nothing to work out
                const u32 solid = LW(model + 12);
                if (!solid || (LH(solid + 50) & 4096) || LW(solid + 44) || (LW(solid + 168) && LH(model + 22))) continue;
                const u32 matrix = LW(e + 4);
                g_vis.push_back({model, matrix ? matrix : 0x2dc380u, 0, 0, 0, 0, false});
            }
            break;
        }
        if (g_vis.size() >= 48 && LW(gp - 24892) == view) {
            for (auto &r : g_vis_runs) r.clear();
            const size_t n = g_vis.size(), piece = 24;
            jobs_each((int)((n + piece - 1) / piece), [&](int at, int slot) {
                const size_t from = (size_t)at * piece, to = std::min(n, from + piece);
                for (size_t k = from; k < to; k++) vis_ahead(g_vis[k], view, slot & 15);
            });
            g_vis_at = 0;
            g_vis_on = true;
        }
    }
    orig(c);
    g_vis_on = false;
}

void HOOK_DetermineVisibleStrips__6eModelPC8bMatrix4ii(Ctx &c, void (*orig)(Ctx &))
{
    Phase timed(PH_VISIBILITY);
    static const bool check = getenv("HP2_VISCHECK") != nullptr;
    const VisAhead *v = nullptr;
    if (g_vis_on && !c.r[6].w[0] && c.r[7].w[0] == 1) {
        // the game asks in the table's order; a model not taken over is simply not in the list
        const u32 model = c.r[4].w[0], matrix = c.r[5].w[0];
        for (size_t k = g_vis_at, end = std::min(g_vis.size(), g_vis_at + 16); k < end; k++)
            if (g_vis[k].model == model && g_vis[k].matrix == matrix && g_vis[k].ready) {
                v = &g_vis[k];
                g_vis_at = k + 1;
                break;
            }
        if (!v) g_vis_missed++;
    }
    if (g_phases_on) {      // TEMP (HP2_PHASES): how many of the calls find their answer ahead
        static u64 calls, found, at;
        calls++;
        found += v != nullptr;
        if (g_frame_no - at >= 900 && pc_racing()) {
            fprintf(g_hle_log, "visahead: %llu calls a frame, %llu of them answered ahead\n", calls / 900, found / 900);
            calls = found = 0;
            at = g_frame_no;
        }
    }
    if (!v) {
        orig(c);
        return;
    }
    const u32 table = 0x2d0c80;
    const VisRun *runs = g_vis_runs[v->slot].data() + v->first;
    if (check) {
        orig(c);
        g_vis_checked++;
        bool same = c.r[2].w[0] == v->result;
        if (same && v->result) {
            for (u32 i = 0; i < v->count && same; i++)
                same = LW(table + i * 8) == runs[i].strip && LH(table + i * 8 + 4) == runs[i].klass && LH(table + i * 8 + 6) == runs[i].count;
            // SetAllStripsVisible of a model without strips leaves {0, class, 0}: its first word is a zero as the end is
            same = same && LW(table + v->count * 8) == 0;
        } else if (same) {
            same = LW(table) == 0;
        }
        if (!same && g_vis_bad++ < 10)
            fprintf(g_hle_log, "vischeck: model %08x matrix %08x: the port's %u runs (result %u), the game's result %u, first words %08x %08x\n", c.r[4].w[0],
                    c.r[5].w[0], v->count, v->result, c.r[2].w[0], LW(table), LW(table + 8));
        if (g_vis_checked % 20000 == 0)
            fprintf(g_hle_log, "vischeck: %llu answers compared with the game's, %llu differ; %llu calls not found ahead\n", (unsigned long long)g_vis_checked,
                    (unsigned long long)g_vis_bad, (unsigned long long)g_vis_missed);
        return;
    }
    g_vis_used++;
    if (!v->result) {
        SW(table, 0);
        c.r[2].d[0] = 0;
        return;
    }
    for (u32 i = 0; i < v->count; i++) {
        SW(table + i * 8, runs[i].strip);
        SH(table + i * 8 + 4, runs[i].klass);
        SH(table + i * 8 + 6, runs[i].count);
    }
    SW(table + v->count * 8, 0);
    c.r[2].d[0] = 1;
}

static void cull_view_native(Ctx &c)
{
    const u32 self = c.r[4].w[0], cull = c.r[5].w[0], sp0 = c.r[29].w[0], frame = sp0 - 336;
    const u64 ra = c.r[31].d[0];
    // the view's sections, by the game (and HOOK_WhatSectionsShouldWeDraw's more of them): numbers, 128 at the most, on the stack
    rs_arg(c, 29, frame);
    rs_arg(c, 4, self);
    rs_arg(c, 5, frame);
    rs_arg(c, 6, 128);
    rs_arg(c, 7, cull);
    c.r[31].d[0] = 0x150b3c;
    F_00150950(c);
    const s32 count = (s32)c.r[2].w[0];
    size_t n = 0;
    for (s32 i = 0; i < count && i < 128; i++) {
        const s32 number = (s32)(s16)LH(frame + (u32)i * 2);
        if (number < 0) continue;
        const u32 header = LW(0x2f8fe8 + (u32)number * 4);
        if (!header || !LW(header + 36)) continue;
        if (g_cull_sections.size() <= n) g_cull_sections.emplace_back();
        g_cull_sections[n++].header = header;
    }
    // the size's H: larger for a player's view by the draw distance (HOOK_TreeCull)
    u32 h_bits = LW(cull + 192);
    const bool player = player_view(cull);
    if (g_cfg.draw_distance != 100 && player) {
        float f;
        memcpy(&f, &h_bits, 4);
        f *= g_cfg.draw_distance * 0.01f;
        memcpy(&h_bits, &f, 4);
    }
    const bool max_detail = max_detail_now();
    g_cull_views++;
    g_cull_sections_n += n;
    if (player && n >= 6 && cores_workers()) {
        g_cull_parallel++;
        jobs_each((int)n, [&](int i, int slot) {
            Ctx &w = g_cull_ctx[slot & 15];
            w = c;
            const bool counting = t_das_count;
            t_das_count = false;
            cull_section(w, g_cull_sections[(size_t)i], cull, h_bits, max_detail);
            t_das_count = counting;
        });
    } else {
        Ctx &w = g_cull_ctx[0];
        w = c;
        for (size_t i = 0; i < n; i++) cull_section(w, g_cull_sections[i], cull, h_bits, max_detail);
    }
    // what the game's memory gets, in the game's order
    rs_arg(c, 29, frame - 128);
    for (size_t i = 0; i < n; i++) {
        if (g_irq_pending) ps2_take_interrupts(c);
        g_cull_kept += g_cull_sections[i].kept.size();
        for (const CullKept &k : g_cull_sections[i].kept) cull_keep(c, cull, k, frame - 128);
    }
    rs_arg(c, 29, sp0);
    c.r[31].d[0] = ra;
}

static void cull_view(Ctx &c, void (*orig)(Ctx &))
{
    static const bool off = getenv("HP2_NOCULLNATIVE") != nullptr || getenv("HP2_NODASNATIVE") != nullptr || getenv("HP2_DASCHECK") != nullptr;
    static const int check = getenv("HP2_CULLCHECK") ? atoi(getenv("HP2_CULLCHECK")) : 0;
    if (off || g_lod_log) {
        orig(c);
        return;
    }
    if (check > 0) {
        static u64 calls;
        if (++calls % (u64)check == 0) {
            native_ab_check(c, orig, cull_view_native, "CullView");
            return;
        }
    }
    cull_view_native(c);
}

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
    const unsigned long long cull_t0 = g_phases_on ? __rdtsc() : 0;
    cull_view(c, orig);
    const u32 cur = LW(cull + 140);
    if (g_phases_on) {      // TEMP (HP2_PHASES): the culling by view, for stage 3 of the plan for more than one core
        static unsigned long long tsc[16], kept[16], calls[16], frames_at;
        const u32 view = LW(cull + 128), id = view ? LW(view) & 15 : 0;
        tsc[id] += __rdtsc() - cull_t0;
        kept[id] += (cur - before_cur) / 8;
        calls[id]++;
        if (g_frame_no - frames_at >= 900 && pc_racing()) {
            unsigned long long all = 0;
            for (int i = 0; i < 16; i++) all += tsc[i];
            fprintf(g_hle_log, "cullviews: %u frames:", (unsigned)(g_frame_no - frames_at));
            for (int i = 0; i < 16; i++)
                if (calls[i]) fprintf(g_hle_log, " view %d: %.1f %% of the culling, %llu calls, %llu objects kept a call;", i, 100.0 * (double)tsc[i] / (double)(all ? all : 1), calls[i], kept[i] / calls[i]);
            fprintf(g_hle_log, "\n");
            memset(tsc, 0, sizeof tsc), memset(kept, 0, sizeof kept), memset(calls, 0, sizeof calls);
            frames_at = g_frame_no;
        }
    }
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
static u32 g_frame_mem_peak;                // the most a frame took of its half, for the limits' log

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
    if (g_frame_ext) {                                      // what this frame took of its half (the limits' log)
        const u32 size = LW(gp - 19392), pos = LW(gp - 19388), end = LW(gp - 19384);
        if (end >= pos && end - pos <= size) {
            const u32 used = size - (end - pos) + (LW(gp - 19372) ? LW(gp - 19368) : 0);
            if (used > g_frame_mem_peak) g_frame_mem_peak = used;
        }
    }
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

// ---- the limits' log ----------------------------------------------------------------------------------
// Every buffer and table of a fixed size, the port's and the game's: how full it got at its fullest, against its size. One line
// in speed.log each 1800 frames of a race, and only when one of them has got fuller than the last line said (the user,
// 2026-11-17; step 1 of the plan for mods with more in them, handoff.md). What each is and what happens past its end is in
// handoff.md, "Опись буферов". The renderer's own come from its thread (g_native_limits); those that grow show the size they
// have grown to.
static void limits_frame(Ctx &c)
{
    if (!pc_racing()) return;
    static u32 frames, cache_peak, cars_peak, heap_free = ~0u, heap_largest = ~0u;
    const u32 gp = c.r[28].w[0];
    if (g_cache_top > cache_peak) cache_peak = g_cache_top;
    if (g_shadow_cars.size() > cars_peak) cars_peak = (u32)g_shadow_cars.size();
    if (frames % 64 == 0) {                 // the game's heap: bCountFreeMemory, bLargestMalloc (they only read)
        const u32 free_now = pc_call(0x257988, 0), largest = pc_call(0x2579b0, 0);
        if (free_now < heap_free) heap_free = free_now;
        if (largest < heap_largest) heap_largest = largest;
    }
    if (++frames % 1800) return;
    struct Row {
        const char *name;
        u64 used, cap;
        int kind;                           // 0 a count, 1 bytes (shown in MB), 2 bytes of which the least left counts
    };
    // the game's slot pools of particles (afxInit): MostNumAllocatedSlots and TotalNumSlots
    u64 pool_used[3] = {}, pool_cap[3] = {};
    for (int i = 0; i < 3; i++)
        if (const u32 pool = LW(gp - 23528 + (u32)i * 4)) pool_used[i] = LW(pool + 0x1c), pool_cap[i] = LW(pool + 0x2c);
    const auto lim = [](int id, bool cap) { return cap ? g_native_limits[id].cap.load() : g_native_limits[id].peak.load(); };
    const Row rows[] = {
        {"frame memory", g_frame_mem_peak, g_frame_ext ? LW(gp - 19392) : 0, 1},
        {"scenery table", g_draw_table_peak, kDrawTableEntries, 0},
        {"vertex cache", cache_peak, g_cache_cap, 0},
        {"own vertices", lim(NL_OWN_VERTS, false), lim(NL_OWN_VERTS, true), 1},
        {"lights", lim(NL_LIGHTS, false), lim(NL_LIGHTS, true), 0},
        {"merged draws", lim(NL_MERGE_DRAWS, false), lim(NL_MERGE_DRAWS, true), 0},
        {"merged ranges", lim(NL_MERGE_CMDS, false), lim(NL_MERGE_CMDS, true), 0},
        {"textures", lim(NL_TEXTURES, false), lim(NL_TEXTURES, true), 0},
        {"texture staging", lim(NL_STAGING, false), lim(NL_STAGING, true), 1},
        {"depth read", lim(NL_DEPTH, false), lim(NL_DEPTH, true), 1},
        {"palette rows", lim(NL_PALETTES, false), lim(NL_PALETTES, true), 0},
        {"shadow cells", cars_peak, (u64)kShadowCells, 0},
        {"heap least free", heap_free, 0, 2},
        {"heap least largest block", heap_largest, 0, 2},
        {"particles", pool_used[0], pool_cap[0], 0},
        {"particle groups", pool_used[1], pool_cap[1], 0},
        {"emitters", pool_used[2], pool_cap[2], 0},
    };
    const int n = (int)(sizeof rows / sizeof rows[0]);
    static u64 told[sizeof rows / sizeof rows[0]];
    static bool first = true;
    bool news = first;
    for (int i = 0; i < n; i++) news |= rows[i].kind == 2 ? rows[i].used < told[i] : rows[i].used > told[i];
    if (!news) return;
    first = false;
    fprintf(g_hle_log, "limits: frame %u, draw distance %d, picture aspect %d:", g_frame_no, g_cfg.draw_distance, native_aspect());
    for (int i = 0; i < n; i++) {
        const Row &r = rows[i];
        const char *mark = r.kind != 2 && r.used > told[i] && told[i] ? " (up)" : "";
        if (r.kind == 0)
            fprintf(g_hle_log, " %s %llu of %llu%s;", r.name, (unsigned long long)r.used, (unsigned long long)r.cap, mark);
        else if (r.kind == 1)
            fprintf(g_hle_log, " %s %.2f of %.1f MB%s;", r.name, (double)r.used / 1048576.0, (double)r.cap / 1048576.0, mark);
        else
            fprintf(g_hle_log, " %s %.2f MB;", r.name, (double)r.used / 1048576.0);
        told[i] = r.used;
    }
    fprintf(g_hle_log, " the cache emptied %llu times\n", (unsigned long long)g_cache_flushes);
    fflush(g_hle_log);
}

void HOOK_eDisplayFrame__Fv(Ctx &c, void (*orig)(Ctx &))
{
    g_pc_ctx = &c;
    set_frame_rate(c);
    speed_log(c);
    pc_paint_buttons();         // the device changes in a race too
    g_frame.wide = native_narrow() < 1 && pc_racing();
    mid_frame();
    g_frame.doubled = g_double_now;
    g_frame.pictures = g_double_now ? pc_picture_rate() : 0;
    // Reflections at High go by the frame rate chosen (the user, 2026-11-14): at 120 the mirror and the cars' reflections are drawn
    // for the picture between too
    g_frame.mid_views = g_double_now && g_cfg.reflections == 2;
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
    {
        Phase timed(PH_DISPLAY);
        orig(c);
    }
    phase_view(-1);
    phase_frame();
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
    limits_frame(c);
    native_end_frame();
}
