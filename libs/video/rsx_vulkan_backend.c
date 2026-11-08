/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   disk caches            translated SPIR-V and the VkPipelineCache, kept
 *                          between runs (PS3RECOMP_VK_SHADER_CACHE)
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4), colour snapshots and the GPU light, SSAO
 *                          and depth-decode passes (E5, GLSL in
 *                          vulkan/rsx_vk_passes_glsl.h) -- the stage names
 *                          in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Deliberately synchronous: every clear, draw and present is one one-shot
 * command buffer, submitted and waited on. Slow and trivially correct; it is
 * what lets the engine's submit_and_wait and staging reuse cost nothing, and
 * batching is the first thing to change once correctness is settled.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

#if !defined(_WIN32) || defined(PS3RECOMP_RSX_VULKAN)   /* Windows: opt-in build */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"
#include "vulkan/rsx_vk_passes_glsl.h"
#include "vk_postfx.h"                           /* [postfx] AA, sharpening, anisotropy */
#include "../../include/ps3emu/ps3_ftrace.h"    /* PS3_FTRACE: GPU_WAIT spans at each fence wait */
#include "../../include/ps3emu/ps3_settings.h"  /* [overlay] PC settings the in-game menu changes live */
#include "vk_overlay.h"                          /* [overlay] HUD + settings menu (vk_overlay.cpp) */

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The disk caches: on Windows CreateDirectoryA, GetFileAttributesA and
 * MoveFileExA, from the <windows.h> ps3_ftrace.h already includes. */
#ifndef _WIN32
#include <sys/stat.h>       /* mkdir, stat */
#include <unistd.h>         /* getpid */
#include <pthread.h>        /* the background pipeline-cache save */
#include <sched.h>
#endif

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCmdSetDepthBias) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage) X(vkCmdCopyImage) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkGetQueryPoolResults) X(vkGetFenceStatus)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* GPU timestamp profiler (PS3RECOMP_VK_GPU_PROF): what each interval recorded. The
 * call-like macros count; the loader's assignments (no parentheses) are untouched,
 * and a macro does not expand inside itself, so the real pointer is called. */
static u32 s_gp_ops[8];
#define pvkCmdCopyImage(...)              (s_gp_ops[0]++, pvkCmdCopyImage(__VA_ARGS__))
#define pvkCmdCopyBufferToImage(...)      (s_gp_ops[1]++, pvkCmdCopyBufferToImage(__VA_ARGS__))
#define pvkCmdCopyImageToBuffer(...)      (s_gp_ops[2]++, pvkCmdCopyImageToBuffer(__VA_ARGS__))
#define pvkCmdClearColorImage(...)        (s_gp_ops[3]++, pvkCmdClearColorImage(__VA_ARGS__))
#define pvkCmdClearDepthStencilImage(...) (s_gp_ops[4]++, pvkCmdClearDepthStencilImage(__VA_ARGS__))
#define pvkCmdBlitImage(...)              (s_gp_ops[5]++, pvkCmdBlitImage(__VA_ARGS__))
#define pvkCmdPipelineBarrier(...)        (s_gp_ops[6]++, pvkCmdPipelineBarrier(__VA_ARGS__))
#define pvkCmdDrawIndexed(...)            (s_gp_ops[7]++, pvkCmdDrawIndexed(__VA_ARGS__))
#define pvkCmdDraw(...)                   (s_gp_ops[7]++, pvkCmdDraw(__VA_ARGS__))

/* PS3RECOMP_VK_THREADED: vkCmd* recording on a second thread (vk_threaded_rec.h) */
#include "vk_threaded_rec.h"

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    /* how vk_create_image_levels made it: the recycle pool's key (w == 0: an
     * image made elsewhere, never pooled) */
    VkFormat           fmt;
    u32                w, h, levels;
    VkImageUsageFlags  usage;
    VkImageAspectFlags aspect;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              windowed;
    SDL_Window*      window;
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
} vk_state;

static vk_state s_vk;
static int s_vk_indep_blend;   /* device has independentBlend (per-attachment state) */
static int s_vk_depth_clamp_ok; /* device has depthClamp (ZMIN_MAX_CONTROL clamp, not clip) */
/* DEPTH, or DEPTH|STENCIL when the depth format carries the RSX's 8 stencil
 * bits (D32_SFLOAT_S8_UINT): barriers, clears and attachment views of a depth
 * image name every aspect it has; copies out of it name DEPTH alone. */
static VkImageAspectFlags s_vk_ds_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
#define VK_DS_ASPECT s_vk_ds_aspect

/* ---- batched recording (PS3RECOMP_VK_BATCH, default on) --------------------
 * The backend was written with every operation submitted and waited on, so a
 * draw cost a full CPU -> GPU -> CPU round trip: the inFAMOUS 2 title spent
 * ~265 ms of every ~340 ms frame inside RSX drains, most of it blocked on
 * fences, at 2.8 fps. Batched, the engine's draws and clears are recorded into
 * the one open command buffer and the batch is submitted only where something
 * needs it done: a present, a readback or snapshot (anything that already
 * calls vk_submit_and_wait), the engine's own submit_and_wait hook, a resource
 * about to be destroyed, or an arena running out. What a recorded draw reads
 * from host memory -- vertices, indices, the constant file -- is copied into
 * per-batch linear arenas, and each draw gets its own descriptor set from a
 * pool reset at every flush, so nothing a pending draw uses is overwritten.
 * PS3RECOMP_VK_BATCH=0 restores the synchronous path (A/B). */
#define VK_BATCH_SETS 16384u   /* per-draw sets a frame can hold before a mid-frame flush: the Empire City wide shots draw 5-8k (4096 cost a submit-and-wait each such frame) */
#define VK_BATCH_DEAD_FB 256u
static struct {
    int          on;                     /* -1 unread, 0 off, 1 on            */
    int          recording;              /* s_vk.cmd holds batched work       */
    u32          draws;                  /* engine draws in the open batch    */
    vk_buffer    varena, iarena, uarena; /* vertex / index / uniform arenas   */
    vk_buffer    sarena;                 /* transfer staging (uploads, depth copies) */
    vk_buffer    garena;                 /* GPU-local bounce for image -> buffer -> image copies */
    VkDeviceSize vcur, icur, ucur, ualign, scur, gcur;
    VkFramebuffer dead_fb[VK_BATCH_DEAD_FB]; /* destroyed once the batch is done */
    u32          n_dead_fb;
    vk_image     dead_img[256];          /* likewise images (and their views) */
    u32          n_dead_img;
    u32*         dead_vb;                /* likewise persistent vertex/index buffers (handles) */
    u32          n_dead_vb, cap_dead_vb;
    int          zrgba_pending;          /* depth-as-RGBA8 passes recorded (their set ring's next slot) */
    int          light_pending;          /* the GPU light pass is recorded     */
    int          ssao_pending;           /* the GPU SSAO passes are recorded   */
    /* The engine render pass left open after a draw, so the next draw into the
     * same framebuffer continues it instead of paying a pass and barriers. */
    int          rp_open;
    VkFramebuffer rp_fb;
    VkRenderPass rp_rp;
    u32          rp_w, rp_h;             /* its render area (GPU timestamp labels) */
    u32          rp_rt[RSX_BE_MAX_COLOR_TARGETS], rp_nrt, rp_depth;
    unsigned long long merged;           /* draws that continued an open pass */
    VkDescriptorPool dpool;              /* per-draw descriptor sets          */
    u32          dsets;
    u8*          shadow;                 /* CPU copy of the constant file     */
    size_t       shadow_size;
    unsigned long long flushes, flushed_draws, max_draws;
} s_batch = { .on = -1 };

/* ---- frames in flight (PS3RECOMP_VK_FIF, default 2; 1 = the old behaviour) ---------
 * The windowed present used to submit the frame's batch and WAIT for it: the RSX
 * thread sat idle while the GPU drew, and the GPU idle while the next frame was
 * recorded -- a gameplay frame was ~8 ms of recording plus ~10 ms of waiting, in
 * series (inFAMOUS 2, 2026-11-07: the main thread then waited on the RSX thread).
 * With two slots the present submits slot A and returns; slot B (whose previous
 * frame is waited for, normally long done) records the next frame while the GPU
 * draws A. Everything the host writes for a batch, or that a recorded command
 * refers to by handle and the batch may free, is per slot: the command buffer and
 * fence, the swapchain acquire semaphore, the vertex/index/uniform/staging/bounce
 * arenas, the per-draw descriptor pool, the dead-object lists, the passes' UBOs and
 * descriptor sets (vk_lp_swap), the GPU-timestamp pool. The active slot's live in
 * s_vk/s_batch/s_lp as before -- no other code changed -- and the other slot's are
 * parked in s_fif_bs / s_lp_park. GPU-side order between frames is the queue's:
 * every pass starts with a barrier whose first scope is everything submitted before.
 * An object freed while the other slot may still use it joins the ACTIVE slot's
 * dead list, freed only after that slot's next submission completes -- which, the
 * queue being in order, is after the other slot's. */
#define VK_FIF_MAX 2u
static int             s_fif = -1;             /* -1 unread, else 1 or 2 */
static u32             s_fif_cur;              /* the active slot */
static VkCommandBuffer s_fif_cmd[VK_FIF_MAX];
static VkFence         s_fif_fence[VK_FIF_MAX];
static VkSemaphore     s_fif_acq[VK_FIF_MAX];
static int             s_fif_pending[VK_FIF_MAX];  /* a submission on the fence nobody waited for */
static struct {
    vk_buffer        varena, iarena, uarena, sarena, garena;
    VkDescriptorPool dpool;
    u32              dsets;
    VkFramebuffer    dead_fb[VK_BATCH_DEAD_FB];
    u32              n_dead_fb;
    vk_image         dead_img[256];
    u32              n_dead_img;
    u32*             dead_vb;
    u32              n_dead_vb, cap_dead_vb;
} s_fif_bs[VK_FIF_MAX];                         /* a parked slot's batch resources */
static int vk_fif_on(void)
{
    if (s_fif < 0) {
        const char* e = getenv("PS3RECOMP_VK_FIF");
        s_fif = (e && e[0] == '1') ? 1 : 2;
        fprintf(stderr, "[RSX vulkan] frames in flight: %d (PS3RECOMP_VK_FIF)\n", s_fif);
    }
    return s_fif > 1;
}
/* the other slot's frame may still be on the GPU */
static int vk_fif_busy(void)
{
    if (s_fif <= 1) return 0;
    const u32 o = s_fif_cur ^ 1u;
    return s_fif_pending[o] && pvkGetFenceStatus(s_vk.device, s_fif_fence[o]) != VK_SUCCESS;
}
/* wait for the other slot's frame (its cleanup stays for its turn) */
static void vk_fif_wait_other(void)
{
    if (s_fif <= 1) return;
    const u32 o = s_fif_cur ^ 1u;
    if (s_fif_pending[o]) pvkWaitForFences(s_vk.device, 1, &s_fif_fence[o], VK_TRUE, UINT64_MAX);
}

static int vk_batch_on(void)
{
    if (s_batch.on < 0) {
        const char* e = getenv("PS3RECOMP_VK_BATCH");
        s_batch.on = (e && e[0] == '0') ? 0 : 1;
        fprintf(stderr, "[RSX vulkan] draw batching %s (PS3RECOMP_VK_BATCH)\n",
                s_batch.on ? "on" : "off -- every operation waited on");
    }
    return s_batch.on;
}
static void vk_batch_flush_at(int line);
#define vk_batch_flush() vk_batch_flush_at(__LINE__)

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
#ifdef _WIN32
    static const char* names[] = { "vulkan-1.dll" };   /* the system Vulkan loader */
#else
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    vk_tq_install();                         /* PS3RECOMP_VK_THREADED=1 */
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

/* Recycled images. Every frame inFAMOUS 2 hands the backend a depth snapshot
 * (a new 1280x720 R32F texture per call) and new targets for its depth
 * decodes, and releases last frame's: three image + memory + view creations
 * and destructions a frame, each a kernel allocation (~12% of the RSX thread
 * in the Empire City shots). A destroyed image whose users are complete waits
 * here, and a request with the same format, size, levels, usage and aspect
 * takes it back; contents are undefined, as a fresh image's are (every first
 * use transitions from UNDEFINED, which any layout satisfies). Oldest out. */
#define VK_IMG_POOL 24u
static vk_image s_imgpool[VK_IMG_POOL];
static u32 s_imgpool_n;
static void vk_destroy_image_real(vk_image* im);

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    for (u32 k = 0; k < s_imgpool_n; k++) {
        const vk_image* q = &s_imgpool[k];
        if (q->fmt == fmt && q->w == w && q->h == h && q->levels == levels &&
            q->usage == usage && q->aspect == aspect) {
            *im = *q;
            memmove(&s_imgpool[k], &s_imgpool[k + 1], (s_imgpool_n - k - 1) * sizeof s_imgpool[0]);
            s_imgpool_n--;
            return 0;
        }
    }
    /* PS3RECOMP_VK_ALLOC_LOG=<frames>: after that many presents, log the next 300
     * image creations (steady-state churn: each is a kernel allocation). */
    { static long after = -2, left = 300;
      if (after == -2) { const char* e = getenv("PS3RECOMP_VK_ALLOC_LOG"); after = e ? atol(e) : -1; }
      if (after >= 0 && s_vk.presented >= (u32)after && left > 0) { left--;
          fprintf(stderr, "[vk-alloc] frame %u image %ux%u fmt %d levels %u usage 0x%X caller %p\n",
                  s_vk.presented, w, h, (int)fmt, levels, (unsigned)usage, __builtin_return_address(0)); } }
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    im->fmt = fmt; im->w = w; im->h = h; im->levels = levels; im->usage = usage; im->aspect = aspect;
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static int vk_fif_busy(void);
static void vk_fif_wait_other(void);
static void vk_destroy_image_now(vk_image* im);
/* Bumped whenever an image (and with it a view) goes away or an engine object
 * is released: the descriptor memo (vk_eng_batch_set) holds views by handle. */
