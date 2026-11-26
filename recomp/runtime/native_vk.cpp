// The renderer, the Vulkan side: a picture of its own, blitted to the window through a swapchain (native.h). The picture's size has
// nothing to do with the GS's: clip space is resolution-free.
#include "native.h"
#include "stall.h"
#include <mutex>

#include "hw.h"
#include "pcconfig.h"
#include "pclang.h"
#include "cores.h"

#include <functional>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern FILE *g_hle_log;

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <vulkan/vulkan.h>

static const uint32_t kVert[] =
#include "native.vert.spv.h"
    ;
static const uint32_t kFrag[] =
#include "native.frag.spv.h"
    ;
static const uint32_t kVertClip[] =
#include "clip_native.vert.spv.h"
    ;
static const uint32_t kFragClip[] =
#include "clip_native.frag.spv.h"
    ;
static const uint32_t kFoldVert[] =
#include "native_fold.vert.spv.h"
    ;
static const uint32_t kFoldFrag[] =
#include "native_fold.frag.spv.h"
    ;
static const uint32_t kPostVert[] =
#include "native_post.vert.spv.h"
    ;
static const uint32_t kPostFrag[] =
#include "native_post.frag.spv.h"
    ;
static const uint32_t kRestoreFrag[] =
#include "native_restore.frag.spv.h"
    ;
static const uint32_t kGradeFrag[] =
#include "native_grade.frag.spv.h"
    ;
static const uint32_t kOpaqueFrag[] =
#include "native_opaque.frag.spv.h"
    ;
static const uint32_t kExtraFrag[] =
#include "native_extra.frag.spv.h"
    ;
static const uint32_t kShadowFrag[] =
#include "native_shadow.frag.spv.h"
    ;
static const uint32_t kAoFrag[] =
#include "native_ao.frag.spv.h"
    ;
static const uint32_t kAoApplyFrag[] =
#include "native_ao_apply.frag.spv.h"
    ;
static const uint32_t kShadowFragMs[] =
#include "ms_native_shadow.frag.spv.h"
    ;
static const uint32_t kFragSoft[] =
#include "soft_native.frag.spv.h"
    ;
// SMAA: Jimenez and others' "Enhanced Subpixel Morphological Antialiasing" (shaders/smaa, MIT), its three passes and two tables
static const uint32_t kSmaaEdgeVert[] =
#include "native_smaa_edge.vert.spv.h"
    ;
static const uint32_t kSmaaWeightVert[] =
#include "native_smaa_weight.vert.spv.h"
    ;
static const uint32_t kSmaaBlendVert[] =
#include "native_smaa_blend.vert.spv.h"
    ;
static const uint32_t kSmaaEdgeFrag[] =
#include "native_smaa_edge.frag.spv.h"
    ;
static const uint32_t kSmaaWeightFrag[] =
#include "native_smaa_weight.frag.spv.h"
    ;
static const uint32_t kSmaaBlendFrag[] =
#include "native_smaa_blend.frag.spv.h"
    ;
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

#define NV_INSTANCE(X)                                                                                     \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr)     \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                              \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                           \
    X(vkEnumerateDeviceExtensionProperties) X(vkDestroySurfaceKHR)                                         \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties)
#define NV_DEVICE(X)                                                                                        \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements)        \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateRenderPass) X(vkCreateFramebuffer)                      \
    X(vkCreateShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateCommandPool)        \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)            \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers)                 \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) X(vkCreateFence)        \
    X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) X(vkCreateSampler) X(vkCreateDescriptorSetLayout)        \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkFreeDescriptorSets)        \
    X(vkCmdBindDescriptorSets) X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkDestroyImage) X(vkDestroyImageView)  \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdClearColorImage) X(vkCreateSemaphore) X(vkDeviceWaitIdle)         \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCmdCopyBuffer) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) \
    X(vkCmdCopyImage) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkCmdBeginQuery) X(vkCmdEndQuery)
#define NV_DECLARE(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance vkCreateInstance;
NV_INSTANCE(NV_DECLARE)
NV_DEVICE(NV_DECLARE)

// The picture's size: the resolution's (the window's; fullscreen, the size chosen for it, which blit_to_window stretches to the
// monitor), times Render Scale (20 to 200 %, 100 as it comes). The window shows a 4:3 box of 444 of the picture's 448 lines
// (blit_to_window), the race's wider one beside it, so the picture has as many lines as that box has pixels of height, times
// 448 / 444, and the width that goes with them: at 100 % in a window the picture reaches it pixel for pixel.
// HP2_NATIVESIZE=<w>x<h> sets the 4:3 picture's size whatever the window's.
static int W = 1024, H = 896;       // the race's picture
static int W43 = 1024;              // the 4:3 middle of it
static int g_lines;                 // the height of the window's 4:3 box the picture is to be made for (track_lines); 0: not known yet
// A frame's vertices when they are drawn from the host's copy (no cache), 32 bytes each: 32 million at most (4 million till
// 2026-11-17). The copy itself grows with what a frame needs (vertex_room).
static const VkDeviceSize kMaxBytes = (VkDeviceSize)1024 * 1024 * 1024, kFoldBytes = 1024 * 1024;
static const VkDeviceSize kFpsBytes = 256 * 1024;
// The buffers that grow (2026-11-17, the user: "let a buffer grow as it fills, all the buffers"). Each starts at the size it
// always had and is made anew, twice the size or more, when a frame needs more of it: at the frame's start, when the GPU is done
// with the last frame and nothing reads the old one (lights_room, merge_room, staging_room, depth_room, texture_set,
// vertices_grow). They are not among what rebuild() takes apart: they outlive a change of settings at the size they have
// grown to. HP2_SMALLBUFS=1 starts them all tiny, so that an ordinary race goes through every one's growing: a test of this
// code, not a setting.
static const bool g_small_bufs = getenv("HP2_SMALLBUFS") != nullptr;
static const VkDeviceSize kLightStart = g_small_bufs ? 64 * 1024 : 4 * 1024 * 1024, kLightMost = 64 * 1024 * 1024;
static const VkDeviceSize kStagingStart = g_small_bufs ? 1024 * 1024 : 64 * 1024 * 1024, kStagingMost = (VkDeviceSize)1024 * 1024 * 1024;
static const VkDeviceSize kDepthStart = g_small_bufs ? 4096 : 8 * 1024 * 1024, kDepthMost = 256 * 1024 * 1024;
static const uint32_t kTexSets = g_small_bufs ? 64 : 4096;      // a descriptor pool's textures; a full one is followed by another
// The cars' reflections: the six views round the car and the sphere map they are folded into. The PS2's sizes, or twice them with
// Graphics, Reflections at High (set where the renderer is made: that is part of settings_key).
static int ENV_SIZE = 64, SPHERE_SIZE = 256;

struct VkTex {
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    VkDescriptorSet set;
    VkDescriptorPool pool;                  // the one its set is from (texture_set)
    uint32_t used;
    uint32_t born;                          // the frame it was uploaded at
    NativeTexture *owner;
};

struct Push {
    float clip[16];
    float scroll[2];
    float alpha_test, blend_alpha, textured;
    int light, flags, pass, tfx;
    float lod_l;
    int bits16;
    int tex_mode;
    float snap[2];
    int merged;
    float between;                          // native.vert's particles: where this picture is between the last step and this one
};

// A merged draw's own (native.vert's DrawData), and how many of them and of their commands a picture's buffers have room for at
// first and at most (they grow with the frames: merge_room): past that the draws go out one by one as before
struct DrawData {
    float clip[16];
    int light;
    int pad[3];
};
static const uint32_t kMergeDrawsStart = g_small_bufs ? 256 : 1u << 16, kMergeCmdsStart = g_small_bufs ? 1024 : 1u << 18;
static const uint32_t kMergeDrawsMost = 1u << 20, kMergeCmdsMost = 1u << 22;

struct FoldVertex { float s, t, u, v; };

struct PostPush {
    int mode, row;
    float size[2];
    float lod;
    float pad;
    float scale[2];
    float origin[2];
    float pad2[2];
    float rect[4];
};

// More pictures a second than the game makes frames (NativeFrame::doubled, pictures): a frame is drawn several times, each picture at
// its own place between the frame before and this one: g_pic_f, 1 is the frame itself. A draw's matrix there is on the line through
// its clip_mid (the place halfway) and its clip; so are the vertices of the draws that have a second copy (mid_verts), and the
// port's own particles are simply at that time (native.vert). g_between: the picture is not the frame itself. What a frame does
// once (the copy for the next frame's blend with the last picture, the depth the sun's flare reads, the pictures taken for the
// tests) is left to its last picture, g_pic_last.
// the graphics cards there are, by name, and the one that draws (setup; native_vk_gpus for the menu)
static std::mutex g_gpu_mutex;
static std::vector<std::string> g_gpu_names;
static std::string g_gpu_now;
static bool g_between;
static float g_pic_f = 1.0f;
// Particles: High, the soft sprites: this picture has its copy of the depth, and the port's sprites from here on are drawn as balls
static bool g_soft_now;
static const uint32_t kParticleFlag = 0x10000;      // NativeDraw::flags: a draw of the port's own particles (native.cpp, kPartFlag)
static const uint32_t kSoft2dFlag = 0x40000;        // and of the port's sprites of the game's particles (native.cpp, kSoft2dFlag)
static const uint32_t kSoftKinds = kParticleFlag | kSoft2dFlag;     // what may be drawn as a ball of smoke
static const uint32_t kSoftFlag = 0x20000;          // the constants' flags only: native.vert's SOFT_FRONT
static bool g_pic_last = true;

// a draw's matrix for this picture: exactly clip_mid at the half and clip at 1, as when there were only those two
static void picture_clip(float *out, const NativeDraw &d)
{
    if (!g_between || !d.has_mid) {
        memcpy(out, d.clip, sizeof d.clip);
    } else if (g_pic_f == 0.5f) {
        memcpy(out, d.clip_mid, sizeof d.clip);
    } else {
        const float t = 2.0f * g_pic_f - 1.0f;
        for (int i = 0; i < 16; i++) out[i] = d.clip_mid[i] * (1.0f - t) + d.clip[i] * t;
    }
}

// The effects laid over the picture (NativeFrame::fx) are as many as the frame has. Those that read a palette (the fog, the depth of
// field, the grey: all but the shadows) have a row each of the palette image, and it has so many: one past them is left out. There
// were 8 rows and 8 effects of any kind at most (till 2026-11-17), the shadows, which read none, among them.
static const int kPaletteRows = 32;
// an effect's row of the palette image; -1: it reads none
static bool fx_palette(uint8_t kind) { return kind != FX_SHADOW && kind != FX_AO && kind != FX_AO_WORLD; }
static int fx_row(const NativeFrame &frame, size_t i)
{
    if (!fx_palette(frame.fx[i].kind)) return -1;
    int row = 0;
    for (size_t k = 0; k < i; k++) row += fx_palette(frame.fx[k].kind);
    return row;
}

// Shadows: the map of the cars as the sun sees them is so many cells across and down, a car each (native.cpp, kShadowCells). The
// image is twice as many down: the upper half has the cars' bodies, the lower, cell for cell, their glass (kShadowRows in all).
static const int kShadowAcross = 8, kShadowDown = 4, kShadowRows = kShadowDown * 2;
// At Ultra two rows more below them for the tunnels' lamps (native.cpp, kLampCells): cells of half the side, so 16 across and 4 down
static const int kLampCellsAcross = kShadowAcross * 2, kLampRows = 2, kLampCellsAll = kLampCellsAcross * kLampRows * 2;

// native_shadow.frag's
struct ShadowPush {
    float to[16];
    float rect[4];
    float colour[4];
    float view[4];
    float mode, texel, low, high;
};

// native_extra.frag's
struct ExtraPush {
    float rect[4];
    float size[2];
    float frame;
    float lines;
    float grain;            // the picture's pixels to a pixel of the window, 1 at the least
    float wide;             // 1: the picture has 16 bits a channel (g_color_wide), the noise alone is laid on
    float brightness, contrast, gamma, graded;      // graded 1: the settings' three in this pass (with wide only)
    float opaque;           // 1: the picture's alpha to 1 here
};

static struct {
    bool ready, failed;
    uint64_t key;                           // the settings this was all made for
    VkPhysicalDeviceProperties props;
    bool can_aniso;
    bool clip_far;                          // shaderClipDistance: the far plane by a clip distance, the scene's shaders without a discard but for the alpha test
    VkBool32 depth_clamp;
    uint32_t family;
    VkDevice dev;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties mem;
    // The vertices: the CPU writes the host's copy (vb_stage), the GPU copies it into its own memory (vb) at the start of the frame and draws
    // from there; read over the bus at every pass, the host's copy cost the scene most of its time. Both grow with what a frame needs.
    VkBuffer vb_dev, vb_stage, readback;   // vertex_room
    VkDeviceMemory vb_dev_mem, vb_stage_mem;
    VkDeviceSize vb_cap, dyn_off;
    VkDeviceSize vb_total, vb_side;         // the GPU's buffer, and the end of it that is a frame's own (vb_layout)
    uint32_t cache_verts;
    bool vb_in_vram;
    VkBuffer vb;                            // the frame's: what the draws read, vertices and indices
    VkDeviceSize draw_idx_off;
    uint32_t draw_dyn_first;
    void *vb_map, *rb_map;
    VkImage color, depth;
    VkSampleCountFlagBits samples;          // the picture's, the setting's if the GPU has it
    VkImage color_ms, depth_ms;
    VkPipeline pipeline_ms[2][2][NB_COUNT][2][2], alpha_only_ms[2][2][2], restore_pipeline;
    VkPipelineLayout restore_layout;
    VkDescriptorSet restore_set;
    // SMAA on the finished picture (the setting's): edges, then weights, then the picture blended, each a pass of its own
    bool smaa, smaa_tables;
    VkImage smaa_edges, smaa_weights, smaa_area, smaa_search;
    VkRenderPass smaa_pass;
    VkFramebuffer smaa_fb[2];
    VkPipeline smaa_pipeline[3];
    VkPipelineLayout smaa_layout[3];
    VkDescriptorSet smaa_scene_set, smaa_edges_set, smaa_weights_set, smaa_area_set, smaa_search_set;
    VkBuffer smaa_stage;
    // The port's dithering (native_extra.frag)
    VkPipeline extra_pipeline;
    VkPipelineLayout extra_layout;
    VkDescriptorSetLayout extra_set_layout;
    VkDescriptorSet extra_set;
    // The draws merged: runs of draws alike but for the matrix and the light go out as one vkCmdDrawIndirect each. draws_buf has
    // each such draw's matrix and light (the vertex shader's D[]), cmds_buf the commands, a range of vertices each
    bool can_merge;
    VkBuffer draws_buf, cmds_buf;
    void *draws_map, *cmds_map;
    uint32_t merge_draws, merge_cmds;       // used so far this frame
    uint32_t draws_cap, cmds_cap;           // what the two have room for (merge_room)
    VkDeviceMemory draws_mem, cmds_mem;
    VkPipeline grade_pipeline;              // brightness, contrast, gamma
    VkPipelineLayout grade_layout;
    VkDescriptorSet grade_set;
    VkPipeline opaque_pipeline;             // the picture's alpha to 1, before the window
    VkPipelineLayout opaque_layout;
    int aniso;                              // the setting's anisotropy if the GPU has it, 0: the GS's own sampling
    VkRenderPass pass;
    VkFramebuffer fb;
    // by the shaders (0 the clip distance's, 1 the discard's), without and with the alpha test, the blend, without and with depth writes,
    // without and with the depth test
    VkPipeline pipeline[2][2][NB_COUNT][2][2];
    VkPipeline alpha_only[2][2][2];         // the frame's alpha alone, after a blend: by the shaders, the alpha test, without and with the depth test
    VkBuffer rvm_rb;
    void *rvm_rb_map;
    int rvm_w, rvm_h;                       // the mirror: the GS's 256 x 64 at the picture's scale
    VkImage rvm, rvm_depth;
    VkFramebuffer rvm_fb;
    VkDescriptorSet rvm_set;
    bool rvm_ready;
    VkImage scene, prev, palette;           // the picture so far with its levels, the last frame, the palettes by row
    int scene_levels;
    VkRenderPass pass_load, post_pass;
    VkFramebuffer post_fb;
    VkDescriptorSet post_set, prev_set;
    VkDescriptorSetLayout post_set_layout;
    VkPipelineLayout post_layout;
    VkPipeline post_pipeline[3];            // over, darken, opaque
    // Shadows: High (native.cpp). The map: 8 x 4 cells of depth, a car each as the sun sees it, the furthest kept. shadow_cast draws a
    // car into its cell; shadow_pipeline lays a shadow over the picture (the post pass) or over the mirror (rvm_post_pass)
    VkImage shadow_map;
    VkImageView shadow_view, rvm_depth_view;
    VkRenderPass shadow_pass, rvm_post_pass;
    VkRenderPass env_pass_load;             // env_pass with what is there kept: the rest of the mirror after its shadows
    VkFramebuffer shadow_fb, rvm_post_fb;
    VkPipeline shadow_cast, shadow_pipeline;
    // A car's draw with the alpha test goes into the map with it too (native.frag, ALPHA_TEST): the brake discs are a picture of a
    // disc on a square, the square's corners see-through, and drawn whole the squares stood out behind the wheels, their edges
    // giving the shadow's test "behind the car" places it took for the car itself (the user's pictures, 2026-11-26)
    VkPipeline shadow_cast_test;
    VkPipelineLayout shadow_layout;
    VkDescriptorSet shadow_set, rvm_post_set;
    int palette_rows;                       // of the palette image: how many this frame's effects have (upload_palettes)
    int shadow_cell;                        // a cell's side in texels: 256, at Very High 512, at Ultra 1024
    int shadow_h;                           // the map's height in texels: kShadowRows cells, and at Very High and Ultra two rows more for the lamps
    bool lamp_row;                          // the map has the tunnel lamps' rows (Very High, Ultra): 16 x 4 cells of half a cell each
    bool shadow_ready;                      // the map has been drawn at least once
    // Shadows with multisampling: laid over the samples themselves (ms_read_pass: the picture's samples, their depth read and not
    // written), each sample by its own depth. Null where the card cannot shade by the sample: over the resolved picture then, as before
    bool sample_shading;
    VkImageView depth_ms_view;
    VkRenderPass ms_read_pass;
    VkFramebuffer ms_read_fb;
    VkPipeline shadow_ms_pipeline;
    VkDescriptorSet ms_post_set;
    // Ambient Occlusion (native.cpp, ao_end): made at every second pixel into an image of its own (ao_pass, native_ao.frag), then
    // laid over the picture from it (ao_apply_pipeline, native_ao_apply.frag, in the post pass, blended as the shadows are)
    VkImage ao;
    VkImageView ao_view;
    VkRenderPass ao_pass;
    VkFramebuffer ao_fb;
    VkPipeline ao_pipeline, ao_apply_pipeline;
    VkDescriptorSet ao_set;
    int ao_w, ao_h;
    // and the depth with no car in it yet (FX_AO_WORLD, copy_depth_world): what the road and the walls are shaded by (ПОРА111)
    VkImage depth_world;
    VkImageView depth_world_view;
    VkDescriptorSet ao_world_set;
    bool world_ready;                   // it has been copied into since it was made: its layout is SHADER_READ_ONLY
    // Particles: High, the soft sprites (native.frag, SOFT): a copy of the picture's depth made before a picture's first such sprite
    // (copy_depth_soft), and the scene's pipelines again with that shader, by the samples (0 one, 1 the picture's), the blend, the depth test
    VkImage depth_copy;
    VkImageView depth_copy_view;
    VkDescriptorSet soft_set;
    VkPipelineLayout soft_layout;
    VkPipeline soft_pipeline[2][NB_COUNT][2];
    bool soft_ok;
    bool prev_ready;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    std::vector<VkDescriptorPool> tex_pools;    // more of them for the textures' sets, as the ones before filled up (texture_set)
    VkSampler sampler[32];                  // repeat s, repeat t, mag linear, min linear, mip linear
    VkBuffer staging;
    void *staging_map;
    VkDeviceMemory staging_mem;
    VkDeviceSize staging_cap;               // its size (staging_room)
    VkDeviceSize staging_want;              // a texture that did not fit it whole: so much before the next frame's uploads
    VkDeviceSize staging_refused;           // not 0: so much could not be had, a texture of that size or more stays as it is
    std::vector<VkTex *> textures;
    uint32_t frame;
    VkDescriptorSetLayout light_layout;
    VkDescriptorSet light_set;
    VkBuffer lights, lights_stage, fold_vb;  // the lights as the vertices: the host's copy, the GPU's (lights == lights_stage without one)
    void *lights_map, *fold_map;
    VkDeviceMemory lights_mem, lights_stage_mem;
    VkDeviceSize light_cap;                 // the lights' buffers in bytes (lights_room)
    VkImage env[6], sphere, env_depth;
    VkRenderPass env_pass, fold_pass;
    VkFramebuffer env_fb[6], sphere_fb;
    VkBuffer sphere_rb;
    void *sphere_rb_map;
    VkDescriptorSet env_set[6], sphere_set, sphere_prev_set;
    VkImage sphere_prev;
    VkPipeline fold_pipeline;
    VkPipelineLayout fold_layout;
    bool env_ready[6], env_drawn_now[6], sphere_ready;
    bool scene_ready;                       // the scene image has had a layout: copy_scene, or the fog's first frame
    VkCommandBuffer cmd;
    VkCommandBuffer cmd_b;                  // the frame's second part, after depth_send_early (the two change places each time)
    VkFence fence;
    VkFence depth_fence;                    // the first part's: the depth rects are copied (depth_send_early)
    bool depth_early;                       // the last frame's rects went on depth_fence
    bool waiting;
    // the window
    VkInstance instance;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swap_images;
    int swap_w, swap_h;                     // the swapchain's images
    int lines;                              // g_lines as the picture was made for it
    int swap_cw, swap_ch;                   // the window's client size it was made for
    bool swap_stale;                        // to be made again
    bool swap_check;                        // "suboptimal" was said: made again only if the surface's size is no longer the swapchain's
    VkSurfaceTransformFlagBitsKHR swap_transform;
    int swap_streak;                        // frames in a row that made it again
    uint32_t swap_made_frame;               // the last frame that made it
    uint32_t swap_rest_until;               // after a streak: not before this frame
    bool surface_lost;                      // the surface itself is to be made again first
    bool swap_vsync;                        // made with the setting's VSync on
    bool no_exclusive;                      // VK_EXT_full_screen_exclusive is there: the driver is told not to take the screen for itself
    VkSemaphore acquired, rendered;
    std::vector<VkSemaphore> present_sems;  // per swapchain image: what its present waits on (one for all may still be in use by the last)
    VkBuffer fps_buf;
    void *fps_map;
    int fps_shown, fps_frames, presented;
    uint64_t fps_t0;
    // eTransferZBufferRect's rects of the last frame
    VkBuffer depth_rb;
    void *depth_rb_map;
    VkDeviceMemory depth_rb_mem;
    VkDeviceSize depth_cap;                 // its size (depth_room)
    std::vector<NativeDepthRect> depth_rects;
    std::vector<VkDeviceSize> depth_places;
    float depth_ox, depth_sx;
    VkDeviceSize depth_bytes;               // how much of depth_rb the last frame's rects take
    bool depth_new;                         // copied by a frame and not kept yet (depth_keep)
} g;

// Everything that depends on the settings is made once more when they change (rebuild). What setup_resources makes is written down here as it
// is made, by standing in for the vkCreate functions, so taking it apart cannot miss anything.
static bool g_tracking;
static std::vector<std::function<void()>> g_undo;

