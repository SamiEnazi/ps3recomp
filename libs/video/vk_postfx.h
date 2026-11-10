/*
 * ps3recomp - Vulkan image quality: anti-aliasing, sharpening, anisotropic
 * filtering (vk_postfx.c)
 *
 * The game draws its frame into the backend's offscreen image (1280x720, the
 * guest's resolution); the present used to blit that straight into the window.
 * With this module the present first runs, on that image:
 *   AA       SMAA 1x (Jimenez et al., third_party/smaa, high preset, luma
 *            edges) or FXAA 3.11 (NVIDIA, third_party/fxaa, quality 29)
 *   upscale  when the window is larger than the frame (fullscreen on a
 *            1080p screen: 1.5x), AMD FidelityFX FSR 1 (third_party/ffx_fsr1):
 *            EASU to the window's size, then RCAS sharpening -- in place of
 *            the blit's bilinear stretch
 *   sharpen  at the frame's size (window = frame): AMD FidelityFX CAS
 *            (third_party/ffx_cas); upscaling: FSR's RCAS
 * and the sampler path raises anisotropic filtering on the guest's
 * linear-mipmapped samplers.
 *
 * Settings: environment variables at start, and the setters below at any time
 * from any thread (they take effect at the next present; a new anisotropy
 * level makes new samplers as textures are next bound):
 *   PS3RECOMP_AA=off|fxaa|smaa        default smaa
 *   PS3RECOMP_SHARPEN=0..1            CAS / RCAS sharpness, 0 = off; default 0.3
 *   PS3RECOMP_UPSCALE=fsr|cas|bilinear  a window larger than the frame: FSR 1
 *                                     (default), CAS's scaling form, or the
 *                                     plain bilinear blit
 *   PS3RECOMP_ANISO=1..16             default 16 (1 = the guest's filtering)
 *   PS3RECOMP_RENDER_SCALE=1|1.5|2|3  internal resolution, or 720p|1080p|1440p|4k
 *                                     (PS3_RES_SCALE as on Metal when unset): the
 *                                     backend draws the game's screen targets
 *                                     that much larger (rsx_vulkan_backend.c,
 *                                     [res]); read at start. A frame larger than
 *                                     the window is area-downsampled here.
 *   PS3RECOMP_POSTFX_PROF=1           log the passes' GPU time every 5 s
 * All four at their "off" values (AA=off, SHARPEN=0, ANISO=1) give exactly
 * the old picture: the present blits the frame unchanged.
 */
#ifndef PS3RECOMP_VK_POSTFX_H
#define PS3RECOMP_VK_POSTFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- settings (the overlay menu; any thread) ---------------------------- */
enum { VK_POSTFX_AA_OFF = 0, VK_POSTFX_AA_FXAA = 1, VK_POSTFX_AA_SMAA = 2 };
void  vk_postfx_set_aa(int mode);            /* VK_POSTFX_AA_* */
int   vk_postfx_aa(void);
void  vk_postfx_set_sharpen(float amount);   /* 0 (off) .. 1, clamped */
float vk_postfx_sharpen(void);
void  vk_postfx_set_aniso(int level);        /* 1 (off) .. 16, clamped; also to the device's limit */
int   vk_postfx_aniso(void);                 /* the level in effect (1 when the device has none) */
enum { VK_POSTFX_UP_FSR = 0, VK_POSTFX_UP_CAS = 1, VK_POSTFX_UP_BILINEAR = 2 };
void  vk_postfx_set_upscale(int mode);       /* VK_POSTFX_UP_* */
int   vk_postfx_upscale(void);
void  vk_postfx_set_render_scale(float s);   /* 1 .. 4 in quarter steps: the next start's */
float vk_postfx_render_scale(void);          /* the scale in effect this session (1 = 720p) */
/* [res] the backend: the setting read at start, and the scale it runs at. */
float vk_postfx_render_scale_wanted(void);
void  vk_postfx_render_scale_active(float s);
/* [wide] PS3RECOMP_ASPECT=16:9|auto|21:9|32:9|16:10|<w>:<h>|<w>x<h>|<ratio>, read
 * once at start (docs/WIDESCREEN.md). 16:9 or unset = the game's picture, the
 * old path exactly. auto = the shape of the screen (PS3RECOMP_VK_FULLSCREEN=1)
 * or of PS3RECOMP_VK_WINDOW_SIZE=<w>x<h> (windowed; without it the window is
 * the guest's 16:9). PS3_ASPECT (the Metal port's name) when it is unset.
 * ps3_aspect_wanted: 1 and the shape (reduced, num:den) when a picture other
 * than 16:9 is asked for; the first call decides and logs. The title's half
 * (the port's game_patches.cpp) widens its cameras and says what they see:
 * ps3_aspect_game_set(<width at the guest's height>, <guest height>), e.g.
 * 1680, 720 at 21:9. The backend renders wide only when both are there. */
int   ps3_aspect_wanted(unsigned* num, unsigned* den);
void  ps3_aspect_game_set(unsigned w, unsigned h);
int   ps3_aspect_game(unsigned* w, unsigned* h);   /* 1 + what the title said, else 0 */
/* [wide] the backend: the width / height ratio of its frame relative to 16:9
 * (1.3125 at 21:9, 1 = 16:9), for the menu's "now" line. */
void  vk_postfx_aspect_active(float k);
float vk_postfx_aspect_k(void);
/* By environment-variable name ("PS3RECOMP_AA", "PS3RECOMP_SHARPEN",
 * "PS3RECOMP_ANISO", "PS3RECOMP_RENDER_SCALE", "PS3RECOMP_UPSCALE") and its
 * text value, as the
 * variables take them. 0 on success, -1 for an unknown name or a bad value. */
int   vk_postfx_set(const char* name, const char* value);
/* The last measured GPU time of the post passes in ms (PS3RECOMP_POSTFX_PROF=1;
 * -1 when not measured). */
float vk_postfx_gpu_ms(void);

/* ---- backend hooks (rsx_vulkan_backend.c) ------------------------------- */
#ifdef VK_VERSION_1_0
/* Before vkCreateDevice: the features to enable (samplerAnisotropy). */
void  vk_postfx_device_features(const VkPhysicalDeviceFeatures* have, VkPhysicalDeviceFeatures* want);
/* After vkCreateDevice. `cache` may be VK_NULL_HANDLE. 0 on success; on
 * failure the present keeps its plain blit. */
int   vk_postfx_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                     PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                     VkPipelineCache cache);
/* Before the device is destroyed (the device idle). */
void  vk_postfx_shutdown(void);
/* The anisotropy for a guest sampler (0 = off): the level set, but only for
 * a linear minification through mipmaps -- point, LUT and unmipped samplers
 * keep the guest's own filtering. */
float vk_postfx_sampler_aniso(int min_linear, int mip_present);
/* At present, recording into `cmd`: `src` (w x h, GENERAL, written by
 * earlier commands) through AA and sharpening for a window of dst_w x dst_h.
 * Returns 1 with *out (GENERAL, *out_w x *out_h) to blit into the window, or
 * 0 when nothing is on (or nothing can run): blit `src` as before. */
int   vk_postfx_apply(VkCommandBuffer cmd, VkImage src, VkImageView src_view, uint32_t w, uint32_t h,
                      uint32_t dst_w, uint32_t dst_h, VkImage* out, uint32_t* out_w, uint32_t* out_h);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_VK_POSTFX_H */