static u32 s_eobj_gen = 1;
static void vk_destroy_image(vk_image* im)
{
    s_eobj_gen++;
    /* A batched draw may still sample or render into it -- or, frames in
     * flight, the other slot's frame: destroy it when the batch has completed
     * (or finish the batch now if the list is full). */
    if ((s_batch.recording || vk_fif_busy()) && (im->view || im->img)) {
        if (s_batch.n_dead_img >= sizeof s_batch.dead_img / sizeof s_batch.dead_img[0]) {
            if (s_batch.recording) vk_batch_flush();
            else vk_fif_wait_other();
        }
        if ((s_batch.recording || vk_fif_busy()) &&
            s_batch.n_dead_img < sizeof s_batch.dead_img / sizeof s_batch.dead_img[0]) {
            s_batch.dead_img[s_batch.n_dead_img++] = *im;
            memset(im, 0, sizeof *im);
            return;
        }
    }
    vk_destroy_image_now(im);
}
/* the list's own destroy: everything that could use it is complete */
static void vk_destroy_image_now(vk_image* im)
{
    if (im->w && im->img && im->view && im->mem) {          /* recycle (see s_imgpool) */
        if (s_imgpool_n == VK_IMG_POOL) {
            vk_destroy_image_real(&s_imgpool[0]);
            memmove(&s_imgpool[0], &s_imgpool[1], (VK_IMG_POOL - 1) * sizeof s_imgpool[0]);
            s_imgpool_n--;
        }
        s_imgpool[s_imgpool_n++] = *im;
        memset(im, 0, sizeof *im);
        return;
    }
    vk_destroy_image_real(im);
}
static void vk_destroy_image_real(vk_image* im)
{
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

/* A buffer in device-local memory, not mapped: GPU-only traffic (the depth
 * decode's image -> buffer -> image bounces) stays on the card instead of
 * crossing PCIe twice through host memory. */
static int vk_create_device_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        VK_LOG("no device-local memory for a buffer\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(device buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    b->size = size;
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    if (s_batch.recording && b->buf) vk_batch_flush();
    if (b->buf) vk_fif_wait_other();         /* the other slot's frame may read it */
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * GPU timestamps (PS3RECOMP_VK_GPU_PROF=1)
 *
 * Where the GPU time of a batch goes. A timestamp (bottom of pipe) is written
 * at each boundary the backend crosses -- before an engine render pass (after
 * its barriers), at its end, around each backend pass (light, SSAO, depth
 * decodes) and around the present blit -- and each interval is credited to
 * the boundary that closes it: "gap" intervals are the copies, uploads and
 * barrier drains between passes, "pass" intervals the draws. Labels carry the
 * target size, so the deferred pipeline's passes can be told apart. Every 5 s
 * of batches it prints the top intervals per presented frame.
 * -------------------------------------------------------------------------*/
#define VK_GPROF_N 2048u
enum { GP_START = 0, GP_GAP = 1, GP_PASS = 2, GP_BGAP = 3, GP_BPASS = 4, GP_PGAP = 5, GP_BLIT = 6, GP_END = 7 };
static struct {
    int          on;                 /* -1 unread, 0 off, 1 on */
    struct {
        VkQueryPool pool;
        u32         n;                   /* stamps written into the slot's command buffer */
        u32         label[VK_GPROF_N];
        u32         ops[VK_GPROF_N][8];  /* commands recorded in each interval */
    } sl[VK_FIF_MAX];                    /* one per frame-in-flight slot */
    double       ns_per_tick;
    u32          klab[512]; double kms[512]; unsigned long long kcnt[512]; u32 nk;
    double       total_ms; unsigned long long batches, frames, dropped;
    uint64_t     t_print;
} s_gp = { .on = -1 };
static const char* const gp_kind[8] = { "start", "gap", "pass", "bgap", "bpass", "pgap", "blit", "end" };
#define GPS (s_gp.sl[s_fif_cur])
static void vk_gprof_init(void)
{
    if (s_gp.on >= 0) return;
    s_gp.on = getenv("PS3RECOMP_VK_GPU_PROF") ? 1 : 0;
    if (!s_gp.on) return;
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_gp.ns_per_tick = p.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = VK_GPROF_N };
    for (u32 k = 0; k < VK_FIF_MAX; k++)
        if (pvkCreateQueryPool(s_vk.device, &qi, NULL, &s_gp.sl[k].pool) != VK_SUCCESS) { s_gp.on = 0; return; }
    fprintf(stderr, "[RSX vulkan] GPU timestamps on (%.2f ns/tick)\n", s_gp.ns_per_tick);
}
static void vk_gprof_stamp(u32 kind, u32 w, u32 h)
{
    if (s_gp.on <= 0) return;
    if (GPS.n >= VK_GPROF_N) { s_gp.dropped++; return; }
    pvkCmdWriteTimestamp(s_vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, GPS.pool, GPS.n);
    memcpy(GPS.ops[GPS.n], s_gp_ops, sizeof s_gp_ops); memset(s_gp_ops, 0, sizeof s_gp_ops);
    GPS.label[GPS.n++] = (kind << 28) | ((w >> 2) & 0x3FFFu) << 14 | ((h >> 2) & 0x3FFFu);
}
/* a new command buffer: reset the pool and stamp its start */
static void vk_gprof_begin(void)
{
    if (s_gp.on < 0) vk_gprof_init();
    if (s_gp.on <= 0) return;
    pvkCmdResetQueryPool(s_vk.cmd, GPS.pool, 0, VK_GPROF_N);
    GPS.n = 0;
    vk_gprof_stamp(GP_START, 0, 0);
}
static void vk_gprof_note_present(void) { if (s_gp.on > 0) s_gp.frames++; }
/* the batch is complete: read its stamps and credit the intervals */
static void vk_gprof_collect(void)
{
    if (s_gp.on <= 0 || GPS.n < 2) { GPS.n = 0; return; }
    static uint64_t ts[VK_GPROF_N];
    const u32 n = GPS.n;
    GPS.n = 0;
    if (pvkGetQueryPoolResults(s_vk.device, GPS.pool, 0, n, sizeof(uint64_t) * n, ts, sizeof(uint64_t),
                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return;
    { static int dumped = 0; static unsigned long long seen = 0;
      if (getenv("PS3RECOMP_VK_GPU_PROF_DUMP") && n > 20 && ++seen > 1500 && dumped < 1) { dumped++;
          for (u32 i = 0; i < n; i++)
              fprintf(stderr, "[gpu-prof] raw %3u %-5s %4ux%-4u %llu  cpI=%u upl=%u cpB=%u clr=%u clz=%u blit=%u bar=%u drw=%u\n",
                      i, gp_kind[GPS.label[i] >> 28],
                      ((GPS.label[i] >> 14) & 0x3FFFu) << 2, (GPS.label[i] & 0x3FFFu) << 2, (unsigned long long)ts[i],
                      GPS.ops[i][0], GPS.ops[i][1], GPS.ops[i][2], GPS.ops[i][3], GPS.ops[i][4], GPS.ops[i][5],
                      GPS.ops[i][6], GPS.ops[i][7]); } }
    for (u32 i = 1; i < n; i++) {
        const double ms = (double)(ts[i] - ts[i - 1]) * s_gp.ns_per_tick / 1e6;
        const u32 lab = GPS.label[i];
        u32 k = 0;
        for (; k < s_gp.nk && s_gp.klab[k] != lab; k++) {}
        if (k == s_gp.nk) { if (s_gp.nk == 512) continue; s_gp.klab[s_gp.nk++] = lab; }
        s_gp.kms[k] += ms; s_gp.kcnt[k]++;
    }
    s_gp.total_ms += (double)(ts[n - 1] - ts[0]) * s_gp.ns_per_tick / 1e6;
    s_gp.batches++;
    const uint64_t now = ps3_ft_now();
    if (!s_gp.t_print) s_gp.t_print = now;
    if (now - s_gp.t_print < 5000000000ull || !s_gp.frames) return;   /* ps3_ft_now: ns */
    const double F = (double)s_gp.frames;
    fprintf(stderr, "[gpu-prof] %llu frames, %llu batches: GPU busy %.2f ms/frame (%llu stamps dropped)\n",
            s_gp.frames, s_gp.batches, s_gp.total_ms / F, s_gp.dropped);
    for (int r = 0; r < 24; r++) {
        int best = -1;
        for (u32 k = 0; k < s_gp.nk; k++) if (s_gp.kcnt[k] && (best < 0 || s_gp.kms[k] > s_gp.kms[best])) best = (int)k;
        if (best < 0) break;
        const u32 lab = s_gp.klab[best];
        fprintf(stderr, "[gpu-prof]   %6.3f ms/frame  %5.1f x  %-5s %4ux%-4u\n", s_gp.kms[best] / F,
                (double)s_gp.kcnt[best] / F, gp_kind[lab >> 28], ((lab >> 14) & 0x3FFFu) << 2, (lab & 0x3FFFu) << 2);
        s_gp.kcnt[best] = 0;
    }
    s_gp.nk = 0; s_gp.total_ms = 0; s_gp.batches = 0; s_gp.frames = 0; s_gp.dropped = 0;
    s_gp.t_print = now;
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 * -------------------------------------------------------------------------*/
/* Open the command buffer -- or, batching, keep appending to the open one. */
static void vk_batch_end_pass(void)
{
    if (s_batch.rp_open) {
        pvkCmdEndRenderPass(s_vk.cmd); s_batch.rp_open = 0;
        vk_gprof_stamp(GP_PASS, s_batch.rp_w, s_batch.rp_h);
    }
}
static int vk_begin(void)
{
    /* Any operation other than a continuing draw starts outside a pass. */
    if (vk_batch_on() && s_batch.recording) { vk_batch_end_pass(); return 0; }
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    if (s_batch.on > 0) s_batch.recording = 1;
    vk_gprof_begin();
    return 0;
}

/* Submit everything recorded and wait for it: the whole batch, when batching.
 * Afterwards the arenas and the per-draw descriptor sets are free again. */
/* Which source line forced each batch flush (PS3RECOMP_VK_BATCH_STATS). */
static struct { int line; unsigned long long n, draws; } s_flush_why[32];
static void vk_flush_note(int line, u32 draws)
{
    int k = 0;
    for (; k < 32 && s_flush_why[k].line && s_flush_why[k].line != line; k++) {}
    if (k < 32) { s_flush_why[k].line = line; s_flush_why[k].n++; s_flush_why[k].draws += draws; }
}
static void vk_batch_completed(void);
static void vk_vb_free_now(u32 h);
static u32 s_dset_gen;
static void vk_reuse_stats(void);
static int vk_submit_and_wait_at(int line)
{
    const int was_batch = s_batch.recording;
    if (was_batch) vk_flush_note(line, s_batch.draws);
    vk_batch_end_pass();
    vk_gprof_stamp(GP_END, 0, 0);
    s_batch.recording = 0;                  /* even on failure: never append to a dead buffer */
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence), "vkQueueSubmit");
    const uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    VK_CHECK(pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    if (ft0) ps3_ft_span(PS3_FT_GPU_WAIT, 0, was_batch ? 3u : 0u, s_batch.draws, ft0, ps3_ft_now());
    if (was_batch) vk_batch_completed();
    return 0;
}

/* The batch just submitted has completed: its arenas, descriptor sets and
 * retired objects are free again. */
static void vk_batch_completed(void)
{
    vk_gprof_collect();
    {
        s_batch.flushes++;
        s_batch.flushed_draws += s_batch.draws;
        if (s_batch.draws > s_batch.max_draws) s_batch.max_draws = s_batch.draws;
        s_batch.draws = 0;
        s_batch.vcur = s_batch.icur = s_batch.ucur = s_batch.scur = s_batch.gcur = 0;
        s_batch.zrgba_pending = 0;
        s_batch.light_pending = s_batch.ssao_pending = 0;
        for (u32 k = 0; k < s_batch.n_dead_fb; k++)
            pvkDestroyFramebuffer(s_vk.device, s_batch.dead_fb[k], NULL);
        s_batch.n_dead_fb = 0;
        for (u32 k = 0; k < s_batch.n_dead_img; k++)
            vk_destroy_image_now(&s_batch.dead_img[k]);   /* its users are complete */
        s_batch.n_dead_img = 0;
        for (u32 k = 0; k < s_batch.n_dead_vb; k++)
            vk_vb_free_now(s_batch.dead_vb[k]);
        s_batch.n_dead_vb = 0;
        if (s_batch.dpool && s_batch.dsets) {
            pvkResetDescriptorPool(s_vk.device, s_batch.dpool, 0);
            s_batch.dsets = 0;
        }
        s_dset_gen++;                           /* vk_eng_batch_set's table: those sets are gone */
        static int stats = -1;   /* PS3RECOMP_VK_BATCH_STATS=<every n flushes> (1 = 2000) */
        if (stats < 0) { const char* e = getenv("PS3RECOMP_VK_BATCH_STATS");
                         stats = e ? (atoi(e) > 1 ? atoi(e) : 2000) : 0; }
        if (stats && (s_batch.flushes % (unsigned long long)stats) == 0)
            fprintf(stderr, "[RSX vulkan] batching: %llu flushes, %.1f draws/flush (max %llu), %llu draws continued a pass\n",
                    s_batch.flushes, (double)s_batch.flushed_draws / (double)s_batch.flushes,
                    s_batch.max_draws, s_batch.merged);
        if (stats && (s_batch.flushes % (unsigned long long)stats) == 0) vk_reuse_stats();
        if (stats && (s_batch.flushes % (unsigned long long)stats) == 0) {
            for (int k = 0; k < 32 && s_flush_why[k].line; k++)
                if (s_flush_why[k].n)
                    fprintf(stderr, "[RSX vulkan]   flush from line %d: %llu (%.1f draws each)\n",
                            s_flush_why[k].line, s_flush_why[k].n,
                            (double)s_flush_why[k].draws / (double)s_flush_why[k].n);
            memset(s_flush_why, 0, sizeof s_flush_why);
        }
    }
}
/* Every later call names its source line, for the flush breakdown. */
#define vk_submit_and_wait() vk_submit_and_wait_at(__LINE__)

static void vk_batch_flush_at(int line)
{
    if (s_batch.recording) vk_submit_and_wait_at(line);
}

/* (Re)create one batch arena with room for at least `need`; only while idle. */
static int vk_batch_arena(vk_buffer* b, VkDeviceSize need, VkDeviceSize floor_size,
                          VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= need) return 0;
    VkDeviceSize size = b->size ? b->size * 2 : floor_size;
    while (size < need) size *= 2;
    vk_destroy_buffer(b);
    if (vk_create_host_buffer(b, size, usage) || !b->coherent) {
        VK_LOG("batching: could not get a %llu-byte host-coherent arena\n", (unsigned long long)size);
        vk_destroy_buffer(b);
        return -1;
    }
    return 0;
}

/* `bytes` of transfer staging that lives until the batch completes: the
 * replacement for a temporary buffer created, used and destroyed per call.
 * Returns the offset into s_batch.sarena, or -1. May flush (before the
 * caller opens its recording, so callers reserve first). */
static int64_t vk_batch_stage(VkDeviceSize bytes)
{
    for (int pass = 0; pass < 2; pass++) {
        const VkDeviceSize o = (s_batch.scur + 15u) & ~(VkDeviceSize)15u;
        if (s_batch.sarena.buf && o + bytes <= s_batch.sarena.size) {
            s_batch.scur = o + bytes;
            return (int64_t)o;
        }
        if (pass) break;
        vk_batch_flush();
        if (vk_batch_arena(&s_batch.sarena, bytes, 64u << 20,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            return -1;
    }
    return -1;
}

/* `bytes` of the GPU-local bounce arena, live until the batch completes (as
 * vk_batch_stage, for copies the host never touches). May flush. */
static int64_t vk_batch_gstage(VkDeviceSize bytes)
{
    for (int pass = 0; pass < 2; pass++) {
        const VkDeviceSize o = (s_batch.gcur + 255u) & ~(VkDeviceSize)255u;
        if (s_batch.garena.buf && o + bytes <= s_batch.garena.size) {
            s_batch.gcur = o + bytes;
            return (int64_t)o;
        }
        if (pass) break;
        vk_batch_flush();
        if (s_batch.garena.buf && s_batch.garena.size >= bytes) continue;
        VkDeviceSize size = s_batch.garena.size ? s_batch.garena.size * 2 : (64u << 20);
        while (size < bytes) size *= 2;
        vk_destroy_buffer(&s_batch.garena);
        if (vk_create_device_buffer(&s_batch.garena, size,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&s_batch.garena);
            return -1;
        }
    }
    return -1;
}

/* Destroy a framebuffer a recorded command may still use, once it cannot. */
static void vk_batch_retire_fb(VkFramebuffer fb)
{
    if (!fb) return;
    if (!s_batch.recording && !vk_fif_busy()) { pvkDestroyFramebuffer(s_vk.device, fb, NULL); return; }
    if (s_batch.n_dead_fb >= VK_BATCH_DEAD_FB) {
        if (s_batch.recording) vk_batch_flush();   /* frees the list */
        else vk_fif_wait_other();
    }
    if ((s_batch.recording || vk_fif_busy()) && s_batch.n_dead_fb < VK_BATCH_DEAD_FB)
        s_batch.dead_fb[s_batch.n_dead_fb++] = fb;
    else pvkDestroyFramebuffer(s_vk.device, fb, NULL);
}

/* Order everything recorded before against everything after: within one
 * batch a draw may sample what an earlier draw rendered. */
static void vk_barrier_all(void)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Disk caches: translated SPIR-V and the driver's pipeline cache
 *
 * Every run used to translate each guest shader HLSL -> SPIR-V through
 * glslang and have the driver build every pipeline from nothing: the long
 * black screen at a level load, and the hitch the first time an effect
 * appears. Both now persist, in one directory:
 *
 *   <key>.spv      one shader, exactly as glslang wrote it. The key is 128
 *                  bits -- FNV-1a under two seeds -- of VK_SC_VERSION, the
 *                  translator fingerprint (below), the fragcoord knob the
 *                  translator reads, the stage and the whole HLSL text. A hit
 *                  skips glslang. A file is used only if it is whole words,
 *                  starts with the SPIR-V magic and its instruction stream
 *                  ends exactly on an OpFunctionEnd, as every module glslang
 *                  writes does; anything else is a miss and is rewritten.
 *   pipelines.bin  the VkPipelineCache every vkCreateGraphicsPipelines goes
 *                  through, behind a 32-byte header of our own (magic, size,
 *                  hash). The driver's header in it -- vendor, device, cache
 *                  UUID -- is checked here before the driver sees the data,
 *                  as not every driver checks it: another GPU or a driver
 *                  update starts an empty cache. Saved at shutdown, after
 *                  every VK_SC_SAVE_EVERY new pipelines (at most every 2 s),
 *                  and at the first present 2 s after the last new one --
 *                  the game is usually killed rather than closed. A save
 *                  whose bytes match the file is not written.
 *
 * Every file is written under a temporary name and renamed over the old one,
 * so a killed process never leaves a torn file. Nothing is evicted: delete
 * the directory to start again.
 *
 * VK_SC_VERSION is in every key: bump it when the HLSL -> SPIR-V translation
 * changes in a way the fingerprint cannot see. The fingerprint is the SPIR-V
 * of two small canary shaders, translated once per run before the first
 * lookup: a glslang update, or a change to the translator's options (Y flip,
 * binding shifts, the fragcoord preamble), changes it and with it every key.
 *
 *   PS3RECOMP_VK_SHADER_CACHE=<dir>  the directory, created as needed. Default
 *       %LOCALAPPDATA%\ps3recomp\shader_cache\<title> on Windows, else
 *       ${XDG_CACHE_HOME:-$HOME/.cache}/ps3recomp/shader_cache/<title>, where
 *       <title> is the window title with anything but [A-Za-z0-9_-] as '_'.
 *   PS3RECOMP_VK_SHADER_CACHE=0      off: every shader and pipeline is built
 *       from scratch, exactly as before.
 *   PS3RECOMP_VK_SHADER_CACHE_STATS=1  the stats line at every pipeline-cache
 *       save; by default at most once per VK_SC_STATS_EVERY shaders and
 *       pipelines, and at shutdown.
 *
 * PS3_FTRACE spans, kind PS3_FT_COMPILE: who 1/2 a vertex/fragment shader
 * (SPIR-V, then vkCreateShaderModule), a = 1 built, b = 1 from the disk
 * cache; who 3 one vkCreateGraphicsPipelines, a = 1 built, b = 0 engine,
 * 1 guest, 2 fallback, 3 full-screen pass; who 5 a pipeline-cache save,
 * a = KB written (0: unchanged or failed), b = 0 periodic, 1 idle, 2 shutdown.
 * -------------------------------------------------------------------------*/
#define VK_SC_VERSION      "ps3recomp-vk-spirv-2"   /* bump: the translation changed (2: one binding per unit) */
#define VK_SC_PATH         1024u
#define VK_SC_SAVE_EVERY   64u
#define VK_SC_SAVE_GAP_NS  2000000000ull     /* periodic saves at most this often   */
#define VK_SC_IDLE_NS      2000000000ull     /* idle save: no new pipeline this long */
#define VK_SC_STATS_EVERY  64ull
#define VK_SC_SEED_A       14695981039346656037ull   /* the FNV-1a offset basis */
#define VK_SC_SEED_B       0x9E3779B97F4A7C15ull
#define VK_SC_PC_MAGIC     "PS3RVKPC"
#define VK_SC_PC_HEADER    32u
#ifdef _WIN32
#define VK_SC_SEP '\\'
#else
#define VK_SC_SEP '/'
#endif

static u64 vk_fnv1a64(const void* d, size_t n, u64 h);   /* guest programs */

/* -- shader cache: pure helpers (no Vulkan, no backend state) -- */
typedef struct { u64 a, b; } vk_sc_key;

static void vk_sc_key_add(vk_sc_key* k, const void* d, size_t n)
{
    k->a = vk_fnv1a64(d, n, k->a);
    k->b = vk_fnv1a64(d, n, k->b);
}

static vk_sc_key vk_sc_spv_key(const char* version, u64 fingerprint, u32 frag_rcp,
                               int stage, const char* text)
{
    vk_sc_key k = { VK_SC_SEED_A, VK_SC_SEED_B };
    const u32 st = (u32)stage;
    const size_t n = strlen(text);
    const u64 len = (u64)n;
    vk_sc_key_add(&k, version, strlen(version) + 1u);
    vk_sc_key_add(&k, &fingerprint, sizeof fingerprint);
    vk_sc_key_add(&k, &frag_rcp, sizeof frag_rcp);
    vk_sc_key_add(&k, &st, sizeof st);
    vk_sc_key_add(&k, &len, sizeof len);
    vk_sc_key_add(&k, text, n);
    return k;
}

/* <dir><sep><name>; -1 if it does not fit. */
static int vk_sc_join(char* out, size_t cap, const char* dir, const char* name)
{
    const int r = snprintf(out, cap, "%s%c%s", dir, VK_SC_SEP, name);
    return (r < 0 || (size_t)r >= cap) ? -1 : 0;
}

static int vk_sc_spv_path(char* out, size_t cap, const char* dir, vk_sc_key k)
{
    char name[48];
    snprintf(name, sizeof name, "%016llx%016llx.spv", (unsigned long long)k.a,
             (unsigned long long)k.b);
    return vk_sc_join(out, cap, dir, name);
}

/* `title` as one path component: [A-Za-z0-9_-] kept, anything else '_', at
 * most 48 characters, "default" when there is nothing. */
static void vk_sc_component(const char* title, char* out, size_t cap)
{
    size_t n = 0;
    for (const char* p = title; p && *p && n + 1u < cap && n < 48u; p++) {
        const char c = *p;
        const int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-';
        out[n++] = keep ? c : '_';
    }
    out[n] = 0;
    if (!n) snprintf(out, cap, "default");
}

/* The cache directory: `env` (PS3RECOMP_VK_SHADER_CACHE) when set, without
 * trailing separators, else the per-user default for `title`. -1 when there
 * is neither (no LOCALAPPDATA / HOME) or it does not fit. */
static int vk_sc_choose_dir(const char* env, const char* title, char* out, size_t cap)
{
    out[0] = 0;
    int r;
    if (env && *env) {
        r = snprintf(out, cap, "%s", env);
        if (r < 0 || (size_t)r >= cap) return -1;
        size_t n = (size_t)r;
        while (n > 1u && (out[n - 1u] == '/' || out[n - 1u] == '\\') && out[n - 2u] != ':')
            out[--n] = 0;
        return 0;
    }
    char comp[64];
    vk_sc_component(title, comp, sizeof comp);
#ifdef _WIN32
    const char* base = getenv("LOCALAPPDATA");
    if (!base || !*base) return -1;
    r = snprintf(out, cap, "%s\\ps3recomp\\shader_cache\\%s", base, comp);
#else
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    if (xdg && *xdg)        r = snprintf(out, cap, "%s/ps3recomp/shader_cache/%s", xdg, comp);
    else if (home && *home) r = snprintf(out, cap, "%s/.cache/ps3recomp/shader_cache/%s", home, comp);
    else                    return -1;
#endif
    if (r < 0 || (size_t)r >= cap) { out[0] = 0; return -1; }
    return 0;
}

static int vk_sc_is_dir(const char* p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void vk_sc_mkdir(const char* p)
{
#ifdef _WIN32
    CreateDirectoryA(p, NULL);              /* exists or fails: is_dir decides */
#else
    mkdir(p, 0755);
#endif
}

/* mkdir -p: every prefix ending before a separator, then the whole path. */
static int vk_sc_make_dirs(char* path)
{
    for (char* q = path + 1; *q; q++) {
        if ((*q != '/' && *q != '\\') || q[-1] == ':' || q[-1] == '/' || q[-1] == '\\')
            continue;
        const char c = *q;
        *q = 0;
        vk_sc_mkdir(path);
        *q = c;
    }
    vk_sc_mkdir(path);
    return vk_sc_is_dir(path) ? 0 : -1;
}

static unsigned long vk_sc_pid(void)
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    const pid_t p = getpid();
    return (unsigned long)p;
#endif
}

/* `head` then `body` into `path`, through <path>.<pid>.tmp and a rename over
 * it: a reader sees the old file or the whole new one, never a torn one. */
static int vk_sc_write_file(const char* path, const void* head, size_t nhead,
                            const void* body, size_t nbody)
{
    char tmp[VK_SC_PATH];
    const int r = snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, vk_sc_pid());
    if (r < 0 || (size_t)r >= sizeof tmp) return -1;
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = (!nhead || fwrite(head, 1, nhead, f) == nhead) &&
             (!nbody || fwrite(body, 1, nbody, f) == nbody);
    if (fclose(f) != 0) ok = 0;
#ifdef _WIN32
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 0;
#else
    if (ok && rename(tmp, path) == 0) return 0;
#endif
    remove(tmp);
    return -1;
}

/* The whole of `path` into buf[0, cap): 0 and its size, or -1 when it is
 * missing, unreadable or larger than cap. */
static int vk_sc_read_into(const char* path, void* buf, size_t cap, size_t* got)
{
    *got = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    const size_t n = fread(buf, 1, cap, f);
    const int bad = ferror(f) || (n == cap && fgetc(f) != EOF);
    fclose(f);
    if (bad) return -1;
    *got = n;
    return 0;
}

/* The whole of `path` in a malloc'd buffer (NULL when missing, empty or
 * unreadable). */
static u8* vk_sc_read_all(const char* path, size_t* size)
{
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    long end = -1;
    if (fseek(f, 0, SEEK_END) == 0) end = ftell(f);
    u8* buf = (end > 0 && fseek(f, 0, SEEK_SET) == 0) ? (u8*)malloc((size_t)end) : NULL;
    if (buf && fread(buf, 1, (size_t)end, f) != (size_t)end) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *size = (size_t)end;
    return buf;
}

/* Worth handing to vkCreateShaderModule: whole words, the SPIR-V magic, a
 * header with an id bound, instructions that tile the rest exactly and end
 * on OpFunctionEnd, and every function an OpEntryPoint or OpFunctionCall
 * names defined -- glslang's HLSL output is a `main` wrapper plus `@main`,
 * so a file cut just after the first of them is refused too. */
static int vk_sc_spirv_ok(const u32* w, size_t bytes)
{
    if (bytes % 4u || bytes < 24u) return 0;
    const size_t n = bytes / 4u;
    if (w[0] != 0x07230203u || w[3] == 0u || w[4] != 0u) return 0;
    u32 fn[64], nfn = 0, last = 0;
    int all_fn = 1;
    for (size_t i = 5; i < n;) {
        const u32 wc = w[i] >> 16;
        if (wc == 0u || wc > n - i) return 0;
        last = w[i] & 0xFFFFu;
        if (last == 54u && wc >= 3u) {       /* OpFunction: result id in word 2 */
            if (nfn < 64u) fn[nfn++] = w[i + 2];
            else           all_fn = 0;       /* too many to check: structure only */
        }
        i += wc;
    }
    if (last != 56u) return 0;               /* OpFunctionEnd */
    for (size_t i = 5; all_fn && i < n; i += w[i] >> 16) {
        const u32 op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        u32 target;
        if (op == 15u && wc >= 3u)      target = w[i + 2];   /* OpEntryPoint    */
        else if (op == 57u && wc >= 4u) target = w[i + 3];   /* OpFunctionCall  */
        else continue;
        u32 k = 0;
        while (k < nfn && fn[k] != target) k++;
        if (k == nfn) return 0;
    }
    return 1;
}

/* pipelines.bin: our header -- VK_SC_PC_MAGIC, format u32 1, u32 0, data
 * bytes u64, vk_sc_blob_hash u64, all little-endian -- then the bytes
 * vkGetPipelineCacheData returned. The hash takes eight bytes a step: the
 * blob can be tens of MB and is hashed at every save. */
static u64 vk_sc_blob_hash(const u8* p, size_t n)
{
    u64 h = VK_SC_SEED_A ^ (u64)n;
    size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        u64 v;
        memcpy(&v, p + i, sizeof v);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
}

static u32 vk_sc_le32(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 vk_sc_le64(const u8* p)
{
    return (u64)vk_sc_le32(p) | ((u64)vk_sc_le32(p + 4) << 32);
}

static void vk_sc_put_le64(u8* p, u64 v)
{
    for (u32 i = 0; i < 8u; i++) p[i] = (u8)(v >> (8u * i));
}

static void vk_sc_pc_header(u8* out, size_t n, u64 hash)
{
    memset(out, 0, VK_SC_PC_HEADER);
    memcpy(out, VK_SC_PC_MAGIC, 8);
    out[8] = 1;                                         /* format 1 */
    vk_sc_put_le64(out + 16, (u64)n);
    vk_sc_put_le64(out + 24, hash);
}

/* The driver data inside a pipelines.bin image, or NULL and *why. The second
 * half reads the VkPipelineCacheHeaderVersionOne the driver wrote first:
 * header size, version 1, vendor, device, pipelineCacheUUID. */
static const u8* vk_sc_pc_unwrap(const u8* file, size_t size, u32 vendor, u32 device,
                                 const u8* uuid, size_t* n, const char** why)
{
    *n = 0;
    if (size < VK_SC_PC_HEADER || memcmp(file, VK_SC_PC_MAGIC, 8) != 0 ||
        vk_sc_le32(file + 8) != 1u) {
        *why = "ignored, not a pipeline cache file"; return NULL;
    }
    const u64 len = vk_sc_le64(file + 16);
    if (len != (u64)(size - VK_SC_PC_HEADER)) { *why = "ignored, cut short"; return NULL; }
    const u8* data = file + VK_SC_PC_HEADER;
    if (vk_sc_blob_hash(data, (size_t)len) != vk_sc_le64(file + 24)) {
        *why = "ignored, damaged (hash mismatch)"; return NULL;
    }
    if (len < 32u || vk_sc_le32(data) < 32u || vk_sc_le32(data) > len ||
        vk_sc_le32(data + 4) != 1u) {
        *why = "ignored, no driver header"; return NULL;
    }
    if (vk_sc_le32(data + 8) != vendor || vk_sc_le32(data + 12) != device ||
        memcmp(data + 16, uuid, 16) != 0) {
        *why = "ignored, made by another GPU or driver"; return NULL;
    }
    *n = (size_t)len;
    return data;
}
/* -- end of the pure helpers -- */

/* Two shaders shaped like the decompilers' output, translated once per run:
 * their SPIR-V is the translator fingerprint. The vertex one exercises the Y
 * flip, the fragment one the texture/sampler binding shifts and the
 * fragcoord preamble. */
static const char kVkScCanaryVS[] =
    "cbuffer VPConst : register(b0) { float4 vc[4]; };\n"
    "struct VSInput { float4 a0:ATTR0; float4 a8:ATTR8; };\n"
    "struct VSOutput { float4 pos:SV_Position; float4 tc0:TEXCOORD0; };\n"
    "VSOutput main(VSInput i) {\n"
    "    VSOutput o;\n"
    "    o.pos = float4(dot(i.a0, vc[0]), dot(i.a0, vc[1]), dot(i.a0, vc[2]), dot(i.a0, vc[3]));\n"
    "    o.tc0 = i.a8;\n"
    "    return o;\n"
    "}\n";
static const char kVkScCanaryFS[] =
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "cbuffer PSConstants : register(b1) { float4 pc[2]; };\n"
    "struct PSInput { float4 wpos:SV_Position; float4 tc0:TEXCOORD0; };\n"
    "float4 main(PSInput i) : SV_Target0 {\n"
    "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
    "    float w = 1.0 / i.wpos.w;\n"
    "#else\n"
    "    float w = i.wpos.w;\n"
    "#endif\n"
    "    return rsx_tex[2].Sample(rsx_samp[2], i.tc0.xy) * pc[0] * w;\n"
    "}\n";

static struct {
    int             on;                  /* the directory is usable                 */
    int             fp_done;             /* fingerprint taken (or given up)         */
    int             stats_all;           /* PS3RECOMP_VK_SHADER_CACHE_STATS         */
    u32             frag_rcp;            /* PS3RECOMP_FRAGCOORD_W, as the translator reads it */
    u32             pc_new;              /* pipelines created since the last save   */
    u32             warned;              /* 1 a write failed, 2 unstorable SPIR-V   */
    u64             fp;                  /* translator fingerprint, 0 if none       */
    VkPipelineCache pc;
    u64             pc_hash, pc_size;    /* what pipelines.bin holds                */
    u64             pc_last_new, pc_last_save;   /* ps3_ft_now()                    */
    unsigned long long hits, misses, rejected, stored, spv_ns, fp_ns;
    unsigned long long pipes, pipe_ns, saves, save_ns, stats_at;
    char            dir[VK_SC_PATH];
} s_sc;

static void vk_sc_fingerprint(u32* words, u32 max_words)
{
    s_sc.fp_done = 1;
    const u64 t0 = ps3_ft_now();
    u64 h = VK_SC_SEED_A;
    for (int s = 0; s < 2; s++) {
        char log[512];
        u32 n = 0;
        if (rsx_hlsl_to_spirv(s ? kVkScCanaryFS : kVkScCanaryVS,
                              s ? RSX_SHADER_STAGE_FRAGMENT : RSX_SHADER_STAGE_VERTEX,
                              words, max_words, &n, log, sizeof log) || !n) {
            VK_LOG("shader cache: the %s canary does not translate (%s) -- keyed on "
                   VK_SC_VERSION " alone\n", s ? "fragment" : "vertex", log);
            return;
        }
        h = vk_fnv1a64(words, (size_t)n * 4u, h);
    }
    s_sc.fp = h;
    s_sc.fp_ns = ps3_ft_now() - t0;
    VK_LOG("shader cache: translator fingerprint %016llx (%.1f ms)\n",
           (unsigned long long)h, (double)s_sc.fp_ns / 1e6);
}

/* rsx_hlsl_to_spirv through the disk cache: the same arguments and result,
 * and *from_disk = 1 when the SPIR-V came from a file and glslang never ran. */
/* The fragment decompiler declares its units either as one array
 * ("Texture2D    rsx_tex[16] : register(t0);", "SamplerState rsx_samp[16] :
 * register(s0);", used as rsx_tex[N] / rsx_samp[N] with literal N) or, when a
 * unit is a cube or a volume, one by one ("rsx_texN : register(tN)"). The
 * guest set layout has one binding per unit, which only the second form
 * matches: rewrite the first into it. Returns a malloc'd copy, or NULL when
 * there is nothing to rewrite. */
static char* vk_hlsl_unarray(const char* hlsl)
{
    static const char tdecl[] = "Texture2D    rsx_tex[16] : register(t0);\n";
    static const char sdecl[] = "SamplerState rsx_samp[16] : register(s0);\n";
    if (!hlsl || (!strstr(hlsl, tdecl) && !strstr(hlsl, sdecl))) return NULL;
    const size_t len = strlen(hlsl);
    char* out = (char*)malloc(len + 4096);
    if (!out) return NULL;
    size_t o = 0;
    for (const char* q = hlsl; *q; ) {
        if (!strncmp(q, tdecl, sizeof tdecl - 1)) {
            for (u32 u = 0; u < 16; u++) o += (size_t)sprintf(out + o, "Texture2D   rsx_tex%u : register(t%u);\n", u, u);
            q += sizeof tdecl - 1; continue;
        }
        if (!strncmp(q, sdecl, sizeof sdecl - 1)) {
            for (u32 u = 0; u < 16; u++) o += (size_t)sprintf(out + o, "SamplerState rsx_samp%u : register(s%u);\n", u, u);
            q += sizeof sdecl - 1; continue;
        }
        const size_t nl = !strncmp(q, "rsx_tex[", 8) ? 8 : !strncmp(q, "rsx_samp[", 9) ? 9 : 0;
        if (nl && q[nl] >= '0' && q[nl] <= '9') {
            const char* d = q + nl; u32 v = 0;
            while (*d >= '0' && *d <= '9') v = v * 10u + (u32)(*d++ - '0');
            if (*d == ']' && v < 16u) {
                o += (size_t)sprintf(out + o, "%s%u", nl == 8 ? "rsx_tex" : "rsx_samp", v);
                q = d + 1; continue;
            }
        }
        if (o + 64 >= len + 4096) { free(out); return NULL; }
        out[o++] = *q++;
    }
    out[o] = 0;
    return out;
}

static int vk_sc_hlsl_to_spirv_raw(const char* hlsl, int stage, u32* words, u32 max_words,
                                   u32* out_words, char* log, u32 log_size, int* from_disk);
static int vk_sc_hlsl_to_spirv(const char* hlsl, int stage, u32* words, u32 max_words,
                               u32* out_words, char* log, u32 log_size, int* from_disk)
{
    char* un = vk_hlsl_unarray(hlsl);
    const int rc = vk_sc_hlsl_to_spirv_raw(un ? un : hlsl, stage, words, max_words, out_words,
                                           log, log_size, from_disk);
    free(un);
    return rc;
}
static int vk_sc_hlsl_to_spirv_raw(const char* hlsl, int stage, u32* words, u32 max_words,
                                   u32* out_words, char* log, u32 log_size, int* from_disk)
{
    *from_disk = 0;
    char path[VK_SC_PATH];
    int cached = 0;
    if (s_sc.on && hlsl && words && out_words) {
        if (!s_sc.fp_done) vk_sc_fingerprint(words, max_words);
        const vk_sc_key k = vk_sc_spv_key(VK_SC_VERSION, s_sc.fp, s_sc.frag_rcp, stage, hlsl);
        cached = vk_sc_spv_path(path, sizeof path, s_sc.dir, k) == 0;
    }
    if (cached) {
        size_t got = 0;
        const int rd = vk_sc_read_into(path, words, (size_t)max_words * 4u, &got);
        if (rd == 0 && vk_sc_spirv_ok(words, got)) {
            *out_words = (u32)(got / 4u);
            *from_disk = 1;
            s_sc.hits++;
            return 0;
        }
        s_sc.misses++;
        if (rd == 0 && ++s_sc.rejected <= 4u)
            VK_LOG("shader cache: %s is not valid SPIR-V -- translating again\n", path);
    }
    const u64 t0 = ps3_ft_now();
    const int rc = rsx_hlsl_to_spirv(hlsl, stage, words, max_words, out_words, log, log_size);
    s_sc.spv_ns += ps3_ft_now() - t0;
    if (cached && rc == 0) {
        const size_t bytes = (size_t)*out_words * 4u;
        if (!vk_sc_spirv_ok(words, bytes)) {
            if (!(s_sc.warned & 2u)) VK_LOG("shader cache: glslang wrote SPIR-V the cache's check refuses -- not stored\n");
            s_sc.warned |= 2u;
        } else if (vk_sc_write_file(path, NULL, 0, words, bytes) == 0) {
            s_sc.stored++;
        } else {
            if (!(s_sc.warned & 1u)) VK_LOG("shader cache: cannot write %s\n", path);
            s_sc.warned |= 1u;
        }
    }
    return rc;
}

static void vk_sc_stats(const char* when)
{
    VK_LOG("shader cache: %llu hits, %llu misses; pipelines: %llu created "
           "(%s; glslang %.0f ms, vkCreateGraphicsPipelines %.0f ms, "
           "%llu stored, %llu rejected; pipelines.bin %llu KB, %llu saves %.0f ms)\n",
           s_sc.hits, s_sc.misses, s_sc.pipes, when,
           (double)s_sc.spv_ns / 1e6, (double)s_sc.pipe_ns / 1e6, s_sc.stored, s_sc.rejected,
           (unsigned long long)(s_sc.pc_size / 1024u), s_sc.saves, (double)s_sc.save_ns / 1e6);
    s_sc.stats_at = s_sc.hits + s_sc.misses + s_sc.pipes;
}

/* why: 0 periodic, 1 idle, 2 shutdown (the trace's b). The caller resets pc_new and
 * pc_last_save: this also runs on the background saver (vk_sc_save_async). */
static void vk_sc_save_pipelines(u32 why)
{
    if (!s_sc.pc) return;
    const u64 t0 = ps3_ft_now();
    u32 kb = 0;
    size_t n = 0;
    u8* data = NULL;
    if (pvkGetPipelineCacheData(s_vk.device, s_sc.pc, &n, NULL) == VK_SUCCESS && n)
        data = (u8*)malloc(n);
    if (data && pvkGetPipelineCacheData(s_vk.device, s_sc.pc, &n, data) == VK_SUCCESS) {
        const u64 h = vk_sc_blob_hash(data, n);
        if (h != s_sc.pc_hash || (u64)n != s_sc.pc_size) {
            u8 head[VK_SC_PC_HEADER];
            char path[VK_SC_PATH];
            vk_sc_pc_header(head, n, h);
            if (vk_sc_join(path, sizeof path, s_sc.dir, "pipelines.bin") == 0 &&
                vk_sc_write_file(path, head, sizeof head, data, n) == 0) {
                s_sc.pc_hash = h;
                s_sc.pc_size = (u64)n;
                kb = (u32)(n / 1024u) + 1u;
                s_sc.saves++;
            } else {
                if (!(s_sc.warned & 1u))
                    VK_LOG("shader cache: cannot write %s%cpipelines.bin\n", s_sc.dir, VK_SC_SEP);
                s_sc.warned |= 1u;
            }
        }
    }
    free(data);
    const u64 t1 = ps3_ft_now();
    s_sc.save_ns += t1 - t0;
    if (PS3_FT_ON()) ps3_ft_span(PS3_FT_COMPILE, 5, kb, why, t0, t1);
}

/* Saves run on a short-lived background thread: reading the driver's cache data
 * (vkGetPipelineCacheData, ~55 MB for inFAMOUS 2), hashing it and writing it took
 * 20-76 ms of the RSX thread each time -- 21 times a run, the largest hitch source
 * once the cache was warm. The pipeline cache is internally synchronised (created
 * without EXTERNALLY_SYNCHRONIZED), so reading it while pipelines are created is
 * legal. One save at a time: a trigger while one runs is left to the next. */
static volatile int s_sc_saving;
#ifdef _WIN32
static DWORD WINAPI vk_sc_save_thread(LPVOID why)
{
    vk_sc_save_pipelines((u32)(uintptr_t)why);
    __atomic_store_n(&s_sc_saving, 0, __ATOMIC_RELEASE);
    return 0;
}
#else
static void* vk_sc_save_thread(void* why)
{
    vk_sc_save_pipelines((u32)(uintptr_t)why);
    __atomic_store_n(&s_sc_saving, 0, __ATOMIC_RELEASE);
    return NULL;
}
#endif
static void vk_sc_save_async(u32 why)
{
    if (__atomic_exchange_n(&s_sc_saving, 1, __ATOMIC_ACQ_REL)) return;
    s_sc.pc_new = 0;
    s_sc.pc_last_save = ps3_ft_now();
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, vk_sc_save_thread, (LPVOID)(uintptr_t)why, 0, NULL);
    if (h) { CloseHandle(h); return; }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, vk_sc_save_thread, (void*)(uintptr_t)why) == 0) { pthread_detach(t); return; }
#endif
    vk_sc_save_pipelines(why);                 /* no thread: as before */
    __atomic_store_n(&s_sc_saving, 0, __ATOMIC_RELEASE);
}
static void vk_sc_save_wait(void)
{
    while (__atomic_load_n(&s_sc_saving, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        Sleep(1);
#else
        sched_yield();
#endif
    }
}

static void vk_sc_maybe_stats(const char* when)
{
    if (s_sc.stats_all || s_sc.hits + s_sc.misses + s_sc.pipes - s_sc.stats_at >= VK_SC_STATS_EVERY)
        vk_sc_stats(when);
}

/* Every vkCreateGraphicsPipelines: one pipeline, through the cache, timed;
 * `site` is the trace's b (0 engine, 1 guest, 2 fallback, 3 pass). */
static VkResult vk_sc_create_pipeline(const VkGraphicsPipelineCreateInfo* gci,
                                      VkPipeline* out, u32 site)
{
    const u64 t0 = ps3_ft_now();
    const VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_sc.pc, 1, gci, NULL, out);
    const u64 t1 = ps3_ft_now();
    if (PS3_FT_ON()) ps3_ft_span(PS3_FT_COMPILE, 3, r == VK_SUCCESS ? 1u : 0u, site, t0, t1);
    s_sc.pipes++;
    s_sc.pipe_ns += t1 - t0;
    /* Only a pipeline the driver had to build counts towards a save: one found in
     * the cache comes back in ~0.1 ms, a real build takes 5-50 ms. With a warm cache
     * every run used to re-save (and re-read) the whole file after each 64 pipelines
     * it merely looked up. */
    if (r == VK_SUCCESS && s_sc.pc && t1 - t0 > 1000000ull) {
        s_sc.pc_new++;
        s_sc.pc_last_new = t1;
        if (s_sc.pc_new >= VK_SC_SAVE_EVERY && t1 - s_sc.pc_last_save >= VK_SC_SAVE_GAP_NS) {
            vk_sc_save_async(0);
            vk_sc_maybe_stats("periodic");
        }
    }
    return r;
}

