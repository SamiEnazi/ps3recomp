/*
 * ps3recomp - Vulkan image quality: SMAA / FXAA, CAS sharpening, anisotropic
 * filtering. See vk_postfx.h for what it does and its settings.
 *
 * Built only with the Vulkan backend (PS3RECOMP_RSX_VULKAN). It loads its own
 * device entry points through the backend's vkGetDeviceProcAddr, owns its
 * images, passes and pipelines, and records into the present's command buffer;
 * the backend calls five hooks (vk_postfx.h) and nothing else of it.
 *
 * The passes are third-party shader libraries compiled at run time with
 * glslang (rsx_glsl_to_spirv), each kept as its authors ship it and turned
 * into a C string by tools/gen_glsl_header.py:
 *   SMAA 1x     third_party/smaa/SMAA.hlsl (MIT), SMAA_GLSL_4, high preset,
 *               luma edges: edges (RG8) -> blend weights (RGBA8, with the
 *               AreaTex/SearchTex lookups) -> neighbourhood blend
 *   FXAA 3.11   third_party/fxaa/FXAA3_11.h (NVIDIA, BSD-3), FXAA_GLSL_130,
 *               quality preset 29, green as luma
 *   CAS         third_party/ffx_cas/ffx_{a,cas}.h (AMD, MIT), CasFilter with
 *               the gamma-2 input the CAS docs give for non-linear colour
 *               (and its scaling form for PS3RECOMP_UPSCALE=cas)
 *   FSR 1       third_party/ffx_fsr1/ffx_{a,fsr1}.h (AMD, MIT): EASU + RCAS
 *               when the window is larger than the frame
 * Only the small GLSL wrappers below (entry points, bindings, push constants)
 * are ours.
 *
 * Vulkan has texture row 0 at the top and clip-space +y down, as D3D does, so
 * SMAA's D3D conventions hold unchanged: uv = (0,0) is the first row in memory
 * and the lookup textures are uploaded as they are (no GL flip).
 *
 * Every image stays in VK_IMAGE_LAYOUT_GENERAL, as in the backend, and every
 * pass starts with a full memory barrier: ordered after the frame's own
 * commands, and after the previous frame's post passes (frames in flight run
 * in queue order). Descriptor sets name fixed images and are written only when
 * an image is (re)made, which waits for the device first -- so a set is never
 * rewritten under a frame in flight. Per-frame values go in push constants.
 *
 * Internal resolution (PS3RECOMP_RENDER_SCALE): parsed here (the setting, read
 * at start), drawn by the backend (rsx_vulkan_backend.c, the [res] block, done
 * 2026-11-09 along the survey below): the frame this module is handed is then
 * that much larger than 1280x720. Same size as the window: AA and CAS at that
 * size; larger than the window on both axes: AA, then an area downsample
 * (PS_DOWN), then CAS at the window's size. The survey, as it was:
 * Rendering the game larger is the backend's
 * business, as rsx_metal_backend.m's PS3_RES_SCALE does it; on Vulkan it needs,
 * in rsx_vulkan_backend.c (2026-11-08 survey):
 *   - vk_eng_obj: the guest's size AND the image's size (o->w/h is both today,
 *     ~23 uses); colour/depth targets, surface views, depth snapshots, colour
 *     snapshots, depth decodes made at res_px(w) x res_px(h); seeds
 *     nearest-upscaled (vk_eng_color_target_create, vk_eng_texture_upload
 *     into a target)
 *   - framebuffers, render areas, vk_eng_set_viewport / vk_eng_set_scissor
 *     scaled with Metal's edge rounding; the scratch depth at the scaled size
 *   - vk_eng_light_pass: params width/height = the scaled size;
 *     vk_eng_ssao_pass: the GLSL `rs` (a constant 1 in rsx_vk_passes_glsl.h)
 *     = the scale, and Metal's ssao_up4s_fs (full-size upsample) ported
 *   - vk_eng_readback: point-sample the scaled image down to the guest's pixels
 *   - rsx_fp_set_wpos_scale_xy(s, s) at init (the shared decompiler already
 *     emits the WPOS divide and the shadow-lookup fix)
 *   - vk_eng_present: hand the scaled display surface straight to this module
 *     (not the 1280x720 blit) and add a downsample pass here
 * Gate: the shot suite unchanged at 1x; light, SSAO, shadows and SPU readbacks
 * A/B-checked at 1.5x and 2x.
 */
#if defined(PS3RECOMP_RSX_VULKAN)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "vk_postfx.h"
#include "rsx_shader_spirv.h"
#include "../../include/ps3emu/ps3_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>   /* [wide] EnumDisplaySettingsW: the screen's shape for PS3RECOMP_ASPECT=auto */
#endif

#include "../../third_party/smaa/smaa_glsl.h"
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"
#include "../../third_party/fxaa/fxaa_glsl.h"
#include "../../third_party/ffx_cas/ffx_a_glsl.h"
#include "../../third_party/ffx_cas/ffx_cas_glsl.h"
#include "../../third_party/ffx_fsr1/ffx_a_glsl.h"
#include "../../third_party/ffx_fsr1/ffx_fsr1_glsl.h"

#define PFX_LOG(...) fprintf(stderr, "[RSX vulkan] postfx: " __VA_ARGS__)

/* ---- entry points ------------------------------------------------------- */
#define PFX_INSTANCE_FNS(X) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties)
#define PFX_DEVICE_FNS(X) \
    X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)
#define PFX_DECLARE(name) static PFN_##name q##name;
PFX_INSTANCE_FNS(PFX_DECLARE)
PFX_DEVICE_FNS(PFX_DECLARE)

/* ---- settings ----------------------------------------------------------- */
/* Ints, read and written with __atomic builtins: the menu sets them from its
 * own thread, the present and the sampler path read them on the RSX thread. */
static int s_set_aa      = VK_POSTFX_AA_SMAA;
static int s_set_sharpen = 300;       /* CAS sharpness x 1000 */
static int s_set_aniso   = 16;
static int s_set_scale   = 4;         /* quarter steps: 4 = 1x */
static int s_set_up      = VK_POSTFX_UP_FSR;
static int s_env_done;
static int s_dev_aniso   = -1;        /* the device's limit; -1 = not known yet, 0 = none */

#define PFX_LOAD(v)     __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define PFX_STORE(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELAXED)

static int pfx_parse_aa(const char* v)
{
    if (!v) return -1;
    if (!strcmp(v, "smaa") || !strcmp(v, "SMAA") || !strcmp(v, "2")) return VK_POSTFX_AA_SMAA;
    if (!strcmp(v, "fxaa") || !strcmp(v, "FXAA") || !strcmp(v, "1")) return VK_POSTFX_AA_FXAA;
    if (!strcmp(v, "off") || !strcmp(v, "OFF") || !strcmp(v, "0") || !*v) return VK_POSTFX_AA_OFF;
    return -1;
}
static int pfx_parse_num(const char* v, double* out)
{
    if (!v || !*v) return -1;
    char* end = NULL;
    const double d = strtod(v, &end);
    if (end && (*end == 'x' || *end == 'X')) end++;
    if (!end || *end || !(d == d)) return -1;
    *out = d;
    return 0;
}

