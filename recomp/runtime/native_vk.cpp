// The renderer, the Vulkan side: a picture of its own, blitted to the window through a swapchain (native.h). The picture's size has
// nothing to do with the GS's: clip space is resolution-free.
#include "native.h"

#include "hw.h"
#include "pcconfig.h"
#include "pclang.h"

#include <functional>
#include <algorithm>
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

#define NV_INSTANCE(X)                                                                                     \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr)     \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                              \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define NV_DEVICE(X)                                                                                         \
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
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCmdCopyBuffer) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed)
#define NV_DECLARE(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance vkCreateInstance;
NV_INSTANCE(NV_DECLARE)
NV_DEVICE(NV_DECLARE)

// the picture's size: scale in hp2.ini times 512 x 448; HP2_NATIVESIZE=<w>x<h> sets it
static int W = 1024, H = 896;       // the race's picture
static int W43 = 1024;              // the 4:3 middle of it
// a frame's vertices, 32 bytes each: 4 million. The draw distance takes a race at 200% to half a million and more, the mirror and 16:9 with it
static const VkDeviceSize kMaxBytes = 128 * 1024 * 1024, kStaging = 64 * 1024 * 1024, kFoldBytes = 1024 * 1024, kLightBytes = 4 * 1024 * 1024;
static const VkDeviceSize kFpsBytes = 256 * 1024, kDepthBytes = 8 * 1024 * 1024;
static const int ENV_SIZE = 64, SPHERE_SIZE = 256;

struct VkTex {
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    VkDescriptorSet set;
    uint32_t used;
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
};

struct FoldVertex { float s, t, u, v; };

struct PostPush {
    int mode, row;
    float size[2];
    float lod;
    float pad;
    float scale[2];
    float origin[2];
};