/* At each present: the tail of a burst of new pipelines, once it has ended. */
static void vk_sc_tick(void)
{
    if (!s_sc.pc_new || ps3_ft_now() - s_sc.pc_last_new < VK_SC_IDLE_NS) return;
    vk_sc_save_async(1);
    vk_sc_maybe_stats("idle");
}

/* After device creation: choose and create the directory, and create the
 * pipeline cache from pipelines.bin when it belongs to this device. */
static void vk_sc_open(const char* title)
{
    memset(&s_sc, 0, sizeof s_sc);
    const char* fw = getenv("PS3RECOMP_FRAGCOORD_W");    /* read as rsx_shader_spirv.cpp does */
    s_sc.frag_rcp = (fw && *fw == 'w') ? 0u : 1u;
    const char* st = getenv("PS3RECOMP_VK_SHADER_CACHE_STATS");
    s_sc.stats_all = (st && *st && *st != '0') ? 1 : 0;
    const char* env = getenv("PS3RECOMP_VK_SHADER_CACHE");
    if (env && strcmp(env, "0") == 0) {
        VK_LOG("shader cache: off (PS3RECOMP_VK_SHADER_CACHE=0)\n");
        return;
    }
    if (vk_sc_choose_dir(env, title, s_sc.dir, sizeof s_sc.dir) || vk_sc_make_dirs(s_sc.dir)) {
        VK_LOG("shader cache: off -- no usable directory (%s); set PS3RECOMP_VK_SHADER_CACHE=<dir>\n",
               s_sc.dir[0] ? s_sc.dir : "no LOCALAPPDATA/HOME");
        s_sc.dir[0] = 0;
        return;
    }
    s_sc.on = 1;

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    char path[VK_SC_PATH];
    size_t size = 0, n = 0;
    u8* file = vk_sc_join(path, sizeof path, s_sc.dir, "pipelines.bin") == 0
             ? vk_sc_read_all(path, &size) : NULL;
    const char* why = "none yet";
    const u8* data = file ? vk_sc_pc_unwrap(file, size, p.vendorID, p.deviceID,
                                            p.pipelineCacheUUID, &n, &why) : NULL;
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = data ? n : 0, .pInitialData = data };
    VkResult r = pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_sc.pc);
    if (r != VK_SUCCESS && data) {
        why = "ignored, refused by the driver";
        data = NULL;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        r = pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_sc.pc);
    }
    if (r != VK_SUCCESS) {
        s_sc.pc = VK_NULL_HANDLE;
        why = "vkCreatePipelineCache failed: pipelines not cached";
    }
    if (data) {                    /* a save of the same bytes is skipped */
        s_sc.pc_hash = vk_sc_blob_hash(data, n);
        s_sc.pc_size = (u64)n;
    }
    free(file);
    s_sc.pc_last_save = ps3_ft_now();
    if (data) VK_LOG("shader cache: %s (pipelines.bin: %llu KB loaded)\n", s_sc.dir,
                     (unsigned long long)(n / 1024u));
    else      VK_LOG("shader cache: %s (pipelines.bin: %s)\n", s_sc.dir, why);
}

/* Before the device goes: the last save, the stats, the cache object. */
static void vk_sc_close(void)
{
    vk_sc_save_wait();                         /* a background save first */
    if (s_sc.pc && s_sc.pc_new) { s_sc.pc_new = 0; vk_sc_save_pipelines(2); }
    if (s_sc.hits + s_sc.misses + s_sc.pipes) vk_sc_stats(s_sc.on ? "shutdown" : "shutdown, cache off");
    if (s_sc.pc) pvkDestroyPipelineCache(s_vk.device, s_sc.pc, NULL);
    memset(&s_sc, 0, sizeof s_sc);
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    /* The RSX's Z24S8 has a stencil the titles test (inFAMOUS 2 marks water,
     * sky and lit receivers in it); without one every stencil-tested pass
     * covers the whole target. D32_SFLOAT_S8_UINT keeps the float depth the
     * snapshots copy out. PS3RECOMP_VK_STENCIL=0: depth only (A/B). */
    { const char* e = getenv("PS3RECOMP_VK_STENCIL");
      VkFormatProperties fp;
      pvkGetPhysicalDeviceFormatProperties(s_vk.phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
      if (!(e && *e == '0') && (fp.optimalTilingFeatures & need) == need) {
          s_vk.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
          s_vk_ds_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
          return 0;
      } }
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; return 0;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; return 0;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vk_sc_create_pipeline(&gci, &p, 2);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    const uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    u32 n = 0;
    int disk = 0;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vk_sc_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                            s_g.log, sizeof s_g.log, &disk) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
    } else {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
        if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
            VK_LOG("%s: vkCreateShaderModule failed\n", what);
            m = VK_NULL_HANDLE;
        }
    }
    if (ft0) ps3_ft_span(PS3_FT_COMPILE, stage == RSX_SHADER_STAGE_VERTEX ? 1u : 2u,
                         m ? 1u : 0u, disk ? 1u : 0u, ft0, ps3_ft_now());
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vk_sc_create_pipeline(&gci, &p, 1);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    /* One binding per texture unit and per sampler (register(tN) -> 16+N,
     * register(sN) -> 48+N), never one 16-element array: a program that
     * declares a TextureCube or Texture3D gets every unit declared on its own,
     * and with an array layout its units 1..15 named bindings the layout did
     * not have -- the driver read them as zero (the deferred composite saw no
     * normals, light, AO or sun mask; the cutscene's final pass, no scene).
     * vk_hlsl_unarray gives the array-declared programs the same form. The
     * descriptor writes fill all 16 in one go: consecutive bindings of one
     * type and stage set take the overflow. */
    VkDescriptorSetLayoutBinding b[2 + 2 * RSX_MAX_TEXTURES] = {
        /* dynamic: the offsets of a draw's constant window are given at bind
         * time, so one set serves every draw with the same textures and
         * samplers (vk_eng_batch_set) */
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL },
    };
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        b[2 + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        b[2 + RSX_MAX_TEXTURES + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2 + 2 * RSX_MAX_TEXTURES, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, 0, fp_len },          /* at s_g.ps_off: the dynamic offset */
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    { const u32 dyn[2] = { 0u, (u32)s_g.ps_off };
      pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                               0, 1, &s_g.set, 2, dyn); }
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_DS_ASPECT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        pvkDeviceWaitIdle(s_vk.device);
        vk_overlay_shutdown();               /* [overlay] its views of the swapchain images first */
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        for (u32 k = 0; k < VK_FIF_MAX; k++)
            if (s_fif_acq[k]) { pvkDestroySemaphore(s_vk.device, s_fif_acq[k], NULL); s_fif_acq[k] = VK_NULL_HANDLE; }
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)s_vk.width, (int)s_vk.height, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}

/* Minimized: the surface has no area, so there is no swapchain to make. That
 * used to drop the window for good (the run went on headless: a black window
 * after the player minimized the game once). Now presenting pauses -- each
 * frame is submitted as a headless one and paced by the frame cap -- and the
 * swapchain is rebuilt as soon as the window has an area again. */
static int s_sc_minimized;
#define VK_SC_NO_AREA 1
static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) return VK_SC_NO_AREA;   /* minimized: see s_sc_minimized */

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    /* Present mode. FIFO (vsync) queues finished frames behind the display: with
     * three images and two frames in flight, up to 2-3 frames waited between the
     * game reading the controller and the picture showing it (Sami: "input lag
     * using the controller", 2026-11-07). MAILBOX shows the newest finished frame
     * at the next refresh and drops a waiting one -- no tearing, no queue -- and
     * the frame cap (vk_frame_limit) does the pacing. PS3RECOMP_VK_PRESENT=fifo |
     * mailbox | immediate; default mailbox where the driver has it, else fifo. */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   /* the one mode every driver has */
    {
        const char* want = getenv("PS3RECOMP_VK_PRESENT");
        VkPresentModeKHR pref = VK_PRESENT_MODE_MAILBOX_KHR;
        if (want && !strcmp(want, "fifo")) pref = VK_PRESENT_MODE_FIFO_KHR;
        else if (want && !strcmp(want, "immediate")) pref = VK_PRESENT_MODE_IMMEDIATE_KHR;
        VkPresentModeKHR modes[8];
        u32 nm = 8;
        if (pref != VK_PRESENT_MODE_FIFO_KHR && pvkGetPhysicalDeviceSurfacePresentModesKHR &&
            pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &nm, modes) >= 0)
            for (u32 k = 0; k < nm; k++) if (modes[k] == pref) mode = pref;
        static int said = -1;   /* [overlay] logged again when the menu changes it */
        if (said != (int)mode) { said = (int)mode;
            VK_LOG("present mode: %s\n", mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox (no vsync queue)"
                                         : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo (vsync)"); }
    }

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |   /* [overlay] it draws into them: */
                      (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = mode,
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    /* [overlay] its views of the old images go before the old swapchain does */
    vk_overlay_swapchain_changed(fmt.format, (sci.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d\n", ext.width, ext.height, n, (int)fmt.format);
    return 0;
}

static void vk_swapchain_lost(void)
{
    pvkDeviceWaitIdle(s_vk.device);
    const int r = vk_create_swapchain();
    if (r == VK_SC_NO_AREA) {
        if (!s_sc_minimized) VK_LOG("window minimized: presenting paused\n");
        s_sc_minimized = 1;
        return;
    }
    s_sc_minimized = 0;
    if (r) vk_window_drop("the swapchain could not be recreated");
}
/* 1 when a frame can go to the window: windowed, a swapchain, and -- after a
 * minimize -- the window back with an area (the swapchain rebuilt here). */
static int vk_window_ready(void)
{
    if (!s_vk.windowed || !s_vk.swapchain) return 0;
    if (!s_sc_minimized) return 1;
    static unsigned long long tries;
    if (++tries % 15u) return 0;            /* look a few times a second, not every frame */
    if (s_vk.window && (SDL_GetWindowFlags(s_vk.window) & SDL_WINDOW_MINIMIZED)) return 0;
    pvkDeviceWaitIdle(s_vk.device);
    const int r = vk_create_swapchain();
    if (r == VK_SC_NO_AREA) return 0;
    s_sc_minimized = 0;
    if (r) { vk_window_drop("the swapchain could not be recreated"); return 0; }
    VK_LOG("window restored: presenting again\n");
    return 1;
}

static void vk_fif_advance(void);

/* [overlay] Live PC settings (include/ps3emu/ps3_settings.h): the in-game menu
 * (vk_overlay.cpp) sets the variable, then these run on this, the presenting,
 * thread: the frame cap is re-read at the next present, a present mode or a
 * fullscreen change rebuilds the swapchain before the next acquire. */
static volatile int s_fps_cap_dirty, s_present_dirty;
static int vk_set_fps_cap(const char* v) { (void)v; s_fps_cap_dirty = 1; return 1; }
static int vk_set_present(const char* v) { (void)v; s_present_dirty = 1; return 1; }
static int vk_set_fullscreen(const char* v)
{
    if (!s_vk.window) return 0;
    const int on = v && v[0] && v[0] != '0';
    if (SDL_SetWindowFullscreen(s_vk.window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) return 0;
    s_present_dirty = 1;
    return 1;
}

/* PS3RECOMP_FPS_CAP=<fps> (0/unset = uncapped): present at most that many frames a
 * second, on an even beat -- each present waits for its deadline (the last one plus
 * the period; a frame that is late moves the beat instead of bunching the next ones
 * up). The RSX thread waiting here stalls the FIFO, so the guest, which waits on its
 * flips and labels, follows the beat: the game runs at the cap, not just shows it.
 * The wait is a high-resolution waitable timer to ~1 ms before the deadline and a
 * spin for the rest. */
static void vk_frame_limit(void)
{
    static double period_s = -1.0;
    if (period_s < 0.0 || s_fps_cap_dirty) {   /* [overlay] or changed in the menu */
        const int live = s_fps_cap_dirty;
        s_fps_cap_dirty = 0;
        const char* e = getenv("PS3RECOMP_FPS_CAP");
        const double f = e ? atof(e) : 0.0;
        period_s = f > 0.0 ? 1.0 / f : 0.0;
        if (f > 0.0) fprintf(stderr, "[RSX vulkan] frame cap %.1f fps (PS3RECOMP_FPS_CAP)\n", f);
        else if (live) fprintf(stderr, "[RSX vulkan] frame cap off (PS3RECOMP_FPS_CAP)\n");
    }
    if (period_s <= 0.0) return;
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static LONGLONG next;
    static HANDLE timer;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    const LONGLONG per = (LONGLONG)(period_s * (double)freq.QuadPart);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!next || now.QuadPart - next > per) next = now.QuadPart;   /* first frame, or late: new beat */
    else next += per;
    for (;;) {
        QueryPerformanceCounter(&now);
        const LONGLONG left = next - now.QuadPart;
        if (left <= 0) break;
        const LONGLONG left_100ns = left * 10000000 / freq.QuadPart;
        if (left_100ns > 15000 && timer) {                 /* > 1.5 ms: sleep to ~1 ms short */
            LARGE_INTEGER due; due.QuadPart = -(left_100ns - 10000);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
            else Sleep(1);
        } else {
            YieldProcessor();
        }
    }
#else
    static struct timespec next;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long long per = (long long)(period_s * 1e9);
    long long n = (long long)now.tv_sec * 1000000000ll + now.tv_nsec;
    long long t = (long long)next.tv_sec * 1000000000ll + next.tv_nsec;
    if (!t || n - t > per) t = n; else t += per;
    next.tv_sec = t / 1000000000ll; next.tv_nsec = t % 1000000000ll;
    if (t > n) clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
#endif
}

#ifdef _WIN32
/* PS3RECOMP_VK_RDOC_AT=<sec>[,<sec>...] (diagnostic): when RenderDoc is loaded --
 * ENABLE_VULKAN_RENDERDOC_CAPTURE=1 switches on its implicit Vulkan layer (the
 * installer registers it), or renderdoccmd capture -- ask it for a frame
 * capture at each of these times, in seconds since the process started (the
 * scripted runs' PAD_SCRIPT and screenshot clock); PS3RECOMP_VK_RDOC_TRIGGER
 * and PS3RECOMP_VK_RDOC_FILE below. RenderDoc's in-application API
 * (renderdoc_app.h, MIT): RENDERDOC_GetAPI from the loaded renderdoc.dll, API
 * 1.0.0, whose 16th entry is TriggerCapture (the next frame, present to
 * present). tools/combat_beast.ps1 -RdocAt / -RdocTrigger set all of it up,
 * tools/rdoc_inspect.py reads the captures headless. Without RenderDoc loaded,
 * or without the variables, nothing happens. */
static void vk_rdoc_poll(void)
{
    static int state = -1, n = 0, next = 0;
    static double at[16];
    static void (*trigger)(void);
    static ULONGLONG t_start;
    static const char* tfile;   /* PS3RECOMP_VK_RDOC_TRIGGER */
    static unsigned tick;
    if (state < 0) {
        state = 0;
        const char* e = getenv("PS3RECOMP_VK_RDOC_AT");
        tfile = getenv("PS3RECOMP_VK_RDOC_TRIGGER");
        if (tfile && !*tfile) tfile = NULL;
        HMODULE m = GetModuleHandleA("renderdoc.dll");
        if ((!e || !*e) && !tfile) return;
        if (!e) e = "";
        if (!m) { VK_LOG("PS3RECOMP_VK_RDOC_AT: renderdoc.dll is not loaded (run under renderdoccmd capture)\n"); return; }
        typedef int (*rdoc_get_api_fn)(int version, void** out);
        rdoc_get_api_fn get_api = (rdoc_get_api_fn)(void*)GetProcAddress(m, "RENDERDOC_GetAPI");
        void** api = NULL;
        if (!get_api || !get_api(10000 /* eRENDERDOC_API_Version_1_0_0 */, (void**)&api) || !api) {
            VK_LOG("PS3RECOMP_VK_RDOC_AT: RENDERDOC_GetAPI failed\n"); return;
        }
        trigger = (void (*)(void))api[15];
        /* PS3RECOMP_VK_RDOC_FILE=<path template>: where the captures go (entry 12,
         * SetCaptureFilePathTemplate); RenderDoc appends _frameN.rdc. */
        const char* tmpl = getenv("PS3RECOMP_VK_RDOC_FILE");
        if (tmpl && *tmpl) ((void (*)(const char*))api[11])(tmpl);
        for (const char* p = e; *p && n < 16; ) {
            char* q; const double t = strtod(p, &q);
            if (q == p) break;
            at[n++] = t; p = q; while (*p == ',' || *p == ' ') p++;
        }
        FILETIME c, x, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
        t_start = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
        state = trigger && (n > 0 || tfile);
        VK_LOG("PS3RECOMP_VK_RDOC_AT: %d capture time(s) armed%s\n", n, tfile ? ", trigger file watched" : "");
    }
    if (!state) return;
    /* PS3RECOMP_VK_RDOC_TRIGGER=<file>: a capture whenever that file appears (it is
     * deleted again), for a driver that decides by looking at the screen. */
    if (tfile && (++tick & 15u) == 0 && GetFileAttributesA(tfile) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(tfile);
        trigger();
        VK_LOG("RenderDoc capture triggered by %s\n", tfile);
    }
    if (next >= n) return;
    FILETIME f; GetSystemTimeAsFileTime(&f);
    const double now = (double)((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) - t_start) / 1e7;
    if (now >= at[next]) {
        trigger();
        VK_LOG("RenderDoc capture triggered at %.1f s (asked for %.1f s)\n", now, at[next]);
        while (next < n && now >= at[next]) next++;
    }
}
#endif

/* Blit the offscreen target into the next swapchain image and present it. */
static void vk_present_window_body(void);
/* The present records through postfx's and the overlay's own entry points (and
 * RenderDoc brackets it): with threaded recording, drain the worker and record
 * directly until it is done. */
static void vk_present_window(void)
{
    vk_tq_sync();
    const int was = s_tq.direct;
    s_tq.direct = 1;
    vk_present_window_body();
    s_tq.direct = was;
}
static void vk_present_window_body(void)
{
    if (!s_vk.windowed || !s_vk.swapchain || s_sc_minimized) return;
    if (s_present_dirty) {                  /* [overlay] present mode / fullscreen changed live */
        s_present_dirty = 0;
        vk_swapchain_lost();
        if (!s_vk.windowed || !s_vk.swapchain || s_sc_minimized) return;
    }

    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    /* This submit is made by hand (it waits on the acquire semaphore). An open
     * batch -- the frame itself -- rides along in it instead of paying its own
     * submit and wait; either way no batch is left marked open. */
    const int was_batch = s_batch.recording;
    if (was_batch) vk_flush_note(__LINE__, s_batch.draws);
    if (vk_begin()) return;                 /* batched: ends the open pass */
    s_batch.recording = 0;
    vk_gprof_stamp(GP_PGAP, s_vk.width, s_vk.height);
    vk_gprof_note_present();
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    /* [postfx] AA + CAS (vk_postfx.c) on the frame; the window gets its output */
    VkImage pf_img = s_vk.color.img;
    uint32_t pf_w = s_vk.width, pf_h = s_vk.height;
    if (!vk_postfx_apply(s_vk.cmd, s_vk.color.img, s_vk.color.view, s_vk.width, s_vk.height,
                         s_vk.sc_extent.width, s_vk.sc_extent.height, &pf_img, &pf_w, &pf_h)) {
        pf_img = s_vk.color.img; pf_w = s_vk.width; pf_h = s_vk.height;
    }
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)pf_w, (int32_t)pf_h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)s_vk.sc_extent.width, (int32_t)s_vk.sc_extent.height, 1 } },
    };
    pvkCmdBlitImage(s_vk.cmd, pf_img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    /* [overlay] the HUD / menu on top (vk_overlay.cpp): when it draws, its render
     * pass leaves the image in PRESENT_SRC itself */
    if (!(g_vk_overlay_active &&
          vk_overlay_draw(s_vk.cmd, idx, s_vk.sc_images[idx], s_vk.sc_extent, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)))
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0, 0, NULL, 0, NULL, 1, &b);
    vk_gprof_stamp(GP_BLIT, s_vk.sc_extent.width, s_vk.sc_extent.height);
    if (pvkEndCommandBuffer(s_vk.cmd) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.fence);
    if (pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n");
        if (was_batch) vk_batch_completed();   /* nothing in flight: free it anyway */
        return;
    }
    const int fif = vk_fif_on();
    if (fif) {
        s_fif_pending[s_fif_cur] = 1;          /* waited for when the slot comes round */
    } else {
        const uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
        pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX);
        if (ft0) ps3_ft_span(PS3_FT_GPU_WAIT, 0, 1u, s_batch.draws, ft0, ps3_ft_now());
        if (was_batch) vk_batch_completed();
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    vk_frame_limit();
#ifdef _WIN32
    vk_rdoc_poll();