static void pfx_env(void);
static int pfx_set_one(const char* name, const char* value, int from_env)
{
    double d = 0.0;
    if (!strcmp(name, "PS3RECOMP_AA")) {
        const int m = pfx_parse_aa(value);
        if (m < 0) goto bad;
        vk_postfx_set_aa(m);
    } else if (!strcmp(name, "PS3RECOMP_SHARPEN")) {
        if (pfx_parse_num(value, &d)) goto bad;
        vk_postfx_set_sharpen((float)d);
    } else if (!strcmp(name, "PS3RECOMP_ANISO")) {
        if (pfx_parse_num(value, &d)) goto bad;
        vk_postfx_set_aniso((int)d);
    } else if (!strcmp(name, "PS3RECOMP_UPSCALE")) {
        const int m = !strcmp(value, "fsr") || !strcmp(value, "FSR") ? VK_POSTFX_UP_FSR
                    : !strcmp(value, "cas") || !strcmp(value, "CAS") ? VK_POSTFX_UP_CAS
                    : !strcmp(value, "bilinear") || !strcmp(value, "off") || !strcmp(value, "0") ? VK_POSTFX_UP_BILINEAR : -1;
        if (m < 0) goto bad;
        vk_postfx_set_upscale(m);
    } else if (!strcmp(name, "PS3RECOMP_RENDER_SCALE")) {
        /* [res] the names Metal's PS3_RES_SCALE takes, too */
        if (!strcmp(value, "720p") || !strcmp(value, "native")) d = 1.0;
        else if (!strcmp(value, "1080p")) d = 1.5;
        else if (!strcmp(value, "1440p")) d = 2.0;
        else if (!strcmp(value, "4k") || !strcmp(value, "4K") || !strcmp(value, "2160p")) d = 3.0;
        else if (pfx_parse_num(value, &d)) goto bad;
        if (!(d >= 1.0 && d <= 4.0)) goto bad;
        vk_postfx_set_render_scale((float)d);
    } else {
        return -1;
    }
    return 0;
bad:
    if (from_env) PFX_LOG("%s=%s not understood; the default stays\n", name, value);
    return -1;
}
/* The in-game menu's live appliers (ps3_settings.h): 1 = took effect now. */
static int pfx_apply_aa(const char* v)      { return pfx_set_one("PS3RECOMP_AA", v, 0) == 0; }
static int pfx_apply_sharpen(const char* v) { return pfx_set_one("PS3RECOMP_SHARPEN", v, 0) == 0; }
static int pfx_apply_aniso(const char* v)   { return pfx_set_one("PS3RECOMP_ANISO", v, 0) == 0; }
static int pfx_apply_upscale(const char* v) { return pfx_set_one("PS3RECOMP_UPSCALE", v, 0) == 0; }

static void pfx_env(void)
{
    if (s_env_done) return;
    s_env_done = 1;
    static const char* const names[] = { "PS3RECOMP_AA", "PS3RECOMP_SHARPEN", "PS3RECOMP_ANISO",
                                         "PS3RECOMP_RENDER_SCALE", "PS3RECOMP_UPSCALE" };
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        const char* v = getenv(names[k]);
        if (v) pfx_set_one(names[k], v, 1);
    }
    /* [res] Metal's name for it, when the menu's is not set */
    if (!getenv("PS3RECOMP_RENDER_SCALE") && getenv("PS3_RES_SCALE"))
        pfx_set_one("PS3RECOMP_RENDER_SCALE", getenv("PS3_RES_SCALE"), 1);
    /* RENDER_SCALE is not registered: not live (the targets are made at start). */
    ps3_setting_register("PS3RECOMP_AA", pfx_apply_aa);
    ps3_setting_register("PS3RECOMP_SHARPEN", pfx_apply_sharpen);
    ps3_setting_register("PS3RECOMP_ANISO", pfx_apply_aniso);
    ps3_setting_register("PS3RECOMP_UPSCALE", pfx_apply_upscale);
}

void vk_postfx_set_aa(int mode)
{
    pfx_env();
    PFX_STORE(s_set_aa, (mode == VK_POSTFX_AA_FXAA || mode == VK_POSTFX_AA_SMAA) ? mode : VK_POSTFX_AA_OFF);
}
int vk_postfx_aa(void) { pfx_env(); return PFX_LOAD(s_set_aa); }
void vk_postfx_set_sharpen(float a)
{
    pfx_env();
    const int m = !(a > 0.0f) ? 0 : a >= 1.0f ? 1000 : (int)(a * 1000.0f + 0.5f);
    PFX_STORE(s_set_sharpen, m);
}
float vk_postfx_sharpen(void) { pfx_env(); return (float)PFX_LOAD(s_set_sharpen) / 1000.0f; }
void vk_postfx_set_aniso(int n)
{
    pfx_env();
    PFX_STORE(s_set_aniso, n < 1 ? 1 : n > 16 ? 16 : n);
}
int vk_postfx_aniso(void)
{
    pfx_env();
    const int want = PFX_LOAD(s_set_aniso), lim = s_dev_aniso;
    if (lim == 0) return 1;
    return (lim > 0 && want > lim) ? lim : want;
}
void vk_postfx_set_render_scale(float s)
{
    pfx_env();
    const int q = (int)floor(s * 4.0 + 0.5);
    PFX_STORE(s_set_scale, q < 4 ? 4 : q > 16 ? 16 : q);
}
/* [res] the setting (the backend reads it once, at the engine's init) and the
 * scale it then runs at: 1 until it says otherwise. */
static int s_scale_active = 4;        /* quarter steps */
float vk_postfx_render_scale(void) { pfx_env(); return (float)PFX_LOAD(s_scale_active) / 4.0f; }
float vk_postfx_render_scale_wanted(void) { pfx_env(); return (float)PFX_LOAD(s_set_scale) / 4.0f; }
void vk_postfx_render_scale_active(float s)
{
    const int q = (int)floor(s * 4.0 + 0.5);
    PFX_STORE(s_scale_active, q < 4 ? 4 : q > 16 ? 16 : q);
}

/* ---- [wide] PS3RECOMP_ASPECT (docs/WIDESCREEN.md) -------------------------
 * The shape of the picture, decided once: the title's game patch asks first
 * (before the game runs), the backend at its init. Nothing here changes what
 * the game does; ps3_aspect_game_set is the title saying it did. */
static int      s_asp_done, s_asp_on;
static unsigned s_asp_n = 16u, s_asp_d = 9u;
static unsigned s_asp_gw, s_asp_gh;            /* what the title's cameras see (0: nothing said) */
static int      s_asp_k1000 = 1000;            /* the backend's frame vs 16:9, x1000 */

static int pfx_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        const char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a, y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}
/* "<w>:<h>" or "<w>x<h>" (positive integers, nothing after) */
static int pfx_parse_pair(const char* v, unsigned* w, unsigned* h)
{
    char* e = NULL;
    const unsigned long a = strtoul(v, &e, 10);
    if (!e || e == v || (*e != ':' && *e != 'x' && *e != 'X')) return 0;
    const char* p = e + 1;
    const unsigned long b = strtoul(p, &e, 10);
    if (!e || e == p || *e || !a || !b || a > 100000ul || b > 100000ul) return 0;
    *w = (unsigned)a; *h = (unsigned)b;
    return 1;
}
/* The primary screen's mode: SDL_WINDOWPOS_CENTERED opens the window there, so
 * fullscreen covers it. The pixel count, not the DPI-scaled size. */
static int pfx_screen_size(unsigned* w, unsigned* h)
{
#ifdef _WIN32
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        *w = (unsigned)dm.dmPelsWidth; *h = (unsigned)dm.dmPelsHeight;
        return 1;
    }
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 0 && sh > 0) { *w = (unsigned)sw; *h = (unsigned)sh; return 1; }
#else
    (void)w; (void)h;