static struct {
    bool ready, failed;
    int key;                                // the settings this was all made for
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
    VkDeviceSize vb_cap, dyn_off, idx_off;
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
    VkPipeline grade_pipeline;              // brightness, contrast, gamma
    VkPipelineLayout grade_layout;
    VkDescriptorSet grade_set;
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
    bool prev_ready;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkSampler sampler[32];                  // repeat s, repeat t, mag linear, min linear, mip linear
    VkBuffer staging;
    void *staging_map;
    std::vector<VkTex *> textures;
    uint32_t frame;
    VkDescriptorSetLayout light_layout;
    VkDescriptorSet light_set;
    VkBuffer lights, lights_stage, fold_vb;  // the lights as the vertices: the host's copy, the GPU's (lights == lights_stage without one)
    void *lights_map, *fold_map;
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
    VkFence fence;
    bool waiting;
    // the window
    VkInstance instance;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swap_images;
    int swap_w, swap_h;
    bool swap_stale;
    VkSemaphore acquired, rendered;
    std::vector<VkSemaphore> present_sems;  // per swapchain image: what its present waits on (one for all may still be in use by the last)
    VkBuffer fps_buf;
    void *fps_map;
    int fps_shown, fps_frames, presented;
    uint64_t fps_t0;
    // eTransferZBufferRect's rects of the last frame
    VkBuffer depth_rb;
    void *depth_rb_map;
    std::vector<NativeDepthRect> depth_rects;
    std::vector<VkDeviceSize> depth_places;
    float depth_ox, depth_sx;
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

static PFN_vkCreateGraphicsPipelines real_vkCreateGraphicsPipelines;
static VKAPI_ATTR VkResult VKAPI_CALL track_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o)
{
    VkResult r = real_vkCreateGraphicsPipelines(d, c, n, i, a, o);
    if (r == VK_SUCCESS && g_tracking)
        for (uint32_t k = 0; k < n; k++) {
            VkPipeline h = o[k];
            g_undo.push_back([d, h] { ((PFN_vkDestroyPipeline)vkGetDeviceProcAddr(d, "vkDestroyPipeline"))(d, h, nullptr); });
        }
    return r;
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
static const VkDeviceSize kSide = 16 * 1024 * 1024;  // a frame's own vertices, and its indices, in the GPU's buffer

uint32_t native_vk_cache_vertices() { return g.vb_in_vram ? g.cache_verts : 0; }

static void device_vertices()
{
    static bool tried;
    if (tried || getenv("HP2_HOSTBUFFERS")) return;
    tried = true;
    for (VkDeviceSize total : {(VkDeviceSize)256 << 20, (VkDeviceSize)128 << 20, (VkDeviceSize)64 << 20}) {
        if (make_buffer_in(g.vb_dev, nullptr, total, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &g.vb_dev_mem)) {
            g.vb_in_vram = true;
            g.cache_verts = (uint32_t)((total - 2 * kSide) / sizeof(NativeVertex));
            g.dyn_off = (VkDeviceSize)g.cache_verts * sizeof(NativeVertex);
            g.idx_off = g.dyn_off + kSide;
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

static void free_texture(VkTex *t)
{
    vkFreeDescriptorSets(g.dev, g.pool, 1, &t->set);
    vkDestroyImageView(g.dev, t->view, nullptr);
    vkDestroyImage(g.dev, t->image, nullptr);
    vkFreeMemory(g.dev, t->mem, nullptr);
    if (t->owner) t->owner->vk = nullptr;
    delete t;
}

// a decoded texture into the GPU, with its levels made from the base (the GPU picks the level for now)
static void upload_texture(NativeTexture &tex, VkDeviceSize &at)
{
    VkDeviceSize bytes = (VkDeviceSize)tex.px.size() * 4;
    if (at + bytes > kStaging) return;      // the next frame
    if (tex.vk) {
        VkTex *old = (VkTex *)tex.vk;
        g.textures.erase(std::find(g.textures.begin(), g.textures.end(), old));
        free_texture(old);
    }
    uint32_t levels = (uint32_t)tex.levels;
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
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g.pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g.set_layout;
    vkAllocateDescriptorSets(g.dev, &da, &t->set);
    bind_view(t->set, t->view, g.sampler[(tex.repeat_s ? 1 : 0) | (tex.repeat_t ? 2 : 0) | (tex.mag_linear ? 4 : 0) | (tex.min_linear ? 8 : 0) | (tex.mip_linear ? 16 : 0)]);

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
    t->owner = &tex;
    t->used = g.frame;
    tex.vk = t;
    tex.dirty = false;
    std::vector<uint32_t>().swap(tex.px);
    g.textures.push_back(t);
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
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = instance_extensions;
    VkInstance inst;
    if (!vkCreateInstance || vkCreateInstance(&ci, nullptr, &inst)) return false;
    g.instance = inst;
#define NV_LOAD_I(name) name = (PFN_##name)gipa(inst, #name);
    NV_INSTANCE(NV_LOAD_I)
    uint32_t n = 8;
    VkPhysicalDevice pd[8], phys = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &n, pd);
    for (uint32_t i = 0; i < n && !phys; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = pd[i];
    }
    if (!phys && n) phys = pd[0];
    if (!phys) return false;
    g.phys = phys;
    vkGetPhysicalDeviceMemoryProperties(phys, &g.mem);
    auto make_surface = (PFN_vkCreateWin32SurfaceKHR)gipa(inst, "vkCreateWin32SurfaceKHR");
    VkWin32SurfaceCreateInfoKHR surf = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surf.hinstance = GetModuleHandleA(nullptr);
    surf.hwnd = (HWND)window_handle();
    if (!surf.hwnd || !make_surface || make_surface(inst, &surf, nullptr, &g.surface)) return false;
    uint32_t qn = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qn && family == UINT32_MAX; i++) {
        VkBool32 shows = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, g.surface, &shows);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && shows) family = i;
    }
    if (family == UINT32_MAX) return false;
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
    g.clip_far = has.shaderClipDistance && !getenv("HP2_NOCLIPFAR");
    g.can_aniso = has.samplerAnisotropy != VK_FALSE;
    g.depth_clamp = want.depthClamp;
    vkGetPhysicalDeviceProperties(phys, &g.props);
    di.pEnabledFeatures = &want;
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = device_extensions;
    if (vkCreateDevice(phys, &di, nullptr, &g.dev)) return false;
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
    const bool made = setup_resources();
    g_tracking = false;
    return made;
}

// the picture's size, the multisampling, the filter: what the settings say
static int settings_key() { return g_cfg.scale | g_cfg.aspect << 4 | g_cfg.aa << 8 | g_cfg.aniso << 16; }

