// The renderer (work/gsvk/README.md): the game's draw lists on Vulkan, without VU1 and the GS.
#pragma once
void native_notice_saved();     // "SAVED" over the picture for a moment
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

struct NativeVertex {
    float x, y, z;
    uint8_t r, g, b, a;
    float s, t;
    int8_t nx, ny, nz;                      // VU1's normal, 127 is 1.0
    int8_t k;                               // TEX1's K (1/16 of a mip level), the strip's own
    float w;                                // 1 / Q of a GIF packet: the track map is perspective, in 2D
};

// a texture as RGBA8 with the GS's bytes (alpha 0x80 is 1.0), native_tex.cpp
// A growing array of plain values whose new room is handed out as it is: std::vector's resize clears it first, and the two million
// indices of a heavy frame were each written twice.
template <typename T> struct NativeRaw {
    T *p = nullptr;
    size_t n = 0, cap = 0;
    NativeRaw() = default;
    NativeRaw(const NativeRaw &) = delete;
    NativeRaw &operator=(const NativeRaw &) = delete;
    NativeRaw(NativeRaw &&o) noexcept : p(o.p), n(o.n), cap(o.cap) { o.p = nullptr, o.n = o.cap = 0; }
    NativeRaw &operator=(NativeRaw &&o) noexcept
    {
        if (this != &o) {
            free(p);
            p = o.p, n = o.n, cap = o.cap;
            o.p = nullptr, o.n = o.cap = 0;
        }
        return *this;
    }
    ~NativeRaw() { free(p); }
    size_t size() const { return n; }
    const T *data() const { return p; }
    void clear() { n = 0; }
    T *grow(size_t more)                    // room for `more` at the end, not cleared
    {
        if (n + more > cap) {
            cap = (n + more) * 2 > 65536 ? (n + more) * 2 : 65536;
            p = (T *)realloc(p, cap * sizeof(T));
        }
        T *at = p + n;
        n += more;
        return at;
    }
};

struct NativeTexture {
    int w = 0, h = 0;               // the GS's: 2^TW, 2^TH, what ST 1.0 means. The image's own size can be less
    int image_w = 0, image_h = 0;
    int levels = 1;                 // the game's own
    int gpu_levels = 1;             // what the GPU's image has: the game's, or all of them down to one texel (native_vk's more_levels)
    std::vector<uint32_t> px;       // while it is decoded: the levels one after another. They leave with its NativeTexUpload
    std::vector<uint8_t> font_alpha;  // the fonts' level 0: their glyphs' seams are looked for here (native.cpp)
    uint32_t sum = 0, frame = 0;
    // the pixels' and the palette's sum as last made, and what vouches that their bytes are still those (native_texture_now, memwatch.h)
    uint32_t raw_sum = 0, raw_bytes = 0;
    uint64_t raw_stamp = 0;
    bool raw_watched = false;
    bool repeat_s = false, repeat_t = false, mag_linear = true, min_linear = true, mip_linear = false;
    bool upscaled = false;          // a replacement larger than the game's: the GPU's filter, the GS's levels do not fit it
    bool loaded = false;            // its pixels were handed over for the GPU and the renderer has not let it go since (g_native_tex_mutex)
    void *vk = nullptr;             // native_vk.cpp's, read and written on the renderer's thread only
    char name[28] = {};             // the game's, for the logs
};

// A decoded texture on its way to the GPU. The pixels leave the game's thread with the frame that first draws them, so the renderer's
// thread never reads what the game's is writing; the sampler's settings are the ones the decoding had.
struct NativeTexUpload {
    NativeTexture *tex = nullptr;
    std::vector<uint32_t> px;       // the levels one after another
    int w = 0, h = 0, levels = 1;
    int gpu_levels = 1;             // what the GPU's image gets: the game's, or all of them down to one texel (native_vk's more_levels)
    bool repeat_s = false, repeat_t = false, mag_linear = true, min_linear = true, mip_linear = false;
};
// the game's thread stamps a texture it asks for and reads `loaded`, the renderer's lets one go by that stamp: both under this
extern std::mutex g_native_tex_mutex;
void native_texture_take_uploads(std::vector<NativeTexUpload> &out);    // what was decoded since the last call, in order
uint16_t native_tex_mode(const NativeTexture &t);                       // the sampling's bits a draw carries (NativeDraw::tex_mode)
void native_texture_watch_stats(uint64_t *vouched, uint64_t *summed, uint64_t *bad);

enum NativeBlend {
    NB_OPAQUE,      // ALPHA 0x0a: Cs
    NB_ALPHA,       // 0x44: (Cs - Cd) A + Cd
    NB_ADD,         // 0x48: Cs A + Cd
    NB_SUB,         // 0x42: Cd - Cs A
    NB_DEST,        // 0x58: Cs Ad + Cd, with the frame's alpha
    NB_LERP_DEST,   // 0x54: (Cs - Cd) Ad + Cd
    NB_KEEP,        // 0x40: Cd, and As into the frame's alpha
    NB_COUNT
};

enum { NATIVE_RT_MIRROR = 7, NATIVE_RT_PREVIOUS = 100 };   // the last frame, what DisplayBlendPrevious reads