#endif
    return 0;
}
int ps3_aspect_wanted(unsigned* num, unsigned* den)
{
    if (!s_asp_done) {
        s_asp_done = 1;
        const char* name = "PS3RECOMP_ASPECT";
        const char* e = getenv(name);
        if (!e || !*e) { name = "PS3_ASPECT"; e = getenv(name); }
        unsigned n = 16u, d = 9u;
        const char* from = NULL;
        if (e && *e && !pfx_ieq(e, "16:9") && !pfx_ieq(e, "off") && !pfx_ieq(e, "0") && !pfx_ieq(e, "native")) {
            unsigned w = 0, h = 0;
            if (pfx_ieq(e, "auto")) {
                const char* fs = getenv("PS3RECOMP_VK_FULLSCREEN");
                const char* ws = getenv("PS3RECOMP_VK_WINDOW_SIZE");
                if (fs && fs[0] && fs[0] != '0') { if (pfx_screen_size(&w, &h)) from = "the screen"; }
                else if (ws && pfx_parse_pair(ws, &w, &h)) from = "PS3RECOMP_VK_WINDOW_SIZE";
                if (!from) { w = 16u; h = 9u; from = "a 16:9 window (no PS3RECOMP_VK_WINDOW_SIZE)"; }
            } else if (!pfx_parse_pair(e, &w, &h)) {
                char* end = NULL;
                const double r = strtod(e, &end);
                if (end && end != e && !*end && r > 0.0 && r < 100.0) { w = (unsigned)(r * 10000.0 + 0.5); h = 10000u; }
            }
            if (w && h) { n = w; d = h; }
            else fprintf(stderr, "[aspect] %s=%s not understood (16:9, auto, 21:9, 32:9, 16:10, <w>:<h>, <w>x<h> or a ratio): 16:9\n", name, e);
        }
        { unsigned a = n, b = d; while (b) { const unsigned t = a % b; a = b; b = t; } if (a > 1u) { n /= a; d /= a; } }
        const double r = (double)n / (double)d;
        if (fabs(r * 9.0 / 16.0 - 1.0) < 0.005) {          /* 16:9 (1366x768 too): the old path */
            if (e && *e && from) fprintf(stderr, "[aspect] %s=%s: %u:%u from %s = 16:9, the game's picture\n", name, e, n, d, from);
        } else if (!(r >= 1.2 && r <= 64.0 / 9.0)) {
            fprintf(stderr, "[aspect] %s=%s: %u:%u (%.3f) is outside 6:5 .. 64:9; 16:9\n", name, e, n, d, r);
        } else {
            s_asp_on = 1; s_asp_n = n; s_asp_d = d;
            fprintf(stderr, "[aspect] %s=%s: %u:%u (%.4f%s%s), hor+ (the vertical view is the game's); read at start\n",
                    name, e, n, d, r, from ? ", from " : "", from ? from : "");
        }
    }
    if (s_asp_on) { if (num) *num = s_asp_n; if (den) *den = s_asp_d; }
    return s_asp_on;
}
void ps3_aspect_game_set(unsigned w, unsigned h) { s_asp_gw = w; s_asp_gh = h; }
int ps3_aspect_game(unsigned* w, unsigned* h)
{
    if (!s_asp_gw || !s_asp_gh) return 0;
    if (w) *w = s_asp_gw;
    if (h) *h = s_asp_gh;
    return 1;
}
void vk_postfx_aspect_active(float k) { PFX_STORE(s_asp_k1000, (int)(k * 1000.0f + 0.5f)); }
float vk_postfx_aspect_k(void) { return (float)PFX_LOAD(s_asp_k1000) / 1000.0f; }
void vk_postfx_set_upscale(int mode)
{
    pfx_env();
    PFX_STORE(s_set_up, (mode == VK_POSTFX_UP_CAS || mode == VK_POSTFX_UP_BILINEAR) ? mode : VK_POSTFX_UP_FSR);
}
int vk_postfx_upscale(void) { pfx_env(); return PFX_LOAD(s_set_up); }
int vk_postfx_set(const char* name, const char* value)
{
    pfx_env();
    return (name && value) ? pfx_set_one(name, value, 0) : -1;
}

/* ---- state -------------------------------------------------------------- */
typedef struct pfx_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkFramebuffer  fb;
    uint32_t       w, h;
} pfx_image;

enum { PS_SMAA_EDGE, PS_SMAA_WEIGHT, PS_SMAA_BLEND, PS_FXAA, PS_CAS, PS_CAS_SCALE, PS_EASU, PS_RCAS,
       PS_DOWN,                            /* [res] the area downsample */
       PS_COUNT };
enum { VS_PLAIN, VS_SMAA_EDGE, VS_SMAA_WEIGHT, VS_SMAA_BLEND, VS_COUNT };
enum { DS_EDGE, DS_WEIGHT, DS_BLEND, DS_FXAA, DS_CAS_SRC, DS_CAS_AA, DS_EASU_SRC, DS_EASU_AA, DS_RCAS,
       DS_DOWN_SRC, DS_DOWN_AA, DS_CAS_DOWN,   /* [res] */
       DS_COUNT };
#define PFX_QPAIRS 4u                      /* timestamp pairs: a ring, read 3 frames later */

static struct {
    int              ok;                   /* init succeeded */
    VkDevice         dev;
    VkPhysicalDevice phys;
    VkPipelineCache  cache;
    VkPhysicalDeviceMemoryProperties mp;
    VkSampler        linear, point;
    VkRenderPass     rp_rg8, rp_rgba8_clear, rp_rgba8;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkDescriptorSet  ds[DS_COUNT];
    VkPipeline       pipe[PS_COUNT];
    unsigned char    pipe_failed[PS_COUNT];
    VkShaderModule   vs[VS_COUNT];
    int              weight_ds_luts;       /* DS_WEIGHT names the lookup textures */
    /* SMAA's lookup textures: uploaded from `staging` in the first frame that uses them */
    pfx_image        area, search;
    VkBuffer         staging;
    VkDeviceMemory   staging_mem;
    int              lut_state;            /* 0 not made, 1 made + copy recorded, -1 failed */
    /* the frame-sized targets and the window-sized CAS output */
    pfx_image        edges, weights, aa_out, out;
    pfx_image        up;                   /* FSR: EASU's output (window-sized), RCAS's input */
    pfx_image        down;                 /* [res] the area downsample's output (window-sized), CAS's input */
    VkImageView      src_view;             /* what the DS_* sets that read the frame name */
    int              aa_disabled[3];       /* a mode whose pipelines failed: off, logged once */
    /* PS3RECOMP_POSTFX_PROF */
    int              prof;
    VkQueryPool      qp;
    float            ts_period;            /* ns per tick */
    uint32_t         qframe;
    unsigned char    qused[PFX_QPAIRS];
    double           prof_sum_ms;
    uint32_t         prof_n;
    float            last_ms;
    double           prof_t0;
} P;

typedef struct { float rt[4]; float p[4]; } pfx_pc;   /* the shaders' PfxPC block */

/* ---- GLSL ----------------------------------------------------------------
 * A shader is assembled at run time from up to five parts: our head (version,
 * push constants, the library's configuration defines), a library string
 * (the generated third_party headers), and our wrapper code (bindings and
 * main). The full-screen triangle: vertex index 0..2 -> p = (0,0), (2,0),
 * (0,2), the position p * 2 - 1 (Vulkan: -1 is the top), uv = p. */
#define VK_PFX_PC \
    "layout(push_constant) uniform PfxPC { vec4 rt; vec4 p; } pc;\n"
#define VK_PFX_TRI \
    "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n" \
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"

static const char kPfxPlainVS[] =
    "#version 450\n"
    "void main() {\n" VK_PFX_TRI "}\n";

/* SMAA: rt = (1/w, 1/h, w, h) of the frame (SMAA_RT_METRICS). */
#define VK_PFX_SMAA_HEAD \
    "#version 450\n" VK_PFX_PC \
    "#define SMAA_RT_METRICS pc.rt\n" \
    "#define SMAA_GLSL_4 1\n" \
    "#define SMAA_PRESET_HIGH 1\n"
static const char kPfxSmaaVSHead[] = VK_PFX_SMAA_HEAD "#define SMAA_INCLUDE_PS 0\n";
static const char kPfxSmaaFSHead[] = VK_PFX_SMAA_HEAD "#define SMAA_INCLUDE_VS 0\n";

static const char kPfxSmaaEdgeVS[] =
    "layout(location = 0) out vec2 v_uv;\n"
    "layout(location = 1) out vec4 v_off[3];\n"
    "void main() {\n" VK_PFX_TRI
    "    vec4 off[3];\n"
    "    SMAAEdgeDetectionVS(p, off);\n"
    "    v_uv = p; v_off[0] = off[0]; v_off[1] = off[1]; v_off[2] = off[2];\n"
    "}\n";