#endif
    r = pvkQueuePresentKHR(s_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
    if (fif) vk_fif_advance();
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    FILE* f = fopen(s_vk.dump_path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    for (size_t i = 0; i < (size_t)s_vk.width * s_vk.height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;

    /* Batched and nobody asked for the pixels (PS3RECOMP_VK_DUMP, or
     * PS3RECOMP_VK_READBACK=1 for the centre-pixel debug value): skip the
     * full-frame copy to the CPU and its wait; the window present below
     * submits the frame. */
    { static int rb = -1;
      if (rb < 0) rb = getenv("PS3RECOMP_VK_READBACK") ? 1 : 0;
      if (s_batch.on > 0 && !s_vk.dump_path && !rb) {
          s_vk.presented++;
          s_g.last_draws = s_g.draws;
          s_g.draws = 0;
          if (vk_window_ready()) vk_present_window();
          else {
              vk_batch_flush();
              if (s_sc_minimized) vk_frame_limit();   /* minimized: keep the game's pace */
          }
          return;
      } }

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (vk_submit_and_wait()) return;

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
    if (s_vk.dump_path) vk_dump_ppm(px);
    vk_present_window();
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 2048          /* the engine's texture cache is 1024 */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u32      tag;                    /* E5: which cache made it, 0 = none   */
    u32      faces;                  /* TEXTURE: 6 = a cube (layers = faces) */
    u32      depth;                  /* TEXTURE: > 1 = a volume (3D image)  */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];

/* Defined with the E2 pipeline state and the E5 passes further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng2_shutdown(void* user);
static void vk_eng5_shutdown(void);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

static u32 vk_eng_alloc(void)
{
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_FREE) return i;
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Copy `rows` rows of `w` texels from host memory into one level / layer of
 * an image that is in GENERAL (slice `z` of a 3D image's level). */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes, u32 z)
{
    if (tight > row_bytes) tight = row_bytes;
    if (vk_batch_on()) {
        /* Batched: stage in the arena and leave the copy in the batch; it runs
         * after every draw recorded before it and before every draw after. */
        const int64_t so = vk_batch_stage((VkDeviceSize)tight * rows);
        if (so < 0) return -1;
        for (u32 y = 0; y < rows; y++)
            memcpy((u8*)s_batch.sarena.ptr + so + (size_t)y * tight,
                   (const u8*)src + (size_t)y * row_bytes, tight);
        if (vk_begin()) return -1;
        vk_barrier_all();
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .bufferOffset = (VkDeviceSize)so,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { 0, 0, (int32_t)z },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, s_batch.sarena.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return 0;
    }
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * rows, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !st.coherent) { vk_destroy_buffer(&st); return -1; }
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)st.ptr + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { 0, 0, (int32_t)z },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&st);
    return rc;
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes, 0);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
/* faces 6: a cube view over the six layers; otherwise a 2D view. */
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, u32 faces, VkComponentMapping map,
                            VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = faces == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, faces == 6 ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return s_vk.device ? 0 : -1;
}

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    s_eobj_gen++;
    if (s_eobj[handle].kind == VK_ENG_COLOR)
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                vk_destroy_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
    /* A depth target's snapshots are the engine's to release (E4). */
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers();
    vk_destroy_image(&s_eobj[handle].im);      /* idle: submissions are waited on */
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_shutdown(void* user)
{
    vk_batch_flush();
    vk_eng5_shutdown();
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;
    /* Unbatched every submission has already completed; batched, this is
     * where the engine needs the recorded work done. */
    vk_batch_flush();
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* E1: the render pass is built for R8G8B8A8, so that is the one colour
     * target format for now; FP16 HDR targets need their own pass. */
    if (vf != VK_FORMAT_R8G8B8A8_UNORM || !w || !h || w > 4096u || h > 4096u) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, vf, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    /* Batched, the transition and clear simply join the batch. */
    if (!s_batch.recording && vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    if (seed && seed_row_bytes)
        vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!w || !h || w > 4096u || h > 4096u) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_DS_ASPECT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w; o->h = h;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { VK_DS_ASPECT, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (!s_batch.recording && vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || vk_begin()) return;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (s_batch.recording) vk_barrier_all();
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    if (!s_batch.recording) vk_submit_and_wait();   /* batched: stays in the batch */
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags a = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) a |= (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!o || !a || vk_begin()) return;
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { a, 0, 1, 0, 1 };
    if (s_batch.recording) vk_barrier_all();
    vk_barrier_image(o->im.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    if (!s_batch.recording) vk_submit_and_wait();   /* batched: stays in the batch */
}

/* ---- PS3RECOMP_VK_RT_DUMP=<dir> [PS3RECOMP_VK_RT_DUMP_AT=<sec>] -------------
 * DIAGNOSTIC: every colour target one frame draws into, written as PPM in
 * first-draw order (rt_<order>_h<handle>_<w>x<h>.ppm, alpha as *_a.ppm) with a
 * list of draw counts (rt_list.txt) -- what a black picture is made of. The
 * frame is the first to start at least <sec> seconds (default 30) after the
 * first present; the presented surface is dumped too. */
static struct {
    int on; double at; unsigned long long t0; int phase;
    u32 rt[64], draws[64], first[64], n, seq; char dir[400];
} s_rtd = { .on = -1 };

/* Screen-sized sampled textures of the dumped frame (light buffers, AO,
 * depth-as-colour): what a composite reads, beside what the frame renders. */
static u32 s_rtd_tex[32], s_rtd_ntex;
static void vk_rtd_note_tex(const u32* tex, u32 mask)
{
    if (s_rtd.phase != 1) return;
    for (u32 u = 0; u < 16; u++) {
        if (!((mask >> u) & 1u) || !tex[u] || tex[u] >= 2048u) continue;
        const vk_eng_obj* o = &s_eobj[tex[u]];
        /* Views of surfaces and render targets, and plain 4-byte screen-sized
         * textures (light/AO buffers uploaded from guest memory); never the
         * compressed materials. */
        if (!(o->kind == VK_ENG_VIEW || o->kind == VK_ENG_COLOR ||
              (o->kind == VK_ENG_TEXTURE && o->bpp == 4 && o->w >= 640 && o->h >= 360))) continue;
        u32 k = 0;
        while (k < s_rtd_ntex && s_rtd_tex[k] != tex[u]) k++;
        if (k == s_rtd_ntex && s_rtd_ntex < 32) s_rtd_tex[s_rtd_ntex++] = tex[u];
    }
}

static void vk_rtd_note(const u32* rts, u32 n)
{
    if (s_rtd.phase != 1) return;
    for (u32 i = 0; i < n; i++) {
        u32 k = 0;
        while (k < s_rtd.n && s_rtd.rt[k] != rts[i]) k++;
        if (k == s_rtd.n) {
            if (s_rtd.n >= 64) continue;
            s_rtd.rt[k] = rts[i]; s_rtd.draws[k] = 0; s_rtd.first[k] = s_rtd.seq; s_rtd.n++;
        }
        s_rtd.draws[k]++;
    }
    s_rtd.seq++;
}

static void vk_rtd_write(u32 order, u32 h)
{
    if (!h || h >= VK_ENG_MAX_OBJ) return;
    const vk_eng_obj* o = &s_eobj[h];
    if (o->kind == VK_ENG_VIEW && o->surface && o->surface < VK_ENG_MAX_OBJ) o = &s_eobj[o->surface];
    if ((o->kind != VK_ENG_COLOR && o->kind != VK_ENG_TEXTURE) || !o->im.img || o->bpp != 4 ||
        !o->w || !o->h) return;
    vk_buffer b = {0};
    if (vk_create_host_buffer(&b, (VkDeviceSize)o->w * o->h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&b); return;
    }
    if (vk_begin()) { vk_destroy_buffer(&b); return; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { o->w, o->h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b.buf, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (vk_submit_and_wait()) { vk_destroy_buffer(&b); return; }
    vk_invalidate(&b);
    const u8* px = (const u8*)b.ptr;
    char path[512];
    for (int pass = 0; pass < 2; pass++) {
        snprintf(path, sizeof path, "%s/rt_%02u_h%u_%ux%u%s.ppm", s_rtd.dir, order, h, o->w, o->h,
                 pass ? "_a" : "");
        FILE* f = fopen(path, "wb");
        if (!f) continue;
        fprintf(f, "P6\n%u %u\n255\n", o->w, o->h);
        for (size_t i = 0; i < (size_t)o->w * o->h; i++) {
            const u8* q = px + i * 4;
            u8 rgb[3] = { q[0], q[1], q[2] };
            if (pass) rgb[0] = rgb[1] = rgb[2] = q[3];
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    vk_destroy_buffer(&b);
}

/* PS3RECOMP_VK_RT_DUMP_SEQ=<n>[,<n>..]: in the dumped frame, also read the
 * targets back right after draw number n (the first-draw numbers of
 * rt_list.txt) -- what one pass itself wrote, before later draws cover it.
 * Files rt_s<n>_<k>_h<handle>_*.ppm. */
static void vk_rtd_write(u32 order, u32 h);
static void vk_rtd_after_draw(const u32* rts, u32 n)
{
    if (s_rtd.phase != 1) return;
    static int init; static u32 seqs[8]; static u32 nseq;
    if (!init) { init = 1;
        const char* e = getenv("PS3RECOMP_VK_RT_DUMP_SEQ");
        for (const char* p = e; p && *p && nseq < 8;) {
            char* q; const unsigned long v = strtoul(p, &q, 10);
            if (q == p) break;
            seqs[nseq++] = (u32)v; p = *q == ',' ? q + 1 : q;
        } }
    const u32 this_draw = s_rtd.seq - 1u;      /* vk_rtd_note already counted it */
    for (u32 i = 0; i < nseq; i++)
        if (seqs[i] == this_draw) {
            for (u32 k = 0; k < n; k++) vk_rtd_write(1000u * (this_draw + 1u) + k, rts[k]);
            VK_LOG("RT dump: targets after draw %u\n", this_draw);
        }
    /* PS3RECOMP_VK_RT_DUMP_FIRST=<handle>:<count>: the target after each of
     * the first <count> draws into it (a pass's own output, whatever its draw
     * number this run). Files rt_f<handle>_<k>_d<draw>... via order 2000000+. */
    static long fh = -2; static u32 fcount, fdone;
    if (fh == -2) { const char* e = getenv("PS3RECOMP_VK_RT_DUMP_FIRST");
                    fh = e ? strtol(e, NULL, 10) : -1;
                    const char* c = e ? strchr(e, ':') : NULL; fcount = c ? (u32)atoi(c + 1) : 3u; }
    if (fh > 0 && fdone < fcount)
        for (u32 k = 0; k < n; k++)
            if (rts[k] == (u32)fh) {
                vk_rtd_write(2000000u + fdone * 1000u + this_draw, rts[k]);
                VK_LOG("RT dump: target %ld after its draw %u (frame draw %u)\n", fh, fdone, this_draw);
                fdone++;
                break;
            }
}

/* Called at every present, before it: arms one frame, then dumps it. */
static void vk_rtd_frame(u32 presented)
{
    if (s_rtd.on < 0) {
        const char* d = getenv("PS3RECOMP_VK_RT_DUMP");
        const char* a = getenv("PS3RECOMP_VK_RT_DUMP_AT");
        s_rtd.on = (d && *d) ? 1 : 0;
        if (s_rtd.on) snprintf(s_rtd.dir, sizeof s_rtd.dir, "%s", d);
        s_rtd.at = a ? atof(a) : 30.0;
        s_rtd.t0 = SDL_GetTicks64();
    }
    if (!s_rtd.on || s_rtd.phase == 2) return;
    if (s_rtd.phase == 0) {
        if ((double)(SDL_GetTicks64() - s_rtd.t0) >= s_rtd.at * 1000.0) { s_rtd.phase = 1; s_rtd.n = s_rtd.seq = 0; }
        return;
    }
    s_rtd.phase = 2;
    vk_batch_flush();
    char path[512];
    snprintf(path, sizeof path, "%s/rt_list.txt", s_rtd.dir);
    FILE* f = fopen(path, "w");
    for (u32 k = 0; k < s_rtd.n; k++) {
        const vk_eng_obj* o = vk_eng_get(s_rtd.rt[k], VK_ENG_COLOR);
        if (f) fprintf(f, "%02u handle %u %ux%u draws %u first-draw %u\n", k, s_rtd.rt[k],
                       o ? o->w : 0, o ? o->h : 0, s_rtd.draws[k], s_rtd.first[k]);
        vk_rtd_write(k, s_rtd.rt[k]);
    }
    for (u32 k = 0; k < s_rtd_ntex; k++) {
        const u32 t = s_rtd_tex[k];
        const vk_eng_obj* o = &s_eobj[t];
        if (f) fprintf(f, "tex %02u handle %u kind %d %ux%u bpp %u", 40 + k, t, (int)o->kind, o->w, o->h, o->bpp);
        if (f && o->kind == VK_ENG_VIEW) fprintf(f, " (view of surface %u)", o->surface);
        if (f) fprintf(f, "\n");
        if (o->w >= 320) vk_rtd_write(40 + k, t);   /* screen-sized only: light/AO/depth buffers */
    }
    if (f) { fprintf(f, "presented handle %u, %u draws in the frame\n", presented, s_rtd.seq); fclose(f); }
    vk_rtd_write(99, presented);
    VK_LOG("RT dump: %u colour targets of one frame -> %s\n", s_rtd.n, s_rtd.dir);
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    vk_rtd_frame(surface);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (o && !vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        };
        pvkCmdBlitImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                        (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST
                                                                    : VK_FILTER_LINEAR);
        if (!s_batch.recording) vk_submit_and_wait();   /* batched: vk_cb_present submits */
    }
    vk_cb_present(&s_vk, 0);
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->w || y + h > o->h) return;
    const size_t tight = (size_t)w * o->bpp;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return;
    }
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_submit_and_wait()) {
            vk_invalidate(&st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st.ptr + (size_t)row * tight, tight);
        }
    }
    vk_destroy_buffer(&st);
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. Cube maps are not handled yet:
 * the pixel decompiler declares cube units as separate bindings, which the
 * shared set layout does not have, so faces == 6 reports 0 (placeholder). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > 4096u || h > 4096u || (bc && !s_vk.bc_ok))
        return 0;
    /* A cube: six square layers, one per face (+X -X +Y -Y +Z -Z, the RSX's
     * order and Vulkan's), sampled through a CUBE view. Without them, units a
     * program declares TextureCube had nothing to read -- inFAMOUS 2's
     * deferred composite looks its ambient and reflection up in cubes, and a
     * missing one turned the whole lit world black (NaN -> 0). */
    if (faces != 1 && (faces != 6 || w != h)) {
        static int warned_faces;
        if (!warned_faces) { VK_LOG("engine: %u-face %ux%u texture not supported\n", faces, w, h); warned_faces = 1; }
        return 0;
    }
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .flags = faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = faces,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, faces, vk_remap(remap, rsx_fmt), &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->faces = faces;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level before the uploads arrive
     * (batched: the transition joins the batch, ahead of those uploads). */
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!s_batch.recording && vk_submit_and_wait()) goto fail;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face >= (o->faces ? o->faces : 1u)) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes, 0);   /* face = layer */
}

/* 3D textures (SET_TEXTURE_FORMAT dimension 3, CONTROL3 depth): one 3D image
 * of `depth` slices per level, uploaded a slice at a time, sampled through a
 * 3D view (the pixel decompiler declares those units Texture3D, one binding
 * each). Without these hooks a volume was its first slice sampled in 2D: for
 * inFAMOUS 2's occlusion volumes (an 8x8x8 cone LUT) that darkened whole boxes
 * round props, as it did on Metal before its own 3D textures. */
static u32 vk_eng_texture_create_3d(void* user, rsx_be_format fmt, u32 w, u32 h,
                                    u32 depth, u32 mips, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || bc || !w || !h || !depth || w > 2048u || h > 2048u || depth > 2048u)
        return 0;
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D,
        .format = vf, .extent = { w, h, depth }, .mipLevels = mips, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->im.img,
        .viewType = VK_IMAGE_VIEW_TYPE_3D, .format = vf, .components = vk_remap(remap, rsx_fmt),
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 } };
    if (pvkCreateImageView(s_vk.device, &vci, NULL, &o->im.view) != VK_SUCCESS) goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->faces = 1; o->depth = depth;
    o->bpp = bpp; o->block = 0;
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!s_batch.recording && vk_submit_and_wait()) goto fail;
    { static int said; if (!said++) VK_LOG("engine: 3D textures in use (first %ux%ux%u, %u level(s))\n", w, h, depth, mips); }
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload_3d(void* user, u32 texture, u32 z, u32 mip, u32 w, u32 h,
                                     const void* src, u32 row_bytes)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || o->depth < 2 || !src || !row_bytes) return;
    vk_eng_upload_rows(o->im.img, 0, mip, w, h, (size_t)w * o->bpp, h, src, row_bytes, z);
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
            s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
            return i;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, 1, vk_remap(remap, rsx_format), &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    return hd;
}
/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Every request makes a NEW texture, as Metal's does, and the engine owns it:
 * each caller (eng_zdepth_snapshot, the crop, the GPU light path's depth
 * input) releases the copy it held once a newer one arrives, and the light
 * path does so without comparing handles. One texture per depth target,
 * refilled in place, was released by that very call and the light pass then
 * read a freed (or reused) slot; it also let a later refill overwrite the
 * depth the light pass was handed, which on Metal stays as it was at the
 * blit. */

/* Copy the top-left w x h of depth target `z` into the R32_SFLOAT image `dst`
 * (which is in `dst_layout`, UNDEFINED for a fresh one) and wait. */
static int vk_eng_copy_depth(const vk_eng_obj* z, VkImage dst, VkImageLayout dst_layout, u32 w, u32 h)
{
    if (vk_batch_on()) {
        /* Batched: the depth -> buffer -> colour bounce goes through the
         * GPU-local bounce arena and stays in the batch (GPU-only, nothing to
         * wait for). It went through the host-visible staging arena: two PCIe
         * crossings, ~1.2 ms of every depth decode on an RTX 2070 Super. */
        const VkDeviceSize bytes = (VkDeviceSize)w * h * 4u;
        const int64_t so = vk_batch_gstage(bytes);
        if (so < 0 || vk_begin()) return -1;
        vk_barrier_all();
        vk_barrier_image(z->im.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(dst, VK_IMAGE_ASPECT_COLOR_BIT, dst_layout);
        VkBufferImageCopy out = { .bufferOffset = (VkDeviceSize)so,
                                  .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, s_batch.garena.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = s_batch.garena.buf, .offset = (VkDeviceSize)so, .size = bytes };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .bufferOffset = (VkDeviceSize)so,
                                 .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, s_batch.garena.buf, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        return 0;
    }
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)w * h * 4u,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(dst, VK_IMAGE_ASPECT_COLOR_BIT, dst_layout);
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&st);
    return rc;
}

/* Copy the top-left w x h of depth target `z`'s STENCIL aspect (one byte a
 * texel) into the R8 image `dst` (in `dst_layout`). Batched: through the
 * staging arena, in the batch; unbatched: a host buffer, waited on. */
static int vk_eng_copy_stencil(const vk_eng_obj* z, VkImage dst, VkImageLayout dst_layout, u32 w, u32 h)
{
    if (!(VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT)) return -1;
    const VkDeviceSize bytes = ((VkDeviceSize)w * h + 3u) & ~(VkDeviceSize)3u;
    vk_buffer st = {0};
    VkBuffer buf; VkDeviceSize off = 0;
    const int batched = vk_batch_on();
    if (batched) {
        const int64_t so = vk_batch_gstage(bytes);   /* GPU-local: see vk_eng_copy_depth */
        if (so < 0) return -1;
        buf = s_batch.garena.buf; off = (VkDeviceSize)so;
    } else {
        if (vk_create_host_buffer(&st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&st); return -1;
        }
        buf = st.buf;
    }
    if (vk_begin()) { vk_destroy_buffer(&st); return -1; }
    if (batched) vk_barrier_all();
    vk_barrier_image(z->im.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(dst, VK_IMAGE_ASPECT_COLOR_BIT, dst_layout);
    VkBufferImageCopy out = { .bufferOffset = off,
                              .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
                              .imageExtent = { w, h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &out);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf, .offset = off, .size = bytes };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 1, &bb, 0, NULL);
    VkBufferImageCopy in = { .bufferOffset = off,
                             .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { w, h, 1 } };
    pvkCmdCopyBufferToImage(s_vk.cmd, buf, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
    int rc = 0;
    if (!batched) rc = vk_submit_and_wait();
    vk_destroy_buffer(&st);
    return rc;
}

static int vk_eng_depth_copyable(void)
{
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT ||
        s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT) return 1;
    static int warned_fmt;
    if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                              (int)s_vk.depth_format); warned_fmt = 1; }
    return 0;
}

static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !vk_eng_depth_copyable()) return 0;
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;

    const u32 snap = vk_eng_alloc();
    if (!snap) return 0;
    vk_eng_obj* t = &s_eobj[snap];
    if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
    }
    t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h;
    if (vk_eng_copy_depth(z, t->im.img, VK_IMAGE_LAYOUT_UNDEFINED, w, h)) {
        vk_eng_release(user, snap); return 0;
    }
    return snap;
}
/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), FP16 and other
 * non-RGBA8 colour targets, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine keeps (ENG_MAX_PIPELINES): it never releases a
 * pipeline in play, and inFAMOUS 2 has used 512 by the time its first level
 * loads -- at 512 every later pipeline failed silently and the level's
 * draws were dropped ("pipeline"), leaving a black world. */