// a run of triangles drawn with one matrix (the clip matrix: GS pixels around the target's middle)
struct NativeDraw {
    float clip[16];
    uint32_t first, count;
    bool depth_write;                       // bit 1 of the texture's mode (bWriteZ, eGetZWriteState)
    bool alpha_test;                        // bit 0 clear: alpha GREATER 0x30
    uint8_t blend;                          // NativeBlend
    float blend_fix;                        // the blend's factor if it is ALPHA's FIX (0x80 is 1.0), else -1
    float scroll_s, scroll_t;
    NativeTexture *tex;                     // null: none, or not one yet
    uint16_t tex_mode = 0;                  // its sampling as the frame's lists found it (native_tex_mode): the renderer reads these, not the
    bool tex_upscaled = false, tex_min_linear = false;      // texture, which the game's thread is by then decoding the next frame into
    int rt;                                 // >= 0: the texture is a picture of this renderer's, NATIVE_RT_*
    bool depth_test;                        // the target's ZTST is not ALWAYS
    float snap_x, snap_y;                   // half the target in GS pixels for what VU1 transforms, else 0
    float view[4];                          // where the target is in the picture, GS pixels (Player 2's half of the screen)
    uint8_t tfx;                            // TEX0's: 0 modulate, 1 decal, 2 highlight, 3 highlight2
    uint8_t lod_l;                          // TEX1's L
    uint8_t target;                         // the game's TARGET_ID: 1 the player's view, 8..13 the envmap views
    uint16_t flags;                         // vi07 of the strips: 0x80 lit, 0x40 road, 0x100 sphere map, 4 gloss map
    int light;                              // into NativeFrame::lights, -1: none
    bool indexed = false;                   // a strip's: first and count are into NativeFrame::indices, else into NativeFrame::vertices
};

// a run of strips new to the GPU's vertex cache: fresh[src, src + count) goes to the cache at dst
struct NativeFreshRun {
    uint32_t dst, src, count;
};

// What VU1's lighter had: 18 quads (eLightContext), then 4 (the material)
struct NativeLight {
    float q[22][4];
};

// what the game lays over the view between two draws, without geometry of its own
enum NativeFxKind { FX_FOG, FX_GREY, FX_DOF };
struct NativeFx {
    uint32_t at;                            // the draws before it in NativeFrame::draws are under it
    uint8_t kind;
    bool darken;                            // the fog's mode 1: Cd As
    float rect[4];                          // the view in the target's pixels: x, y, w, h over 512 x 448
    uint32_t palette[256];                  // by Z >> 8, RGBA with the GS's bytes
};

struct NativeDepthRect {
    int x, y, w, h;                         // GS pixels
};

struct NativeFrame {
    std::vector<NativeVertex> vertices;     // this frame's own: the 2D and the GIF packets, and strips the cache had no room for
    NativeRaw<uint32_t> indices;            // the strips': into the GPU's buffer, the cache below dyn_base, this frame's vertices from it
    std::vector<NativeVertex> fresh;        // the strips the cache gets this frame
    std::vector<NativeFreshRun> fresh_runs;
    uint32_t dyn_base = 0;                  // where vertices start in the GPU's buffer, as the indices were made: the cache's size
    uint32_t epoch = 0;                     // g_native_epoch as the game's thread knew it when it made the frame: a frame made for a cache the
                                            // GPU no longer has (an older epoch) is not drawn
    uint32_t number = 0;                    // the game's count of frames
    bool dumping = false;                   // F11, HP2_SHOTSTEPS: this frame's picture is wanted
    std::vector<NativeTexUpload> uploads;   // the textures decoded for this frame
    double list_ms = 0, tex_ms = 0;         // the game's thread on this frame's lists, and on decoding textures (HP2_NATIVELOG's long frames)
    uint32_t tex_decoded = 0;
    std::vector<NativeDraw> draws;
    std::vector<NativeLight> lights;
    std::vector<NativeFx> fx;
    NativeTexture *road_specular = nullptr;  // pTextureInfoSpecularFalloff: the second pass of bit 0x40
    NativeTexture *env_override = nullptr;  // in the frontend, CARSELECTENVMAP: what the third pass of bit 0x100 reads there
    uint16_t road_mode = 0, env_mode = 0;   // their native_tex_mode
    uint8_t samples[6 * 49 * 28];           // ReflectionSamples, BackReflectionSamples: the fold's grids, as the game had them at the
    uint8_t back_samples[4 * 49 * 28];      // frame's end
    bool env_linear = true;                 // the envmap views' MMag
    bool wide = false;                      // this frame fills the picture; the menus keep to 4:3 in the middle of it
    std::vector<NativeDepthRect> depth_rects;   // eTransferZBufferRect's: what the next frame's FetchVRAM wants of this frame's depth
};

void native_read_list(uint32_t list);       // an eDmaDataList as AttachDmaDataListMain gets it
void native_end_frame();
NativeTexture *native_texture(uint32_t texture_info, uint32_t frame);
uint32_t native_vk_cache_vertices();        // the GPU's vertex cache, in vertices: 0 without one
void native_texture_take_time(double &ms, uint32_t &decoded);   // the time native_texture decoded in since the last call

// native_vk.cpp
// The frame drawn and shown. On the renderer's thread (native.cpp's hand-over), or the game's with HP2_RENDERSYNC=1. Its uploads are taken.
bool native_vk_draw(NativeFrame &frame);
// Counts up each time the GPU's vertex cache stops being what the game's thread was told it is: a frame that did not get to the GPU, the
// renderer made again for other settings. The game's thread empties its side when it sees a new value, and the frames made before that
// are not drawn.
extern std::atomic<uint32_t> g_native_epoch;
void native_render_wait();                  // the game's thread: until the renderer's has no frame in hand
float native_narrow();                      // 4:3 over the race's aspect: 1 or less
int native_aspect();                        // that aspect, 0 4:3, 1 16:10, 2 16:9: this frame's
void native_dump_next();                    // TEMP (HP2_F11DUMP): the frame after next dumped, its draws and pictures
bool native_dumping_now();
// the last frame's depth over a rect of depth_rects, the GS's PSMZ24 words: false if it has not been read
bool native_vk_depth(const NativeDepthRect &r, uint32_t *out);