static const char kPfxSmaaEdgeFS[] =
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 1) in vec4 v_off[3];\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec4 off[3] = vec4[3](v_off[0], v_off[1], v_off[2]);\n"
    "    o = vec4(SMAALumaEdgeDetectionPS(v_uv, off, colorTex), 0.0, 0.0);\n"
    "}\n";
static const char kPfxSmaaWeightVS[] =
    "layout(location = 0) out vec2 v_uv;\n"
    "layout(location = 1) out vec2 v_pix;\n"
    "layout(location = 2) out vec4 v_off[3];\n"
    "void main() {\n" VK_PFX_TRI
    "    vec2 pix; vec4 off[3];\n"
    "    SMAABlendingWeightCalculationVS(p, pix, off);\n"
    "    v_uv = p; v_pix = pix; v_off[0] = off[0]; v_off[1] = off[1]; v_off[2] = off[2];\n"
    "}\n";
static const char kPfxSmaaWeightFS[] =
    "layout(set = 0, binding = 0) uniform sampler2D edgesTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D areaTex;\n"
    "layout(set = 0, binding = 2) uniform sampler2D searchTex;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 1) in vec2 v_pix;\n"
    "layout(location = 2) in vec4 v_off[3];\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec4 off[3] = vec4[3](v_off[0], v_off[1], v_off[2]);\n"
    "    o = SMAABlendingWeightCalculationPS(v_uv, v_pix, off, edgesTex, areaTex, searchTex, vec4(0.0));\n"
    "}\n";
static const char kPfxSmaaBlendVS[] =
    "layout(location = 0) out vec2 v_uv;\n"
    "layout(location = 1) out vec4 v_off;\n"
    "void main() {\n" VK_PFX_TRI
    "    vec4 off;\n"
    "    SMAANeighborhoodBlendingVS(p, off);\n"
    "    v_uv = p; v_off = off;\n"
    "}\n";
static const char kPfxSmaaBlendFS[] =
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 1) in vec4 v_off;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = SMAANeighborhoodBlendingPS(v_uv, v_off, colorTex, blendTex); }\n";

/* FXAA 3.11: rt = (1/w, 1/h, w, h). The quality defaults of FXAA3_11.h's
 * integration notes: subpix 0.75, edge threshold 0.166, min 0.0833. */
static const char kPfxFxaaHead[] =
    "#version 450\n" VK_PFX_PC
    "#define FXAA_PC 1\n"
    "#define FXAA_GLSL_130 1\n"
    "#define FXAA_QUALITY__PRESET 29\n"
    "#define FXAA_GREEN_AS_LUMA 1\n"
    "#define FXAA_GATHER4_ALPHA 1\n";
static const char kPfxFxaaFS[] =
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy * pc.rt.xy;\n"
    "    o = FxaaPixelShader(uv, vec4(0.0), colorTex, colorTex, colorTex, pc.rt.xy,\n"
    "                        vec4(0.0), vec4(0.0), vec4(0.0), 0.75, 0.166, 0.0833,\n"
    "                        0.0, 0.0, 0.0, vec4(0.0));\n"
    "    o.a = 1.0;\n"
    "}\n";

/* CAS: rt = (1/out_w, 1/out_h, out_w, out_h), p = (sharpness, in_w, in_h, 0).
 * CasSetup runs per pixel (a few ALU ops; it is A_GPU-capable); loads clamp to
 * the image (CAS reads one texel around, two for the scaling form). */
static const char kPfxCasHead[] =
    "#version 450\n" VK_PFX_PC
    "#define A_GPU 1\n"
    "#define A_GLSL 1\n";
static const char kPfxCasMid[] =
    "layout(set = 0, binding = 0) uniform sampler2D srcTex;\n"
    "AF3 CasLoad(ASU2 p) { return texelFetch(srcTex, clamp(p, ASU2(0), ASU2(pc.p.yz) - 1), 0).rgb; }\n"
    "void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) { r *= r; g *= g; b *= b; }\n";
#define VK_PFX_CAS_MAIN(noscale) \
    "layout(location = 0) out vec4 o;\n" \
    "void main() {\n" \
    "    AU4 c0, c1;\n" \
    "    CasSetup(c0, c1, pc.p.x, pc.p.y, pc.p.z, pc.rt.z, pc.rt.w);\n" \
    "    AF3 c;\n" \
    "    CasFilter(c.r, c.g, c.b, AU2(gl_FragCoord.xy), c0, c1, " noscale ");\n" \
    "    o = vec4(sqrt(max(c, AF3(0.0))), 1.0);\n" \
    "}\n"
static const char kPfxCasFS[]      = VK_PFX_CAS_MAIN("true");
static const char kPfxCasScaleFS[] = VK_PFX_CAS_MAIN("false");

/* FSR 1: EASU (the upscale; rt = the output's metrics, p = (-, in_w, in_h, -),
 * reads through the linear clamp sampler with textureGather) and RCAS (the
 * sharpening at the output size; p.x = sharpness in stops, 0 = most). */
static const char kPfxFsrHead[] =
    "#version 450\n" VK_PFX_PC
    "#define A_GPU 1\n"
    "#define A_GLSL 1\n";
static const char kPfxEasuMid[] =
    "layout(set = 0, binding = 0) uniform sampler2D srcTex;\n"
    "#define FSR_EASU_F 1\n";
static const char kPfxEasuFS[] =
    "AF4 FsrEasuRF(AF2 p) { return textureGather(srcTex, p, 0); }\n"
    "AF4 FsrEasuGF(AF2 p) { return textureGather(srcTex, p, 1); }\n"
    "AF4 FsrEasuBF(AF2 p) { return textureGather(srcTex, p, 2); }\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    AU4 c0, c1, c2, c3;\n"
    "    FsrEasuCon(c0, c1, c2, c3, pc.p.y, pc.p.z, pc.p.y, pc.p.z, pc.rt.z, pc.rt.w);\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, AU2(gl_FragCoord.xy), c0, c1, c2, c3);\n"
    "    o = vec4(c, 1.0);\n"
    "}\n";
static const char kPfxRcasMid[] =
    "layout(set = 0, binding = 0) uniform sampler2D srcTex;\n"
    "#define FSR_RCAS_F 1\n";
static const char kPfxRcasFS[] =
    "AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(srcTex, clamp(p, ASU2(0), ASU2(pc.rt.zw) - 1), 0); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    AU4 con;\n"
    "    FsrRcasCon(con, pc.p.x);\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), con);\n"
    "    o = vec4(c, 1.0);\n"
    "}\n";

/* [res] A frame larger than the window (PS3RECOMP_RENDER_SCALE above the
 * window's size): each output pixel the mean of n x n bilinear taps spread
 * evenly over its footprint, n = ceil(source texels per output pixel) per
 * axis -- at 2x and 3x exactly the box of texels it covers, at 1.5x a tent
 * over ~2.5. rt = the output's metrics, p = (in_w, in_h, n_x, n_y). */
static const char kPfxDownFS[] =
    "#version 450\n" VK_PFX_PC
    "layout(set = 0, binding = 0) uniform sampler2D srcTex;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec2 k = pc.p.xy * pc.rt.xy;\n"
    "    ivec2 n = ivec2(pc.p.zw);\n"
    "    vec2 base = floor(gl_FragCoord.xy) * k;\n"
    "    vec2 dk = k / vec2(n);\n"
    "    vec2 inv = 1.0 / pc.p.xy;\n"
    "    vec3 acc = vec3(0.0);\n"
    "    for (int j = 0; j < n.y; j++)\n"
    "        for (int i = 0; i < n.x; i++)\n"
    "            acc += texture(srcTex, (base + (vec2(i, j) + 0.5) * dk) * inv).rgb;\n"
    "    o = vec4(acc / float(n.x * n.y), 1.0);\n"
    "}\n";