#define VK_ENG_MAX_PIPE   8192
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    /* By topology, then depth clamp (bit 0) and depth bias (bit 1): both
     * are pipeline state in Vulkan, encoder state in the engine. */
    VkPipeline          variant[VK_ENG_TOPOLOGIES][4];
    /* Units the fragment program declares TextureCube / Texture3D: a sampled
     * image's view type has to match the declaration, bound or not. */
    u32                 cube_mask, vol_mask;
    /* FUNC_ADD_SIGNED emulation (vk_eng_sblend_hlsl): lanes that blend signed
     * (bit 0 RGB, bit 1 A), the fragment module of the negative half and its
     * pipelines (REVERSE_SUBTRACT, drawn after the positive half). */
    u32                 sblend;
    VkShaderModule      fs_neg;
    VkPipeline          variant_neg[VK_ENG_TOPOLOGIES][4];
} vk_eng_pipe;

/* Bit u set for each "<kind> rsx_tex<u>" declaration in the program text. */
static u32 vk_eng_decl_mask(const char* hlsl, const char* kind)
{
    u32 m = 0;
    const size_t kl = strlen(kind);
    for (const char* p = hlsl; p && (p = strstr(p, kind)); p += kl) {
        const char* q = p + kl;
        while (*q == ' ') q++;
        if (strncmp(q, "rsx_tex", 7)) continue;
        const unsigned u = (unsigned)strtoul(q + 7, NULL, 10);
        if (u < 32) m |= 1u << u;
    }
    return m;
}

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

static struct {
    VkRenderPass rp[RSX_BE_MAX_COLOR_TARGETS + 1];      /* by colour count */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    /* bind_vs_constants_range, batched: the rows the program reads (pointer
     * into the engine's constant file, valid until the draw), and the tail */
    int          vs_ranged;
    const u8*    vs_rows;
    u32          vs_first, vs_count, vs_tail_off, vs_tail_bytes;
    u8           vs_tail[64];
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    int          depth_clamp;                          /* set_depth_clamp */
    float        bias_const, bias_slope;               /* set_depth_bias  */
    vk_buffer    indices;
    u32          warned;
} s_e2;

#define VK_ENG_SAMP_CACHE 256   /* [postfx] room for samplers of a live anisotropy change */
static struct { u64 key; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;

static void vk_eng_drop_framebuffers(void)
{
    /* Recorded draws may use any of them: retire them with the batch. The
     * open pass is one of them, so it ends here (the next draw opens anew). */
    if (s_e2.fb_count && s_batch.recording) vk_batch_end_pass();
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].fb) vk_batch_retire_fb(s_e2.fb[i].fb);
    s_e2.fb_count = 0;
}

/* The render pass for `n` RGBA8 colour attachments plus depth, LOAD/STORE in
 * GENERAL like the vtable path's: clears are separate operations. */
static VkRenderPass vk_eng_render_pass(u32 n)
{
    if (n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp[n]) return s_e2.rp[n];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? VK_FORMAT_R8G8B8A8_UNORM : s_vk.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = (i < n) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = (i < n) ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp[n]) != VK_SUCCESS)
        s_e2.rp[n] = VK_NULL_HANDLE;
    return s_e2.rp[n];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8006: return VK_BLEND_OP_ADD;                  /* FUNC_ADD */
    default: {
        static u32 seen[8]; static u32 ns;
        u32 k = 0; while (k < ns && seen[k] != e) k++;
        if (k == ns && ns < 8) { seen[ns++] = e;
            VK_LOG("blend equation 0x%04X not mapped: drawn as FUNC_ADD\n", e); }
        return VK_BLEND_OP_ADD; }
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out)
{
    const uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    u32 words = 0;
    int disk = 0, rc = -1;
    if (vk_sc_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log,
                            &disk) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
    } else {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
        rc = pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
    }
    if (ft0) ps3_ft_span(PS3_FT_COMPILE, stage == RSX_SHADER_STAGE_VERTEX ? 1u : 2u,
                         rc == 0 ? 1u : 0u, disk ? 1u : 0u, ft0, ps3_ft_now());
    return rc;
}

/* NV4097_SET_BLEND_EQUATION FUNC_ADD_SIGNED (0xF006, per half: RGB [0:15],
 * A [16:31]): the destination plus the source taken as a SIGNED byte -- the
 * 8-bit value the fragment wrote, read as two's complement (128..255 are
 * -128..-1). inFAMOUS 2's heat distortion is built on it: the map is cleared to
 * 0x80 (no offset) and each distorting fragment (the Beast's FP
 * d8160c63900ef8ce, fire, explosions) writes its offset as x for x >= 0 and
 * 1 + x for x < 0, with ONE/ONE; the composite then reads the map back
 * unsigned around 0.5. Drawn as FUNC_ADD (Vulkan has no signed blend; RPCS3
 * does the same), every negative offset saturates the map at 1.0 instead --
 * half of each distortion turns into a +0.5 shift, the Beast's body tears
 * into horizontal strips. Vulkan clamps a UNORM source to [0,1] before
 * blending, so the draw is split in two: the positive half as written with
 * FUNC_ADD (dst x Fd + max(s, 0) x Fs), then the negative half again with
 * REVERSE_SUBTRACT (dst x ONE - max(-s, 0) x Fs) and only the signed lanes
 * enabled. Exact unless a lane saturates half-way; factors that read the
 * split source itself keep FUNC_ADD (vk_eng_pipeline_create says which).
 * The game sets 0xF006 for both halves (ONE/ONE; and SRC_ALPHA/ONE with
 * alpha ZERO/ONE) -- the semantics are inferred from the encoding its
 * programs write, which only adds up this way.
 * PS3RECOMP_VK_SIGNED_BLEND=1 (default): the ONE/ONE case; =2: every
 * factor combination the split can do; =0: FUNC_ADD, as before. */
static int vk_sblend_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_SIGNED_BLEND"); on = e ? atoi(e) : 1; }
    return on;
}
/* The fragment program with its single-target return wrapped: neg = 0 keeps
 * the non-negative part of each signed lane, neg = 1 the magnitude of the
 * negative part. NULL when the program's return is not one this knows. */
static char* vk_eng_sblend_hlsl(const char* ps_hlsl, u32 lanes, int neg)
{
    static const char* const forms[2] = { "    return (_o == _o) ? _o : (float4)0;\n}",
                                          "    return _rsx_out;\n}" };
    const char* at = NULL; const char* form = NULL;
    for (int i = 0; i < 2 && !at; i++) { at = strstr(ps_hlsl, forms[i]); form = forms[i]; }
    const char* main_at = strstr(ps_hlsl, "float4 main(");
    if (!at || !main_at || main_at > at || strstr(ps_hlsl, "struct PSOutput")) return NULL;
    char fn[640];
    snprintf(fn, sizeof fn,
             "float4 _rsx_sblend(float4 v) {\n"
             "    float4 b = round(saturate(v) * 255.0);\n"
             "    float4 s = (b >= 128.0 ? b - 256.0 : b) / 255.0;   /* the byte, two's complement */\n"
             "    float4 r = %s;\n"
             "    return float4(%s, %s);\n"
             "}\n",
             neg ? "max(-s, (float4)0)" : "max(s, (float4)0)",
             (lanes & 1u) ? "r.xyz" : "v.xyz", (lanes & 2u) ? "r.w" : "v.w");
    const char* expr = form == forms[0] ? "(_o == _o) ? _o : (float4)0" : "_rsx_out";
    char ret[128];
    snprintf(ret, sizeof ret, "    return _rsx_sblend(%s);\n}", expr);
    const size_t n = strlen(ps_hlsl) + strlen(fn) + strlen(ret) + 1;
    char* out = (char*)malloc(n);
    if (!out) return NULL;
    size_t k = 0;
    memcpy(out + k, ps_hlsl, (size_t)(main_at - ps_hlsl)); k += (size_t)(main_at - ps_hlsl);
    memcpy(out + k, fn, strlen(fn)); k += strlen(fn);
    memcpy(out + k, main_at, (size_t)(at - main_at)); k += (size_t)(at - main_at);
    memcpy(out + k, ret, strlen(ret)); k += strlen(ret);
    strcpy(out + k, at + strlen(form));
    return out;
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    if (rt_fmt != RSX_BE_FMT_R8G8B8A8) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        static int said;
        if (!said++) VK_LOG("engine: all %u pipeline slots in use -- new pipelines fail, their draws are dropped\n",
                            (unsigned)VK_ENG_MAX_PIPE);
        return 0;
    }
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    /* FUNC_ADD_SIGNED lanes (see vk_eng_sblend_hlsl). A lane whose source
     * factor is ZERO adds nothing and needs no split. The split changes the
     * source the factors see, so it is refused (FUNC_ADD, said a few times)
     * when a split RGB is its own factor (SRC_COLOR) or a split alpha feeds
     * any factor (SRC_ALPHA...). inFAMOUS 2 uses ONE/ONE on both halves (the
     * heat-distortion maps: checked against the game's look in the Beast
     * fight) and SRC_ALPHA/ONE with alpha ZERO/ONE (some particles: whether
     * those darken on hardware is not checked yet). PS3RECOMP_VK_SIGNED_BLEND:
     * 1 (default) = the ONE/ONE case only, 2 = every case the split can do,
     * 0 = none (FUNC_ADD everywhere, as before). */
    u32 sbl = 0;
    const int sbl_mode = vk_sblend_on();
    if (rs->blend_enable && sbl_mode) {
        int srgb = rs->eq_rgb == 0xF006u && rs->sf_rgb != 0u, sa = rs->eq_a == 0xF006u && rs->sf_a != 0u;
        if (sbl_mode == 1 && ((srgb && (rs->sf_rgb != 1u || rs->df_rgb != 1u)) ||
                              (sa && (rs->sf_a != 1u || rs->df_a != 1u)))) {
            static int said;
            if (said++ < 4) VK_LOG("engine: FUNC_ADD_SIGNED with factors 0x%X/0x%X 0x%X/0x%X: drawn as FUNC_ADD "
                                   "(PS3RECOMP_VK_SIGNED_BLEND=2 splits it)\n", rs->sf_rgb, rs->df_rgb, rs->sf_a, rs->df_a);
            srgb = sa = 0;
        }
        #define VK_SRC_COLOR_F(f) ((f) == 0x300u || (f) == 0x301u)
        #define VK_SRC_ALPHA_F(f) ((f) == 0x302u || (f) == 0x303u || (f) == 0x308u)
        const int bad = (srgb && (VK_SRC_COLOR_F(rs->sf_rgb) || VK_SRC_COLOR_F(rs->df_rgb))) ||
                        (sa && (VK_SRC_ALPHA_F(rs->sf_rgb) || VK_SRC_ALPHA_F(rs->df_rgb) ||
                                VK_SRC_ALPHA_F(rs->sf_a) || VK_SRC_ALPHA_F(rs->df_a) ||
                                VK_SRC_COLOR_F(rs->sf_a) || VK_SRC_COLOR_F(rs->df_a)));
        #undef VK_SRC_COLOR_F
        #undef VK_SRC_ALPHA_F
        if (bad) {
            static int said;
            if (said++ < 4) VK_LOG("engine: FUNC_ADD_SIGNED with factors 0x%X/0x%X 0x%X/0x%X: drawn as FUNC_ADD\n",
                                   rs->sf_rgb, rs->df_rgb, rs->sf_a, rs->df_a);
        } else {
            sbl = (srgb ? 1u : 0u) | (sa ? 2u : 0u);
        }
    }
    char* ps_pos = sbl ? vk_eng_sblend_hlsl(ps_hlsl, sbl, 0) : NULL;
    char* ps_neg = sbl ? vk_eng_sblend_hlsl(ps_hlsl, sbl, 1) : NULL;
    if (sbl && (!ps_pos || !ps_neg)) {
        static int said;
        if (!said++) VK_LOG("engine: FUNC_ADD_SIGNED on a program whose output is not a single float4: drawn as FUNC_ADD\n");
        free(ps_pos); free(ps_neg); ps_pos = ps_neg = NULL; sbl = 0;
    }
    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs)) { free(ps_pos); free(ps_neg); return 0; }
    if (vk_eng_translate(ps_pos ? ps_pos : ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs) ||
        (ps_neg && vk_eng_translate(ps_neg, RSX_SHADER_STAGE_FRAGMENT, &P->fs_neg))) {
        if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
        pvkDestroyShaderModule(s_vk.device, P->vs, NULL); memset(P, 0, sizeof *P);
        free(ps_pos); free(ps_neg); return 0;
    }
    free(ps_pos); free(ps_neg);
    P->sblend = sbl;
    if (sbl) { static int said; if (!said++) VK_LOG("engine: FUNC_ADD_SIGNED blending emulated (lanes 0x%X)\n", sbl); }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->cube_mask = vk_eng_decl_mask(ps_hlsl, "TextureCube");
    P->vol_mask  = vk_eng_decl_mask(ps_hlsl, "Texture3D");
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    vk_batch_flush();                       /* a recorded draw may still bind it */
    vk_fif_wait_other();                    /* ... or the other slot's frame */
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        for (u32 k = 0; k < 4; k++)
            if (P->variant[t][k]) pvkDestroyPipeline(s_vk.device, P->variant[t][k], NULL);
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        for (u32 k = 0; k < 4; k++)
            if (P->variant_neg[t][k]) pvkDestroyPipeline(s_vk.device, P->variant_neg[t][k], NULL);
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    if (P->fs_neg) pvkDestroyShaderModule(s_vk.device, P->fs_neg, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant_ex(vk_eng_pipe* P, u32 topo_idx, u32 vk_idx, int neg);
static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx, u32 vk_idx)
{
    return vk_eng_variant_ex(P, topo_idx, vk_idx, 0);
}
/* neg: the negative half of a FUNC_ADD_SIGNED draw (vk_eng_sblend_hlsl). */
static VkPipeline vk_eng_variant_ex(vk_eng_pipe* P, u32 topo_idx, u32 vk_idx, int neg)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipeline* slotp = neg ? &P->variant_neg[topo_idx][vk_idx] : &P->variant[topo_idx][vk_idx];
    if (*slotp) return *slotp;
    if (neg && !P->fs_neg) return VK_NULL_HANDLE;
    VkRenderPass rp = vk_eng_render_pass(P->rt_count);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = neg ? P->fs_neg : P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .depthClampEnable = (vk_idx & 1u) ? VK_TRUE : VK_FALSE,
        .depthBiasEnable = (vk_idx & 2u) ? VK_TRUE : VK_FALSE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back };
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Every attachment takes target A's blend and mask (see rsx_draw_engine.h). */
    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    /* Targets B..D of an MRT set take their own write mask from COLOR_MASK_MRT
     * (0x0370: a nibble per target at [4i..4i+3], bit 0 A, 1 R, 2 G, 3 B) and
     * their own blend enable from BLEND_ENABLE_MRT (0x036C bit i) -- the
     * hardware rule, as rsx_metal_backend.m applies it. Giving every target
     * A's mask let inFAMOUS 2's G-buffer pass overwrite channels of planes B
     * and C that it masks off (and leave stale the ones it writes), and the
     * composite multiplies its lighting by those channels.
     * PS3RECOMP_VK_MRT_MASK=0: target A's state everywhere (old behaviour). */
    static int mrt_mode = -1;
    if (mrt_mode < 0) { const char* e = getenv("PS3RECOMP_VK_MRT_MASK"); mrt_mode = e ? atoi(e) : 1; }
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++) {
        VkColorComponentFlags twm = wm;
        int tblend = rs->blend_enable != 0;
        if (r > 0 && mrt_mode && s_vk_indep_blend) {
            const u32 nib = (rs->color_mask_mrt >> (r * 4)) & 0xFu;
            twm = 0;
            if (nib & 1u) twm |= VK_COLOR_COMPONENT_A_BIT;
            if (nib & 2u) twm |= VK_COLOR_COMPONENT_R_BIT;
            if (nib & 4u) twm |= VK_COLOR_COMPONENT_G_BIT;
            if (nib & 8u) twm |= VK_COLOR_COMPONENT_B_BIT;
            tblend = (rs->blend_mrt >> r) & 1u;
        }
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = tblend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = (P->sblend & 1u) ? (neg ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD)
                                             : vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = (P->sblend & 2u) ? (neg ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD)
                                             : vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = twm };
        /* The negative half writes only the signed lanes (the others were
         * drawn, once, by the positive half), and subtracts from what the
         * positive half left: dst x ONE - neg x Fs, so dst x Fd is applied
         * once and the sum is dst x Fd + (pos - neg) x Fs. */
        if (neg) {
            cba[r].colorWriteMask &= ((P->sblend & 1u) ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                          VK_COLOR_COMPONENT_B_BIT) : 0u) |
                                     ((P->sblend & 2u) ? VK_COLOR_COMPONENT_A_BIT : 0u);
            if (P->sblend & 1u) cba[r].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            if (P->sblend & 2u) cba[r].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (vk_idx & 2u) ? 4u : 3u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_g.pipe_layout, .renderPass = rp, .subpass = 0 };
    if (vk_sc_create_pipeline(&gci, slotp, 0) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        *slotp = VK_NULL_HANDLE;
    }
    return *slotp;
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
        if (s_e2.rp[n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[n], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Constants go straight into the shared UBO: vertex at 0, pixel at ps_off.
 * Unbatched, every operation is waited on, so overwriting per draw is safe.
 * Batched, the same bytes also land in a CPU shadow of the whole file, which
 * each recorded draw copies into the uniform arena -- the whole file, so what
 * a draw sees past the bytes bound last is what the shared UBO would hold. */
static u8* vk_batch_shadow(void)
{
    if (!s_batch.shadow && s_g.ubo.size) {
        s_batch.shadow = (u8*)calloc(1, (size_t)s_g.ubo.size);
        s_batch.shadow_size = s_batch.shadow ? (size_t)s_g.ubo.size : 0;
    }
    return s_batch.shadow;
}
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_bytes = 0; return; }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    /* Batched, draws read the shadow (copied into the uniform arena per draw);
     * the shared UBO is the unbatched path's only. */
    if (vk_batch_on() && vk_batch_shadow()) memcpy(s_batch.shadow, data, bytes);
    else memcpy(s_g.ubo.ptr, data, bytes);
    s_e2.vs_bytes = bytes;
    s_e2.vs_ranged = 0;
}
/* rsx_draw_backend.bind_vs_constants_range (Metal: eng_bind_vs_constants_range):
 * only rows [first, first + count) and the viewport tail at the end of the
 * `total_bytes` block are read by the bound program. bind_vs_constants copied
 * the whole 8 KB file three times a draw (the engine's block, this shadow, the
 * draw's arena copy): 52 MB a frame in the Empire City skyline (~6000 draws).
 * Batched, the rows stay where they are until the draw copies them into its
 * own window of the uniform arena (vk_eng_draw_ex); unbatched, they go straight
 * into the shared UBO. */
static void vk_eng_bind_vs_constants_range(void* user, const void* rows, u32 first, u32 count,
                                           const void* tail, u32 tail_bytes, u32 total_bytes)
{
    (void)user;
    s_e2.vs_ranged = 0;
    if (!s_g.on || !rows) { s_e2.vs_bytes = 0; return; }
    if (total_bytes > (u32)s_g.ps_off || tail_bytes > sizeof s_e2.vs_tail || tail_bytes > total_bytes ||
        first > total_bytes / 16u || count > total_bytes / 16u - first ||
        (first + count) * 16u > total_bytes - tail_bytes) {
        if (!(s_e2.warned & 0x100u)) { VK_LOG("engine: VS constant range %u+%u of %u B rejected\n", first, count, total_bytes); s_e2.warned |= 0x100u; }
        s_e2.vs_bytes = 0;
        return;
    }
    const u32 tail_off = total_bytes - tail_bytes;
    if (vk_batch_on() && vk_batch_shadow()) {
        s_e2.vs_ranged = 1;
        s_e2.vs_rows = (const u8*)rows;
        s_e2.vs_first = first; s_e2.vs_count = count;
        s_e2.vs_tail_off = tail_off; s_e2.vs_tail_bytes = tail_bytes;
        if (tail_bytes) memcpy(s_e2.vs_tail, tail, tail_bytes);
    } else {
        if (count) memcpy((u8*)s_g.ubo.ptr + first * 16u, (const u8*)rows + first * 16u, count * 16u);
        if (tail_bytes) memcpy((u8*)s_g.ubo.ptr + tail_off, tail, tail_bytes);
    }
    s_e2.vs_bytes = total_bytes;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    if (vk_batch_on() && vk_batch_shadow()) memcpy(s_batch.shadow + s_g.ps_off, data, bytes);
    else memcpy((u8*)s_g.ubo.ptr + s_g.ps_off, data, bytes);
    s_e2.ps_bytes = bytes;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user; (void)textures; (void)samplers;
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures not supported yet\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }
/* POLYGON_OFFSET: `constant` in units of the guest depth's resolution
 * (2^-24, D24). The depth image is D32_SFLOAT, whose constant unit is
 * 2^(e-23) for the primitive's largest exponent e: the same 2^-24 for depths
 * in [0.5, 1), half that per octave below. */
static void vk_eng_set_depth_bias(void* user, float constant, float slope)
{ (void)user; s_e2.bias_const = constant; s_e2.bias_slope = slope; }
/* ZMIN_MAX_CONTROL: 1 = clamp depth to the viewport range instead of
 * clipping at the near/far planes (needs the depthClamp feature). */
static void vk_eng_set_depth_clamp(void* user, int clamp) { (void)user; s_e2.depth_clamp = clamp; }

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_DS_ASPECT) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { VK_DS_ASPECT, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

static int vk_eng_ensure_indices(VkDeviceSize bytes)
{
    if (s_e2.indices.buf && s_e2.indices.size >= bytes) return 0;
    VkDeviceSize size = s_e2.indices.size ? s_e2.indices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_e2.indices);
    if (vk_create_host_buffer(&s_e2.indices, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    return s_e2.indices.coherent ? 0 : -1;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    /* [postfx] PS3RECOMP_ANISO: only a linear minification through mipmaps */
    const u32 aniso = (u32)vk_postfx_sampler_aniso(d->min_linear, d->mip_present) & 0x1Fu;
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32)
                  | ((u64)aniso << 48);    /* [postfx] a new level keys new samplers */
    for (u32 i = 0; i < s_esamp_count; i++) if (s_esamp[i].key == key) return s_esamp[i].s;
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod,
        .anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE, .maxAnisotropy = aniso > 1 ? (float)aniso : 1.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].key = key;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view. */
static VkImageView vk_eng_sample_view(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == VK_ENG_TEXTURE || o->kind == VK_ENG_COLOR || o->kind == VK_ENG_VIEW)
               ? o->im.view : VK_NULL_HANDLE;
}

static void vk_eng_write_descriptors_to(VkDescriptorSet set, VkBuffer ubuf, VkDeviceSize ubase);
static void vk_eng_write_descriptors(void)
{
    vk_eng_write_descriptors_to(s_g.set, s_g.ubo.buf, 0);
}
/* The guest set layout's four bindings, with the constant file at `ubase` of
 * `ubuf` (the shared UBO at 0, or a draw's copy in the batch's arena). */
/* 1x1 black stand-ins of the other view types: a unit declared TextureCube or
 * Texture3D must be given a view of that type even when nothing usable is
 * bound -- a 2D view there is undefined, and on this driver it read as NaN. */
static vk_image s_dummy_cube, s_dummy_vol;
static VkImageView vk_eng_dummy_view(int vol)
{
    vk_image* im = vol ? &s_dummy_vol : &s_dummy_cube;
    if (im->view) return im->view;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = vol ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .imageType = vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 1, 1, 1 }, .mipLevels = 1,
        .arrayLayers = vol ? 1u : 6u, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req; u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &im->img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, im->img, im->mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = vol ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_CUBE,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, vol ? 1u : 6u } };
    if (pvkCreateImageView(s_vk.device, &vci, NULL, &im->view) != VK_SUCCESS) goto fail;
    if (vk_begin()) goto fail;
    vk_barrier_image(im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
    pvkCmdClearColorImage(s_vk.cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
    if (!s_batch.recording && vk_submit_and_wait()) goto fail;
    return im->view;
fail:
    VK_LOG("engine: could not make the 1x1 %s stand-in\n", vol ? "volume" : "cube");
    vk_destroy_image(im);
    return s_vk.dummy.view;
}

/* What the bound program samples per unit: views and samplers. */
static void vk_eng_image_infos(VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], VkDescriptorImageInfo si[RSX_MAX_TEXTURES])
{
    /* What the bound program declares per unit (vk_eng_decl_mask): the view
     * handed over must be of that type -- a cube for TextureCube (the bound
     * cube, else the stand-in), a volume for Texture3D (the bound one, else
     * the stand-in), and never a cube or a volume where a 2D texture is read. */
    const vk_eng_pipe* EP = (s_e2.pipeline && s_e2.pipeline <= VK_ENG_MAX_PIPE) ? &s_epipe[s_e2.pipeline - 1] : NULL;
    const u32 cube_mask = EP ? EP->cube_mask : 0u, vol_mask = EP ? EP->vol_mask : 0u;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const u32 h = ((s_e2.tex_mask >> u) & 1u) ? s_e2.tex[u] : 0u;
        VkImageView v = h ? vk_eng_sample_view(h) : VK_NULL_HANDLE;
        const int is_cube = h && h < VK_ENG_MAX_OBJ && s_eobj[h].kind == VK_ENG_TEXTURE && s_eobj[h].faces == 6;
        const int is_vol  = h && h < VK_ENG_MAX_OBJ && s_eobj[h].kind == VK_ENG_TEXTURE && s_eobj[h].depth > 1;
        if ((cube_mask >> u) & 1u) { if (!v || !is_cube) v = vk_eng_dummy_view(0); }
        else if ((vol_mask >> u) & 1u) {
            if (v && !is_vol) { static int said; if (said++ < 4)
                VK_LOG("engine: unit %u reads a volume but a %ux%u 2D texture is bound (compressed volume?): black stand-in\n",
                       u, s_eobj[h].w, s_eobj[h].h); }
            if (!v || !is_vol) v = vk_eng_dummy_view(1);
        }
        else if (is_cube || is_vol) v = VK_NULL_HANDLE; /* a cube or volume where 2D is read: the 2D stand-in */
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, v ? v : s_vk.dummy.view, VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
}
/* The guest set layout's four bindings: the constant blocks of `ubuf` (their
 * offsets are dynamic: the vertex file at the bind's first offset, the pixel
 * constants at its second) and the units' views and samplers. */