static bool setup_resources()
{
    g.key = settings_key();
    const VkPhysicalDeviceProperties &props = g.props;
    const uint32_t family = g.family;
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    g.aniso = !exact && g.can_aniso ? std::min(g_cfg.aniso, (int)props.limits.maxSamplerAnisotropy) : 0;
    g.samples = VK_SAMPLE_COUNT_1_BIT;
    if (!exact && g_cfg.aa > 1 && props.apiVersion >= VK_API_VERSION_1_2) {
        VkSampleCountFlags can = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
        for (int s = g_cfg.aa; s >= 2 && g.samples == VK_SAMPLE_COUNT_1_BIT; s /= 2)
            if (can & (VkSampleCountFlags)s) g.samples = (VkSampleCountFlagBits)s;
    }

    W43 = 512 * g_cfg.scale;
    H = 448 * g_cfg.scale;
    if (const char *e = getenv("HP2_NATIVESIZE")) sscanf(e, "%dx%d", &W43, &H);
    W = (int)(W43 / native_narrow() + 0.5f) & ~1;
    if (!make_buffer(g.fps_buf, &g.fps_map, kFpsBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !make_buffer(g.depth_rb, &g.depth_rb_map, kDepthBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(g.dev, &semaphore, nullptr, &g.acquired) || vkCreateSemaphore(g.dev, &semaphore, nullptr, &g.rendered)) return false;
    if (!make_buffer(g.staging, &g.staging_map, kStaging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !make_buffer_in(g.lights_stage, &g.lights_map, kLightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ||
        !make_buffer(g.fold_vb, &g.fold_map, kFoldBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !make_buffer(g.readback, &g.rb_map, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    if (getenv("HP2_HOSTBUFFERS") || !make_buffer_in(g.lights, nullptr, kLightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        g.lights = g.lights_stage;
    VkImageView cv, dv;
    if (!make_image(g.color, cv, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT) ||
        !make_image(g.depth, dv, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    // the picture so far, down to the GS's half buffer and a level past it; the last frame; the palettes
    g.scene_levels = 2;
    for (int m = W; m > 256; m >>= 1) g.scene_levels++;
    VkImageView scene_view, prev_view, palette_view;
    if (!make_image(g.scene, scene_view, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, W, H, g.scene_levels) ||
        !make_image(g.prev, prev_view, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !make_image(g.palette, palette_view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 256, 8))
        return false;

    // Every render pass waits for all that came before it, and all that comes after waits for it, the layout changes at its start and end
    // included. Without these the passes' own changes of layout raced the copies and the passes around them: NVIDIA's GPUs keep them in
    // order anyway, AMD's do not (blocks of black and old pictures in the menus, 2026-11-06; the validation layer's sync checks name them).
    static const VkAccessFlags kAll = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    static const VkSubpassDependency kDeps[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, kAll, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, kAll, 0}};
    VkAttachmentDescription at[2] = {};
    at[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    at[0].format = VK_FORMAT_B8G8R8A8_UNORM;
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

    // the envmap views: the same formats, read back by the fold
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkCreateRenderPass(g.dev, &rp, nullptr, &g.env_pass)) return false;

    // Multisampling: the picture proper is in samples, and each pass ends by resolving it, colour and depth, into the images everything else reads
    if (g.samples != VK_SAMPLE_COUNT_1_BIT) {
        real_vkCreateRenderPass2 = (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(g.dev, "vkCreateRenderPass2");
        auto create2 = track_vkCreateRenderPass2;
        VkImageView msc, msd;
        if (!real_vkCreateRenderPass2 || !make_image(g.color_ms, msc, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, g.samples) ||
            !make_image(g.depth_ms, msd, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1, g.samples))
            return false;
        VkAttachmentDescription2 a2[4] = {};
        for (int i = 0; i < 4; i++) {
            a2[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            a2[i].format = i & 1 ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_B8G8R8A8_UNORM;
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
    }
    VkImageView ed;
    if (!make_image(g.env_depth, ed, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, ENV_SIZE, ENV_SIZE)) return false;
    VkImageView env_view[6];
    for (int i = 0; i < 6; i++) {
        if (!make_image(g.env[i], env_view[i], VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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
    if (!make_image(g.rvm, rv, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, g.rvm_w, g.rvm_h) ||
        !make_image(g.rvm_depth, rd, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, g.rvm_w, g.rvm_h))
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
    // the sphere map: kept from frame to frame
    VkImageView sv;
    if (!make_buffer(g.sphere_rb, &g.sphere_rb_map, SPHERE_SIZE * SPHERE_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    if (!make_image(g.sphere, sv, VK_FORMAT_B8G8R8A8_UNORM,
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
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ll = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ll.bindingCount = 1;
    ll.pBindings = &lb;
    if (vkCreateDescriptorSetLayout(g.dev, &ll, nullptr, &g.light_layout)) return false;
    VkDescriptorSetLayoutBinding pb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo psl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    psl.bindingCount = 3;
    psl.pBindings = pb;
    if (vkCreateDescriptorSetLayout(g.dev, &psl, nullptr, &g.post_set_layout)) return false;
    VkDescriptorPoolSize dps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4100}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 4100;
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
    VkDescriptorBufferInfo lbi = {g.lights, 0, kLightBytes};
    VkWriteDescriptorSet lw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    lw.dstSet = g.light_set;
    lw.descriptorCount = 1;
    lw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    lw.pBufferInfo = &lbi;
    vkUpdateDescriptorSets(g.dev, 1, &lw, 0, nullptr);
    la.pSetLayouts = &g.set_layout;
    for (int i = 0; i < 6; i++) {
        if (vkAllocateDescriptorSets(g.dev, &la, &g.env_set[i])) return false;
        bind_view(g.env_set[i], env_view[i], g.sampler[4 | 8]);
    }
    if (vkAllocateDescriptorSets(g.dev, &la, &g.sphere_set)) return false;
    bind_view(g.sphere_set, sv, g.sampler[4 | 8]);
    VkImageView spv;
    if (!make_image(g.sphere_prev, spv, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, SPHERE_SIZE, SPHERE_SIZE))
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
    la.pSetLayouts = &g.set_layout;

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
    VkBool32 spec_alpha_test = VK_TRUE;
    VkSpecializationMapEntry spec_entry = {0, 0, sizeof(VkBool32)};
    VkSpecializationInfo spec = {1, &spec_entry, sizeof(VkBool32), &spec_alpha_test};
    VkVertexInputBindingDescription bind = {0, sizeof(NativeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                 {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                                                 {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},
                                                 {3, 0, VK_FORMAT_R8G8B8A8_SNORM, 24},
                                                 {4, 0, VK_FORMAT_R32_SFLOAT, 28}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 5;
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
    vin.pVertexBindingDescriptions = &fbind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = fattr;
    ds.depthTestEnable = ds.depthWriteEnable = VK_FALSE;
    ba.blendEnable = VK_FALSE;              // the shader does the GS's blend
    pi.layout = g.fold_layout;
    pi.renderPass = g.fold_pass;
    pi.pDepthStencilState = nullptr;
    if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.fold_pipeline)) return false;
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
        VkPushConstantRange gpc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 3};
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
    return true;
}

// The settings changed: all of it is taken apart and made again. The textures go too, native_texture decodes them again as they are used.
// The device and the swapchain stay, so the window goes on.
static bool rebuild()
{
    vkDeviceWaitIdle(g.dev);
    for (VkTex *t : g.textures) free_texture(t);
    g.textures.clear();
    for (auto it = g_undo.rbegin(); it != g_undo.rend(); ++it) (*it)();
    g_undo.clear();
    g.color_ms = g.depth_ms = VK_NULL_HANDLE;
    g.rvm_ready = g.prev_ready = g.sphere_ready = g.scene_ready = false;
    for (int i = 0; i < 6; i++) g.env_ready[i] = g.env_drawn_now[i] = false;
    g.waiting = false;
    g.depth_rects.clear();
    g.depth_places.clear();
    g_tracking = true;
    const bool made = setup_resources();
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

static void copy_scene();

// Brightness, contrast and gamma from the settings on the finished picture, in the frame's own part of it: the bars beside stay black.
// At their defaults nothing runs. After the copy for the next frame's DisplayBlendPrevious, so it does not build up from frame to frame.
static void grade_picture(const NativeFrame &frame)
{
    static const bool exact = getenv("HP2_NATIVEEXACT") != nullptr;
    if (exact || (g_cfg.brightness == 0 && g_cfg.contrast == 100 && g_cfg.gamma == 100)) return;
    copy_scene();
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
    const float push[3] = {g_cfg.brightness / 100.0f, g_cfg.contrast / 100.0f, g_cfg.gamma / 100.0f};
    vkCmdPushConstants(g.cmd, g.grade_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
}

// HP2_PERFAB: every other frame the old way (variant 1), so that a change is measured on the same scenes; the log line gives both
static const bool g_ab_on = getenv("HP2_PERFAB") != nullptr;
static int g_ab;                            // this frame's variant
static double g_ab_scene[2], g_ab_total[2];
static int g_ab_n[2];

// HP2_DRAWPROF: once every 600 frames, a timestamp every 16 draws of the scene; the slowest stretches go to the log with what was in them
static const bool g_dp_on = getenv("HP2_DRAWPROF") != nullptr;
static VkQueryPool g_dp_pool;
static bool g_dp_now, g_dp_pending;
struct DpSeg { uint32_t draw, draws, vertices, sphere, road, alpha, blended, big; };
static DpSeg g_dp_seg[512];
static uint32_t g_dp_n;

static void dp_mark(uint32_t draw)
{
    if (!g_dp_now || g_dp_n >= 512) return;
    vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_dp_pool, g_dp_n);
    g_dp_seg[g_dp_n] = {draw, 0, 0, 0, 0, 0, 0, 0};
    g_dp_n++;
}

static void dp_read()
{
    if (!g_dp_pending) return;
    g_dp_pending = false;
    if (g_dp_n < 3) return;
    static uint64_t t[512];
    if (vkGetQueryPoolResults(g.dev, g_dp_pool, 0, g_dp_n, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    const double ns = g.props.limits.timestampPeriod;
    std::vector<std::pair<double, uint32_t>> d;
    double total = 0;
    for (uint32_t i = 0; i + 1 < g_dp_n; i++) {
        double ms = t[i + 1] > t[i] ? (double)(t[i + 1] - t[i]) * ns / 1e6 : 0;
        total += ms;
        d.push_back({ms, i});
    }
    std::sort(d.begin(), d.end(), [](auto &a, auto &b) { return a.first > b.first; });
    fprintf(g_hle_log, "native: drawprof %.3f ms over %u stretches\n", total, g_dp_n - 1);
    for (size_t k = 0; k < d.size() && k < 12; k++) {
        const DpSeg &s = g_dp_seg[d[k].second];
        fprintf(g_hle_log, "native: drawprof %.3f ms from draw %u: %u draws, %u vertices, sphere %u, road %u, alpha-test %u, blended %u, over 3000 vertices %u\n",
                d[k].first, s.draw, s.draws, s.vertices, s.sphere, s.road, s.alpha, s.blended, s.big);
    }
    // the spread: how many stretches it takes for half of the time
    double half = 0;
    size_t k = 0;
    while (k < d.size() && half < total / 2) half += d[k++].first;
    fprintf(g_hle_log, "native: drawprof half the time in %zu of %zu stretches\n", k, d.size());
}

// every strip of a target from [first, last) of the draws, up to three passes each
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
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(g.cmd, 0, 1, &g.vb, &zero);
    vkCmdBindIndexBuffer(g.cmd, g.vb, g.draw_idx_off, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 1, 1, &g.light_set, 0, nullptr);
    VkTex *road = frame.road_specular ? (VkTex *)frame.road_specular->vk : nullptr;
    VkTex *env_tex = frame.env_override ? (VkTex *)frame.env_override->vk : nullptr;
    const VkDescriptorSet env_set = env_tex ? env_tex->set : g.sphere_set;
    auto tex_mode = [](const NativeTexture &t) {
        return (t.repeat_s ? 1 : 0) | (t.repeat_t ? 2 : 0) | (t.mag_linear ? 4 : 0) | (t.min_linear ? 8 : 0) | (t.mip_linear ? 16 : 0) | (t.levels - 1) << 8;
    };
    // what is bound already: the passes of a strip go between a few pipelines and sets, and binding again what is there costs the GPU's front end
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    auto pass = [&](const NativeDraw &d, int which, VkDescriptorSet set, VkPipeline pipeline, float factor, bool textured) {
        Push p;
        memcpy(p.clip, d.clip, sizeof p.clip);
        p.scroll[0] = which == 2 ? 0 : d.scroll_s;
        p.scroll[1] = which == 2 ? 0 : d.scroll_t;
        p.alpha_test = d.alpha_test ? 1.0f : 0.0f;
        p.blend_alpha = factor;
        p.textured = textured ? 1.0f : 0.0f;
        p.light = d.light;
        p.flags = d.flags;
        p.pass = which;
        p.tfx = which == 0 ? d.tfx : 0;
        p.lod_l = which == 0 ? (float)d.lod_l : 0.0f;
        p.bits16 = target >= 8 && target <= 13;
        p.snap[0] = d.snap_x;
        p.snap[1] = d.snap_y;
        // the pictures of this renderer: clamped and linear, as TEX1's 0xe0 has it at level 0
        p.tex_mode = which == 2 && env_tex ? tex_mode(*frame.env_override) | 64
                   : which == 2 || d.rt >= 0 || !d.tex ? 4 | 8 : which == 1 ? tex_mode(*frame.road_specular) : tex_mode(*d.tex);
        // what VU1 transforms, the world; the setting's filter leaves the 2D and the envmap views to the GS's sampling
        if (which == 0 && d.tex && d.rt < 0 && (d.tex->upscaled || (g.aniso && d.tex->min_linear && d.snap_x != 0 && target < 8))) p.tex_mode |= 32;
        if (pipeline != bound_pipeline) vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (set != bound_set) vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
        bound_pipeline = pipeline;
        bound_set = set;
        vkCmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        if (d.indexed) vkCmdDrawIndexed(g.cmd, d.count, 1, d.first, 0, 0);
        else vkCmdDraw(g.cmd, d.count, 1, g.draw_dyn_first + d.first, 0);
    };
    if (last > frame.draws.size()) last = frame.draws.size();
    float last_view[4] = {-1, -1, -1, -1};
    for (size_t i = first; i < last; i++) {
        const NativeDraw &d = frame.draws[i];
        // the players' views and the 2D layer share a picture and go in the game's order
        bool mine = target == 1 ? d.target <= 4 || d.target == 6 : d.target == target;
        if (!d.count || !mine) continue;
        if (g_dp_now && target == 1) {
            static uint32_t since;
            if (g_dp_n == 0 || ++since >= 16) {
                since = 0;
                dp_mark((uint32_t)i);
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
        VkPipeline dest_pipeline = pipes[NB_DEST][0][d.depth_test];
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
        if (has_sphere) {
            if (gloss) {
                pass(d, 2, env_set, dest_pipeline, -3.0f, true);
                pass(d, 2, env_set, dest_pipeline, -3.0f, true);
            } else {
                pass(d, 2, env_set, pipes[NB_ADD][0][d.depth_test], -1.0f, true);
            }
            if (target == 7) pass(d, 2, env_set, alpha_pipes[d.depth_test], -2.0f, true);
        }
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
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        draw_target(frame, 7, g.rvm_w, g.rvm_h);
        vkCmdEndRenderPass(g.cmd);
        g.rvm_ready = true;
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
            // XYZ2 is the whole pixel, trunc(s * 256 - 128 + 2048) on a target at 1920
            out[n++] = {std::floor(q[3] * SPHERE_SIZE) / SPHERE_SIZE, std::floor(q[4] * SPHERE_SIZE) / SPHERE_SIZE, q[5], q[6]};
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

static void shot_this_frame()
{
    static const char *shot = getenv("HP2_NATIVESHOT");
    static const int every = getenv("HP2_NATIVESHOTN") ? atoi(getenv("HP2_NATIVESHOTN")) : 150;
    static int frames;
    if (!shot || ++frames % every) return;
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    char path[512];
    snprintf(path, sizeof path, shot, frames / every);
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
static void make_swapchain(int w, int h)
{
    vkDeviceWaitIdle(g.dev);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    uint32_t n = 64;
    VkSurfaceFormatKHR formats[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, formats);
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < n; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) format = formats[i];
    uint32_t m = 8;
    VkPresentModeKHR modes[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &m, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   // always there; the game keeps its own time, so a mailbox is better
    for (uint32_t i = 0; i < m; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = caps.currentExtent;
    if (ci.imageExtent.width == UINT32_MAX) ci.imageExtent = {(uint32_t)w, (uint32_t)h};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -(int)caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.swapchain;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(g.dev, &ci, nullptr, &made);
    if (g.swapchain) vkDestroySwapchainKHR(g.dev, g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
    g.swap_images.clear();
    if (r) {
        fprintf(g_hle_log, "native: no swapchain (%d)\n", (int)r);
        return;
    }
    g.swapchain = made;
    g.swap_w = (int)ci.imageExtent.width;
    g.swap_h = (int)ci.imageExtent.height;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g.dev, made, &count, nullptr);
    g.swap_images.resize(count);
    vkGetSwapchainImagesKHR(g.dev, made, &count, g.swap_images.data());
    g.swap_stale = false;
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
    if ((float)bw > (float)bh * aspect) bw = (int)((float)bh * aspect);
    else bh = (int)((float)bw / aspect);
    int bx = (g.swap_w - bw) / 2, by = (g.swap_h - bh) / 2;
    int from = frame.wide ? 0 : (W - W43) / 2, across = frame.wide ? W : W43;
    VkImageBlit bl = {};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {from, 0, 0};
    bl.srcOffsets[1] = {from + across, H * 444 / 448, 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[0] = {bx, by, 0};
    bl.dstOffsets[1] = {bx + bw, by + bh, 1};
    vkCmdBlitImage(g.cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   g_cfg.smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
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
        if (place + bytes > kDepthBytes) break;
        VkBufferImageCopy c = {};
        c.bufferOffset = place;
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.imageOffset = {x0, y0, 0};
        c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyImageToBuffer(g.cmd, g.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.depth_rb, 1, &c);
        g.depth_places.push_back(place);
        place += bytes;
    }
    depth_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

bool native_vk_depth(const NativeDepthRect &r, uint32_t *out)
{
    if (!g.ready) return false;
    for (size_t i = 0; i < g.depth_rects.size() && i < g.depth_places.size(); i++) {
        const NativeDepthRect &have = g.depth_rects[i];
        if (have.x != r.x || have.y != r.y || have.w != r.w || have.h != r.h) continue;
        vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
        float ox = g.depth_ox, sx = g.depth_sx, sy = H / 448.0f;
        int x0 = std::clamp((int)(ox + r.x * sx), 0, W - 1), y0 = std::clamp((int)(r.y * sy), 0, H - 1);
        int w = std::clamp((int)std::ceil(r.w * sx), 1, W - x0), h = std::clamp((int)std::ceil(r.h * sy), 1, H - y0);
        const float *depth = (const float *)((const char *)g.depth_rb_map + g.depth_places[i]);
        for (int y = 0; y < r.h; y++)
            for (int x = 0; x < r.w; x++) {
                int px = std::clamp((int)(ox + (r.x + x + 0.5f) * sx) - x0, 0, w - 1), py = std::clamp((int)((r.y + y + 0.5f) * sy) - y0, 0, h - 1);
                out[(size_t)y * r.w + x] = (uint32_t)(depth[(size_t)py * w + px] * 16777216.0f);
            }
        return true;
    }
    return false;
}

// the picture so far into the scene image, with its levels down
static void copy_scene()
{
    all_barrier();
    image_barrier(g.scene, 0, g.scene_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit bl = {};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {W, H, 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[1] = {W, H, 1};
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

// the render pass of the picture is over: lay this on it
static void draw_fx(const NativeFrame &frame, const NativeFx &f, int row)
{
    if (f.kind != FX_FOG) {
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
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.post_pass;
    rb.framebuffer = g.post_fb;
    rb.renderArea.extent = {(uint32_t)W, (uint32_t)H};
    vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    float ox, sx, sy;
    active_region(frame, ox, sx, sy);
    VkViewport vp = {ox + f.rect[0] * sx, f.rect[1] * sy, f.rect[2] * sx, f.rect[3] * sy, 0, 1};
    VkRect2D sc = {{(int32_t)vp.x, (int32_t)vp.y}, {(uint32_t)vp.width, (uint32_t)vp.height}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    PostPush p = {};
    p.mode = f.kind == FX_FOG ? 0 : f.kind == FX_GREY ? 1 : 2;
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
    if (frame.fx.empty()) return;
    image_barrier(g.palette, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (size_t i = 0; i < frame.fx.size() && i < 8; i++) {
        if (at + sizeof frame.fx[i].palette > kStaging) break;
        memcpy((char *)g.staging_map + at, frame.fx[i].palette, sizeof frame.fx[i].palette);
        VkBufferImageCopy c = {};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, (int32_t)i, 0};
        c.imageExtent = {256, 1, 1};
        vkCmdCopyBufferToImage(g.cmd, g.staging, g.palette, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        at += sizeof frame.fx[i].palette;
    }
    image_barrier(g.palette, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// where the CPU's frame goes, for HP2_NATIVELOG's "cpu" line: the game between two frames, the wait for the GPU, the copy of the vertices,
// the recording, the submit and present
static double g_cpu_sum[5];
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

bool native_vk_draw(const NativeFrame &frame)
{
    if (g.failed) return false;
    const double game_ms = cpu_lap();
    if (!g.ready) {
        if (!window_handle()) return false;     // not there yet
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
    // the settings changed: this frame's textures are gone with the rest, so it is not drawn
    if (g.key != settings_key()) {
        if (!rebuild()) {
            g.failed = true;
            fprintf(g_hle_log, "native: the renderer could not be made again\n");
            return false;
        }
        return true;
    }
    device_vertices();
    const size_t bytes = frame.vertices.size() * sizeof(NativeVertex), fresh_bytes = frame.fresh.size() * sizeof(NativeVertex),
                 index_bytes = frame.indices.size() * sizeof(uint32_t);
    {
        // not drawn: the window keeps the last picture while the game goes on (and native.cpp empties its cache)
        const char *why = frame.dyn_base != native_vk_cache_vertices() ? nullptr
                          : g.vb_in_vram ? (bytes > kSide || index_bytes > kSide ? "more than the GPU's buffer takes" : "")
                          : bytes + index_bytes > kMaxBytes ? "more than a frame can take" : "";
        if (!why) return false;             // made for a cache of another size: the first frame
        if (*why) {
            static int told;
            if (told++ < 5) fprintf(g_hle_log, "native: %zu vertices, %zu indices are %s; not drawn\n", frame.vertices.size(), frame.indices.size(), why);
            return false;
        }
    }
    cpu_lap();
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    const double wait_ms = cpu_lap();
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
    frame.uploaded = true;
    g.vb = g.vb_in_vram ? g.vb_dev : g.vb_stage;
    g.draw_idx_off = g.vb_in_vram ? g.idx_off : bytes + fresh_bytes;
    g.draw_dyn_first = g.vb_in_vram ? g.cache_verts : 0;
    size_t light_bytes = frame.lights.size() * sizeof(NativeLight);
    if (light_bytes > kLightBytes) light_bytes = 0;
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
            if (index_bytes) regions.push_back({bytes + fresh_bytes, g.idx_off, index_bytes});
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
    g_ab = g_ab_on ? (int)(g.frame & 1) : 0;
    g_dp_now = false;
    if (g_dp_on && g.frame % 600 == 0) {
        if (!g_dp_pool) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 512;
            vkCreateQueryPool(g.dev, &qi, nullptr, &g_dp_pool);
        }
        if (g_dp_pool) {
            vkCmdResetQueryPool(g.cmd, g_dp_pool, 0, 512);
            g_dp_now = true;
            g_dp_pending = true;
            g_dp_n = 0;
        }
    }
    VkDeviceSize staged = 0;
    for (const NativeDraw &d : frame.draws)
        if (d.tex && d.count) {
            if (d.tex->dirty) upload_texture(*d.tex, staged);
            if (d.tex->vk) ((VkTex *)d.tex->vk)->used = g.frame;
        }
    // what has not been drawn for a quarter of a minute goes, and is decoded again if it comes back. A few a frame: freed all at once every
    // 300 frames, they cost that frame 5 to 16 ms (a vkFreeMemory each), a stutter
    {
        static size_t cursor;
        int freed = 0;
        for (int looked = 0; looked < 64 && !g.textures.empty() && freed < 2; looked++) {
            if (cursor >= g.textures.size()) cursor = 0;
            if (g.frame - g.textures[cursor]->used > 900) {
                free_texture(g.textures[cursor]);
                g.textures[cursor] = g.textures.back();
                g.textures.pop_back();
                freed++;
            } else {
                cursor++;
            }
        }
    }
    if (frame.road_specular && frame.road_specular->dirty) upload_texture(*frame.road_specular, staged);
    if (frame.road_specular && frame.road_specular->vk) ((VkTex *)frame.road_specular->vk)->used = g.frame;
    if (frame.env_override && frame.env_override->dirty) upload_texture(*frame.env_override, staged);
    if (frame.env_override && frame.env_override->vk) ((VkTex *)frame.env_override->vk)->used = g.frame;
    upload_palettes(frame, staged);
    VkDeviceSize zero_vb = 0;
    draw_env_views(frame, zero_vb);
    all_barrier();
    fold_sphere_map(frame);
    all_barrier();
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
    // the view, cut at what is laid over it
    size_t at = 0;
    for (size_t i = 0; i < frame.fx.size() && i < 8; i++) {
        draw_target(frame, 1, W, H, at, frame.fx[i].at);
        at = frame.fx[i].at;
        vkCmdEndRenderPass(g.cmd);
        draw_fx(frame, frame.fx[i], (int)i);
        if (g.samples != VK_SAMPLE_COUNT_1_BIT) copy_scene();
        rb.renderPass = g.pass_load;
        vkCmdBeginRenderPass(g.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        if (g.samples != VK_SAMPLE_COUNT_1_BIT) {
            // the effect went on the resolved picture: back into every sample. The edges drawn so far stay as they were resolved
            VkViewport whole = {0, 0, (float)W, (float)H, 0, 1};
            VkRect2D all = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
            vkCmdSetViewport(g.cmd, 0, 1, &whole);
            vkCmdSetScissor(g.cmd, 0, 1, &all);
            vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.restore_pipeline);
            vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.restore_layout, 0, 1, &g.restore_set, 0, nullptr);
            vkCmdDraw(g.cmd, 3, 1, 0, 0);
        }
    }
    draw_target(frame, 1, W, H, at);
    dp_mark((uint32_t)frame.draws.size());
    vkCmdEndRenderPass(g.cmd);
    all_barrier();
    if (compare_now) copy_color_to(round == 0 ? 1 : 0);
    }
    if (compare_now) g_cmp_pending = true;
    gpu_mark(2);
    g_fx_count += frame.fx.size() < 8 ? frame.fx.size() : 8;
    // what the next frame's DisplayBlendPrevious reads, if this one asked for it
    bool wants_prev = false;
    for (const NativeDraw &d : frame.draws)
        if (d.rt == NATIVE_RT_PREVIOUS) wants_prev = true;
    g.prev_ready = wants_prev;
    if (wants_prev) {
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
    copy_depth_rects(frame);
    grade_picture(frame);
    gpu_mark(3);
    static const bool shots = getenv("HP2_NATIVESHOT") != nullptr;
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
    if (cw > 0 && ch > 0) {
        if (!g.swapchain || g.swap_stale || cw != g.swap_w || ch != g.swap_h) {
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
            else if (r == VK_ERROR_OUT_OF_DATE_KHR) g.swap_stale = true;
        }
    }
    if (on_screen) blit_to_window(g.swap_images[image], frame);
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
    vkQueueSubmit(g.queue, 1, &si, g.fence);
    if (on_screen) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &present_wait;
        pi.swapchainCount = 1;
        pi.pSwapchains = &g.swapchain;
        pi.pImageIndices = &image;
        VkResult r = vkQueuePresentKHR(g.queue, &pi);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) g.presented++;
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) g.swap_stale = true;
    }
    g.waiting = true;
    const double submit_ms = cpu_lap();
    // a long frame, end to end: where it went (HP2_NATIVELOG)
    {
        static const bool log = getenv("HP2_NATIVELOG") != nullptr || getenv("HP2_LONGFRAMES") != nullptr;
        const double total = game_ms + wait_ms + copy_ms + record_ms + submit_ms;
        static int told;
        if (log && total > 21.0 && g.frame > 300 && told < 2000) {
            told++;
            fprintf(g_hle_log, "native: long frame %u: %.1f ms = game %.1f (lists %.1f, textures %.1f for %u), wait for the gpu %.1f, copy %.1f, recording %.1f, "
                               "submit and present %.1f; %.2f MB new vertices, %.2f MB of textures, %zu vertices\n",
                    g.frame, total, game_ms, frame.list_ms, frame.tex_ms, frame.tex_decoded, wait_ms, copy_ms, record_ms, submit_ms,
                    fresh_bytes / 1048576.0, staged / 1048576.0, frame.indices.size() + frame.vertices.size());
        }
    }
    g_cpu_sum[0] += game_ms;
    g_cpu_sum[1] += wait_ms;
    g_cpu_sum[2] += copy_ms;
    g_cpu_sum[3] += record_ms;
    g_cpu_sum[4] += submit_ms;
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
                        g_gpu_sum[3] / g_gpu_frames, (double)g_fx_count / g_gpu_frames, frame.indices.size() + frame.vertices.size(), frame.draws.size());
            fprintf(g_hle_log, "\n");
            if (g_cpu_n)
                fprintf(g_hle_log, "native: cpu a frame: game %.2f ms, wait for the gpu %.2f, vertices %.2f, recording %.2f, submit and present %.2f\n",
                        g_cpu_sum[0] / g_cpu_n, g_cpu_sum[1] / g_cpu_n, g_cpu_sum[2] / g_cpu_n, g_cpu_sum[3] / g_cpu_n, g_cpu_sum[4] / g_cpu_n);
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