/* Each pass: its vertex shader (an index into the VS table) and fragment parts. */
static const char* const kPfxVS[VS_COUNT][3] = {
    [VS_PLAIN]       = { kPfxPlainVS, NULL, NULL },
    [VS_SMAA_EDGE]   = { kPfxSmaaVSHead, kSmaaGLSL, kPfxSmaaEdgeVS },
    [VS_SMAA_WEIGHT] = { kPfxSmaaVSHead, kSmaaGLSL, kPfxSmaaWeightVS },
    [VS_SMAA_BLEND]  = { kPfxSmaaVSHead, kSmaaGLSL, kPfxSmaaBlendVS },
};
static const struct { const char* what; int vs; const char* fs[5]; } kPfxPass[PS_COUNT] = {
    [PS_SMAA_EDGE]   = { "SMAA edge",          VS_SMAA_EDGE,   { kPfxSmaaFSHead, kSmaaGLSL, kPfxSmaaEdgeFS } },
    [PS_SMAA_WEIGHT] = { "SMAA blend weight",  VS_SMAA_WEIGHT, { kPfxSmaaFSHead, kSmaaGLSL, kPfxSmaaWeightFS } },
    [PS_SMAA_BLEND]  = { "SMAA neighbourhood", VS_SMAA_BLEND,  { kPfxSmaaFSHead, kSmaaGLSL, kPfxSmaaBlendFS } },
    [PS_FXAA]        = { "FXAA",               VS_PLAIN,       { kPfxFxaaHead, kFxaaGLSL, kPfxFxaaFS } },
    [PS_CAS]         = { "CAS",                VS_PLAIN,       { kPfxCasHead, kFfxAGLSL, kPfxCasMid, kFfxCasGLSL, kPfxCasFS } },
    [PS_CAS_SCALE]   = { "CAS scaling",        VS_PLAIN,       { kPfxCasHead, kFfxAGLSL, kPfxCasMid, kFfxCasGLSL, kPfxCasScaleFS } },
    [PS_EASU]        = { "FSR EASU",           VS_PLAIN,       { kPfxFsrHead, kFsrAGLSL, kPfxEasuMid, kFsr1GLSL, kPfxEasuFS } },
    [PS_RCAS]        = { "FSR RCAS",           VS_PLAIN,       { kPfxFsrHead, kFsrAGLSL, kPfxRcasMid, kFsr1GLSL, kPfxRcasFS } },
    [PS_DOWN]        = { "area downsample",    VS_PLAIN,       { kPfxDownFS } },   /* [res] */
};
/* ---- helpers ------------------------------------------------------------ */
static int pfx_memtype(uint32_t bits, VkMemoryPropertyFlags want, uint32_t* out)
{
    for (uint32_t i = 0; i < P.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (P.mp.memoryTypes[i].propertyFlags & want) == want) { *out = i; return 0; }
    return -1;
}

static void pfx_image_destroy(pfx_image* im)
{
    if (im->fb)   qvkDestroyFramebuffer(P.dev, im->fb, NULL);
    if (im->view) qvkDestroyImageView(P.dev, im->view, NULL);
    if (im->img)  qvkDestroyImage(P.dev, im->img, NULL);
    if (im->mem)  qvkFreeMemory(P.dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* A device-local 2D image + view, and a framebuffer when `rp` is given. */
static int pfx_image_make(pfx_image* im, VkFormat fmt, uint32_t w, uint32_t h,
                          VkImageUsageFlags usage, VkRenderPass rp)
{
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (qvkCreateImage(P.dev, &ici, NULL, &im->img) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    qvkGetImageMemoryRequirements(P.dev, im->img, &req);
    uint32_t type;
    if (pfx_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        pfx_memtype(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (qvkAllocateMemory(P.dev, &mai, NULL, &im->mem) != VK_SUCCESS ||
        qvkBindImageMemory(P.dev, im->img, im->mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (qvkCreateImageView(P.dev, &vci, NULL, &im->view) != VK_SUCCESS) goto fail;
    if (rp) {
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                        .renderPass = rp, .attachmentCount = 1, .pAttachments = &im->view,
                                        .width = w, .height = h, .layers = 1 };
        if (qvkCreateFramebuffer(P.dev, &fci, NULL, &im->fb) != VK_SUCCESS) goto fail;
    }
    im->w = w; im->h = h;
    return 0;
fail:
    PFX_LOG("cannot make a %ux%u image (format %d)\n", w, h, (int)fmt);
    pfx_image_destroy(im);
    return -1;
}

/* One colour attachment, GENERAL in and out (the backend's convention). */
static VkRenderPass pfx_render_pass(VkFormat fmt, VkAttachmentLoadOp load)
{
    VkAttachmentDescription att = {
        .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1, .pAttachments = &att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp = VK_NULL_HANDLE;
    return qvkCreateRenderPass(P.dev, &rci, NULL, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}

/* Compile the concatenation of `n` parts (NULL parts skipped). */
static VkShaderModule pfx_module(const char* const* parts, int n_parts, int stage, const char* what)
{
    static uint32_t spv[1u << 16];
    char log[2048];
    size_t len = 1;
    for (int k = 0; k < n_parts; k++) if (parts[k]) len += strlen(parts[k]);
    char* src = (char*)malloc(len);
    if (!src) return VK_NULL_HANDLE;
    src[0] = 0;
    for (int k = 0, at = 0; k < n_parts; k++)
        if (parts[k]) { const size_t l = strlen(parts[k]); memcpy(src + at, parts[k], l + 1); at += (int)l; }
    { const char* dump = getenv("PS3RECOMP_POSTFX_DUMP");   /* <dir>: each assembled shader, for glslangValidator */
      if (dump && *dump) {
          char path[512];
          snprintf(path, sizeof path, "%s/%s.%s", dump, what, stage == RSX_SHADER_STAGE_VERTEX ? "vert" : "frag");
          FILE* f = fopen(path, "wb");
          if (f) { fwrite(src, 1, strlen(src), f); fclose(f); } } }
    uint32_t n = 0;
    const int rc = rsx_glsl_to_spirv(src, stage, spv, (uint32_t)(sizeof spv / sizeof spv[0]), &n, log, sizeof log);
    free(src);
    if (rc || !n) {
        PFX_LOG("%s shader failed: %s\n", what, log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (qvkCreateShaderModule(P.dev, &ci, NULL, &m) != VK_SUCCESS) {
        PFX_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

static VkPipeline pfx_pipeline(int which)
{
    if (P.pipe[which] || P.pipe_failed[which]) return P.pipe[which];
    P.pipe_failed[which] = 1;
    const clock_t t0 = clock();
    const int v = kPfxPass[which].vs;
    VkShaderModule* vs = &P.vs[v];
    if (!*vs && !(*vs = pfx_module(kPfxVS[v], 3, RSX_SHADER_STAGE_VERTEX, kPfxPass[which].what)))
        return VK_NULL_HANDLE;
    VkShaderModule fs = pfx_module(kPfxPass[which].fs, 5, RSX_SHADER_STAGE_FRAGMENT, kPfxPass[which].what);
    if (!fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = *vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xFu };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &dy, .layout = P.pl,
        .renderPass = which == PS_SMAA_EDGE ? P.rp_rg8 : P.rp_rgba8, .subpass = 0 };
    const VkResult r = qvkCreateGraphicsPipelines(P.dev, P.cache, 1, &gci, NULL, &P.pipe[which]);
    qvkDestroyShaderModule(P.dev, fs, NULL);
    if (r != VK_SUCCESS) {
        PFX_LOG("%s pipeline failed (VkResult %d)\n", kPfxPass[which].what, (int)r);
        P.pipe[which] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    P.pipe_failed[which] = 0;
    PFX_LOG("%s pipeline built in %.0f ms\n", kPfxPass[which].what,
            (double)(clock() - t0) * 1000.0 / (double)CLOCKS_PER_SEC);
    return P.pipe[which];
}

static void pfx_write_set(VkDescriptorSet set, VkImageView a, VkSampler sa,
                          VkImageView b, VkSampler sb, VkImageView c, VkSampler sc)
{
    VkDescriptorImageInfo ii[3] = {
        { sa, a, VK_IMAGE_LAYOUT_GENERAL }, { sb, b ? b : a, VK_IMAGE_LAYOUT_GENERAL },
        { sc, c ? c : a, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet w[3];
    for (int k = 0; k < 3; k++)
        w[k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                       .dstBinding = (uint32_t)k, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &ii[k] };
    qvkUpdateDescriptorSets(P.dev, 3, w, 0, NULL);
}

/* Order everything recorded before (and submitted before) ahead of what follows. */
static void pfx_barrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    qvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);
}

/* An image's one-time move UNDEFINED -> GENERAL. */
static void pfx_to_general(VkCommandBuffer cmd, VkImage img)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    qvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
}

/* One full-screen pass into `dst`. */
static void pfx_pass(VkCommandBuffer cmd, int which, VkRenderPass rp, const pfx_image* dst,
                     VkDescriptorSet set, const pfx_pc* pc)
{
    pfx_barrier(cmd);
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = dst->fb,
                                  .renderArea = { { 0, 0 }, { dst->w, dst->h } },
                                  .clearValueCount = 1, .pClearValues = &clear };
    qvkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    qvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe[which]);
    VkViewport vp = { 0.0f, 0.0f, (float)dst->w, (float)dst->h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { dst->w, dst->h } };
    qvkCmdSetViewport(cmd, 0, 1, &vp);
    qvkCmdSetScissor(cmd, 0, 1, &sc);
    qvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl, 0, 1, &set, 0, NULL);
    qvkCmdPushConstants(cmd, P.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof *pc, pc);
    qvkCmdDraw(cmd, 3, 1, 0, 0);
    qvkCmdEndRenderPass(cmd);
}

/* ---- the backend's hooks ----------------------------------------------- */
void vk_postfx_device_features(const VkPhysicalDeviceFeatures* have, VkPhysicalDeviceFeatures* want)
{
    pfx_env();
    want->samplerAnisotropy = have->samplerAnisotropy;
    if (!have->samplerAnisotropy) s_dev_aniso = 0;
}

int vk_postfx_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                   PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkPipelineCache cache)
{
    pfx_env();
    memset(&P, 0, sizeof P);
#define PFX_LOAD_I(name) if (!(q##name = (PFN_##name)gipa(inst, #name))) { PFX_LOG("no " #name "\n"); return -1; }
#define PFX_LOAD_D(name) if (!(q##name = (PFN_##name)gdpa(dev, #name))) { PFX_LOG("no " #name "\n"); return -1; }
    PFX_INSTANCE_FNS(PFX_LOAD_I)
    PFX_DEVICE_FNS(PFX_LOAD_D)
    P.dev = dev; P.phys = phys; P.cache = cache;
    qvkGetPhysicalDeviceMemoryProperties(phys, &P.mp);
    VkPhysicalDeviceProperties props;
    qvkGetPhysicalDeviceProperties(phys, &props);
    if (s_dev_aniso != 0) s_dev_aniso = (int)props.limits.maxSamplerAnisotropy;
    P.ts_period = props.limits.timestampPeriod;

    if (!rsx_hlsl_to_spirv_available()) { PFX_LOG("built without glslang: no AA or sharpening\n"); return -1; }
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    if (qvkCreateSampler(dev, &sci, NULL, &P.linear) != VK_SUCCESS) return -1;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    if (qvkCreateSampler(dev, &sci, NULL, &P.point) != VK_SUCCESS) return -1;
    if (!(P.rp_rg8 = pfx_render_pass(VK_FORMAT_R8G8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR)) ||
        !(P.rp_rgba8_clear = pfx_render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR)) ||
        !(P.rp_rgba8 = pfx_render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_DONT_CARE))) return -1;
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t k = 0; k < 3; k++)
        b[k] = (VkDescriptorSetLayoutBinding){ k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 3, .pBindings = b };
    if (qvkCreateDescriptorSetLayout(dev, &lci, NULL, &P.dsl) != VK_SUCCESS) return -1;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pfx_pc) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &P.dsl,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (qvkCreatePipelineLayout(dev, &plci, NULL, &P.pl) != VK_SUCCESS) return -1;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3u * DS_COUNT };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = DS_COUNT, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (qvkCreateDescriptorPool(dev, &dpci, NULL, &P.dpool) != VK_SUCCESS) return -1;
    VkDescriptorSetLayout layouts[DS_COUNT];
    for (int k = 0; k < DS_COUNT; k++) layouts[k] = P.dsl;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = P.dpool, .descriptorSetCount = DS_COUNT,
                                        .pSetLayouts = layouts };
    if (qvkAllocateDescriptorSets(dev, &dai, P.ds) != VK_SUCCESS) return -1;

    const char* pe = getenv("PS3RECOMP_POSTFX_PROF");
    P.prof = (pe && *pe && *pe != '0' && props.limits.timestampComputeAndGraphics) ? 1 : 0;
    if (P.prof) {
        VkQueryPoolCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2u * PFX_QPAIRS };
        if (qvkCreateQueryPool(dev, &qci, NULL, &P.qp) != VK_SUCCESS) P.prof = 0;
    }
    P.last_ms = -1.0f;
    P.ok = 1;
    {
        static const char* const aa_name[] = { "off", "FXAA 3.11", "SMAA 1x" };
        PFX_LOG("AA %s, CAS sharpen %.2f, anisotropic %dx (device max %d), render scale setting %.2fx\n",
                aa_name[vk_postfx_aa()], (double)vk_postfx_sharpen(), vk_postfx_aniso(), s_dev_aniso,
                (double)PFX_LOAD(s_set_scale) / 4.0);
    }
    return 0;
}