static void vk_eng_write_set(VkDescriptorSet set, VkBuffer ubuf,
                             const VkDescriptorImageInfo* ii, const VkDescriptorImageInfo* si)
{
    VkDescriptorBufferInfo bi[2] = {
        { ubuf, 0, s_e2.vs_bytes ? s_e2.vs_bytes : 16u },
        { ubuf, 0, s_e2.ps_bytes ? s_e2.ps_bytes : 16u },
    };
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);
}
static void vk_eng_write_descriptors_to(VkDescriptorSet set, VkBuffer ubuf, VkDeviceSize ubase)
{
    (void)ubase;
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    vk_eng_image_infos(ii, si);
    vk_eng_write_set(set, ubuf, ii, si);
}

/* Batched draws' descriptor sets, reused: with the constant offsets dynamic, a
 * set is only its constant buffer, the two block sizes and the sixteen units'
 * views and samplers, and runs of draws share all of those (a building's
 * windows, a street of one material). Allocating and writing a set per draw
 * was ~6000 vkAllocateDescriptorSets + vkUpdateDescriptorSets (34 descriptors
 * each) a frame in the Empire City skyline. The table holds sets of the open
 * batch's pool only: a pool reset (vk_batch_completed) or a slot swap
 * (vk_fif_advance) moves s_dset_gen on and empties it. Views do not die while
 * a batch records (vk_destroy_image defers to its completion), so a key's
 * view handles name live views. PS3RECOMP_VK_DSET_REUSE=0: a set per draw. */
typedef struct {
    VkBuffer    ubuf;
    u32         vs, ps;
    VkImageView v[RSX_MAX_TEXTURES];
    VkSampler   smp[RSX_MAX_TEXTURES];
} vk_dset_key;
#define VK_DSET_CACHE 4096u
static struct { u32 gen; vk_dset_key key; VkDescriptorSet set; } s_dset_tab[VK_DSET_CACHE];
static u32 s_dset_gen = 1;
static unsigned long long s_dset_hit, s_dset_miss;
static VkDescriptorSet vk_batch_alloc_set(void);
static VkDescriptorSet vk_eng_batch_set(VkBuffer ubuf)
{
    static int reuse = -1;
    if (reuse < 0) { const char* e = getenv("PS3RECOMP_VK_DSET_REUSE"); reuse = !(e && e[0] == '0'); }
    /* PS3RECOMP_VK_DSET_MEMO (default 1; 0 = off): the previous draw's set
     * again when everything it was made from is unchanged -- the units'
     * handles and sampler states, the pipeline (its cube / volume masks), the
     * block sizes, the buffer, the anisotropy setting, no object or image
     * destroyed since (s_eobj_gen) and the same set table (s_dset_gen).
     * Building the sixteen views and samplers and the table key per draw was
     * ~4% of the RSX thread. */
    static int memo_on = -1;
    if (memo_on < 0) { const char* e = getenv("PS3RECOMP_VK_DSET_MEMO"); memo_on = !(e && e[0] == '0'); }
    static struct {
        int valid, aniso;
        u32 obj_gen, dset_gen, pipeline, tex_mask, vs, ps;
        VkBuffer ubuf;
        u32 tex[RSX_MAX_TEXTURES];
        rsx_be_sampler_desc samp[RSX_MAX_TEXTURES];
        VkDescriptorSet set;
    } m;
    const int memo = memo_on && reuse;
    const u32 m_vs = s_e2.vs_bytes ? s_e2.vs_bytes : 16u, m_ps = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
    const int m_aniso = memo ? vk_postfx_aniso() : 0;
    if (memo && m.valid && m.obj_gen == s_eobj_gen && m.dset_gen == s_dset_gen && m.ubuf == ubuf &&
        m.vs == m_vs && m.ps == m_ps && m.pipeline == s_e2.pipeline && m.tex_mask == s_e2.tex_mask &&
        m.aniso == m_aniso && !memcmp(m.tex, s_e2.tex, sizeof m.tex)) {
        int same = 1;
        for (u32 u = 0; u < RSX_MAX_TEXTURES && same; u++)
            if (((s_e2.tex_mask >> u) & 1u) && memcmp(&m.samp[u], &s_e2.samp[u], sizeof m.samp[u])) same = 0;
        if (same) { s_dset_hit++; return m.set; }
    }
    m.valid = 0;
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    vk_eng_image_infos(ii, si);
    if (memo) {   /* filled now; valid once the set below exists */
        m.obj_gen = s_eobj_gen; m.pipeline = s_e2.pipeline; m.tex_mask = s_e2.tex_mask;
        m.vs = m_vs; m.ps = m_ps; m.aniso = m_aniso; m.ubuf = ubuf;
        memcpy(m.tex, s_e2.tex, sizeof m.tex); memcpy(m.samp, s_e2.samp, sizeof m.samp);
    }
    if (!reuse) {
        VkDescriptorSet set = vk_batch_alloc_set();
        if (set) vk_eng_write_set(set, ubuf, ii, si);
        return set;
    }
    vk_dset_key k;
    memset(&k, 0, sizeof k);
    k.ubuf = ubuf;
    k.vs = s_e2.vs_bytes ? s_e2.vs_bytes : 16u;
    k.ps = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
    u64 h = 0xCBF29CE484222325ull ^ (u64)(uintptr_t)ubuf ^ ((u64)k.vs << 32) ^ k.ps;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        k.v[u] = ii[u].imageView; k.smp[u] = si[u].sampler;
        h = (h ^ (u64)(uintptr_t)k.v[u]) * 0x100000001B3ull;
        h = (h ^ (u64)(uintptr_t)k.smp[u]) * 0x100000001B3ull;
    }
    h ^= h >> 29;
    const u32 slot = (u32)h & (VK_DSET_CACHE - 1u);
    if (s_dset_tab[slot].gen == s_dset_gen && !memcmp(&s_dset_tab[slot].key, &k, sizeof k)) {
        s_dset_hit++;
        if (memo) { m.set = s_dset_tab[slot].set; m.dset_gen = s_dset_gen; m.valid = m.obj_gen == s_eobj_gen; }
        return s_dset_tab[slot].set;
    }
    VkDescriptorSet set = vk_batch_alloc_set();
    if (!set) return VK_NULL_HANDLE;
    vk_eng_write_set(set, ubuf, ii, si);
    s_dset_tab[slot].gen = s_dset_gen;
    s_dset_tab[slot].key = k;
    s_dset_tab[slot].set = set;
    s_dset_miss++;
    if (memo) { m.set = set; m.dset_gen = s_dset_gen; m.valid = m.obj_gen == s_eobj_gen; }
    return set;
}

/* Room in all three arenas for one draw: offsets out, cursors advanced. A
 * batch that cannot take the draw is flushed first (then the arenas are free
 * and may grow). */
static int vk_batch_reserve(VkDeviceSize vb, VkDeviceSize ib, VkDeviceSize ub,
                            VkDeviceSize* vo, VkDeviceSize* io, VkDeviceSize* uo)
{
    if (!s_batch.ualign) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
        s_batch.ualign = p.limits.minUniformBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : 256;
    }
    for (int pass = 0; pass < 2; pass++) {
        const VkDeviceSize v = (s_batch.vcur + 15u) & ~(VkDeviceSize)15u;
        const VkDeviceSize i = (s_batch.icur + 3u) & ~(VkDeviceSize)3u;
        const VkDeviceSize u = (s_batch.ucur + s_batch.ualign - 1u) / s_batch.ualign * s_batch.ualign;
        if (s_batch.varena.buf && s_batch.iarena.buf && s_batch.uarena.buf &&
            v + vb <= s_batch.varena.size && i + (ib ? ib : 4u) <= s_batch.iarena.size &&
            u + ub <= s_batch.uarena.size && s_batch.dsets < VK_BATCH_SETS) {
            *vo = v; *io = i; *uo = u;
            s_batch.vcur = v + vb; s_batch.icur = i + ib; s_batch.ucur = u + ub;
            return 0;
        }
        if (pass) break;
        vk_batch_flush();                       /* idle from here: arenas may grow */
        if (vk_batch_arena(&s_batch.varena, vb, 32u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            vk_batch_arena(&s_batch.iarena, ib ? ib : 4u, 8u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
            vk_batch_arena(&s_batch.uarena, ub, 64u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return -1;
    }
    VK_LOG("batching: a single draw does not fit the arenas\n");
    return -1;
}

/* A fresh descriptor set for one recorded draw (the pool resets at flush). */
static VkDescriptorSet vk_batch_alloc_set(void)
{
    if (!s_batch.dpool) {
        VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2u * VK_BATCH_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES * VK_BATCH_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES * VK_BATCH_SETS },
        };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = VK_BATCH_SETS, .poolSizeCount = 3, .pPoolSizes = ps };
        if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_batch.dpool) != VK_SUCCESS) {
            VK_LOG("batching: vkCreateDescriptorPool failed\n");
            s_batch.dpool = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
    }
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_batch.dpool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) {
        VK_LOG("batching: vkAllocateDescriptorSets failed after %u sets\n", s_batch.dsets);
        return VK_NULL_HANDLE;
    }
    s_batch.dsets++;
    return set;
}

/* ---- persistent vertex / index buffers (rsx_draw_backend.vbuf_create) ------------
 * The engine's vertex cache keeps the decoded vertices of geometry whose guest
 * bytes did not change since the last frame, and its index cache the rebuilt
 * index lists, and draws from them instead of fetching, decoding and staging
 * them again (rsx_draw_engine.c, "vertex cache"). Without these entry points
 * both caches were off on this backend: the Empire City skyline converted and
 * staged ~31 MB of vertices a frame, ~6 ms of the RSX thread plus the copy.
 * Blocks are sub-allocated from 64 MB host-coherent chunks (one VkBuffer each:
 * a buffer per entry would run into the driver's allocation-count limit, 4096
 * on NVIDIA) in size classes four to an octave, so a freed block is reused by
 * the next entry of its class. The GPU reads them where it read the per-frame
 * arena before -- host memory. A released handle stays valid for the draws
 * already recorded: it is freed with the batch's dead objects. */
#define VK_VB_CHUNK   (64ull << 20)
#define VK_VB_CHUNKS  32u                    /* 2 GB ceiling (the engine budgets 512 MB) */
#define VK_VB_HANDLES (1u << 17)
#define VK_VB_CLASSES 73u                    /* 256 B .. 64 MB */
typedef struct { u32 chunk, off, cls, live; } vk_vb_ent;
static struct {
    vk_buffer    chunk[VK_VB_CHUNKS];
    u32          nchunks;
    VkDeviceSize bump;                      /* used part of the newest chunk */
    vk_vb_ent*   ent;                       /* by handle; 0 is no buffer */
    u32*         free_h;
    u32          nfree_h, next_h;
    u64*         fl[VK_VB_CLASSES];         /* free blocks: chunk << 32 | offset */
    u32          fl_n[VK_VB_CLASSES], fl_cap[VK_VB_CLASSES];
    u32          live;
    VkDeviceSize live_bytes;
} s_vb;
static VkDeviceSize vk_vb_class_size(u32 c) { return (VkDeviceSize)(4u + (c & 3u)) << (6u + c / 4u); }
static void vk_reuse_stats(void)
{
    fprintf(stderr, "[RSX vulkan]   descriptor sets reused %llu, written %llu; persistent vbufs %u (%.0f MB in %u chunks)\n",
            s_dset_hit, s_dset_miss, s_vb.live, (double)s_vb.live_bytes / 1e6, s_vb.nchunks);
}

static u32 vk_eng_vbuf_create(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!data || !bytes || !s_vk.device) return 0;
    u32 c = 0;
    while (c < VK_VB_CLASSES && vk_vb_class_size(c) < bytes) c++;
    if (c == VK_VB_CLASSES) return 0;
    if (!s_vb.ent) {
        s_vb.ent = (vk_vb_ent*)calloc(VK_VB_HANDLES, sizeof *s_vb.ent);
        s_vb.free_h = (u32*)malloc(VK_VB_HANDLES * sizeof(u32));
        if (!s_vb.ent || !s_vb.free_h) { free(s_vb.ent); free(s_vb.free_h); s_vb.ent = NULL; s_vb.free_h = NULL; return 0; }
        s_vb.next_h = 1;
    }
    u32 h;
    if (s_vb.nfree_h) h = s_vb.free_h[--s_vb.nfree_h];
    else if (s_vb.next_h < VK_VB_HANDLES) h = s_vb.next_h++;
    else return 0;
    u32 chunk, off;
    if (s_vb.fl_n[c]) {
        const u64 b = s_vb.fl[c][--s_vb.fl_n[c]];
        chunk = (u32)(b >> 32); off = (u32)b;
    } else {
        const VkDeviceSize sz = vk_vb_class_size(c);
        if (!s_vb.nchunks || s_vb.bump + sz > VK_VB_CHUNK) {
            vk_buffer* nb = &s_vb.chunk[s_vb.nchunks];
            if (s_vb.nchunks == VK_VB_CHUNKS ||
                vk_create_host_buffer(nb, VK_VB_CHUNK, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
                !nb->coherent) {
                if (s_vb.nchunks < VK_VB_CHUNKS) vk_destroy_buffer(nb);
                if (!(s_e2.warned & 0x200u)) { VK_LOG("engine: persistent vertex buffers full (%u chunks)\n", s_vb.nchunks); s_e2.warned |= 0x200u; }
                s_vb.free_h[s_vb.nfree_h++] = h;
                return 0;
            }
            s_vb.nchunks++;
            s_vb.bump = 0;
        }
        chunk = s_vb.nchunks - 1u; off = (u32)s_vb.bump;
        s_vb.bump += sz;
    }
    memcpy((u8*)s_vb.chunk[chunk].ptr + off, data, bytes);
    s_vb.ent[h] = (vk_vb_ent){ chunk, off, c, 1u };
    s_vb.live++; s_vb.live_bytes += vk_vb_class_size(c);
    return h;
}
static void vk_vb_free_now(u32 h)
{
    vk_vb_ent* e = &s_vb.ent[h];
    if (!e->live) return;
    const u32 c = e->cls;
    if (s_vb.fl_n[c] == s_vb.fl_cap[c]) {
        const u32 cap = s_vb.fl_cap[c] ? s_vb.fl_cap[c] * 2u : 256u;
        u64* n = (u64*)realloc(s_vb.fl[c], (size_t)cap * sizeof(u64));
        if (!n) return;                         /* leak the block rather than lose track */
        s_vb.fl[c] = n; s_vb.fl_cap[c] = cap;
    }
    s_vb.fl[c][s_vb.fl_n[c]++] = ((u64)e->chunk << 32) | e->off;
    s_vb.live--; s_vb.live_bytes -= vk_vb_class_size(c);
    e->live = 0;
    s_vb.free_h[s_vb.nfree_h++] = h;
}
static void vk_eng_vbuf_release(void* user, u32 h)
{
    (void)user;
    if (!h || h >= VK_VB_HANDLES || !s_vb.ent || s_vb.ent[h].live != 1u) return;
    if (s_batch.recording || vk_fif_busy()) {   /* a recorded draw may still read it */
        if (s_batch.n_dead_vb == s_batch.cap_dead_vb) {
            const u32 cap = s_batch.cap_dead_vb ? s_batch.cap_dead_vb * 2u : 1024u;
            u32* n = (u32*)realloc(s_batch.dead_vb, (size_t)cap * sizeof(u32));
            if (!n) { if (s_batch.recording) vk_batch_flush(); else vk_fif_wait_other(); vk_vb_free_now(h); return; }
            s_batch.dead_vb = n; s_batch.cap_dead_vb = cap;
        }
        s_vb.ent[h].live = 2u;                  /* released: freed with the batch */
        s_batch.dead_vb[s_batch.n_dead_vb++] = h;
        return;
    }
    vk_vb_free_now(h);
}
/* The persistent buffer and offset behind a handle holding at least `bytes`. */
static int vk_vb_get(u32 h, VkDeviceSize bytes, VkBuffer* buf, VkDeviceSize* off)
{
    if (!h || h >= VK_VB_HANDLES || !s_vb.ent || s_vb.ent[h].live != 1u) return 0;
    const vk_vb_ent* e = &s_vb.ent[h];
    if (bytes > vk_vb_class_size(e->cls)) return 0;
    *buf = s_vb.chunk[e->chunk].buf;
    *off = e->off;
    return 1;
}

/* The state the open engine pass last recorded. Within one run of draws in a
 * pass nothing else records into it (any other operation ends the pass first,
 * vk_begin), and every engine pipeline declares viewport, scissor, stencil
 * reference and depth bias dynamic, so a bind or set that repeats the last
 * one is skipped: of the ~9 commands a draw recorded, most repeated the draw
 * before (a building's pieces share pipeline, viewport, scissor, stencil and,
 * from the vertex cache, often the buffer). Forgotten at every pass start.
 * PS3RECOMP_VK_STATE_FILTER=0 records everything. */
static struct {
    int          valid;
    VkPipeline   pipe;
    VkViewport   vp;
    VkRect2D     sc;
    u32          sref;
    float        bc, bs;
    int          bias_set;
    VkBuffer     vb, ib;
    VkDeviceSize vo, io;
} s_ecs;
static unsigned long long s_ecs_skipped;

/* One draw: vertices from host memory (staged) or from a persistent buffer
 * (pvb/pvo), indices likewise -- host, persistent (pib/pio) or none. */
static void vk_eng_draw_ex(rsx_topology topology, const void* vertices, VkBuffer pvb, VkDeviceSize pvo,
                           u32 vertex_count, u32 stride, const u32* indices, VkBuffer pib, VkDeviceSize pio,
                           u32 index_count)
{
    if (!s_e2.pipeline || (!vertices && !pvb) || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return;
    vk_rtd_note(s_eng_bound.rt, n);
    vk_rtd_note_tex(s_e2.tex, s_e2.tex_mask);
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    const int use_bias = s_e2.bias_const != 0.0f || s_e2.bias_slope != 0.0f;
    const u32 vk_idx = ((s_e2.depth_clamp && s_vk_depth_clamp_ok) ? 1u : 0u) | (use_bias ? 2u : 0u);
    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u, vk_idx);
    VkRenderPass rp = vk_eng_render_pass(n);
    VkFramebuffer fb = vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) return;

    const VkDeviceSize vbytes = vertices ? (VkDeviceSize)vertex_count * stride : 0;
    const VkDeviceSize ibytes = (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    const int has_idx = index_count && (indices || pib);
    const int batched = vk_batch_on() && vk_batch_shadow();
    VkBuffer vbuf = s_vk.vertices.buf, ibuf = s_e2.indices.buf;
    VkDeviceSize voff = 0, ioff = 0;
    VkDescriptorSet set = s_g.set;
    VkDeviceSize ubase = 0;                    /* the constant window: dynamic offsets */
    if (batched) {
        VkDeviceSize uoff = 0;
        /* The VS file whole (programs index it freely) and the PS bytes bound
         * -- the descriptor ranges cover exactly that much. */
        VkDeviceSize ub = s_g.ps_off + ((s_e2.ps_bytes ? s_e2.ps_bytes : 16u) + 15u) / 16u * 16u;
        if (ub > (VkDeviceSize)s_batch.shadow_size) ub = (VkDeviceSize)s_batch.shadow_size;
        /* A ranged VS block (bind_vs_constants_range): the descriptor window
         * still starts at an aligned base and spans the whole file, but only
         * the rows read, the tail and the PS constants are written, and the
         * reservation starts at the first row read -- the rows below it, which
         * the program never reads, may be another draw's bytes. */
        const int ranged = s_e2.vs_ranged && ub >= s_g.ps_off;
        VkDeviceSize lead = 0;
        if (ranged) {
            if (!s_batch.ualign) {              /* as vk_batch_reserve reads it */
                VkPhysicalDeviceProperties pp;
                pvkGetPhysicalDeviceProperties(s_vk.phys, &pp);
                s_batch.ualign = pp.limits.minUniformBufferOffsetAlignment ? pp.limits.minUniformBufferOffsetAlignment : 256;
            }
            lead = (VkDeviceSize)s_e2.vs_first * 16u / s_batch.ualign * s_batch.ualign;
        }
        if (vk_batch_reserve(vbytes, ibytes, ub - lead, &voff, &ioff, &uoff) ||
            !(set = vk_eng_batch_set(s_batch.uarena.buf)))
            return;
        if (vertices) memcpy((u8*)s_batch.varena.ptr + voff, vertices, (size_t)vbytes);
        if (ibytes) memcpy((u8*)s_batch.iarena.ptr + ioff, indices, (size_t)ibytes);
        if (ranged) {
            VkDeviceSize base = uoff - lead;
            if (uoff < lead) {                  /* the arena's start: no room below, take it whole */
                base = uoff;
                if (base + ub > s_batch.uarena.size) return;
                if (s_batch.ucur < base + ub) s_batch.ucur = base + ub;
            }
            u8* u = (u8*)s_batch.uarena.ptr + base;
            if (s_e2.vs_count)
                memcpy(u + s_e2.vs_first * 16u, s_e2.vs_rows + s_e2.vs_first * 16u, s_e2.vs_count * 16u);
            if (s_e2.vs_tail_bytes) memcpy(u + s_e2.vs_tail_off, s_e2.vs_tail, s_e2.vs_tail_bytes);
            memcpy(u + s_g.ps_off, s_batch.shadow + s_g.ps_off, (size_t)(ub - s_g.ps_off));
            uoff = base;
        } else {
            memcpy((u8*)s_batch.uarena.ptr + uoff, s_batch.shadow, (size_t)ub);
        }
        ubase = uoff;
        vbuf = vertices ? s_batch.varena.buf : pvb;
        voff = vertices ? voff : pvo;
        ibuf = indices ? s_batch.iarena.buf : pib;
        ioff = indices ? ioff : pio;
    } else {
        if (vertices) {
            if (vk_ensure_vertex_buffer(vbytes)) return;
            memcpy(s_vk.vertices.ptr, vertices, (size_t)vbytes);
            vbuf = s_vk.vertices.buf;
        } else { vbuf = pvb; voff = pvo; }
        if (ibytes) {
            if (vk_eng_ensure_indices(ibytes)) return;
            memcpy(s_e2.indices.ptr, indices, (size_t)ibytes);
            ibuf = s_e2.indices.buf;
        } else if (pib) { ibuf = pib; ioff = pio; }
        vk_eng_write_descriptors();
    }

    /* Batched, a draw into the framebuffer of the pass left open by the draw
     * before continues that pass: draws inside one pass are ordered against
     * each other's attachment writes by the rasterizer, so the barriers and
     * the pass are paid once per run of draws, not per draw. A draw that
     * samples one of the pass's own targets breaks the run (a new pass behind
     * a barrier), which is what every draw got before. */
    int cont = 0;
    if (batched && s_batch.rp_open && s_batch.rp_fb == fb && s_batch.rp_rp == rp) {
        cont = 1;
        for (u32 u = 0; u < RSX_MAX_TEXTURES && cont; u++) {
            if (!((s_e2.tex_mask >> u) & 1u)) continue;
            u32 t = s_e2.tex[u];
            if (!t || t >= VK_ENG_MAX_OBJ) continue;
            if (s_eobj[t].kind == VK_ENG_VIEW) t = s_eobj[t].surface;
            if (t == s_eng_bound.depth) cont = 0;
            for (u32 i = 0; i < n; i++) if (t == s_eng_bound.rt[i]) cont = 0;
        }
    }
    if (cont) {
        s_batch.merged++;
    } else {
        if (vk_begin()) return;                 /* batched: ends the pass left open */
        /* Unbatched, everything submitted before is complete (each submission
         * is waited on); batched, a global barrier orders this pass after
         * everything recorded before it (it may sample what that rendered).
         * Either way the image barriers order the surfaces it draws into. */
        /* PS3RECOMP_VK_BARRIER_MODE (measurement only, not correct rendering): 1 = no global
         * barrier at an engine pass start, 2 = and no per-target barriers either. */
        static int bm = -1;
        if (bm < 0) { const char* e = getenv("PS3RECOMP_VK_BARRIER_MODE"); bm = e ? atoi(e) : 0; }
        if (batched && bm < 1) vk_barrier_all();
        if (bm < 2)
        for (u32 i = 0; i < n; i++)
            vk_barrier_image(s_eobj[s_eng_bound.rt[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = rp, .framebuffer = fb,
                                      .renderArea = { { 0, 0 }, { w, h } } };
        vk_gprof_stamp(GP_GAP, w, h);
        pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        if (batched) { s_batch.rp_open = 1; s_batch.rp_fb = fb; s_batch.rp_rp = rp; s_batch.rp_w = w; s_batch.rp_h = h; }
        s_ecs.valid = 0;
    }
    static int filt = -1;
    if (filt < 0) { const char* e = getenv("PS3RECOMP_VK_STATE_FILTER"); filt = !(e && e[0] == '0'); }
    const int known = filt && s_ecs.valid;
    if (!known || s_ecs.pipe != pipe) pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    else s_ecs_skipped++;
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 x0 = s_e2.sc[0] < w ? s_e2.sc[0] : w, y0 = s_e2.sc[1] < h ? s_e2.sc[1] : h;
        u32 x1 = s_e2.sc[0] + s_e2.sc[2], y1 = s_e2.sc[1] + s_e2.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    if (!known || memcmp(&s_ecs.vp, &vp, sizeof vp)) pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    if (!known || memcmp(&s_ecs.sc, &sc, sizeof sc)) pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    const u32 sref = s_e2.stencil_ref & 0xFFu;
    if (!known || s_ecs.sref != sref)
        pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sref);
    int bias_set = known && s_ecs.bias_set;
    if (use_bias && (!bias_set || s_ecs.bc != s_e2.bias_const || s_ecs.bs != s_e2.bias_slope)) {
        pvkCmdSetDepthBias(s_vk.cmd, s_e2.bias_const, 0.0f, s_e2.bias_slope);
        bias_set = 1; s_ecs.bc = s_e2.bias_const; s_ecs.bs = s_e2.bias_slope;
    }
    { const u32 dyn[2] = { (u32)ubase, (u32)(ubase + s_g.ps_off) };
      pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                               0, 1, &set, 2, dyn); }
    if (!known || s_ecs.vb != vbuf || s_ecs.vo != voff) pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vbuf, &voff);
    if (has_idx && (!known || s_ecs.ib != ibuf || s_ecs.io != ioff))
        pvkCmdBindIndexBuffer(s_vk.cmd, ibuf, ioff, VK_INDEX_TYPE_UINT32);
    s_ecs.valid = batched;                     /* unbatched, every draw is its own pass */
    s_ecs.pipe = pipe; s_ecs.vp = vp; s_ecs.sc = sc; s_ecs.sref = sref; s_ecs.bias_set = bias_set;
    s_ecs.vb = vbuf; s_ecs.vo = voff;
    if (has_idx) { s_ecs.ib = ibuf; s_ecs.io = ioff; }
    else if (!known) { s_ecs.ib = VK_NULL_HANDLE; s_ecs.io = 0; }
    if (has_idx) {
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    if (P->sblend) {                            /* FUNC_ADD_SIGNED: the negative half */
        VkPipeline npipe = vk_eng_variant_ex(P, (u32)topology - 1u, vk_idx, 1);
        if (npipe) {
            pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, npipe);
            s_ecs.pipe = npipe;
            if (has_idx) pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
            else         pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
        }
    }
    if (batched) {
        s_batch.draws++;                        /* the pass stays open in the batch */
        if (s_rtd.phase == 1) vk_rtd_after_draw(s_eng_bound.rt, n);
        return;
    }
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}
static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!vertices) return;
    vk_eng_draw_ex(topology, vertices, VK_NULL_HANDLE, 0, vertex_count, stride, indices, VK_NULL_HANDLE, 0, index_count);
}
static void vk_eng_draw_vbuf(void* user, rsx_topology topology, u32 vb, u32 vertex_count, u32 stride,
                             const u32* indices, u32 index_count)
{
    (void)user;
    VkBuffer b; VkDeviceSize o;
    if (!vk_vb_get(vb, (VkDeviceSize)vertex_count * stride, &b, &o)) return;
    vk_eng_draw_ex(topology, NULL, b, o, vertex_count, stride, indices, VK_NULL_HANDLE, 0, index_count);
}
static void vk_eng_draw_vbuf_ib(void* user, rsx_topology topology, u32 vb, u32 vertex_count, u32 stride,
                                u32 ib, u32 index_count)
{
    (void)user;
    VkBuffer b, ibb; VkDeviceSize o, io;
    if (!index_count || !vk_vb_get(vb, (VkDeviceSize)vertex_count * stride, &b, &o) ||
        !vk_vb_get(ib, (VkDeviceSize)index_count * 4u, &ibb, &io)) return;
    vk_eng_draw_ex(topology, NULL, b, o, vertex_count, stride, NULL, ibb, io, index_count);
}

