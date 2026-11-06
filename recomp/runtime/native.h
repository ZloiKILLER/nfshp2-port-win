// The renderer (work/gsvk/README.md): the game's draw lists on Vulkan, without VU1 and the GS.
#pragma once
void native_notice_saved();     // "SAVED" over the picture for a moment
#include <cstdint>
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
struct NativeTexture {
    int w = 0, h = 0;               // the GS's: 2^TW, 2^TH, what ST 1.0 means. The image's own size can be less
    int image_w = 0, image_h = 0;
    int levels = 1;                 // the game's own
    int gpu_levels = 1;             // what the GPU's image has: the game's, or all of them down to one texel (native_vk's more_levels)
    std::vector<uint32_t> px;       // until the Vulkan side has it: the levels one after another
    std::vector<uint8_t> font_alpha;  // the fonts' level 0: their glyphs' seams are looked for here (native.cpp)
    uint32_t sum = 0, frame = 0;
    bool repeat_s = false, repeat_t = false, mag_linear = true, min_linear = true, mip_linear = false;
    bool upscaled = false;          // a replacement larger than the game's: the GPU's filter, the GS's levels do not fit it
    bool dirty = false;
    void *vk = nullptr;             // native_vk.cpp's
};

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
    std::vector<uint32_t> indices;          // the strips': into the GPU's buffer, the cache below dyn_base, this frame's vertices from it
    std::vector<NativeVertex> fresh;        // the strips the cache gets this frame
    std::vector<NativeFreshRun> fresh_runs;
    uint32_t dyn_base = 0;                  // where vertices start in the GPU's buffer, as the indices were made: the cache's size
    mutable bool uploaded = false;          // native_vk: this frame's vertices went to the GPU (else the cache does not have what it was told)
    double list_ms = 0, tex_ms = 0;         // the game's thread on this frame's lists, and on decoding textures (HP2_NATIVELOG's long frames)
    uint32_t tex_decoded = 0;
    std::vector<NativeDraw> draws;
    std::vector<NativeLight> lights;
    std::vector<NativeFx> fx;
    NativeTexture *road_specular = nullptr;  // pTextureInfoSpecularFalloff: the second pass of bit 0x40
    NativeTexture *env_override = nullptr;  // in the frontend, CARSELECTENVMAP: what the third pass of bit 0x100 reads there
    const void *samples = nullptr;          // ReflectionSamples, BackReflectionSamples: the fold's grids
    const void *back_samples = nullptr;
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
bool native_vk_draw(const NativeFrame &frame);
float native_narrow();                      // 4:3 over the race's aspect: 1 or less
int native_aspect();                        // that aspect, 0 4:3, 1 16:10, 2 16:9: this frame's
void native_dump_next();                    // TEMP (HP2_F11DUMP): the frame after next dumped, its draws and pictures
bool native_dumping_now();
// the last frame's depth over a rect of depth_rects, the GS's PSMZ24 words: false if it has not been read
bool native_vk_depth(const NativeDepthRect &r, uint32_t *out);