static void pfx_free_targets(void)
{
    pfx_image_destroy(&P.edges);
    pfx_image_destroy(&P.weights);
    pfx_image_destroy(&P.aa_out);
    P.src_view = VK_NULL_HANDLE;
}

void vk_postfx_shutdown(void)
{
    if (!P.dev) return;
    pfx_free_targets();
    pfx_image_destroy(&P.out);
    pfx_image_destroy(&P.up);
    pfx_image_destroy(&P.down);                 /* [res] */
    pfx_image_destroy(&P.area);
    pfx_image_destroy(&P.search);
    if (P.staging)     qvkDestroyBuffer(P.dev, P.staging, NULL);
    if (P.staging_mem) qvkFreeMemory(P.dev, P.staging_mem, NULL);
    for (int k = 0; k < PS_COUNT; k++) if (P.pipe[k]) qvkDestroyPipeline(P.dev, P.pipe[k], NULL);
    for (int k = 0; k < VS_COUNT; k++) if (P.vs[k]) qvkDestroyShaderModule(P.dev, P.vs[k], NULL);
    if (P.qp)    qvkDestroyQueryPool(P.dev, P.qp, NULL);
    if (P.dpool) qvkDestroyDescriptorPool(P.dev, P.dpool, NULL);
    if (P.pl)    qvkDestroyPipelineLayout(P.dev, P.pl, NULL);
    if (P.dsl)   qvkDestroyDescriptorSetLayout(P.dev, P.dsl, NULL);
    VkRenderPass rps[3] = { P.rp_rg8, P.rp_rgba8_clear, P.rp_rgba8 };
    for (int k = 0; k < 3; k++) if (rps[k]) qvkDestroyRenderPass(P.dev, rps[k], NULL);
    if (P.linear) qvkDestroySampler(P.dev, P.linear, NULL);
    if (P.point)  qvkDestroySampler(P.dev, P.point, NULL);
    memset(&P, 0, sizeof P);
}

float vk_postfx_sampler_aniso(int min_linear, int mip_present)
{
    if (!min_linear || !mip_present) return 0.0f;
    const int a = vk_postfx_aniso();
    return a > 1 ? (float)a : 0.0f;
}

float vk_postfx_gpu_ms(void) { return P.prof ? P.last_ms : -1.0f; }

/* SMAA's AreaTex (160x560 RG8) and SearchTex (64x16 R8): a staging buffer
 * holding both, copied in the first frame that runs SMAA. The buffer stays
 * until shutdown (180 KB), so nothing waits for that frame. */