/* ---- E5: the GPU light path -----------------------------------------------
 * color_snapshot, light_pass, ssao_pass and depth_snapshot_rgba8: what the
 * engine's PS3RECOMP_ENG_GPU_LIGHT and PS3RECOMP_ENG_GPU_SSAO paths run on
 * (rsx_draw_engine.c; eng_gpu_light_on wants light_pass, color_snapshot and
 * depth_snapshot), as the Metal backend has them (rsx_metal_backend.m,
 * eng_color_snapshot .. eng_depth_snapshot_rgba8). The passes are Metal's
 * helper fragment functions ported to GLSL (vulkan/rsx_vk_passes_glsl.h),
 * compiled on first use with glslang like the guest programs, and drawn as
 * full-screen triangles with their constants in a UBO. Metal records a pass
 * into the stream and encodes it later; this backend records, submits and
 * waits at once, as for every other operation -- the call order IS the stream
 * order, and each pass's one UBO and descriptor set can be rewritten on every
 * call (Metal needs a ring of 8 for frames in flight).
 *
 * What a pass hands the engine is a COLOR object: RGBA8, sampleable, and
 * surface_view can cut a crossbar view of it, as Metal's PixelFormatView
 * textures allow. The engine never releases the light and SSAO outputs or the
 * colour copies (they persist and are refilled, as on Metal); it does release
 * a depth decode it has replaced, which `tag` lets the cache notice. */
enum { VK_PASS_LIGHT, VK_PASS_SSAO_HD, VK_PASS_SSAO_AO, VK_PASS_SSAO_BLUR_H, VK_PASS_SSAO_BLUR_V,
       VK_PASS_SSAO_UP, VK_PASS_ZRGBA, VK_PASS_COUNT };
#define VK_PASS_TEXTURES  3u                /* bindings 1..3: MSL texture(0..2) */
#define VK_SSAO_UBO_BYTES 512u              /* the engine's bound (s_gs.consts) */
#define VK_PASS_SPV_WORDS 16384u
#define VK_CSNAP_MAX      64u
#define VK_ZRGBA_MAX      32u
#define VK_ZRGBA_SETS     16u              /* depth decodes one batch can hold */
#define VK_TAG_CSNAP      0xC5000000u       /* a colour copy                      */
#define VK_TAG_PASS       0xA0000000u       /* a light or SSAO output             */
#define VK_TAG_ZRGBA      0x5A000000u       /* | depth handle: a depth decode     */

static struct {
    int                   tried, ok;
    VkShaderModule        vs, fs[VK_PASS_COUNT];
    VkPipeline            pipe[VK_PASS_COUNT];
    u8                    failed[VK_PASS_COUNT];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set[VK_PASS_COUNT];      /* one per pass */
    VkDescriptorSet       zset[VK_ZRGBA_SETS];     /* the depth decode's, one per decode in a batch */
    VkRenderPass          rp_light, rp_rgba8, rp_r8, rp_r32f;
    vk_buffer             light_ubo, ssao_ubo;
    u32                   light_out[2], light_w, light_h;
    VkFramebuffer         light_fb;
    vk_image              ssao_hd, ssao_a, ssao_b; /* R32F half depth, two R8 AO planes */
    u32                   ssao_out, ssao_dims[4];
    VkFramebuffer         ssao_fb[4];              /* hd, a, b, out */
    vk_image              zr32;                    /* the depth decode's R32F copy */
    vk_image              zs8;                     /* ... and its stencil (R8), same size */
    u32                   zr32_w, zr32_h;
} s_lp;
/* the parked slot's pass UBOs and descriptor sets (see frames in flight) */
static struct {
    VkDescriptorSet set[VK_PASS_COUNT], zset[VK_ZRGBA_SETS];
    vk_buffer       light_ubo, ssao_ubo;
} s_lp_park[VK_FIF_MAX];
static void vk_lp_swap(u32 out, u32 in)
{
    memcpy(s_lp_park[out].set, s_lp.set, sizeof s_lp.set);
    memcpy(s_lp_park[out].zset, s_lp.zset, sizeof s_lp.zset);
    s_lp_park[out].light_ubo = s_lp.light_ubo; s_lp_park[out].ssao_ubo = s_lp.ssao_ubo;
    memcpy(s_lp.set, s_lp_park[in].set, sizeof s_lp.set);
    memcpy(s_lp.zset, s_lp_park[in].zset, sizeof s_lp.zset);
    s_lp.light_ubo = s_lp_park[in].light_ubo; s_lp.ssao_ubo = s_lp_park[in].ssao_ubo;
}

static struct { u32 src, dst; } s_csnap[VK_CSNAP_MAX];
static u32 s_csnap_count;
static struct { u32 depth, w, h, dst; } s_zrgba[VK_ZRGBA_MAX];
static u32 s_zrgba_count;

static VkShaderModule vk_pass_module(const char* glsl, int stage, const char* what)
{
    static u32 spv[VK_PASS_SPV_WORDS];
    char log[2048];
    u32 n = 0;
    if (rsx_glsl_to_spirv(glsl, stage, spv, VK_PASS_SPV_WORDS, &n, log, sizeof log) || !n) {
        VK_LOG("engine: %s shader failed: %s\n", what, log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("engine: %s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* What every pass shares: the vertex stage, one set layout (binding 0 the
 * constants, 1..3 the textures, all fragment-stage), its pipeline layout, a
 * set per pass, and the two constant buffers. Tried once. */
static int vk_pass_init(void)
{
    if (s_lp.tried) return s_lp.ok ? 0 : -1;
    s_lp.tried = 1;
    if (!rsx_hlsl_to_spirv_available()) {
        VK_LOG("engine: built without glslang: no GPU light, SSAO or depth-decode passes\n");
        return -1;
    }
    if (!(s_lp.vs = vk_pass_module(kVkPassVS, RSX_SHADER_STAGE_VERTEX, "full-screen pass vertex")))
        return -1;
    VkDescriptorSetLayoutBinding b[1 + VK_PASS_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 k = 0; k < VK_PASS_TEXTURES; k++)
        b[1 + k] = (VkDescriptorSetLayoutBinding){ 1 + k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1 + VK_PASS_TEXTURES, .pBindings = b };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_lp.set_layout) != VK_SUCCESS) return -1;
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_lp.set_layout };
    if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_lp.pipe_layout) != VK_SUCCESS) return -1;
    VkDescriptorPoolSize ps[2] = {   /* a set group per frame-in-flight slot */
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (VK_PASS_COUNT + VK_ZRGBA_SETS) * VK_FIF_MAX },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (VK_PASS_COUNT + VK_ZRGBA_SETS) * VK_PASS_TEXTURES * VK_FIF_MAX } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = (VK_PASS_COUNT + VK_ZRGBA_SETS) * VK_FIF_MAX,
                                       .poolSizeCount = 2, .pPoolSizes = ps };
    if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_lp.pool) != VK_SUCCESS) return -1;
    VkDescriptorSetLayout layouts[VK_PASS_COUNT];
    for (u32 k = 0; k < VK_PASS_COUNT; k++) layouts[k] = s_lp.set_layout;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_lp.pool, .descriptorSetCount = VK_PASS_COUNT,
                                        .pSetLayouts = layouts };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, s_lp.set) != VK_SUCCESS) return -1;
    { VkDescriptorSetLayout zl[VK_ZRGBA_SETS];
      for (u32 k = 0; k < VK_ZRGBA_SETS; k++) zl[k] = s_lp.set_layout;
      VkDescriptorSetAllocateInfo zai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s_lp.pool, .descriptorSetCount = VK_ZRGBA_SETS,
                                          .pSetLayouts = zl };
      if (pvkAllocateDescriptorSets(s_vk.device, &zai, s_lp.zset) != VK_SUCCESS) return -1; }
    /* Coherent, as every host-written buffer here (vk_invalidate's note). */
    if (vk_create_host_buffer(&s_lp.light_ubo, sizeof(rsx_gpu_light_params),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_lp.light_ubo.coherent ||
        vk_create_host_buffer(&s_lp.ssao_ubo, VK_SSAO_UBO_BYTES,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_lp.ssao_ubo.coherent)
        return -1;
    /* the parked slot's own group (frames in flight) */
    { const u32 o = s_fif_cur ^ 1u;
      VkDescriptorSetAllocateInfo d2 = dai; d2.pSetLayouts = layouts;
      if (pvkAllocateDescriptorSets(s_vk.device, &d2, s_lp_park[o].set) != VK_SUCCESS) return -1;
      VkDescriptorSetLayout zl[VK_ZRGBA_SETS];
      for (u32 k = 0; k < VK_ZRGBA_SETS; k++) zl[k] = s_lp.set_layout;
      VkDescriptorSetAllocateInfo z2 = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = s_lp.pool, .descriptorSetCount = VK_ZRGBA_SETS,
                                         .pSetLayouts = zl };
      if (pvkAllocateDescriptorSets(s_vk.device, &z2, s_lp_park[o].zset) != VK_SUCCESS) return -1;
      if (vk_create_host_buffer(&s_lp_park[o].light_ubo, sizeof(rsx_gpu_light_params),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_lp_park[o].light_ubo.coherent ||
          vk_create_host_buffer(&s_lp_park[o].ssao_ubo, VK_SSAO_UBO_BYTES,
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_lp_park[o].ssao_ubo.coherent)
          return -1; }
    s_lp.ok = 1;
    return 0;
}

/* `n` attachments of one format, no depth; DONT_CARE in (the triangle covers
 * every pixel, as Metal's MTLLoadActionDontCare), STORE out, GENERAL as
 * everywhere in this backend. */
static VkRenderPass vk_pass_render_pass(VkRenderPass* rp, VkFormat fmt, u32 n)
{
    if (*rp || n < 1 || n > 2) return *rp;
    VkAttachmentDescription att[2];
    VkAttachmentReference   ref[2];
    for (u32 i = 0; i < n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        ref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = ref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, rp) != VK_SUCCESS) *rp = VK_NULL_HANDLE;
    return *rp;
}

/* The full-screen pipeline of pass `which`, built on first use; a failure is
 * logged once and not retried (the engine then falls back, as with Metal's
 * nil pipeline state). */
static VkPipeline vk_pass_pipeline(u32 which, const char* glsl, const char* what, VkRenderPass rp, u32 n_att)
{
    if (s_lp.pipe[which] || s_lp.failed[which] || !rp) return s_lp.pipe[which];
    s_lp.failed[which] = 1;
    if (!s_lp.fs[which] &&
        !(s_lp.fs[which] = vk_pass_module(glsl, RSX_SHADER_STAGE_FRAGMENT, what)))
        return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_lp.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_lp.fs[which], .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba[2] = { { .colorWriteMask = 0xFu }, { .colorWriteMask = 0xFu } };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = n_att, .pAttachments = cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_lp.pipe_layout, .renderPass = rp, .subpass = 0 };
    if (vk_sc_create_pipeline(&gci, &s_lp.pipe[which], 3) != VK_SUCCESS) {
        VK_LOG("engine: %s pipeline failed\n", what);
        s_lp.pipe[which] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    s_lp.failed[which] = 0;
    return s_lp.pipe[which];
}

static VkFramebuffer vk_pass_framebuffer(VkRenderPass rp, const VkImageView* views, u32 n, u32 w, u32 h)
{
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    return pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE;
}

/* Binding 0 the whole of `ubo`, bindings 1..3 the views (the dummy where a
 * pass has fewer). The sampler is the nearest one: every pass reads with
 * texelFetch, which ignores it, as Metal's texture.read() has none. */
static void vk_pass_write_set(VkDescriptorSet set, const vk_buffer* ubo,
                              VkImageView t0, VkImageView t1, VkImageView t2)
{
    const VkImageView v[VK_PASS_TEXTURES] = { t0, t1, t2 };
    VkDescriptorBufferInfo bi = { ubo->buf, 0, ubo->size };
    VkDescriptorImageInfo ii[VK_PASS_TEXTURES];
    VkWriteDescriptorSet w[1 + VK_PASS_TEXTURES];
    w[0] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                   .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi };
    for (u32 k = 0; k < VK_PASS_TEXTURES; k++) {
        ii[k] = (VkDescriptorImageInfo){ s_vk.sampler, v[k] ? v[k] : s_vk.dummy.view, VK_IMAGE_LAYOUT_GENERAL };
        w[1 + k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                           .dstBinding = 1 + k, .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           .pImageInfo = &ii[k] };
    }
    pvkUpdateDescriptorSets(s_vk.device, 1 + VK_PASS_TEXTURES, w, 0, NULL);
}

/* One pass into the command buffer being recorded: ordered after everything
 * before it (the previous pass's writes, the waited-on submissions), then the
 * full-screen triangle over w x h. */
static void vk_pass_record(VkRenderPass rp, VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set,
                           u32 w, u32 h)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    vk_gprof_stamp(GP_BGAP, w, h);
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_lp.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_gprof_stamp(GP_BPASS, w, h);
}

/* Frames in flight: park the active slot (its frame is on the GPU), make the
 * other one active, and wait for ITS previous frame -- normally finished while
 * this one was recorded -- then free what that frame held. */
static void vk_fif_advance(void)
{
    const u32 out = s_fif_cur, in = out ^ 1u;
#define FIF_SW(f) s_fif_bs[out].f = s_batch.f; s_batch.f = s_fif_bs[in].f;
    FIF_SW(varena) FIF_SW(iarena) FIF_SW(uarena) FIF_SW(sarena) FIF_SW(garena)
    FIF_SW(dpool) FIF_SW(dsets) FIF_SW(n_dead_fb) FIF_SW(n_dead_img)
    FIF_SW(dead_vb) FIF_SW(n_dead_vb) FIF_SW(cap_dead_vb)
#undef FIF_SW
    memcpy(s_fif_bs[out].dead_fb, s_batch.dead_fb, sizeof s_batch.dead_fb);
    memcpy(s_batch.dead_fb, s_fif_bs[in].dead_fb, sizeof s_batch.dead_fb);
    memcpy(s_fif_bs[out].dead_img, s_batch.dead_img, sizeof s_batch.dead_img);
    memcpy(s_batch.dead_img, s_fif_bs[in].dead_img, sizeof s_batch.dead_img);
    vk_lp_swap(out, in);
    s_dset_gen++;                              /* the cached sets are the parked pool's */
    s_fif_cur = in;
    s_vk.cmd = s_fif_cmd[in];
    s_vk.fence = s_fif_fence[in];
    if (s_fif_acq[in]) s_vk.sem_acquire = s_fif_acq[in];
    if (s_fif_pending[in]) {
        const uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
        pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX);
        if (ft0) ps3_ft_span(PS3_FT_GPU_WAIT, 0, 1u, 0, ft0, ps3_ft_now());
        s_fif_pending[in] = 0;
        vk_batch_completed();                  /* its arenas, sets, dead objects */
    }
    /* the cursors and per-batch counters are the open batch's: none is open,
     * and this slot's arenas are idle (never used, or just completed) */
    s_batch.vcur = s_batch.icur = s_batch.ucur = s_batch.scur = s_batch.gcur = 0;
    s_batch.zrgba_pending = 0;
    s_batch.light_pending = s_batch.ssao_pending = 0;
}

/* A backend-owned RGBA8 COLOR object (cleared to transparent black). */
static u32 vk_pass_color(void* user, u32 w, u32 h, u32 tag)
{
    const u32 hd = vk_eng_color_target_create(user, RSX_BE_FMT_R8G8B8A8, w, h, NULL, 0);
    if (hd) s_eobj[hd].tag = tag;
    return hd;
}

/* An intermediate attachment the engine never sees, left in GENERAL. */
static int vk_pass_image(vk_image* im, VkFormat fmt, u32 w, u32 h)
{
    if (vk_create_image(im, fmt, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT) || vk_begin()) {
        vk_destroy_image(im); return -1;
    }
    vk_barrier_image(im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!s_batch.recording && vk_submit_and_wait()) { vk_destroy_image(im); return -1; }
    return 0;
}

/* rsx_draw_backend.color_snapshot (Metal: eng_color_snapshot): one persistent
 * copy per source surface, refilled by a copy wherever in the stream it is
 * asked for, so the draw after it reads what the passes before it wrote.
 * Kept by source handle; a source recreated at another size gets a new copy
 * (Metal leaves the old one alive; here it is released first -- the engine
 * only holds a copy until its next request for the same surface). */