#define NV_TRACK(Create, Info, Handle, Destroy)                                                                       \
    static PFN_##Create real_##Create;                                                                                \
    static VKAPI_ATTR VkResult VKAPI_CALL track_##Create(VkDevice d, const Info *i, const VkAllocationCallbacks *a, Handle *o) \
    {                                                                                                                 \
        VkResult r = real_##Create(d, i, a, o);                                                                       \
        if (r == VK_SUCCESS && g_tracking) {                                                                          \
            Handle h = *o;                                                                                            \
            g_undo.push_back([d, h] { ((PFN_##Destroy)vkGetDeviceProcAddr(d, #Destroy))(d, h, nullptr); });           \
        }                                                                                                             \
        return r;                                                                                                     \
    }
NV_TRACK(vkCreateBuffer, VkBufferCreateInfo, VkBuffer, vkDestroyBuffer)
NV_TRACK(vkAllocateMemory, VkMemoryAllocateInfo, VkDeviceMemory, vkFreeMemory)
NV_TRACK(vkCreateImage, VkImageCreateInfo, VkImage, vkDestroyImage)
NV_TRACK(vkCreateImageView, VkImageViewCreateInfo, VkImageView, vkDestroyImageView)
NV_TRACK(vkCreateRenderPass, VkRenderPassCreateInfo, VkRenderPass, vkDestroyRenderPass)
NV_TRACK(vkCreateRenderPass2, VkRenderPassCreateInfo2, VkRenderPass, vkDestroyRenderPass)
NV_TRACK(vkCreateFramebuffer, VkFramebufferCreateInfo, VkFramebuffer, vkDestroyFramebuffer)
NV_TRACK(vkCreateShaderModule, VkShaderModuleCreateInfo, VkShaderModule, vkDestroyShaderModule)
NV_TRACK(vkCreatePipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout, vkDestroyPipelineLayout)
NV_TRACK(vkCreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool, vkDestroyCommandPool)
NV_TRACK(vkCreateFence, VkFenceCreateInfo, VkFence, vkDestroyFence)
NV_TRACK(vkCreateSemaphore, VkSemaphoreCreateInfo, VkSemaphore, vkDestroySemaphore)
NV_TRACK(vkCreateSampler, VkSamplerCreateInfo, VkSampler, vkDestroySampler)
NV_TRACK(vkCreateDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout, vkDestroyDescriptorSetLayout)
NV_TRACK(vkCreateDescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool, vkDestroyDescriptorPool)

// ---- the pipelines: kept, and made on the workers (2026-11-24) --------------------------------------------------------------------
// The user, on Intel's UHD 630: "switching the graphics settings the game sometimes froze: the choice was not taken and two seconds
// of the music went round". The log: the change to MSAA made the renderer again in 4.4 s (the other changes in 0.1 to 0.3 s), the
// game's thread waiting for the renderer's at the hand-over all that time, and the sound with it. setup_resources makes some 90
// pipelines, with multisampling 75 more, one after another on one thread and with no pipeline cache: every one compiled by the
// driver from nothing, each time anew.
// Now, at the one place all of them are made through (the stand-in below):
//   - a pipeline cache for the device's life, so what was compiled once in a session is not compiled again by a later change; and
//     kept in a file beside the exe (shaders_<vendor>_<device>.bin, the driver's own data behind Vulkan's header, which is looked at
//     before the file is given back to the driver), so the next start and the first change of the next session have it too;
//   - what setup_resources asks for is written down whole (a copy of everything the request points at: the code goes on to change
//     its structures for the next one) and made at its end by the workers and this thread together. A handle is there only then:
//     nothing in setup_resources reads one.
// The time is in the log's "native: pipelines" line. HP2_PIPEJOBS=0: each made where it is asked for, as before. HP2_PIPECACHE=0:
// no cache.
static PFN_vkCreateGraphicsPipelines real_vkCreateGraphicsPipelines;

struct PipeJob {
    VkGraphicsPipelineCreateInfo pi;
    VkPipelineShaderStageCreateInfo stages[2];
    VkSpecializationInfo spec[2];
    std::vector<VkSpecializationMapEntry> spec_map[2];
    std::vector<unsigned char> spec_data[2];
    VkPipelineVertexInputStateCreateInfo vin;
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vs;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    VkPipelineRasterizationStateCreateInfo ra;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendStateCreateInfo cb;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    VkPipelineDynamicStateCreateInfo dyn;
    std::vector<VkDynamicState> dyn_states;
    VkPipeline *out;
    VkResult result;
    std::atomic<int> state{0};              // 0 asked for, 1 being made, 2 made, 3 could not be made
    VkPipeline made = VK_NULL_HANDLE;       // (there with state 2)
};
static std::vector<std::unique_ptr<PipeJob>> g_pipe_jobs;

static bool g_pipe_wait;                    // setup_resources is running: its pipelines are written down, not made
static VkPipelineCache g_pipe_cache;
static size_t g_pipe_saved;                 // the cache's size as the file has it
static double g_pipe_ms;                    // made where they were asked for: the time, and how many
static unsigned g_pipe_direct;

static double pipe_clock()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- made when first bound, the rest behind the picture (2026-11-24) -----------------------------------------------------------
// The cache and the workers above left the first time a set of settings is seen as it was: the user on Intel's UHD 630, "MSAA
// freezes all the same": 161 pipelines in 2.1 s, 88 in 2.8 s for a change of the picture's format, the workers or no (that driver
// compiles one at a time, some 30 ms each), the game's thread waiting and the music going round. A picture needs a handful of
// them; so none is made by setup_resources at all. Where a pipeline was asked for, its variable gets a stand-in handle (the
// request's own address); the first vkCmdBindPipeline of it makes the pipeline there and then and puts the real handle in the
// variable; and a thread of its own goes through the rest meanwhile, so that by the time a race begins they are there (and in the
// cache's file: the next start has them at once). A change of settings is then a few pipelines' wait, once for each set of
// settings ever.
// Only the renderer's thread writes a variable or binds; the other thread makes a pipeline and says so in the request. Before the
// renderer's things are taken apart (rebuild) that thread is stopped and what was made is destroyed here: these are not in g_undo.
// HP2_PIPELAZY=0: all of them at the end of setup_resources, by the workers, as before.
static const bool g_pipe_lazy = !(getenv("HP2_PIPELAZY") && atoi(getenv("HP2_PIPELAZY")) == 0);
static std::vector<std::unique_ptr<PipeJob>> g_pipe_live;           // the requests the stand-in handles are of
static std::unordered_map<uint64_t, PipeJob *> g_pipe_tokens;       // by stand-in handle (the renderer's thread only)
static std::atomic<bool> g_pipe_stop, g_pipe_running;
static unsigned g_pipe_asked;               // made at a bind, since the last setup_resources: how many, and the time
static double g_pipe_asked_ms;
static PFN_vkCmdBindPipeline real_vkCmdBindPipeline;
static void pipe_cache_save();

// by whichever thread comes first; the other waits for it
static bool pipe_make(PipeJob &j)
{
    int asked = 0;
    if (j.state.compare_exchange_strong(asked, 1)) {
        VkPipeline made = VK_NULL_HANDLE;
        const VkResult r = real_vkCreateGraphicsPipelines(g.dev, g_pipe_cache, 1, &j.pi, nullptr, &made);
        j.result = r;
        j.made = made;
        j.state.store(r == VK_SUCCESS ? 2 : 3);
    } else {
        while (j.state.load() == 1) std::this_thread::yield();
    }
    return j.state.load() == 2;
}

static void pipe_background()
{
    cores_thread(CORE_BACKGROUND);
    const double t0 = pipe_clock();
    unsigned made = 0, failed = 0;
    for (auto &j : g_pipe_live) {
        if (g_pipe_stop.load()) break;
        if (j->state.load() != 0) continue;
        if (pipe_make(*j)) made++;
        else if (j->state.load() == 3) failed++;
    }
    if (!g_pipe_stop.load()) {
        const size_t before = g_pipe_saved;
        pipe_cache_save();
        fprintf(g_hle_log, "native: pipelines: %u made behind the picture in %.0f ms%s; the cache %zu KB%s\n", made, pipe_clock() - t0,
                failed ? " (SOME COULD NOT BE MADE)" : "", g_pipe_saved >> 10, g_pipe_saved != before ? ", written to its file" : "");
    }
    g_pipe_running.store(false);
}

static VKAPI_ATTR void VKAPI_CALL lazy_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline)
{
    if (!g_pipe_tokens.empty()) {
        const auto it = g_pipe_tokens.find((uint64_t)(uintptr_t)pipeline);
        if (it != g_pipe_tokens.end()) {
            PipeJob &j = *it->second;
            if (j.state.load() != 2) {
                const double t0 = pipe_clock();
                pipe_make(j);
                g_pipe_asked_ms += pipe_clock() - t0;
                g_pipe_asked++;
            }
            if (j.state.load() != 2) {
                static int told;
                if (told++ < 5) fprintf(g_hle_log, "native: a pipeline could not be made (%d): the renderer stops\n", (int)j.result);
                g.failed = true;
                return;
            }
            pipeline = j.made;
            *j.out = pipeline;              // the variable has the real handle from here on
        }
    }
    real_vkCmdBindPipeline(cb, point, pipeline);
}

// Before the renderer's things are taken apart, the device idle: the thread stopped, what was made destroyed, the requests gone.
static void pipes_retire()
{
    g_pipe_stop.store(true);
    while (g_pipe_running.load()) std::this_thread::yield();
    g_pipe_stop.store(false);
    if (!g_pipe_live.empty()) {
        const auto destroy = (PFN_vkDestroyPipeline)vkGetDeviceProcAddr(g.dev, "vkDestroyPipeline");
        for (auto &j : g_pipe_live)
            if (j->state.load() == 2 && j->made && destroy) destroy(g.dev, j->made, nullptr);
        if (g_pipe_asked)
            fprintf(g_hle_log, "native: pipelines: %u were made when first bound, %.0f ms in all\n", g_pipe_asked, g_pipe_asked_ms);
    }
    g_pipe_tokens.clear();
    g_pipe_live.clear();
    g_pipe_asked = 0;
    g_pipe_asked_ms = 0;
}

// a request the copy below takes whole: no chains, no more than two stages, nothing it has no place for
static bool pipe_can_wait(const VkGraphicsPipelineCreateInfo &i)
{
    if (i.pNext || !i.pStages || i.stageCount > 2 || i.pTessellationState) return false;
    for (uint32_t k = 0; k < i.stageCount; k++)
        if (i.pStages[k].pNext) return false;
    if ((i.pVertexInputState && i.pVertexInputState->pNext) || (i.pInputAssemblyState && i.pInputAssemblyState->pNext) ||
        (i.pViewportState && i.pViewportState->pNext) || (i.pRasterizationState && i.pRasterizationState->pNext) ||
        (i.pMultisampleState && (i.pMultisampleState->pNext || i.pMultisampleState->pSampleMask)) ||
        (i.pDepthStencilState && i.pDepthStencilState->pNext) || (i.pColorBlendState && i.pColorBlendState->pNext) ||
        (i.pDynamicState && i.pDynamicState->pNext))
        return false;
    return true;
}

static void pipe_keep(const VkGraphicsPipelineCreateInfo &i, VkPipeline *out)
{
    g_pipe_jobs.push_back(std::make_unique<PipeJob>());
    PipeJob &j = *g_pipe_jobs.back();       // (on the heap and never moved: it points into itself)
    j.pi = i;
    j.out = out;
    j.result = VK_SUCCESS;
    for (uint32_t k = 0; k < i.stageCount; k++) {
        j.stages[k] = i.pStages[k];
        if (const VkSpecializationInfo *sp = i.pStages[k].pSpecializationInfo) {
            j.spec[k] = *sp;
            if (sp->pMapEntries) j.spec_map[k].assign(sp->pMapEntries, sp->pMapEntries + sp->mapEntryCount);
            if (sp->pData) j.spec_data[k].assign((const unsigned char *)sp->pData, (const unsigned char *)sp->pData + sp->dataSize);
            j.spec[k].pMapEntries = j.spec_map[k].data();
            j.spec[k].pData = j.spec_data[k].data();
            j.stages[k].pSpecializationInfo = &j.spec[k];
        }
    }
    j.pi.pStages = j.stages;
    if (i.pVertexInputState) {
        j.vin = *i.pVertexInputState;
        if (j.vin.pVertexBindingDescriptions) j.binds.assign(j.vin.pVertexBindingDescriptions, j.vin.pVertexBindingDescriptions + j.vin.vertexBindingDescriptionCount);
        if (j.vin.pVertexAttributeDescriptions)
            j.attrs.assign(j.vin.pVertexAttributeDescriptions, j.vin.pVertexAttributeDescriptions + j.vin.vertexAttributeDescriptionCount);
        j.vin.pVertexBindingDescriptions = j.binds.data();
        j.vin.pVertexAttributeDescriptions = j.attrs.data();
        j.pi.pVertexInputState = &j.vin;
    }
    if (i.pInputAssemblyState) j.ia = *i.pInputAssemblyState, j.pi.pInputAssemblyState = &j.ia;
    if (i.pViewportState) {
        j.vs = *i.pViewportState;
        if (j.vs.pViewports) j.viewports.assign(j.vs.pViewports, j.vs.pViewports + j.vs.viewportCount), j.vs.pViewports = j.viewports.data();
        if (j.vs.pScissors) j.scissors.assign(j.vs.pScissors, j.vs.pScissors + j.vs.scissorCount), j.vs.pScissors = j.scissors.data();
        j.pi.pViewportState = &j.vs;
    }
    if (i.pRasterizationState) j.ra = *i.pRasterizationState, j.pi.pRasterizationState = &j.ra;
    if (i.pMultisampleState) j.ms = *i.pMultisampleState, j.pi.pMultisampleState = &j.ms;
    if (i.pDepthStencilState) j.ds = *i.pDepthStencilState, j.pi.pDepthStencilState = &j.ds;
    if (i.pColorBlendState) {
        j.cb = *i.pColorBlendState;
        if (j.cb.pAttachments) j.blends.assign(j.cb.pAttachments, j.cb.pAttachments + j.cb.attachmentCount), j.cb.pAttachments = j.blends.data();
        j.pi.pColorBlendState = &j.cb;
    }
    if (i.pDynamicState) {
        j.dyn = *i.pDynamicState;
        if (j.dyn.pDynamicStates) j.dyn_states.assign(j.dyn.pDynamicStates, j.dyn.pDynamicStates + j.dyn.dynamicStateCount), j.dyn.pDynamicStates = j.dyn_states.data();
        j.pi.pDynamicState = &j.dyn;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL track_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o)
{
    if (!c) c = g_pipe_cache;
    if (g_pipe_wait && n == 1 && !a && pipe_can_wait(*i)) {
        *o = VK_NULL_HANDLE;
        pipe_keep(*i, o);
        return VK_SUCCESS;
    }
    const double t0 = pipe_clock();
    VkResult r = real_vkCreateGraphicsPipelines(d, c, n, i, a, o);
    g_pipe_ms += pipe_clock() - t0;
    g_pipe_direct += n;
    if (r == VK_SUCCESS && g_tracking)
        for (uint32_t k = 0; k < n; k++) {
            VkPipeline h = o[k];
            g_undo.push_back([d, h] { ((PFN_vkDestroyPipeline)vkGetDeviceProcAddr(d, "vkDestroyPipeline"))(d, h, nullptr); });
        }
    return r;
}

// the file is the device's own: a machine with two cards keeps one for each
static std::string pipe_cache_path()
{
    char name[64];
    snprintf(name, sizeof name, "shaders_%04x_%04x.bin", g.props.vendorID, g.props.deviceID);
    std::string p = name;
#ifdef _WIN32
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        p = exe;
        p = p.substr(0, p.find_last_of("\\/") + 1) + name;
    }
#endif
    return p;
}

// Once, with the device there. The file is given to the driver only if Vulkan's header on it says this card and this driver's data
// (the drivers look too; this is so that no driver has to).
static void pipe_cache_open()
{
    static bool tried;
    if (tried) return;
    tried = true;
    static const bool off = getenv("HP2_PIPECACHE") && atoi(getenv("HP2_PIPECACHE")) == 0;
    if (off || !vkCreatePipelineCache || !vkGetPipelineCacheData) return;
    std::vector<unsigned char> data;
    if (FILE *f = fopen(pipe_cache_path().c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size <= (64 << 20)) {
            data.resize((size_t)size);
            if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        fclose(f);
    }
    struct Head {
        uint32_t length, version, vendor, device;
        uint8_t uuid[VK_UUID_SIZE];
    } h;
    bool good = data.size() > sizeof h;
    if (good) {
        memcpy(&h, data.data(), sizeof h);
        good = h.length >= sizeof h && h.version == (uint32_t)VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h.vendor == g.props.vendorID &&
               h.device == g.props.deviceID && !memcmp(h.uuid, g.props.pipelineCacheUUID, VK_UUID_SIZE);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (good) ci.initialDataSize = data.size(), ci.pInitialData = data.data();
    if (vkCreatePipelineCache(g.dev, &ci, nullptr, &g_pipe_cache)) {
        g_pipe_cache = VK_NULL_HANDLE;
        ci.initialDataSize = 0, ci.pInitialData = nullptr;
        good = false;
        if (vkCreatePipelineCache(g.dev, &ci, nullptr, &g_pipe_cache)) g_pipe_cache = VK_NULL_HANDLE;
    }
    g_pipe_saved = good ? data.size() : 0;
    fprintf(g_hle_log, "native: pipeline cache: %s\n",
            !g_pipe_cache ? "none could be made" : good ? "taken from its file" : data.empty() ? "new, no file yet" : "new, the file is another driver's");
}

// to the file, when there is more in it than the file has: written beside and put in the file's place whole
static void pipe_cache_save()
{
    if (!g_pipe_cache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(g.dev, g_pipe_cache, &size, nullptr) || !size || size == g_pipe_saved || size > (64u << 20)) return;
    std::vector<unsigned char> data(size);
    if (vkGetPipelineCacheData(g.dev, g_pipe_cache, &size, data.data()) != VK_SUCCESS || !size) return;
    const std::string path = pipe_cache_path();
#ifdef _WIN32
    const std::string tmp = path + "." + std::to_string((unsigned)GetCurrentProcessId()) + ".tmp";
#else
    const std::string tmp = path + ".tmp";
#endif
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool written = fwrite(data.data(), 1, size, f) == size;
    fclose(f);
#ifdef _WIN32
    if (!written || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return;
    }
#else
    if (!written || rename(tmp.c_str(), path.c_str())) {
        remove(tmp.c_str());
        return;
    }
#endif
    g_pipe_saved = size;
}

static void pipes_begin()
{
    static const bool jobs_off = getenv("HP2_PIPEJOBS") && atoi(getenv("HP2_PIPEJOBS")) == 0;
    pipe_cache_open();
    pipes_retire();                         // (rebuild has done it already, before it took the rest apart)
    if (!real_vkCmdBindPipeline) real_vkCmdBindPipeline = vkCmdBindPipeline, vkCmdBindPipeline = lazy_vkCmdBindPipeline;
    g_pipe_jobs.clear();
    g_pipe_ms = 0;
    g_pipe_direct = 0;
    g_pipe_wait = !jobs_off;
}

// setup_resources is through (made: all of it went well): what it asked for is made, by the workers and this thread. The pool is
// the game's too: if the game's thread has it at this moment, the pieces are done here in order (cores.cpp), which is the old way.
static bool pipes_end(bool made)
{
    g_pipe_wait = false;
    if (g_pipe_lazy) {
        // none made here: each variable gets its stand-in, and the thread begins on them
        if (!made) {
            g_pipe_jobs.clear();
            return false;
        }
        g_pipe_live = std::move(g_pipe_jobs);
        g_pipe_jobs.clear();
        for (auto &j : g_pipe_live) {
            const VkPipeline token = reinterpret_cast<VkPipeline>(j.get());
            *j->out = token;
            g_pipe_tokens[(uint64_t)(uintptr_t)token] = j.get();
        }
        fprintf(g_hle_log, "native: pipelines: %zu to be made when first bound and behind the picture, %u more in %.0f ms where asked for\n",
                g_pipe_live.size(), g_pipe_direct, g_pipe_ms);
        if (!g_pipe_live.empty()) {
            g_pipe_running.store(true);
            std::thread(pipe_background).detach();
        }
        return true;
    }
    bool ok = true;
    const size_t count = made ? g_pipe_jobs.size() : 0;
    const double t0 = pipe_clock();
    if (count) {
        const VkDevice d = g.dev;
        jobs_each((int)count, [d](int k, int) {
            PipeJob &j = *g_pipe_jobs[(size_t)k];
            j.result = real_vkCreateGraphicsPipelines(d, g_pipe_cache, 1, &j.pi, nullptr, j.out);
        });
        for (size_t k = 0; k < count; k++) {
            PipeJob &j = *g_pipe_jobs[k];
            if (j.result != VK_SUCCESS) {
                ok = false;
                *j.out = VK_NULL_HANDLE;
                continue;
            }
            VkPipeline h = *j.out;
            g_undo.push_back([d, h] { ((PFN_vkDestroyPipeline)vkGetDeviceProcAddr(d, "vkDestroyPipeline"))(d, h, nullptr); });
        }
    }
    const double jobs_ms = pipe_clock() - t0;
    g_pipe_jobs.clear();
    if (made) {
        const size_t before = g_pipe_saved;
        pipe_cache_save();
        fprintf(g_hle_log, "native: pipelines: %zu in %.0f ms with %d workers, %u more in %.0f ms where asked for%s; the cache %zu KB%s\n", count, jobs_ms,
                cores_workers(), g_pipe_direct, g_pipe_ms, ok ? "" : " (SOME WERE NOT MADE)", g_pipe_saved >> 10,
                g_pipe_saved != before ? ", written to its file" : "");
    }
    return made && ok;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// A buffer in memory with need, prefer as well if there is such (HOST_CACHED for what the CPU writes whole: the copy is several times
// faster). map: mapped, else not. mem: the memory, for the buffers that are let go of by hand.
static bool make_buffer_in(VkBuffer &b, void **map, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags need,
                           VkMemoryPropertyFlags prefer = 0, VkDeviceMemory *mem = nullptr)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    b = VK_NULL_HANDLE;
    if (vkCreateBuffer(g.dev, &bi, nullptr, &b)) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(g.dev, b, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    VkDeviceMemory m = VK_NULL_HANDLE;
    ai.memoryTypeIndex = prefer ? mem_type(r.memoryTypeBits, need | prefer) : UINT32_MAX;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.dev, &ai, nullptr, &m)) {
        m = VK_NULL_HANDLE;
        ai.memoryTypeIndex = mem_type(r.memoryTypeBits, need);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.dev, &ai, nullptr, &m)) m = VK_NULL_HANDLE;
    }
    if (!m || vkBindBufferMemory(g.dev, b, m, 0) || (map && vkMapMemory(g.dev, m, 0, size, 0, map) != VK_SUCCESS)) {
        if (!g_tracking) {
            vkDestroyBuffer(g.dev, b, nullptr);
            if (m) vkFreeMemory(g.dev, m, nullptr);
        }
        b = VK_NULL_HANDLE;
        return false;
    }
    if (mem) *mem = m;
    return true;
}

static bool make_buffer(VkBuffer &b, void **map, VkDeviceSize size, VkBufferUsageFlags usage)
{
    return make_buffer_in(b, map, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

// The vertices. The GPU's buffer, made once and kept through changes of settings: the strips' cache (native.cpp), then a frame's own vertices,
// then its indices, 256 MB or less where there is not the room. The host's copy (vb_stage) holds a frame's own vertices, the strips new to
// the cache and the indices, one after another, and grows when a frame needs more; the GPU copies what it holds at the frame's start.
// Without the GPU's buffer (HP2_HOSTBUFFERS, or no memory) the host's copy is drawn from, and there is no cache.
// The end of the buffer that is a frame's own (vb_side): its vertices, then its indices right after them (there are none but with
// HP2_INDEXED=1), 32 MB at first. Till 2026-11-17 it was 16 MB for the vertices and 16 MB for the indices, and a frame with more
// of either was not drawn at all. Now both the buffer and that end of it grow (vertices_grow).
static const VkDeviceSize kVbSideStart = g_small_bufs ? 1024 * 1024 : 32 * 1024 * 1024, kVbTotalMost = (VkDeviceSize)1024 * 1024 * 1024;

static uint32_t cache_verts_now() { return g.vb_in_vram ? g.cache_verts : 0; }
static void vb_layout(VkDeviceSize total, VkDeviceSize side)
{
    g.vb_total = total, g.vb_side = side;
    g.cache_verts = (uint32_t)((total - side) / sizeof(NativeVertex));
    g.dyn_off = (VkDeviceSize)g.cache_verts * sizeof(NativeVertex);
}
// what the game's thread reads of it: as it stood when the last frame was done
static std::atomic<uint32_t> g_cache_cap_pub{0};
uint32_t native_vk_cache_vertices() { return g_cache_cap_pub.load(); }
static std::atomic<bool> g_geo_ok_pub{false};
bool native_vk_geo_ok() { return g_geo_ok_pub.load(); }
// A merged draw's command: VkDrawIndirectCommand, or for the strips by indices VkDrawIndexedIndirectCommand, in cells of one size
struct MergeCmd {
    uint32_t count, instances, first, d, e;     // not indexed: d the instance; indexed: d the vertex offset (0), e the instance
};

static void device_vertices()
{
    static bool tried;
    if (tried || getenv("HP2_HOSTBUFFERS")) return;
    tried = true;
    for (VkDeviceSize total : {(VkDeviceSize)256 << 20, (VkDeviceSize)128 << 20, (VkDeviceSize)64 << 20}) {
        if (g_small_bufs && total != (VkDeviceSize)64 << 20) continue;
        if (make_buffer_in(g.vb_dev, nullptr, total, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &g.vb_dev_mem)) {
            g.vb_in_vram = true;
            vb_layout(total, kVbSideStart);
            fprintf(g_hle_log, "native: %llu MB of the GPU's memory for the vertices, %u of them cached\n", (unsigned long long)(total >> 20), g.cache_verts);
            return;
        }
    }
    fprintf(g_hle_log, "native: no room for the vertices in the GPU's memory: read from the host's\n");
}

static bool vertex_room(VkDeviceSize bytes)
{
    if (bytes <= g.vb_cap) return true;
    VkDeviceSize cap = 16 * 1024 * 1024;
    while (cap < bytes) cap *= 2;
    if (g.vb_cap) {
        vkDestroyBuffer(g.dev, g.vb_stage, nullptr);
        vkFreeMemory(g.dev, g.vb_stage_mem, nullptr);
        g.vb_stage = VK_NULL_HANDLE;
        g.vb_cap = 0;
    }
    if (!make_buffer_in(g.vb_stage, &g.vb_map, cap,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &g.vb_stage_mem))
        return false;
    g.vb_cap = cap;
    return true;
}

// ---- the buffers that grow ------------------------------------------------------------------------------
NativeLimit g_native_limits[NL_COUNT];

// A buffer kept by hand: made, or made anew at another size in place of the one there (which is then destroyed: only with no
// frame of the GPU's in flight). Not written down for rebuild(). False: none could be made, and the old one stays.
static bool own_buffer(VkBuffer &b, VkDeviceMemory &mem, void **map, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags need,
                       VkMemoryPropertyFlags prefer = 0)
{
    const bool tracked = g_tracking;
    g_tracking = false;
    VkBuffer made = VK_NULL_HANDLE;
    VkDeviceMemory made_mem = VK_NULL_HANDLE;
    void *made_map = nullptr;
    const bool ok = make_buffer_in(made, map ? &made_map : nullptr, size, usage, need, prefer, &made_mem);
    g_tracking = tracked;
    if (!ok) return false;
    if (b) {
        vkDestroyBuffer(g.dev, b, nullptr);
        vkFreeMemory(g.dev, mem, nullptr);
    }
    b = made, mem = made_mem;
    if (map) *map = made_map;
    return true;
}

// the scene's second set: the lights, and the merged draws' matrices and lights. Written again when either buffer is another
static void bind_frame_buffers()
{
    if (!g.light_set || !g.lights || !g.draws_buf) return;
    VkDescriptorBufferInfo lbi = {g.lights, 0, g.light_cap};
    VkDescriptorBufferInfo dbi = {g.draws_buf, 0, (VkDeviceSize)g.draws_cap * sizeof(DrawData)};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 2; i++) {
        w[i].dstSet = g.light_set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    w[0].pBufferInfo = &lbi;
    w[1].pBufferInfo = &dbi;
    vkUpdateDescriptorSets(g.dev, 2, w, 0, nullptr);
}

// The frame's lights (NativeLight, 352 bytes each): the host's copy and the GPU's (the same buffer without one). 4 MB at first,
// 11915 lights: a frame with more had none of its lights copied, and its lit draws read the frame before's. Now twice the room
// and more when a frame has more, up to 64 MB. False: no room for so many, and none could be made.
static bool lights_room(VkDeviceSize bytes)
{
    if (bytes <= g.light_cap) return true;
    VkDeviceSize cap = g.light_cap ? g.light_cap : kLightStart;
    while (cap < bytes && cap < kLightMost) cap *= 2;
    if (cap < bytes) return false;
    const VkBuffer old_dev = g.lights != g.lights_stage ? g.lights : (VkBuffer)VK_NULL_HANDLE;
    const VkDeviceMemory old_dev_mem = g.lights_mem;
    if (!own_buffer(g.lights_stage, g.lights_stage_mem, &g.lights_map, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        return false;
    VkBuffer dev = VK_NULL_HANDLE;
    VkDeviceMemory dev_mem = VK_NULL_HANDLE;
    if (!getenv("HP2_HOSTBUFFERS"))
        own_buffer(dev, dev_mem, nullptr, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (old_dev) {
        vkDestroyBuffer(g.dev, old_dev, nullptr);
        vkFreeMemory(g.dev, old_dev_mem, nullptr);
    }
    g.lights = dev ? dev : g.lights_stage;
    g.lights_mem = dev_mem;
    if (g.light_cap)
        fprintf(g_hle_log, "native: a frame has %llu lights, the buffer of them grows from %llu to %llu KB\n", (unsigned long long)(bytes / sizeof(NativeLight)),
                (unsigned long long)(g.light_cap >> 10), (unsigned long long)(cap >> 10));
    g.light_cap = cap;
    bind_frame_buffers();
    return true;
}

// The merged draws' buffers: room for so many draws and so many of their commands (a range of vertices each). A picture that
// has more than there is room for sends the rest out one by one, which is only slower.
static bool merge_room(uint32_t draws, uint32_t cmds)
{
    if (draws <= g.draws_cap && cmds <= g.cmds_cap) return true;
    uint32_t dc = g.draws_cap ? g.draws_cap : kMergeDrawsStart, cc = g.cmds_cap ? g.cmds_cap : kMergeCmdsStart;
    while (dc < draws && dc < kMergeDrawsMost) dc *= 2;
    while (cc < cmds && cc < kMergeCmdsMost) cc *= 2;
    const uint32_t was_d = g.draws_cap, was_c = g.cmds_cap;
    if (dc != g.draws_cap && own_buffer(g.draws_buf, g.draws_mem, &g.draws_map, (VkDeviceSize)dc * sizeof(DrawData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        g.draws_cap = dc;
    if (cc != g.cmds_cap && own_buffer(g.cmds_buf, g.cmds_mem, &g.cmds_map, (VkDeviceSize)cc * sizeof(MergeCmd), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        g.cmds_cap = cc;
    if ((was_d && g.draws_cap != was_d) || (was_c && g.cmds_cap != was_c))
        fprintf(g_hle_log, "native: a frame has %u draws in %u ranges, the merged draws' buffers grow from %u and %u to %u and %u\n", draws, cmds, was_d, was_c,
                g.draws_cap, g.cmds_cap);
    bind_frame_buffers();
    return g.draws_cap && g.cmds_cap;
}

// What the textures and the palettes go to the GPU through, 64 MB at first. A frame's textures that do not fit wait for the next
// frame; one that is larger than all of it (a replacement of 4096 x 4096 with its levels is 85 MB) was never uploaded. Now the
// buffer is made for it before the next frame's uploads (upload_texture sets staging_want), up to 1 GB.
static bool staging_room(VkDeviceSize bytes)
{
    if (bytes <= g.staging_cap) return true;
    VkDeviceSize cap = g.staging_cap ? g.staging_cap : kStagingStart;
    while (cap < bytes && cap < kStagingMost) cap *= 2;
    if (cap < bytes) return false;
    if (!own_buffer(g.staging, g.staging_mem, &g.staging_map, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return false;
    if (g.staging_cap)
        fprintf(g_hle_log, "native: a texture of %llu KB: the buffer the textures are uploaded through grows from %llu to %llu MB\n",
                (unsigned long long)(bytes >> 10), (unsigned long long)(g.staging_cap >> 20), (unsigned long long)(cap >> 20));
    g.staging_cap = cap;
    return true;
}

// The rects of the depth the game reads back (the sun's flare), at the picture's own scale: 8 MB at first. The rects past its
// end were not read. A large picture (5120 x 2160 at a render scale of 200 %) can need more: made anew for what the frame asks.
static void active_region(const NativeFrame &frame, float &ox, float &sx, float &sy);
static VkDeviceSize depth_rects_bytes(const NativeFrame &frame)
{
    if (frame.depth_rects.empty()) return 0;
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    VkDeviceSize bytes = 0;
    for (const NativeDepthRect &r : frame.depth_rects) {
        const int x0 = std::clamp((int)(ox + r.x * sx), 0, W - 1), y0 = std::clamp((int)(r.y * sy), 0, H - 1);
        const int w = std::clamp((int)std::ceil(r.w * sx), 1, W - x0), h = std::clamp((int)std::ceil(r.h * sy), 1, H - y0);
        bytes += (VkDeviceSize)w * h * 4;
    }
    return bytes;
}
static bool depth_room(VkDeviceSize bytes)
{
    if (bytes <= g.depth_cap) return true;
    VkDeviceSize cap = g.depth_cap ? g.depth_cap : kDepthStart;
    while (cap < bytes && cap < kDepthMost) cap *= 2;
    if (cap < bytes) return false;
    if (!own_buffer(g.depth_rb, g.depth_rb_mem, &g.depth_rb_map, cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return false;
    if (g.depth_cap) {
        fprintf(g_hle_log, "native: the depth read back for a frame is %llu KB, its buffer grows from %llu to %llu KB\n", (unsigned long long)(bytes >> 10),
                (unsigned long long)(g.depth_cap >> 10), (unsigned long long)(cap >> 10));
        g.depth_rects.clear();              // what the old buffer held of the last frame is gone with it
        g.depth_places.clear();
    }
    g.depth_cap = cap;
    return true;
}

// The GPU's buffer of vertices grows too. It is the strips' cache, then a frame's own vertices (vb_side). Looked at once a frame,
// with no frame in flight, by what the frame says of itself:
//  - its own vertices (and indices) take more than half of their end of the buffer: that end is made twice the size, and again,
//    until they take half of it or less. It was a fixed 16 MB, and a frame past it was not drawn: the picture stood still;
//  - the cache was emptied for want of room before this frame, or strips found no room in it this frame (cache_short): a buffer
//    twice the size, up to 1 GB, is asked for (if the card has no room for it, it is not asked again), so that a track with
//    more in it does not have the cache emptied over and over. Not sooner: the demos at Draw Distance 200 take the cache to
//    4.2 million vertices of the 7.3 at 256 MB without its ever being emptied, and growing costs what an emptying costs;
//  - a frame's own vertices that fit no end of the buffer the card can give: the host's copy is drawn from, with no cache
//    (as with HP2_HOSTBUFFERS), which is slower and draws everything.
// The cache's place and size are what the frames are made for (NativeFrame::dyn_base), so a change of either costs the frame in
// hand and the one being made (they are not drawn, the picture stands for two frames) and empties the cache: the game's thread
// hears of both by g_native_epoch and native_vk_cache_vertices, as when the renderer is made anew. True: so it is now.
static bool vertices_grow(const NativeFrame &frame, VkDeviceSize own)
{
    static bool refused;
    VkDeviceSize side = g.vb_side, total = g.vb_total;
    while (side < own * 2 && side < kVbTotalMost / 2) side *= 2;
    bool short_now = frame.cache_short != 0;
    while (!refused && total < kVbTotalMost && (side > total / 2 || short_now)) {
        total *= 2;
        short_now = false;
    }
    if (total > g.vb_total) {
        VkBuffer made = VK_NULL_HANDLE;
        VkDeviceMemory made_mem = VK_NULL_HANDLE;
        for (; total > g.vb_total; total /= 2)
            if (make_buffer_in(made, nullptr, total, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &made_mem))
                break;
        if (total > g.vb_total) {
            vkDestroyBuffer(g.dev, g.vb_dev, nullptr);
            vkFreeMemory(g.dev, g.vb_dev_mem, nullptr);
            g.vb_dev = made, g.vb_dev_mem = made_mem;
        } else {
            refused = true;
            fprintf(g_hle_log, "native: the vertex cache has %u of %u vertices in use (%u found no room this frame), and the GPU has no room for a larger buffer\n",
                    frame.cache_top, g.cache_verts, frame.cache_short);
        }
    }
    while (side > total / 2 && side > g.vb_side) side /= 2;
    if (own > side) {
        fprintf(g_hle_log, "native: a frame's own vertices are %llu KB, more than the GPU's buffer can take (%llu MB of %llu): drawn from the host's memory from now on, "
                           "without the cache\n", (unsigned long long)(own >> 10), (unsigned long long)(side >> 20), (unsigned long long)(total >> 20));
        vkDestroyBuffer(g.dev, g.vb_dev, nullptr);
        vkFreeMemory(g.dev, g.vb_dev_mem, nullptr);
        g.vb_dev = VK_NULL_HANDLE;
        g.vb_in_vram = false;
        return true;
    }
    if (total == g.vb_total && side == g.vb_side) return false;
    const uint32_t was = g.cache_verts;
    const VkDeviceSize was_total = g.vb_total, was_side = g.vb_side;
    vb_layout(total, side);
    fprintf(g_hle_log, "native: the GPU's buffer of vertices grows: %llu MB with %llu MB a frame's own (it was %llu and %llu), %u vertices cached (%u); the frame had "
                       "%llu KB of its own, the cache %u in use, %u without room\n", (unsigned long long)(total >> 20), (unsigned long long)(side >> 20),
            (unsigned long long)(was_total >> 20), (unsigned long long)(was_side >> 20), g.cache_verts, was, (unsigned long long)(own >> 10), frame.cache_top,
            frame.cache_short);
    return true;
}

// A texture's descriptor set. The pool made with the rest (g.pool) has room for kTexSets of them (4096); it was the only one,
// and nothing looked whether a set was had. Now a pool that is full is followed by another (written down for rebuild(), which
// takes the textures apart with the rest). False: no set.
static bool texture_set(VkTex *t)
{
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g.set_layout;
    for (size_t i = 0; i <= g.tex_pools.size(); i++) {
        da.descriptorPool = i ? g.tex_pools[i - 1] : g.pool;
        if (vkAllocateDescriptorSets(g.dev, &da, &t->set) == VK_SUCCESS) {
            t->pool = da.descriptorPool;
            return true;
        }
    }
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexSets};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = kTexSets;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    const bool tracked = g_tracking;
    g_tracking = true;
    const VkResult made = vkCreateDescriptorPool(g.dev, &dp, nullptr, &pool);
    g_tracking = tracked;
    if (made != VK_SUCCESS) return false;
    g.tex_pools.push_back(pool);
    fprintf(g_hle_log, "native: %zu textures on the GPU, more than the pools of their sets take: pool %zu of %u more\n", g.textures.size(), g.tex_pools.size() + 1,
            kTexSets);
    da.descriptorPool = pool;
    if (vkAllocateDescriptorSets(g.dev, &da, &t->set) != VK_SUCCESS) return false;
    t->pool = pool;
    return true;
}

static void bind_view(VkDescriptorSet set, VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo di = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, nullptr);
}

static void image_barrier(VkImage img, uint32_t base, uint32_t levels, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1};
    vkCmdPipelineBarrier(g.cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// between two passes: everything written so far is there for whatever comes next
static void all_barrier()
{
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

static void depth_barrier(VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    // the copies of copy_depth_rects read it too
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g.depth;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Replaced textures, destroyed once the frame that could still name them is done: an upload records copies into its image, and a second
// upload of the same texture in that frame would otherwise destroy an image the frame's commands are about to write.
static std::vector<VkTex *> g_dead_textures;

static void free_texture(VkTex *t)
{
    vkFreeDescriptorSets(g.dev, t->pool, 1, &t->set);
    vkDestroyImageView(g.dev, t->view, nullptr);
    vkDestroyImage(g.dev, t->image, nullptr);
    vkFreeMemory(g.dev, t->mem, nullptr);
    if (t->owner) {                         // let go: the game's thread decodes it again when it is next asked for
        std::lock_guard<std::mutex> hold(g_native_tex_mutex);
        t->owner->vk = nullptr;
        t->owner->loaded = false;
    }
    delete t;
}

// The levels the game does not have, down to one texel, for the setting's anisotropic filter. Most of the world's textures come with one
// level (the grass, the walls, the cars' skins) or a few: the GS takes them at level 0 however far they are. The GPU's filter needs every
// level, or it has nothing to filter with and the texture only shimmers the more for it. Each new level is the one above it halved.
// A cut-out (a fence, leaves: alpha either side of the alpha test's 0x30, little in between) keeps the share of its texels that pass the
// test, as the base has it, or its thin parts would average away with distance; its colours are averaged by alpha, so that what is cut out
// does not darken the edges.
static void more_levels(NativeTexUpload &tex)
{
    const int have = tex.levels;
    int full = 1;
    while ((std::max(tex.w, tex.h) >> full) > 0) full++;
    if (have >= full || tex.w <= 0 || tex.h <= 0) return;
    size_t texels = 0;
    for (int l = 0; l < have; l++) texels += (size_t)std::max(tex.w >> l, 1) * std::max(tex.h >> l, 1);
    size_t all = 0;
    for (int l = 0; l < full; l++) all += (size_t)std::max(tex.w >> l, 1) * std::max(tex.h >> l, 1);
    if (tex.px.size() == all) {             // made already, by an upload that found no room that frame
        tex.gpu_levels = full;
        return;
    }
    if (tex.px.size() != texels) return;    // not laid out as its levels say: left as it is
    const size_t base_n = (size_t)tex.w * tex.h;
    size_t low = 0, high = 0, pass = 0;
    for (size_t i = 0; i < base_n; i++) {
        const uint32_t a = tex.px[i] >> 24;
        low += a <= 48;
        high += a >= 96;
        pass += a > 48;
    }
    const bool cutout = low * 50 >= base_n && high * 50 >= base_n && (low + high) * 100 >= base_n * 85;
    const double coverage = (double)pass / (double)base_n;
    size_t from = texels - (size_t)std::max(tex.w >> (have - 1), 1) * std::max(tex.h >> (have - 1), 1);
    for (int l = have; l < full; l++) {
        const int sw = std::max(tex.w >> (l - 1), 1), sh = std::max(tex.h >> (l - 1), 1), dw = std::max(tex.w >> l, 1), dh = std::max(tex.h >> l, 1);
        const size_t to = tex.px.size();
        tex.px.resize(to + (size_t)dw * dh);
        const uint32_t *src = tex.px.data() + from;
        uint32_t *dst = tex.px.data() + to;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                const int x0 = std::min(x * 2, sw - 1), x1 = std::min(x * 2 + 1, sw - 1), y0 = std::min(y * 2, sh - 1), y1 = std::min(y * 2 + 1, sh - 1);
                const uint32_t p[4] = {src[(size_t)y0 * sw + x0], src[(size_t)y0 * sw + x1], src[(size_t)y1 * sw + x0], src[(size_t)y1 * sw + x1]};
                uint32_t c[3] = {}, a = 0, plain[3] = {};
                for (uint32_t t : p) {
                    const uint32_t ta = t >> 24;
                    a += ta;
                    for (int k = 0; k < 3; k++) {
                        c[k] += (t >> (8 * k) & 255) * ta;
                        plain[k] += t >> (8 * k) & 255;
                    }
                }
                uint32_t out = ((a + 2) / 4) << 24;
                for (int k = 0; k < 3; k++) out |= (cutout && a ? (c[k] + a / 2) / a : (plain[k] + 2) / 4) << (8 * k);
                dst[(size_t)y * dw + x] = out;
            }
        if (cutout) {
            // the alpha's scale at which as many texels pass as in the base
            const size_t n = (size_t)dw * dh;
            double lo = 0.25, hi = 8.0;
            for (int step = 0; step < 12; step++) {
                const double s = (lo + hi) / 2;
                size_t ok = 0;
                for (size_t i = 0; i < n; i++) ok += (double)(dst[i] >> 24) * s > 48.0;
                if ((double)ok / (double)n > coverage) hi = s; else lo = s;
            }
            const double s = (lo + hi) / 2;
            for (size_t i = 0; i < n; i++) {
                const uint32_t a = (uint32_t)std::min((double)(dst[i] >> 24) * s + 0.5, 255.0);
                dst[i] = (dst[i] & 0xFFFFFFu) | a << 24;
            }
        }
        from = to;
    }
    tex.gpu_levels = full;
}

// a decoded texture into the GPU, with the game's own levels; with the setting's anisotropic filter, the rest of them made here (more_levels)
// False: no room in this frame's staging, the next frame's then.
static bool upload_texture(NativeTexUpload &tex, VkDeviceSize &at)
{
    // not for a texture that changes every few frames (a movie's, a screen's): it is 2D, and its levels would be made each time
    VkTex *const was = (VkTex *)tex.tex->vk;
    const bool often = was && g.frame - was->born < 120;
    tex.gpu_levels = tex.levels;
    if (g.aniso && !often) more_levels(tex);
    VkDeviceSize bytes = (VkDeviceSize)tex.px.size() * 4;
    if (bytes > g.staging_cap) {            // larger than all of the staging: that grows before the next frame's uploads (draw_frame)
        if (bytes > kStagingMost || (g.staging_refused && bytes >= g.staging_refused)) return true;     // never fits: it stays as it is
        if (bytes > g.staging_want) g.staging_want = bytes;
        return false;
    }
    if (at + bytes > g.staging_cap) return false;
    if (was) {
        g.textures.erase(std::find(g.textures.begin(), g.textures.end(), was));
        was->owner = nullptr;               // replaced, not let go: the game's thread goes on counting it as loaded
        g_dead_textures.push_back(was);     // destroyed when the last frame's and this frame's commands are done (free_dead_textures)
        tex.tex->vk = nullptr;
    }
    uint32_t levels = (uint32_t)tex.gpu_levels;
    VkTex *t = new VkTex();
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {(uint32_t)tex.w, (uint32_t)tex.h, 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(g.dev, &ii, nullptr, &t->image);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(g.dev, t->image, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g.dev, &ai, nullptr, &t->mem);
    vkBindImageMemory(g.dev, t->image, t->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    vkCreateImageView(g.dev, &vi, nullptr, &t->view);
    if (!texture_set(t)) {                  // no set for it and no pool for another: left without a picture
        static int told;
        if (told++ < 5) fprintf(g_hle_log, "native: no descriptor set for a texture (%zu of them on the GPU)\n", g.textures.size());
        vkDestroyImageView(g.dev, t->view, nullptr);
        vkDestroyImage(g.dev, t->image, nullptr);
        vkFreeMemory(g.dev, t->mem, nullptr);
        delete t;
        return true;
    }
    // The sampler is the GPU's filter's (a draw with tex_mode's bit 32); the GS's own sampling fetches texels and does not ask it. With the
    // setting's anisotropic filter every texture is taken linear between texels and levels when small: "nearest, mip linear", which most of
    // the world's textures have, is the GS's way of saving time, and it is what kept the filter off them.
    bind_view(t->set, t->view, g.sampler[(tex.repeat_s ? 1 : 0) | (tex.repeat_t ? 2 : 0) | (tex.mag_linear ? 4 : 0) | (tex.min_linear || g.aniso ? 8 : 0) |
                                         (tex.mip_linear || g.aniso ? 16 : 0)]);
    t->born = g.frame;

    memcpy((char *)g.staging_map + at, tex.px.data(), (size_t)bytes);
    image_barrier(t->image, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // the game's own levels: they are not a filter of the base
    VkDeviceSize place = at;
    for (uint32_t l = 0; l < levels; l++) {
        VkBufferImageCopy c = {};
        c.bufferOffset = place;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        c.imageExtent = {(uint32_t)std::max(tex.w >> l, 1), (uint32_t)std::max(tex.h >> l, 1), 1};
        vkCmdCopyBufferToImage(g.cmd, g.staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        place += (VkDeviceSize)c.imageExtent.width * c.imageExtent.height * 4;
    }
    at += bytes;
    image_barrier(t->image, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    t->owner = tex.tex;
    t->used = g.frame;
    tex.tex->vk = t;
    g.textures.push_back(t);
    return true;
}

// with no frame of the GPU's in flight: after the wait for the last one, before this one's uploads
static void free_dead_textures()
{
    for (VkTex *t : g_dead_textures) free_texture(t);
    g_dead_textures.clear();
}

static bool make_image(VkImage &img, VkImageView &view, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, int iw = 0, int ih = 0, uint32_t levels = 1,
                       VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {(uint32_t)(iw ? iw : W), (uint32_t)(ih ? ih : H), 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.usage = usage;
    if (vkCreateImage(g.dev, &ii, nullptr, &img)) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(g.dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.dev, &ai, nullptr, &m) || vkBindImageMemory(g.dev, img, m, 0))
        return false;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    return vkCreateImageView(g.dev, &vi, nullptr, &view) == VK_SUCCESS;
}

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bytes;
    si.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g.dev, &si, nullptr, &m);
    return m;
}

static bool setup_resources();
static bool g_stats_query;                  // the device counts the fragment shader's runs (HP2_DRAWPROF)

// setup_resources, and the pipelines it asked for (pipes_begin, pipes_end above)
static bool make_resources()
{
    const double t0 = pipe_clock();
    pipes_begin();
    const bool made = pipes_end(setup_resources());
    fprintf(g_hle_log, "native: the renderer's own things made in %.0f ms\n", pipe_clock() - t0);
    return made;
}

static bool setup()
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) return false;
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) return false;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!gipa) return false;
    vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;    // what the depth's resolve needs; a GPU below it runs without multisampling
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    // VK_KHR_get_surface_capabilities2 where the loader has it: what VK_EXT_full_screen_exclusive asks for (make_swapchain)
    bool caps2 = false;
    if (auto list = (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties")) {
        uint32_t en = 0;
        list(nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ext(en);
        if (en) list(nullptr, &en, ext.data());
        for (uint32_t i = 0; i < en; i++)
            if (!strcmp(ext[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) caps2 = true;
    }
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
    ci.enabledExtensionCount = caps2 ? 3 : 2;
    ci.ppEnabledExtensionNames = instance_extensions;
    VkInstance inst;
    if (!vkCreateInstance || vkCreateInstance(&ci, nullptr, &inst)) return false;
    g.instance = inst;
#define NV_LOAD_I(name) name = (PFN_##name)gipa(inst, #name);
    NV_INSTANCE(NV_LOAD_I)
    auto make_surface = (PFN_vkCreateWin32SurfaceKHR)gipa(inst, "vkCreateWin32SurfaceKHR");
    VkWin32SurfaceCreateInfoKHR surf = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surf.hinstance = GetModuleHandleA(nullptr);
    surf.hwnd = (HWND)window_handle();
    if (!surf.hwnd || !make_surface || make_surface(inst, &surf, nullptr, &g.surface)) return false;
    // The GPU: the first that can show on the window, the discrete ones before the others. One that cannot (a second card with no screen of
    // its own, on some systems) is passed over, not taken and then given up on.
    uint32_t n = 8;
    VkPhysicalDevice pd[8], phys = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &n, pd);
    uint32_t family = UINT32_MAX;
    // The player's own choice first (Options > Graphics > Resolution > Select GPU; hp2.ini's gpu, by name; the user, 2026-11-17), if it is
    // there and can show on the window. And the names of all of them, for that menu.
    {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd[i], &p);
            if (std::find(names.begin(), names.end(), std::string(p.deviceName)) == names.end()) names.push_back(p.deviceName);
            if (phys || !g_cfg.gpu[0] || strcmp(p.deviceName, g_cfg.gpu)) continue;
            uint32_t qn = 16;
            VkQueueFamilyProperties qp[16];
            vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, qp);
            for (uint32_t q = 0; q < qn && !phys; q++) {
                VkBool32 shows = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd[i], q, g.surface, &shows);
                if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && shows) phys = pd[i], family = q;
            }
            if (!phys) fprintf(g_hle_log, "native: %s, the card chosen, cannot show on the window: another is taken\n", p.deviceName);
        }
        if (g_cfg.gpu[0] && !phys) fprintf(g_hle_log, "native: the card chosen (%s) is not there or cannot be used: the game chooses\n", g_cfg.gpu);
        std::lock_guard<std::mutex> hold(g_gpu_mutex);
        g_gpu_names = names;
    }
    for (int want_discrete = 1; want_discrete >= 0 && !phys; want_discrete--)
        for (uint32_t i = 0; i < n && !phys; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd[i], &p);
            if ((p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) != (want_discrete == 1)) continue;
            uint32_t qn = 16;
            VkQueueFamilyProperties qp[16];
            vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, qp);
            for (uint32_t q = 0; q < qn && !phys; q++) {
                VkBool32 shows = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd[i], q, g.surface, &shows);
                if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && shows) phys = pd[i], family = q;
            }
            if (!phys) fprintf(g_hle_log, "native: %s cannot show on the window, passed over\n", p.deviceName);
        }
    if (!phys) return false;
    g.phys = phys;
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys, &p);
        std::lock_guard<std::mutex> hold(g_gpu_mutex);
        g_gpu_now = p.deviceName;
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &g.mem);
    bool exclusive_ext = false;
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ext(en);
        if (en) vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, ext.data());
        for (uint32_t i = 0; i < en; i++)
            if (!strcmp(ext[i].extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)) exclusive_ext = true;
    }
    g.no_exclusive = caps2 && exclusive_ext && !getenv("HP2_EXCLUSIVE");    // HP2_EXCLUSIVE: the driver's own choice, as before, for comparing
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VkPhysicalDeviceFeatures has = {}, want = {};
    vkGetPhysicalDeviceFeatures(phys, &has);
    want.depthClamp = has.depthClamp;       // the GS clamps Z; without it the nearest of the mirror's geometry is clipped
    want.samplerAnisotropy = has.samplerAnisotropy;
    want.shaderClipDistance = has.shaderClipDistance;
    want.sampleRateShading = has.sampleRateShading;     // the shadows over a multisampled picture, a sample at a time (gl_SampleID)
    g.sample_shading = has.sampleRateShading != VK_FALSE && !getenv("HP2_NOMSSHADOW");
    // the draws merged (draw_target): many draws in one call, each with an instance number of its own. HP2_NOMERGE=1: one by one
    want.multiDrawIndirect = has.multiDrawIndirect;
    // the strips' triangles by indices (native.cpp): an index is a vertex's place in the whole buffer, up to a gigabyte of it
    want.fullDrawIndexUint32 = has.fullDrawIndexUint32;
    g_geo_ok_pub = has.fullDrawIndexUint32 != VK_FALSE;
    want.drawIndirectFirstInstance = has.drawIndirectFirstInstance;
    // HP2_DRAWPROF's count of the pixels shaded; asked of the device only for that
    if (getenv("HP2_DRAWPROF")) want.pipelineStatisticsQuery = has.pipelineStatisticsQuery;
    g_stats_query = want.pipelineStatisticsQuery != VK_FALSE;
    g.can_merge = has.multiDrawIndirect && has.drawIndirectFirstInstance && !getenv("HP2_NOMERGE");
    g.clip_far = has.shaderClipDistance && !getenv("HP2_NOCLIPFAR");
    g.can_aniso = has.samplerAnisotropy != VK_FALSE;
    g.depth_clamp = want.depthClamp;
    vkGetPhysicalDeviceProperties(phys, &g.props);
    di.pEnabledFeatures = &want;
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME};
    di.enabledExtensionCount = g.no_exclusive ? 2 : 1;
    di.ppEnabledExtensionNames = device_extensions;
    if (vkCreateDevice(phys, &di, nullptr, &g.dev)) return false;
    // who draws, for a report of a picture that is wrong on one kind of card
    fprintf(g_hle_log, "native: %s, vendor %04x, driver %u.%u.%u (%08x), Vulkan %u.%u.%u; the screen %s\n", g.props.deviceName, g.props.vendorID,
            VK_VERSION_MAJOR(g.props.driverVersion), VK_VERSION_MINOR(g.props.driverVersion), VK_VERSION_PATCH(g.props.driverVersion), g.props.driverVersion,
            VK_VERSION_MAJOR(g.props.apiVersion), VK_VERSION_MINOR(g.props.apiVersion), VK_VERSION_PATCH(g.props.apiVersion),
            g.no_exclusive ? "is never taken by the driver for itself (VK_EXT_full_screen_exclusive)" : "is the driver's to take: no VK_EXT_full_screen_exclusive");
#define NV_LOAD_D(name) name = (PFN_##name)vkGetDeviceProcAddr(g.dev, #name);
    NV_DEVICE(NV_LOAD_D)
    vkGetDeviceQueue(g.dev, family, 0, &g.queue);
    g.family = family;
#define NV_STAND_IN(name) real_##name = name, name = track_##name;
    NV_STAND_IN(vkCreateBuffer) NV_STAND_IN(vkAllocateMemory) NV_STAND_IN(vkCreateImage) NV_STAND_IN(vkCreateImageView)
    NV_STAND_IN(vkCreateRenderPass) NV_STAND_IN(vkCreateFramebuffer) NV_STAND_IN(vkCreateShaderModule) NV_STAND_IN(vkCreatePipelineLayout)
    NV_STAND_IN(vkCreateCommandPool) NV_STAND_IN(vkCreateFence) NV_STAND_IN(vkCreateSemaphore) NV_STAND_IN(vkCreateSampler)
    NV_STAND_IN(vkCreateDescriptorSetLayout) NV_STAND_IN(vkCreateDescriptorPool) NV_STAND_IN(vkCreateGraphicsPipelines)
    g_tracking = true;
    const bool made = make_resources();
    g_tracking = false;
    return made;
}

// the picture's size, the multisampling, the filter: what the settings say
// The picture's format. B8G8R8A8, the GS's own 8 bits a channel, as it always was; with Dithering on (2026-11-18, the user: "it does
// not seem to work") R16G16B16A16_UNORM, 16 bits a channel. Dithering was a pass over the finished 8-bit picture that smoothed what
// lay within a few steps of its neighbours: with every step of the way kept to 8 bits (each blend, the fog, the shadows, the
// brightness and gamma) there was nothing between two steps left to show, and a band wider than the pass's reach stayed a band. Now
// the picture keeps what the shading and the blends really give, the steps of 1 / 255 are made once, where the picture goes to the
// window, and the noise laid on just before that (native_extra.frag) turns them into grain instead of bands. UNORM and not a float
// format: the GS's colours stop at 255 at every blend, and so must these. Everything the game draws into has the format (the user,
// 2026-11-18: "the whole render"): the picture, its multisampled twin, its copy for the effects and the last frame's copy; the
// mirror; the six views of the reflections, the sphere map folded from them and its copy. One format for all of them is also what
// lets one set of pipelines draw into the picture and into the views (without multisampling they share it: the passes must agree).
// The textures are the game's own 8 bits, and what is banded in them stays so. Not where a test reads a picture back as 4 bytes a
// pixel (HP2_NATIVESHOT, HP2_PIXAB, HP2_ENVLOG), not with HP2_NATIVEEXACT; HP2_COLOR8=1: never.
static VkFormat g_color_format = VK_FORMAT_B8G8R8A8_UNORM;
static bool g_color_wide;
static void choose_color_format()
{
    static const bool never = getenv("HP2_NATIVEEXACT") || getenv("HP2_NATIVESHOT") || getenv("HP2_PIXAB") || getenv("HP2_ENVLOG") || getenv("HP2_COLOR8");
    g_color_format = VK_FORMAT_B8G8R8A8_UNORM;
    g_color_wide = false;
    if (!g_cfg.deband || never) return;
    const VkFormat wide = VK_FORMAT_R16G16B16A16_UNORM;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatProperties fp = {};
    vkGetPhysicalDeviceFormatProperties(g.phys, wide, &fp);
    bool ok = (fp.optimalTilingFeatures & need) == need;
    if (ok && g.samples != VK_SAMPLE_COUNT_1_BIT) {
        VkImageFormatProperties ip = {};
        ok = vkGetPhysicalDeviceImageFormatProperties(g.phys, wide, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ip) == VK_SUCCESS &&
             (ip.sampleCounts & (VkSampleCountFlags)g.samples) != 0;
    }
    static int told = -1;
    if (told != (int)ok) {
        told = ok;
        fprintf(g_hle_log, "native: dithering: the picture has %s\n", ok ? "16 bits a channel (the mirror and the reflections too), dithered as it goes to the window"
                                                                        : "8 bits a channel (the card has no 16-bit picture for this): the old smoothing pass");
    }
    if (ok) g_color_format = wide, g_color_wide = true;
}

static uint64_t settings_key()
{
    const uint32_t rest = (uint32_t)(g_cfg.msaa() << 8 | g_cfg.aniso << 16 | (g_cfg.aa_mode == 2) << 24 | (g_cfg.reflections == 2) << 25 |
                                     (g_cfg.shadows >= 2 ? g_cfg.shadows - 1 : 0) << 26 | (g_cfg.deband != 0) << 28);
    return (uint64_t)(uint32_t)native_aspect() << 32 | rest;
}

// what the menu may offer: the most samples the picture can have, the most anisotropy
bool native_vk_limits(int &aa, int &aniso)
{
    if (!g.ready) return false;
    aa = 0;
    if (g.props.apiVersion >= VK_API_VERSION_1_2) {
        const VkSampleCountFlags can = g.props.limits.framebufferColorSampleCounts & g.props.limits.framebufferDepthSampleCounts;
        for (int s = 8; s >= 2 && !aa; s /= 2)
            if (can & (VkSampleCountFlags)s) aa = s;
    }
    aniso = 0;
    if (g.can_aniso)
        for (int s = 16; s >= 2 && !aniso; s /= 2)
            if ((float)s <= g.props.limits.maxSamplerAnisotropy) aniso = s;
    return true;
}

// the height of the 4:3 box the window has now: by the swapchain's images, before there are any by the window itself
static int lines_now()
{
    int w = g.swapchain ? g.swap_w : 0, h = g.swapchain ? g.swap_h : 0;
    if (w <= 0 || h <= 0) {
        RECT r = {};
        GetClientRect((HWND)window_handle(), &r);
        w = r.right, h = r.bottom;
    }
    if (w <= 0 || h <= 0) return 0;         // no area (minimized): what there was stays
    if (g_cfg.fullscreen && g_cfg.fs_w > 0 && g_cfg.fs_h > 0) w = std::min(w, g_cfg.fs_w), h = std::min(h, g_cfg.fs_h);
    const int lines = std::min(h, w * 3 / 4);
    return std::clamp((lines * std::clamp(g_cfg.render_scale, 20, 200) + 50) / 100, 224, 4320);
}

// Each frame: the window's size as the picture is to follow it. A size that stays a quarter of a second counts, so a window being dragged
// by its edge does not make the renderer again at every step (the picture is scaled to it meanwhile).
static void track_lines()
{
    static const bool fixed = getenv("HP2_NATIVESIZE") != nullptr;
    if (fixed) return;
    static int seen;
    static ULONGLONG since;
    const int now = lines_now();
    if (!now || now == g_lines) {
        seen = now;
        return;
    }
    const ULONGLONG t = GetTickCount64();
    if (now != seen) seen = now, since = t;
    else if (t - since >= 250) g_lines = now;
}

static bool setup_resources()
{
    if (!g_lines) g_lines = lines_now();
    g.lines = g_lines;
    g.key = settings_key();
    const VkPhysicalDeviceProperties &props = g.props;
    const uint32_t family = g.family;
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    g.aniso = !exact && g.can_aniso ? std::min(g_cfg.aniso, (int)props.limits.maxSamplerAnisotropy) : 0;
    g.samples = VK_SAMPLE_COUNT_1_BIT;
    if (!exact && g_cfg.msaa() > 1 && props.apiVersion >= VK_API_VERSION_1_2) {
        VkSampleCountFlags can = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
        for (int s = g_cfg.msaa(); s >= 2 && g.samples == VK_SAMPLE_COUNT_1_BIT; s /= 2)
            if (can & (VkSampleCountFlags)s) g.samples = (VkSampleCountFlagBits)s;
    }

    ENV_SIZE = !exact && g_cfg.reflections == 2 ? 128 : 64;
    SPHERE_SIZE = ENV_SIZE * 4;
    {
        const int lines = g_lines ? g_lines : 720;
        H = (lines * 448 + 443) / 444;
        W43 = (lines * 4 / 3 + 1) & ~1;
    }
    if (const char *e = getenv("HP2_NATIVESIZE")) sscanf(e, "%dx%d", &W43, &H);
    W = (int)(W43 / native_narrow() + 0.5f) & ~1;
    // (the buffers that grow are made once and kept through rebuild(): depth_room and the like only look, the second time)
    if (!make_buffer(g.fps_buf, &g.fps_map, kFpsBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !depth_room(kDepthStart)) return false;
    VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(g.dev, &semaphore, nullptr, &g.acquired) || vkCreateSemaphore(g.dev, &semaphore, nullptr, &g.rendered)) return false;
    g.light_set = VK_NULL_HANDLE;           // the one before went with its pool: none to write till it is made again below
    if (!staging_room(kStagingStart) || !lights_room(kLightStart) ||
        !make_buffer(g.fold_vb, &g.fold_map, kFoldBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !make_buffer(g.readback, &g.rb_map, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    VkImageView cv, dv;
    choose_color_format();
    if (!make_image(g.color, cv, g_color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT) ||
        !make_image(g.depth, dv, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    g.ms_read_pass = VK_NULL_HANDLE;
    g.ms_read_fb = VK_NULL_HANDLE;
    g.shadow_ms_pipeline = VK_NULL_HANDLE;
    g.depth_ms_view = VK_NULL_HANDLE;
    g.soft_ok = false;
    g.depth_copy = VK_NULL_HANDLE;
    g.depth_copy_view = VK_NULL_HANDLE;
    if (!getenv("HP2_NOSOFT")) {            // the soft sprites' copy of the depth; a card that cannot have one keeps the sprites as they were
        VkImage copy = VK_NULL_HANDLE;
        VkImageView copy_view = VK_NULL_HANDLE;
        if (make_image(copy, copy_view, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
            g.depth_copy = copy, g.depth_copy_view = copy_view;
    }
    // the picture so far, down to the GS's half buffer and a level past it; the last frame; the palettes
    g.scene_levels = 2;
    for (int m = W; m > 256; m >>= 1) g.scene_levels++;
    VkImageView scene_view, prev_view, palette_view;
    if (!make_image(g.scene, scene_view, g_color_format,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, W, H, g.scene_levels) ||
        !make_image(g.prev, prev_view, g_color_format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !make_image(g.palette, palette_view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 256, kPaletteRows))
        return false;

    // Every render pass waits for all that came before it, and all that comes after waits for it, the layout changes at its start and end
    // included. Without these the passes' own changes of layout raced the copies and the passes around them: NVIDIA's GPUs keep them in
    // order anyway, AMD's do not (blocks of black and old pictures in the menus, 2026-11-06; the validation layer's sync checks name them).
    static const VkAccessFlags kAll = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    static const VkSubpassDependency kDeps[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, kAll, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, kAll, 0}};
    VkAttachmentDescription at[2] = {};
    at[0].format = g_color_format;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    at[1] = at[0];
    at[1].format = VK_FORMAT_D32_SFLOAT;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cr;
    sub.pDepthStencilAttachment = &dr;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = at;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = getenv("HP2_NODEPS") ? 0 : 2;
    rp.pDependencies = kDeps;
    if (vkCreateRenderPass(g.dev, &rp, nullptr, &g.pass)) return false;
    // the picture again after an effect
    at[0].loadOp = at[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (vkCreateRenderPass(g.dev, &rp, nullptr, &g.pass_load)) return false;
    at[0].loadOp = at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].initialLayout = at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageView views[2] = {cv, dv};
    VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = g.pass;
    fb.attachmentCount = 2;
    fb.pAttachments = views;
    fb.width = W;
    fb.height = H;
    fb.layers = 1;
    if (vkCreateFramebuffer(g.dev, &fb, nullptr, &g.fb)) return false;

    // the envmap views, in the picture's format (g_color_format); the mirror's and the fold's passes below are made from this one too
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkCreateRenderPass(g.dev, &rp, nullptr, &g.env_pass)) return false;
    // the same going on with what a pass before left (the mirror, after its shadows)
    at[0].loadOp = at[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (vkCreateRenderPass(g.dev, &rp, nullptr, &g.env_pass_load)) return false;
    at[0].loadOp = at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].initialLayout = at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Multisampling: the picture proper is in samples, and each pass ends by resolving it, colour and depth, into the images everything else reads
    if (g.samples != VK_SAMPLE_COUNT_1_BIT) {
        real_vkCreateRenderPass2 = (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(g.dev, "vkCreateRenderPass2");
        auto create2 = track_vkCreateRenderPass2;
        VkImageView msc, msd;
        if (!real_vkCreateRenderPass2 || !make_image(g.color_ms, msc, g_color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, g.samples) ||
            !make_image(g.depth_ms, msd, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1, g.samples))
            return false;
        VkAttachmentDescription2 a2[4] = {};
        for (int i = 0; i < 4; i++) {
            a2[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            a2[i].format = i & 1 ? VK_FORMAT_D32_SFLOAT : g_color_format;
            a2[i].samples = i < 2 ? g.samples : VK_SAMPLE_COUNT_1_BIT;
            a2[i].loadOp = i < 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a2[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a2[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a2[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a2[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a2[i].finalLayout = i & 1 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        a2[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference2 r2[4] = {};
        for (uint32_t i = 0; i < 4; i++) {
            r2[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
            r2[i].attachment = i;
            r2[i].layout = i & 1 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            r2[i].aspectMask = i & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        }
        VkSubpassDescriptionDepthStencilResolve resolve = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        resolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &r2[3];
        VkSubpassDescription2 sub2 = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        sub2.pNext = &resolve;
        sub2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub2.colorAttachmentCount = 1;
        sub2.pColorAttachments = &r2[0];
        sub2.pResolveAttachments = &r2[2];
        sub2.pDepthStencilAttachment = &r2[1];
        VkRenderPassCreateInfo2 rp2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        rp2.attachmentCount = 4;
        rp2.pAttachments = a2;
        rp2.subpassCount = 1;
        rp2.pSubpasses = &sub2;
        VkSubpassDependency2 deps2[2] = {};
        for (int i = 0; i < 2; i++) {
            deps2[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
            deps2[i].srcSubpass = kDeps[i].srcSubpass;
            deps2[i].dstSubpass = kDeps[i].dstSubpass;
            deps2[i].srcStageMask = kDeps[i].srcStageMask;
            deps2[i].dstStageMask = kDeps[i].dstStageMask;
            deps2[i].srcAccessMask = kDeps[i].srcAccessMask;
            deps2[i].dstAccessMask = kDeps[i].dstAccessMask;
        }
        rp2.dependencyCount = getenv("HP2_NODEPS") ? 0 : 2;
        rp2.pDependencies = deps2;
        if (create2(g.dev, &rp2, nullptr, &g.pass)) return false;
        a2[0].loadOp = a2[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a2[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a2[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (create2(g.dev, &rp2, nullptr, &g.pass_load)) return false;
        VkImageView all4[4] = {msc, msd, cv, dv};
        VkFramebufferCreateInfo mf = fb;
        mf.renderPass = g.pass;
        mf.attachmentCount = 4;
        mf.pAttachments = all4;
        if (vkCreateFramebuffer(g.dev, &mf, nullptr, &g.fb)) return false;
        // Shadows: over the samples, their depth read by the shader and not written: no resolve, the picture's passes do that
        g.depth_ms_view = msd;
        if (g.sample_shading) {
            VkAttachmentDescription ra[2] = {};
            for (int i = 0; i < 2; i++) {
                ra[i].format = i ? VK_FORMAT_D32_SFLOAT : g_color_format;
                ra[i].samples = g.samples;
                ra[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ra[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                ra[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                ra[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                ra[i].initialLayout = ra[i].finalLayout = i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            VkAttachmentReference rcr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, rdr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            VkSubpassDescription rsub = {};
            rsub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            rsub.colorAttachmentCount = 1;
            rsub.pColorAttachments = &rcr;
            rsub.pDepthStencilAttachment = &rdr;
            VkRenderPassCreateInfo rrp = rp;
            rrp.attachmentCount = 2;
            rrp.pAttachments = ra;
            rrp.pSubpasses = &rsub;
            if (vkCreateRenderPass(g.dev, &rrp, nullptr, &g.ms_read_pass)) return false;
            VkImageView two[2] = {msc, msd};
            VkFramebufferCreateInfo rf = fb;
            rf.renderPass = g.ms_read_pass;
            rf.attachmentCount = 2;
            rf.pAttachments = two;
            if (vkCreateFramebuffer(g.dev, &rf, nullptr, &g.ms_read_fb)) return false;
        }
    }
    VkImageView ed;
    if (!make_image(g.env_depth, ed, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, ENV_SIZE, ENV_SIZE)) return false;
    VkImageView env_view[6];
    for (int i = 0; i < 6; i++) {
        if (!make_image(g.env[i], env_view[i], g_color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, ENV_SIZE, ENV_SIZE))
            return false;
        VkImageView v2[2] = {env_view[i], ed};
        VkFramebufferCreateInfo ef = fb;
        ef.renderPass = g.env_pass;
        ef.pAttachments = v2;
        ef.width = ef.height = ENV_SIZE;
        if (vkCreateFramebuffer(g.dev, &ef, nullptr, &g.env_fb[i])) return false;
    }
    // the mirror
    g.rvm_w = std::max(W43 / 2, 1);
    g.rvm_h = std::max(H / 7, 1);
    VkImageView rv, rd;
    if (!make_buffer(g.rvm_rb, &g.rvm_rb_map, (VkDeviceSize)g.rvm_w * g.rvm_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    if (!make_image(g.rvm, rv, g_color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, g.rvm_w, g.rvm_h) ||
        !make_image(g.rvm_depth, rd, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    g.rvm_w, g.rvm_h))
        return false;
    {
        VkImageView rvv[2] = {rv, rd};
        VkFramebufferCreateInfo rf = fb;
        rf.renderPass = g.env_pass;
        rf.pAttachments = rvv;
        rf.width = g.rvm_w;
        rf.height = g.rvm_h;
        if (vkCreateFramebuffer(g.dev, &rf, nullptr, &g.rvm_fb)) return false;
    }
    // Shadows: High. The shadows over the mirror: its colour alone, its depth being read
    {
        g.rvm_depth_view = rd;
        VkAttachmentDescription ma = at[0];
        ma.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ma.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ma.initialLayout = ma.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference mcr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription msub = {};
        msub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        msub.colorAttachmentCount = 1;
        msub.pColorAttachments = &mcr;
        VkRenderPassCreateInfo mrp = rp;
        mrp.attachmentCount = 1;
        mrp.pAttachments = &ma;
        mrp.pSubpasses = &msub;
        if (vkCreateRenderPass(g.dev, &mrp, nullptr, &g.rvm_post_pass)) return false;
        VkFramebufferCreateInfo mf = fb;
        mf.renderPass = g.rvm_post_pass;
        mf.attachmentCount = 1;
        mf.pAttachments = &rv;
        mf.width = g.rvm_w;
        mf.height = g.rvm_h;
        if (vkCreateFramebuffer(g.dev, &mf, nullptr, &g.rvm_post_fb)) return false;
    }
    // and the map of the cars as the sun sees them: depth alone
    {
        // Ultra (a test setting, the user, 2026-11-17): 1024, a map of 8192 x 8192, 256 MB; not on a card with no image that large
        g.shadow_cell = g_cfg.shadows >= 3 ? 1024 : g_cfg.shadows == 2 ? 512 : 256;
        // Ultra has two rows more below the glass, for the tunnels' lamps (native.cpp, "the tunnels' lamps"): 8192 x 10240, 320 MB
        g.lamp_row = g_cfg.shadows >= 2;     // Very High (4096 x 5120, 80 MB) and Ultra
        const int rows = kShadowRows + (g.lamp_row ? kLampRows : 0);
        if ((uint32_t)(g.shadow_cell * rows) > g.props.limits.maxImageDimension2D) g.shadow_cell = 512;
        g.shadow_h = g.shadow_cell * rows;
        g.shadow_ready = false;
        if (!make_image(g.shadow_map, g.shadow_view, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT, g.shadow_cell * kShadowAcross, g.shadow_h))
            return false;
        VkAttachmentDescription sa = at[1];
        sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sa.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference sdr = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription ssub = {};
        ssub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        ssub.pDepthStencilAttachment = &sdr;
        VkRenderPassCreateInfo srp = rp;
        srp.attachmentCount = 1;
        srp.pAttachments = &sa;
        srp.pSubpasses = &ssub;
        if (vkCreateRenderPass(g.dev, &srp, nullptr, &g.shadow_pass)) return false;
        VkFramebufferCreateInfo sf = fb;
        sf.renderPass = g.shadow_pass;
        sf.attachmentCount = 1;
        sf.pAttachments = &g.shadow_view;
        sf.width = g.shadow_cell * kShadowAcross;
        sf.height = g.shadow_h;
        if (vkCreateFramebuffer(g.dev, &sf, nullptr, &g.shadow_fb)) return false;
    }
    // Ambient Occlusion: its image, half the picture each way, and the pass that makes it
    {
        g.ao_w = (W + 1) / 2, g.ao_h = (H + 1) / 2;
        if (!make_image(g.depth_world, g.depth_world_view, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT))
            return false;
        g.world_ready = false;
        if (!make_image(g.ao, g.ao_view, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, g.ao_w,
                        g.ao_h))
            return false;
        VkAttachmentDescription aa = at[0];
        aa.format = VK_FORMAT_R8_UNORM;
        aa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        aa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        aa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aa.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference acr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription asub = {};
        asub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        asub.colorAttachmentCount = 1;
        asub.pColorAttachments = &acr;
        VkRenderPassCreateInfo arp = rp;
        arp.attachmentCount = 1;
        arp.pAttachments = &aa;
        arp.pSubpasses = &asub;
        if (vkCreateRenderPass(g.dev, &arp, nullptr, &g.ao_pass)) return false;
        VkFramebufferCreateInfo af = fb;
        af.renderPass = g.ao_pass;
        af.attachmentCount = 1;
        af.pAttachments = &g.ao_view;
        af.width = g.ao_w;
        af.height = g.ao_h;
        if (vkCreateFramebuffer(g.dev, &af, nullptr, &g.ao_fb)) return false;
    }
    // the sphere map: kept from frame to frame
    VkImageView sv;
    if (!make_buffer(g.sphere_rb, &g.sphere_rb_map, SPHERE_SIZE * SPHERE_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    if (!make_image(g.sphere, sv, g_color_format,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, SPHERE_SIZE, SPHERE_SIZE))
        return false;
    VkAttachmentDescription fa = at[0];
    fa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    fa.initialLayout = fa.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference fr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription fs = {};
    fs.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    fs.colorAttachmentCount = 1;
    fs.pColorAttachments = &fr;
    VkRenderPassCreateInfo frp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    frp.attachmentCount = 1;
    frp.pAttachments = &fa;
    frp.subpassCount = 1;
    frp.pSubpasses = &fs;
    frp.dependencyCount = getenv("HP2_NODEPS") ? 0 : 2;
    frp.pDependencies = kDeps;
    if (vkCreateRenderPass(g.dev, &frp, nullptr, &g.fold_pass)) return false;
    VkFramebufferCreateInfo sfb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    sfb.renderPass = g.fold_pass;
    sfb.attachmentCount = 1;
    sfb.pAttachments = &sv;
    sfb.width = sfb.height = SPHERE_SIZE;
    sfb.layers = 1;
    if (vkCreateFramebuffer(g.dev, &sfb, nullptr, &g.sphere_fb)) return false;
    // an effect on the picture: colour only, the depth is being read
    VkAttachmentDescription pa = at[0];
    pa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    pa.initialLayout = pa.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    frp.pAttachments = &pa;
    if (vkCreateRenderPass(g.dev, &frp, nullptr, &g.post_pass)) return false;
    frp.pAttachments = &fa;
    VkFramebufferCreateInfo pfb = sfb;
    pfb.renderPass = g.post_pass;
    pfb.pAttachments = &cv;
    pfb.width = W;
    pfb.height = H;
    if (vkCreateFramebuffer(g.dev, &pfb, nullptr, &g.post_fb)) return false;

    VkDescriptorSetLayoutBinding sb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 1;
    sl.pBindings = &sb;
    if (vkCreateDescriptorSetLayout(g.dev, &sl, nullptr, &g.set_layout)) return false;
    // the lights, and the merged draws' matrices and lights
    // the lights are read by the shadows' fragment shader too: the tunnel lamps' blocks (native.cpp, lamps_pick)
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo ll = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ll.bindingCount = 2;
    ll.pBindings = lb;
    if (!merge_room(kMergeDrawsStart, kMergeCmdsStart)) return false;
    if (vkCreateDescriptorSetLayout(g.dev, &ll, nullptr, &g.light_layout)) return false;
    VkDescriptorSetLayoutBinding pb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo psl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    psl.bindingCount = 3;
    psl.pBindings = pb;
    if (vkCreateDescriptorSetLayout(g.dev, &psl, nullptr, &g.post_set_layout)) return false;
    // the renderer's own sets and kTexSets of the textures'; the textures past those have pools of their own (texture_set)
    VkDescriptorPoolSize dps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexSets + 80}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = kTexSets + 40;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = dps;
    if (vkCreateDescriptorPool(g.dev, &dp, nullptr, &g.pool)) return false;
    for (int i = 0; i < 32; i++) {
        VkSamplerCreateInfo sa = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sa.addressModeU = i & 1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sa.addressModeV = i & 2 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sa.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sa.magFilter = i & 4 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sa.minFilter = i & 8 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sa.mipmapMode = i & 16 || g.aniso ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sa.maxLod = VK_LOD_CLAMP_NONE;
        if (g.aniso && (i & 8)) {
            sa.anisotropyEnable = VK_TRUE;
            sa.maxAnisotropy = (float)g.aniso;
        }
        if (vkCreateSampler(g.dev, &sa, nullptr, &g.sampler[i])) return false;
    }

    VkDescriptorSetAllocateInfo la = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    la.descriptorPool = g.pool;
    la.descriptorSetCount = 1;
    la.pSetLayouts = &g.light_layout;
    if (vkAllocateDescriptorSets(g.dev, &la, &g.light_set)) return false;
    bind_frame_buffers();
    la.pSetLayouts = &g.set_layout;
    for (int i = 0; i < 6; i++) {
        if (vkAllocateDescriptorSets(g.dev, &la, &g.env_set[i])) return false;
        bind_view(g.env_set[i], env_view[i], g.sampler[4 | 8]);
    }
    if (vkAllocateDescriptorSets(g.dev, &la, &g.sphere_set)) return false;
    bind_view(g.sphere_set, sv, g.sampler[4 | 8]);
    VkImageView spv;
    if (!make_image(g.sphere_prev, spv, g_color_format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, SPHERE_SIZE, SPHERE_SIZE))
        return false;
    if (vkAllocateDescriptorSets(g.dev, &la, &g.sphere_prev_set)) return false;
    bind_view(g.sphere_prev_set, spv, g.sampler[0]);
    if (vkAllocateDescriptorSets(g.dev, &la, &g.rvm_set)) return false;
    bind_view(g.rvm_set, rv, g.sampler[4 | 8]);
    if (vkAllocateDescriptorSets(g.dev, &la, &g.prev_set)) return false;
    bind_view(g.prev_set, prev_view, g.sampler[4 | 8]);
    la.pSetLayouts = &g.post_set_layout;
    if (vkAllocateDescriptorSets(g.dev, &la, &g.post_set)) return false;
    {
        VkDescriptorImageInfo pi[3] = {{g.sampler[0], dv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g.sampler[0], palette_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g.sampler[4 | 8], scene_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet pw[3] = {};
        for (uint32_t i = 0; i < 3; i++) {
            pw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            pw[i].dstSet = g.post_set;
            pw[i].dstBinding = i;
            pw[i].descriptorCount = 1;
            pw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pw[i].pImageInfo = &pi[i];
        }
        vkUpdateDescriptorSets(g.dev, 3, pw, 0, nullptr);
    }
    // the same for the shadows over the mirror: the mirror's depth
    if (vkAllocateDescriptorSets(g.dev, &la, &g.rvm_post_set)) return false;
    {
        VkDescriptorImageInfo pi[3] = {{g.sampler[0], g.rvm_depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g.sampler[0], palette_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g.sampler[4 | 8], scene_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet pw[3] = {};
        for (uint32_t i = 0; i < 3; i++) {
            pw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            pw[i].dstSet = g.rvm_post_set;
            pw[i].dstBinding = i;
            pw[i].descriptorCount = 1;
            pw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pw[i].pImageInfo = &pi[i];
        }
        vkUpdateDescriptorSets(g.dev, 3, pw, 0, nullptr);
    }
    la.pSetLayouts = &g.set_layout;
    if (vkAllocateDescriptorSets(g.dev, &la, &g.shadow_set)) return false;
    bind_view(g.shadow_set, g.shadow_view, g.sampler[0]);
    if (vkAllocateDescriptorSets(g.dev, &la, &g.ao_set)) return false;
    bind_view(g.ao_set, g.ao_view, g.sampler[0]);
    if (vkAllocateDescriptorSets(g.dev, &la, &g.ao_world_set)) return false;
    bind_view(g.ao_world_set, g.depth_world_view, g.sampler[0]);
    if (g.depth_copy) {
        if (vkAllocateDescriptorSets(g.dev, &la, &g.soft_set)) return false;
        bind_view(g.soft_set, g.depth_copy_view, g.sampler[0]);
    }
    if (g.ms_read_pass) {                   // the post set's three, the first the samples' depth as the pass over them has it
        la.pSetLayouts = &g.post_set_layout;
        if (vkAllocateDescriptorSets(g.dev, &la, &g.ms_post_set)) return false;
        VkDescriptorImageInfo pi[3] = {{g.sampler[0], g.depth_ms_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                       {g.sampler[0], palette_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g.sampler[4 | 8], scene_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet pw[3] = {};
        for (uint32_t i = 0; i < 3; i++) {
            pw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            pw[i].dstSet = g.ms_post_set;
            pw[i].dstBinding = i;
            pw[i].descriptorCount = 1;
            pw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pw[i].pImageInfo = &pi[i];
        }
        vkUpdateDescriptorSets(g.dev, 3, pw, 0, nullptr);
        la.pSetLayouts = &g.set_layout;
    }

    VkDescriptorSetLayout both[2] = {g.set_layout, g.light_layout};
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = both;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(g.dev, &li, nullptr, &g.layout)) return false;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].pName = "main";
    const VkShaderModule vert_old = shader(kVert, sizeof kVert), frag_old = shader(kFrag, sizeof kFrag);
    const VkShaderModule vert_clip = g.clip_far ? shader(kVertClip, sizeof kVertClip) : VK_NULL_HANDLE;
    const VkShaderModule frag_clip = g.clip_far ? shader(kFragClip, sizeof kFragClip) : VK_NULL_HANDLE;
    // the old shaders as well only for HP2_PERFAB's comparison, or where there is no clip distance
    const bool old_too = getenv("HP2_PERFAB") != nullptr || getenv("HP2_PIXAB") != nullptr;
    // the shader's constants: ALPHA_TEST, and WIDE with the picture of 16 bits a channel (Dithering on: nothing rounded to a byte)
    VkBool32 spec_data[2] = {VK_TRUE, g_color_wide ? VK_TRUE : VK_FALSE};
    VkBool32 &spec_alpha_test = spec_data[0];
    VkSpecializationMapEntry spec_entry[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
    VkSpecializationInfo spec = {2, spec_entry, sizeof spec_data, spec_data};
    // the fold's: WIDE alone
    VkSpecializationMapEntry fold_spec_entry = {0, 0, sizeof(VkBool32)};
    VkSpecializationInfo fold_spec = {1, &fold_spec_entry, sizeof(VkBool32), &spec_data[1]};
    // the second binding is the same buffer again, for the place of a vertex in the picture halfway (a_pos_mid): from further on
    // in the buffer for a draw with a second copy of its vertices (NativeDraw::mid_verts), else the vertex itself
    VkVertexInputBindingDescription bind[2] = {{0, sizeof(NativeVertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(NativeVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attr[6] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                 {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                                                 {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},
                                                 {3, 0, VK_FORMAT_R8G8B8A8_SNORM, 24},
                                                 {4, 0, VK_FORMAT_R32_SFLOAT, 28},
                                                 {5, 1, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 2;
    vin.pVertexBindingDescriptions = bind;
    vin.vertexAttributeDescriptionCount = 6;
    vin.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs.viewportCount = vs.scissorCount = 1;
    vs.pViewports = &vp;
    vs.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo ra = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    ra.polygonMode = VK_POLYGON_MODE_FILL;
    ra.depthClampEnable = g.depth_clamp;
    ra.cullMode = VK_CULL_MODE_NONE;        // the GS has none
    ra.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // Z24 on the GS: larger is nearer
    VkPipelineColorBlendAttachmentState ba = {};
    ba.colorWriteMask = 0xf;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vs;
    pi.pRasterizationState = &ra;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.layout = g.layout;
    pi.pDynamicState = &dsi;
    // the shader has already multiplied the colour by the factor
    // the mirror and the envmap views are drawn with the first set, single samples; with multisampling the picture has its own
    for (int set = 0; set < (g.samples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1); set++) {
    pi.renderPass = set ? g.pass : g.env_pass;
    ms.rasterizationSamples = set ? g.samples : VK_SAMPLE_COUNT_1_BIT;
    for (int v = 0; v < 2; v++) {
    if (v == 0 ? !g.clip_far : g.clip_far && !old_too) continue;
    st[0].module = v ? vert_old : vert_clip;
    st[1].module = v ? frag_old : frag_clip;
    st[1].pSpecializationInfo = &spec;
    for (int at = 0; at < 2; at++) {
    spec_alpha_test = v == 1 || at == 1;    // the old shaders kept the test whatever the draw
    auto &pipes = (set ? g.pipeline_ms : g.pipeline)[v][at];
    auto &alpha_pipes = (set ? g.alpha_only_ms : g.alpha_only)[v][at];
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    ba.colorWriteMask = 0xf;
    for (int b = 0; b < NB_COUNT; b++)
        for (int i = 0; i < 2; i++) {
            ds.depthWriteEnable = i ? VK_TRUE : VK_FALSE;
            ba.blendEnable = b != NB_OPAQUE;
            bool by_dest = b == NB_DEST || b == NB_LERP_DEST;
            ba.srcColorBlendFactor = by_dest ? VK_BLEND_FACTOR_DST_ALPHA : b == NB_KEEP ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = b == NB_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : b == NB_LERP_DEST ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE;
            ba.colorBlendOp = b == NB_SUB ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
            // the frame's alpha takes As, not a blend; the passes by Ad leave it for the next
            ba.srcAlphaBlendFactor = by_dest ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = by_dest ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
            for (int t = 0; t < 2; t++) {
                ds.depthCompareOp = t ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
                if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipes[b][i][t])) return false;
            }
        }
    ds.depthWriteEnable = VK_FALSE;
    ba.blendEnable = VK_FALSE;
    ba.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
    for (int t = 0; t < 2; t++) {
        ds.depthCompareOp = t ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &alpha_pipes[t])) return false;
    }
    ba.colorWriteMask = 0xf;
    }
    }
    }
    st[1].pSpecializationInfo = nullptr;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Particles: High, the soft sprites: the scene's pipelines again with native.frag's SOFT, which reads the depth's copy as a third
    // set. The first two sets and the constants are the scene's own, so what the scene has bound stays bound. No depth is written.
    if (g.depth_copy) {
        VkDescriptorSetLayout soft_sets[3] = {g.set_layout, g.light_layout, g.set_layout};
        VkPipelineLayoutCreateInfo sli = li;
        sli.setLayoutCount = 3;
        sli.pSetLayouts = soft_sets;
        if (vkCreatePipelineLayout(g.dev, &sli, nullptr, &g.soft_layout)) return false;
        st[0].module = g.clip_far ? vert_clip : vert_old;
        st[1].module = shader(kFragSoft, sizeof kFragSoft);
        spec_alpha_test = VK_TRUE;
        st[1].pSpecializationInfo = &spec;
        pi.layout = g.soft_layout;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_FALSE;
        ba.colorWriteMask = 0xf;
        for (int set = 0; set < (g.samples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1); set++) {
            pi.renderPass = set ? g.pass : g.env_pass;
            ms.rasterizationSamples = set ? g.samples : VK_SAMPLE_COUNT_1_BIT;
            for (int b = 0; b < NB_COUNT; b++) {
                ba.blendEnable = b != NB_OPAQUE;
                const bool by_dest = b == NB_DEST || b == NB_LERP_DEST;
                ba.srcColorBlendFactor = by_dest ? VK_BLEND_FACTOR_DST_ALPHA : b == NB_KEEP ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
                ba.dstColorBlendFactor = b == NB_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : b == NB_LERP_DEST ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE;
                ba.colorBlendOp = b == NB_SUB ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
                ba.srcAlphaBlendFactor = by_dest ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
                ba.dstAlphaBlendFactor = by_dest ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
                ba.alphaBlendOp = VK_BLEND_OP_ADD;
                for (int t = 0; t < 2; t++) {
                    ds.depthCompareOp = t ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
                    if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.soft_pipeline[set][b][t])) return false;
                }
            }
        }
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        ba.blendEnable = VK_FALSE;
        pi.layout = g.layout;
        st[1].pSpecializationInfo = nullptr;
        g.soft_ok = true;
    }

    // Shadows: High: a car as the sun sees it, into its cell of the map. The scene's vertex shader alone: depth, the furthest kept
    {
        st[0].module = g.clip_far ? vert_clip : vert_old;
        pi.stageCount = 1;
        pi.renderPass = g.shadow_pass;
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER;
        cb.attachmentCount = 0;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.shadow_cast)) return false;
        // and with the scene's fragment shader for the draws with the alpha test: what it throws away is no car
        pi.stageCount = 2;
        st[1].module = g.clip_far ? frag_clip : frag_old;
        spec_alpha_test = VK_TRUE;
        st[1].pSpecializationInfo = &spec;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.shadow_cast_test)) return false;
        cb.attachmentCount = 1;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    }

    // the fold
    VkDescriptorSetLayout fold_sets[2] = {g.set_layout, g.set_layout};
    VkPushConstantRange fold_pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int)};
    VkPipelineLayoutCreateInfo fl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    fl.setLayoutCount = 2;
    fl.pSetLayouts = fold_sets;
    fl.pushConstantRangeCount = 1;
    fl.pPushConstantRanges = &fold_pc;
    if (vkCreatePipelineLayout(g.dev, &fl, nullptr, &g.fold_layout)) return false;
    st[0].module = shader(kFoldVert, sizeof kFoldVert);
    st[1].module = shader(kFoldFrag, sizeof kFoldFrag);
    VkVertexInputBindingDescription fbind = {0, sizeof(FoldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription fattr[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
    vin.vertexBindingDescriptionCount = 1;      // the scene's has two (the place halfway is the second)
    vin.pVertexBindingDescriptions = &fbind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = fattr;
    ds.depthTestEnable = ds.depthWriteEnable = VK_FALSE;
    ba.blendEnable = VK_FALSE;              // the shader does the GS's blend
    pi.layout = g.fold_layout;
    pi.renderPass = g.fold_pass;
    pi.pDepthStencilState = nullptr;
    st[1].pSpecializationInfo = &fold_spec;
    if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.fold_pipeline)) return false;
    st[1].pSpecializationInfo = nullptr;
    ba.blendEnable = VK_TRUE;
    ba.colorBlendOp = VK_BLEND_OP_ADD;

    // what is laid over the view
    VkPushConstantRange ppc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush)};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.post_set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &ppc;
    if (vkCreatePipelineLayout(g.dev, &pl, nullptr, &g.post_layout)) return false;
    st[0].module = shader(kPostVert, sizeof kPostVert);
    st[1].module = shader(kPostFrag, sizeof kPostFrag);
    VkPipelineVertexInputStateCreateInfo no_in = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    pi.pVertexInputState = &no_in;
    pi.layout = g.post_layout;
    pi.renderPass = g.post_pass;
    for (int m = 0; m < 3; m++) {
        ba.srcColorBlendFactor = m == 1 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;     // the shader has multiplied by As
        ba.dstColorBlendFactor = m == 0 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : m == 1 ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ZERO;
        ba.srcAlphaBlendFactor = m == 2 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = m == 2 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.post_pipeline[m])) return false;
    }
    // Shadows: High: a shadow over the picture by its depth (native_shadow.frag): the depth's set, and the patch's texture or the map
    {
        const VkPipelineColorBlendAttachmentState ba_was = ba;
        // the third: the frame's lights, for the tunnel lamps' blocks (mode 3)
        VkDescriptorSetLayout shadow_sets[3] = {g.post_set_layout, g.set_layout, g.light_layout};
        VkPushConstantRange spc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPush)};
        VkPipelineLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sl.setLayoutCount = 3;
        sl.pSetLayouts = shadow_sets;
        sl.pushConstantRangeCount = 1;
        sl.pPushConstantRanges = &spc;
        if (vkCreatePipelineLayout(g.dev, &sl, nullptr, &g.shadow_layout)) return false;
        st[1].module = shader(kShadowFrag, sizeof kShadowFrag);
        pi.layout = g.shadow_layout;
        ba.blendEnable = VK_TRUE;
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;           // the shader has multiplied by its alpha
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.shadow_pipeline)) return false;
        // Ambient Occlusion: laid over the picture with the shadows' own blend and sets; and made, unblended, into its image
        st[1].module = shader(kAoApplyFrag, sizeof kAoApplyFrag);
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.ao_apply_pipeline)) return false;
        st[1].module = shader(kAoFrag, sizeof kAoFrag);
        ba.blendEnable = VK_FALSE;
        pi.renderPass = g.ao_pass;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.ao_pipeline)) return false;
        ba.blendEnable = VK_TRUE;
        pi.renderPass = g.post_pass;
        if (g.ms_read_pass) {               // the same over the picture's samples, once a sample (the shader reads gl_SampleID)
            st[1].module = shader(kShadowFragMs, sizeof kShadowFragMs);
            pi.renderPass = g.ms_read_pass;
            pi.pDepthStencilState = &ds;    // the pass has the depth, read by the shader: neither tested nor written (ds since the fold)
            ms.rasterizationSamples = g.samples;
            ms.sampleShadingEnable = VK_TRUE;
            ms.minSampleShading = 1.0f;
            if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.shadow_ms_pipeline)) return false;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            ms.sampleShadingEnable = VK_FALSE;
            ms.minSampleShading = 0;
            pi.pDepthStencilState = nullptr;
            pi.renderPass = g.post_pass;
        }
        ba = ba_was;
        st[1].module = shader(kPostFrag, sizeof kPostFrag);
        pi.layout = g.post_layout;
    }

    // the picture back into the samples
    if (g.samples != VK_SAMPLE_COUNT_1_BIT) {
        if (vkAllocateDescriptorSets(g.dev, &la, &g.restore_set)) return false;
        bind_view(g.restore_set, scene_view, g.sampler[0]);
        VkPipelineLayoutCreateInfo rl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        rl.setLayoutCount = 1;
        rl.pSetLayouts = &g.set_layout;
        if (vkCreatePipelineLayout(g.dev, &rl, nullptr, &g.restore_layout)) return false;
        st[1].module = shader(kRestoreFrag, sizeof kRestoreFrag);
        ms.rasterizationSamples = g.samples;
        ds.depthTestEnable = ds.depthWriteEnable = VK_FALSE;
        ba.blendEnable = VK_FALSE;
        pi.pDepthStencilState = &ds;
        pi.layout = g.restore_layout;
        pi.renderPass = g.pass;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.restore_pipeline)) return false;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    }

    // the settings' brightness, contrast and gamma, last of all on the picture
    {
        la.pSetLayouts = &g.set_layout;
        if (vkAllocateDescriptorSets(g.dev, &la, &g.grade_set)) return false;
        bind_view(g.grade_set, scene_view, g.sampler[0]);
        VkPushConstantRange gpc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4};
        VkPipelineLayoutCreateInfo gl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        gl.setLayoutCount = 1;
        gl.pSetLayouts = &g.set_layout;
        gl.pushConstantRangeCount = 1;
        gl.pPushConstantRanges = &gpc;
        if (vkCreatePipelineLayout(g.dev, &gl, nullptr, &g.grade_layout)) return false;
        st[0].module = shader(kPostVert, sizeof kPostVert);
        st[1].module = shader(kGradeFrag, sizeof kGradeFrag);
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ba.blendEnable = VK_FALSE;
        ba.colorWriteMask = 0xf;
        pi.pDepthStencilState = nullptr;
        pi.layout = g.grade_layout;
        pi.renderPass = g.post_pass;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.grade_pipeline)) return false;
    }

    // The port's dithering (native_extra.frag): one pipeline over the finished picture, reading its copy. Made whatever the
    // settings say: turning it on is then no remaking of the renderer.
    {
        VkDescriptorSetLayoutBinding eb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo esl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        esl.bindingCount = 1;
        esl.pBindings = &eb;
        if (vkCreateDescriptorSetLayout(g.dev, &esl, nullptr, &g.extra_set_layout)) return false;
        la.pSetLayouts = &g.extra_set_layout;
        if (vkAllocateDescriptorSets(g.dev, &la, &g.extra_set)) return false;
        la.pSetLayouts = &g.set_layout;
        // the picture between its texels, clamped
        VkDescriptorImageInfo ei = {g.sampler[4 | 8], scene_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet ew = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ew.dstSet = g.extra_set;
        ew.dstBinding = 0;
        ew.descriptorCount = 1;
        ew.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ew.pImageInfo = &ei;
        vkUpdateDescriptorSets(g.dev, 1, &ew, 0, nullptr);
        VkPushConstantRange epc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ExtraPush)};
        VkPipelineLayoutCreateInfo el = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        el.setLayoutCount = 1;
        el.pSetLayouts = &g.extra_set_layout;
        el.pushConstantRangeCount = 1;
        el.pPushConstantRanges = &epc;
        if (vkCreatePipelineLayout(g.dev, &el, nullptr, &g.extra_layout)) return false;
        st[0].module = shader(kPostVert, sizeof kPostVert);
        st[1].module = shader(kExtraFrag, sizeof kExtraFrag);
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ba.blendEnable = VK_FALSE;
        ba.colorWriteMask = 0xf;
        pi.pDepthStencilState = nullptr;
        pi.layout = g.extra_layout;
        pi.renderPass = g.post_pass;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.extra_pipeline)) return false;
    }

    // SMAA: two images of the picture's size for the edges and the weights, the two tables, and a pipeline a pass
    g.smaa = false;
    static const bool exact_smaa = getenv("HP2_NATIVEEXACT") != nullptr;
    if (g_cfg.aa_mode == 2 && !exact_smaa) {
        VkImageView ev, wv, av, sv2;
        const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const VkImageUsageFlags table = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        void *stage_map = nullptr;
        if (!make_image(g.smaa_edges, ev, VK_FORMAT_R8G8B8A8_UNORM, target, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !make_image(g.smaa_weights, wv, VK_FORMAT_R8G8B8A8_UNORM, target, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !make_image(g.smaa_area, av, VK_FORMAT_R8G8_UNORM, table, VK_IMAGE_ASPECT_COLOR_BIT, AREATEX_WIDTH, AREATEX_HEIGHT) ||
            !make_image(g.smaa_search, sv2, VK_FORMAT_R8_UNORM, table, VK_IMAGE_ASPECT_COLOR_BIT, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT) ||
            !make_buffer(g.smaa_stage, &stage_map, AREATEX_SIZE + SEARCHTEX_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;
        memcpy(stage_map, areaTexBytes, AREATEX_SIZE);
        memcpy((uint8_t *)stage_map + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
        g.smaa_tables = false;
        VkAttachmentDescription sa2 = at[0];
        sa2.format = VK_FORMAT_R8G8B8A8_UNORM;
        sa2.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sa2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sa2.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frp.pAttachments = &sa2;
        if (vkCreateRenderPass(g.dev, &frp, nullptr, &g.smaa_pass)) return false;
        frp.pAttachments = &fa;
        VkFramebufferCreateInfo sf = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        sf.renderPass = g.smaa_pass;
        sf.attachmentCount = 1;
        sf.width = W;
        sf.height = H;
        sf.layers = 1;
        sf.pAttachments = &ev;
        if (vkCreateFramebuffer(g.dev, &sf, nullptr, &g.smaa_fb[0])) return false;
        sf.pAttachments = &wv;
        if (vkCreateFramebuffer(g.dev, &sf, nullptr, &g.smaa_fb[1])) return false;
        // bilinear, clamped, the first level only: what SMAA's fetches between texels count on
        VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        VkSampler linear;
        if (vkCreateSampler(g.dev, &sc, nullptr, &linear)) return false;
        la.pSetLayouts = &g.set_layout;
        struct {
            VkDescriptorSet *set;
            VkImageView view;
        } sets[5] = {{&g.smaa_scene_set, scene_view}, {&g.smaa_edges_set, ev}, {&g.smaa_weights_set, wv}, {&g.smaa_area_set, av}, {&g.smaa_search_set, sv2}};
        for (auto &e : sets) {
            if (vkAllocateDescriptorSets(g.dev, &la, e.set)) return false;
            bind_view(*e.set, e.view, linear);
        }
        const VkDescriptorSetLayout three[3] = {g.set_layout, g.set_layout, g.set_layout};
        VkPushConstantRange spc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 5 + sizeof(int) * 3};
        const struct {
            const uint32_t *vert, *frag;
            size_t vert_bytes, frag_bytes;
            uint32_t sets;
            VkRenderPass pass;
        } passes[3] = {{kSmaaEdgeVert, kSmaaEdgeFrag, sizeof kSmaaEdgeVert, sizeof kSmaaEdgeFrag, 1, g.smaa_pass},
                       {kSmaaWeightVert, kSmaaWeightFrag, sizeof kSmaaWeightVert, sizeof kSmaaWeightFrag, 3, g.smaa_pass},
                       {kSmaaBlendVert, kSmaaBlendFrag, sizeof kSmaaBlendVert, sizeof kSmaaBlendFrag, 2, g.post_pass}};
        for (int i = 0; i < 3; i++) {
            VkPipelineLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            sl.setLayoutCount = passes[i].sets;
            sl.pSetLayouts = three;
            sl.pushConstantRangeCount = 1;
            sl.pPushConstantRanges = &spc;
            if (vkCreatePipelineLayout(g.dev, &sl, nullptr, &g.smaa_layout[i])) return false;
            st[0].module = shader(passes[i].vert, passes[i].vert_bytes);
            st[1].module = shader(passes[i].frag, passes[i].frag_bytes);
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            ba.blendEnable = VK_FALSE;
            ba.colorWriteMask = 0xf;
            pi.pDepthStencilState = nullptr;
            pi.layout = g.smaa_layout[i];
            pi.renderPass = passes[i].pass;
            if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.smaa_pipeline[i])) return false;
        }
        g.smaa = true;
    }

    // the picture's alpha to 1, before the window takes it (opaque_picture)
    {
        VkPipelineLayoutCreateInfo ol = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (vkCreatePipelineLayout(g.dev, &ol, nullptr, &g.opaque_layout)) return false;
        st[0].module = shader(kPostVert, sizeof kPostVert);
        st[1].module = shader(kOpaqueFrag, sizeof kOpaqueFrag);
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ba.blendEnable = VK_FALSE;
        ba.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
        pi.pDepthStencilState = nullptr;
        pi.layout = g.opaque_layout;
        pi.renderPass = g.post_pass;
        if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.opaque_pipeline)) return false;
        ba.colorWriteMask = 0xf;
    }

    VkCommandPoolCreateInfo cp = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = family;
    VkCommandPool pool;
    if (vkCreateCommandPool(g.dev, &cp, nullptr, &pool)) return false;
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.dev, &ca, &g.cmd)) return false;
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(g.dev, &fi, nullptr, &g.fence)) return false;
    // depth_send_early's: without them the rects go at the end of the frame, as before
    if (vkAllocateCommandBuffers(g.dev, &ca, &g.cmd_b) || vkCreateFence(g.dev, &fi, nullptr, &g.depth_fence)) {
        g.cmd_b = VK_NULL_HANDLE;
        g.depth_fence = VK_NULL_HANDLE;
    }
    return true;
}

// The settings changed: all of it is taken apart and made again. The textures go too, native_texture decodes them again as they are used.
// The device and the swapchain stay, so the window goes on.
static bool rebuild()
{
    const double idle0 = pipe_clock();
    vkDeviceWaitIdle(g.dev);
    fprintf(g_hle_log, "native: the settings changed: the device waited for %.0f ms\n", pipe_clock() - idle0);
    pipes_retire();                         // before what the pipelines are made of goes
    free_dead_textures();
    for (VkTex *t : g.textures) free_texture(t);
    g.textures.clear();
    g.tex_pools.clear();                    // they are among what is undone here
    for (auto it = g_undo.rbegin(); it != g_undo.rend(); ++it) (*it)();
    g_undo.clear();
    g.color_ms = g.depth_ms = VK_NULL_HANDLE;
    g.rvm_ready = g.prev_ready = g.sphere_ready = g.scene_ready = false;
    for (int i = 0; i < 6; i++) g.env_ready[i] = g.env_drawn_now[i] = false;
    g.waiting = false;
    g.depth_rects.clear();
    g.depth_places.clear();
    g.depth_early = false;
    g_tracking = true;
    const bool made = make_resources();
    g_tracking = false;
    fprintf(stderr, "native: picture %dx%d, %d samples, anisotropy %d\n", W, H, (int)g.samples, g.aniso);
    return made;
}

// this frame's 512 x 448 in the picture
static void active_region(const NativeFrame &frame, float &ox, float &sx, float &sy)
{
    int w = frame.wide ? W : W43;
    ox = (float)((W - w) / 2);
    sx = w / 512.0f;
    sy = H / 448.0f;
}

static void copy_scene(bool levels = true);

// ---- the picture's last passes, cheaper (2026-11-24) ---------------------------------------------------------------------------
// Intel's UHD 630 at 3840x2180 (the user's ride, speed_uhd630_prof.log): the brightness / contrast / gamma and the dithering took
// some 20 ms of a frame, SMAA 20 more and the alpha's pass before the window its share of 6. Each of the three read a copy of the
// picture made for it, and every copy came with the five smaller levels the blur effects need and these never read (texelFetch
// and textureLod at level 0; SMAA's sampler has the first level only). Now:
//   - their copies are the picture's first level alone (copy_scene(false));
//   - with the picture at 16 bits a channel the three settings are worked in the dithering's pass (native_extra.frag does
//     native_grade.frag's sums, and rounds to the 16 bits the picture would have kept them in): one copy and one pass in place of
//     two and two. With a picture of 8 bits, or one of the two off, the passes are as they were;
//   - the last of these passes writes the alpha 1 that opaque_picture wrote after it, over the same part of the picture the
//     window is shown (blit_to_window takes the scissor's own columns), and opaque_picture is then left out. Not with
//     HP2_NATIVESHOT: a shot is read between the two and has the game's alpha.
// HP2_POSTMERGE=0: all as before.
static const bool g_post_merge = !(getenv("HP2_POSTMERGE") && atoi(getenv("HP2_POSTMERGE")) == 0);
static bool g_opaque_done;                  // this picture's alpha is 1 already (draw_frame clears it before the passes)

static bool post_graded()
{
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    return !exact && !(g_cfg.brightness == 0 && g_cfg.contrast == 100 && g_cfg.gamma == 100);
}
static bool post_dithered()
{
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    return !exact && g_cfg.deband;
}
static bool post_folds_alpha()
{
    static const bool shots = getenv("HP2_NATIVESHOT") != nullptr;
    return g_post_merge && !shots;
}

// Brightness, contrast and gamma from the settings on the finished picture, in the frame's own part of it: the bars beside stay black.
// At their defaults nothing runs. After the copy for the next frame's DisplayBlendPrevious, so it does not build up from frame to frame.
static void grade_picture(const NativeFrame &frame)
{
    if (!post_graded()) return;
    if (g_post_merge && g_color_wide && post_dithered()) return;       // in dither_picture's pass
    const bool last = !post_dithered() && post_folds_alpha();
    copy_scene(false);
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.post_pass;
    rb.framebuffer = g.post_fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{(int32_t)ox, 0}, {(uint32_t)(W - 2 * (int)ox), (uint32_t)H}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.grade_pipeline);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.grade_layout, 0, 1, &g.grade_set, 0, nullptr);
    const float push[4] = {g_cfg.brightness / 100.0f, g_cfg.contrast / 100.0f, g_cfg.gamma / 100.0f, last ? 1.0f : 0.0f};
    vkCmdPushConstants(g.cmd, g.grade_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
    if (last) g_opaque_done = true;
}

// SMAA on the finished picture, the 2D over it included (the game's 3D and 2D are one list in one picture): the edges by luma, the weights,
// the blend. Level 2 is the reference's HIGH preset, 4 its ULTRA. The tables go to the GPU with the first frame that needs them.
static void smaa_picture()
{
    if (!g.smaa) return;
    if (!g.smaa_tables) {
        g.smaa_tables = true;
        const struct {
            VkImage img;
            int w, h;
            VkDeviceSize at;
        } tables[2] = {{g.smaa_area, AREATEX_WIDTH, AREATEX_HEIGHT, 0}, {g.smaa_search, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, AREATEX_SIZE}};
        for (const auto &t : tables) {
            image_barrier(t.img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {};
            copy.bufferOffset = t.at;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
            vkCmdCopyBufferToImage(g.cmd, g.smaa_stage, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            image_barrier(t.img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }
    copy_scene(false);
    struct {
        float rt[4], threshold;
        int steps, diag, corner;
    } push = {{1.0f / W, 1.0f / H, (float)W, (float)H}, 0.1f, 16, 8, 25};
    if (g_cfg.aa_level >= 4) push.threshold = 0.05f, push.steps = 32, push.diag = 16;
    const VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    const VkRect2D sc = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    const VkDescriptorSet sets[3][3] = {{g.smaa_scene_set}, {g.smaa_edges_set, g.smaa_area_set, g.smaa_search_set}, {g.smaa_scene_set, g.smaa_weights_set}};
    const uint32_t counts[3] = {1, 3, 2};
    for (int i = 0; i < 3; i++) {
        VkClearValue clear = {};
        VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = i < 2 ? g.smaa_pass : g.post_pass;
        rb.framebuffer = i < 2 ? g.smaa_fb[i] : g.post_fb;
        rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
        rb.clearValueCount = i < 2 ? 1 : 0;
        rb.pClearValues = &clear;
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(g.cmd, 0, 1, &vp);
        vkCmdSetScissor(g.cmd, 0, 1, &sc);
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.smaa_pipeline[i]);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.smaa_layout[i], 0, counts[i], sets[i], 0, nullptr);
        vkCmdPushConstants(g.cmd, g.smaa_layout[i], VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdDraw(g.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(g.cmd);
        all_barrier();
    }
}

// The picture's alpha to 1, last of all and only for the window: the game's alpha (depth for the fog, the blends' masks) has been read by
// whatever reads it (the next frame's copy, a screenshot), and the next frame clears the picture. The window's image is then opaque
// everywhere, so nothing that shows it by its alpha (a display plane of a fullscreen window, a capture, an overlay) can darken it.
static void opaque_picture()
{
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.post_pass;
    rb.framebuffer = g.post_fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.opaque_pipeline);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
}

// HP2_PERFAB: every other frame the old way (variant 1), so that a change is measured on the same scenes; the log line gives both
static const bool g_ab_on = getenv("HP2_PERFAB") != nullptr;
static int g_ab;                            // this frame's variant
static double g_ab_scene[2], g_ab_total[2];
static int g_ab_n[2];

// HP2_DRAWPROF=<n>: once every n frames (600 without a number above 1) the main picture's pass is timed in stretches, and the
// log says where its time went. A stretch is up to 16 draws of one kind (opaque, opaque with the alpha test, blended), or what is
// laid over the picture between two of its passes (an effect: the fog, the shadows, the ambient occlusion ...; the depth's copy for
// the soft sprites). Made full on 2026-11-24 for Intel's UHD 630, where the whole frame is the GPU's (the scene 90 to 220 ms of
// it) and the question is what in the scene: each stretch also counts the pixels its draws shaded (a pipeline statistics query,
// the fragment shader's runs: a pixel the depth test turned away before the shader is not among them), so the sums say how many
// times over the picture is shaded, and by what. That frame's draws go one by one (not merged), so its own time is a little more.
static const int g_dp_period = [] {
    const char *e = getenv("HP2_DRAWPROF");
    if (!e) return 0;
    const int n = atoi(e);
    return n >= 2 ? n : 600;
}();
static const bool g_dp_on = g_dp_period != 0;
enum { kDpMax = 2048 };
enum { DP_OPAQUE, DP_TEST, DP_BLEND, DP_FX, DP_END };
enum { DP_FX_SOFT_DEPTH = 5 };              // DP_FX's sub: the effect's kind (FX_FOG ...), or this
static VkQueryPool g_dp_pool, g_dp_stat_pool;
static bool g_dp_now, g_dp_pending;
static bool g_dp_open;                      // the last stretch's count of pixels is running
static bool g_dp_fresh;                     // the next draw begins a stretch (a pass has just begun)
struct DpSeg { uint32_t draw, draws, vertices, sphere, road, alpha, blended, big, kind, sub; };
static DpSeg g_dp_seg[kDpMax];
static uint32_t g_dp_n, g_dp_frame;

// A stretch begins here and the one before ends. counted: inside the picture's pass, its pixels are counted (a count begins and
// ends within one pass, so every pass's end is a mark too).
static void dp_mark(uint32_t draw, uint32_t kind, uint32_t fx, bool counted)
{
    if (!g_dp_now) return;
    if (g_dp_open) {
        vkCmdEndQuery(g.cmd, g_dp_stat_pool, g_dp_n - 1);
        g_dp_open = false;
    }
    if (g_dp_n >= kDpMax) return;
    vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_dp_pool, g_dp_n);
    g_dp_seg[g_dp_n] = {draw, 0, 0, 0, 0, 0, 0, 0, kind, fx};
    if (counted && g_dp_stat_pool && g_dp_n + 1 < kDpMax) {
        vkCmdBeginQuery(g.cmd, g_dp_stat_pool, g_dp_n, 0);
        g_dp_open = true;
    }
    g_dp_n++;
}

static void dp_read()
{
    if (!g_dp_pending) return;
    g_dp_pending = false;
    if (g_dp_n < 3) return;
    static uint64_t t[kDpMax];
    static uint64_t st[kDpMax][2];          // the pixels shaded, and whether the count is there
    if (vkGetQueryPoolResults(g.dev, g_dp_pool, 0, g_dp_n, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    memset(st, 0, sizeof st);
    bool counted = false;
    if (g_dp_stat_pool) {
        const VkResult r = vkGetQueryPoolResults(g.dev, g_dp_stat_pool, 0, g_dp_n, sizeof st, st, sizeof st[0],
                                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        counted = r == VK_SUCCESS || r == VK_NOT_READY;
        if (!counted) memset(st, 0, sizeof st);
    }
    const double ns = g.props.limits.timestampPeriod, picture = (double)W * (double)H;
    struct Sum {
        double ms, px;
        unsigned long long draws, vertices;
        unsigned n;
    };
    Sum by[3] = {}, fx[8] = {}, cars = {}, road = {}, rest = {}, tenth[10] = {}, all = {};
    const uint32_t last_draw = std::max(g_dp_seg[g_dp_n - 1].draw, 1u);
    std::vector<std::pair<double, uint32_t>> d;
    double total = 0;
    static double px_of[kDpMax];
    for (uint32_t i = 0; i + 1 < g_dp_n; i++) {
        double ms = t[i + 1] > t[i] ? (double)(t[i + 1] - t[i]) * ns / 1e6 : 0;
        total += ms;
        d.push_back({ms, i});
        const DpSeg &s = g_dp_seg[i];
        const double px = st[i][1] ? (double)st[i][0] : 0;
        px_of[i] = px;
        auto add = [&](Sum &to) {
            to.ms += ms, to.px += px, to.draws += s.draws, to.vertices += s.vertices, to.n++;
        };
        add(all);
        if (s.kind == DP_FX) {
            add(fx[std::min(s.sub, 7u)]);
        } else if (s.kind <= DP_BLEND) {
            add(by[s.kind]);
            add(s.sphere * 2 > s.draws ? cars : s.road * 2 > s.draws ? road : rest);
            add(tenth[std::min((uint32_t)((uint64_t)s.draw * 10 / last_draw), 9u)]);
        }
    }
    std::sort(d.begin(), d.end(), [](auto &a, auto &b) { return a.first > b.first; });
    fprintf(g_hle_log, "native: drawprof frame %u: the picture's pass %.2f ms over %u stretches, %llu draws, %llu vertices; picture %dx%d, %d samples%s\n", g_dp_frame,
            total, g_dp_n - 1, all.draws, all.vertices, W, H, (int)g.samples, g_dp_n >= kDpMax ? " (MORE STRETCHES THAN THERE IS ROOM FOR: the end is missing)" : "");
    if (counted)
        fprintf(g_hle_log, "native: drawprof   pixels shaded %.2f M = %.2f times the picture\n", all.px / 1e6, all.px / picture);
    else
        fprintf(g_hle_log, "native: drawprof   (the device does not count the pixels shaded)\n");
    const auto line = [&](const char *name, const Sum &m) {
        if (!m.n) return;
        fprintf(g_hle_log, "native: drawprof   %-22s %8.2f ms (%4.1f%%), %5llu draws, %7llu vertices, %7.2f M pixels = %5.2f pictures, %.2f ms a picture\n", name, m.ms,
                total > 0 ? m.ms * 100 / total : 0, m.draws, m.vertices, m.px / 1e6, m.px / picture, m.px > 0 ? m.ms * picture / m.px : 0);
    };
    line("opaque", by[DP_OPAQUE]);
    line("opaque, alpha test", by[DP_TEST]);
    line("blended", by[DP_BLEND]);
    static const char *const fx_name[8] = {"fx: fog", "fx: grey", "fx: depth of field", "fx: shadows", "fx: ambient occlusion", "soft sprites' depth", "fx: ambient occlusion's world", "fx 7"};
    for (int k = 0; k < 8; k++)
        if (fx[k].n)
            fprintf(g_hle_log, "native: drawprof   %-22s %8.2f ms (%4.1f%%), %u times (with the draws left of the stretch before it)\n", fx_name[k], fx[k].ms,
                    total > 0 ? fx[k].ms * 100 / total : 0, fx[k].n);
    line("of the draws: cars", cars);
    line("of the draws: road", road);
    line("of the draws: the rest", rest);
    fprintf(g_hle_log, "native: drawprof   by tenths of the draws' order, ms:");
    for (int k = 0; k < 10; k++) fprintf(g_hle_log, " %.1f", tenth[k].ms);
    fprintf(g_hle_log, "; pictures shaded:");
    for (int k = 0; k < 10; k++) fprintf(g_hle_log, " %.2f", tenth[k].px / picture);
    fprintf(g_hle_log, "\n");
    static const char *const kind_name[5] = {"opaque", "alpha test", "blended", "fx", "end"};
    for (size_t k = 0; k < d.size() && k < 12; k++) {
        const DpSeg &s = g_dp_seg[d[k].second];
        fprintf(g_hle_log, "native: drawprof %.3f ms from draw %u (%s%s%s): %u draws, %u vertices, %.2f pictures shaded, sphere %u, road %u, alpha-test %u, blended %u, over 3000 vertices %u\n",
                d[k].first, s.draw, kind_name[std::min(s.kind, 4u)], s.kind == DP_FX ? " " : "", s.kind == DP_FX ? fx_name[std::min(s.sub, 7u)] : "", s.draws,
                s.vertices, px_of[d[k].second] / picture, s.sphere, s.road, s.alpha, s.blended, s.big);
    }
    // the spread: how many stretches it takes for half of the time
    double half = 0;
    size_t k = 0;
    while (k < d.size() && half < total / 2) half += d[k++].first;
    fprintf(g_hle_log, "native: drawprof half the time in %zu of %zu stretches\n", k, d.size());
}

// every strip of a target from [first, last) of the draws, up to three passes each
static const bool g_merge_stat = getenv("HP2_MERGESTAT") != nullptr;
static struct {
    unsigned long long frames, draws, singles, opaque, passes, pipelines, sets, calls, runs;
} g_ms;

static void draw_target(const NativeFrame &frame, int target, int vw, int vh, size_t first = 0, size_t last = SIZE_MAX)
{
    const bool in_samples = target == 1 && g.samples != VK_SAMPLE_COUNT_1_BIT;
    // HP2_PERFAB's odd frames: the old shaders, with the far plane's discard in all
    const int v = g.clip_far && g_ab != 1 ? 0 : 1;
    auto &pipes_at = in_samples ? g.pipeline_ms[v] : g.pipeline[v];
    auto &alpha_at = in_samples ? g.alpha_only_ms[v] : g.alpha_only[v];
    VkViewport vp = {0, 0, (float)vw, (float)vh, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)vw, (uint32_t)vh}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    const VkBuffer both[2] = {g.vb, g.vb};
    const VkDeviceSize zeros[2] = {0, 0};
    vkCmdBindVertexBuffers(g.cmd, 0, 2, both, zeros);
    VkDeviceSize mid_off = 0;               // where the second binding starts: a draw's second copy of its vertices less its first
    vkCmdBindIndexBuffer(g.cmd, g.vb, frame.geo_indexed ? 0 : g.draw_idx_off, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 1, 1, &g.light_set, 0, nullptr);
    VkTex *road = frame.road_specular ? (VkTex *)frame.road_specular->vk : nullptr;
    VkTex *env_tex = frame.env_override ? (VkTex *)frame.env_override->vk : nullptr;
    const VkDescriptorSet env_set = env_tex ? env_tex->set : g.sphere_set;
    auto tex_mode = [](const NativeTexture &t) {
        return (t.repeat_s ? 1 : 0) | (t.repeat_t ? 2 : 0) | (t.mag_linear ? 4 : 0) | (t.min_linear ? 8 : 0) | (t.mip_linear ? 16 : 0) | (t.levels - 1) << 8;
    };
    // Particles: High: a sprite of the port's own that blends and writes no depth is drawn as a ball of smoke (native.frag, SOFT),
    // once this picture has its copy of the depth (g_soft_now). Its pipeline, null: drawn as it is
    auto soft_of = [&](const NativeDraw &d) -> VkPipeline {
        if (!g_soft_now || target != 1 || !(d.flags & kSoftKinds) || d.depth_write || d.blend == NB_OPAQUE || d.blend >= NB_COUNT) return VK_NULL_HANDLE;
        return g.soft_pipeline[in_samples ? 1 : 0][d.blend][d.depth_test ? 1 : 0];
    };
    // what is bound already: the passes of a strip go between a few pipelines and sets, and binding again what is there costs the GPU's front end
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    // merged_cmds not 0: d is the first of a run of draws, and the run's commands are merged_cmds of g.cmds_buf from merged_from
    auto pass = [&](const NativeDraw &d, int which, VkDescriptorSet set, VkPipeline pipeline, float factor, bool textured, uint32_t merged_cmds = 0,
                    uint32_t merged_from = 0) {
        Push p;
        p.merged = merged_cmds ? 1 : 0;
        picture_clip(p.clip, d);
        p.scroll[0] = which == 2 ? 0 : d.scroll_s;
        p.scroll[1] = which == 2 ? 0 : d.scroll_t;
        p.alpha_test = d.alpha_test ? 1.0f : 0.0f;
        p.blend_alpha = factor;
        p.textured = textured ? 1.0f : 0.0f;
        p.light = d.light;
        p.flags = d.flags;
        // a sprite drawn as a ball (soft_of): the vertex shader tests it at the ball's front, and the depth's copy is its third set
        const bool soft = g_soft_now && target == 1 && (d.flags & kSoftKinds) && pipeline == soft_of(d);
        if (soft) p.flags |= kSoftFlag;
        p.pass = which;
        p.tfx = which == 0 ? d.tfx : 0;
        p.lod_l = which == 0 ? (float)d.lod_l : 0.0f;
        p.bits16 = target >= 8 && target <= 13;
        p.snap[0] = d.snap_x;
        p.snap[1] = d.snap_y;
        p.between = g_pic_f;
        // the pictures of this renderer: clamped and linear, as TEX1's 0xe0 has it at level 0
        p.tex_mode = which == 2 && env_tex ? frame.env_mode | 64
                   : which == 2 || d.rt >= 0 || !d.tex ? 4 | 8 : which == 1 ? frame.road_mode : d.tex_mode;
        // the mirror's picture on a car's own mirror glass: native.frag's sample_glass (native.cpp, mirror_glass_now)
        if (which == 0 && d.rt == NATIVE_RT_MIRROR && (d.tex_mode & 128)) p.tex_mode |= 128;
        // what VU1 transforms, the world, whatever filter the game gave the texture; the setting's filter leaves the 2D and the envmap views
        // to the GS's sampling. HP2_AFOLD: only the textures the game filters linear, as it was; HP2_AFFLIP: that and this by turns of
        // 120 frames, for two pictures of one place
        static const bool af_old = getenv("HP2_AFOLD") != nullptr, af_flip = getenv("HP2_AFFLIP") != nullptr;
        const bool af_every = af_flip ? (g.frame / 120 & 1) != 0 : !af_old;
        if (which == 0 && d.tex && d.rt < 0 && (d.tex_upscaled || (g.aniso && (af_every || d.tex_min_linear) && d.snap_x != 0 && target < 8))) p.tex_mode |= 32;
        if (g_merge_stat && target == 1) {
            g_ms.passes++;
            g_ms.pipelines += pipeline != bound_pipeline;
            g_ms.sets += set != bound_set;
            g_ms.calls += merged_cmds ? 1 : d.ranges ? d.ranges : 1;
        }
        if (pipeline != bound_pipeline) vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (set != bound_set) vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
        bound_pipeline = pipeline;
        bound_set = set;
        if (soft) vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.soft_layout, 2, 1, &g.soft_set, 0, nullptr);
        vkCmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        const VkDeviceSize mid_want = g_between && d.mid_verts && !merged_cmds && !d.ranges && !d.indexed
                                          ? (VkDeviceSize)(d.mid_first - d.first) * sizeof(NativeVertex) : 0;
        if (mid_want != mid_off) {
            vkCmdBindVertexBuffers(g.cmd, 1, 1, &g.vb, &mid_want);
            mid_off = mid_want;
        }
        if (merged_cmds && frame.geo_indexed && d.ranges) {
            vkCmdDrawIndexedIndirect(g.cmd, g.cmds_buf, (VkDeviceSize)merged_from * sizeof(MergeCmd), merged_cmds, sizeof(MergeCmd));
        } else if (merged_cmds) {
            vkCmdDrawIndirect(g.cmd, g.cmds_buf, (VkDeviceSize)merged_from * sizeof(MergeCmd), merged_cmds, sizeof(MergeCmd));
        } else if (d.ranges) {
            // the strips' vertices where they lie in the buffer: the same triangles in the same order as their indices gave
            const NativeRange *r = frame.ranges.data() + d.first;
            if (frame.geo_indexed)
                for (uint32_t i = 0; i < d.ranges; i++) vkCmdDrawIndexed(g.cmd, r[i].count, 1, r[i].first, 0, 0);
            else
                for (uint32_t i = 0; i < d.ranges; i++) vkCmdDraw(g.cmd, r[i].count, 1, r[i].first, 0);
        } else if (d.indexed) {
            vkCmdDrawIndexed(g.cmd, d.count, 1, d.first, 0, 0);
        } else {
            vkCmdDraw(g.cmd, d.count, 1, g.draw_dyn_first + d.first, 0);
        }
    };
    // The draws merged. A run: draws one after another, each of one pass, that have the same pipeline, texture and constants, all but
    // the matrix and the light. Its draws' matrices and lights go into g.draws_buf, their ranges of vertices into g.cmds_buf as
    // commands with the draw's number in that buffer as the instance, and the run is one vkCmdDrawIndirect: the commands are drawn in
    // their order, so the picture is made of the same triangles in the same order as draw by draw. A draw of more passes (the
    // road's shine, a car's reflection, a blend that leaves its alpha) ends the run and goes out as before.
    struct RunKey {
        VkPipeline pipeline;
        VkDescriptorSet set;
        float factor, scroll[2], snap[2], view[4];
        int flags, tfx, lod_l, tex_mode, alpha_test, textured, upscaled, min_linear;
        int by_indices;                     // the run's commands are indexed ones: strips' draws do not share a run with the others
    };
    RunKey run_key;
    const NativeDraw *run_first = nullptr;
    uint32_t run_from = 0;
    const bool merging = g.can_merge && !(g_dp_now && target == 1);
    auto flush_run = [&] {
        if (!run_first) return;
        if (g.merge_cmds > run_from)
            pass(*run_first, 0, run_key.set, run_key.pipeline, run_key.factor, run_key.textured != 0, g.merge_cmds - run_from, run_from);
        run_first = nullptr;
    };
    if (last > frame.draws.size()) last = frame.draws.size();
    float last_view[4] = {-1, -1, -1, -1};
    for (size_t i = first; i < last; i++) {
        const NativeDraw &d = frame.draws[i];
        // the players' views and the 2D layer share a picture and go in the game's order
        // Target 6 in the frontend is the car a second time, half a line off, which the GS averages with the first (eAttachBlendTargets):
        // its antialiasing. Here the first is the picture, and the second would only be drawn over it.
        bool mine = target == 1 ? d.target <= 4 || (d.target == 6 && !frame.env_override) : d.target == target;
        if (!d.count || !mine) continue;
        if (g_dp_now && target == 1) {
            static uint32_t since;
            const uint32_t kind = d.blend != NB_OPAQUE ? DP_BLEND : d.alpha_test ? DP_TEST : DP_OPAQUE;
            if (g_dp_n == 0 || g_dp_fresh || ++since >= 16 || g_dp_seg[g_dp_n - 1].kind != kind) {
                since = 0;
                g_dp_fresh = false;
                dp_mark((uint32_t)i, kind, 0, true);
            }
            DpSeg &s = g_dp_seg[g_dp_n - 1];
            s.draws++;
            s.vertices += d.count;
            s.sphere += (d.flags & 0x100) != 0;
            s.road += (d.flags & 0x40) != 0;
            s.alpha += d.alpha_test;
            s.blended += d.blend != NB_OPAQUE;
            s.big += d.count > 3000;
        }
        if (target == 1 && memcmp(d.view, last_view, sizeof last_view)) {
            flush_run();                    // the run so far is the last view's
            memcpy(last_view, d.view, sizeof last_view);
            float ox, sx, sy;
            active_region(frame, ox, sx, sy);
            VkViewport dv = {ox + d.view[0] * sx, d.view[1] * sy, d.view[2] * sx, d.view[3] * sy, 0, 1};
            VkRect2D ds = {{(int32_t)dv.x, (int32_t)dv.y}, {(uint32_t)dv.width, (uint32_t)dv.height}};
            vkCmdSetViewport(g.cmd, 0, 1, &dv);
            vkCmdSetScissor(g.cmd, 0, 1, &ds);
        }
        VkTex *t = d.tex ? (VkTex *)d.tex->vk : nullptr;
        if (d.tex && !t) continue;                          // not in the GPU yet
        if (d.rt == NATIVE_RT_MIRROR && !g.rvm_ready) continue;
        if (d.rt == NATIVE_RT_PREVIOUS && !g.prev_ready) continue;
        // textured = 0 does not read the set, but one must be bound
        VkDescriptorSet set = d.rt == NATIVE_RT_MIRROR ? g.rvm_set : d.rt == NATIVE_RT_PREVIOUS ? g.prev_set : t ? t->set : g.sphere_set;
        float factor = d.blend == NB_OPAQUE ? -2.0f : d.blend == NB_KEEP ? -4.0f : d.blend == NB_DEST || d.blend == NB_LERP_DEST ? -3.0f : d.blend_fix;
        bool textured = d.tex != nullptr || d.rt >= 0;
        static const int off = getenv("HP2_NATIVEOFF") ? atoi(getenv("HP2_NATIVEOFF")) : 0;     // 1 road, 2 sphere map
        bool has_road = d.light >= 0 && (d.flags & 0x40) && road && !(off & 1);
        bool has_sphere = d.light >= 0 && (d.flags & 0x100) && (g.sphere_ready || env_tex) && !(off & 2);
        bool gloss = (d.flags & 4) != 0;
        auto &pipes = pipes_at[d.alpha_test ? 1 : 0];
        auto &alpha_pipes = alpha_at[d.alpha_test ? 1 : 0];
        VkPipeline main_pipeline = pipes[d.blend][d.depth_write][d.depth_test];
        if (VkPipeline soft = soft_of(d)) main_pipeline = soft;
        VkPipeline dest_pipeline = pipes[NB_DEST][0][d.depth_test];
        if (g_merge_stat && target == 1) {
            // a draw of one pass, and what a draw next to it must share to go out with it in one call
            const bool alpha_extra = d.blend != NB_OPAQUE && d.blend != NB_KEEP && d.blend != NB_LERP_DEST && (has_road || (has_sphere && gloss));
            const bool single = d.blend != NB_DEST && !alpha_extra && !has_road && !has_sphere;
            struct Key {
                VkPipeline pipeline;
                VkDescriptorSet set;
                float factor, scroll[2], snap[2], view[4];
                int flags, tfx, lod_l, tex_mode, alpha_test, textured, strips;
            } key = {main_pipeline, set, factor, {d.scroll_s, d.scroll_t}, {d.snap_x, d.snap_y}, {d.view[0], d.view[1], d.view[2], d.view[3]},
                     (int)d.flags, d.tfx, d.lod_l, d.tex_mode, d.alpha_test, textured, d.ranges != 0};
            static Key last;
            static bool last_single;
            g_ms.draws++;
            g_ms.singles += single;
            g_ms.opaque += d.blend == NB_OPAQUE;
            if (!(single && last_single && !memcmp(&key, &last, sizeof key))) g_ms.runs++;
            last = key;
            last_single = single;
        }
        {
            const bool alpha_extra = d.blend != NB_OPAQUE && d.blend != NB_KEEP && d.blend != NB_LERP_DEST && (target == 7 || has_road || (has_sphere && gloss));
            const bool one_pass = d.blend != NB_DEST && !alpha_extra && !has_road && !has_sphere;
            const uint32_t cmds = d.ranges ? d.ranges : 1;
            // (a draw with a second copy of its vertices has its own start of the second binding in a picture between: not merged)
            if (merging && one_pass && (d.ranges || !d.indexed) && !(g_between && d.mid_verts) && g.merge_draws < g.draws_cap &&
                g.merge_cmds + cmds <= g.cmds_cap) {
                RunKey key;
                memset(&key, 0, sizeof key);
                key.pipeline = main_pipeline;
                key.set = set;
                key.factor = factor;
                key.scroll[0] = d.scroll_s, key.scroll[1] = d.scroll_t;
                key.snap[0] = d.snap_x, key.snap[1] = d.snap_y;
                memcpy(key.view, d.view, sizeof key.view);
                key.flags = d.flags, key.tfx = d.tfx, key.lod_l = d.lod_l, key.tex_mode = d.tex_mode, key.alpha_test = d.alpha_test;
                key.textured = textured, key.upscaled = d.tex_upscaled, key.min_linear = d.tex_min_linear;
                key.by_indices = frame.geo_indexed && d.ranges;
                if (!run_first || memcmp(&key, &run_key, sizeof key)) {
                    flush_run();
                    run_key = key;
                    run_first = &d;
                    run_from = g.merge_cmds;
                }
                DrawData &own = ((DrawData *)g.draws_map)[g.merge_draws];
                picture_clip(own.clip, d);
                own.light = d.light;
                MergeCmd *cmd = (MergeCmd *)g.cmds_map + g.merge_cmds;
                if (d.ranges && frame.geo_indexed) {
                    const NativeRange *r = frame.ranges.data() + d.first;
                    for (uint32_t k = 0; k < d.ranges; k++) cmd[k] = {r[k].count, 1, r[k].first, 0, g.merge_draws};
                } else if (d.ranges) {
                    const NativeRange *r = frame.ranges.data() + d.first;
                    for (uint32_t k = 0; k < d.ranges; k++) cmd[k] = {r[k].count, 1, r[k].first, g.merge_draws, 0};
                } else {
                    cmd[0] = {d.count, 1, g.draw_dyn_first + d.first, g.merge_draws, 0};
                }
                g.merge_cmds += cmds;
                g.merge_draws++;
                continue;
            }
            flush_run();
        }
        pass(d, 0, set, main_pipeline, factor, textured);
        if (d.blend == NB_DEST) pass(d, 0, set, main_pipeline, factor, textured);
        // A blend's alpha is its factor here, on the GS it is As. As matters if a later pass or the mirror's quad reads it.
        bool alpha_read = target == 7 || has_road || (has_sphere && gloss);
        if (d.blend != NB_OPAQUE && d.blend != NB_KEEP && d.blend != NB_LERP_DEST && alpha_read)
            pass(d, 0, set, alpha_pipes[d.depth_test], -2.0f, textured);
        // VU1's second kick (0x320): the falloff with Cs Ad + Cd, FIX unused
        if (has_road) {
            pass(d, 1, road->set, dest_pipeline, -3.0f, true);
            pass(d, 1, road->set, dest_pipeline, -3.0f, true);
            if (target == 7 || (has_sphere && gloss)) pass(d, 1, road->set, alpha_pipes[d.depth_test], -2.0f, true);
        }
        // the third (0x318, 0x31c with a gloss map): the sphere map, Cs As + Cd or Cs Ad + Cd
        // In the frontend (eDisplayFrame's header for 0x318 and 0x31c alike, 0x10af58 and 0x10b040) both are ALPHA 0x48, Cs As + Cd:
        // a body's reflection there goes by the material's strength and the map's alpha, not by the gloss the first pass left
        if (has_sphere) {
            if (gloss && !env_tex && frame.chrome) {
                // the Chrome Cars cheat: ALPHA (0 - 0) Ad + Cs, the map as it is over the paint (NativeFrame::chrome)
                pass(d, 2, env_set, pipes[NB_OPAQUE][0][d.depth_test], -2.0f, true);
            } else if (gloss && !env_tex) {
                pass(d, 2, env_set, dest_pipeline, -3.0f, true);
                pass(d, 2, env_set, dest_pipeline, -3.0f, true);
            } else {
                pass(d, 2, env_set, pipes[NB_ADD][0][d.depth_test], -1.0f, true);
            }
            if (target == 7) pass(d, 2, env_set, alpha_pipes[d.depth_test], -2.0f, true);
        }
    }
    flush_run();
}

static VkBuffer g_env_log_rb;
static void *g_env_log_map;
static int g_env_log_have;

// ---- Shadows: High (native.cpp, "Shadows: High, Very High and Ultra") ----
static void depth_image_barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// the cars as the sun sees them, each into its cell of the map: once for every picture, before anything reads the map
static void draw_shadow_map(const NativeFrame &frame)
{
    if (!frame.shadow_level || (frame.casters.empty() && frame.shadows.empty())) return;
    VkClearValue clear = {};
    clear.depthStencil.depth = 0;
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.shadow_pass;
    rb.framebuffer = g.shadow_fb;
    rb.renderArea.extent = {(uint32_t)(g.shadow_cell * kShadowAcross), (uint32_t)g.shadow_h};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_cast);
    const VkBuffer both[2] = {g.vb, g.vb};
    const VkDeviceSize zeros[2] = {0, 0};
    vkCmdBindVertexBuffers(g.cmd, 0, 2, both, zeros);
    vkCmdBindIndexBuffer(g.cmd, g.vb, frame.geo_indexed ? 0 : g.draw_idx_off, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 1, 1, &g.light_set, 0, nullptr);
    bool tested_bound = false;              // the pipeline bound: shadow_cast_test (else shadow_cast)
    for (const NativeShadowCaster &k : frame.casters) {
        if (k.draw >= frame.draws.size()) continue;
        VkViewport vp;
        if (k.cell >= kShadowAcross * kShadowDown) {        // a tunnel lamp's cell, in the row below the glass
            const int lamp = k.cell - kShadowAcross * kShadowDown, half = g.shadow_cell / 2;
            if (!g.lamp_row || lamp >= kLampCellsAll) continue;
            vp = {(float)((lamp % kLampCellsAcross) * half), (float)(kShadowRows * g.shadow_cell + (lamp / kLampCellsAcross) * half), (float)half, (float)half, 0, 1};
        } else {
            vp = {(float)((k.cell % kShadowAcross) * g.shadow_cell), (float)((k.cell / kShadowAcross + (k.glass ? kShadowDown : 0)) * g.shadow_cell),
                  (float)g.shadow_cell, (float)g.shadow_cell, 0, 1};
        }
        const NativeDraw &d = frame.draws[k.draw];
        if (!d.count) continue;
        // the alpha test: with the draw's texture, by the scene's shader
        VkTex *t = d.alpha_test && d.tex && d.rt < 0 ? (VkTex *)d.tex->vk : nullptr;
        const bool tested = t && t->set;   // every level (the user, 2026-11-26)
        if (tested != tested_bound) {
            vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tested ? g.shadow_cast_test : g.shadow_cast);
            tested_bound = tested;
        }
        if (tested) {
            t->used = g.frame;
            vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &t->set, 0, nullptr);
        }
        VkRect2D sc = {{(int32_t)vp.x, (int32_t)vp.y}, {(uint32_t)vp.width, (uint32_t)vp.height}};
        vkCmdSetViewport(g.cmd, 0, 1, &vp);
        vkCmdSetScissor(g.cmd, 0, 1, &sc);
        Push p = {};
        memcpy(p.clip, k.clip, sizeof p.clip);
        p.blend_alpha = -2.0f;
        p.light = -1;
        p.between = 1.0f;
        if (tested) {
            p.alpha_test = 1.0f;
            p.textured = 1.0f;
            p.tfx = d.tfx;
            p.lod_l = (float)d.lod_l;
            p.tex_mode = d.tex_mode | 32;           // the texture by its own sampler: its alpha is what is asked
            p.scroll[0] = d.scroll_s, p.scroll[1] = d.scroll_t;
        }
        vkCmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        if (d.ranges) {
            const NativeRange *r = frame.ranges.data() + d.first;
            if (frame.geo_indexed)
                for (uint32_t i = 0; i < d.ranges; i++) vkCmdDrawIndexed(g.cmd, r[i].count, 1, r[i].first, 0, 0);
            else
                for (uint32_t i = 0; i < d.ranges; i++) vkCmdDraw(g.cmd, r[i].count, 1, r[i].first, 0);
        } else if (d.indexed) {
            vkCmdDrawIndexed(g.cmd, d.count, 1, d.first, 0, 0);
        } else {
            vkCmdDraw(g.cmd, d.count, 1, g.draw_dyn_first + d.first, 0);
        }
    }
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
    g.shadow_ready = true;
}

// A view's shadows, inside a pass over its colour with its depth being read. The outline's shadow fades as the car gets small on
// the real picture: all there above 36 pixels of the car's length, none below 12 (the game's own went by the PS2's pixels).
static void draw_shadows(const NativeFrame &frame, int target, bool mirror, bool samples = false)
{
    float ox = 0, sx = 1, sy = 1;
    if (!mirror) active_region(frame, ox, sx, sy);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, samples ? g.shadow_ms_pipeline : g.shadow_pipeline);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_layout, 0, 1, samples ? &g.ms_post_set : mirror ? &g.rvm_post_set : &g.post_set, 0,
                            nullptr);
    if (g.light_set) vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_layout, 2, 1, &g.light_set, 0, nullptr);
    for (const NativeShadow &s : frame.shadows) {
        if (s.target != target) continue;
        VkViewport vp = {0, 0, (float)g.rvm_w, (float)g.rvm_h, 0, 1};
        if (!mirror) vp = {ox + s.view[0] * sx, s.view[1] * sy, s.view[2] * sx, s.view[3] * sy, 0, 1};
        if (!(vp.width >= 1 && vp.height >= 1)) continue;
        // only where it can fall (NativeShadow::box, the view's -1..1): not over the whole view for every car
        const float bx0 = std::max(vp.x + (s.box[0] * 0.5f + 0.5f) * vp.width - 2.0f, std::max(vp.x, 0.0f));
        const float by0 = std::max(vp.y + (s.box[1] * 0.5f + 0.5f) * vp.height - 2.0f, std::max(vp.y, 0.0f));
        const float bx1 = std::min(vp.x + (s.box[2] * 0.5f + 0.5f) * vp.width + 2.0f, vp.x + vp.width);
        const float by1 = std::min(vp.y + (s.box[3] * 0.5f + 0.5f) * vp.height + 2.0f, vp.y + vp.height);
        if (!(bx1 - bx0 >= 1 && by1 - by0 >= 1)) continue;     // off the picture
        const int32_t x0 = (int32_t)bx0, y0 = (int32_t)by0;
        const int32_t x1 = std::min((int32_t)(bx1 + 0.999f), (int32_t)(vp.x + vp.width)), y1 = std::min((int32_t)(by1 + 0.999f), (int32_t)(vp.y + vp.height));
        if (x1 <= x0 || y1 <= y0) continue;
        VkRect2D sc = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        ShadowPush p = {};
        if (g_between && s.has_mid && g_pic_f == 0.5f) {
            memcpy(p.to, s.to_mid, sizeof p.to);
        } else if (g_between && s.has_mid) {
            const float t = 2.0f * g_pic_f - 1.0f;
            for (int i = 0; i < 16; i++) p.to[i] = s.to_mid[i] * (1.0f - t) + s.to[i] * t;
        } else {
            memcpy(p.to, s.to, sizeof p.to);
        }
        p.view[0] = vp.x, p.view[1] = vp.y, p.view[2] = vp.width, p.view[3] = vp.height;
        VkDescriptorSet set;
        if (s.kind == 1) {
            if (!g.shadow_ready || s.cell >= kShadowAcross * kShadowDown) continue;
            const float pixels = s.size * vp.width;
            const float fade = std::min(std::max((pixels - 12.0f) / (36.0f - 12.0f), 0.0f), 1.0f);
            if (!(fade > 0)) continue;
            p.colour[3] = s.colour[3] * fade;
            p.mode = 1;
            p.texel = 1.0f / (float)g.shadow_cell;
            // the soft edge: no narrower than a texel of the map (what smooths the texels' stairs), no wider than its widest
            p.low = std::max(s.soft > 0 ? s.soft : 1.5f * p.texel, p.texel);
            p.high = std::min(std::max(s.low, p.texel), p.low);
            p.colour[0] = s.high;
            p.colour[1] = s.metres[0], p.colour[2] = s.metres[1];
            p.rect[0] = (float)(s.cell % kShadowAcross) / kShadowAcross, p.rect[1] = (float)((s.cell / kShadowAcross) * g.shadow_cell) / (float)g.shadow_h;
            p.rect[2] = 1.0f / kShadowAcross, p.rect[3] = (float)g.shadow_cell / (float)g.shadow_h;
            set = g.shadow_set;
        } else if (s.kind == 3) {               // the light a car's tunnel lamps lose to it (native.cpp, "the tunnels' lamps")
            if (!g.shadow_ready || !g.lamp_row || !g.light_set) continue;
            const float pixels = s.size * vp.width;
            const float fade = std::min(std::max((pixels - 12.0f) / (36.0f - 12.0f), 0.0f), 1.0f);
            if (!(fade > 0)) continue;
            p.colour[3] = fade;
            p.mode = 3;
            p.texel = (float)s.block;           // its block's first quad in the lights: whole, and well below 2^24
            set = g.shadow_set;
        } else {
            VkTex *t = s.tex ? (VkTex *)s.tex->vk : nullptr;
            if (!t) continue;                   // not in the GPU yet
            t->used = g.frame;
            memcpy(p.colour, s.colour, sizeof p.colour);
            p.low = s.low, p.high = s.high;
            p.rect[0] = s.metres[0], p.rect[1] = s.metres[1];
            set = t->set;
        }
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_layout, 1, 1, &set, 0, nullptr);
        vkCmdSetViewport(g.cmd, 0, 1, &vp);
        vkCmdSetScissor(g.cmd, 0, 1, &sc);
        vkCmdPushConstants(g.cmd, g.shadow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        vkCmdDraw(g.cmd, 3, 1, 0, 0);
    }
}

// the targets 8..13 that have strips this frame
static void draw_env_views(const NativeFrame &frame, VkDeviceSize &)
{
    // the mirror first: nothing in it reads it
    bool has_rvm = false;
    for (const NativeDraw &d : frame.draws)
        if (d.count && d.target == 7) has_rvm = true;
    if (has_rvm) {
        VkClearValue clear[2] = {};
        clear[0].color.float32[3] = 128 / 255.0f;
        VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = g.env_pass;
        rb.framebuffer = g.rvm_fb;
        rb.renderArea.extent = {(uint32_t)g.rvm_w, (uint32_t)g.rvm_h};
        rb.clearValueCount = 2;
        rb.pClearValues = clear;
        // Shadows: High: the mirror's go over what is drawn before NativeFrame::rvm_shadow_at, and the rest of the mirror (its
        // smoke, sparks, flares) is drawn after them, over them
        bool rvm_shadows = false;
        for (const NativeShadow &s : frame.shadows) rvm_shadows |= s.target == 7;
        const size_t rvm_cut = rvm_shadows ? std::min((size_t)frame.rvm_shadow_at, frame.draws.size()) : frame.draws.size();
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        draw_target(frame, 7, g.rvm_w, g.rvm_h, 0, rvm_cut);
        vkCmdEndRenderPass(g.cmd);
        g.rvm_ready = true;
        if (rvm_shadows) {
            all_barrier();
            depth_image_barrier(g.rvm_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            VkRenderPassBeginInfo sb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            sb.renderPass = g.rvm_post_pass;
            sb.framebuffer = g.rvm_post_fb;
            sb.renderArea.extent = {(uint32_t)g.rvm_w, (uint32_t)g.rvm_h};
            vkCmdBeginRenderPass(g.cmd, &sb, VK_SUBPASS_CONTENTS_INLINE);
            draw_shadows(frame, 7, true);
            vkCmdEndRenderPass(g.cmd);
            all_barrier();
            depth_image_barrier(g.rvm_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            if (rvm_cut < frame.draws.size()) {
                rb.renderPass = g.env_pass_load;
                rb.clearValueCount = 0;
                vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
                draw_target(frame, 7, g.rvm_w, g.rvm_h, rvm_cut);
                vkCmdEndRenderPass(g.cmd);
            }
        }
        static const bool shot_images = getenv("HP2_NATIVESHOT") != nullptr;
    if (shot_images) {
            image_barrier(g.rvm, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {(uint32_t)g.rvm_w, (uint32_t)g.rvm_h, 1};
            vkCmdCopyImageToBuffer(g.cmd, g.rvm, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.rvm_rb, 1, &copy);
            image_barrier(g.rvm, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }
    bool has[6] = {};
    for (const NativeDraw &d : frame.draws)
        if (d.count && d.target >= 8 && d.target <= 13) has[d.target - 8] = true;
    // TEMP (HP2_ENVLOG=<frames>): the alpha bit the views hold, which decides on the GS what of a view is folded into the sphere map. The
    // views copied the last time are counted first (their frame is long done), then this frame's are copied.
    static const int env_log_every = getenv("HP2_ENVLOG") ? std::max(atoi(getenv("HP2_ENVLOG")), 30) : 0;
    bool env_log_now = false;
    if (env_log_every && g.frame % env_log_every == 0) {
        if (!g_env_log_rb) make_buffer(g_env_log_rb, &g_env_log_map, 6 * 128 * 128 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);    // room for either size
        if (g_env_log_rb && g_env_log_have) {
            fprintf(g_hle_log, "envlog: frame %llu, pixels with the alpha bit clear (drawn / the clear's grey kept), of 4096:", (unsigned long long)g.frame - env_log_every);
            for (int i = 0; i < 6; i++) {
                if (!(g_env_log_have >> i & 1)) continue;
                const uint8_t *px = (const uint8_t *)g_env_log_map + (size_t)i * ENV_SIZE * ENV_SIZE * 4;
                const int grey = (96 + 16 * i) & 0xf8;
                int drawn = 0, kept = 0;
                for (int k = 0; k < ENV_SIZE * ENV_SIZE; k++)
                    if (px[k * 4 + 3] < 128) (px[k * 4] == grey && px[k * 4 + 1] == grey && px[k * 4 + 2] == grey ? kept : drawn)++;
                fprintf(g_hle_log, " view %d: %d / %d;", i, drawn, kept);
            }
            fputc(10, g_hle_log);
            // what was drawn into the views: each texture once, with what its alpha comes from
            std::string seen;
            for (const NativeDraw &d : frame.draws) {
                if (!d.count || d.target < 8 || d.target > 13) continue;
                char one[160];
                snprintf(one, sizeof one, " %s(blend %d atest %d flags %x)", d.tex ? d.tex->name : "-", d.blend, d.alpha_test, d.flags);
                if (seen.find(one) == std::string::npos && seen.size() < 3000) seen += one;
            }
            fprintf(g_hle_log, "envlog: drawn into the views:%s", seen.c_str());
            fputc(10, g_hle_log);
            fflush(g_hle_log);
        }
        g_env_log_have = 0;
        env_log_now = g_env_log_rb != VK_NULL_HANDLE;
    }
    for (int i = 0; i < 6; i++) {
        if (!has[i]) continue;
        VkClearValue clear[2] = {};
        float grey = (float)(((96 + 16 * i) & 0xf8)) / 255.0f;      // 0x60, 0x90, 0xb0 ...: seen in the GS's stream; its alpha bit is clear
        clear[0].color.float32[0] = clear[0].color.float32[1] = clear[0].color.float32[2] = grey;
        VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = g.env_pass;
        rb.framebuffer = g.env_fb[i];
        rb.renderArea.extent = {(uint32_t)ENV_SIZE, (uint32_t)ENV_SIZE};
        rb.clearValueCount = 2;
        rb.pClearValues = clear;
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        draw_target(frame, 8 + i, ENV_SIZE, ENV_SIZE);
        vkCmdEndRenderPass(g.cmd);
        g.env_ready[i] = true;
        g.env_drawn_now[i] = true;
        if (env_log_now) {
            image_barrier(g.env[i], 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {};
            copy.bufferOffset = (VkDeviceSize)i * ENV_SIZE * ENV_SIZE * 4;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {(uint32_t)ENV_SIZE, (uint32_t)ENV_SIZE, 1};
            vkCmdCopyImageToBuffer(g.cmd, g.env[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_env_log_rb, 1, &copy);
            image_barrier(g.env[i], 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            g_env_log_have |= 1 << i;
        }
    }
}

// CreateEnvMap, CreateBackEnvMap: each view drawn this frame but view 2 by its grid, then view 2 round the rim
static void fold_sphere_map(const NativeFrame &frame)
{
    if (!g.sphere_ready) {
        image_barrier(g.sphere, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black = {};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(g.cmd, g.sphere, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        image_barrier(g.sphere, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        g.sphere_ready = true;
    }
    // RefSample: x y z s t u v; a grid is 7 by 7 with its last row and column unused
    FoldVertex *out = (FoldVertex *)g.fold_map;
    int first[10] = {}, count[10] = {}, n = 0;
    auto grid = [&](const uint8_t *base, int which) {
        first[which] = n;
        auto at = [&](int r, int c) {
            float q[7];
            memcpy(q, base + (r * 7 + c) * 28, sizeof q);
            // XYZ2 is the whole pixel, trunc(s * 256 - 128 + 2048) on a target at 1920: the PS2's pixel whatever the map's size here, so
            // a larger map is the same picture, finer
            out[n++] = {std::floor(q[3] * 256.0f) / 256.0f, std::floor(q[4] * 256.0f) / 256.0f, q[5], q[6]};
        };
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 5; c++) {
                at(r, c);
                at(r, c + 1);
                at(r + 1, c);
                at(r + 1, c);
                at(r, c + 1);
                at(r + 1, c + 1);
            }
        count[which] = n - first[which];
    };
    for (int i = 0; i < 6; i++) grid((const uint8_t *)frame.samples + (size_t)i * 49 * 28, i);
    for (int i = 0; i < 4; i++) grid((const uint8_t *)frame.back_samples + (size_t)i * 49 * 28, 6 + i);
    VkDeviceSize zero = 0;
    // a view at a time, each over the map as the one before left it
    auto fold = [&](int view, int from, int vertices) {
        image_barrier(g.sphere, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        image_barrier(g.sphere_prev, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit bl = {};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {SPHERE_SIZE, SPHERE_SIZE, 1};
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1] = {SPHERE_SIZE, SPHERE_SIZE, 1};
        vkCmdBlitImage(g.cmd, g.sphere, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.sphere_prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        image_barrier(g.sphere_prev, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        image_barrier(g.sphere, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = g.fold_pass;
        rb.framebuffer = g.sphere_fb;
        rb.renderArea.extent = {(uint32_t)SPHERE_SIZE, (uint32_t)SPHERE_SIZE};
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp = {0, 0, (float)SPHERE_SIZE, (float)SPHERE_SIZE, 0, 1};
        VkRect2D sc = {{0, 0}, {(uint32_t)SPHERE_SIZE, (uint32_t)SPHERE_SIZE}};
        vkCmdSetViewport(g.cmd, 0, 1, &vp);
        vkCmdSetScissor(g.cmd, 0, 1, &sc);
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.fold_pipeline);
        vkCmdBindVertexBuffers(g.cmd, 0, 1, &g.fold_vb, &zero);
        VkDescriptorSet sets[2] = {g.env_set[view], g.sphere_prev_set};
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.fold_layout, 0, 2, sets, 0, nullptr);
        int linear = frame.env_linear;
        vkCmdPushConstants(g.cmd, g.fold_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof linear, &linear);
        vkCmdDraw(g.cmd, vertices, 1, from, 0);
        vkCmdEndRenderPass(g.cmd);
    };
    for (int i = 0; i < 6; i++)
        if (i != 2 && g.env_drawn_now[i]) fold(i, first[i], count[i]);
    if (g.env_drawn_now[2])
        for (int i = 0; i < 4; i++) fold(2, first[6 + i], count[6 + i]);
    memset(g.env_drawn_now, 0, sizeof g.env_drawn_now);
    static const bool shot_images = getenv("HP2_NATIVESHOT") != nullptr;
    if (shot_images) {
        image_barrier(g.sphere, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {(uint32_t)SPHERE_SIZE, (uint32_t)SPHERE_SIZE, 1};
        vkCmdCopyImageToBuffer(g.cmd, g.sphere, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.sphere_rb, 1, &copy);
        image_barrier(g.sphere, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
}

// ---- HP2_NATIVESHOT=<file with %d>: pictures of the renderer -------------------------------------
// Every HP2_NATIVESHOTN frames (150): n, this frame's picture, finished; s, the sphere map; r, the mirror.
static void write_bmp(const std::string &path, const void *px, int w, int h)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t size = (uint32_t)w * h * 4;
    uint8_t head[54] = {'B', 'M'};
    uint32_t v[] = {size + 54, 0, 54, 40, (uint32_t)w, (uint32_t)-h, 1u | 32u << 16, 0, size};
    memcpy(head + 2, v, sizeof v);
    fwrite(head, 1, 54, f);
    fwrite(px, 1, size, f);
    fclose(f);
}

// HP2_PIXAB=<dir>: every 120 frames the scene twice, the old shaders' then the new, and the two pictures compared to the pixel; the frames
// that differ go to <dir> as a.bmp / b.bmp pairs
static VkBuffer g_cmp_buf[2];
static void *g_cmp_map[2];
static bool g_cmp_pending;
static uint64_t g_cmp_frames, g_cmp_differ;

static void copy_color_to(int which)
{
    if (!g_cmp_buf[which] && !make_buffer(g_cmp_buf[which], &g_cmp_map[which], (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return;
    all_barrier();
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
    vkCmdCopyImageToBuffer(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cmp_buf[which], 1, &copy);
    all_barrier();
}

static void write_bmp(const std::string &path, const void *px, int w, int h);

static void compare_pictures()
{
    if (!g_cmp_pending || !g_cmp_map[0] || !g_cmp_map[1]) return;
    g_cmp_pending = false;
    const uint8_t *a = (const uint8_t *)g_cmp_map[0], *b = (const uint8_t *)g_cmp_map[1];
    size_t differ = 0;
    int most = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        bool d = false;
        for (int c = 0; c < 3; c++) {
            int k = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]);
            if (k) d = true;
            if (k > most) most = k;
        }
        differ += d;
    }
    g_cmp_frames++;
    if (differ) g_cmp_differ++;
    fprintf(g_hle_log, "native: pixab frame %llu: %zu pixels differ, by up to %d; %llu of %llu compared frames differ\n", (unsigned long long)g.frame - 1,
            differ, most, (unsigned long long)g_cmp_differ, (unsigned long long)g_cmp_frames);
    static int saved;
    if (differ && saved < 6) {
        saved++;
        char p[512];
        snprintf(p, sizeof p, "%s/%03d_new.bmp", getenv("HP2_PIXAB"), saved);
        write_bmp(p, a, W, H);
        snprintf(p, sizeof p, "%s/%03d_old.bmp", getenv("HP2_PIXAB"), saved);
        write_bmp(p, b, W, H);
    }
}

static bool g_frame_dumping;                // this frame's NativeFrame::dumping
static void shot_this_frame()
{
    static const char *shot = getenv("HP2_NATIVESHOT");
    static const int every = getenv("HP2_NATIVESHOTN") ? atoi(getenv("HP2_NATIVESHOTN")) : 150;
    static int frames;
    const bool forced = g_frame_dumping;
    if (!shot || (++frames % every && !forced)) return;
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    char path[512];
    static int presses;
    snprintf(path, sizeof path, shot, forced ? 900 + presses++ : frames / every);     // F11: 900, 901 ...
    std::string name = path;
    size_t slash = name.find_last_of("\\/");
    size_t base = slash == std::string::npos ? 0 : slash + 1;
    auto named = [&](char first) {
        std::string n = name;
        if (n.size() > base) n[base] = first;
        return n;
    };
    write_bmp(named('n'), g.rb_map, W, H);
    write_bmp(named('s'), g.sphere_rb_map, SPHERE_SIZE, SPHERE_SIZE);
    write_bmp(named('r'), g.rvm_rb_map, g.rvm_w, g.rvm_h);
}

// ---- the window ----------------------------------------------------------------------------------
// The window's swapchain, for a client area of w x h. Everything a driver may choose its own way is chosen here, so that the window is shown
// the same on every card:
//  - its size is the surface's own (the process is DPI aware, window_dpi_aware: the client area in real pixels);
//  - 8 bits a channel in sRGB, never a wide or HDR colour space a driver may list first;
//  - the driver is told not to take the screen for itself when the window covers it (VK_EXT_full_screen_exclusive, "disallowed"): NVIDIA's
//    and AMD's do by themselves otherwise, Intel's does not, and an exclusive screen has its own rules (its size is a display mode, it is
//    lost on Alt+Tab, overlays and the driver's scaling take part);
//  - opaque: the picture's alpha is 1 by then (opaque_picture).
// It is made again when the window's size changes, or the driver says it is out of date; "suboptimal" alone only if the surface's size is no
// longer the swapchain's (native_vk_draw): a driver that keeps saying it would have it made on every frame.
static void make_swapchain(int w, int h)
{
    vkDeviceWaitIdle(g.dev);
    g.swap_cw = w, g.swap_ch = h;
    g.swap_stale = g.swap_check = false;
    auto drop = [] {
        if (g.swapchain) vkDestroySwapchainKHR(g.dev, g.swapchain, nullptr);
        g.swapchain = VK_NULL_HANDLE;
        g.swap_images.clear();
    };
    static int told;
    if (g.surface_lost) {
        drop();
        vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
        g.surface = VK_NULL_HANDLE;
        auto make_surface = (PFN_vkCreateWin32SurfaceKHR)gipa(g.instance, "vkCreateWin32SurfaceKHR");
        VkWin32SurfaceCreateInfoKHR surf = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surf.hinstance = GetModuleHandleA(nullptr);
        surf.hwnd = (HWND)window_handle();
        if (!make_surface || make_surface(g.instance, &surf, nullptr, &g.surface)) {
            g.surface = VK_NULL_HANDLE;
            return;                             // lost still: the next try makes it
        }
        g.surface_lost = false;
        VkBool32 shows = VK_FALSE;              // the spec's: asked of a surface before a swapchain is made on it
        vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.family, g.surface, &shows);
        fprintf(g_hle_log, "native: the window's surface was lost and is made again\n");
    }
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    if (r) {
        if (r == VK_ERROR_SURFACE_LOST_KHR) g.surface_lost = true;
        if (told++ < 20) fprintf(g_hle_log, "native: the surface does not answer (%d)\n", (int)r);
        drop();
        return;
    }
    uint32_t n = 64;
    VkSurfaceFormatKHR formats[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, formats);
    if (!n) {
        drop();
        return;
    }
    // BGRA as the picture is, else RGBA, else whatever is 8-bit sRGB's, else the first: the blit converts
    VkSurfaceFormatKHR format = formats[0];
    int best = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        const int rank = formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ? 3 : formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ? 2 : 1;
        if (rank > best) best = rank, format = formats[i];
    }
    uint32_t m = 8;
    VkPresentModeKHR modes[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &m, modes);
    // VSync (the setting): on, every driver's FIFO, the one mode they all have, a frame shown whole at the screen's refresh. Off, shown at
    // once (it may tear), or by a mailbox where the driver has no such mode. The game keeps its own time either way: a frame that finds no
    // image free is not shown, it does not wait.
    g.swap_vsync = g_cfg.vsync != 0;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g.swap_vsync) {
        int best_mode = 0;
        for (uint32_t i = 0; i < m; i++) {
            const int rank = modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR ? 2 : modes[i] == VK_PRESENT_MODE_MAILBOX_KHR ? 1 : 0;
            if (rank > best_mode) best_mode = rank, mode = modes[i];
        }
    }
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSurfaceFullScreenExclusiveInfoEXT exclusive = {VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    exclusive.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
    if (g.no_exclusive) ci.pNext = &exclusive;
    ci.surface = g.surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = caps.currentExtent;
    if (ci.imageExtent.width == UINT32_MAX)
        ci.imageExtent = {std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width),
                          std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height)};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -(int)caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.swapchain;
    if (!ci.imageExtent.width || !ci.imageExtent.height) {      // a window with no area: nothing to show till it has one
        drop();
        return;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        if (told++ < 20) fprintf(g_hle_log, "native: the window's images cannot be copied to (usage %08x)\n", (unsigned)caps.supportedUsageFlags);
        drop();
        return;
    }
    VkSwapchainKHR made = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(g.dev, &ci, nullptr, &made);
    drop();
    if (r) {
        if (r == VK_ERROR_SURFACE_LOST_KHR) g.surface_lost = true;
        if (told++ < 20) fprintf(g_hle_log, "native: no swapchain (%d)\n", (int)r);
        return;
    }
    g.swapchain = made;
    g.swap_w = (int)ci.imageExtent.width;
    g.swap_h = (int)ci.imageExtent.height;
    g.swap_transform = ci.preTransform;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g.dev, made, &count, nullptr);
    g.swap_images.resize(count);
    vkGetSwapchainImagesKHR(g.dev, made, &count, g.swap_images.data());
    // every one of them, in the log: a window that makes its swapchain again each frame shows here (the first 20, then each 100th)
    static unsigned made_n;
    if (++made_n <= 20 || made_n % 100 == 0)
        fprintf(g_hle_log, "native: swapchain %u: %d x %d for a window of %d x %d, format %d in colour space %d of %u, %u images, present mode %d, "
                           "alpha %x of %x, transform %x, at frame %u\n", made_n, g.swap_w, g.swap_h, w, h, (int)format.format, (int)format.colorSpace, n,
                count, (int)mode, (unsigned)ci.compositeAlpha, (unsigned)caps.supportedCompositeAlpha, (unsigned)ci.preTransform, g.frame);
}

// the FPS counter: 5 x 7 digits, drawn as pixels straight on the window's image
// ---- the GPU's time, by part of the frame (HP2_NATIVELOG's line): timestamps around the environment maps, the scene with its effects, the
// picture's own passes and the window. Read back a frame later, once its fence is through.
static VkQueryPool g_qpool;
static bool g_qpool_tried, g_qpool_written;
static double g_gpu_sum[5], g_gpu_max_total;
static int g_gpu_frames;
static size_t g_fx_count;

static void gpu_mark(int i, bool first = false)
{
    if (!g_qpool && !g_qpool_tried) {
        g_qpool_tried = true;
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 5;
        if (!g.props.limits.timestampComputeAndGraphics || vkCreateQueryPool(g.dev, &qi, nullptr, &g_qpool)) g_qpool = VK_NULL_HANDLE;
    }
    if (!g_qpool) return;
    if (first) {
        vkCmdResetQueryPool(g.cmd, g_qpool, 0, 5);
        g_qpool_written = false;
    }
    vkCmdWriteTimestamp(g.cmd, first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, (uint32_t)i);
    if (i == 4) g_qpool_written = true;
}

static void gpu_times_read()
{
    if (!g_qpool || !g_qpool_written) return;
    uint64_t t[5];
    if (vkGetQueryPoolResults(g.dev, g_qpool, 0, 5, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    for (int i = 1; i < 5; i++)
        if (t[i] < t[i - 1]) return;            // a frame that made its swapchain again in the middle
    const double ns = g.props.limits.timestampPeriod;
    for (int i = 0; i < 4; i++) g_gpu_sum[i] += (double)(t[i + 1] - t[i]) * ns / 1e6;
    const double total = (double)(t[4] - t[0]) * ns / 1e6;
    g_gpu_sum[4] += total;
    g_ab_scene[g_ab] += (double)(t[2] - t[1]) * ns / 1e6;
    g_ab_total[g_ab] += total;
    g_ab_n[g_ab]++;
    if (total > g_gpu_max_total) g_gpu_max_total = total;
    g_gpu_frames++;
}

// the copy of the vertices and lights into the GPU's memory, timed by itself: by HP2_STAGEAB's variant
static VkQueryPool g_copy_pool;
static bool g_copy_written;
static int g_copy_variant;
static size_t g_fresh_bytes;               // the cache's new vertices, a second's
static double g_copy_sum[2], g_copy_cpu[2];
static int g_copy_n[2];

static void copy_times_read()
{
    if (!g_copy_pool || !g_copy_written) return;
    uint64_t t[2];
    if (vkGetQueryPoolResults(g.dev, g_copy_pool, 0, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[1] < t[0]) return;
    g_copy_sum[g_copy_variant] += (double)(t[1] - t[0]) * g.props.limits.timestampPeriod / 1e6;
    g_copy_n[g_copy_variant]++;
}

// the word in the corner after a save
static std::atomic<int64_t> g_notice_until;

void native_notice_saved()
{
    g_notice_until = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1500;
}

static void draw_notice(VkImage image, int right, int bottom, VkDeviceSize place)
{
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() >= g_notice_until) return;
    // A to Z, 5 x 7, a row a byte; the word in the language in use (pclang.h), capitals without their accents
    static const uint8_t kLetter[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
        {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
        {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
        {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
        {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},
        {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
        {17, 17, 17, 10, 4, 4, 4},    {31, 1, 2, 4, 8, 16, 31}};
    // Latin-1's letters with accents, 0xC0..0xFF, as the letter without
    static const char kPlain[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string word;
    for (unsigned char ch : tr("SAVED")) {
        char c = ch >= 0xc0 ? kPlain[ch - 0xc0] : (char)ch;
        c = (char)toupper((unsigned char)c);
        if ((c >= 'A' && c <= 'Z') || c == ' ') word += c;
    }
    const int n = (int)word.size();
    int scale = std::max(g.swap_h / 270, 1);
    while (scale > 1 && (VkDeviceSize)(n * 6 * scale + 2 * scale) * (9 * scale) * 4 > kFpsBytes / 2) scale--;
    const int cell = 6 * scale;
    int w = n * cell + 2 * scale, h = 9 * scale, x = right - w - 8, y = bottom - h - 8;
    if (!n || place + (VkDeviceSize)w * h * 4 > kFpsBytes || x < 0 || y < 0) return;
    uint32_t *out = (uint32_t *)((char *)g.fps_map + place);
    for (int i = 0; i < w * h; i++) out[i] = 0xFF000000u;
    for (int d = 0; d < n; d++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (word[(size_t)d] != ' ' && kLetter[word[(size_t)d] - 'A'][row] >> (4 - col) & 1)
                    for (int py = 0; py < scale; py++)
                        for (int px = 0; px < scale; px++)
                            out[(size_t)(row * scale + scale + py) * w + (d * cell + col * scale + scale + px)] = 0xFFFFFFFFu;
    VkBufferImageCopy c = {};
    c.bufferOffset = place;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(g.cmd, g.fps_buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

static void draw_fps(VkImage image, int x, int y)
{
    static const uint8_t kDigit[10][7] = {{14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},   {14, 17, 1, 2, 4, 8, 31},
                                          {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2},   {31, 16, 30, 1, 1, 17, 14},
                                          {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},     {14, 17, 17, 14, 17, 17, 14},
                                          {14, 17, 17, 15, 1, 2, 12}};
    char text[16];
    int digits = snprintf(text, sizeof text, "%d", g.fps_shown);
    int scale = std::max(g.swap_h / 270, 1), cell = 6 * scale;
    int w = digits * cell + 2 * scale, h = 9 * scale;
    if ((VkDeviceSize)w * h * 4 > kFpsBytes || x + w > g.swap_w || y + h > g.swap_h) return;
    uint32_t *out = (uint32_t *)g.fps_map;
    for (int i = 0; i < w * h; i++) out[i] = 0xFF000000u;
    for (int d = 0; d < digits; d++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (kDigit[text[d] - '0'][row] >> (4 - col) & 1)
                    for (int py = 0; py < scale; py++)
                        for (int px = 0; px < scale; px++)
                            out[(size_t)(row * scale + scale + py) * w + (d * cell + col * scale + scale + px)] = 0xFFFFFFFFu;
    VkBufferImageCopy c = {};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(g.cmd, g.fps_buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

// the picture onto the window's image: the screen shows 444 of the target's 448 lines (the GS's DISPLAY), the game's frames stop short of the last ones
static void blit_to_window(VkImage image, const NativeFrame &frame)
{
    image_barrier(image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {};
    black.float32[3] = 1.0f;                // opaque, as the picture is (opaque_picture)
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    // each write over the last waits for it: the clear, the picture, the counter and the word. Without, AMD's GPUs do them at once, by tiles
    auto after_write = [&] {
        image_barrier(image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    };
    after_write();
    // a TV picture whatever its size in pixels, 4:3 or the race's: the largest such box in the middle of the window
    float aspect = (4.0f / 3.0f) / (frame.wide ? native_narrow() : 1.0f);
    int bw = g.swap_w, bh = g.swap_h;
    if ((float)bw > (float)bh * aspect) bw = (int)((float)bh * aspect + 0.5f);
    else bh = (int)((float)bw / aspect + 0.5f);
    int from = frame.wide ? 0 : (W - W43) / 2, across = frame.wide ? W : W43;
    // the picture was made for this box: pixel for pixel, where the rounding of the two would leave a pixel between them
    const int rows = H * 444 / 448;
    if (abs(bw - across) <= 2 && across <= g.swap_w) bw = across;
    if (abs(bh - rows) <= 2 && rows <= g.swap_h) bh = rows;
    int bx = (g.swap_w - bw) / 2, by = (g.swap_h - bh) / 2;
    VkImageBlit bl = {};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {from, 0, 0};
    bl.srcOffsets[1] = {from + across, rows, 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[0] = {bx, by, 0};
    bl.dstOffsets[1] = {bx + bw, by + bh, 1};
    vkCmdBlitImage(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_LINEAR);          // the same size as a rule; another only while the window changes
    after_write();
    if (g_cfg.show_fps) draw_fps(image, bx + 8, by + 8);
    after_write();
    draw_notice(image, bx + bw, by + bh, kFpsBytes / 2);
    image_barrier(image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

// the frame's rects of the depth buffer, for the next frame's FetchVRAM
static void copy_depth_rects(const NativeFrame &frame)
{
    g.depth_rects = frame.depth_rects;
    g.depth_places.clear();
    if (frame.depth_rects.empty()) return;
    depth_barrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    g.depth_ox = ox, g.depth_sx = sx;
    VkDeviceSize place = 0;
    for (const NativeDepthRect &r : frame.depth_rects) {
        int x0 = std::clamp((int)(ox + r.x * sx), 0, W - 1), y0 = std::clamp((int)(r.y * sy), 0, H - 1);
        int w = std::clamp((int)std::ceil(r.w * sx), 1, W - x0), h = std::clamp((int)std::ceil(r.h * sy), 1, H - y0);
        VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
        if (place + bytes > g.depth_cap) break;
        VkBufferImageCopy c = {};
        c.bufferOffset = place;
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.imageOffset = {x0, y0, 0};
        c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyImageToBuffer(g.cmd, g.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.depth_rb, 1, &c);
        g.depth_places.push_back(place);
        place += bytes;
    }
    g.depth_bytes = place;
    g.depth_new = true;
    depth_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

// The frame in two parts (2026-11-26): what is recorded so far, the scene and the copy of its depth rects, goes to the GPU now on a
// fence of its own, and the rest (SMAA, the grading, the window) is recorded into the other command buffer and goes at the end as
// before. The game's thread wants those rects in StuffSun (native.cpp, depth_settle) and no longer waits for the last passes, nor
// for this thread to record them. The two parts are one queue's, in order: the frame's own fence, at the end, is past both. The
// depth is final here: nothing after the copy draws into it. HP2_DEPTHEARLY=0: all of the frame at the end, as it was.
static void depth_send_early(const NativeFrame &frame, const VkCommandBufferBeginInfo &bi)
{
    static const bool off = getenv("HP2_DEPTHEARLY") && atoi(getenv("HP2_DEPTHEARLY")) == 0;
    if (off || !g.cmd_b || !g.depth_fence) return;
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};     // the copies, for the host to read
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(g.cmd);
    vkResetFences(g.dev, 1, &g.depth_fence);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    const VkResult sent = vkQueueSubmit(g.queue, 1, &si, g.depth_fence);
    std::swap(g.cmd, g.cmd_b);
    vkResetCommandBuffer(g.cmd, 0);
    vkBeginCommandBuffer(g.cmd, &bi);
    if (sent != VK_SUCCESS) return;         // (the frame's own submit says what the device said)
    g.depth_early = true;
    native_depth_sent(frame.handed);
}

// The last frame's rects kept apart, for a game's thread that does not wait for the renderer (native.cpp's queue of one): taken on
// the renderer's thread once the card is done with that frame (draw_frame, after its wait for the fence), read under the mutex.
static std::mutex g_depth_kept_mutex;
static struct {
    std::vector<NativeDepthRect> rects;
    std::vector<VkDeviceSize> places;
    std::vector<char> bytes;
    float ox = 0, sx = 0;
    int w = 0, h = 0;                       // the picture's size they were taken at
} g_depth_kept;
static void depth_keep()
{
    if (!g.depth_new) return;
    g.depth_new = false;
    std::lock_guard<std::mutex> hold(g_depth_kept_mutex);
    g_depth_kept.rects.clear();
    g_depth_kept.places.clear();
    if (!g.depth_rb_map || g.depth_bytes > g.depth_cap) return;
    g_depth_kept.rects = g.depth_rects;
    g_depth_kept.places = g.depth_places;
    g_depth_kept.bytes.assign((const char *)g.depth_rb_map, (const char *)g.depth_rb_map + g.depth_bytes);
    g_depth_kept.ox = g.depth_ox, g_depth_kept.sx = g.depth_sx;
    g_depth_kept.w = W, g_depth_kept.h = H;
}

static void depth_words(const NativeDepthRect &r, uint32_t *out, const float *depth, float ox, float sx, int pw, int ph)
{
    const float sy = ph / 448.0f;
    int x0 = std::clamp((int)(ox + r.x * sx), 0, pw - 1), y0 = std::clamp((int)(r.y * sy), 0, ph - 1);
    int w = std::clamp((int)std::ceil(r.w * sx), 1, pw - x0), h = std::clamp((int)std::ceil(r.h * sy), 1, ph - y0);
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            int px = std::clamp((int)(ox + (r.x + x + 0.5f) * sx) - x0, 0, w - 1), py = std::clamp((int)((r.y + y + 0.5f) * sy) - y0, 0, h - 1);
            out[(size_t)y * r.w + x] = (uint32_t)(depth[(size_t)py * w + px] * 16777216.0f);
        }
}

bool native_vk_depth(const NativeDepthRect &r, uint32_t *out, bool idle)
{
    if (!g.ready) return false;
    if (!idle) {
        // The renderer may be at work: what it kept. The rect asked for is the last frame's, and what is kept may be the frame
        // before that one's, with the rect a little elsewhere: the same rect by its size then (the sun's, a frame earlier).
        std::lock_guard<std::mutex> hold(g_depth_kept_mutex);
        int at = -1;
        for (size_t i = 0; i < g_depth_kept.rects.size() && i < g_depth_kept.places.size() && at < 0; i++) {
            const NativeDepthRect &have = g_depth_kept.rects[i];
            if (have.x == r.x && have.y == r.y && have.w == r.w && have.h == r.h) at = (int)i;
        }
        for (size_t i = 0; i < g_depth_kept.rects.size() && i < g_depth_kept.places.size() && at < 0; i++)
            if (g_depth_kept.rects[i].w == r.w && g_depth_kept.rects[i].h == r.h) at = (int)i;
        if (at < 0) return false;
        const NativeDepthRect &have = g_depth_kept.rects[(size_t)at];
        // (the copy is as long as the rects' places say: depth_keep took all of them)
        depth_words(have, out, (const float *)(g_depth_kept.bytes.data() + g_depth_kept.places[(size_t)at]), g_depth_kept.ox, g_depth_kept.sx,
                    g_depth_kept.w, g_depth_kept.h);
        return true;
    }
    for (size_t i = 0; i < g.depth_rects.size() && i < g.depth_places.size(); i++) {
        const NativeDepthRect &have = g.depth_rects[i];
        if (have.x != r.x || have.y != r.y || have.w != r.w || have.h != r.h) continue;
        // (the renderer's thread may be recording the frame's last passes now: the rects and their fence are the frame's first part's)
        vkWaitForFences(g.dev, 1, g.depth_early ? &g.depth_fence : &g.fence, VK_TRUE, UINT64_MAX);
        depth_words(r, out, (const float *)((const char *)g.depth_rb_map + g.depth_places[i]), g.depth_ox, g.depth_sx, W, H);
        return true;
    }
    return false;
}

// the picture so far into the scene image, with its levels down
// levels false: the first level alone, for a pass that reads no other (the others keep what an earlier copy left in them, and
// their layout: so only once a whole copy has been made)
static void copy_scene(bool levels)
{
    all_barrier();
    VkImageBlit bl = {};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {W, H, 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[1] = {W, H, 1};
    if (!levels && g_post_merge && g.scene_ready) {
        image_barrier(g.scene, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdBlitImage(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        image_barrier(g.scene, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        return;
    }
    image_barrier(g.scene, 0, g.scene_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdBlitImage(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    int mw = W, mh = H;
    for (int l = 1; l < g.scene_levels; l++) {
        image_barrier(g.scene, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        bl.srcSubresource.mipLevel = l - 1;
        bl.srcOffsets[1] = {mw, mh, 1};
        mw = std::max(mw / 2, 1);
        mh = std::max(mh / 2, 1);
        bl.dstSubresource.mipLevel = l;
        bl.dstOffsets[1] = {mw, mh, 1};
        vkCmdBlitImage(g.cmd, g.scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        image_barrier(g.scene, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    image_barrier(g.scene, g.scene_levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    g.scene_ready = true;
}

// Dithering (Advanced), on the finished picture of every frame, a race's or a menu's or a film's. With the picture at 16 bits a
// channel (g_color_wide, choose_color_format): a step's worth of noise, a grain to each pixel of the window, so that the steps of
// 1 / 255 the window's 8 bits make of it come out as grain and not as bands. With a picture of 8 bits (a card without the wide
// format): the old pass, where the pixels around one are all within a few steps of its colour it takes their average, and the
// noise goes over the lot (native_extra.frag). The picture's last step: after the antialiasing and the
// brightness, contrast and gamma, and after the copy the next frame may blend with, so the noise does not build up.
static void dither_picture(const NativeFrame &frame)
{
    if (!post_dithered()) return;
    copy_scene(false);
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    ExtraPush p = {};
    p.rect[0] = ox;
    p.rect[1] = 0;
    p.rect[2] = (float)W - ox;
    p.rect[3] = (float)H;
    p.size[0] = (float)W;
    p.size[1] = (float)H;
    p.frame = (float)(frame.number & 255);
    p.lines = sy;
    // the noise's grain is a pixel of the window: a picture drawn finer than the window (Render Scale above 100) is averaged down
    // as it is shown, and noise of its own pixels would be averaged away with it
    p.grain = g.swap_h > 0 && H > g.swap_h ? (float)H / (float)g.swap_h : 1.0f;
    p.wide = g_color_wide ? 1.0f : 0.0f;
    // the settings' brightness, contrast and gamma here too (grade_picture has left them to this pass)
    if (g_post_merge && g_color_wide && post_graded()) {
        p.brightness = g_cfg.brightness / 100.0f, p.contrast = g_cfg.contrast / 100.0f, p.gamma = g_cfg.gamma / 100.0f;
        p.graded = 1.0f;
    }
    const bool last = post_folds_alpha();
    p.opaque = last ? 1.0f : 0.0f;
    if (last) g_opaque_done = true;
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.post_pass;
    rb.framebuffer = g.post_fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{(int32_t)ox, 0}, {(uint32_t)(W - 2 * (int)ox), (uint32_t)H}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.extra_pipeline);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.extra_layout, 0, 1, &g.extra_set, 0, nullptr);
    vkCmdPushConstants(g.cmd, g.extra_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
}

// Particles: High, the soft sprites: the picture's depth as it is now, for the sprites to read (between two of the picture's passes)
static void copy_depth_soft()
{
    // the copy's own changes of layout, each waiting for what wrote it last and holding back what reads or writes it next
    const auto copy_layout = [](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g.depth_copy;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    all_barrier();
    depth_barrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // (the last picture's sprites read the copy: this waits for them, and the old contents go)
    copy_layout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy c = {};
    c.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    c.dstSubresource = c.srcSubresource;
    c.extent = {(uint32_t)W, (uint32_t)H, 1};
    vkCmdCopyImage(g.cmd, g.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.depth_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    copy_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    depth_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    all_barrier();
}

// Ambient Occlusion's world (FX_AO_WORLD, ПОРА111): the view's depth before its first car is drawn, into depth_world, the view's
// rectangle only (on a split screen the other view's part stays as its own copy made it)
static void copy_depth_world(const NativeFrame &frame, const NativeFx &f)
{
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    const int32_t x0 = std::max((int32_t)(ox + f.rect[0] * sx), 0), y0 = std::max((int32_t)(f.rect[1] * sy), 0);
    const int32_t x1 = std::min((int32_t)(ox + (f.rect[0] + f.rect[2]) * sx + 0.999f), W);
    const int32_t y1 = std::min((int32_t)((f.rect[1] + f.rect[3]) * sy + 0.999f), H);
    if (x1 <= x0 || y1 <= y0) return;
    const auto world_layout = [](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g.depth_world;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    all_barrier();
    depth_barrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    world_layout(g.world_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy c = {};
    c.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    c.dstSubresource = c.srcSubresource;
    c.srcOffset = c.dstOffset = {x0, y0, 0};
    c.extent = {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1};
    vkCmdCopyImage(g.cmd, g.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.depth_world, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    world_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    g.world_ready = true;
    depth_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    all_barrier();
}

// Shadows with multisampling: a view's shadows over the picture's samples, between two of the picture's passes
static void draw_shadows_samples(const NativeFrame &frame, const NativeFx &f)
{
    all_barrier();
    depth_image_barrier(g.depth_ms, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.ms_read_pass;
    rb.framebuffer = g.ms_read_fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    draw_shadows(frame, f.target, false, true);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
    depth_image_barrier(g.depth_ms, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

// the render pass of the picture is over: lay this on it
static void draw_fx(const NativeFrame &frame, const NativeFx &f, int row)
{
    if (f.kind != FX_FOG && f.kind != FX_SHADOW && f.kind != FX_AO) {
        copy_scene();
    } else {
        all_barrier();
        // the fog does not read the scene image, but its pipeline has it bound: it must not be in no layout at all
        if (!g.scene_ready) {
            image_barrier(g.scene, 0, g.scene_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue black = {};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)g.scene_levels, 0, 1};
            vkCmdClearColorImage(g.cmd, g.scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            image_barrier(g.scene, 0, g.scene_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            g.scene_ready = true;
        }
    }
    depth_barrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Ambient Occlusion: first into its own image, at every second pixel of the view
    ShadowPush ao = {};
    VkViewport ao_vp = {};
    if (f.kind == FX_AO) {
        float ox, sx, sy;
        active_region(frame, ox, sx, sy);
        ao_vp = {ox + f.rect[0] * sx, f.rect[1] * sy, f.rect[2] * sx, f.rect[3] * sy, 0, 1};
        memcpy(ao.to, f.m, sizeof ao.to);
        ao.view[0] = ao_vp.x, ao.view[1] = ao_vp.y, ao.view[2] = ao_vp.width, ao.view[3] = ao_vp.height;
        // it reaches 7 m (the user, 2026-11-25, after 0.8, 1.5 and 3 m were all "no wow"; Off / Low / High for a day, On / Off again)
        ao.rect[0] = 7.0f;                  // the reach in metres
        ao.rect[1] = (float)g_cfg.ao_strength * 0.01f;      // the strength
        ao.rect[2] = 2.0f;                  // the image is half the picture each way
        ao.mode = f.world && g.world_ready ? 1.0f : 0.0f;   // 1: the view's world depth is in depth_world (FX_AO_WORLD)
        if (!g.world_ready) {               // never copied into: bound all the same, so in the layout the set says, with nothing in it
            VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = g.depth_world;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            g.world_ready = true;
        }
        VkClearValue none = {};
        VkRenderPassBeginInfo ab = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        ab.renderPass = g.ao_pass;
        ab.framebuffer = g.ao_fb;
        ab.renderArea.extent = {(uint32_t)g.ao_w, (uint32_t)g.ao_h};
        ab.clearValueCount = 1;
        ab.pClearValues = &none;
        vkCmdBeginRenderPass(g.cmd, &ab, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport hv = {ao_vp.x * 0.5f, ao_vp.y * 0.5f, ao_vp.width * 0.5f, ao_vp.height * 0.5f, 0, 1};
        const int32_t hx0 = std::max((int32_t)hv.x, 0), hy0 = std::max((int32_t)hv.y, 0);
        const int32_t hx1 = std::min((int32_t)(hv.x + hv.width + 0.999f), g.ao_w), hy1 = std::min((int32_t)(hv.y + hv.height + 0.999f), g.ao_h);
        if (hv.width >= 1 && hv.height >= 1 && hx1 > hx0 && hy1 > hy0) {
            const VkRect2D hs = {{hx0, hy0}, {(uint32_t)(hx1 - hx0), (uint32_t)(hy1 - hy0)}};
            vkCmdSetViewport(g.cmd, 0, 1, &hv);
            vkCmdSetScissor(g.cmd, 0, 1, &hs);
            vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ao_pipeline);
            const VkDescriptorSet with_world[2] = {g.post_set, g.ao_world_set};
            vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_layout, 0, 2, with_world, 0, nullptr);
            vkCmdPushConstants(g.cmd, g.shadow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof ao, &ao);
            vkCmdDraw(g.cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(g.cmd);
        all_barrier();
    }
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.post_pass;
    rb.framebuffer = g.post_fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    if (f.kind == FX_AO) {                  // and from that image over the view
        const int32_t x0 = std::max((int32_t)ao_vp.x, 0), y0 = std::max((int32_t)ao_vp.y, 0);
        const int32_t x1 = std::min((int32_t)(ao_vp.x + ao_vp.width), W), y1 = std::min((int32_t)(ao_vp.y + ao_vp.height), H);
        if (ao_vp.width >= 1 && ao_vp.height >= 1 && x1 > x0 && y1 > y0) {
            const VkRect2D as = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
            const VkDescriptorSet both[2] = {g.post_set, g.ao_set};
            vkCmdSetViewport(g.cmd, 0, 1, &ao_vp);
            vkCmdSetScissor(g.cmd, 0, 1, &as);
            vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ao_apply_pipeline);
            vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadow_layout, 0, 2, both, 0, nullptr);
            vkCmdPushConstants(g.cmd, g.shadow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof ao, &ao);
            vkCmdDraw(g.cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(g.cmd);
        all_barrier();
        depth_barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        return;
    }
    if (f.kind == FX_SHADOW) {              // Shadows: High: the view's shadows, by the depth just read
        draw_shadows(frame, f.target, false);
        vkCmdEndRenderPass(g.cmd);
        all_barrier();
        depth_barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        return;
    }
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    VkViewport vp = {ox + f.rect[0] * sx, f.rect[1] * sy, f.rect[2] * sx, f.rect[3] * sy, 0, 1};
    VkRect2D sc = {{(int32_t)vp.x, (int32_t)vp.y}, {(uint32_t)vp.width, (uint32_t)vp.height}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    PostPush p = {};
    // The depth of field: the port's own blur, the GS's amounts by depth (native_post.frag). HP2_NATIVEEXACT keeps the GS's half buffer;
    // HP2_DOFSPLIT draws the left half of the picture the GS's way and the right half the port's, for comparing
    static const bool dof_exact = getenv("HP2_NATIVEEXACT") != nullptr, dof_split = getenv("HP2_DOFSPLIT") != nullptr;
    p.mode = f.kind == FX_FOG ? 0 : f.kind == FX_GREY ? 1 : dof_exact ? 2 : dof_split ? 4 : 3;
    p.rect[0] = vp.x;
    p.rect[1] = vp.y;
    p.rect[2] = vp.x + vp.width;
    p.rect[3] = vp.y + vp.height;
    p.row = row;
    p.size[0] = (float)W;
    p.size[1] = (float)H;
    p.lod = std::max(std::log2(sy), 0.0f);
    p.scale[0] = sx;
    p.scale[1] = sy;
    p.origin[0] = ox;
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.post_pipeline[f.kind == FX_GREY ? 2 : f.darken ? 1 : 0]);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.post_layout, 0, 1, &g.post_set, 0, nullptr);
    vkCmdPushConstants(g.cmd, g.post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
    depth_barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

// this frame's palettes, a row each
static void upload_palettes(const NativeFrame &frame, VkDeviceSize &at)
{
    g.palette_rows = 0;
    if (frame.fx.empty()) return;
    {
        uint64_t wanted = 0;
        for (const NativeFx &f : frame.fx) wanted += fx_palette(f.kind);
        native_limit(NL_PALETTES, wanted, kPaletteRows);
    }
    image_barrier(g.palette, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (size_t i = 0; i < frame.fx.size(); i++) {
        if (!fx_palette(frame.fx[i].kind)) continue;        // it reads no palette
        if (g.palette_rows >= kPaletteRows || at + sizeof frame.fx[i].palette > g.staging_cap) break;
        memcpy((char *)g.staging_map + at, frame.fx[i].palette, sizeof frame.fx[i].palette);
        VkBufferImageCopy c = {};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, g.palette_rows++, 0};
        c.imageExtent = {256, 1, 1};
        vkCmdCopyBufferToImage(g.cmd, g.staging, g.palette, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        at += sizeof frame.fx[i].palette;
    }
    image_barrier(g.palette, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// where the CPU's frame goes, for HP2_NATIVELOG's "cpu" line: the game between two frames, the wait for the GPU, the copy of the vertices,
// the recording, the submit and present
static double g_cpu_sum[6];
static int g_cpu_n;
static LARGE_INTEGER g_cpu_mark, g_cpu_freq;
static double cpu_lap()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_cpu_freq.QuadPart) QueryPerformanceFrequency(&g_cpu_freq);
    const double ms = g_cpu_mark.QuadPart ? (double)(t.QuadPart - g_cpu_mark.QuadPart) * 1000.0 / (double)g_cpu_freq.QuadPart : 0;
    g_cpu_mark = t;
    return ms;
}

static bool draw_frame(const NativeFrame &frame, bool &uploaded);
std::atomic<uint32_t> g_native_epoch{0};
static std::vector<NativeTexUpload> g_tex_pending;      // decoded and not yet on the GPU, in order

void native_vk_gpus(std::vector<std::string> &names, std::string &now)
{
    std::lock_guard<std::mutex> hold(g_gpu_mutex);
    names = g_gpu_names;
    now = g_gpu_now;
}

// the screen the window is on: its refresh rate, 0: not known
int native_vk_screen_hz()
{
    // HP2_FAKEHZ=<n>: a test's, the screen taken to have that rate (the queue of one and its catch-up on a 60 Hz screen)
    static const int fake = getenv("HP2_FAKEHZ") ? atoi(getenv("HP2_FAKEHZ")) : 0;
    if (fake > 0) return fake;
    static ULONGLONG looked;
    static int hz;
    const ULONGLONG now = GetTickCount64();
    if (now - looked < 1000 && looked) return hz;
    looked = now ? now : 1;
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEA dm = {};
    dm.dmSize = sizeof dm;
    hz = 0;
    if (window_handle() && GetMonitorInfoA(MonitorFromWindow((HWND)window_handle(), MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hz = (int)dm.dmDisplayFrequency;
    if (hz <= 1) hz = 0;                    // 0 or 1: the screen's default
    return hz;
}

// The pictures a second to draw for a frame that asks for `asked` of them (above the game's 60), 0: the frame alone. With VSync a
// picture waits for the screen, so more pictures than the screen has refreshes would hold the game back: no more than those.
double native_vk_clock()
{
    LARGE_INTEGER t, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// Without VSync every picture is shown at once, torn or not, whatever the screen.
static int picture_rate(int asked)
{
    int rate = asked;
    const int hz = native_vk_screen_hz();
    if (rate >= 1000) return rate;          // Unlimited: no rate of its own (native_vk_draw)
    if (g_cfg.vsync && hz && rate > hz) rate = hz;
    if (rate <= 60) rate = 0;
    static int told = -1;
    if (told != rate) {
        told = rate;
        fprintf(g_hle_log, "native: frame rate %d asked: the screen has %d Hz, VSync is %s: %d pictures a second%s\n", asked, hz, g_cfg.vsync ? "on" : "off",
                rate ? rate : 60, rate ? ", the game's 60 frames with pictures between them" : " (the game's frames alone)");
    }
    return rate;
}

bool native_vk_draw(NativeFrame &frame)
{
    native_frame_clips(frame);              // the clip matrices the game's thread left to this one
    // the textures first, whatever comes of the frame: the game's thread counts them as loaded
    for (NativeTexUpload &u : frame.uploads) g_tex_pending.push_back(std::move(u));
    frame.uploads.clear();
    g_frame_dumping = frame.dumping;
    bool uploaded = false, ok = false;
    static double next_f;                   // where the next picture is, in frames from the frame before the one it is drawn from
    // a frame made before the game's thread heard of the cache's loss draws from places that hold something else
    if (frame.epoch == g_native_epoch.load()) {
        const int rate = frame.doubled ? picture_rate(frame.pictures) : 0;
        if (frame.doubled && frame.pictures >= 1000) {
            // Unlimited (the user, 2026-11-17): as many pictures as the card makes in the sixtieth of a second a frame is shown for,
            // each at the point between the two frames that the clock says it will be shown at; the frame itself is the last. With
            // VSync a picture waits for the screen, and the screen's rate is what comes of it. The game's frames are not held.
            static LONGLONG begin;          // this frame's pictures are shown from here on, for a sixtieth
            static double took = 0.004;     // a picture's time in seconds, smoothed
            LARGE_INTEGER t, f;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t);
            const LONGLONG span = f.QuadPart / 60;
            if (!begin || t.QuadPart - begin > 2 * span || begin - t.QuadPart > span) begin = t.QuadPart;     // lost step: from now
            double was = 0;
            for (int made = 0;; made++) {
                QueryPerformanceCounter(&t);
                double at = (double)(t.QuadPart - begin) / (double)span + took * 60.0;
                if (at < was + 0.01) at = was + 0.01;
                const bool last = at >= 1.0 - took * 30.0 || made >= 64;
                if (last) at = 1.0;
                was = at;
                g_pic_f = (float)at;
                g_between = !last;
                g_pic_last = last;
                bool first = false;
                const bool drawn = draw_frame(frame, last ? uploaded : first);
                LARGE_INTEGER after;
                QueryPerformanceCounter(&after);
                took = took * 0.9 + 0.1 * (double)(after.QuadPart - t.QuadPart) / (double)f.QuadPart;
                if (took < 0.0002) took = 0.0002;
                if (last) {
                    ok = drawn;
                    break;
                }
                if (!drawn) made = 64;      // nothing came of it (the window is being made anew): the frame alone, and on
            }
            begin += span;
            next_f = 0;
            g_pic_f = 1.0f, g_between = false, g_pic_last = true;
        } else if (rate && g_cfg.vsync) {
            // With VSync: the pictures are 60 / rate of a frame apart, whatever the frame they fall in: this frame has those at
            // next_f, next_f + that, ... up to 1, the frame itself (at 120: the half and 1; at 144 two or three, 1 only now and
            // then). They are shown 1 / rate s apart, and the screen's refresh spaces them. (Without VSync it is done by the
            // clock, below; on a model of both, work/nshots/t5/sim_rate.py, this one keeps the game at its 60 frames on a 75 or
            // 100 Hz screen, where the other holds it at a whole number of refreshes a frame.)
            static LONGLONG due;
            static int rate_was;
            LARGE_INTEGER t, f;
            QueryPerformanceFrequency(&f);
            const double step = 60.0 / (double)rate;
            if (rate != rate_was || next_f <= 0 || next_f > step + 1e-6) next_f = step, rate_was = rate, due = 0;
            int n = 0;
            while (next_f + n * step <= 1.0 + 1e-6) n++;
            // The queue of one (native.cpp, hand_over): a frame that has waited for this thread more than 12.5 ms says the screen
            // shows fewer pictures a second than the game's frames ask for (a screen is never exactly its nominal rate). One
            // picture between the frames is then left out, of this frame or of the next that has more than one, and the
            // renderer is ahead again by a refresh: on the model once in five to ten seconds, in place of the game waiting
            // for the renderer in four frames of ten.
            static bool owed;
            QueryPerformanceCounter(&t);
            if (frame.queued && frame.made_s > 0 && native_vk_clock() - frame.made_s > 0.0125) owed = true;
            if (!frame.queued) owed = false;
            for (int i = 0; i < n; i++) {
                double at = next_f + i * step;
                if (at > 1.0 - 1e-6) at = 1.0;
                if (owed && i == 0 && n > 1) {
                    owed = false;
                    continue;
                }
                QueryPerformanceCounter(&t);
                // A picture between two frames that is already more than a picture late is left out (2026-11-17, with the rates up
                // to 500): a card that cannot make so many would otherwise take longer over a frame than the game does, and hold
                // the game back. The frame itself, the last, is always drawn.
                if (i < n - 1 && due && t.QuadPart > due + f.QuadPart / rate) {
                    due += f.QuadPart / rate;
                    continue;
                }
                if (due > t.QuadPart && due - t.QuadPart < f.QuadPart / 20) {
                    for (;;) {
                        QueryPerformanceCounter(&t);
                        const LONGLONG left = due - t.QuadPart;
                        if (left <= 0) break;
                        if (left > f.QuadPart * 3 / 2000) Sleep(1);     // more than 1.5 ms: a sleep (the timer's resolution is 1 ms, iop_kernel.cpp)
                    }
                } else {
                    due = t.QuadPart;       // late, or the first: from now
                }
                due += f.QuadPart / rate;
                g_pic_f = (float)at;
                g_between = at < 1.0;
                g_pic_last = i == n - 1;
                bool first = false;
                const bool drawn = draw_frame(frame, g_pic_last ? uploaded : first);
                if (g_pic_last) ok = drawn;
            }
            next_f += n * step - 1.0;
            g_pic_f = 1.0f, g_between = false, g_pic_last = true;
        } else if (rate) {
            // A set rate without VSync, by the clock as Unlimited is (the user, 2026-11-18). The pictures are 1 / rate s apart on
            // the clock, and each shows the place between the two frames that the clock is at when it is started: this frame's
            // pictures have the time from `begin` on, for as long as the game says the frame is (NativeFrame::step_s, its own
            // real time step), and a picture started `off` into that is at off / length + lead of the way. In step it is the grid
            // it always was (at 120 the half and the frame itself; at 144 two or three a frame, the frame itself now and then):
            // lead, 1 / p where rate / 60 is p / q in lowest terms, is what makes the grid meet the frame itself.
            // What is new is what happens when the card does not make that many. It was: the pictures of a frame were all drawn,
            // and the game's thread waited for them, so the game itself ran slower (a tester: 120 asked at 3840 x 2160, "the
            // world lags behind me sometimes"; on the model, work/nshots/t5/sim_rate.py, 12 ms a picture at 120 held the game at
            // 42 frames a second). Now a frame's pictures have a sixtieth of a second from when the frame is taken, the time the
            // game needs for the next one: a picture between that would not leave the last one room in it is left out, the last
            // one starts early enough to end with it, and a picture started late shows where the clock is, not where the grid
            // had it. The game keeps its 60 frames, and the pictures are as many as the card makes.
            static double begin, next_off, took = 0.004;
            static int rate_was;
            LARGE_INTEGER t, f;
            QueryPerformanceFrequency(&f);
            const auto clock = [&] {
                QueryPerformanceCounter(&t);
                return (double)t.QuadPart / (double)f.QuadPart;
            };
            const double gap = 1.0 / (double)rate;
            double len = frame.step_s;
            if (!(len > 1.0 / 240.0 && len < 1.0 / 15.0)) len = 1.0 / 60.0;
            int common = rate;
            for (int other = 60; other;) {
                const int rest = common % other;
                common = other, other = rest;
            }
            const double lead = (double)common / (double)rate;
            double now = clock();
            // the first, or out of step by more than a frame: from now
            if (rate != rate_was || begin == 0 || now - begin > 2.0 * len + 0.02 || begin - now > len) begin = now, next_off = 0;
            rate_was = rate;
            const double limit = len * (1.0 - lead) + 0.02 * len;       // (the game's step is in ticks of a quarter millisecond)
            const double end = now + 1.0 / 60.0;                        // the game has its next frame by then
            for (int made = 0; made < 64; made++) {
                const bool last = next_off + gap > limit;       // nothing of this frame's after it
                double due = begin + next_off;
                now = clock();
                if (!last && (now > due + gap || (now > due ? now : due) + 2.0 * took > end + 0.0005)) {
                    next_off += gap;
                    continue;
                }
                if (last && due > end - took) due = end - took;
                if (due > now && due - now < 0.05) {
                    for (;;) {
                        now = clock();
                        const double left = due - now;
                        if (left <= 0) break;
                        if (left > 0.0015) Sleep(1);    // more than 1.5 ms: a sleep (the timer's resolution is 1 ms, iop_kernel.cpp)
                    }
                }
                now = clock();
                double at = (now - begin) / len + lead;
                if (at > 0.98) at = 1.0;
                if (at < 0.01) at = 0.01;
                g_pic_f = (float)at;
                g_between = at < 1.0;
                g_pic_last = last;
                bool first = false;
                const bool drawn = draw_frame(frame, last ? uploaded : first);
                took = took * 0.9 + 0.1 * (clock() - now);
                if (took < 0.0002) took = 0.0002;
                next_off += gap;
                if (last) {
                    ok = drawn;
                    break;
                }
            }
            begin += len;
            next_off -= len;
            next_f = 0;
            g_pic_f = 1.0f, g_between = false, g_pic_last = true;
        } else {
            next_f = 0;
            ok = draw_frame(frame, uploaded);
        }
        if (!uploaded) g_native_epoch++;
    }
    g_cache_cap_pub.store(cache_verts_now());
    return ok;
}

static bool draw_frame(const NativeFrame &frame, bool &uploaded)
{
    if (g.failed) return false;
    const double game_ms = cpu_lap();
    if (!g.ready) {
        if (!window_handle()) return false;     // not there yet
        window_dpi_thread();
        if (!setup()) {
            g.failed = true;
            fprintf(g_hle_log, "native: no Vulkan device\n");
#ifdef _WIN32
            MessageBoxA((HWND)window_handle(), "This game needs a graphics card with Vulkan.", "Need for Speed: Hot Pursuit 2", MB_ICONERROR);
#endif
            return false;
        }
        g.ready = true;
    }
    // the settings or the window's size changed: this frame's textures are gone with the rest, so it is not drawn
    track_lines();
    if (g.key != settings_key() || g.lines != g_lines) {
        native_rebuild_mark(true);          // the game's thread need not wait meanwhile (native.cpp, hand_over)
        const bool made = rebuild();
        native_rebuild_mark(false);
        if (!made) {
            g.failed = true;
            fprintf(g_hle_log, "native: the renderer could not be made again\n");
            return false;
        }
        return true;
    }
    device_vertices();
    const size_t bytes = frame.vertices.size() * sizeof(NativeVertex), fresh_bytes = frame.fresh.size() * sizeof(NativeVertex),
                 index_bytes = frame.indices.size() * sizeof(uint32_t);
    // made for a cache of another size (the first frame, or the GPU's buffer has grown since): not drawn, the window keeps the
    // last picture while the game goes on (and native.cpp empties its cache)
    if (frame.dyn_base != cache_verts_now()) return false;
    cpu_lap();
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    if (frame.queued) depth_keep();         // the card is done with the frame before: its rects of the depth, for the game's thread
    free_dead_textures();
    const double wait_ms = cpu_lap();
    // The buffers that grow, now that the GPU is done with the last frame. The vertices first: if their buffer is another now,
    // this frame was made for the old one and is not drawn.
    native_limit(NL_OWN_VERTS, bytes + index_bytes, g.vb_in_vram ? g.vb_side : kMaxBytes);
    if (g.vb_in_vram && vertices_grow(frame, bytes + index_bytes)) return false;
    if (!g.vb_in_vram && bytes + index_bytes > kMaxBytes) {
        static int told;
        if (told++ < 5) fprintf(g_hle_log, "native: %zu vertices, %zu indices are more than a frame can take; not drawn\n", frame.vertices.size(), frame.indices.size());
        return false;
    }
    if (g.staging_want > g.staging_cap && !staging_room(g.staging_want)) {
        if (!g.staging_refused || g.staging_want < g.staging_refused) g.staging_refused = g.staging_want;
        fprintf(g_hle_log, "native: no memory to upload a texture of %llu KB through: it stays as it is\n", (unsigned long long)(g.staging_want >> 10));
    }
    g.staging_want = 0;
    if (g.can_merge) merge_room((uint32_t)frame.draws.size(), (uint32_t)(frame.ranges.size() + frame.draws.size()));
    if (g_pic_last) {
        const VkDeviceSize depth_bytes = depth_rects_bytes(frame);
        native_limit(NL_DEPTH, depth_bytes, g.depth_cap);
        depth_room(depth_bytes);
    }
    vkResetFences(g.dev, 1, &g.fence);
    gpu_times_read();
    copy_times_read();
    dp_read();
    compare_pictures();
    if (!vertex_room(bytes + fresh_bytes + index_bytes + 4)) {
        g.failed = true;
        fprintf(g_hle_log, "native: no memory for the vertices\n");
        return false;
    }
    LARGE_INTEGER c0, c1, cf;
    QueryPerformanceCounter(&c0);
    // the host's copy: the frame's own vertices, the cache's new ones, the indices
    if (bytes) memcpy(g.vb_map, frame.vertices.data(), bytes);
    if (fresh_bytes) memcpy((char *)g.vb_map + bytes, frame.fresh.data(), fresh_bytes);
    if (index_bytes) memcpy((char *)g.vb_map + bytes + fresh_bytes, frame.indices.data(), index_bytes);
    QueryPerformanceCounter(&c1);
    QueryPerformanceFrequency(&cf);
    g_copy_cpu[0] += (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / (double)cf.QuadPart;
    g_fresh_bytes += fresh_bytes;
    uploaded = true;
    g.vb = g.vb_in_vram ? g.vb_dev : g.vb_stage;
    // in the GPU's buffer: right after the frame's own vertices (with no indices, anywhere inside the buffer will do)
    g.draw_idx_off = g.vb_in_vram ? g.dyn_off + (index_bytes ? bytes : 0) : bytes + fresh_bytes;
    g.draw_dyn_first = g.vb_in_vram ? g.cache_verts : 0;
    size_t light_bytes = frame.lights.size() * sizeof(NativeLight);
    native_limit(NL_LIGHTS, frame.lights.size(), g.light_cap / sizeof(NativeLight));
    if (!lights_room(light_bytes)) {
        static int told;
        if (told++ < 5) fprintf(g_hle_log, "native: %zu lights are more than a frame can have: none of them this frame\n", frame.lights.size());
        light_bytes = 0;
    }
    if (light_bytes) memcpy(g.lights_map, frame.lights.data(), light_bytes);
    const double copy_ms = cpu_lap();
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(g.cmd, &bi);
    gpu_mark(0, true);
    // the host's copies into the GPU's memory, before anything is drawn from them
    {
        bool copied = false;
        if (!g_copy_pool) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            vkCreateQueryPool(g.dev, &qi, nullptr, &g_copy_pool);
        }
        g_copy_written = false;
        if (g_copy_pool) {
            vkCmdResetQueryPool(g.cmd, g_copy_pool, 0, 2);
            vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_copy_pool, 0);
        }
        if (g.vb_in_vram) {
            std::vector<VkBufferCopy> regions;
            if (bytes) regions.push_back({0, g.dyn_off, bytes});
            for (const NativeFreshRun &r : frame.fresh_runs)
                if (r.count)
                    regions.push_back({bytes + (VkDeviceSize)r.src * sizeof(NativeVertex), (VkDeviceSize)r.dst * sizeof(NativeVertex),
                                       (VkDeviceSize)r.count * sizeof(NativeVertex)});
            if (index_bytes) regions.push_back({bytes + fresh_bytes, g.dyn_off + bytes, index_bytes});
            if (!regions.empty()) {
                vkCmdCopyBuffer(g.cmd, g.vb_stage, g.vb_dev, (uint32_t)regions.size(), regions.data());
                copied = true;
            }
        }
        if (light_bytes && g.lights != g.lights_stage) {
            VkBufferCopy c = {0, 0, light_bytes};
            vkCmdCopyBuffer(g.cmd, g.lights_stage, g.lights, 1, &c);
            copied = true;
        }
        if (copied) {
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb,
                                 0, nullptr, 0, nullptr);
        }
        if (g_copy_pool) {
            vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_copy_pool, 1);
            g_copy_written = true;
            g_copy_variant = 0;
        }
    }
    g.frame++;
    native_limit(NL_MERGE_DRAWS, g.merge_draws, g.draws_cap);       // the picture before's
    native_limit(NL_MERGE_CMDS, g.merge_cmds, g.cmds_cap);
    g.merge_draws = g.merge_cmds = 0;
    g_ab = g_ab_on ? (int)(g.frame & 1) : 0;
    g_dp_now = false;
    if (g_dp_on && g.frame % (uint32_t)g_dp_period == 0) {
        if (!g_dp_pool) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kDpMax;
            vkCreateQueryPool(g.dev, &qi, nullptr, &g_dp_pool);
            if (g_stats_query && vkCmdBeginQuery && vkCmdEndQuery) {
                qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
                if (vkCreateQueryPool(g.dev, &qi, nullptr, &g_dp_stat_pool)) g_dp_stat_pool = VK_NULL_HANDLE;
            }
        }
        if (g_dp_pool) {
            vkCmdResetQueryPool(g.cmd, g_dp_pool, 0, kDpMax);
            if (g_dp_stat_pool) vkCmdResetQueryPool(g.cmd, g_dp_stat_pool, 0, kDpMax);
            g_dp_now = true;
            g_dp_pending = true;
            g_dp_open = false;
            g_dp_fresh = true;
            g_dp_frame = g.frame;
            g_dp_n = 0;
        }
    }
    VkDeviceSize staged = 0;
    {   // the textures decoded for this frame, and what an earlier one had no room for, in the order they were decoded
        // Frames that were not drawn (a change of settings, the cache emptied) leave their uploads here, and a texture that changes every
        // frame (a menu's animation, a movie) is then here several times: only its newest is the texture now
        size_t done = 0;
        for (; done < g_tex_pending.size(); done++) {
            bool newer = false;
            for (size_t later = done + 1; later < g_tex_pending.size() && !newer; later++) newer = g_tex_pending[later].tex == g_tex_pending[done].tex;
            if (!newer && !upload_texture(g_tex_pending[done], staged)) break;
        }
        g_tex_pending.erase(g_tex_pending.begin(), g_tex_pending.begin() + (ptrdiff_t)done);
    }
    for (const NativeDraw &d : frame.draws)
        if (d.tex && d.count && d.tex->vk) ((VkTex *)d.tex->vk)->used = g.frame;
    // what has not been drawn for a quarter of a minute goes, and is decoded again if it comes back. A few a frame: freed all at once every
    // 300 frames, they cost that frame 5 to 16 ms (a vkFreeMemory each), a stutter
    {
        static size_t cursor;
        int freed = 0;
        for (int looked = 0; looked < 64 && !g.textures.empty() && freed < 2; looked++) {
            if (cursor >= g.textures.size()) cursor = 0;
            VkTex *const old = g.textures[cursor];
            bool go = g.frame - old->used > 900;
            if (go && old->owner) {
                // and not one the game's thread has asked for since: its frames run ahead of these, and what it asked for it counts on
                std::lock_guard<std::mutex> hold(g_native_tex_mutex);
                go = frame.number >= old->owner->frame && frame.number - old->owner->frame > 900;
                if (go) {
                    old->owner->vk = nullptr;
                    old->owner->loaded = false;
                    old->owner = nullptr;
                }
            }
            if (go) {
                free_texture(old);
                g.textures[cursor] = g.textures.back();
                g.textures.pop_back();
                freed++;
            } else {
                cursor++;
            }
        }
    }
    if (frame.road_specular && frame.road_specular->vk) ((VkTex *)frame.road_specular->vk)->used = g.frame;
    if (frame.env_override && frame.env_override->vk) ((VkTex *)frame.env_override->vk)->used = g.frame;
    upload_palettes(frame, staged);
    native_limit(NL_STAGING, staged, g.staging_cap);
    native_limit(NL_TEXTURES, g.textures.size(), (uint64_t)kTexSets * (1 + g.tex_pools.size()));
    VkDeviceSize zero_vb = 0;
    // the picture between two frames (120) has the mirror and the reflections as the frame before left them: once a frame, not twice.
    // With Reflections at High they are drawn for it too, each draw halfway as the picture's own are (NativeFrame::mid_views).
    draw_shadow_map(frame);                 // Shadows: High
    if (g_pic_last || frame.mid_views) {
        draw_env_views(frame, zero_vb);
        all_barrier();
        fold_sphere_map(frame);
        all_barrier();
    }
    gpu_mark(1);
    static const bool pixab = getenv("HP2_PIXAB") != nullptr;
    const bool compare_now = pixab && g.clip_far && g.frame % 120 == 0;
    for (int round = compare_now ? 0 : 1; round < 2; round++) {
    if (compare_now) g_ab = round == 0 ? 1 : 0;
    VkClearValue clear[2] = {};
    clear[0].color.float32[3] = 128 / 255.0f;   // the frame's alpha on the GS: rgba 00000080
    clear[1].depthStencil.depth = 0;
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.pass;
    rb.framebuffer = g.fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    rb.clearValueCount = 2;
    rb.pClearValues = clear;
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    // The view, cut at what is laid over it: the frame's effects, each at its draw, and before the first of the port's own
    // particles the copy of the depth they read (fx -1). An effect and the copy at one draw: the effect first.
    struct Cut {
        size_t at;
        int fx;
    };
    static std::vector<Cut> cuts;
    cuts.clear();
    for (size_t i = 0; i < frame.fx.size(); i++) cuts.push_back({frame.fx[i].at, (int)i});
    g_soft_now = false;
    // The copy is made before the first such particle of EACH view. One copy a frame, before the frame's first, was before
    // player 1's: on a split screen player 2's world was not drawn yet, so player 2's smoke had nothing behind it in the copy
    // and was cut hard by the road, except in the frames where player 1's view had no particle and the first was player 2's:
    // its look went back and forth with what the other view showed (the user, 2026-11-23: "the particles flicker for player 2").
    if (g.soft_ok) {
        uint32_t views = 0;                 // by the target's number: its copy is asked for
        for (size_t i = 0; i < frame.draws.size(); i++) {
            const NativeDraw &d = frame.draws[i];
            if (!d.count || !(d.flags & kSoftKinds) || d.target > 4 || d.depth_write || d.blend == NB_OPAQUE) continue;
            if (views >> d.target & 1) continue;
            views |= 1u << d.target;
            size_t where = 0;
            while (where < cuts.size() && cuts[where].at <= i) where++;
            cuts.insert(cuts.begin() + where, Cut{i, -1});
        }
    }
    size_t at = 0;
    for (const Cut &cut : cuts) {
        const NativeFx *f = cut.fx >= 0 ? &frame.fx[cut.fx] : nullptr;
        const int row = f ? fx_row(frame, (size_t)cut.fx) : -1;
        if (f && fx_palette(f->kind) && row >= g.palette_rows) continue;        // no row of the palette image for it
        if (cut.at < at) continue;          // out of order: not this way
        draw_target(frame, 1, W, H, at, cut.at);
        at = cut.at;
        dp_mark((uint32_t)cut.at, DP_FX, f ? (uint32_t)f->kind : (uint32_t)DP_FX_SOFT_DEPTH, false);      // (HP2_DRAWPROF: what follows is the effect's)
        vkCmdEndRenderPass(g.cmd);
        bool resolved = false;              // the effect went over the resolved picture: it has to go back into the samples
        if (!f) {
            copy_depth_soft();
            g_soft_now = true;
        } else if (f->kind == FX_AO_WORLD) {
            copy_depth_world(frame, *f);
        } else if (f->kind == FX_SHADOW && g.samples != VK_SAMPLE_COUNT_1_BIT && g.shadow_ms_pipeline) {
            draw_shadows_samples(frame, *f);
        } else {
            draw_fx(frame, *f, row);
            if (g.samples != VK_SAMPLE_COUNT_1_BIT) copy_scene(), resolved = true;
        }
        rb.renderPass = g.pass_load;
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        if (resolved) {
            // the effect went on the resolved picture: back into every sample. The edges drawn so far stay as they were resolved
            VkViewport whole = {0, 0, (float)W, (float)H, 0, 1};
            VkRect2D all = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
            vkCmdSetViewport(g.cmd, 0, 1, &whole);
            vkCmdSetScissor(g.cmd, 0, 1, &all);
            vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.restore_pipeline);
            vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.restore_layout, 0, 1, &g.restore_set, 0, nullptr);
            vkCmdDraw(g.cmd, 3, 1, 0, 0);
        }
        g_dp_fresh = true;
    }
    draw_target(frame, 1, W, H, at);
    g_soft_now = false;
    if (g_merge_stat && ++g_ms.frames >= 600) {
        const double n = (double)g_ms.frames;
        fprintf(g_hle_log, "mergestat: a frame's main picture: %.0f draws (%.0f of one pass, %.0f opaque), %.0f passes with %.0f changes of pipeline and %.0f of "
                           "texture, %.0f draw calls; in runs alike but for the matrix and the light: %.0f\n",
                g_ms.draws / n, g_ms.singles / n, g_ms.opaque / n, g_ms.passes / n, g_ms.pipelines / n, g_ms.sets / n, g_ms.calls / n, g_ms.runs / n);
        memset(&g_ms, 0, sizeof g_ms);
    }
    dp_mark((uint32_t)frame.draws.size(), DP_END, 0, false);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
    if (compare_now) copy_color_to(round == 0 ? 1 : 0);
    }
    if (compare_now) g_cmp_pending = true;
    gpu_mark(2);
    g_fx_count += frame.fx.size();
    // what the next frame's DisplayBlendPrevious reads, if this one asked for it
    bool wants_prev = false;
    for (const NativeDraw &d : frame.draws)
        if (d.rt == NATIVE_RT_PREVIOUS) wants_prev = true;
    if (g_pic_last) g.prev_ready = wants_prev;
    if (wants_prev && g_pic_last) {
        image_barrier(g.prev, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit bl = {};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {W, H, 1};
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1] = {W, H, 1};
        vkCmdBlitImage(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        image_barrier(g.prev, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (g_pic_last) {
        copy_depth_rects(frame);
        g.depth_early = false;
        if (!g.depth_places.empty()) depth_send_early(frame, bi);
    }
    g_opaque_done = false;
    smaa_picture();
    grade_picture(frame);
    dither_picture(frame);
    gpu_mark(3);
    // HP2_MIDSHOTS=1: the pictures taken are those drawn between (for looking at them), not the frames' own
    static const bool shots_asked = getenv("HP2_NATIVESHOT") != nullptr, mid_shots = getenv("HP2_MIDSHOTS") != nullptr;
    const bool shots = shots_asked && (!frame.doubled || g_pic_last != mid_shots);
    if (shots) {
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        all_barrier();                      // after the passes that wrote the picture
        vkCmdCopyImageToBuffer(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback, 1, &copy);
    }
    // the window: its size, then an image if one is free (a frame that finds none is simply not shown)
    RECT client = {};
    GetClientRect((HWND)window_handle(), &client);
    int cw = client.right, ch = client.bottom;
    bool on_screen = false;
    uint32_t image = 0;
    // what the driver said of the swapchain, by acquire or present: out of date (or lost) and it is made again; suboptimal and its size is
    // looked at first; "nothing free now" is no news
    auto swap_said = [](VkResult r, const char *where) {
        if (r == VK_SUCCESS || r == VK_NOT_READY || r == VK_TIMEOUT) return;
        if (r == VK_SUBOPTIMAL_KHR) {
            g.swap_check = true;
            return;
        }
        g.swap_stale = true;
        if (r == VK_ERROR_SURFACE_LOST_KHR) g.surface_lost = true;
        static int told;
        if (r != VK_ERROR_OUT_OF_DATE_KHR && told++ < 20) fprintf(g_hle_log, "native: %s: %d at frame %u\n", where, (int)r, g.frame);
    };
    if (cw > 0 && ch > 0) {
        // The surface's size is what counts, and the window's client size only says when to look: the two are read at different moments
        // of a window that is changing, and where Windows scales a window they are not the same numbers at all. A swapchain the surface's
        // size still fits stays, whatever the client size has become.
        bool resized = cw != g.swap_cw || ch != g.swap_ch;
        bool again = !g.swapchain || g.swap_stale || g.swap_vsync != (g_cfg.vsync != 0);
        if (!again && (resized || g.swap_check)) {
            g.swap_check = false;
            VkSurfaceCapabilitiesKHR caps;
            const VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
            if (cr == VK_SUCCESS && caps.currentExtent.width != UINT32_MAX) {
                again = caps.currentTransform != g.swap_transform || (int)caps.currentExtent.width != g.swap_w || (int)caps.currentExtent.height != g.swap_h;
                if (!again) g.swap_cw = cw, g.swap_ch = ch;
            } else {
                again = resized || cr != VK_SUCCESS;        // a surface with no size of its own takes the window's
            }
            resized = again;
        }
        // made on each of several frames in a row, it does not hold (a driver's state, a window in between two sizes): it is left alone for
        // half a second at a time, and the frames go to the one there is, or to none, rather than the device waiting on every frame
        if (again && !resized && g.frame < g.swap_rest_until) again = false;
        if (again) {
            g.swap_streak = g.frame - g.swap_made_frame <= 1 ? g.swap_streak + 1 : 1;
            g.swap_made_frame = g.frame;
            if (g.swap_streak >= 4) {
                static int told;
                if (told++ < 10) fprintf(g_hle_log, "native: the swapchain was made on %d frames in a row, left alone for 30 frames\n", g.swap_streak);
                g.swap_rest_until = g.frame + 30;
                g.swap_streak = 0;
            }
            vkEndCommandBuffer(g.cmd);          // nothing is waiting on it: make_swapchain waits for the device
            VkSubmitInfo before = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            before.commandBufferCount = 1;
            before.pCommandBuffers = &g.cmd;
            vkQueueSubmit(g.queue, 1, &before, VK_NULL_HANDLE);
            make_swapchain(cw, ch);
            vkResetCommandBuffer(g.cmd, 0);
            vkBeginCommandBuffer(g.cmd, &bi);
        }
        if (g.swapchain) {
            VkResult r = vkAcquireNextImageKHR(g.dev, g.swapchain, 0, g.acquired, VK_NULL_HANDLE, &image);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) on_screen = true;
            swap_said(r, "no image of the window's");
        }
    }
    if (on_screen) {
        if (!g_opaque_done) opaque_picture();
        blit_to_window(g.swap_images[image], frame);
    }
    // the semaphore the present waits on: the image's own
    VkSemaphore present_wait = g.rendered;
    if (on_screen) {
        while (g.present_sems.size() <= image) {
            VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore sem = VK_NULL_HANDLE;
            vkCreateSemaphore(g.dev, &sci, nullptr, &sem);
            g.present_sems.push_back(sem);
        }
        if (g.present_sems[image]) present_wait = g.present_sems[image];
    }
    gpu_mark(4);
    vkEndCommandBuffer(g.cmd);
    const double record_ms = cpu_lap();
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    if (on_screen) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &g.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &present_wait;
    }
    {   // a device that is lost draws nothing more: said once, where it was a silent still picture
        const VkResult submitted = vkQueueSubmit(g.queue, 1, &si, g.fence);
        static bool told;
        if (submitted != VK_SUCCESS && !told) {
            told = true;
            fprintf(g_hle_log, "native: the frame was not taken by the graphics device (%d%s) at frame %u\n", (int)submitted,
                    submitted == VK_ERROR_DEVICE_LOST ? ", the device is lost" : "", g.frame);
        }
    }
    if (on_screen) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &present_wait;
        pi.swapchainCount = 1;
        pi.pSwapchains = &g.swapchain;
        pi.pImageIndices = &image;
        VkResult r = vkQueuePresentKHR(g.queue, &pi);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) g.presented++, stall_frame();
        swap_said(r, "the frame was not shown");
    }
    // The picture standing while the game goes on: a frame that found no image of the window's free is not shown (the acquire above does
    // not wait), and a run of such frames is a still picture that nothing else here knows of. Said when it ends, with what the last
    // acquire answered, for a run of a tenth of a second or more (the stall catcher's third part, stall.h).
    {
        static LARGE_INTEGER freq, shown_at;
        static unsigned missed;
        static int told;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq), QueryPerformanceCounter(&shown_at);
        QueryPerformanceCounter(&now);
        if (on_screen) {
            const double ms = (now.QuadPart - shown_at.QuadPart) * 1000.0 / freq.QuadPart;
            if (missed >= 3 && ms >= 100.0 && g.frame > 300 && told++ < 300)
                fprintf(g_hle_log, "native: the picture stood for %.0f ms at frame %u: %u frames in a row found no image of the window's free (%d images)\n",
                        ms, g.frame, missed, (int)g.swap_images.size());
            missed = 0, shown_at = now;
        } else if (g.swapchain) missed++;
    }
    g.waiting = true;
    const double submit_ms = cpu_lap();
    // a long frame, end to end: where it went (HP2_NATIVELOG)
    {
        static const bool log = getenv("HP2_NATIVELOG") != nullptr || getenv("HP2_LONGFRAMES") != nullptr;
        const double total = game_ms + wait_ms + copy_ms + record_ms + submit_ms;
        static int told;
        // (always for a frame of more than a tenth of a second, the stall catcher's other half: stall.h)
        const int cd_us = g_stall_cd_us.exchange(0), rpc_us = g_stall_rpc_us.exchange(0);
        if (((log && total > 21.0) || total > 100.0) && g.frame > 300 && told < 2000) {
            told++;
            fprintf(g_hle_log, "native: long frame %u: %.1f ms = game %.1f (lists %.1f, textures %.1f for %u, the disc %.1f, the IOP %.1f), wait for the gpu %.1f, "
                               "copy %.1f, recording %.1f, submit and present %.1f; %.2f MB new vertices, %.2f MB of textures, %zu vertices\n",
                    g.frame, total, game_ms, frame.list_ms, frame.tex_ms, frame.tex_decoded, cd_us / 1000.0, rpc_us / 1000.0, wait_ms, copy_ms, record_ms,
                    submit_ms, fresh_bytes / 1048576.0, staged / 1048576.0, frame.strip_vertices + frame.vertices.size());
        }
    }
    g_cpu_sum[0] += game_ms;
    g_cpu_sum[1] += wait_ms;
    g_cpu_sum[2] += copy_ms;
    g_cpu_sum[3] += record_ms;
    g_cpu_sum[4] += submit_ms;
    g_cpu_sum[5] += frame.list_ms;
    g_cpu_n++;
    // the game's own frames, whatever the window got to see
    uint64_t now = GetTickCount64();
    if (!g.fps_t0) g.fps_t0 = now;
    g.fps_frames++;
    // the longest frame of the second, end to end, by the precise clock: a hitch shows here before it shows in the count
    static LARGE_INTEGER freq, last;
    static double worst_ms;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (last.QuadPart) {
        const double ms = (double)(t.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (ms > worst_ms) worst_ms = ms;
    }
    last = t;
    if (now - g.fps_t0 >= 1000) {
        static const bool log = getenv("HP2_NATIVELOG") != nullptr;
        if (log) {
            fprintf(g_hle_log, "native: %d frames in the last second, %d shown, window %dx%d, worst %.1f ms, t %llu", g.fps_frames, g.presented, g.swap_w,
                    g.swap_h, worst_ms, (unsigned long long)(now / 1000));
            if (g_gpu_frames)
                fprintf(g_hle_log, " | gpu %.2f ms (max %.2f): env %.2f scene %.2f post %.2f window %.2f, fx %.1f a frame, %zu vertices, %zu draws",
                        g_gpu_sum[4] / g_gpu_frames, g_gpu_max_total, g_gpu_sum[0] / g_gpu_frames, g_gpu_sum[1] / g_gpu_frames, g_gpu_sum[2] / g_gpu_frames,
                        g_gpu_sum[3] / g_gpu_frames, (double)g_fx_count / g_gpu_frames, frame.strip_vertices + frame.vertices.size(), frame.draws.size());
            fprintf(g_hle_log, "\n");
            if (g_cpu_n)
                fprintf(g_hle_log, "native: cpu a frame: game %.2f ms (lists %.2f), wait for the gpu %.2f, vertices %.2f, recording %.2f, submit and present %.2f\n",
                        g_cpu_sum[0] / g_cpu_n, g_cpu_sum[5] / g_cpu_n, g_cpu_sum[1] / g_cpu_n, g_cpu_sum[2] / g_cpu_n, g_cpu_sum[3] / g_cpu_n, g_cpu_sum[4] / g_cpu_n);
            memset(g_cpu_sum, 0, sizeof g_cpu_sum);
            g_cpu_n = 0;
            if (g_copy_n[0])
                fprintf(g_hle_log, "native: copy: gpu %.2f ms, cpu %.2f ms; %zu indices, %zu of the frame's own vertices, %.2f MB new to the cache a frame\n",
                        g_copy_sum[0] / g_copy_n[0], g_copy_cpu[0] / g_copy_n[0], frame.indices.size(), frame.vertices.size(),
                        (double)g_fresh_bytes / g_copy_n[0] / 1048576.0);
            g_fresh_bytes = 0;
            memset(g_copy_sum, 0, sizeof g_copy_sum);
            memset(g_copy_cpu, 0, sizeof g_copy_cpu);
            memset(g_copy_n, 0, sizeof g_copy_n);
            if (g_ab_on && g_ab_n[0] && g_ab_n[1])
                fprintf(g_hle_log, "native: ab scene new %.3f old %.3f, gpu new %.3f old %.3f, %zu draws\n", g_ab_scene[0] / g_ab_n[0],
                        g_ab_scene[1] / g_ab_n[1], g_ab_total[0] / g_ab_n[0], g_ab_total[1] / g_ab_n[1], frame.draws.size());
            memset(g_ab_scene, 0, sizeof g_ab_scene);
            memset(g_ab_total, 0, sizeof g_ab_total);
            memset(g_ab_n, 0, sizeof g_ab_n);
            memset(g_gpu_sum, 0, sizeof g_gpu_sum);
            g_gpu_max_total = 0;
            g_gpu_frames = 0;
            g_fx_count = 0;
            worst_ms = 0;
            g.presented = 0;
        }
        g.fps_shown = (int)(g.fps_frames * 1000 / (now - g.fps_t0));
        g.fps_frames = 0;
        g.fps_t0 = now;
    }
    shot_this_frame();
    return true;
}