static int pfx_luts(VkCommandBuffer cmd)
{
    if (P.lut_state) return P.lut_state > 0 ? 0 : -1;
    P.lut_state = -1;
    const VkDeviceSize asz = sizeof areaTexBytes, ssz = sizeof searchTexBytes;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = asz + ssz,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (qvkCreateBuffer(P.dev, &bci, NULL, &P.staging) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    qvkGetBufferMemoryRequirements(P.dev, P.staging, &req);
    uint32_t type;
    if (pfx_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type))
        return -1;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    void* ptr = NULL;
    if (qvkAllocateMemory(P.dev, &mai, NULL, &P.staging_mem) != VK_SUCCESS ||
        qvkBindBufferMemory(P.dev, P.staging, P.staging_mem, 0) != VK_SUCCESS ||
        qvkMapMemory(P.dev, P.staging_mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) return -1;
    memcpy(ptr, areaTexBytes, (size_t)asz);
    memcpy((unsigned char*)ptr + asz, searchTexBytes, (size_t)ssz);
    qvkUnmapMemory(P.dev, P.staging_mem);
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (pfx_image_make(&P.area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, u, VK_NULL_HANDLE) ||
        pfx_image_make(&P.search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, u, VK_NULL_HANDLE))
        return -1;
    pfx_to_general(cmd, P.area.img);
    pfx_to_general(cmd, P.search.img);
    VkBufferImageCopy ra = { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { AREATEX_WIDTH, AREATEX_HEIGHT, 1 } };
    VkBufferImageCopy rs = { .bufferOffset = asz, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1 } };
    qvkCmdCopyBufferToImage(cmd, P.staging, P.area.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ra);
    qvkCmdCopyBufferToImage(cmd, P.staging, P.search.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
    P.lut_state = 1;
    return 0;
}

/* The frame-sized targets for a w x h frame read through `src_view`, and the
 * sets that name them. Remade (after the device is idle) when either changes. */
static int pfx_targets(VkCommandBuffer cmd, VkImageView src_view, uint32_t w, uint32_t h)
{
    if (P.aa_out.img && P.aa_out.w == w && P.aa_out.h == h && P.src_view == src_view) return 0;
    if (P.aa_out.img) qvkDeviceWaitIdle(P.dev);
    pfx_free_targets();
    const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (pfx_image_make(&P.edges, VK_FORMAT_R8G8_UNORM, w, h, u, P.rp_rg8) ||
        pfx_image_make(&P.weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, u, P.rp_rgba8) ||
        pfx_image_make(&P.aa_out, VK_FORMAT_R8G8B8A8_UNORM, w, h, u | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, P.rp_rgba8)) {
        pfx_free_targets();
        return -1;
    }
    pfx_to_general(cmd, P.edges.img);
    pfx_to_general(cmd, P.weights.img);
    pfx_to_general(cmd, P.aa_out.img);
    pfx_write_set(P.ds[DS_EDGE], src_view, P.linear, NULL, P.linear, NULL, P.linear);
    /* the lookups may not exist yet (no SMAA so far): placeholders, rewritten
     * before the set's first use (vk_postfx_apply) -- never under a frame */
    pfx_write_set(P.ds[DS_WEIGHT], P.edges.view, P.linear, P.area.view ? P.area.view : P.edges.view, P.linear,
                  P.search.view ? P.search.view : P.edges.view, P.point);
    P.weight_ds_luts = P.area.view && P.search.view;
    pfx_write_set(P.ds[DS_BLEND], src_view, P.linear, P.weights.view, P.linear, NULL, P.linear);
    pfx_write_set(P.ds[DS_FXAA], src_view, P.linear, NULL, P.linear, NULL, P.linear);
    pfx_write_set(P.ds[DS_CAS_SRC], src_view, P.point, NULL, P.point, NULL, P.point);
    pfx_write_set(P.ds[DS_CAS_AA], P.aa_out.view, P.point, NULL, P.point, NULL, P.point);
    pfx_write_set(P.ds[DS_EASU_SRC], src_view, P.linear, NULL, P.linear, NULL, P.linear);
    pfx_write_set(P.ds[DS_EASU_AA], P.aa_out.view, P.linear, NULL, P.linear, NULL, P.linear);
    pfx_write_set(P.ds[DS_DOWN_SRC], src_view, P.linear, NULL, P.linear, NULL, P.linear);      /* [res] */
    pfx_write_set(P.ds[DS_DOWN_AA], P.aa_out.view, P.linear, NULL, P.linear, NULL, P.linear);
    P.src_view = src_view;
    return 0;
}

/* [res] the downsample's window-sized output and the set CAS reads it through;
 * remade (after the device is idle) when the window's size changes. */
static int pfx_down_target(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (P.down.img && P.down.w == w && P.down.h == h) return 0;
    if (P.down.img) { qvkDeviceWaitIdle(P.dev); pfx_image_destroy(&P.down); }
    if (pfx_image_make(&P.down, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT, P.rp_rgba8))
        return -1;
    pfx_to_general(cmd, P.down.img);
    pfx_write_set(P.ds[DS_CAS_DOWN], P.down.view, P.point, NULL, P.point, NULL, P.point);
    return 0;
}

/* The window-sized output, and with 
eed_up the FSR intermediate (EASU out,
 * RCAS in) and its set. */
static int pfx_out_target(VkCommandBuffer cmd, uint32_t w, uint32_t h, int need_up)
{
    if (P.out.img && (P.out.w != w || P.out.h != h)) {
        qvkDeviceWaitIdle(P.dev);
        pfx_image_destroy(&P.out);
        pfx_image_destroy(&P.up);
    }
    if (!P.out.img) {
        if (pfx_image_make(&P.out, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, P.rp_rgba8))
            return -1;
        pfx_to_general(cmd, P.out.img);
    }
    if (need_up && !P.up.img) {
        /* new: DS_RCAS has never been bound, or the device was idled above */
        if (pfx_image_make(&P.up, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, P.rp_rgba8))
            return -1;
        pfx_to_general(cmd, P.up.img);
        pfx_write_set(P.ds[DS_RCAS], P.up.view, P.point, NULL, P.point, NULL, P.point);
    }
    return 0;
}

static double pfx_now_s(void)
{
    static double t0;
    const double t = (double)clock() / (double)CLOCKS_PER_SEC;
    if (!t0) t0 = t;
    return t - t0;
}

/* PS3RECOMP_POSTFX_PROF: the pair written PFX_QPAIRS - 1 frames ago, if done. */
static void pfx_prof_collect(void)
{
    const uint32_t q = (P.qframe + 1u) % PFX_QPAIRS;   /* the oldest pair */
    if (!P.qused[q]) return;
    uint64_t ts[2];
    if (qvkGetQueryPoolResults(P.dev, P.qp, 2u * q, 2, sizeof ts, ts, sizeof ts[0],
                               VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    P.qused[q] = 0;
    const double ms = (double)(ts[1] - ts[0]) * (double)P.ts_period * 1e-6;
    P.prof_sum_ms += ms;
    P.prof_n++;
    const double now = pfx_now_s();
    if (now - P.prof_t0 >= 5.0 && P.prof_n) {
        P.last_ms = (float)(P.prof_sum_ms / P.prof_n);
        PFX_LOG("GPU %.3f ms/frame over %u frames (AA %d, sharpen %.2f)\n",
                (double)P.last_ms, P.prof_n, vk_postfx_aa(), (double)vk_postfx_sharpen());
        P.prof_sum_ms = 0.0; P.prof_n = 0; P.prof_t0 = now;
    }
}

int vk_postfx_apply(VkCommandBuffer cmd, VkImage src, VkImageView src_view, uint32_t w, uint32_t h,
                    uint32_t dst_w, uint32_t dst_h, VkImage* out, uint32_t* out_w, uint32_t* out_h)
{
    if (!P.ok || !src || !src_view || !w || !h || !dst_w || !dst_h) return 0;
    int aa = vk_postfx_aa();
    if (aa && P.aa_disabled[aa]) aa = VK_POSTFX_AA_OFF;
    const float sharp = vk_postfx_sharpen();
    /* The window larger than the frame on both axes (fullscreen 1080p: 1.5x):
     * FSR 1 (EASU, then RCAS when sharpening) or CAS's own scaling (up to 4x
     * the area, CAS_AREA_LIMIT) instead of the blit's bilinear. Anything else
     * (the same size, a smaller or odd-shaped window) keeps the blit for the
     * scale, with CAS at the frame's size when sharpening. */
    const int larger = (dst_w != w || dst_h != h) && dst_w >= w && dst_h >= h;
    int up = larger ? vk_postfx_upscale() : VK_POSTFX_UP_BILINEAR;
    if (up == VK_POSTFX_UP_CAS && (!(sharp > 0.0f) || (double)dst_w * dst_h > 4.0 * (double)w * h))
        up = VK_POSTFX_UP_BILINEAR;
    if (up == VK_POSTFX_UP_FSR && (!pfx_pipeline(PS_EASU) || (sharp > 0.0f && !pfx_pipeline(PS_RCAS))))
        up = VK_POSTFX_UP_BILINEAR;
    if (up == VK_POSTFX_UP_CAS && !pfx_pipeline(PS_CAS_SCALE))
        up = VK_POSTFX_UP_BILINEAR;
    const int cas = up == VK_POSTFX_UP_BILINEAR && sharp > 0.0f && pfx_pipeline(PS_CAS);
    /* [res] a frame larger than the window on both axes (PS3RECOMP_RENDER_SCALE
     * in a smaller window): the area downsample, then CAS at the window's size,
     * instead of the blit's 2-tap bilinear shrink */
    const int smaller = dst_w < w && dst_h < h && pfx_pipeline(PS_DOWN);
    if (!aa && up == VK_POSTFX_UP_BILINEAR && !cas && !smaller) return 0;

    /* the AA pipelines: a mode that cannot build is off from now on */
    if (aa == VK_POSTFX_AA_SMAA &&
        (!pfx_pipeline(PS_SMAA_EDGE) || !pfx_pipeline(PS_SMAA_WEIGHT) || !pfx_pipeline(PS_SMAA_BLEND) ||
         pfx_luts(cmd))) {
        PFX_LOG("SMAA unavailable: AA off\n");
        P.aa_disabled[aa] = 1; aa = VK_POSTFX_AA_OFF;
    }
    if (aa == VK_POSTFX_AA_FXAA && !pfx_pipeline(PS_FXAA)) {
        PFX_LOG("FXAA unavailable: AA off\n");
        P.aa_disabled[aa] = 1; aa = VK_POSTFX_AA_OFF;
    }
    if (!aa && up == VK_POSTFX_UP_BILINEAR && !cas && !smaller) return 0;
    if (pfx_targets(cmd, src_view, w, h)) { P.ok = 0; PFX_LOG("targets failed: post passes off\n"); return 0; }
    const int rcas = up == VK_POSTFX_UP_FSR && sharp > 0.0f;
    if (up != VK_POSTFX_UP_BILINEAR || cas || smaller) {
        const int win = up != VK_POSTFX_UP_BILINEAR || smaller;   /* [res] the output at the window's size */
        if (pfx_out_target(cmd, win ? dst_w : w, win ? dst_h : h, rcas))
            return 0;
    }
    if (smaller && cas && pfx_down_target(cmd, dst_w, dst_h)) return 0;   /* [res] */

    if (P.prof) {
        pfx_prof_collect();
        P.qframe = (P.qframe + 1u) % PFX_QPAIRS;
        qvkCmdResetQueryPool(cmd, P.qp, 2u * P.qframe, 2);
        /* bottom of pipe: once the frame's own work is done, so the pair times the passes */
        qvkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, P.qp, 2u * P.qframe);
    }
    pfx_pc pc = { { 1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h }, { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImage cur = src;
    int from_aa = 0;
    if (aa == VK_POSTFX_AA_SMAA) {
        if (!P.weight_ds_luts) {
            pfx_write_set(P.ds[DS_WEIGHT], P.edges.view, P.linear, P.area.view, P.linear, P.search.view, P.point);
            P.weight_ds_luts = 1;
        }
        pfx_pass(cmd, PS_SMAA_EDGE, P.rp_rg8, &P.edges, P.ds[DS_EDGE], &pc);
        pfx_pass(cmd, PS_SMAA_WEIGHT, P.rp_rgba8_clear, &P.weights, P.ds[DS_WEIGHT], &pc);
        pfx_pass(cmd, PS_SMAA_BLEND, P.rp_rgba8, &P.aa_out, P.ds[DS_BLEND], &pc);
        cur = P.aa_out.img; from_aa = 1;
    } else if (aa == VK_POSTFX_AA_FXAA) {
        pfx_pass(cmd, PS_FXAA, P.rp_rgba8, &P.aa_out, P.ds[DS_FXAA], &pc);
        cur = P.aa_out.img; from_aa = 1;
    }
    uint32_t ow = w, oh = h;
    const pfx_pc wc = { { 1.0f / (float)dst_w, 1.0f / (float)dst_h, (float)dst_w, (float)dst_h },
                        { sharp, (float)w, (float)h, 0.0f } };
    if (smaller) {                              /* [res] area downsample, then CAS at the window's size */
        uint32_t nx = (w + dst_w - 1u) / dst_w, ny = (h + dst_h - 1u) / dst_h;
        if (nx > 8u) nx = 8u;
        if (ny > 8u) ny = 8u;
        const pfx_pc dc = { { 1.0f / (float)dst_w, 1.0f / (float)dst_h, (float)dst_w, (float)dst_h },
                            { (float)w, (float)h, (float)nx, (float)ny } };
        pfx_pass(cmd, PS_DOWN, P.rp_rgba8, cas ? &P.down : &P.out, P.ds[from_aa ? DS_DOWN_AA : DS_DOWN_SRC], &dc);
        if (cas) {
            const pfx_pc cp = { { 1.0f / (float)dst_w, 1.0f / (float)dst_h, (float)dst_w, (float)dst_h },
                                { sharp, (float)dst_w, (float)dst_h, 0.0f } };
            pfx_pass(cmd, PS_CAS, P.rp_rgba8, &P.out, P.ds[DS_CAS_DOWN], &cp);
        }
        cur = P.out.img; ow = dst_w; oh = dst_h;
    } else if (up == VK_POSTFX_UP_FSR) {
        pfx_pass(cmd, PS_EASU, P.rp_rgba8, rcas ? &P.up : &P.out, P.ds[from_aa ? DS_EASU_AA : DS_EASU_SRC], &wc);
        if (rcas) {
            /* our 0..1 to RCAS's stops (0 = most, each +1 halves): 1 -> 0, 0.5 -> 1, 0.3 -> 1.4 */
            pfx_pc rc = wc;
            rc.p[0] = 2.0f * (1.0f - sharp);
            pfx_pass(cmd, PS_RCAS, P.rp_rgba8, &P.out, P.ds[DS_RCAS], &rc);
        }
        cur = P.out.img; ow = dst_w; oh = dst_h;
    } else if (up == VK_POSTFX_UP_CAS) {
        pfx_pass(cmd, PS_CAS_SCALE, P.rp_rgba8, &P.out, P.ds[from_aa ? DS_CAS_AA : DS_CAS_SRC], &wc);
        cur = P.out.img; ow = dst_w; oh = dst_h;
    } else if (cas) {
        pfx_pc cp = { { 1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h }, { sharp, (float)w, (float)h, 0.0f } };
        pfx_pass(cmd, PS_CAS, P.rp_rgba8, &P.out, P.ds[from_aa ? DS_CAS_AA : DS_CAS_SRC], &cp);
        cur = P.out.img;
    }
    pfx_barrier(cmd);
    if (P.prof) {
        qvkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, P.qp, 2u * P.qframe + 1u);
        P.qused[P.qframe] = 1;
    }
    *out = cur; *out_w = ow; *out_h = oh;
    return 1;
}
#endif /* PS3RECOMP_RSX_VULKAN */