static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    const vk_eng_obj* s = vk_eng_get(surface, VK_ENG_COLOR);
    if (!s) return 0;
    if (s->fmt != VK_FORMAT_R8G8B8A8_UNORM) return 0;     /* the one target format (E1) */
    u32 dst = 0, slot = VK_CSNAP_MAX;
    for (u32 i = 0; i < s_csnap_count; i++)
        if (s_csnap[i].src == surface) {
            slot = i;
            const vk_eng_obj* d = vk_eng_get(s_csnap[i].dst, VK_ENG_COLOR);
            if (d && d->tag == VK_TAG_CSNAP && d->w == s->w && d->h == s->h && d->fmt == s->fmt)
                dst = s_csnap[i].dst;
            break;
        }
    if (!dst) {
        if (slot < VK_CSNAP_MAX) {                         /* stale: the surface was recreated */
            if (vk_eng_get(s_csnap[slot].dst, VK_ENG_COLOR) &&
                s_eobj[s_csnap[slot].dst].tag == VK_TAG_CSNAP)
                vk_eng_release(user, s_csnap[slot].dst);
            s_csnap[slot].src = s_csnap[slot].dst = 0;
        } else {
            for (u32 i = 0; i < s_csnap_count && slot == VK_CSNAP_MAX; i++)
                if (!s_csnap[i].src) slot = i;
            if (slot == VK_CSNAP_MAX) {
                if (s_csnap_count >= VK_CSNAP_MAX) return 0;
                slot = s_csnap_count++;
            }
        }
        if (!(dst = vk_pass_color(user, s->w, s->h, VK_TAG_CSNAP))) return 0;
        s_csnap[slot].src = surface;
        s_csnap[slot].dst = dst;
    }
    const vk_eng_obj* d = &s_eobj[dst];
    if (vk_begin()) return 0;
    if (s_batch.recording) vk_barrier_all();
    vk_barrier_image(s->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(d->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy r = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .extent = { s->w, s->h, 1 } };
    pvkCmdCopyImage(s_vk.cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    if (s_batch.recording) return dst;      /* GPU to GPU: stays in the batch */
    return vk_submit_and_wait() ? 0 : dst;
}

static void vk_pass_drop_light(void)
{
    if (s_lp.light_fb) { vk_batch_flush(); vk_fif_wait_other(); pvkDestroyFramebuffer(s_vk.device, s_lp.light_fb, NULL); }
    s_lp.light_fb = VK_NULL_HANDLE;
    for (u32 k = 0; k < 2; k++) {
        if (vk_eng_get(s_lp.light_out[k], VK_ENG_COLOR) && s_eobj[s_lp.light_out[k]].tag == VK_TAG_PASS)
            vk_eng_release(NULL, s_lp.light_out[k]);
        s_lp.light_out[k] = 0;
    }
    s_lp.light_w = s_lp.light_h = 0;
}

static int vk_pass_out_live(u32 handle)
{
    const vk_eng_obj* o = vk_eng_get(handle, VK_ENG_COLOR);
    return o && o->tag == VK_TAG_PASS;
}

/* rsx_draw_backend.light_pass (Metal: eng_light_pass, eng_light_fs): one
 * full-screen pass with two RGBA8 targets, diffuse and specular, kept across
 * frames and remade when the size changes. `normals` is the engine's colour
 * copy, `depth` its depth snapshot (R32F); the u, v the shader divides by are
 * params->width/height, which the engine sets to w, h (no PS3_RES_SCALE here,
 * so Metal's override of them never applies). */
static u32 vk_eng_light_pass(void* user, u32 normals, u32 depth, u32 w, u32 h,
                             const void* params, u32 param_bytes, u32* spec_out)
{
    const VkImageView nv = vk_eng_sample_view(normals), zv = vk_eng_sample_view(depth);
    if (!nv || !zv || !w || !h || w > 4096u || h > 4096u || !params || !spec_out) return 0;
    if (vk_pass_init()) return 0;
    VkRenderPass rp = vk_pass_render_pass(&s_lp.rp_light, VK_FORMAT_R8G8B8A8_UNORM, 2);
    VkPipeline pipe = vk_pass_pipeline(VK_PASS_LIGHT, kVkLightFS, "GPU light", rp, 2);
    if (!pipe) return 0;
    if (!s_lp.light_fb || s_lp.light_w != w || s_lp.light_h != h ||
        !vk_pass_out_live(s_lp.light_out[0]) || !vk_pass_out_live(s_lp.light_out[1])) {
        vk_pass_drop_light();
        for (u32 k = 0; k < 2; k++)
            if (!(s_lp.light_out[k] = vk_pass_color(user, w, h, VK_TAG_PASS))) { vk_pass_drop_light(); return 0; }
        const VkImageView views[2] = { s_eobj[s_lp.light_out[0]].im.view, s_eobj[s_lp.light_out[1]].im.view };
        if (!(s_lp.light_fb = vk_pass_framebuffer(rp, views, 2, w, h))) { vk_pass_drop_light(); return 0; }
        s_lp.light_w = w; s_lp.light_h = h;
    }
    /* Batched, the pass rides in the frame's batch: its outputs are only
     * sampled by later draws (the composite), never read back, so nothing
     * waits for it. Its one UBO and descriptor set may not be rewritten while
     * a recorded pass still reads them -- a second light pass in one batch
     * flushes first (the depth-RGBA8 pass's rule). Unbatched: as before. */
    if (s_batch.recording && s_batch.light_pending) vk_batch_flush();
    const size_t cap = (size_t)s_lp.light_ubo.size;
    memset(s_lp.light_ubo.ptr, 0, cap);
    memcpy(s_lp.light_ubo.ptr, params, param_bytes < cap ? param_bytes : cap);
    vk_pass_write_set(s_lp.set[VK_PASS_LIGHT], &s_lp.light_ubo, nv, zv, VK_NULL_HANDLE);
    if (vk_begin()) return 0;
    vk_pass_record(rp, s_lp.light_fb, pipe, s_lp.set[VK_PASS_LIGHT], w, h);
    if (s_batch.recording) s_batch.light_pending = 1;
    else if (vk_submit_and_wait()) return 0;
    *spec_out = s_lp.light_out[1];
    return s_lp.light_out[0];
}

static void vk_pass_drop_ssao(void)
{
    for (u32 k = 0; k < 4; k++) {
        if (s_lp.ssao_fb[k]) { vk_batch_flush(); vk_fif_wait_other(); pvkDestroyFramebuffer(s_vk.device, s_lp.ssao_fb[k], NULL); }
        s_lp.ssao_fb[k] = VK_NULL_HANDLE;
    }
    vk_destroy_image(&s_lp.ssao_hd);
    vk_destroy_image(&s_lp.ssao_a);
    vk_destroy_image(&s_lp.ssao_b);
    if (vk_pass_out_live(s_lp.ssao_out)) vk_eng_release(NULL, s_lp.ssao_out);
    s_lp.ssao_out = 0;
    memset(s_lp.ssao_dims, 0, sizeof s_lp.ssao_dims);
}

/* rsx_draw_backend.ssao_pass (Metal: eng_ssao_pass and its ENG_REC_SSAO
 * encode): five full-screen passes over persistent targets -- half depth
 * R32F, two R8 AO planes, the RGBA8 output -- in one submission:
 *   hd <- depth;  a <- (hd, normals);  b <- blur_h(a);  a <- blur_v(b);
 *   out <- up(a, hd, depth)
 * Metal's PS3_RES_SSAO_FULL / ssao_up4s_fs variant exists for PS3_RES_SCALE
 * only, which this backend does not have: the output is always fw x fh. */
static u32 vk_eng_ssao_pass(void* user, u32 depth, u32 normals, const void* consts, u32 const_bytes,
                            u32 fw, u32 fh, u32 hw, u32 hh)
{
    const VkImageView zv = vk_eng_sample_view(depth), nv = vk_eng_sample_view(normals);
    if (!zv || !nv || !consts || !const_bytes || !fw || !fh || !hw || !hh ||
        fw > 4096u || fh > 4096u || hw > 4096u || hh > 4096u) return 0;
    if (vk_pass_init()) return 0;
    VkRenderPass rp_hd  = vk_pass_render_pass(&s_lp.rp_r32f, VK_FORMAT_R32_SFLOAT, 1);
    VkRenderPass rp_ao  = vk_pass_render_pass(&s_lp.rp_r8, VK_FORMAT_R8_UNORM, 1);
    VkRenderPass rp_out = vk_pass_render_pass(&s_lp.rp_rgba8, VK_FORMAT_R8G8B8A8_UNORM, 1);
    const VkPipeline p_hd = vk_pass_pipeline(VK_PASS_SSAO_HD, kVkSsaoHdFS, "SSAO half depth", rp_hd, 1);
    const VkPipeline p_ao = vk_pass_pipeline(VK_PASS_SSAO_AO, kVkSsaoAoFS, "SSAO occlusion", rp_ao, 1);
    const VkPipeline p_bh = vk_pass_pipeline(VK_PASS_SSAO_BLUR_H, kVkSsaoBlurHFS, "SSAO blur h", rp_ao, 1);
    const VkPipeline p_bv = vk_pass_pipeline(VK_PASS_SSAO_BLUR_V, kVkSsaoBlurVFS, "SSAO blur v", rp_ao, 1);
    const VkPipeline p_up = vk_pass_pipeline(VK_PASS_SSAO_UP, kVkSsaoUp4FS, "SSAO upsample", rp_out, 1);
    if (!p_hd || !p_ao || !p_bh || !p_bv || !p_up) return 0;

    if (!s_lp.ssao_fb[3] || s_lp.ssao_dims[0] != fw || s_lp.ssao_dims[1] != fh ||
        s_lp.ssao_dims[2] != hw || s_lp.ssao_dims[3] != hh || !vk_pass_out_live(s_lp.ssao_out)) {
        vk_pass_drop_ssao();
        if (vk_pass_image(&s_lp.ssao_hd, VK_FORMAT_R32_SFLOAT, hw, hh) ||
            vk_pass_image(&s_lp.ssao_a, VK_FORMAT_R8_UNORM, hw, hh) ||
            vk_pass_image(&s_lp.ssao_b, VK_FORMAT_R8_UNORM, hw, hh) ||
            !(s_lp.ssao_out = vk_pass_color(user, fw, fh, VK_TAG_PASS))) { vk_pass_drop_ssao(); return 0; }
        s_lp.ssao_fb[0] = vk_pass_framebuffer(rp_hd, &s_lp.ssao_hd.view, 1, hw, hh);
        s_lp.ssao_fb[1] = vk_pass_framebuffer(rp_ao, &s_lp.ssao_a.view, 1, hw, hh);
        s_lp.ssao_fb[2] = vk_pass_framebuffer(rp_ao, &s_lp.ssao_b.view, 1, hw, hh);
        s_lp.ssao_fb[3] = vk_pass_framebuffer(rp_out, &s_eobj[s_lp.ssao_out].im.view, 1, fw, fh);
        if (!s_lp.ssao_fb[0] || !s_lp.ssao_fb[1] || !s_lp.ssao_fb[2] || !s_lp.ssao_fb[3]) {
            vk_pass_drop_ssao(); return 0;
        }
        s_lp.ssao_dims[0] = fw; s_lp.ssao_dims[1] = fh; s_lp.ssao_dims[2] = hw; s_lp.ssao_dims[3] = hh;
    }
    /* Batched: recorded into the frame's batch, as the light pass (one UBO,
     * five sets: a second SSAO in one batch flushes first). */
    if (s_batch.recording && s_batch.ssao_pending) vk_batch_flush();
    memset(s_lp.ssao_ubo.ptr, 0, VK_SSAO_UBO_BYTES);
    memcpy(s_lp.ssao_ubo.ptr, consts, const_bytes < VK_SSAO_UBO_BYTES ? const_bytes : VK_SSAO_UBO_BYTES);
    const VkImageView hd = s_lp.ssao_hd.view, a = s_lp.ssao_a.view, b = s_lp.ssao_b.view;
    const struct { u32 set; VkPipeline pipe; VkRenderPass rp; VkFramebuffer fb; u32 w, h;
                   VkImageView t0, t1, t2; } steps[5] = {
        { VK_PASS_SSAO_HD,     p_hd, rp_hd,  s_lp.ssao_fb[0], hw, hh, zv, VK_NULL_HANDLE, VK_NULL_HANDLE },
        { VK_PASS_SSAO_AO,     p_ao, rp_ao,  s_lp.ssao_fb[1], hw, hh, hd, nv, VK_NULL_HANDLE },
        { VK_PASS_SSAO_BLUR_H, p_bh, rp_ao,  s_lp.ssao_fb[2], hw, hh, a, hd, VK_NULL_HANDLE },
        { VK_PASS_SSAO_BLUR_V, p_bv, rp_ao,  s_lp.ssao_fb[1], hw, hh, b, hd, VK_NULL_HANDLE },
        { VK_PASS_SSAO_UP,     p_up, rp_out, s_lp.ssao_fb[3], fw, fh, a, hd, zv } };
    for (u32 k = 0; k < 5; k++)
        vk_pass_write_set(s_lp.set[steps[k].set], &s_lp.ssao_ubo, steps[k].t0, steps[k].t1, steps[k].t2);
    if (vk_begin()) return 0;
    for (u32 k = 0; k < 5; k++)
        vk_pass_record(steps[k].rp, steps[k].fb, steps[k].pipe, s_lp.set[steps[k].set], steps[k].w, steps[k].h);
    if (s_batch.recording) { s_batch.ssao_pending = 1; return s_lp.ssao_out; }
    return vk_submit_and_wait() ? 0 : s_lp.ssao_out;
}

/* rsx_draw_backend.depth_snapshot_rgba8 (Metal: eng_depth_snapshot_rgba8,
 * eng_depth_rgba8_fs): the depth copied into an R32F scratch, then packed by
 * a full-screen pass into one persistent RGBA8 target per (depth, w, h),
 * re-rendered on every request. The engine releases a decode it replaces (a
 * crop of another size, a recreated zeta) and the slot may then be handed to
 * anything: an entry is reused only while its handle is still a COLOR object
 * carrying this depth's tag. w, h are clamped to the depth target, as
 * vk_eng_depth_snapshot does (Metal makes the w x h target regardless). */
static u32 vk_eng_depth_snapshot_rgba8(void* user, u32 depth, u32 w, u32 h)
{
    const vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !vk_eng_depth_copyable()) return 0;
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h || vk_pass_init()) return 0;
    VkRenderPass rp = vk_pass_render_pass(&s_lp.rp_rgba8, VK_FORMAT_R8G8B8A8_UNORM, 1);
    VkPipeline pipe = vk_pass_pipeline(VK_PASS_ZRGBA, kVkDepthRgba8FS, "depth-as-RGBA8", rp, 1);
    if (!pipe) return 0;

    const u32 tag = VK_TAG_ZRGBA | depth;
    u32 dst = 0, slot = VK_ZRGBA_MAX;
    for (u32 i = 0; i < s_zrgba_count; i++)
        if (s_zrgba[i].depth == depth && s_zrgba[i].w == w && s_zrgba[i].h == h) {
            slot = i;
            const vk_eng_obj* o = vk_eng_get(s_zrgba[i].dst, VK_ENG_COLOR);
            if (o && o->tag == tag && o->w == w && o->h == h) dst = s_zrgba[i].dst;
            break;
        }
    if (!dst) {
        if (!(dst = vk_pass_color(user, w, h, tag))) return 0;
        if (slot == VK_ZRGBA_MAX)
            slot = s_zrgba_count < VK_ZRGBA_MAX ? s_zrgba_count++ : depth % VK_ZRGBA_MAX;
        s_zrgba[slot].depth = depth; s_zrgba[slot].w = w; s_zrgba[slot].h = h; s_zrgba[slot].dst = dst;
    }

    VkImageLayout zl = VK_IMAGE_LAYOUT_GENERAL;
    if (!s_lp.zr32.img || s_lp.zr32_w < w || s_lp.zr32_h < h) {
        const u32 nw = w > s_lp.zr32_w ? w : s_lp.zr32_w, nh = h > s_lp.zr32_h ? h : s_lp.zr32_h;
        vk_destroy_image(&s_lp.zr32);
        vk_destroy_image(&s_lp.zs8);
        s_lp.zr32_w = s_lp.zr32_h = 0;
        if (vk_create_image(&s_lp.zr32, VK_FORMAT_R32_SFLOAT, nw, nh,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) { vk_destroy_image(&s_lp.zr32); return 0; }
        /* The stencil byte rides in the decode's blue channel, as Z24S8 read
         * as A8R8G8B8 carries it: the deferred composite tests its bit 7. */
        if ((VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT) &&
            vk_create_image(&s_lp.zs8, VK_FORMAT_R8_UNORM, nw, nh,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) vk_destroy_image(&s_lp.zs8);
        s_lp.zr32_w = nw; s_lp.zr32_h = nh;
        zl = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (vk_eng_copy_depth(z, s_lp.zr32.img, zl, w, h)) return 0;
    VkImageView sview = VK_NULL_HANDLE;
    if (s_lp.zs8.img && !vk_eng_copy_stencil(z, s_lp.zs8.img, zl, w, h)) sview = s_lp.zs8.view;

    VkFramebuffer fb = vk_pass_framebuffer(rp, &s_eobj[dst].im.view, 1, w, h);
    if (!fb) return 0;
    /* Batched: a descriptor set may not be rewritten while a recorded pass
     * still uses it, so each decode in a batch takes the next set of a ring
     * (the scratch copies are reused safely: every pass is ordered after the
     * one before by barriers); a full ring flushes. Unbatched: slot 0. */
    if (s_batch.recording && s_batch.zrgba_pending >= (int)VK_ZRGBA_SETS) vk_batch_flush();
    const VkDescriptorSet zset = s_lp.zset[s_batch.recording ? (u32)s_batch.zrgba_pending : 0u];
    vk_pass_write_set(zset, &s_lp.ssao_ubo, s_lp.zr32.view, sview, VK_NULL_HANDLE);
    int rc = vk_begin();
    if (!rc) {
        if (s_batch.recording) vk_barrier_all();
        vk_pass_record(rp, fb, pipe, zset, w, h);
        if (s_batch.recording) {             /* stays in the batch; fb dies with it */
            s_batch.zrgba_pending++;
            vk_batch_retire_fb(fb);
            return dst;
        }
        rc = vk_submit_and_wait();
    }
    pvkDestroyFramebuffer(s_vk.device, fb, NULL);
    return rc ? 0 : dst;
}

/* Before the object table goes (vk_eng_shutdown): the passes' own objects.
 * The COLOR handles they made go with the table. */
static void vk_eng5_shutdown(void)
{
    if (s_vk.device) {
        const VkDevice d = s_vk.device;
        if (s_lp.light_fb) pvkDestroyFramebuffer(d, s_lp.light_fb, NULL);
        for (u32 k = 0; k < 4; k++) if (s_lp.ssao_fb[k]) pvkDestroyFramebuffer(d, s_lp.ssao_fb[k], NULL);
        vk_destroy_image(&s_lp.ssao_hd);
        vk_destroy_image(&s_lp.ssao_a);
        vk_destroy_image(&s_lp.ssao_b);
        vk_destroy_image(&s_lp.zr32);
        vk_destroy_image(&s_lp.zs8);
        for (u32 k = 0; k < VK_PASS_COUNT; k++) {
            if (s_lp.pipe[k]) pvkDestroyPipeline(d, s_lp.pipe[k], NULL);
            if (s_lp.fs[k])   pvkDestroyShaderModule(d, s_lp.fs[k], NULL);
        }
        if (s_lp.vs) pvkDestroyShaderModule(d, s_lp.vs, NULL);
        VkRenderPass* rps[4] = { &s_lp.rp_light, &s_lp.rp_rgba8, &s_lp.rp_r8, &s_lp.rp_r32f };
        for (u32 k = 0; k < 4; k++) if (*rps[k]) pvkDestroyRenderPass(d, *rps[k], NULL);
        vk_destroy_buffer(&s_lp.light_ubo);
        vk_destroy_buffer(&s_lp.ssao_ubo);
        if (s_lp.pool)        pvkDestroyDescriptorPool(d, s_lp.pool, NULL);
        if (s_lp.pipe_layout) pvkDestroyPipelineLayout(d, s_lp.pipe_layout, NULL);
        if (s_lp.set_layout)  pvkDestroyDescriptorSetLayout(d, s_lp.set_layout, NULL);
    }
    memset(&s_lp, 0, sizeof s_lp);
    memset(s_csnap, 0, sizeof s_csnap);  s_csnap_count = 0;
    memset(s_zrgba, 0, sizeof s_zrgba);  s_zrgba_count = 0;
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_create_3d    = vk_eng_texture_create_3d,
    .texture_upload_3d    = vk_eng_texture_upload_3d,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .color_snapshot       = vk_eng_color_snapshot,
    .depth_snapshot_rgba8 = vk_eng_depth_snapshot_rgba8,
    .light_pass           = vk_eng_light_pass,
    .ssao_pass            = vk_eng_ssao_pass,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_vs_constants_range = vk_eng_bind_vs_constants_range,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .set_depth_bias       = vk_eng_set_depth_bias,
    .set_depth_clamp      = vk_eng_set_depth_clamp,
    .draw                 = vk_eng_draw,
    .vbuf_create          = vk_eng_vbuf_create,
    .vbuf_release         = vk_eng_vbuf_release,
    .draw_vbuf            = vk_eng_draw_vbuf,
    .draw_vbuf_ib         = vk_eng_draw_vbuf_ib,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
            char why[256];
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    static const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    /* User clip planes (rsx_vp_apply_user_clip) write ClipDistance. */
    want.shaderClipDistance = have.shaderClipDistance;
    /* Per-attachment write masks and blend (COLOR_MASK_MRT/BLEND_ENABLE_MRT):
     * without independentBlend every attachment must share one state. */
    want.independentBlend = have.independentBlend;
    s_vk_indep_blend = have.independentBlend ? 1 : 0;
    /* NV4097_SET_ZMIN_MAX_CONTROL: a draw the RSX clamps at the near/far
     * planes instead of clipping (eng_apply_depth_clip). */
    want.depthClamp = have.depthClamp;
    vk_postfx_device_features(&have, &want);   /* [postfx] samplerAnisotropy */
    s_vk_depth_clamp_ok = have.depthClamp ? 1 : 0;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    vk_sc_open(title);               /* the shader + pipeline disk caches */
    vk_postfx_init(s_vk.instance, s_vk.phys, s_vk.device, pvkGetInstanceProcAddr,   /* [postfx] */
                   pvkGetDeviceProcAddr, s_sc.pc);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    cai.commandBufferCount = VK_FIF_MAX;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, s_fif_cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (u32 k = 0; k < VK_FIF_MAX; k++)
        VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_fif_fence[k]), "vkCreateFence");
    s_fif_cur = 0;
    s_vk.cmd = s_fif_cmd[0];
    s_vk.fence = s_fif_fence[0];

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT /* [postfx] reads it */, VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_DS_ASPECT)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { VK_DS_ASPECT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_fif_acq[0]) != VK_SUCCESS ||
            pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_fif_acq[1]) != VK_SUCCESS ||
            (s_vk.sem_acquire = s_fif_acq[s_fif_cur], vk_create_swapchain()))
            vk_window_drop("the swapchain could not be created");
    }
    if (s_vk.windowed && s_vk.swapchain) {   /* [overlay] the live settings and the HUD / menu */
        ps3_setting_register("PS3RECOMP_FPS_CAP", vk_set_fps_cap);
        ps3_setting_register("PS3RECOMP_VK_PRESENT", vk_set_present);
        ps3_setting_register("PS3RECOMP_VK_FULLSCREEN", vk_set_fullscreen);
        const vk_overlay_attach_info oi = {
            pvkGetInstanceProcAddr, s_vk.instance, s_vk.phys, s_vk.device, s_vk.queue_family, s_vk.queue,
            s_vk.window, s_vk.width, s_vk.height };
        vk_overlay_attach(&oi);
    }
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    /* PS3RECOMP_VK_TEX3D=0 (A/B): no 3D textures -- a volume is its first
     * slice sampled in 2D, as before vk_eng_texture_create_3d. */
    { static rsx_draw_backend no3d;
      const char* e = getenv("PS3RECOMP_VK_TEX3D");
      if (e && e[0] == '0') {
          no3d = s_vk_engine_backend; no3d.texture_create_3d = NULL; no3d.texture_upload_3d = NULL;
          rsx_draw_engine_set_backend(&no3d);
          VK_LOG("engine: 3D textures off (PS3RECOMP_VK_TEX3D=0)\n");
      } else {
          rsx_draw_engine_set_backend(&s_vk_engine_backend);
      } }
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

static int s_vk_holding;   /* the shutdown hold loop is running: Esc ends it (pump_messages) */
void rsx_vulkan_backend_shutdown(void)
{
    vk_tq_stop();                        /* PS3RECOMP_VK_THREADED: drain, real entry points back */
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        s_vk_holding = 1;
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
        s_vk_holding = 0;
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        if (pvkDeviceWaitIdle) pvkDeviceWaitIdle(s_vk.device);
        vk_postfx_shutdown();            /* [postfx] */
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        vk_sc_close();
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    free(s_vk.cpu_verts);
    memset(&s_vk, 0, sizeof s_vk);
}

/* ---- PS3RECOMP_PLAYLOG=<file>: a play session's frame record (tools/play.ps1) ----
 * Light enough to leave on while playing: one line per second (fps, frame-time
 * p50/p99/max, frames over 17/25/33/50 ms, guest draws per frame) and one per
 * frame over 50 ms (HITCH), flushed once a second so a killed game loses at
 * most a second; F9 in the game window writes a MARK ("that felt bad").
 * Read by tools/playlog_report.py. */
static struct {
    int    state;                       /* 0 unread, 1 on, -1 off */
    FILE*  f;
    double t0, last, sec_start;
    float  dt[4096];
    u32    n;
    u32    draws0;
    int    mark;
} s_pl;
static double vk_pl_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
static int vk_pl_cmp(const void* a, const void* b)
{
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}
u32 rsx_vulkan_backend_guest_draws(void);
static void vk_playlog_frame(void)
{
    if (s_pl.state == 0) {
        const char* path = getenv("PS3RECOMP_PLAYLOG");
        s_pl.state = -1;
        if (path && *path && (s_pl.f = fopen(path, "w")) != NULL) {
            s_pl.state = 1;
            s_pl.t0 = vk_pl_now();
            fprintf(s_pl.f, "#t_s	fps	p50_ms	p99_ms	max_ms	n17	n25	n33	n50	draws_per_frame\n");
            fflush(s_pl.f);
        }
    }
    if (s_pl.state != 1) return;
    const double now = vk_pl_now();
    if (s_pl.last == 0.0) { s_pl.last = s_pl.sec_start = now; s_pl.draws0 = rsx_vulkan_backend_guest_draws(); return; }
    const double dt = now - s_pl.last;
    s_pl.last = now;
    if (s_pl.n < sizeof s_pl.dt / sizeof s_pl.dt[0]) s_pl.dt[s_pl.n++] = (float)(dt * 1000.0);
    if (dt > 0.050) fprintf(s_pl.f, "HITCH	%.2f	%.1f\n", now - s_pl.t0, dt * 1000.0);
    if (s_pl.mark) { fprintf(s_pl.f, "MARK	%.2f\n", now - s_pl.t0); s_pl.mark = 0; }
    if (now - s_pl.sec_start < 1.0 || !s_pl.n) return;
    const u32 draws = rsx_vulkan_backend_guest_draws();
    u32 c17 = 0, c25 = 0, c33 = 0, c50 = 0;
    for (u32 i = 0; i < s_pl.n; i++) {
        const float d = s_pl.dt[i];
        c17 += d > 17.5f; c25 += d > 25.0f; c33 += d > 33.4f; c50 += d > 50.0f;
    }
    qsort(s_pl.dt, s_pl.n, sizeof s_pl.dt[0], vk_pl_cmp);
    const double span = now - s_pl.sec_start;
    fprintf(s_pl.f, "%.1f	%.1f	%.1f	%.1f	%.1f	%u	%u	%u	%u	%.0f\n",
            now - s_pl.t0, s_pl.n / span, s_pl.dt[s_pl.n / 2], s_pl.dt[(s_pl.n * 99) / 100],
            s_pl.dt[s_pl.n - 1], c17, c25, c33, c50, (double)(draws >= s_pl.draws0 ? draws - s_pl.draws0 : 0u) / s_pl.n);
    fflush(s_pl.f);
    s_pl.draws0 = draws;
    s_pl.n = 0;
    s_pl.sec_start = now;
}

/* ---- window events and the mouse (cellPad.c owns the decisions) -----------
 * Esc no longer quits: with a mouse-driven camera it is the key players hit
 * to get their cursor back. It goes to cellPad (cellPad_host_window_event):
 * the in-game menu's open/close counter, and releasing the captured mouse.
 * Quit = closing the window or Alt+F4 (SDL turns both into SDL_QUIT).
 * PS3_ESC=quit restores Esc = quit; while the last frame is held on screen
 * after the game ended (PS3RECOMP_VK_HOLD) Esc still ends it.
 * The mouse is captured (cursor hidden, kept in the window) only after a
 * click in the window, and only while the window has the focus and no menu
 * is open; the motion itself comes from raw input on cellPad's own thread. */
int  cellPad_host_window_event(int ev);          /* 1 click, 2 Esc; 1 = swallowed */
void cellPad_host_window_focus(int focused);
int  cellPad_host_capture_wanted_now(void);      /* 0 none, 1 motion only, 2 hold the cursor */
void cellPad_host_set_mouse_captured(int on);
static int s_cap_state;                          /* what the window applied (0/1/2) */

static int vk_esc_quits(void)
{
    static int q = -1;
    if (q < 0) { const char* e = getenv("PS3_ESC"); q = (e && !strcmp(e, "quit")) ? 1 : 0; }
    return q;
}

static void vk_mouse_capture_sync(void)
{
    if (!s_vk.window) return;
    const int focused = (SDL_GetWindowFlags(s_vk.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    cellPad_host_window_focus(focused);
    const int want = cellPad_host_capture_wanted_now();
    if (want == s_cap_state) return;
    if (want == 2) {
        SDL_SetWindowGrab(s_vk.window, SDL_TRUE);   /* SDL re-clips/unclips it on focus changes */
        SDL_ShowCursor(SDL_DISABLE);
    } else if (s_cap_state == 2) {
        SDL_SetWindowGrab(s_vk.window, SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
        if (focused) {                               /* give the cursor back in the middle */
            int w = 0, h = 0;
            SDL_GetWindowSize(s_vk.window, &w, &h);
            SDL_WarpMouseInWindow(s_vk.window, w / 2, h / 2);
        }
    }
    s_cap_state = want;
    cellPad_host_set_mouse_captured(want != 0);
}

int rsx_vulkan_backend_pump_messages(void)
{
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    vk_overlay_poll();                       /* [overlay] the L3+R3 hold, a test's timed menu */
    while (SDL_PollEvent(&e)) {
        if (vk_overlay_event(&e)) continue;  /* [overlay] F1/F2/Alt+Enter; all input while the menu is open */
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
            if (s_vk_holding || vk_esc_quits()) quit = 1;
            /* the window's Esc (cellPad: unless [keyboard] gives it to the game): the
             * PC menu opens [overlay]; the mouse is free while it is open and
             * captured again when it closes */
            else if (!e.key.repeat && cellPad_host_window_event(2)) vk_overlay_open_menu();
        }
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9 && !e.key.repeat) s_pl.mark = 1;
        else if (e.type == SDL_MOUSEBUTTONDOWN) cellPad_host_window_event(1);
    }
    vk_mouse_capture_sync();
    return quit ? -1 : 0;
}

void rsx_vulkan_backend_present(void)
{
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
    vk_sc_tick();                    /* save the pipeline cache once a burst ends */
    vk_playlog_frame();
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

#endif /* !_WIN32 || PS3RECOMP_RSX_VULKAN */
