/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * The first non-Windows render path in the project. It implements the subset
 * of the rsx_backend vtable needed to drive a guest clear/draw/flip loop:
 * `clear` captures the colour written by NV4097_CLEAR_SURFACE,
 * `set_render_target` tracks the guest's surface dimensions, the draw
 * callbacks record draws with their vertices fetched out of guest memory, and
 * present replays them into a drawable. Every dispatch site in rsx_commands.c
 * is guarded (`if (s_backend && s_backend->x)`), so a partial vtable is the
 * intended way to bring a backend up incrementally.
 *
 * Guest shaders. A draw runs the guest's own vertex and fragment programs
 * when both translate: the NV40 microcode goes through the RSX decompilers
 * (HLSL), then rsx_hlsl_to_msl (glslang + spirv-cross), then
 * -newLibraryWithSource:. What the D3D12 backend does with those programs is
 * mirrored here field for field -- where the vertex program starts, how the
 * fragment program is located and keyed, the viewport epilogue, the per-draw
 * constant blocks -- because that is the path real titles have exercised.
 * With no vertex program loaded, or with the translator compiled out, a draw
 * falls back to the built-in fixed-function shader, which is what the
 * clear/draw host checks use.
 *
 * Guest textures. A guest-program draw binds the enabled texture units the
 * way the D3D12 backend does: SET_TEXTURE_FORMAT's location bits say where
 * the bytes are, rsx_texture_layout says what shape they are,
 * rsx_texture_decode turns them into host rows, and the TEXTURE_CONTROL1
 * crossbar becomes the texture's swizzle. Every mip level the register says
 * is there is uploaded, and the sampler's mip filter and LOD range come from
 * SET_TEXTURE_FILTER and SET_TEXTURE_CONTROL0. A unit SET_TEXTURE_FORMAT
 * bit 2 marks as a cube map becomes an MTLTextureTypeCube of six faces, and
 * the fragment program is compiled against that so it samples with a
 * direction. NV4097_SET_VERTEX_TEXTURE_* units go through the same path and
 * bind to the vertex stage, so a transform program's TXL samples.
 *
 * Render targets. SET_SURFACE_COLOR_TARGET picks colour surface A, B or one of
 * the MRT sets, and a colour offset that is not a registered display buffer is
 * an offscreen surface: a texture in a registry keyed by that raw offset, kept
 * across frames because a surface rendered in one frame is sampled in the next.
 * Each record carries the target it was recorded against, and present walks the
 * records opening one render pass per contiguous run with the same target. A
 * texture unit bound at a registered surface's offset then samples that surface
 * rather than guest memory, which is what render-to-texture is.
 *
 * Why Metal rather than SDL_Renderer: SDL2's renderer is a 2D sprite API with
 * no route to a custom vertex program, depth/stencil, MRT or render-to-texture,
 * so it cannot grow into the real pipeline.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>
#if !TARGET_OS_IPHONE
#  import <AppKit/AppKit.h>
#endif
/* PS3_UPSCALE=metalfx (macOS 13+). The framework is weak-linked, so a binary
 * built here still starts on an older system and simply goes without it. */
#if !TARGET_OS_IPHONE && __has_include(<MetalFX/MetalFX.h>)
#  import <MetalFX/MetalFX.h>
#  define GFX_HAVE_METALFX 1
#endif

#include "../../include/ps3emu/log_first.h"   /* PS3_LOG_FIRST */
#include <unistd.h>

/* On-demand dump (PS3RECOMP_METAL_DUMP_TRIGGER): set at a present, consumed by
 * the surface dump and the next pass log; see eng_dump_surfaces. */
static int s_dump_force, s_pass_log_force;
/* The in-game capture key (ps3_overlay.mm, F12): same as touching the trigger
 * file, without leaving the game. */
static volatile int s_capture_req;
int rsx_metal_capture_available(void) { const char* d = getenv("PS3RECOMP_METAL_SURF_DUMP"); return d && *d; }
void rsx_metal_request_capture(void) { __atomic_store_n(&s_capture_req, 1, __ATOMIC_RELEASE); }
#include <pthread.h>
#include "metal_frame_rate.h"
#include "rsx_commands.h"
#include "rsx_metal_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_primitives.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_msl.h"
#include "rsx_texture_layout.h"
#include "../../include/ps3emu/ps3_toggles.h"  /* PS3_TOGGLE_FILE: live graphics options */
#include "../../include/ps3emu/ps3_ftrace.h"   /* PS3_FTRACE: encode / GPU wait / readback spans */
#include "ps3_overlay.h"                       /* the in-game menu and HUD */
/* SMAA (PS3_AA=smaa): its shaders and lookup textures, MIT, third_party/smaa. */
#include "../../third_party/smaa/smaa_msl.h"
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */

static id<MTLDevice>       s_dev;
static id<MTLCommandQueue> s_queue;
static CAMetalLayer*       s_layer;      /* windowed only   */
static id<MTLTexture>      s_offscreen;  /* headless only   */
static id<MTLTexture>      s_depth;      /* depth + stencil, both surfaces  */
static NSWindow*           s_window;
static int                 s_headless;
static int                 s_closed;
static int                 s_ready;
static u32                 s_width  = 1280;
static u32                 s_height = 720;

/* Quick graphics options, read once at init; unset, the output is exactly
 * what it was without them.
 *   PS3_ANISO=2|4|8|16   anisotropic filtering on the guest's mipmapped,
 *                        linearly minified samplers (point, LUT and
 *                        unmipped samplers keep their filtering)
 *   PS3_AA=fxaa          FXAA 3.11 (quality) on the display buffer, at the
 *                        guest's resolution, before anything scales it
 *   PS3_AA=smaa          SMAA 1x (high preset, luma edges) instead: three
 *                        passes, sharper than FXAA on text and textures
 *   PS3_SHARPEN=0..1     contrast-adaptive sharpening (AMD CAS) at the
 *                        output resolution, in the present pass
 *   PS3_VIBRANCE=-1..1   vibrance (ReShade's idea): saturation raised most
 *                        on dull colours, so skin and sky do not blow out;
 *                        negative mutes; 0 off
 *   PS3_CONTRAST=0.5..1.5  contrast around mid-grey; 1 off. Both in the
 *                        present pass, before the display gamma
 *   PS3_LUT=<file.png>   a colour LUT in ReShade's layout (a strip of N
 *                        N x N tiles, e.g. 1024x32), the player's own;
 *                        PS3_LUT_STRENGTH=0..1 mixes it in (1 by default)
 *   PS3_VIGNETTE=0..1    darker corners; PS3_GRAIN=0..1 film grain
 *   PS3_SHADOWS, PS3_MIDTONES, PS3_HIGHLIGHTS=-1..1  a tone curve: each
 *                        lifts (or, negative, lowers) its third of the range
 *                        while black and white stay where they are; 0 off
 *   PS3_CLARITY=-1..1    local contrast: an unsharp mask of the luminance
 *                        with a wide radius (~1% of the picture height), so
 *                        shapes gain depth without the fine grain that
 *                        sharpening raises; negative softens; 0 off
 *   PS3_LUT_PRESET=warm|cool|film|vivid  a built-in colour LUT (generated
 *                        here, 32^3) in place of PS3_LUT's file, mixed in by
 *                        PS3_LUT_STRENGTH too; unset or off = none
 *   PS3_UPSCALE=metalfx  windowed, draw engine only: the drawable follows the
 *                        view's pixel size (fitted to the guest's aspect, so
 *                        the layer still letterboxes) and the display buffer
 *                        goes through a MetalFX spatial scaler to it. Off if
 *                        MetalFX is missing or fails (one log line). */
static int   s_gfx_aniso;              /* 0 = off, else 2..16 */
static int   s_gfx_fxaa;
static int   s_gfx_smaa;               /* PS3_AA=smaa (FXAA and SMAA are exclusive) */
static float s_gfx_sharpen;            /* 0 = off */
static float s_gfx_vibrance;           /* 0 = off, -1..1 */
static float s_gfx_contrast = 1.0f;    /* 1 = off, 0.5..1.5 */
static float s_gfx_vignette;           /* 0 = off */
static float s_gfx_grain;              /* 0 = off */
static float s_gfx_lut_strength = 1.0f;
static id<MTLTexture> s_gfx_lut;       /* the LUT the present uses: s_gfx_lut_file or a preset's */
static float s_gfx_lut_size;
static id<MTLTexture> s_gfx_lut_file;  /* PS3_LUT, loaded once */
static float s_gfx_lut_file_size;
static float s_gfx_shadows, s_gfx_midtones, s_gfx_highlights;   /* 0 = off, -1..1 */
static float s_gfx_clarity;            /* 0 = off, -1..1 */
static int   s_gfx_lut_preset;         /* 0 none, else GFX_LUT_* */
static int   s_gfx_lut_preset_built = -1;
enum { GFX_LUT_NONE, GFX_LUT_WARM, GFX_LUT_COOL, GFX_LUT_FILM, GFX_LUT_VIVID, GFX_LUT_N };
static const char* const kGfxLutPreset[GFX_LUT_N] = { "off", "warm", "cool", "film", "vivid" };
static id<MTLTexture> s_gfx_lut_presets[GFX_LUT_N];
static int   s_gfx_upscale;            /* asked for (and windowed) */
static volatile int s_gfx_upscale_on;  /* still usable: main + render thread */

/* PS3_RES_SCALE=1|1.5|2|2.5|3|4 (or 720p, 1080p, 1440p, 4k): internal
 * resolution. Draw engine only, read once at init. Every colour and depth
 * target the engine creates (and the depth decodes, the colour copies and
 * the GPU light pass made from them) is `scale` times the guest's size;
 * viewports and scissors scale with them and fragment programs divide WPOS
 * back (rsx_fp_set_wpos_scale), so the game never sees a difference. What
 * the guest reads back (write-backs to XDR for the SPUs, the PPU's
 * readbacks) is point-sampled down to the guest's size, and the GPU SSAO
 * runs at the guest's size from the scaled depth and normals, as the job
 * did. Guest textures are untouched. 1 (unset) is the old path exactly. */
static float s_res_scale = 1.0f;
/* PS3_ASPECT=21:9|32:9|<w>:<h>|<ratio>: anamorphic targets for a wider
 * picture. Every engine target is also s_aspect_k = aspect / (16/9) times
 * wider than the guest's (1680x720 at 21:9), and everything that maps the
 * guest's pixels to the target's (viewports, scissors, WPOS, the seed and
 * readback samplers, the GPU light and SSAO passes) scales x by
 * s_res_sx = s_res_scale * s_aspect_k and y by s_res_sy = s_res_scale. The
 * game itself has to draw a wider view into the same clip space (the port's
 * hor+ projection patch) and the present shows the wide image without a
 * pillarbox. The game's 2D overlay (its HUD) keeps 16:9 when the draw engine
 * flags its draws (set_hud: centred in the wide target). Unset, 16:9 or
 * anything narrower is the old path exactly. */
static float s_aspect_k = 1.0f;
static float s_res_sx = 1.0f, s_res_sy = 1.0f;
static int   s_res_scaled;            /* s_res_sx or s_res_sy is not 1 */
static int   s_eng_hud_pin;           /* the draws being recorded are the game's HUD */
static u32   s_eng_hud_w = 1280;      /* ...into a target this wide (the guest's pixels) */
static int   s_eng_bars_next;         /* the next present gets black bars beside 16:9 (overlay-only frame) */
static u32 res_px(u32 v)              /* a height (and, without PS3_ASPECT, a width) */
{
    return s_res_sy == 1.0f ? v : (u32)floorf((float)v * s_res_sy + 0.5f);
}
static u32 res_px_x(u32 v)            /* a width or an x */
{
    return s_res_sx == 1.0f ? v : (u32)floorf((float)v * s_res_sx + 0.5f);
}
static void res_axes_update(void)
{
    s_res_sx = s_res_scale * s_aspect_k;
    s_res_sy = s_res_scale;
    s_res_scaled = (s_res_sx != 1.0f || s_res_sy != 1.0f);
}
static void aspect_load(void)
{
    const char* e = getenv("PS3_ASPECT");
    s_aspect_k = 1.0f;
    if (e && *e) {
        double a = 0.0;
        unsigned w = 0, h = 0;
        if (sscanf(e, "%u:%u", &w, &h) == 2 && w && h) a = (double)w / (double)h;
        else if (sscanf(e, "%ux%u", &w, &h) == 2 && w && h) a = (double)w / (double)h;
        else { char* end = NULL; a = strtod(e, &end); if (!end || *end) a = 0.0; }
        const double k = a / (16.0 / 9.0);
        if (!(k >= 0.999 && k <= 4.0))
            fprintf(stderr, "[RSX metal] PS3_ASPECT=%s: expected 16:9 up to 64:9 (21:9, 32:9, 2.39); 16:9\n", e);
        else if (k > 1.001)
            s_aspect_k = (float)k;
    }
    res_axes_update();
}
static void res_scale_load(void)
{
    const char* e = getenv("PS3_RES_SCALE");
    s_res_scale = 1.0f;
    aspect_load();
    if (!e || !*e) return;
    double v = 0.0;
    if (!strcasecmp(e, "720p") || !strcasecmp(e, "native")) v = 1.0;
    else if (!strcasecmp(e, "1080p")) v = 1.5;
    else if (!strcasecmp(e, "1440p")) v = 2.0;
    else if (!strcasecmp(e, "4k") || !strcasecmp(e, "2160p")) v = 3.0;
    else { char* end = NULL; v = strtod(e, &end); if (end && (*end == 'x' || *end == 'X')) end++;
           if (!end || *end) v = 0.0; }
    if (!(v >= 1.0 && v <= 4.0)) {
        fprintf(stderr, "[RSX metal] PS3_RES_SCALE=%s: expected 1..4 (1, 1.5, 2, 3) or 720p/1080p/1440p/4k; native\n", e);
        return;
    }
    s_res_scale = (float)(floor(v * 4.0 + 0.5) / 4.0);   /* quarter steps */
    res_axes_update();
}

/* PS3_ANISO, PS3_AA and PS3_SHARPEN (and PS3RECOMP_METAL_FPS) can also change
 * while the game runs, from PS3_TOGGLE_FILE: the watcher marks them dirty and
 * the next present re-reads them (eng_blit_to_display). A new anisotropy level
 * keys new samplers; the old ones stay cached, unused. */
static int s_gfx_dirty;
static void gfx_option_changed(int id, int v) { (void)id; (void)v; __atomic_store_n(&s_gfx_dirty, 1, __ATOMIC_RELAXED); }
static void gfx_options_read(void)
{
    char buf[PS3_TOGGLE_STR];
    const char* e = ps3_toggle_str(PS3T_ANISO, buf, sizeof buf);
    int a = 0;
    if (*e) a = atoi(e);
    __atomic_store_n(&s_gfx_aniso, a <= 1 ? 0 : a > 16 ? 16 : a, __ATOMIC_RELAXED);
    { const char* aa = ps3_toggle_str(PS3T_AA, buf, sizeof buf);
      s_gfx_fxaa = !strcasecmp(aa, "fxaa");
      s_gfx_smaa = !strcasecmp(aa, "smaa"); }
    e = ps3_toggle_str(PS3T_SHARPEN, buf, sizeof buf);
    const double v = *e ? strtod(e, NULL) : 0.0;
    s_gfx_sharpen = !(v > 0.0) ? 0.0f : v > 1.0 ? 1.0f : (float)v;
    e = ps3_toggle_str(PS3T_VIBRANCE, buf, sizeof buf);
    { const double x = *e ? strtod(e, NULL) : 0.0;
      s_gfx_vibrance = !(x == x) ? 0.0f : x < -1.0 ? -1.0f : x > 1.0 ? 1.0f : (float)x; }
    e = ps3_toggle_str(PS3T_CONTRAST, buf, sizeof buf);
    { const double x = *e ? strtod(e, NULL) : 1.0;
      s_gfx_contrast = !(x > 0.0) ? 1.0f : x < 0.5 ? 0.5f : x > 1.5 ? 1.5f : (float)x; }
    e = ps3_toggle_str(PS3T_VIGNETTE, buf, sizeof buf);
    { const double x = *e ? strtod(e, NULL) : 0.0; s_gfx_vignette = !(x > 0.0) ? 0.0f : x > 1.0 ? 1.0f : (float)x; }
    e = ps3_toggle_str(PS3T_GRAIN, buf, sizeof buf);
    { const double x = *e ? strtod(e, NULL) : 0.0; s_gfx_grain = !(x > 0.0) ? 0.0f : x > 1.0 ? 1.0f : (float)x; }
    e = ps3_toggle_str(PS3T_LUT_STRENGTH, buf, sizeof buf);
    { const double x = *e ? strtod(e, NULL) : 1.0; s_gfx_lut_strength = !(x > 0.0) ? 0.0f : x > 1.0 ? 1.0f : (float)x; }
    { static const int ids[3] = { PS3T_SHADOWS, PS3T_MIDTONES, PS3T_HIGHLIGHTS };
      float* const dst[3] = { &s_gfx_shadows, &s_gfx_midtones, &s_gfx_highlights };
      for (int k = 0; k < 3; k++) {
          e = ps3_toggle_str(ids[k], buf, sizeof buf);
          const double x = *e ? strtod(e, NULL) : 0.0;
          *dst[k] = !(x == x) ? 0.0f : x < -1.0 ? -1.0f : x > 1.0 ? 1.0f : (float)x;
      } }
    e = ps3_toggle_str(PS3T_CLARITY, buf, sizeof buf);
    { const double x = *e ? strtod(e, NULL) : 0.0;
      s_gfx_clarity = !(x == x) ? 0.0f : x < -1.0 ? -1.0f : x > 1.0 ? 1.0f : (float)x; }
    e = ps3_toggle_str(PS3T_LUT_PRESET, buf, sizeof buf);
    { int pr = 0;
      for (int k = 1; k < GFX_LUT_N; k++) if (!strcasecmp(e, kGfxLutPreset[k])) pr = k;
      if (*e && !pr && strcasecmp(e, "off") && strcmp(e, "0")) {
          static char warned[PS3_TOGGLE_STR];
          if (strcmp(warned, e)) { snprintf(warned, sizeof warned, "%s", e);
              fprintf(stderr, "[RSX metal] PS3_LUT_PRESET=%s unknown (warm, cool, film, vivid); off\n", e); }
      }
      s_gfx_lut_preset = pr; }
}

/* ---- the built-in LUT presets (PS3_LUT_PRESET) ---------------------------
 * Each is a small grade written for this port as a formula, baked into a
 * 32^3 LUT in ReShade's strip layout (the same one PS3_LUT reads), so the
 * present samples it exactly like a player's file. Display-referred RGB in,
 * RGB out, all in 0..1. */
static float gfx_lut_sat(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float gfx_lut_smooth(float x) { x = gfx_lut_sat(x); return x * x * (3.0f - 2.0f * x); }
static void gfx_lut_grade(int preset, const float in[3], float out[3])
{
    float r = in[0], g = in[1], b = in[2];
    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    switch (preset) {
    case GFX_LUT_WARM:     /* low sun: red up, blue down, a little more colour */
        r = r * 1.06f + 0.012f * (1.0f - r);
        g = g * 1.01f;
        b = b * 0.88f;
        { const float m = 0.2126f * r + 0.7152f * g + 0.0722f * b;
          r = m + (r - m) * 1.06f; g = m + (g - m) * 1.06f; b = m + (b - m) * 1.06f; }
        break;
    case GFX_LUT_COOL:     /* overcast: blue up, a teal lift in the shadows */
        { const float sh = (1.0f - l) * (1.0f - l);
          r = r * 0.93f;
          g = g * 0.99f + 0.010f * sh;
          b = b * 1.07f + 0.022f * sh; }
        break;
    case GFX_LUT_FILM: {   /* print film: soft S-curve, lifted blacks, rolled-off
                            * whites, a little less colour, cool shadows and warm
                            * highlights */
        const float m = l;
        r = m + (r - m) * 0.86f; g = m + (g - m) * 0.86f; b = m + (b - m) * 0.86f;
        float c[3] = { r, g, b };
        for (int k = 0; k < 3; k++) {
            const float x = gfx_lut_sat(c[k]);
            const float s = x + (gfx_lut_smooth(x) - x) * 0.55f;
            c[k] = 0.035f + s * (0.955f - 0.035f);
        }
        const float hi = gfx_lut_smooth((m - 0.45f) / 0.55f), lo = 1.0f - gfx_lut_smooth(m / 0.45f);
        r = c[0] + 0.030f * hi - 0.010f * lo;
        g = c[1] + 0.008f * hi + 0.004f * lo;
        b = c[2] - 0.022f * hi + 0.026f * lo;
        break; }
    case GFX_LUT_VIVID: {  /* more colour, a gentle S-curve */
        r = l + (r - l) * 1.32f; g = l + (g - l) * 1.32f; b = l + (b - l) * 1.32f;
        float c[3] = { r, g, b };
        for (int k = 0; k < 3; k++) {
            const float x = gfx_lut_sat(c[k]);
            c[k] = x + (gfx_lut_smooth(x) - x) * 0.30f;
        }
        r = c[0]; g = c[1]; b = c[2];
        break; }
    default: break;
    }
    out[0] = gfx_lut_sat(r); out[1] = gfx_lut_sat(g); out[2] = gfx_lut_sat(b);
}
static id<MTLTexture> gfx_lut_preset_texture(int preset)
{
    if (preset <= 0 || preset >= GFX_LUT_N || !s_dev) return nil;
    if (s_gfx_lut_presets[preset]) return s_gfx_lut_presets[preset];
    enum { N = 32 };
    uint8_t* px = (uint8_t*)malloc((size_t)N * N * N * 4);
    if (!px) return nil;
    for (int bi = 0; bi < N; bi++)
        for (int gi = 0; gi < N; gi++)
            for (int ri = 0; ri < N; ri++) {
                const float in[3] = { ri / (float)(N - 1), gi / (float)(N - 1), bi / (float)(N - 1) };
                float o[3];
                gfx_lut_grade(preset, in, o);
                uint8_t* q = px + ((size_t)gi * N * N + (size_t)bi * N + ri) * 4;
                q[0] = (uint8_t)(o[0] * 255.0f + 0.5f);
                q[1] = (uint8_t)(o[1] * 255.0f + 0.5f);
                q[2] = (uint8_t)(o[2] * 255.0f + 0.5f);
                q[3] = 255;
            }
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                     width:N * N height:N mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, N * N, N) mipmapLevel:0 withBytes:px bytesPerRow:N * N * 4];
    free(px);
    s_gfx_lut_presets[preset] = t;
    return t;
}
/* The LUT the present samples: a preset when one is chosen, else the file. */
static void gfx_lut_select(void)
{
    if (s_gfx_lut_preset_built == s_gfx_lut_preset) return;
    s_gfx_lut_preset_built = s_gfx_lut_preset;
    id<MTLTexture> t = gfx_lut_preset_texture(s_gfx_lut_preset);
    if (t) { s_gfx_lut = t; s_gfx_lut_size = 32.0f; }
    else   { s_gfx_lut = s_gfx_lut_file; s_gfx_lut_size = s_gfx_lut_file_size; }
    if (s_gfx_lut_preset)
        fprintf(stderr, "[RSX metal] colour LUT preset %s, strength %.2f\n",
                kGfxLutPreset[s_gfx_lut_preset], (double)s_gfx_lut_strength);
}

/* PS3_LUT: the player's colour LUT (ReShade's LUT.png layout: N tiles of N x N
 * side by side, blue picks the tile). Read once at init. */
static void gfx_lut_load(id<MTLDevice> dev)
{
    const char* path = getenv("PS3_LUT");
    if (!path || !*path || !dev) return;
    NSImage* img = [[NSImage alloc] initWithContentsOfFile:@(path)];
    CGImageRef cg = img ? [img CGImageForProposedRect:NULL context:nil hints:nil] : NULL;
    if (!cg) { fprintf(stderr, "[RSX metal] PS3_LUT=%s: cannot read the image; no LUT\n", path); return; }
    const size_t w = CGImageGetWidth(cg), h = CGImageGetHeight(cg);
    if (h < 2 || h > 256 || w != h * h) {
        fprintf(stderr, "[RSX metal] PS3_LUT=%s is %zux%zu: a LUT is N*N wide and N high (ReShade's 1024x32); no LUT\n", path, w, h);
        return;
    }
    uint8_t* px = (uint8_t*)calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = px ? CGBitmapContextCreate(px, w, h, 8, w * 4, cs,
                                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), cg);
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
        s_gfx_lut = s_gfx_lut_file = t;
        s_gfx_lut_size = s_gfx_lut_file_size = (float)h;
        fprintf(stderr, "[RSX metal] colour LUT %s (%zu^3), strength %.2f\n", path, h, (double)s_gfx_lut_strength);
        CGContextRelease(ctx);
    }
    if (cs) CGColorSpaceRelease(cs);
    free(px);
}
static void gfx_options_load(void)
{
    ps3_toggle_watch(PS3T_ANISO, gfx_option_changed);
    ps3_toggle_watch(PS3T_AA, gfx_option_changed);
    ps3_toggle_watch(PS3T_SHARPEN, gfx_option_changed);
    ps3_toggle_watch(PS3T_VIBRANCE, gfx_option_changed);
    ps3_toggle_watch(PS3T_VIGNETTE, gfx_option_changed);
    ps3_toggle_watch(PS3T_GRAIN, gfx_option_changed);
    ps3_toggle_watch(PS3T_LUT_STRENGTH, gfx_option_changed);
    ps3_toggle_watch(PS3T_CONTRAST, gfx_option_changed);
    ps3_toggle_watch(PS3T_SHADOWS, gfx_option_changed);
    ps3_toggle_watch(PS3T_MIDTONES, gfx_option_changed);
    ps3_toggle_watch(PS3T_HIGHLIGHTS, gfx_option_changed);
    ps3_toggle_watch(PS3T_CLARITY, gfx_option_changed);
    ps3_toggle_watch(PS3T_LUT_PRESET, gfx_option_changed);
    __atomic_store_n(&s_gfx_dirty, 0, __ATOMIC_RELAXED);
    gfx_options_read();
    res_scale_load();
    const char* e = getenv("PS3_UPSCALE");
    s_gfx_upscale = (e && !strcasecmp(e, "metalfx") && !s_headless);
    if (e && *e && strcasecmp(e, "metalfx") && strcasecmp(e, "off") && strcmp(e, "0"))
        fprintf(stderr, "[RSX metal] PS3_UPSCALE=%s unknown (metalfx); off\n", e);
    if (e && !strcasecmp(e, "metalfx") && s_headless)
        fprintf(stderr, "[RSX metal] PS3_UPSCALE=metalfx ignored headless\n");
    __atomic_store_n(&s_gfx_upscale_on, s_gfx_upscale, __ATOMIC_RELEASE);
    if (s_gfx_aniso || s_gfx_fxaa || s_gfx_smaa || s_gfx_sharpen > 0.0f || s_gfx_upscale ||
        s_gfx_vibrance != 0.0f || s_gfx_contrast != 1.0f || s_gfx_vignette > 0.0f || s_gfx_grain > 0.0f ||
        s_gfx_shadows != 0.0f || s_gfx_midtones != 0.0f || s_gfx_highlights != 0.0f ||
        s_gfx_clarity != 0.0f || s_gfx_lut_preset)
        fprintf(stderr, "[RSX metal] graphics: anisotropic %dx, AA %s, sharpen %.2f, upscale %s,"
                        " vibrance %.2f, contrast %.2f, curves %.2f/%.2f/%.2f, clarity %.2f, LUT preset %s\n",
                s_gfx_aniso ? s_gfx_aniso : 1, s_gfx_fxaa ? "FXAA" : s_gfx_smaa ? "SMAA" : "off",
                (double)s_gfx_sharpen, s_gfx_upscale ? "MetalFX spatial" : "off",
                (double)s_gfx_vibrance, (double)s_gfx_contrast, (double)s_gfx_shadows,
                (double)s_gfx_midtones, (double)s_gfx_highlights, (double)s_gfx_clarity,
                kGfxLutPreset[s_gfx_lut_preset]);
}

/* The MetalFX path gave up: say why once; the next pump puts the drawable
 * back at the guest's size, which is the path without it. */
static void gfx_upscale_fail(const char* why)
{
    if (!__atomic_exchange_n(&s_gfx_upscale_on, 0, __ATOMIC_ACQ_REL)) return;
    fprintf(stderr, "[RSX metal] MetalFX upscaling unavailable (%s); presenting at the guest size\n", why);
}

/* Each initialization owns its counters. A drawable callback retained by the
 * compositor can outlive shutdown without writing into a later session. */
#if !TARGET_OS_IPHONE
@interface MetalFrameRateSession : NSObject {
@public
    pthread_mutex_t lock;
    MetalFrameRate stats;
}
@end
@implementation MetalFrameRateSession
- (instancetype)init {
    if ((self=[super init])) {
        pthread_mutex_init(&lock,NULL);
        metal_frame_rate_reset(&stats,CACurrentMediaTime());
    }
    return self;
}
- (void)dealloc { pthread_mutex_destroy(&lock); }
@end
static MetalFrameRateSession *s_fps_session;
static dispatch_source_t s_fps_timer;
static pthread_mutex_t s_fps_session_lock=PTHREAD_MUTEX_INITIALIZER;
static MetalFrameRateSession *fps_session(void)
{
    pthread_mutex_lock(&s_fps_session_lock);
    MetalFrameRateSession *session=s_fps_session;
    pthread_mutex_unlock(&s_fps_session_lock);
    return session;
}

static void fps_track_drawable(id<CAMetalDrawable> drawable)
{
    MetalFrameRateSession *session=fps_session();
    if (!session || !drawable) return;
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        pthread_mutex_lock(&session->lock);
        metal_frame_rate_presented(&session->stats,shown.presentedTime);
        pthread_mutex_unlock(&session->lock);
    }];
}

void rsx_metal_backend_note_guest_frame(void)
{
    MetalFrameRateSession *session=fps_session();
    if (!session) return;
    pthread_mutex_lock(&session->lock);
    metal_frame_rate_guest(&session->stats);
    pthread_mutex_unlock(&session->lock);
}

/* Main queue timer continues to report zero FPS even when frame submission
 * blocks. It changes only the title, with no extra GPU work or image readback. */
static void fps_stop(void)
{
    if (s_fps_timer) { dispatch_source_cancel(s_fps_timer); s_fps_timer=nil; }
    pthread_mutex_lock(&s_fps_session_lock);
    s_fps_session=nil;
    pthread_mutex_unlock(&s_fps_session_lock);
}
/* PS3RECOMP_METAL_FPS=1, which PS3_TOGGLE_FILE can switch while the window is
 * open: the watcher starts or stops the counter on the main queue, and
 * stopping puts the window's own title back. */
static int s_fps_want=-1;
static NSString *s_fps_title0;
static void fps_start(void);
static void fps_toggle_changed(int id,int on)
{
    (void)id; on=on!=0;
    const int was=__atomic_exchange_n(&s_fps_want,on,__ATOMIC_RELAXED);
    if (was<0 || was==on) return;   /* the registration itself, or no change */
    dispatch_async(dispatch_get_main_queue(),^{
        if (!s_window || s_headless) return;
        if (on) fps_start();
        else { fps_stop(); if (s_fps_title0) s_window.title=s_fps_title0; }
    });
}
static void fps_start(void)
{
    fps_stop();
    ps3_toggle_watch(PS3T_METAL_FPS,fps_toggle_changed);
    if (!ps3_toggle_value(PS3T_METAL_FPS) || s_headless || !s_window) return;
    if (!s_fps_title0) s_fps_title0=[s_window.title copy];
    MetalFrameRateSession *session=[MetalFrameRateSession new];
    pthread_mutex_lock(&s_fps_session_lock);
    s_fps_session=session;
    pthread_mutex_unlock(&s_fps_session_lock);
    NSWindow *window=s_window;
    NSString *title=[window.title copy];
    s_fps_timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(s_fps_timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),NSEC_PER_SEC,50*NSEC_PER_MSEC);
    dispatch_source_set_event_handler(s_fps_timer,^{
        if (fps_session()!=session) return;
        pthread_mutex_lock(&session->lock);
        MetalFrameRateSample r=metal_frame_rate_sample(&session->stats,CACurrentMediaTime());
        pthread_mutex_unlock(&session->lock);
        window.title=[NSString stringWithFormat:@"%@ | display %.1f FPS | guest %.1f FPS | %.1f ms avg / %.1f p95 / %.1f max | age %.1f s",
            title,r.display_fps,r.guest_fps,r.average_ms,r.p95_ms,r.worst_ms,r.age_seconds];
        fprintf(stderr,"[RSX FPS] display=%.2f guest=%.2f avg_ms=%.2f p95_ms=%.2f max_ms=%.2f age_s=%.2f\n",
            r.display_fps,r.guest_fps,r.average_ms,r.p95_ms,r.worst_ms,r.age_seconds);
    });
    dispatch_resume(s_fps_timer);
}

#else
static void fps_track_drawable(id<CAMetalDrawable> drawable) { (void)drawable; }
void rsx_metal_backend_note_guest_frame(void) {}
#endif

/* ---- vertex layout ----------------------------------------------------------
 * Every draw carries all 16 RSX vertex attributes as float4, fetched on the
 * CPU by rsx_fetch_attrib: 256 bytes per vertex, attribute i at offset i*16.
 * That is the layout a decompiled vertex program expects (ATTRn arrives at
 * [[attribute(n)]]), and the built-in shader reads slots 0, 3 and 8 of the
 * same layout, so one staging buffer and one vertex descriptor serve both.
 * --------------------------------------------------------------------------*/

#define MTL_ATTRIBS        16
#define MTL_MAX_RECORDS    4096
#define MTL_MAX_VERTS      (256u * 1024u)
#define MTL_VB_INDEX       30      /* buffer(0) and buffer(1) are the constant blocks */

typedef struct { float a[MTL_ATTRIBS][4]; } MtlVertex;

/* ---- the frame's record stream ----------------------------------------------
 * RSX work arrives while the guest builds its frame; the flip comes later. So
 * draws are recorded with their vertices already fetched out of guest memory,
 * then replayed at present time. This mirrors the D3D12 backend's
 * D3D12DrawRecord / render_frame split.
 *
 * Clears are records in the same ordered stream, because a clear in the middle
 * of a frame is something a title really does -- it draws the world, clears
 * depth, and draws the first-person weapon over it. Replaying every draw into
 * one pass that clears once at the top loses that clear entirely. So a clear
 * record ends the open render pass and starts a new one whose flagged
 * attachments load-action Clear and whose others load-action Load.
 *
 * Render targets hang off the same stream: a target change is one more reason
 * to end the pass and open the next one somewhere else.
 * --------------------------------------------------------------------------*/

/* NV4097_CLEAR_SURFACE flags: 0x01 depth, 0x02 stencil, and one bit per colour
 * channel in 0xF0 (nouveau's NV30_3D_CLEAR_BUFFERS layout). A title clears the
 * four colour channels together, so 0xF0 is treated as one colour bit here,
 * exactly as the old clear callback did. */
#define MTL_CLEAR_DEPTH    0x01u
#define MTL_CLEAR_STENCIL  0x02u
#define MTL_CLEAR_COLOR    0xF0u

/* Which surfaces a record is aimed at, latched when the record is made because
 * the guest retargets several times inside a frame and present replays it long
 * after the registers moved on. Every record carries one, draws and clears
 * alike: a clear belongs to a surface as much as a draw does. */
typedef struct {
    u32 off;                  /* 0 = a display buffer (the drawable); else the
                               * raw RSX offset an offscreen surface is keyed by */
    u32 mrt[3];               /* colour targets B, C, D, 0 = none  */
    u32 w, h;                 /* the surface's size at record time */
    u32 fmt;                  /* SET_SURFACE_FORMAT                */
    u32 zeta_off;             /* SET_SURFACE_ZETA_OFFSET           */
    /* The guest viewport rect in target pixels. Only draws use it; a clear is
     * a load action over the whole attachment. */
    u32 vp_x, vp_y, vp_w, vp_h;
} MtlTarget;

typedef struct {
    u32 base;        /* first vertex in s_verts               */
    u32 count;       /* vertex count after primitive expansion */
    MTLPrimitiveType topology;
    int blend_enable;
    u32 blend_sfactor, blend_dfactor, blend_equation;
    u32 color_mask;  /* NV4097_SET_COLOR_MASK word              */
    MTLCullMode cull;
    MTLWinding  winding;
    int ds_idx;      /* slot in s_ds_cache, or -1 for no test / no writes */
    u32 stencil_ref; /* SET_STENCIL_FUNC_REF; encoder state, not pipeline */
    /* Guest programs: cache slots, or -1 when the draw uses the built-in
     * shader. Slots are stable for the whole frame (see s_caches_full). */
    int vs_idx, fs_idx;
    u32 vp_cb_off;            /* VPConst block in s_cb                  */
    u32 fp_cb_off, fp_cb_len; /* PSConstants block in s_cb              */
    /* Per texture unit: texture and sampler cache slots, or -1 for the
     * zero texture / default sampler. Guest-program draws only. */
    int tex[RSX_MAX_TEXTURES];
    int samp[RSX_MAX_TEXTURES];
    /* The same, for the four vertex-texture units a transform program's TXL
     * samples. They share the caches with the fragment units. */
    int vtex[RSX_MAX_VERTEX_TEXTURES];
    int vsamp[RSX_MAX_VERTEX_TEXTURES];
    /* Render-to-texture: the surface-view slot a unit samples, -1 = none. It
     * takes precedence over tex[], which is the guest-memory upload. */
    int tex_rt[RSX_MAX_TEXTURES];
    MtlTarget rt;
    float mvp[16];   /* built-in path: 4 rows of the RSX vertex-constant matrix */
} MtlDraw;

/* One NV4097_CLEAR_SURFACE, with the values rsx_commands.c decoded out of
 * SET_COLOR_CLEAR_VALUE and SET_ZSTENCIL_CLEAR_VALUE. */
typedef struct {
    u32   flags;     /* CLEAR_SURFACE mask                     */
    u32   color;     /* ARGB8888                               */
    float depth;     /* [0,1]                                  */
    u8    stencil;
    MtlTarget rt;
} MtlClear;

typedef enum { MTL_REC_DRAW, MTL_REC_CLEAR } MtlRecordKind;

typedef struct {
    MtlRecordKind kind;
    union { MtlDraw draw; MtlClear clear; } u;
} MtlRecord;

static MtlVertex* s_verts;
static u32        s_vert_count;
static MtlRecord  s_records[MTL_MAX_RECORDS];
static u32        s_rec_count;
static u32        s_dropped_records;
static u32        s_guest_draws;       /* draws through guest programs this frame */
static u32        s_last_guest_draws;  /* ... in the last presented frame        */

static id<MTLLibrary> s_shader_lib;    /* built-in fixed-function shaders */

/* ---- per-draw constant blocks -----------------------------------------------
 * VPConst is the 512 float4 transform constants plus the viewport epilogue's
 * posscale/posoffset (8224 bytes); PSConstants is the fragment program's
 * inline constants plus fp_alpha. Constants change between draws in a frame,
 * so each draw snapshots its own blocks -- the D3D12 backend keeps a per-draw
 * VP_CB_STRIDE slot for the same reason. Both blocks are past the 4 KB limit
 * of setVertexBytes:, and Metal wants setVertexBuffer:offset: 256-aligned,
 * so they are appended here at record time and uploaded once per frame in
 * one buffer, exactly like the vertices.
 * --------------------------------------------------------------------------*/

#define MTL_VP_CB_BYTES  ((RSX_MAX_VERTEX_CONSTANTS + 2) * 16)
#define MTL_CB_ALIGN     256u
#define MTL_CB_MAX       (256u << 20)
static u8* s_cb;
static u32 s_cb_used, s_cb_cap;

/* ---- guest shader caches ----------------------------------------------------
 * Vertex programs are keyed on their microcode bytes, fragment programs on
 * rsx_fp_structural_hash (the code, not the inline constants, which live in
 * PSConstants and change per draw) plus the export width and the alpha test
 * that gets patched into the source. A slot with a nil function is a program
 * that failed to translate: it is remembered so the draw does not retry the
 * whole pipeline on every frame.
 *
 * Draw records hold slot indices, so slots must not move while a frame is
 * being recorded. When a cache fills, translation stops for the rest of the
 * frame (those draws use the built-in shader) and every cache is emptied
 * after the frame presents, rather than evicting under a live record.
 * --------------------------------------------------------------------------*/

#define MTL_VP_CACHE     1024
#define MTL_FP_CACHE     2048
#define MTL_PSO_CACHE    2048
#define MTL_FP_MAX_BYTES 4096u     /* bound on a fragment program, as D3D12 */

typedef struct { u32 hash; id<MTLFunction> fn; } MtlVpEntry;
typedef struct { u64 key;  id<MTLFunction> fn; u32 nconst; } MtlFpEntry;
/* A pipeline state is bound inside a render pass, so the pass's colour format
 * and target count are part of what identifies it: the same shaders against a
 * float surface and against the drawable are two pipelines. The depth format
 * is not, because every pass binds MTL_DEPTH_FORMAT. */
typedef struct {
    int vs, fs; u32 blend; u32 cmask;
    u32 color_pf, nrt;
} MtlPsoKey;
typedef struct { MtlPsoKey key; id<MTLRenderPipelineState> pso; } MtlPsoEntry;

static MtlVpEntry  s_vp_cache[MTL_VP_CACHE];
static MtlFpEntry  s_fp_cache[MTL_FP_CACHE];
static MtlPsoEntry s_pso_cache[MTL_PSO_CACHE];
static u32 s_vp_count, s_fp_count, s_pso_count;
static int s_caches_full;
static int s_guest_shaders;            /* translator present and not disabled */
static rsx_fp_constant_block s_fp_consts;

/* The decompilers' HLSL and the MSL made from it. The VP decompiler builds
 * bodies up to 192 KB, and the legalized MSL runs longer than its HLSL. */
static char s_hlsl[512 * 1024];
static char s_msl[512 * 1024];
static char s_log[8192];

/* What a fragment program samples before guest textures are bound (task:
 * textures). A D3D12 null SRV reads zero; Metal validation rejects a nil
 * texture on use, so zero is a real 1x1 texture here. */
static id<MTLTexture>      s_null_tex;
static id<MTLSamplerState> s_default_sampler;
/* What a draw gets when the depth/stencil cache is full: no test, no writes. */
static id<MTLDepthStencilState> s_ds_default;

/* ---- guest textures ---------------------------------------------------------
 * Keyed on where the bytes are and what the register says they are, plus a
 * sparse checksum of the bytes themselves, because titles animate textures
 * in place (the D3D12 backend's tex_csum exists for the same reason). A
 * changed checksum gets a fresh MTLTexture rather than an in-place write: a
 * frame in flight may still be sampling the old one. The checksum covers the
 * whole mip chain, not just level 0, so a title animating a lower level is
 * noticed. Same slot-stability rule as the shader caches. Samplers are keyed
 * on the wrap, filter and LOD registers.
 * --------------------------------------------------------------------------*/

#define MTL_TEX_CACHE   1024
#define MTL_SAMP_CACHE  64

typedef struct {
    u32 ea, w, h, format, control1, control3, csum;
    id<MTLTexture> tex;
} MtlTexEntry;
typedef struct { u64 key; id<MTLSamplerState> samp; } MtlSampEntry;

static MtlTexEntry  s_tex_cache[MTL_TEX_CACHE];
static MtlSampEntry s_samp_cache[MTL_SAMP_CACHE];
static u32 s_tex_count, s_samp_count;
static u8* s_tex_staging;
static u32 s_tex_staging_cap;

/* Frames the CPU may run ahead of the GPU.
 *
 * Presenting used to end in -waitUntilCompleted unconditionally, which is the
 * one pattern where a translation layer or a driver cannot hide its encoding
 * work: the CPU stalls on every frame instead of preparing the next one.
 * Dolphin measured exactly this shape (their bounding-box path) as the single
 * largest CPU-side penalty in their Metal/MoltenVK comparison.
 *
 * Windowed presents now bound in-flight frames with a semaphore released from
 * the command buffer's completion handler, so the CPU keeps working while the
 * GPU drains. Headless still waits, because the readback has to observe a
 * finished frame -- there the stall is the point. */
#define MTL_MAX_INFLIGHT 3
static dispatch_semaphore_t s_inflight;

/* RSX clear colour, ARGB8888, as written by NV4097_SET_COLOR_CLEAR_VALUE. */
static u32 s_clear_argb = 0xFF000000u;
static u32 s_last_present_bgra;

/* ---- rsx_backend vtable -------------------------------------------------- */

/* The draw callbacks are not handed the state, so it is latched here. Every
 * state setter caches it; set_vertex_attribs in particular always fires before
 * a draw. The D3D12 backend does the same via s_d3d.current_rsx_state. */
static const rsx_state* s_state;

/* Which surfaces the live state is aimed at; see the guest surfaces section. */
static void target_for(const rsx_state* st, MtlTarget* t);

static void mtl_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    /* The debug hook reports the last colour the guest asked for, whether or
     * not the record stream had room for the clear itself. */
    if (flags & MTL_CLEAR_COLOR) s_clear_argb = color;
    if (!s_ready) return;
    if (!(flags & (MTL_CLEAR_COLOR | MTL_CLEAR_DEPTH | MTL_CLEAR_STENCIL))) return;
    if (s_rec_count >= MTL_MAX_RECORDS) { s_dropped_records++; return; }

    MtlRecord* r = &s_records[s_rec_count++];
    r->kind = MTL_REC_CLEAR;
    r->u.clear.flags   = flags;
    r->u.clear.color   = color;
    r->u.clear.depth   = depth;
    r->u.clear.stencil = stencil;
    /* A clear belongs to a surface as much as a draw does: a title clears its
     * shadow map, renders into it, then clears the display. */
    target_for(s_state, &r->u.clear.rt);
}

static int create_depth(void);

/* Is this raw RSX offset one of the buffers cellGcmSetDisplayBuffer
 * registered? The D3D12 backend takes the same private hook to tell a draw
 * aimed at the scanout from one aimed at an offscreen surface. */
extern int cellGcmOffsetIsDisplay(u32 offset);

static void mtl_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    if (state) s_state = state;
    if (!state) return;
    /* The surface clip is the size of whatever surface is bound, so only a
     * display buffer's clip says anything about the window: a title that
     * renders a 256x256 effect into an offscreen surface would otherwise
     * shrink the drawable to it, and never grow it back. */
    if (!cellGcmOffsetIsDisplay(state->surface_color_offset[
            state->color_target == CELL_GCM_SURFACE_TARGET_1 ? 1 : 0]))
        return;
    u32 w = state->surface_clip_w, h = state->surface_clip_h;
    if (!w || !h || (w == s_width && h == s_height)) return;
    s_width = w; s_height = h;
    if (s_layer) s_layer.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);
    if (s_depth && create_depth() != 0)
        fprintf(stderr, "[RSX metal] depth/stencil resize to %ux%u failed; "
                        "the old buffer stays bound\n", w, h);
}

static void mtl_latch_state(void* ud, const rsx_state* state)
{ (void)ud; if (state) s_state = state; }

static void mtl_draw_arrays(void*, u32, u32, u32);
static void mtl_draw_indexed(void*, u32, u32, u32);

static rsx_backend s_backend_vtable = {
    .userdata           = NULL,
    .clear              = mtl_clear,
    .set_render_target  = mtl_set_render_target,
    .set_vertex_attribs = mtl_latch_state,
    .set_blend          = mtl_latch_state,
    .set_depth_stencil  = mtl_latch_state,
    .set_viewport       = mtl_latch_state,
    .set_color_mask     = mtl_latch_state,
    .set_alpha_test     = mtl_latch_state,
    /* Programs are resolved when the draw is recorded, from the live state:
     * SET_SHADER_PROGRAM, the transform program words and SHADER_CONTROL
     * all land there before BEGIN_END fires this. */
    .set_shader         = mtl_latch_state,
    .draw_arrays        = mtl_draw_arrays,
    .draw_indexed       = mtl_draw_indexed,
};

/* ---- helpers ------------------------------------------------------------- */

static MTLClearColor clear_color_from_argb(u32 argb)
{
    /* ARGB8888 -> normalised RGBA. sRGB conversion is deliberately skipped:
     * the D3D12 backend treats the guest value as raw UNORM too, so both
     * backends agree pixel-for-pixel. */
    const double a = (double)((argb >> 24) & 0xFF) / 255.0;
    const double r = (double)((argb >> 16) & 0xFF) / 255.0;
    const double g = (double)((argb >>  8) & 0xFF) / 255.0;
    const double b = (double)( argb        & 0xFF) / 255.0;
    return MTLClearColorMake(r, g, b, a);
}

static u32 fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 fnv1a64(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* One depth/stencil texture for the display target, sized like the colour
 * attachment, shared by every pass the frame opens -- the D3D12 backend shares
 * one depth buffer the same way, because a PS3 title typically uses a single
 * zeta surface. Zeta offsets and per-target depth belong with render targets.
 *
 * D24S8 is not a format Apple GPUs have, so the depth is 32-bit float. Both
 * hold the guest's [0,1] window z, with more precision than the RSX's 24 bits
 * rather than less. The texture is never read back, so it is private.
 *
 * Assigned only on success: a failed resize keeps the old texture bound rather
 * than leaving passes and pipelines disagreeing about whether depth exists. */
#define MTL_DEPTH_FORMAT   MTLPixelFormatDepth32Float_Stencil8

static int create_depth(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTL_DEPTH_FORMAT
                                                           width:s_width
                                                          height:s_height
                                                       mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    if (!t) return -1;
    s_depth = t;
    return 0;
}

static int create_offscreen(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:res_px_x(s_width)
                                                          height:res_px(s_height)
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_offscreen = [s_dev newTextureWithDescriptor:td];
    return s_offscreen ? 0 : -1;
}

#if !TARGET_OS_IPHONE
/* ---- the window: modes, letterbox, icon, cursor capture ---------------------
 * Everything here runs on the main thread: at creation, from the event pump
 * and from the window's delegate.
 *
 * Letterbox: the drawable keeps the guest's resolution whatever the window's
 * size, and the layer scales it to fit with its aspect ratio kept (black
 * bars), which is what the compositor already did to it on a Retina screen.
 *
 * Fullscreen comes in two kinds: macOS's own (the window moves to a Space of
 * its own, with the system's animation) and borderless (the window drops its
 * frame and covers its screen in place, menu bar and Dock hidden; switching
 * apps is instant). One toggle leaves whichever is on, or enters the kind the
 * shortcut asks for. */
enum { WM_WINDOWED, WM_FULLSCREEN, WM_BORDERLESS };
static int               s_wm_fs_style = WM_FULLSCREEN;  /* what Cmd+F / Option+Return enter */
static int               s_wm_borderless;
static NSRect            s_wm_saved_frame;
static NSWindowStyleMask s_wm_saved_style;
static int               s_cap_on;        /* capture wanted (mouse camera on, capture not turned off) */
static int               s_captured;
static NSTimeInterval    s_cap_since;     /* motion before this is the warp, not the player */
static id                s_win_delegate;  /* NSWindow.delegate is weak */
static volatile int      s_wm_mode_now;   /* WM_*, for the in-game menu (pump keeps it) */
static int               s_wm_after_exit = -1;  /* the menu asked for borderless from macOS fullscreen */

/* A window without a title bar cannot become key unless it says it can, and
 * then no key ever reaches the game in borderless fullscreen. */
@interface PS3RecompWindow : NSWindow
@end
@implementation PS3RecompWindow
- (BOOL)canBecomeKeyWindow  { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

static void mouse_release(void)
{
    if (!s_captured) return;
    s_captured = 0;
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
    extern volatile uint32_t g_pad_mouse_buttons;
    g_pad_mouse_buttons = 0;
}

static void mouse_capture(void)
{
    if (s_captured || !s_cap_on || !s_window) return;
    /* Park the cursor in the middle of the game, so it comes back there and
     * not wherever the motion would have carried it. CG's global space has
     * its origin at the top left of the primary screen, AppKit's at the
     * bottom left. */
    const NSRect r = [s_window convertRectToScreen:[[s_window contentView] frame]];
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (screens.count) {
        const CGFloat top = NSMaxY(screens[0].frame);
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r)));
    }
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    s_captured  = 1;
    s_cap_since = [[NSProcessInfo processInfo] systemUptime] + 0.05;
    extern volatile int32_t g_pad_mouse_dx, g_pad_mouse_dy;
    __atomic_store_n(&g_pad_mouse_dx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pad_mouse_dy, 0, __ATOMIC_RELAXED);
}

/* The drawable must stay at the guest's size for the layer to letterbox it;
 * re-assert it in case AppKit resized it along with the view. With MetalFX
 * upscaling it is the view's backing pixel size fitted to the guest's aspect
 * instead (the layer still letterboxes whatever rounding is left), and the
 * guest's size whenever that would not be larger. Main thread. */
static void wm_keep_drawable(void)
{
    if (!s_layer) return;
    /* PS3_RES_SCALE: the game's image is that much larger, and so is this. */
    const u32 bw = res_px_x(s_width), bh = res_px(s_height);
    CGSize want = CGSizeMake((CGFloat)bw, (CGFloat)bh);
    if (__atomic_load_n(&s_gfx_upscale_on, __ATOMIC_ACQUIRE) && s_window) {
        NSView* v = [s_window contentView];
        const NSSize px = v ? [v convertSizeToBacking:[v bounds].size] : NSZeroSize;
        double fw = floor(px.width), fh = floor(px.height);
        if (fw * bh > fh * bw) fw = floor(fh * bw / bh + 0.5);   /* PS3_ASPECT: the wide image's shape */
        else                   fh = floor(fw * bh / bw + 0.5);
        if (fw > bw && fh > bh) want = CGSizeMake(fw, fh);
    }
    if (!CGSizeEqualToSize(s_layer.drawableSize, want)) s_layer.drawableSize = want;
}

static int wm_native_fullscreen(void)
{
    return s_window && ([s_window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

static void wm_enter_borderless(void)
{
    if (!s_window || s_wm_borderless || wm_native_fullscreen()) return;
    NSScreen* scr = [s_window screen] ? [s_window screen] : [NSScreen mainScreen];
    if (!scr) return;
    s_wm_saved_frame = [s_window frame];
    s_wm_saved_style = [s_window styleMask];
    s_wm_borderless  = 1;
    [NSApp setPresentationOptions:(NSApplicationPresentationHideDock |
                                   NSApplicationPresentationHideMenuBar)];
    [s_window setStyleMask:NSWindowStyleMaskBorderless];
    [s_window setFrame:[scr frame] display:YES];
    [s_window makeKeyAndOrderFront:nil];
    wm_keep_drawable();
}

static void wm_leave_borderless(void)
{
    if (!s_window || !s_wm_borderless) return;
    s_wm_borderless = 0;
    [s_window setStyleMask:s_wm_saved_style];
    [s_window setFrame:s_wm_saved_frame display:YES];
    [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    [s_window makeKeyAndOrderFront:nil];
    wm_keep_drawable();
}

/* Leave fullscreen if any is on, else enter the kind asked for. */
static void wm_toggle(int style)
{
    if (!s_window) return;
    if (s_wm_borderless)              { wm_leave_borderless(); return; }
    if (wm_native_fullscreen())       { [s_window toggleFullScreen:nil]; return; }
    if (style == WM_BORDERLESS) wm_enter_borderless();
    else                        [s_window toggleFullScreen:nil];
}

@interface PS3RecompWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation PS3RecompWindowDelegate
- (void)windowDidResignKey:(NSNotification*)n  { (void)n; mouse_release(); }
- (void)windowDidResize:(NSNotification*)n     { (void)n; wm_keep_drawable(); }
- (void)windowDidEnterFullScreen:(NSNotification*)n { (void)n; wm_keep_drawable(); }
- (void)windowDidExitFullScreen:(NSNotification*)n
{
    (void)n; wm_keep_drawable();
    if (s_wm_after_exit == WM_BORDERLESS) wm_enter_borderless();
    s_wm_after_exit = -1;
}
- (void)windowWillClose:(NSNotification*)n
{
    (void)n;
    mouse_release();
    if (s_wm_borderless) [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    s_wm_borderless = 0;
    s_closed = 1;
}
@end

/* PS3_WINDOW_ICON: the Dock / app switcher icon. A plain executable has none
 * of its own, so without this the Dock shows a generic one. */
static void apply_icon(void)
{
    const char* e = getenv("PS3_WINDOW_ICON");
    if (!e || !*e || !strcmp(e, "none") || !strcmp(e, "0")) return;
    char path[2048];
    if (!strcmp(e, "game")) {             /* the player's own copy: PS3_GAME/ICON0.PNG */
        const char* root = getenv("PS3_VFS_ROOT");
        snprintf(path, sizeof path, "%s/PS3_GAME/ICON0.PNG", root && *root ? root : ".");
    } else {
        snprintf(path, sizeof path, "%s", e);
    }
    NSImage* img = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:path]];
    if (!img) {
        fprintf(stderr, "[RSX metal] PS3_WINDOW_ICON: %s is not an image AppKit reads\n", path);
        return;
    }
    /* The Dock draws the icon square; a wide one (a PS3 ICON0 is 320x176) is
     * centred on a transparent square rather than stretched. */
    const NSSize sz = [img size];
    if (sz.width > 0 && sz.height > 0 && sz.width != sz.height) {
        const CGFloat d = sz.width > sz.height ? sz.width : sz.height;
        NSImage* src = img;
        img = [NSImage imageWithSize:NSMakeSize(d, d) flipped:NO
                      drawingHandler:^BOOL(NSRect dst) {
            (void)dst;
            [src drawInRect:NSMakeRect((d - sz.width) / 2, (d - sz.height) / 2, sz.width, sz.height)];
            return YES;
        }];
    }
    [NSApp setApplicationIconImage:img];
}

/* PS3_DISPLAY=<n>|<name>: the screen the window opens on, and so the one
 * fullscreen and borderless cover. <n> counts from 1 in the system's order
 * (1 = the screen with the menu bar); a name is matched against the screens'
 * names as System Settings shows them ("Built-in Retina Display", "DELL
 * U2720Q"): exactly, then ignoring case, then as a part of one. Unset, or a
 * screen that is not there: the main screen, as before. */
static NSScreen* wm_pick_screen(const char** why)
{
    NSArray<NSScreen*>* screens = [NSScreen screens];
    NSScreen* fallback = [NSScreen mainScreen];
    const char* e = getenv("PS3_DISPLAY");
    *why = "main screen";
    if (!e || !*e || !screens.count) return fallback;
    char* end = NULL;
    const long k = strtol(e, &end, 10);
    if (end != e && *end == 0) {
        if (k >= 1 && k <= (long)screens.count) { *why = "PS3_DISPLAY"; return screens[(NSUInteger)(k - 1)]; }
        fprintf(stderr, "[RSX metal] PS3_DISPLAY=%s: there are %lu screen(s), using the main one\n",
                e, (unsigned long)screens.count);
        return fallback;
    }
    if (@available(macOS 10.15, *)) {
        NSString* want = [NSString stringWithUTF8String:e];
        for (int pass = 0; want && pass < 3; pass++)
            for (NSScreen* sc in screens) {
                NSString* n = sc.localizedName;
                if (!n) continue;
                if ((pass == 0 && [n isEqualToString:want]) ||
                    (pass == 1 && [n caseInsensitiveCompare:want] == NSOrderedSame) ||
                    (pass == 2 && [n rangeOfString:want options:NSCaseInsensitiveSearch].location != NSNotFound)) {
                    *why = "PS3_DISPLAY";
                    return sc;
                }
            }
        fprintf(stderr, "[RSX metal] PS3_DISPLAY=%s: no such screen, using the main one. Screens:\n", e);
        int i = 1;
        for (NSScreen* sc in screens)
            fprintf(stderr, "[RSX metal]   %d: %s\n", i++, sc.localizedName ? [sc.localizedName UTF8String] : "?");
    } else {
        fprintf(stderr, "[RSX metal] PS3_DISPLAY=%s: names need macOS 10.15; give a number\n", e);
    }
    return fallback;
}

static const char* wm_screen_name(NSScreen* scr)
{
    if (@available(macOS 10.15, *)) {
        if (scr && scr.localizedName) return [scr.localizedName UTF8String];
    }
    return "?";
}

static int create_window_impl(const char* title)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    /* The host turns a plain run loop, not [NSApp run]: finish launching by
     * hand (SDL and GLFW do the same), or the app never fully becomes a
     * regular one -- macOS fullscreen and the Dock icon need that. */
    { static int launched;
      if (!launched) { launched = 1; [NSApp finishLaunching]; } }
    apply_icon();

    int mode = WM_WINDOWED;
    { const char* e = getenv("PS3_WINDOW_MODE");
      if (e && (!strcmp(e, "fullscreen") || !strcmp(e, "native"))) mode = WM_FULLSCREEN;
      else if (e && !strcmp(e, "borderless"))                      mode = WM_BORDERLESS;
      else if (e && *e && strcmp(e, "windowed") && strcmp(e, "window"))
          fprintf(stderr, "[RSX metal] PS3_WINDOW_MODE=%s: expected windowed, fullscreen or borderless\n", e);
      s_wm_fs_style = mode == WM_BORDERLESS ? WM_BORDERLESS : WM_FULLSCREEN; }
    /* PS3_MOUSE_CAPTURE, or [mouse] capture in PS3_INPUT_MAP (cellPad.c).
     * Whether the mouse turns the camera at all (PS3_MOUSE, PS3_INPUT_MODE)
     * can change while the game runs, from the in-game menu: the pump asks
     * cellPad_host_mouse_active before it captures. */
    { extern int cellPad_host_mouse_capture_setting(void);
      s_cap_on = cellPad_host_mouse_capture_setting(); }

    /* Content size: PS3_WINDOW_SIZE, else the guest's; shrunk, keeping its
     * shape, to what the screen has room for. */
    CGFloat cw = floor((CGFloat)s_width * s_aspect_k + 0.5), ch = (CGFloat)s_height;   /* PS3_ASPECT */
    { const char* e = getenv("PS3_WINDOW_SIZE");
      unsigned w = 0, h = 0;
      if (e && *e) {
          if (sscanf(e, "%ux%u", &w, &h) == 2 && w >= 160 && h >= 90 && w <= 16384 && h <= 16384) {
              cw = (CGFloat)w; ch = (CGFloat)h;
          } else {
              fprintf(stderr, "[RSX metal] PS3_WINDOW_SIZE=%s: expected <width>x<height>\n", e);
          }
      } }
    const char* scr_why = "";
    NSScreen* scr = wm_pick_screen(&scr_why);
    if (scr) {
        const NSRect vis = [NSWindow contentRectForFrameRect:[scr visibleFrame]
                                                   styleMask:NSWindowStyleMaskTitled];
        const CGFloat k = fmin(1.0, fmin(vis.size.width / cw, vis.size.height / ch));
        if (k < 1.0) { cw = floor(cw * k); ch = floor(ch * k); }
    }

    NSRect frame = NSMakeRect(0, 0, cw, ch);
    s_window = [[PS3RecompWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!s_window) return -1;

    [s_window setTitle:[NSString stringWithUTF8String:(title ? title : "ps3recomp")]];
    /* Mouse-moved events are off by default; the mouse camera (cellPad.c) needs them. */
    [s_window setAcceptsMouseMovedEvents:YES];
    [s_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [s_window setContentMinSize:NSMakeSize(320, 180)];
    [s_window setBackgroundColor:[NSColor blackColor]];
    [s_window center];
    /* center goes by the main screen; put it in the middle of the chosen one
     * (fullscreen and borderless then cover that screen). */
    if (scr && scr != [s_window screen]) {
        const NSRect vf = [scr visibleFrame], wf = [s_window frame];
        [s_window setFrameOrigin:NSMakePoint(floor(NSMidX(vf) - wf.size.width / 2),
                                             floor(NSMidY(vf) - wf.size.height / 2))];
    }
    s_win_delegate = [PS3RecompWindowDelegate new];
    [s_window setDelegate:s_win_delegate];

    s_layer = [CAMetalLayer layer];
    s_layer.device          = s_dev;
    s_layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = YES;
    s_layer.drawableSize    = CGSizeMake((CGFloat)res_px_x(s_width), (CGFloat)res_px(s_height));
    s_layer.contentsGravity = kCAGravityResizeAspect;
    s_layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    /* PS3_VSYNC=0: present as soon as a frame is ready instead of at the
     * screen's next refresh (lower latency; may tear in fullscreen). The
     * game's own 30/60 fps pacing does not depend on this: it runs off the
     * host's vblank clock (frame_clock in the port's main.cpp), never off
     * when a drawable reaches the screen. */
    int vsync = 1;
    { const char* e = getenv("PS3_VSYNC");
      if (e && *e) vsync = !(e[0] == '0' && e[1] == 0); }
    if (@available(macOS 10.13, *)) s_layer.displaySyncEnabled = vsync ? YES : NO;
    { extern void rsx_metal_note_vsync(int); rsx_metal_note_vsync(vsync); }

    NSView* view = [s_window contentView];
    [view setWantsLayer:YES];
    [view setLayer:s_layer];
    /* A layer-backed view sets its layer's gravity from this; keep the two
     * saying the same thing. */
    [view setLayerContentsPlacement:NSViewLayerContentsPlacementScaleProportionallyToFit];

    [s_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (mode == WM_BORDERLESS) wm_enter_borderless();
    /* macOS fullscreen animates and wants the window on screen first. */
    if (mode == WM_FULLSCREEN)
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s_window && !wm_native_fullscreen()) [s_window toggleFullScreen:nil];
        });
    fprintf(stderr, "[RSX metal] window %.0fx%.0f, %s on \"%s\" (%s), vsync %s; Cmd+F / Option+Return: "
                    "fullscreen, Cmd+Ctrl+F: macOS fullscreen%s\n", cw, ch,
            mode == WM_FULLSCREEN ? "fullscreen" : mode == WM_BORDERLESS ? "borderless" : "windowed",
            wm_screen_name(scr), scr_why, vsync ? "on" : "off",
            s_cap_on ? "; click captures the mouse, Esc or Cmd releases it" : "");
    fps_start();
    return 0;
}

static int create_window(const char* title)
{
    if ([NSThread isMainThread])
        return create_window_impl(title);
    __block int result = 0;
    NSString* t = title ? [NSString stringWithUTF8String:title] : nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = create_window_impl(t ? [t UTF8String] : NULL);
    });
    return result;
}
#endif

static int create_placeholders(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    s_null_tex = [s_dev newTextureWithDescriptor:td];
    if (!s_null_tex) return -1;
    u32 zero = 0;
    [s_null_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                    withBytes:&zero bytesPerRow:4];

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_default_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    if (!s_default_sampler) return -1;

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = NO;
    s_ds_default = [s_dev newDepthStencilStateWithDescriptor:dd];
    return s_ds_default ? 0 : -1;
}

/* ---- guest vertex fetch --------------------------------------------------
 * rsx_vertex_fetch.c holds the one definition of how a vertex comes out of
 * guest memory (this file used to carry a drifted copy). Every attribute is
 * fetched, used or not: a disabled array yields the constant attribute
 * register, which is what the hardware feeds a program that reads it.
 * --------------------------------------------------------------------------*/

static void fetch_vertex(const rsx_state* st, u32 vi, MtlVertex* out)
{
    for (int i = 0; i < MTL_ATTRIBS; i++)
        rsx_fetch_attrib(st, i, vi, out->a[i]);
}

/* ---- primitive conversion -------------------------------------------------
 * Metal, like D3D12, has no quads, quad strips, triangle fans or polygons, so
 * those are expanded to triangle lists on the CPU while the vertices are being
 * fetched. Everything else maps directly.
 * --------------------------------------------------------------------------*/

/* Which primitives need CPU expansion, and what they end up drawn as, both
 * come from rsx_primitives.h now -- this file used to answer the first
 * question with its own list, which disagreed with the one in that header
 * about LINE_LOOP and POLYGON. Only the Metal enum mapping stays here. */
static MTLPrimitiveType topo_to_metal(rsx_topology t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return MTLPrimitiveTypePoint;
    case RSX_TOPOLOGY_LINES:          return MTLPrimitiveTypeLine;
    case RSX_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default:                          return MTLPrimitiveTypeTriangle;
    }
}

/* Emit `count` vertices starting at `first`, expanding fans/quads/polygons.
 * `resolve` maps a sequence position to a guest vertex index, so the same code
 * serves both array and indexed draws. Returns vertices written. */
typedef u32 (*IndexResolver)(const rsx_state*, u32 /*seq*/, void* /*ctx*/);

static u32 emit_vertices(const rsx_state* st, u32 prim, u32 count,
                         IndexResolver resolve, void* ctx)
{
    u32 wrote = 0;
    #define PUSH(seq) do {                                                     \
        if (s_vert_count >= MTL_MAX_VERTS) return wrote;                       \
        fetch_vertex(st, resolve(st, (seq), ctx), &s_verts[s_vert_count++]);   \
        wrote++;                                                               \
    } while (0)

    if (prim == RSX_PRIMITIVE_QUADS) {
        for (u32 q = 0; q + 3 < count; q += 4) {
            PUSH(q); PUSH(q+1); PUSH(q+2);
            PUSH(q); PUSH(q+2); PUSH(q+3);
        }
    } else if (prim == RSX_PRIMITIVE_QUAD_STRIP) {
        for (u32 q = 0; q + 3 < count; q += 2) {
            PUSH(q); PUSH(q+1); PUSH(q+2);
            PUSH(q+1); PUSH(q+3); PUSH(q+2);
        }
    } else if (prim == RSX_PRIMITIVE_TRIANGLE_FAN || prim == RSX_PRIMITIVE_POLYGON) {
        for (u32 t = 1; t + 1 < count; t++) { PUSH(0); PUSH(t); PUSH(t+1); }
    } else {
        for (u32 v = 0; v < count; v++) PUSH(v);
    }
    #undef PUSH
    return wrote;
}

static u32 resolve_linear(const rsx_state* st, u32 seq, void* ctx)
{
    (void)st; return *(u32*)ctx + seq;
}

static u32 resolve_indexed(const rsx_state* st, u32 seq, void* ctx)
{
    u32 base = *(u32*)ctx;
    /* index_array_offset bits [7:4] select the type: 0 = u32, 1 = u16. */
    int u16type = ((st->index_array_offset >> 4) & 0xFu) == 1;
    u32 off = st->index_array_offset & ~0xFFu;
    u32 ea  = cellGcmResolveOffset(off) + (base + seq) * (u16type ? 2u : 4u);
    if (!vm_base) return 0;
    const u8* p = vm_base + ea;
    return u16type ? (u32)((p[0] << 8) | p[1])
                   : (u32)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* ---- guest shader translation -------------------------------------------- */

/* PS3RECOMP_METAL_SHADER_DUMP=<dir>: write every translated program's HLSL and
 * MSL there, named by the cache key. The first thing to look at when a title
 * draws wrong. */
static void dump_shader(const char* name, const char* text)
{
    static const char* dir = (const char*)1;
    if (dir == (const char*)1) dir = getenv("PS3RECOMP_METAL_SHADER_DUMP");
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static MTLCompileOptions* guest_compile_options(void)
{
    MTLCompileOptions* o = [MTLCompileOptions new];
    /* IEEE comparisons. The decompilers flush a NaN result to zero with
     * `x == x`, and the alpha test compares with isunordered; Metal's default
     * fast math is free to fold both away. */
#if defined(MAC_OS_VERSION_15_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
    if (@available(macOS 15.0, *)) {
        o.mathMode = MTLMathModeSafe;
        return o;
    }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    return o;
}

/* MSL source -> its `main0` function, or nil with the compiler's diagnostic
 * on stderr. */
static id<MTLFunction> compile_guest_function(const char* msl, const char* what)
{
    NSError* err = nil;
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:msl]
                                             options:guest_compile_options()
                                               error:&err];
    if (!lib) {
        fprintf(stderr, "[RSX metal] %s: MSL compile failed: %s\n", what,
                [[err localizedDescription] UTF8String]);
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    if (!fn) fprintf(stderr, "[RSX metal] %s: no main0 in the compiled MSL\n", what);
    return fn;
}

/* The vertex program the draw would run: a slot in s_vp_cache, or -1 when
 * there is none, it could not be translated, or the cache is full. Follows
 * the D3D12 backend's vp_get_vs: the program starts at the instruction
 * SET_TRANSFORM_PROGRAM_START selects (the store holds every resident
 * program), falling back to 0 when that lies past what was uploaded. */
static int vp_slot_for(const rsx_state* st, u32 vtex_mask)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    /* Hash the program's own bytes, to its end bit, not everything after it
     * in the store: another program uploaded behind it must not miss. */
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    /* The bound vertex-texture units are part of the key: an unbound unit's
     * TXL decompiles to a defined zero and declares no sampler at all, so the
     * same microcode with a different mask is a different program. */
    const u32 hash = fnv1a32(uc, len) ^ (vtex_mask * 0x9E3779B9u);

    for (u32 i = 0; i < s_vp_count; i++)
        if (s_vp_cache[i].hash == hash) return s_vp_cache[i].fn ? (int)i : -1;
    if (s_vp_count >= MTL_VP_CACHE) { s_caches_full = 1; return -1; }

    char name[64];
    snprintf(name, sizeof name, "vertex program %08X", hash);
    id<MTLFunction> fn = nil;
    const int ni = rsx_vp_decompile_ex(uc, len, vtex_mask, s_hlsl, sizeof s_hlsl);
    if (ni <= 0) {
        fprintf(stderr, "[RSX metal] %s: decompile failed (%d)\n", name, ni);
    } else {
        snprintf(name, sizeof name, "vp_%08x.hlsl", hash); dump_shader(name, s_hlsl);
        if (rsx_hlsl_to_msl(s_hlsl, RSX_SHADER_STAGE_VERTEX, s_msl, sizeof s_msl,
                            s_log, sizeof s_log) != 0) {
            fprintf(stderr, "[RSX metal] vertex program %08X: %s\n", hash, s_log);
        } else {
            snprintf(name, sizeof name, "vp_%08x.msl", hash); dump_shader(name, s_msl);
            snprintf(name, sizeof name, "vertex program %08X", hash);
            fn = compile_guest_function(s_msl, name);
        }
    }
    { static int n = 0; if (PS3_LOG_FIRST(n, 32))
        fprintf(stderr, "[RSX metal] vertex program %08X: %d instrs -> %s\n",
                hash, ni, fn ? "ok" : "FAILED (built-in shader instead)"); }

    const int slot = (int)s_vp_count++;
    s_vp_cache[slot].hash = hash;
    s_vp_cache[slot].fn   = fn;
    return fn ? slot : -1;
}

/* Which texture units are cube maps, and which vertex-texture units are
 * bound. Defined with the rest of the texture code below; declared here
 * because the guest programs are compiled against them. */
static u32 cube_mask_for(const rsx_state* st);
static u32 vtex_mask_for(const rsx_state* st);

/* The fragment program the draw would run, resolved as the D3D12 backend's
 * vp_get_fp_pso does: SET_SHADER_PROGRAM's low bits are the location (1 =
 * local, 2 = main), the rest the offset. Keyed on the program's structure
 * (its inline constants are hoisted into PSConstants, so the compiled shader
 * is invariant under constant changes), the export width from
 * SHADER_CONTROL, and the alpha test patched into the source. Returns the
 * slot or -1; `*uc` receives the program bytes for the constant collection.
 * `cube_mask` says which units are cube maps: those are declared TextureCube
 * and sampled with a direction rather than a 2D coordinate, so it changes the
 * compiled program and belongs in the key -- the D3D12 backend's PSO key
 * carries cube_mask for the same reason. */
static u32 fp_shadow_state(const rsx_state* st, const u8* uc,
                           rsx_fp_shadow_desc shadow[RSX_MAX_TEXTURES])
{
    memset(shadow, 0, sizeof(rsx_fp_shadow_desc) * RSX_MAX_TEXTURES);
    u32 shadow_mask = 0;
    const u32 sampled_units = rsx_fp_texture_units(uc, MTL_FP_MAX_BYTES);
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        if (!(sampled_units & (1u << u))) continue;
        const rsx_texture_state* t = &st->textures[u];
        rsx_fp_shadow_desc_from_texture(&shadow[u], (t->format >> 8) & 255u,
            (t->format >> 4) & 15u, (t->format >> 2) & 1u,
            (t->format >> 16) & 0xFFFFu, t->filter, t->address, t->control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    return shadow_mask;
}

static int fp_slot_for(const rsx_state* st, const u8** uc, u32 cube_mask)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;

    u64 key = rsx_fp_structural_hash(*uc, MTL_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                    /* malformed or unterminated */
    rsx_fp_shadow_desc shadow[RSX_MAX_TEXTURES];
    const u32 shadow_mask = fp_shadow_state(st, *uc, shadow);
    const u32 ctrl      = st->shader_control;
    const u32 ctrl_key  = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en  = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn  = alpha_en ? st->alpha_func : 0u;
    key = fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    key = fnv1a64(&cube_mask, sizeof cube_mask, key);

    if (shadow_mask) {
        key = fnv1a64(&shadow_mask, sizeof shadow_mask, key);
        key = fnv1a64(shadow, sizeof shadow, key);
    }
    for (u32 i = 0; i < s_fp_count; i++)
        if (s_fp_cache[i].key == key)
            return s_fp_cache[i].fn ? (int)i : (shadow_mask ? -2 : -1);
    if (s_fp_count >= MTL_FP_CACHE) { s_caches_full = 1; return shadow_mask ? -2 : -1; }

    char name[64];
    id<MTLFunction> fn = nil;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_shadow_ex(*uc, MTL_FP_MAX_BYTES, ctrl, cube_mask,
        shadow_mask ? shadow : NULL, s_hlsl, sizeof s_hlsl, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_hlsl, sizeof s_hlsl, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) {
        fprintf(stderr, "[RSX metal] fragment program %016llX: decompile failed (%d)\n",
                (unsigned long long)key, ni);
    } else {
        snprintf(name, sizeof name, "fp_%016llx.hlsl", (unsigned long long)key);
        dump_shader(name, s_hlsl);
        if (rsx_hlsl_to_msl(s_hlsl, RSX_SHADER_STAGE_FRAGMENT, s_msl, sizeof s_msl,
                            s_log, sizeof s_log) != 0) {
            fprintf(stderr, "[RSX metal] fragment program %016llX: %s\n",
                    (unsigned long long)key, s_log);
        } else {
            snprintf(name, sizeof name, "fp_%016llx.msl", (unsigned long long)key);
            dump_shader(name, s_msl);
            snprintf(name, sizeof name, "fragment program %016llX", (unsigned long long)key);
            fn = compile_guest_function(s_msl, name);
        }
    }
    { static int n = 0; if (PS3_LOG_FIRST(n, 32))
        fprintf(stderr, "[RSX metal] fragment program %016llX: %d instrs, %u constants -> %s\n",
                (unsigned long long)key, ni, nconst,
                fn ? "ok" : (shadow_mask ? "FAILED (depth comparison draw rejected)" :
                                             "FAILED (built-in shader instead)")); }

    const int slot = (int)s_fp_count++;
    s_fp_cache[slot].key    = key;
    s_fp_cache[slot].fn     = fn;
    s_fp_cache[slot].nconst = nconst;
    return fn ? slot : (shadow_mask ? -2 : -1);
}

/* Reserve `bytes` of constant staging at a 256-byte boundary. */
static int cb_alloc(u32 bytes, u32* off)
{
    const u32 start = (s_cb_used + MTL_CB_ALIGN - 1u) & ~(MTL_CB_ALIGN - 1u);
    if (start + bytes > s_cb_cap) {
        u32 cap = s_cb_cap ? s_cb_cap : (4u << 20);
        while (start + bytes > cap) {
            if (cap >= MTL_CB_MAX) return 0;
            cap *= 2u;
        }
        u8* n = (u8*)realloc(s_cb, cap);
        if (!n) return 0;
        s_cb = n; s_cb_cap = cap;
    }
    *off = start;
    s_cb_used = start + bytes;
    return 1;
}

/* Resolve both guest programs for a draw and snapshot their constant blocks.
 * Returns 1 with the record's shader fields filled in, 0 when the draw must
 * use the built-in shader (no vertex program loaded, a translation failure,
 * or a full cache). Both programs or neither: a guest vertex program's
 * outputs do not link with the built-in fragment shader's inputs. */
static int guest_programs_for(const rsx_state* st, MtlDraw* d)
{
    const u8* fp_uc = NULL;
    const int fs = fp_slot_for(st, &fp_uc, cube_mask_for(st));
    if (fs == -2) return -1; /* Active comparison must never use a raw fallback. */
    if (fs < 0) return 0;
    rsx_fp_shadow_desc shadow[RSX_MAX_TEXTURES];
    const int fallback = fp_shadow_state(st, fp_uc, shadow) ? -1 : 0;
    const int vs = vp_slot_for(st, vtex_mask_for(st));
    if (vs < 0) return fallback;

    /* PSConstants: the program's inline constants as host-order bit patterns,
     * one float4 per slot, then fp_alpha. The count must match what the
     * shader was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, MTL_FP_MAX_BYTES, &s_fp_consts) < 0 ||
        s_fp_consts.count != s_fp_cache[fs].nconst)
        return fallback;
    const u32 nslots = s_fp_consts.count ? s_fp_consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;

    u32 vp_off, fp_off;
    if (!cb_alloc(MTL_VP_CB_BYTES, &vp_off)) return fallback;
    if (!cb_alloc(fp_len, &fp_off)) return fallback;

    /* VPConst: the 512 transform constants, then the viewport epilogue
     * (see the D3D12 backend's vp_record_cb): x/y identity, and the z lane
     * remaps GL clip z to [0,1] when the guest programmed a z scale. Metal
     * clip space matches D3D's here, so the mapping transfers unchanged. */
    u8* vp = s_cb + vp_off;
    memcpy(vp, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    float* vpx = (float*)(vp + RSX_MAX_VERTEX_CONSTANTS * 16);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }

    u8* fp = s_cb + fp_off;
    memset(fp, 0, fp_len);
    if (s_fp_consts.count)
        memcpy(fp, s_fp_consts.values, s_fp_consts.count * 16u);
    float* alpha = (float*)(fp + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);
    alpha[1] = 1.0f; alpha[2] = alpha[3] = 0.0f;   /* WPOS scale/bias: top origin */

    d->vs_idx = vs; d->fs_idx = fs;
    d->vp_cb_off = vp_off;
    d->fp_cb_off = fp_off; d->fp_cb_len = fp_len;
    return 1;
}

/* ---- guest textures ------------------------------------------------------ */

static MTLPixelFormat texfmt_to_metal(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8G8:     return MTLPixelFormatRG8Unorm;
    case RSX_TEXFMT_R8G8B8A8: return MTLPixelFormatRGBA8Unorm;
    case RSX_TEXFMT_BC1:      return MTLPixelFormatBC1_RGBA;
    case RSX_TEXFMT_BC2:      return MTLPixelFormatBC2_RGBA;
    case RSX_TEXFMT_BC3:      return MTLPixelFormatBC3_RGBA;
    case RSX_TEXFMT_R16:      return MTLPixelFormatR16Unorm;
    case RSX_TEXFMT_R16G16:   return MTLPixelFormatRG16Unorm;
    case RSX_TEXFMT_R16G16F:  return MTLPixelFormatRG16Float;
    case RSX_TEXFMT_R16G16B16A16F: return MTLPixelFormatRGBA16Float;
    case RSX_TEXFMT_R32F:     return MTLPixelFormatR32Float;
    case RSX_TEXFMT_R32G32B32A32F: return MTLPixelFormatRGBA32Float;
    default:                  return MTLPixelFormatR8Unorm;
    }
}

/* A crossbar selector (0..3 = the uploaded R,G,B,A; RSX_REMAP_ZERO/ONE) as
 * a Metal swizzle source. */
static MTLTextureSwizzle swizzle_sel(u8 sel)
{
    switch (sel) {
    case 0: return MTLTextureSwizzleRed;
    case 1: return MTLTextureSwizzleGreen;
    case 2: return MTLTextureSwizzleBlue;
    case 3: return MTLTextureSwizzleAlpha;
    case RSX_REMAP_ONE: return MTLTextureSwizzleOne;
    default: return MTLTextureSwizzleZero;
    }
}

/* Sparse FNV-1a over a texture's source bytes: enough to notice an animated
 * surface changing, cheap enough to run on every draw that binds it. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    const u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (u32 i = 0; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* A unit is a cube map when SET_TEXTURE_FORMAT bit 2 says so. A cube face is
 * square by construction, so a non-square image cannot be one. The check
 * lives in one place because two things read it and they must not disagree:
 * the fragment program is compiled against a cube unit's sampler, and a
 * `texturecube` slot filled with a 2D texture is a validation failure, not a
 * wrong pixel. */
static int texture_is_cube(const rsx_texture_state* t)
{
    const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
    return (t->format & 4u) && w && w == h;
}

/* The enabled cube units as a bit mask, which is what the fragment program is
 * decompiled against. Built from the registers rather than from the resolved
 * textures, as the D3D12 backend's dr_cube_mask is, so a unit whose bytes
 * could not be uploaded still compiles to the sampler type the register
 * asked for. */
static u32 cube_mask_for(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        if ((st->textures[u].control0 & 0x80000000u) && texture_is_cube(&st->textures[u]))
            m |= 1u << u;
    return m;
}

/* The enabled vertex-texture units. The vertex program is decompiled against
 * this: an unmasked unit's TXL becomes a defined zero and declares nothing,
 * so a program compiled with the wrong mask samples the wrong thing or reads
 * a slot the backend never bound. */
static u32 vtex_mask_for(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u)
            m |= 1u << u;
    return m;
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the checksum has to cover -- a title that animates
 * only a lower level, or only one face, would otherwise keep the stale
 * upload. */
static u32 texture_source_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch,
                               int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* Room for one decoded level. Levels shrink as the chain descends, so the
 * buffer is sized once by level 0 and reused. */
static int tex_staging_reserve(u32 bytes)
{
    if (s_tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(s_tex_staging, bytes);
    if (!n) return 0;
    s_tex_staging = n; s_tex_staging_cap = bytes;
    return 1;
}

/* Decode a guest texture out of guest memory into a new texture whose swizzle
 * applies the TEXTURE_CONTROL1 crossbar. `fmt` is the format byte with its
 * LN/UN flags, `levels` SET_TEXTURE_FORMAT's level count, `pitch`
 * SET_TEXTURE_CONTROL3's row pitch, and `cube` its bit 2. */
static id<MTLTexture> upload_texture(const u8* src, u32 w, u32 h, u32 fmt,
                                     u32 control1, u32 levels, u32 pitch,
                                     int cube)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || lv[0].tl.face_bytes == 0) return nil;
    /* Faces are stored face-major, each one a whole mip pyramid rather than a
     * single image, so the stride between them is the pyramid rounded up to
     * 128 bytes. */
    const u32 nfaces = cube ? 6u : 1u;
    const u32 face_stride = cube ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;

    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType       = cube ? MTLTextureTypeCube : MTLTextureType2D;
    td.pixelFormat       = texfmt_to_metal(lv[0].tl.fmt);
    td.width             = w;
    td.height            = h;
    td.mipmapLevelCount  = nlv;
    td.usage             = MTLTextureUsageShaderRead;
    /* The crossbar's selectors arrive in its own field order A,R,G,B; the
     * D3D12 backend maps them to destR = out[1], destG = out[2],
     * destB = out[3], destA = out[0], and so does this. */
    u8 remap[4];
    rsx_texture_component_remap(control1, fmt & 0x9Fu, remap);
    td.swizzle = MTLTextureSwizzleChannelsMake(swizzle_sel(remap[1]), swizzle_sel(remap[2]),
                                               swizzle_sel(remap[3]), swizzle_sel(remap[0]));
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    if (!tex) return nil;

    for (u32 f = 0; f < nfaces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!tex_staging_reserve(tl->dst_row_bytes * tl->rows)) return nil;
            /* TEX_RGBA is consulted unconditionally, as the D3D12 backend
             * does: the decode only reads it for the two formats whose bytes
             * are A,R,G,B. */
            rsx_texture_decode(s_tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            /* bytesPerImage is for 3D textures only; a cube face is a slice. */
            [tex replaceRegion:MTLRegionMake2D(0, 0, lv[m].w, lv[m].h)
                   mipmapLevel:m
                         slice:f
                     withBytes:s_tex_staging
                   bytesPerRow:tl->dst_row_bytes
                 bytesPerImage:0];
        }
    }
    return tex;
}

/* The texture a unit samples: a slot in s_tex_cache, or -1 for the zero
 * texture (unit disabled, unresolvable, or a full cache). Resolved as the
 * D3D12 backend's bind_texture does it: the location is SET_TEXTURE_FORMAT's
 * low two bits (1 = local, 2 = main), the format byte its bits [15:8]. */
static int tex_slot_for(const rsx_texture_state* t)
{
    if (!(t->control0 & 0x80000000u) || !vm_base) return -1;
    const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) return -1;
    const u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (ea == 0xFFFFFFFFu) return -1;
    const u32 fmt = (t->format >> 8) & 0xFFu;
    /* SET_TEXTURE_FORMAT's level count. gcm packs it into bits [19:16], but
     * the D3D12 backend and the method dispatcher both read the whole field
     * above bit 15; rsx_texture_mip_chain clamps whatever comes out to the
     * levels the dimensions allow, so the two readings agree. */
    const u32 levels = (t->format >> 16) & 0xFFFFu;
    const u32 pitch  = t->control3 & 0xFFFFFu;
    const int cube   = texture_is_cube(t);
    const u32 span   = texture_source_span(fmt, w, h, levels, pitch, cube);
    if (!span) return -1;
    const u32 csum = tex_csum(vm_base + ea, span);

    for (u32 i = 0; i < s_tex_count; i++) {
        MtlTexEntry* e = &s_tex_cache[i];
        /* `format` carries the level count and the cube bit as well as the
         * format byte, so the key covers both; the pitch has its own
         * register and has to be compared on its own. */
        if (e->ea != ea || e->w != w || e->h != h || e->format != t->format ||
            e->control1 != t->control1 || e->control3 != t->control3)
            continue;
        if (e->csum != csum) {
            id<MTLTexture> fresh = upload_texture(vm_base + ea, w, h, fmt,
                                                  t->control1, levels, pitch, cube);
            if (!fresh) return -1;
            e->tex = fresh; e->csum = csum;
        }
        return e->tex ? (int)i : -1;
    }
    if (s_tex_count >= MTL_TEX_CACHE) { s_caches_full = 1; return -1; }

    id<MTLTexture> tex = upload_texture(vm_base + ea, w, h, fmt, t->control1,
                                        levels, pitch, cube);
    { static int n = 0; if (PS3_LOG_FIRST(n, 16))
        fprintf(stderr, "[RSX metal] texture %ux%u fmt 0x%02X %u level(s)%s at 0x%08X (%s) -> %s\n",
                w, h, fmt, levels ? levels : 1u, cube ? " cube" : "", ea,
                (t->format & 3u) == 1u ? "local" : "main",
                tex ? "ok" : "FAILED"); }
    const int slot = (int)s_tex_count++;
    MtlTexEntry* e = &s_tex_cache[slot];
    e->ea = ea; e->w = w; e->h = h; e->format = t->format; e->control1 = t->control1;
    e->control3 = t->control3; e->csum = csum; e->tex = tex;
    return tex ? slot : -1;
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field (one per axis): 1 = WRAP, 2 =
 * MIRROR, 3 = CLAMP_TO_EDGE, 4 = BORDER, 5 = CLAMP, 6..8 = the MIRROR_ONCE
 * family. The same table the live draw engine's gcm_wrap uses. */
static MTLSamplerAddressMode gcm_wrap_to_metal(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return MTLSamplerAddressModeRepeat;
    case 2:  return MTLSamplerAddressModeMirrorRepeat;
    case 4:  return MTLSamplerAddressModeClampToBorderColor;
    case 6: case 7: case 8:
             return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeClampToEdge;   /* 3, 5, unset */
    }
}

/* Sampler from the unit's wrap, filter and LOD registers, as the live draw
 * engine's decode_sampler reads them: SET_TEXTURE_FILTER min at [18:16]
 * (1 NEAREST, 2 LINEAR, then 3..6, which are the four combinations of a
 * nearest/linear minification with a nearest/linear mip filter), mag at
 * [26:24], and SET_TEXTURE_CONTROL0's max LOD at [18:7] and min LOD at
 * [30:19], both 4.8 fixed point.
 *
 * RSX's LOD bias is SET_TEXTURE_FILTER [12:0] and is not applied: Metal has
 * no sampler-side bias -- it is an argument to the sampling call, which means
 * patching the fragment program rather than the sampler -- and the live draw
 * engine leaves MipLODBias at zero too. */
static int samp_slot_for(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;   /* max then min LOD */
    const u64 key = (u64)minf | ((u64)magf << 3)
                  | ((u64)(t->address & 0x000F0F0Fu) << 6)
                  | ((u64)lod << 26);
    for (u32 i = 0; i < s_samp_count; i++)
        if (s_samp_cache[i].key == key) return (int)i;
    if (s_samp_count >= MTL_SAMP_CACHE) return -1;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = (minf == 2 || minf == 4 || minf == 6) ? MTLSamplerMinMagFilterLinear
                                                         : MTLSamplerMinMagFilterNearest;
    sd.magFilter = (magf == 2) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = !mip_present ? MTLSamplerMipFilterNotMipmapped
                 : mip_linear   ? MTLSamplerMipFilterLinear
                                : MTLSamplerMipFilterNearest;
    /* A min filter with no mip term samples level 0 only, which is what
     * pinning the LOD range to the minimum says. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    sd.lodMinClamp = min_lod;
    sd.lodMaxClamp = max_lod;
    sd.sAddressMode = gcm_wrap_to_metal(t->address);
    sd.tAddressMode = gcm_wrap_to_metal(t->address >> 8);
    sd.rAddressMode = gcm_wrap_to_metal(t->address >> 16);
    sd.borderColor  = MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> samp = [s_dev newSamplerStateWithDescriptor:sd];
    if (!samp) return -1;
    const int slot = (int)s_samp_count++;
    s_samp_cache[slot].key  = key;
    s_samp_cache[slot].samp = samp;
    return slot;
}

/* ---- guest surfaces ---------------------------------------------------------
 * A title does not render into the scanout. It renders into its own colour
 * surfaces, samples them, and composites the result; SET_SURFACE_COLOR_TARGET
 * says which of A, B or an MRT set is bound, and the offsets in
 * SET_SURFACE_COLOR_[ABCD]OFFSET say where they are.
 *
 * A surface is keyed by its RAW RSX offset, as the D3D12 backend's
 * current_rt_off keys them: surface and texture registers share the offset
 * space, so a texture bound at a surface's offset is that same buffer, and
 * matching on the raw value sidesteps guessing which context DMA the surface
 * lives in. An offset cellGcmSetDisplayBuffer registered is the display and
 * still draws into the drawable.
 *
 * Surfaces outlive the frame that created them -- a surface rendered in one
 * frame is sampled in the next -- so they are not part of clear_caches. A size
 * or format change gets a fresh texture; replacing the reference is safe
 * because a command buffer retains what it uses.
 * --------------------------------------------------------------------------*/

#define MTL_MAX_SURFACES  32
#define MTL_MAX_ZETA      16
#define MTL_RTVIEW_CACHE  64

typedef struct {
    u32 off;               /* raw RSX colour offset: the key      */
    u32 w, h, fmt;
    u32 gen;               /* bumped whenever the texture is replaced */
    id<MTLTexture> tex;
} MtlSurface;
typedef struct { u32 off, w, h; id<MTLTexture> tex; } MtlZeta;
typedef struct { int surf; u32 gen, control1, format; id<MTLTexture> view; } MtlRtView;

static MtlSurface s_surf[MTL_MAX_SURFACES];
static MtlZeta    s_zeta[MTL_MAX_ZETA];
static MtlRtView  s_rtview[MTL_RTVIEW_CACHE];
static u32 s_surf_count, s_zeta_count, s_rtview_count;

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> Metal, as the
 * D3D12 backend's rsx_surface_dxgi maps it. The float targets are the reason
 * this is not one format: a title's height fields and differential planes hold
 * signed values that an 8-bit target clamps to zero. Everything else is the
 * drawable's BGRA8Unorm, so the clear colour and the headless readback keep
 * the conventions they already have. */
static MTLPixelFormat surface_format_to_metal(u32 fmt)
{
    switch (fmt & 0x1Fu) {
    case 0x0B: return MTLPixelFormatRGBA16Float;   /* F_W16Z16Y16X16 */
    case 0x0C: return MTLPixelFormatRGBA32Float;   /* F_W32Z32Y32X32 */
    case 0x0D: return MTLPixelFormatR32Float;      /* F_X32          */
    default:   return MTLPixelFormatBGRA8Unorm;
    }
}

/* Which colour surface the current state renders to, as current_rt_off decides
 * it: 0 for a display buffer, else the surface's raw offset, with the MRT set
 * and the surface size alongside. */
static u32 current_rt_off(const rsx_state* st, u32* out_w, u32* out_h, u32 out_mrt[3])
{
    *out_w = 0; *out_h = 0;
    out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B), 0x17 = MRT2
     * (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    const int sel = (st->color_target == CELL_GCM_SURFACE_TARGET_1) ? 1 : 0;
    const u32 raw = st->surface_color_offset[sel];
    if (st->color_target >= CELL_GCM_SURFACE_TARGET_MRT1) out_mrt[0] = st->surface_color_offset[1];
    if (st->color_target >= CELL_GCM_SURFACE_TARGET_MRT2) out_mrt[1] = st->surface_color_offset[2];
    if (st->color_target >= CELL_GCM_SURFACE_TARGET_MRT3) out_mrt[2] = st->surface_color_offset[3];
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* The clip dims are the surface's size when they are sane, else the window.
     * Any size works, since passes draw normalised full-surface geometry; this
     * only picks the resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    *out_w = w; *out_h = h;
    return raw;
}

/* The registry slot holding a live texture for this raw offset, or -1. */
static int surface_find(u32 off)
{
    if (!off) return -1;
    for (u32 i = 0; i < s_surf_count; i++)
        if (s_surf[i].tex && s_surf[i].off == off) return (int)i;
    return -1;
}

/* Make sure a surface exists for this offset at this size and format, and
 * return its slot. */
static int surface_get(u32 off, u32 w, u32 h, u32 fmt)
{
    if (!off || !s_dev) return -1;
    if (!w) w = s_width;
    if (!h) h = s_height;
    const MTLPixelFormat pf = surface_format_to_metal(fmt);

    int slot = -1;
    for (u32 i = 0; i < s_surf_count; i++)
        if (s_surf[i].off == off) { slot = (int)i; break; }
    if (slot >= 0) {
        MtlSurface* s = &s_surf[slot];
        if (s->tex && s->w == w && s->h == h &&
            surface_format_to_metal(s->fmt) == pf)
            return slot;
    } else {
        if (s_surf_count >= MTL_MAX_SURFACES) return -1;
        slot = (int)s_surf_count++;
    }

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                           width:w height:h mipmapped:NO];
    /* PixelFormatView because a draw that samples the surface wears the unit's
     * TEXTURE_CONTROL1 crossbar as a view swizzle. */
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
               MTLTextureUsagePixelFormatView;
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    { static int n = 0; if (PS3_LOG_FIRST(n, 16))
        fprintf(stderr, "[RSX metal] surface 0x%08X %ux%u fmt 0x%02X -> %s\n",
                off, w, h, fmt & 0x1Fu, tex ? "ok" : "FAILED"); }

    MtlSurface* s = &s_surf[slot];
    s->off = off; s->w = w; s->h = h; s->fmt = fmt;
    s->gen++;               /* invalidates the views cut from the old texture */
    s->tex = tex;
    return tex ? slot : -1;
}

/* The depth/stencil texture an offscreen surface renders against: one per
 * zeta offset and size, since a title points several surfaces of one size at
 * the same zeta buffer. Only the attachment is set up here; the compare and
 * write state belong to the depth/stencil path. */
static id<MTLTexture> zeta_get(u32 off, u32 w, u32 h)
{
    if (!s_dev || !w || !h) return nil;
    for (u32 i = 0; i < s_zeta_count; i++)
        if (s_zeta[i].off == off && s_zeta[i].w == w && s_zeta[i].h == h)
            return s_zeta[i].tex;
    if (s_zeta_count >= MTL_MAX_ZETA) return nil;

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                           width:w height:h mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    if (!tex) return nil;
    const u32 slot = s_zeta_count++;
    s_zeta[slot].off = off; s_zeta[slot].w = w; s_zeta[slot].h = h;
    s_zeta[slot].tex = tex;
    return tex;
}

/* A view of a surface carrying the sampling unit's TEXTURE_CONTROL1 crossbar
 * as its swizzle, so a render-to-texture bind reads the channels the guest
 * asked for. Cached per (surface, generation, crossbar, format) and, like the
 * shader and texture caches, slot-stable for the whole frame, because a draw
 * record holds the slot index. A recreated surface bumps its generation, so
 * the views cut from the old texture simply stop matching. */
static int rtview_for(int surf, u32 control1, u32 format)
{
    MtlSurface* s = &s_surf[surf];
    if (!s->tex) return -1;
    for (u32 i = 0; i < s_rtview_count; i++) {
        const MtlRtView* v = &s_rtview[i];
        if (v->surf == surf && v->gen == s->gen &&
            v->control1 == control1 && v->format == format)
            return v->view ? (int)i : -1;
    }
    if (s_rtview_count >= MTL_RTVIEW_CACHE) { s_caches_full = 1; return -1; }

    /* Same field order as an uploaded texture's swizzle: the crossbar's own
     * A,R,G,B order becomes destR = out[1] .. destA = out[0]. Its selectors
     * index the sampled vector as R,G,B,A, which is what a surface reads back
     * as whatever byte order it stores. */
    u8 remap[4];
    rsx_texture_component_remap(control1, (format >> 8) & 0x9Fu, remap);
    id<MTLTexture> view =
        [s->tex newTextureViewWithPixelFormat:[s->tex pixelFormat]
                                  textureType:MTLTextureType2D
                                       levels:NSMakeRange(0, 1)
                                       slices:NSMakeRange(0, 1)
                                      swizzle:MTLTextureSwizzleChannelsMake(
                                                  swizzle_sel(remap[1]), swizzle_sel(remap[2]),
                                                  swizzle_sel(remap[3]), swizzle_sel(remap[0]))];
    const int slot = (int)s_rtview_count++;
    s_rtview[slot].surf     = surf;
    s_rtview[slot].gen      = s->gen;
    s_rtview[slot].control1 = control1;
    s_rtview[slot].format   = format;
    s_rtview[slot].view     = view;
    return view ? slot : -1;
}

/* Is this offset one of the colour targets the record itself renders into?
 * Reading a surface a pass is writing is undefined on any API. */
static int rt_is_own_target(const MtlDraw* d, u32 off)
{
    if (!off) return 0;
    if (off == d->rt.off) return 1;
    for (int m = 0; m < 3; m++) if (off == d->rt.mrt[m]) return 1;
    return 0;
}

/* Fill in the colour target, MRT set, zeta and viewport a record was made
 * against, and make sure the surfaces it names exist. Registering here rather
 * than at present time is what lets a draw later in the same frame find the
 * surface an earlier draw rendered into (the D3D12 backend gets the same
 * effect from its render-to-texture pre-pass). */
static void target_for(const rsx_state* st, MtlTarget* t)
{
    memset(t, 0, sizeof *t);
    if (!st) return;
    t->off      = current_rt_off(st, &t->w, &t->h, t->mrt);
    t->fmt      = st->surface_format;
    t->zeta_off = st->surface_zeta_offset;
    t->vp_x = st->viewport_x; t->vp_y = st->viewport_y;
    t->vp_w = st->viewport_w; t->vp_h = st->viewport_h;
    if (!t->off) return;
    surface_get(t->off, t->w, t->h, t->fmt);
    for (int m = 0; m < 3; m++)
        if (t->mrt[m]) surface_get(t->mrt[m], t->w, t->h, t->fmt);
}

/* Resolve every enabled texture unit for a guest-program draw. */
static void textures_for(const rsx_state* st, MtlDraw* d)
{
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        if (!(t->control0 & 0x80000000u)) continue;
        /* Render-to-texture, the D3D12 backend's tex_rt rule: a unit whose raw
         * offset is a registered surface samples that surface. Uploading from
         * guest memory instead would read a buffer the pass that produced the
         * image never wrote, which is how a reflection or a shadow map comes
         * out black. */
        const int surf = surface_find(t->offset);
        if (surf >= 0) {
            d->samp[u] = samp_slot_for(t);
            if (rt_is_own_target(d, t->offset)) {
                { static int n = 0; if (PS3_LOG_FIRST(n, 1))
                    fprintf(stderr, "[RSX metal] unit %u samples the surface the pass "
                                    "draws into (0x%08X); reading the zero texture\n",
                            u, t->offset); }
                continue;                 /* leaves the unit on the zero texture */
            }
            d->tex_rt[u] = rtview_for(surf, t->control1, t->format);
            continue;
        }
        d->tex[u]  = tex_slot_for(t);
        d->samp[u] = samp_slot_for(t);
    }
    /* Vertex textures go through the same resolve, upload and caches: a
     * vertex unit's registers are decoded into the same rsx_texture_state,
     * with no crossbar, so nothing below here knows the difference. */
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->vertex_textures[u];
        if (!(t->control0 & 0x80000000u)) continue;
        d->vtex[u]  = tex_slot_for(t);
        d->vsamp[u] = samp_slot_for(t);
    }
}

/* Guest face culling, packed as the D3D12 backend's rsx_cull_key reads it:
 * CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408 (FRONT here, as
 * there); FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face and
 * front_face with plain 1/0 before any register arrives, which reads as
 * BACK/CW. Metal's default winding is clockwise, so the mapping is direct. */
static void cull_from_state(const rsx_state* st, MtlDraw* d)
{
    d->cull = MTLCullModeNone;
    if (st->cull_face_enable)
        d->cull = (st->cull_face == 0x0404u || st->cull_face == 0x0408u)
                      ? MTLCullModeFront : MTLCullModeBack;
    d->winding = (st->front_face == 0x0901u) ? MTLWindingCounterClockwise
                                             : MTLWindingClockwise;
}

/* ---- depth/stencil state -------------------------------------------------
 * The NV4097 comparison functions are the GL enums, 0x200 NEVER .. 0x207
 * ALWAYS, and the stencil ops are GL's too. Both tables are copied from the
 * live draw engine's gcm_cmp / gcm_stencil_op, which is what a title has
 * actually run through.
 *
 * Note that rsx_state_init seeds depth_func with 1 and calls it LESS, which is
 * not the GL enum for anything; like the reference, an unrecognised function
 * decodes as ALWAYS. Nothing reaches the depth test with that value unless the
 * guest enabled the test without programming SET_DEPTH_FUNC.
 * --------------------------------------------------------------------------*/

static MTLCompareFunction gcm_compare_to_metal(u32 f)
{
    switch (f) {
    case 0x0200: return MTLCompareFunctionNever;
    case 0x0201: return MTLCompareFunctionLess;
    case 0x0202: return MTLCompareFunctionEqual;
    case 0x0203: return MTLCompareFunctionLessEqual;
    case 0x0204: return MTLCompareFunctionGreater;
    case 0x0205: return MTLCompareFunctionNotEqual;
    case 0x0206: return MTLCompareFunctionGreaterEqual;
    default:     return MTLCompareFunctionAlways;
    }
}

/* GL_ZERO is 0, which is also what the register reads before a title ever
 * writes it -- and GL_KEEP is the value that leaves such a stream alone, so
 * zero maps to ZERO only because the guest asked for it explicitly; an
 * unrecognised op keeps, as in the reference. */
static MTLStencilOperation gcm_stencil_op_to_metal(u32 op)
{
    switch (op) {
    case 0x0000: return MTLStencilOperationZero;              /* GL_ZERO       */
    case 0x1E01: return MTLStencilOperationReplace;
    case 0x1E02: return MTLStencilOperationIncrementClamp;    /* GL_INCR       */
    case 0x1E03: return MTLStencilOperationDecrementClamp;    /* GL_DECR       */
    case 0x150A: return MTLStencilOperationInvert;
    case 0x8507: return MTLStencilOperationIncrementWrap;
    case 0x8508: return MTLStencilOperationDecrementWrap;
    default:     return MTLStencilOperationKeep;              /* GL_KEEP 0x1E00 */
    }
}

/* MTLDepthStencilState objects are immutable, so one is built per distinct
 * combination of the registers and kept for the frame, exactly like the
 * pipeline states. The stencil reference value is not part of the object --
 * Metal sets it on the encoder -- so it is not part of the key either. */
#define MTL_DS_CACHE 256

typedef struct {
    u8  depth_test, depth_mask, stencil_test;
    u32 depth_func;
    u32 stencil_func, stencil_mask;
    u32 op_fail, op_zfail, op_zpass;
} MtlDsKey;
typedef struct { MtlDsKey key; id<MTLDepthStencilState> state; } MtlDsEntry;

static MtlDsEntry s_ds_cache[MTL_DS_CACHE];
static u32        s_ds_count;

static int ds_slot_for(const rsx_state* st)
{
    MtlDsKey key;
    memset(&key, 0, sizeof key);
    key.depth_test   = (u8)(st->depth_test_enable ? 1 : 0);
    key.depth_mask   = (u8)(st->depth_mask ? 1 : 0);
    key.stencil_test = (u8)(st->stencil_test_enable ? 1 : 0);
    key.depth_func   = st->depth_func;
    key.stencil_func = st->stencil_func;
    key.stencil_mask = st->stencil_mask;
    key.op_fail      = st->stencil_op_fail;
    key.op_zfail     = st->stencil_op_zfail;
    key.op_zpass     = st->stencil_op_zpass;

    for (u32 i = 0; i < s_ds_count; i++)
        if (memcmp(&s_ds_cache[i].key, &key, sizeof key) == 0) return (int)i;
    if (s_ds_count >= MTL_DS_CACHE) { s_caches_full = 1; return -1; }

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    /* The attachment is always bound, so "depth test off" is compare Always
     * with writes off -- which is also what the hardware does: with the test
     * disabled the RSX writes no depth, whatever SET_DEPTH_MASK says. */
    dd.depthCompareFunction = key.depth_test ? gcm_compare_to_metal(key.depth_func)
                                             : MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = (key.depth_test && key.depth_mask) ? YES : NO;
    if (key.stencil_test) {
        MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction    = gcm_compare_to_metal(key.stencil_func);
        sd.stencilFailureOperation   = gcm_stencil_op_to_metal(key.op_fail);
        sd.depthFailureOperation     = gcm_stencil_op_to_metal(key.op_zfail);
        sd.depthStencilPassOperation = gcm_stencil_op_to_metal(key.op_zpass);
        sd.readMask                  = key.stencil_mask & 0xFFu;
        /* SET_STENCIL_FUNC_MASK is the read mask. The write mask lives in
         * NV4097_SET_STENCIL_MASK (0x032C), which rsx_commands.c does not
         * decode and rsx_state has no field for, so every plane is writable
         * here. It reads back wrong for a title that masks stencil planes;
         * fixing it means adding the register upstream, not guessing here. */
        sd.writeMask                 = 0xFFu;
        /* Two-sided stencil (SET_BACK_STENCIL_*) is not decoded upstream
         * either; nv40 applies the front state to both faces when it is off,
         * which is what this does. */
        dd.frontFaceStencil = sd;
        dd.backFaceStencil  = sd;
    } else {
        dd.frontFaceStencil = nil;
        dd.backFaceStencil  = nil;
    }

    id<MTLDepthStencilState> state = [s_dev newDepthStencilStateWithDescriptor:dd];
    if (!state) return -1;
    const int slot = (int)s_ds_count++;
    s_ds_cache[slot].key   = key;
    s_ds_cache[slot].state = state;
    return slot;
}

/* ---- draw recording ------------------------------------------------------ */

static void record_draw(const rsx_state* st, u32 prim, u32 base, u32 count,
                        IndexResolver resolve)
{
    if (!s_ready || !s_verts || !st || count == 0) return;
    if (s_rec_count >= MTL_MAX_RECORDS) { s_dropped_records++; return; }

    MtlRecord* r = &s_records[s_rec_count];
    r->kind = MTL_REC_DRAW;
    MtlDraw* d = &r->u.draw;
    memset(d, 0, sizeof *d);
    d->vs_idx = d->fs_idx = d->ds_idx = -1;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        d->tex[u] = d->samp[u] = d->tex_rt[u] = -1;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) d->vtex[u] = d->vsamp[u] = -1;

    /* The target before anything else: the surfaces it names have to be
     * registered before the textures are resolved against them. */
    target_for(st, &d->rt);

    /* Programs first: their translation is what can fail, and a draw that
     * drops to the built-in shader must still fetch its vertices the same
     * way. Constant staging reserved here for a draw that then fails on
     * vertices is simply left unused until the frame resets it. */
    const int guest = s_guest_shaders ? guest_programs_for(st, d) : 0;
    if (guest < 0) { s_dropped_records++; return; }
    if (guest) textures_for(st, d);

    u32 first_vert = s_vert_count;
    u32 wrote = emit_vertices(st, prim, count, resolve, &base);
    if (wrote == 0) { s_dropped_records++; return; }
    s_rec_count++;
    if (guest) s_guest_draws++;

    d->base  = first_vert;
    d->count = wrote;
    /* Expanded primitives always come out as a triangle list. */
    d->topology = rsx_primitive_needs_expansion(prim) ? MTLPrimitiveTypeTriangle
                                             : topo_to_metal(rsx_primitive_topology(prim));
    d->blend_enable   = st->blend_enable;
    d->blend_sfactor  = st->blend_sfactor;
    d->blend_dfactor  = st->blend_dfactor;
    d->blend_equation = st->blend_equation;
    d->color_mask     = st->color_mask;
    d->ds_idx         = ds_slot_for(st);
    d->stencil_ref    = st->stencil_ref;
    cull_from_state(st, d);

    /* Built-in path: RSX vertex constant slots 0..3 hold the MVP rows when no
     * vertex program has been translated. An all-zero matrix would collapse
     * every vertex to the origin, so fall back to identity. */
    int nonzero = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = st->vertex_constants[r][c];
            d->mvp[r * 4 + c] = v;
            if (v != 0.0f) nonzero = 1;
        }
    if (!nonzero) {
        memset(d->mvp, 0, sizeof(d->mvp));
        d->mvp[0] = d->mvp[5] = d->mvp[10] = d->mvp[15] = 1.0f;
    }
}

static void mtl_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    record_draw(s_state, primitive, first, count, resolve_linear);
}

static void mtl_draw_indexed(void* ud, u32 primitive, u32 index_offset, u32 count)
{
    (void)ud;
    record_draw(s_state, primitive, index_offset, count, resolve_indexed);
}

/* ---- pipeline state ------------------------------------------------------ */

static MTLBlendFactor blend_factor_to_metal(u32 f)
{
    switch (f) {
    case 0x0000: return MTLBlendFactorZero;
    case 0x0001: return MTLBlendFactorOne;
    case 0x0300: return MTLBlendFactorSourceColor;
    case 0x0301: return MTLBlendFactorOneMinusSourceColor;
    case 0x0302: return MTLBlendFactorSourceAlpha;
    case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x0304: return MTLBlendFactorDestinationAlpha;
    case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x0306: return MTLBlendFactorDestinationColor;
    case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x0308: return MTLBlendFactorSourceAlphaSaturated;
    default:     return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_equation_to_metal(u32 e)
{
    switch (e) {
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    case 0x800A: return MTLBlendOperationSubtract;
    case 0x800B: return MTLBlendOperationReverseSubtract;
    default:     return MTLBlendOperationAdd;
    }
}

/* NV4097_SET_COLOR_MASK: bit 0 = B, 8 = G, 16 = R, 24 = A. */
static MTLColorWriteMask color_mask_to_metal(u32 m)
{
    MTLColorWriteMask w = MTLColorWriteMaskNone;
    if (m & 0x00010000u) w |= MTLColorWriteMaskRed;
    if (m & 0x00000100u) w |= MTLColorWriteMaskGreen;
    if (m & 0x00000001u) w |= MTLColorWriteMaskBlue;
    if (m & 0x01000000u) w |= MTLColorWriteMaskAlpha;
    return w;
}

/* Built-in shaders: transform by the RSX vertex-constant matrix and interpolate
 * the diffuse colour. The rows are dotted explicitly rather than using float4x4
 * so there is no column-major/row-major ambiguity with the guest's layout.
 * Reads position, diffuse colour and texcoord0 from RSX attribute slots 0, 3
 * and 8 of the shared vertex layout. The guest-shader path replaces this, it
 * does not extend it. */
static NSString* const kBuiltinMSL = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn  { float4 pos [[attribute(0)]]; float4 col [[attribute(3)]]; float4 tc [[attribute(8)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 col; float4 tc; };\n"
"struct VU   { float4 mvp[4]; };\n"
"vertex VOut vs_main(VIn v [[stage_in]], constant VU& u [[buffer(0)]]) {\n"
"    VOut o;\n"
"    o.pos = float4(dot(u.mvp[0], v.pos), dot(u.mvp[1], v.pos),\n"
"                   dot(u.mvp[2], v.pos), dot(u.mvp[3], v.pos));\n"
"    o.col = v.col; o.tc = v.tc;\n"
"    return o;\n"
"}\n"
"fragment float4 fs_main(VOut in [[stage_in]]) { return in.col; }\n";

static u32 blend_key(const MtlDraw* d)
{
    if (!d->blend_enable) return 0u;
    return 1u | ((d->blend_sfactor & 0xFFFu) << 1)
              | ((d->blend_dfactor & 0xFFFu) << 13)
              | ((d->blend_equation & 0x7u) << 25);
}

/* All 16 RSX attributes as float4 at i*16, stride 256, from MTL_VB_INDEX.
 * A guest program declares only the attributes it reads; Metal ignores the
 * rest of the descriptor. */
static MTLVertexDescriptor* vertex_descriptor(void)
{
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < MTL_ATTRIBS; i++) {
        vd.attributes[i].format      = MTLVertexFormatFloat4;
        vd.attributes[i].offset      = (NSUInteger)(i * 16);
        vd.attributes[i].bufferIndex = MTL_VB_INDEX;
    }
    vd.layouts[MTL_VB_INDEX].stride = sizeof(MtlVertex);
    return vd;
}

static id<MTLRenderPipelineState> pso_for(const MtlDraw* d, MTLPixelFormat color_pf,
                                          u32 nrt)
{
    MtlPsoKey key;
    memset(&key, 0, sizeof key);
    key.vs = d->vs_idx; key.fs = d->fs_idx;
    key.blend = blend_key(d); key.cmask = d->color_mask;
    key.color_pf = (u32)color_pf; key.nrt = nrt;
    for (u32 i = 0; i < s_pso_count; i++)
        if (memcmp(&s_pso_cache[i].key, &key, sizeof key) == 0) return s_pso_cache[i].pso;
    if (s_pso_count >= MTL_PSO_CACHE) { s_caches_full = 1; return nil; }

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    if (d->vs_idx >= 0) {
        pd.vertexFunction   = s_vp_cache[d->vs_idx].fn;
        pd.fragmentFunction = s_fp_cache[d->fs_idx].fn;
    } else {
        pd.vertexFunction   = [s_shader_lib newFunctionWithName:@"vs_main"];
        pd.fragmentFunction = [s_shader_lib newFunctionWithName:@"fs_main"];
    }
    pd.vertexDescriptor = vertex_descriptor();
    /* A depth/stencil attachment is bound on every pass -- the shared one for
     * the display, the target's own zeta for an offscreen surface, both
     * MTL_DEPTH_FORMAT -- so every pipeline declares it; whether a draw tests
     * or writes is the MTLDepthStencilState's business, not the pipeline's. */
    pd.depthAttachmentPixelFormat   = MTL_DEPTH_FORMAT;
    pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    /* Every bound colour target takes target A's blend and colour mask, which
     * is what the D3D12 backend does with RTVFormats[1..]: a zero-initialised
     * secondary attachment writes nothing at all, and a deferred pass would
     * come back with only its first G-buffer plane filled in. The fragment
     * decompiler feeds them now -- a program writing r2/r3/r4 (or h4/h6/h8)
     * returns that many SV_TARGETs. The two counts need not agree: Metal
     * accepts a function with fewer outputs than the pipeline has attachments
     * AND one with more, discarding the surplus, which is how the guest's own
     * hardware behaves when a program exports past the bound set. */
    for (u32 r = 0; r < nrt; r++) {
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[r];
        ca.pixelFormat = color_pf;
        ca.writeMask   = color_mask_to_metal(d->color_mask);
        if (d->blend_enable) {
            ca.blendingEnabled             = YES;
            ca.sourceRGBBlendFactor        = blend_factor_to_metal(d->blend_sfactor);
            ca.destinationRGBBlendFactor   = blend_factor_to_metal(d->blend_dfactor);
            ca.sourceAlphaBlendFactor      = blend_factor_to_metal(d->blend_sfactor);
            ca.destinationAlphaBlendFactor = blend_factor_to_metal(d->blend_dfactor);
            ca.rgbBlendOperation           = blend_equation_to_metal(d->blend_equation);
            ca.alphaBlendOperation         = blend_equation_to_metal(d->blend_equation);
        }
    }

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
        fprintf(stderr, "[RSX metal] pipeline state failed (vs %d, fs %d): %s\n",
                d->vs_idx, d->fs_idx, [[err localizedDescription] UTF8String]);
        return nil;
    }
    s_pso_cache[s_pso_count].key = key;
    s_pso_cache[s_pso_count].pso = pso;
    s_pso_count++;
    return pso;
}

/* ---- replay --------------------------------------------------------------
 * Everything a render pass has to bind before its first draw. Encoder state
 * does not outlive endEncoding, so a clear in the middle of a frame -- or a
 * switch to another render target -- pays for this again on the pass it opens.
 * --------------------------------------------------------------------------*/

typedef struct {
    id<MTLBuffer> vb;
    id<MTLBuffer> cbuf;
    int tex[RSX_MAX_TEXTURES];    /* what each unit currently holds, or -1 */
    int samp[RSX_MAX_TEXTURES];
    int vtex[RSX_MAX_VERTEX_TEXTURES];
    int vsamp[RSX_MAX_VERTEX_TEXTURES];
    /* The target this pass is aimed at, and what the attachments turned out
     * to be. A record whose target differs ends the pass and opens the next. */
    MtlTarget rt;
    double w, h;                  /* the target's size, for the viewport */
    MTLPixelFormat color_pf;
    u32 nrt;                      /* bound colour attachments */
} MtlPassState;

/* Do two records belong in the same pass? Only the colour targets decide it:
 * everything else about a record is encoder or pipeline state. */
static int target_same(const MtlTarget* a, const MtlTarget* b)
{
    return a->off == b->off && memcmp(a->mrt, b->mrt, sizeof a->mrt) == 0;
}

/* What a unit binds, as one identity covering both sources: a surface view, an
 * uploaded texture, or the zero texture. Surface views are numbered above the
 * texture cache so the "already bound?" test stays a single compare. */
static int tex_binding_id(const MtlDraw* d, int u)
{
    return d->tex_rt[u] >= 0 ? MTL_TEX_CACHE + d->tex_rt[u] : d->tex[u];
}

static id<MTLTexture> tex_binding(const MtlDraw* d, int u)
{
    if (d->tex_rt[u] >= 0 && s_rtview[d->tex_rt[u]].view)
        return s_rtview[d->tex_rt[u]].view;
    if (d->tex[u] >= 0) return s_tex_cache[d->tex[u]].tex;
    return s_null_tex;
}

/* Where a vertex texture binds. The VP decompiler declares unit N's texture
 * at HLSL register t(16+N) and its sampler at sN, and spirv-cross carries
 * both numbers into the vertex function -- so the two do NOT share an index.
 * test_shader_msl asserts exactly that. */
#define MTL_VTEX_INDEX(n) ((NSUInteger)(16u + (n)))
#define MTL_VSAMP_INDEX(n) ((NSUInteger)(n))

/* Open a render pass on the record's target: the drawable for a display
 * buffer, else the offscreen surface registered at that raw offset, with its
 * MRT set and its own zeta in place of the shared depth buffer. The
 * attachments `clear` flags name load-action Clear with its values; the rest
 * load-action Load, so what an earlier pass in the same frame left behind
 * survives -- which is how a title fills one surface across several runs of
 * records. */
static id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> cb,
                                              id<MTLTexture> display,
                                              const MtlTarget* t,
                                              const MtlClear* clear,
                                              MtlPassState* ps)
{
    /* Latched before anything can fail, so a target that cannot be bound is
     * still this pass's target: the rest of its run is skipped rather than
     * reopened once per record. */
    ps->rt = *t;

    const int surf = surface_find(t->off);
    id<MTLTexture> color = (surf >= 0) ? s_surf[surf].tex : display;
    if (!color) return nil;

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = color;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear->flags & MTL_CLEAR_COLOR) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = clear_color_from_argb(clear->color);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }

    /* MRT B, C and D, up to the first one that is not a registered surface --
     * the D3D12 backend stops at the same gap. Metal wants every attachment
     * the same size, and surfaces are registered at target A's size, so a set
     * whose members disagree is one this frame cannot honour. */
    ps->nrt = 1;
    for (int m = 0; m < 3 && t->mrt[m]; m++) {
        const int ms = surface_find(t->mrt[m]);
        if (ms < 0 || [s_surf[ms].tex width]  != [color width]
                   || [s_surf[ms].tex height] != [color height]) break;
        rp.colorAttachments[ps->nrt].texture     = s_surf[ms].tex;
        rp.colorAttachments[ps->nrt].storeAction = MTLStoreActionStore;
        if (clear->flags & MTL_CLEAR_COLOR) {
            rp.colorAttachments[ps->nrt].loadAction = MTLLoadActionClear;
            rp.colorAttachments[ps->nrt].clearColor = clear_color_from_argb(clear->color);
        } else {
            rp.colorAttachments[ps->nrt].loadAction = MTLLoadActionLoad;
        }
        ps->nrt++;
    }

    /* Both halves of one Depth32Float_Stencil8 texture: the shared one for the
     * display, the surface's own zeta otherwise, so an offscreen pass does not
     * trample the depth the display pass is building. Stored rather than
     * discarded: a later pass in the same frame loads what this one wrote. */
    id<MTLTexture> depth = s_depth;
    if (surf >= 0) {
        id<MTLTexture> z = zeta_get(t->zeta_off, s_surf[surf].w, s_surf[surf].h);
        if (z) depth = z;
    }
    rp.depthAttachment.texture       = depth;
    rp.depthAttachment.storeAction   = MTLStoreActionStore;
    rp.depthAttachment.loadAction    = (clear->flags & MTL_CLEAR_DEPTH)
                                           ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth    = (double)clear->depth;
    rp.stencilAttachment.texture     = depth;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.loadAction  = (clear->flags & MTL_CLEAR_STENCIL)
                                           ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.stencilAttachment.clearStencil = clear->stencil;

    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) return nil;
    ps->w        = (double)[color width];
    ps->h        = (double)[color height];
    ps->color_pf = [color pixelFormat];

    if (ps->vb) [enc setVertexBuffer:ps->vb offset:0 atIndex:MTL_VB_INDEX];
    /* Every fragment texture unit starts on the zero texture -- a guest
     * program may sample a unit nothing was bound to, and Metal validation
     * rejects a nil texture on use. Units are then rebound per draw only where
     * the draw's slot differs. */
    for (int u = 0; u < RSX_MAX_TEXTURES; u++) {
        [enc setFragmentTexture:s_null_tex atIndex:(NSUInteger)u];
        [enc setFragmentSamplerState:s_default_sampler atIndex:(NSUInteger)u];
        ps->tex[u] = ps->samp[u] = -1;
    }
    /* The vertex stage's units get the same treatment, for the same reason. */
    for (int u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) {
        [enc setVertexTexture:s_null_tex atIndex:MTL_VTEX_INDEX(u)];
        [enc setVertexSamplerState:s_default_sampler atIndex:MTL_VSAMP_INDEX(u)];
        ps->vtex[u] = ps->vsamp[u] = -1;
    }
    /* The target's own size, which is not the window's once a title renders
     * into a surface of its own. Each draw narrows it to the guest rect. */
    MTLViewport vp = { 0.0, 0.0, ps->w, ps->h, 0.0, 1.0 };
    [enc setViewport:vp];
    return enc;
}

static void encode_draw(id<MTLRenderCommandEncoder> enc, const MtlDraw* d,
                        MtlPassState* ps)
{
    id<MTLRenderPipelineState> pso = pso_for(d, ps->color_pf, ps->nrt);
    if (!pso) return;
    [enc setRenderPipelineState:pso];
    [enc setCullMode:d->cull];
    [enc setFrontFacingWinding:d->winding];
    [enc setDepthStencilState:(d->ds_idx >= 0 ? s_ds_cache[d->ds_idx].state
                                              : s_ds_default)];
    [enc setStencilReferenceValue:d->stencil_ref];
    /* The guest viewport rect when it fits inside the target, else the whole
     * target -- render_frame's rule. Sub-viewport layouts place their quads
     * with this rect and nothing else, so a forced full-target viewport draws
     * every one of them target-sized. The scissor tracks it. */
    MTLViewport vp = { 0.0, 0.0, ps->w, ps->h, 0.0, 1.0 };
    if (d->rt.vp_w >= 2 && d->rt.vp_h >= 2 &&
        (double)(d->rt.vp_x + d->rt.vp_w) <= ps->w + 0.5 &&
        (double)(d->rt.vp_y + d->rt.vp_h) <= ps->h + 0.5) {
        vp.originX = (double)d->rt.vp_x; vp.originY = (double)d->rt.vp_y;
        vp.width   = (double)d->rt.vp_w; vp.height  = (double)d->rt.vp_h;
    }
    [enc setViewport:vp];
    [enc setScissorRect:(MTLScissorRect){
        (NSUInteger)vp.originX, (NSUInteger)vp.originY,
        (NSUInteger)vp.width,   (NSUInteger)vp.height }];
    if (d->vs_idx >= 0 && ps->cbuf) {
        [enc setVertexBuffer:ps->cbuf offset:d->vp_cb_off atIndex:0];
        [enc setFragmentBuffer:ps->cbuf offset:d->fp_cb_off atIndex:1];
        for (int u = 0; u < RSX_MAX_TEXTURES; u++) {
            const int want = tex_binding_id(d, u);
            if (want != ps->tex[u]) {
                [enc setFragmentTexture:tex_binding(d, u) atIndex:(NSUInteger)u];
                ps->tex[u] = want;
            }
            if (d->samp[u] != ps->samp[u]) {
                [enc setFragmentSamplerState:(d->samp[u] >= 0 ? s_samp_cache[d->samp[u]].samp
                                                              : s_default_sampler)
                                     atIndex:(NSUInteger)u];
                ps->samp[u] = d->samp[u];
            }
        }
        for (int u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) {
            if (d->vtex[u] != ps->vtex[u]) {
                [enc setVertexTexture:(d->vtex[u] >= 0 ? s_tex_cache[d->vtex[u]].tex
                                                       : s_null_tex)
                              atIndex:MTL_VTEX_INDEX(u)];
                ps->vtex[u] = d->vtex[u];
            }
            if (d->vsamp[u] != ps->vsamp[u]) {
                [enc setVertexSamplerState:(d->vsamp[u] >= 0 ? s_samp_cache[d->vsamp[u]].samp
                                                             : s_default_sampler)
                                   atIndex:MTL_VSAMP_INDEX(u)];
                ps->vsamp[u] = d->vsamp[u];
            }
        }
    } else {
        [enc setVertexBytes:d->mvp length:sizeof(d->mvp) atIndex:0];
    }
    [enc drawPrimitives:d->topology vertexStart:d->base vertexCount:d->count];
}

static void clear_caches(void)
{
    for (u32 i = 0; i < s_vp_count;   i++) s_vp_cache[i].fn     = nil;
    for (u32 i = 0; i < s_fp_count;   i++) s_fp_cache[i].fn     = nil;
    for (u32 i = 0; i < s_pso_count;  i++) s_pso_cache[i].pso   = nil;
    for (u32 i = 0; i < s_tex_count;  i++) s_tex_cache[i].tex   = nil;
    for (u32 i = 0; i < s_samp_count; i++) s_samp_cache[i].samp = nil;
    for (u32 i = 0; i < s_ds_count;   i++) s_ds_cache[i].state  = nil;
    for (u32 i = 0; i < s_rtview_count; i++) s_rtview[i].view   = nil;
    s_vp_count = s_fp_count = s_pso_count = s_tex_count = s_samp_count = 0;
    s_ds_count = s_rtview_count = 0;
    s_caches_full = 0;
}

/* Surfaces survive a cache clear -- they hold rendered pixels a later frame
 * still samples -- so they are released only on the way out. */
static void release_surfaces(void)
{
    for (u32 i = 0; i < s_surf_count; i++) s_surf[i].tex = nil;
    for (u32 i = 0; i < s_zeta_count; i++) s_zeta[i].tex = nil;
    s_surf_count = s_zeta_count = 0;
}


/* ---- the register-file draw engine's backend --------------------------------
 * rsx_draw_engine.c implements rsx_dispatch_sink over the whole NV4097
 * register file and drives a backend through rsx_draw_backend. Everything
 * below is this file's implementation of that interface. It is a second
 * record stream rather than a reworking of the one above, because the two
 * front ends are different -- the vtable one is fed rsx_state and resolves a
 * draw's programs and textures itself, while the engine hands over resolved
 * handles -- and the vtable path stays the default until they have parity.
 *
 * What is shared is everything below the front end: the device and queue, the
 * shader translation, the texture decode, the sampler decode tables and the
 * blend, compare and stencil translations.
 *
 * Handles are indices into one object table, because the engine binds colour
 * targets, depth snapshots and uploaded textures through the same array and a
 * bind cannot tell them apart. Pipelines have their own table: only
 * bind_pipeline ever names one.
 * --------------------------------------------------------------------------*/

#include "rsx_draw_engine.h"

/* Textures, colour targets, depth snapshots and views share this table. New
 * Marais keeps ~4000 of them live (the texture cache plus every surface's
 * views and snapshots): at 4096 a full table made new textures fail and
 * sample black ("texture bind FAILED ... -> black", ~48 in a 260 s walk). */
#define ENG_MAX_OBJECTS  16384
#define ENG_MAX_PIPES    4096
#define ENG_MAX_RECORDS  65536   /* inFamous world frames issue >9k records */
#define ENG_MAX_VIEWS    256
#define ENG_MAX_FUNCS    4096
#define ENG_MAX_SAMPLERS 256
#define ENG_ALIGN        256u
/* One frame's staging. Past it the backend submits and waits rather than
 * dropping the rest of the frame: dropping "discarded 181/184 sampled a010
 * groups" once the orphanage workload filled the D3D12 engine's ring. */
#define ENG_STAGE_MAX    (384u << 20)

static int s_eng_active;

static id<MTLTexture> s_eng_obj[ENG_MAX_OBJECTS];
static u32 s_eng_obj_count;
static u32 s_eng_obj_free[ENG_MAX_OBJECTS];
static u32 s_eng_obj_free_count;
static u32 s_eng_obj_retired[ENG_MAX_OBJECTS];   /* 0, or the submission it was released in */
/* PS3_RES_SCALE: the object is res_px() of the size the engine knows it by
 * (a target, a copy or decode of one, a view of one): readback samples it
 * down. */
static u8 s_eng_obj_scaled[ENG_MAX_OBJECTS];
/* Persistent vertex buffers (rsx_draw_backend.vbuf_*), a table of their own:
 * the object table above is shared with every texture and view. Released
 * buffers are retired like objects, freed once no recorded draw can name them. */
#define ENG_MAX_VBUFS 32768
static id<MTLBuffer> s_vbuf[ENG_MAX_VBUFS];
static u32 s_vbuf_count, s_vbuf_free[ENG_MAX_VBUFS], s_vbuf_free_count;
static u32 s_vbuf_retired[ENG_MAX_VBUFS];         /* 0, or the submission it was released in */
/* Submissions: the one being recorded (1, 2, ...) and the last one whose
 * records have been encoded (an object released in submission n may be
 * freed once n is encoded: later records never name it). */
static u32 s_rec_seq = 1;
static volatile u32 s_enc_done_seq;
static int eng_enc_busy(void);
static void eng_enc_join(void);    /* PS3RECOMP_METAL_ENCODE_THREAD: wait for the encoder thread */
static int eng_headless_sync(void);

typedef struct {
    id<MTLRenderPipelineState> pso;
    id<MTLDepthStencilState>   ds;
    MTLCullMode cull;
    MTLWinding  winding;
    int ready;       /* pso is final (set last; PS3_SHADER_ASYNC builds it on a queue) */
    char desc[144];  /* PS3RECOMP_METAL_PASS_LOG summary of the fixed-function state */
} EngPipeline;
static EngPipeline s_eng_pipe[ENG_MAX_PIPES];
static u32 s_eng_pipe_count;

/* Compiled MSL functions, keyed on the HLSL the decompilers emitted: one
 * program is translated and compiled once however many pipeline variants
 * reference it. */
typedef struct { u64 hash; id<MTLFunction> fn; } EngFunc;
static EngFunc s_eng_func[ENG_MAX_FUNCS];
static u32 s_eng_func_count;
/* PS3_SHADER_ASYNC (below, with the shader cache): the build queue, builds
 * still queued, and draws skipped because their pipeline was not ready. */
static dispatch_queue_t s_sc_queue;
static u32 s_sc_async_pending, s_sc_async_skipped;

typedef struct { u64 key; id<MTLSamplerState> samp; } EngSampler;
static EngSampler s_eng_samp[ENG_MAX_SAMPLERS];
static u32 s_eng_samp_count;
/* PS3RECOMP_ENG_SAMP_INDEX: key -> slot memo in front of eng_sampler_slot's scan. */
static struct { u64 key; u32 slot; } s_samp_memo[256];   /* slot + 1, 0 = empty */
static int s_samp_memo_on = 1;

typedef struct { u32 surface, remap, format, view; } EngView;
static EngView s_eng_view[ENG_MAX_VIEWS];
static u32 s_eng_view_count;

typedef enum {
    ENG_REC_DRAW, ENG_REC_CLEAR_COLOR, ENG_REC_CLEAR_DS, ENG_REC_DEPTH_RESOLVE,
    ENG_REC_COLOR_COPY, ENG_REC_LIGHT, ENG_REC_SSAO
} EngRecKind;

typedef struct {
    EngRecKind kind;
    /* The colour targets a draw writes, A first, and how many of them; a
     * colour clear names one, in rt[0]. */
    u32 rt[RSX_BE_MAX_COLOR_TARGETS];
    u32 nrt;
    u32 depth;
    u32 pipeline;
    MTLPrimitiveType topology;
    u32 vb_off, stride, vertex_count;
    u32 vb_buf;                 /* a persistent vertex buffer (eng_vbuf_*), else 0: the arena */
    u32 ib_off, index_count;
    u32 ib_buf;                 /* a persistent index buffer (eng_vbuf_*), else 0: the arena at ib_off */
    u32 vs_cb_off, vs_cb_bytes;
    float vs_xf[8];             /* VPXform (buffer 2): vp_posscale, vp_posoffset */
    u32 ps_cb_off, ps_cb_bytes;
    u32 tex[RSX_BE_MAX_TEXTURES];
    int samp[RSX_BE_MAX_TEXTURES];
    u32 vtex[RSX_BE_MAX_VERTEX_TEXTURES];
    int vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
    float vp[4];
    u32   sc[4];
    u32   stencil_ref;
    float zbias[2];             /* set_depth_bias: constant (D24 units), slope */
    int   zclamp;               /* set_depth_clamp: MTLDepthClipModeClamp */
    float clear_rgba[4];
    u32   clear_flags;
    float clear_depth;
    u8    clear_stencil;
    u32   resolve_dst;          /* ENG_REC_DEPTH_RESOLVE target texture */
} EngRecord;

/* Two banks (PS3RECOMP_METAL_ENCODE_THREAD): the RSX thread records the
 * next submission into one while the encoder thread turns the other into a
 * command buffer. s_eng_rec is the bank being recorded. */
static EngRecord s_eng_rec_bank[2][ENG_MAX_RECORDS];
static EngRecord* s_eng_rec = s_eng_rec_bank[0];
static u32 s_eng_rec_count;
/* The records eng_encode_records walks: the bank being encoded. */
static const EngRecord* s_enc_rec;
static u32 s_enc_count;
static u32 s_eng_dropped;

static u8* s_eng_stage;
static u32 s_eng_stage_used, s_eng_stage_cap;
/* The staging arena IS a shared MTLBuffer (T-0016). It used to be malloc'd
 * memory that eng_encode_and_commit copied, every commit, into a fresh
 * newBufferWithBytes buffer: a second full copy of the frame's vertex,
 * index and constant data plus a buffer allocation per frame -- the largest
 * single cost on the RSX thread (memmove under newBufferWithBytes, ~15% of
 * it). On Apple silicon shared storage is the same memory for CPU and GPU,
 * so the arena is written in place and the commit hands that buffer over.
 *
 * A ring of ENG_STAGE_SLOTS buffers: a windowed present leaves its command
 * buffer in flight (at most MTL_MAX_INFLIGHT, bounded by s_inflight) and
 * every other commit waits for completion, so the slot the CPU fills next is
 * never one the GPU is still reading. Each slot also carries a busy flag,
 * set at an asynchronous commit and cleared by its completion handler, and
 * acquiring a busy slot waits for it -- belt and braces, counted if it ever
 * happens. */
#define ENG_STAGE_SLOTS (MTL_MAX_INFLIGHT + 1)
static id<MTLBuffer> s_eng_slot_buf[ENG_STAGE_SLOTS];
static volatile int s_eng_slot_busy[ENG_STAGE_SLOTS];
static int s_eng_slot = -1;          /* slot the arena currently lives in; -1: none */
static unsigned s_eng_slot_next;
static unsigned long s_eng_slot_waits;

/* What the bind_* calls have accumulated for the next draw. */
static EngRecord s_eng_pending;

/* The blit and depth-resolve helpers, and their sampler. */
static id<MTLLibrary>             s_eng_helper_lib;
static id<MTLRenderPipelineState> s_eng_blit_pso;
static MTLPixelFormat             s_eng_blit_pso_fmt;
static id<MTLRenderPipelineState> s_eng_depth_pso;
static id<MTLRenderPipelineState> s_eng_depth_rgba_pso;
static id<MTLSamplerState>        s_eng_point_sampler;
static id<MTLSamplerState>        s_eng_linear_sampler;

/* The post passes' state (PS3_AA, PS3_UPSCALE): the FXAA target and pipeline,
 * the MetalFX scaler and its output, each rebuilt when a size or format the
 * frame brings no longer matches. */
static id<MTLRenderPipelineState> s_gfx_fxaa_pso;
static MTLPixelFormat             s_gfx_fxaa_pso_fmt;
static id<MTLTexture>             s_gfx_fxaa_tex;
static id                         s_gfx_fx_scaler;   /* id<MTLFXSpatialScaler> */
static id<MTLTexture>             s_gfx_fx_out;
static int                        s_gfx_fx_logged;
/* PS3_AA=smaa: its library (third_party/smaa/smaa_msl.h), the three passes'
 * pipelines, the two lookup textures and the per-size targets. */
static id<MTLLibrary>             s_smaa_lib;
static id<MTLRenderPipelineState> s_smaa_edge_pso, s_smaa_weight_pso, s_smaa_blend_pso;
static MTLPixelFormat             s_smaa_blend_fmt;
static id<MTLTexture>             s_smaa_area, s_smaa_search, s_smaa_edges, s_smaa_weights, s_smaa_out;
static void gfx_post_release(void)
{
    s_gfx_fxaa_pso = nil; s_gfx_fxaa_tex = nil;
    s_smaa_lib = nil; s_smaa_edge_pso = s_smaa_weight_pso = s_smaa_blend_pso = nil;
    s_smaa_area = s_smaa_search = s_smaa_edges = s_smaa_weights = s_smaa_out = nil;
    s_gfx_fx_scaler = nil; s_gfx_fx_out = nil;
    for (int k = 0; k < GFX_LUT_N; k++) s_gfx_lut_presets[k] = nil;
    if (s_gfx_lut != s_gfx_lut_file) { s_gfx_lut = s_gfx_lut_file; s_gfx_lut_size = s_gfx_lut_file_size; }
    s_gfx_lut_preset_built = -1;
}

static NSString* const kEngHelperMSL = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct BOut { float4 pos [[position]]; float2 uv; };\n"
"vertex BOut eng_fullscreen_vs(uint vid [[vertex_id]]) {\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    BOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv = float2(p.x, 1.0 - p.y);\n"
"    return o;\n"
"}\n"
"fragment float4 eng_blit_fs(BOut i [[stage_in]],\n"
"                            texture2d<float> src [[texture(0)]],\n"
"                            sampler s [[sampler(0)]]) {\n"
"    return src.sample(s, i.uv);\n"
"}\n"
/* The present: optional contrast-adaptive sharpening (AMD FidelityFX CAS,
 * the non-scaling form: a cross whose negative weight grows where the 3x3
 * neighbourhood has headroom), at the output's pixel spacing, then the video
 * out's gamma. sharp = 0 is the plain sample it always was. */
"struct PresentP { float gamma; float sharp; float2 px; float vib; float con; float vig; float grain;\n"
"                  float time; float lsz; float lamt; float pad; float csh; float cmid; float chi; float cla; };\n"
"fragment float4 eng_present_fs(BOut i [[stage_in]],\n"
"                               texture2d<float> src [[texture(0)]],\n"
"                               sampler s [[sampler(0)]],\n"
"                               constant PresentP& p [[buffer(0)]],\n"
"                               texture2d<float> lut [[texture(1)]],\n"
"                               sampler ls [[sampler(1)]]) {\n"
"    float4 c = src.sample(s, i.uv);\n"
"    if (p.sharp > 0.0) {\n"
"        float2 d = p.px;\n"
"        float3 a = src.sample(s, i.uv + float2(-d.x, -d.y)).rgb;\n"
"        float3 b = src.sample(s, i.uv + float2( 0.0, -d.y)).rgb;\n"
"        float3 cc = src.sample(s, i.uv + float2( d.x, -d.y)).rgb;\n"
"        float3 l = src.sample(s, i.uv + float2(-d.x,  0.0)).rgb;\n"
"        float3 e = saturate(c.rgb);\n"
"        float3 r = src.sample(s, i.uv + float2( d.x,  0.0)).rgb;\n"
"        float3 g = src.sample(s, i.uv + float2(-d.x,  d.y)).rgb;\n"
"        float3 h = src.sample(s, i.uv + float2( 0.0,  d.y)).rgb;\n"
"        float3 k = src.sample(s, i.uv + float2( d.x,  d.y)).rgb;\n"
"        b = saturate(b); l = saturate(l); r = saturate(r); h = saturate(h);\n"
"        float3 mn = min(min(min(l, e), min(r, b)), h);\n"
"        float3 mn2 = min(mn, min(min(saturate(a), saturate(cc)), min(saturate(g), saturate(k))));\n"
"        mn += mn2;\n"
"        float3 mx = max(max(max(l, e), max(r, b)), h);\n"
"        float3 mx2 = max(mx, max(max(saturate(a), saturate(cc)), max(saturate(g), saturate(k))));\n"
"        mx += mx2;\n"
"        float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));\n"
"        float3 w = amp * (-1.0 / mix(8.0, 5.0, saturate(p.sharp)));\n"
"        c.rgb = saturate((b * w + l * w + r * w + h * w + e) / (1.0 + 4.0 * w));\n"
"    }\n"
/* Clarity: the luminance minus a wide blur of it (two rings of 8 bilinear
 * taps, ~1.1% and ~0.55% of the picture height), added back weighted toward
 * the midtones and clamped, so edges between large shapes gain contrast
 * without halos round the brightest and darkest parts. */
"    if (p.cla != 0.0) {\n"
"        const float3 W = float3(0.2126, 0.7152, 0.0722);\n"
"        float2 rr = float2(0.011 * p.px.x / p.px.y, 0.011);\n"
"        float l0 = dot(saturate(c.rgb), W), acc = l0 * 0.5, wsum = 0.5;\n"
"        for (int k = 0; k < 8; k++) {\n"
"            float a = float(k) * 0.785398 + 0.392699;\n"
"            float2 d = float2(cos(a), sin(a)) * rr;\n"
"            acc += dot(saturate(src.sample(ls, i.uv + d).rgb), W) * 0.6;\n"
"            acc += dot(saturate(src.sample(ls, i.uv + d * 0.5).rgb), W);\n"
"            wsum += 1.6;\n"
"        }\n"
"        float det = clamp(l0 - acc / wsum, -0.15, 0.15);\n"
"        float mid = saturate(1.0 - abs(l0 * 2.0 - 1.0));\n"
"        c.rgb = saturate(c.rgb + det * p.cla * 1.25 * (0.35 + 0.65 * mid));\n"
"    }\n"
/* Curves: three bumps that are zero at black and white (so neither moves),
 * peaking at 0.25, 0.5 and 0.75; at +-1 each shifts its zone by 0.1 and the
 * curve stays monotonic. */
"    if (p.csh != 0.0 || p.cmid != 0.0 || p.chi != 0.0) {\n"
"        float3 x = saturate(c.rgb), y = 1.0 - x;\n"
"        float3 ws = 9.4815 * x * y * y * y;\n"
"        float3 wm = 16.0 * x * x * y * y;\n"
"        float3 wh = 9.4815 * x * x * x * y;\n"
"        c.rgb = saturate(x + 0.1 * (p.csh * ws + p.cmid * wm + p.chi * wh));\n"
"    }\n"
"    if (p.vib != 0.0) {\n"
"        float l = dot(c.rgb, float3(0.299, 0.587, 0.114));\n"
"        float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));\n"
"        c.rgb = saturate(mix(float3(l), c.rgb, 1.0 + p.vib * (1.0 - sat)));\n"
"    }\n"
"    if (p.con != 1.0) c.rgb = saturate((c.rgb - 0.5) * p.con + 0.5);\n"
"    if (p.lamt > 0.0) {\n"
"        float sz = p.lsz;\n"
"        float3 q = saturate(c.rgb) * (sz - 1.0);\n"
"        float b0 = floor(q.b), b1 = min(b0 + 1.0, sz - 1.0);\n"
"        float2 u0 = float2((b0 * sz + q.r + 0.5) / (sz * sz), (q.g + 0.5) / sz);\n"
"        float2 u1 = float2((b1 * sz + q.r + 0.5) / (sz * sz), (q.g + 0.5) / sz);\n"
"        float3 l = mix(lut.sample(ls, u0).rgb, lut.sample(ls, u1).rgb, q.b - b0);\n"
"        c.rgb = mix(c.rgb, l, p.lamt);\n"
"    }\n"
"    if (p.vig > 0.0) {\n"
"        float v = smoothstep(0.95, 0.35, length(i.uv - 0.5) * 1.35);\n"
"        c.rgb *= mix(1.0, v, p.vig);\n"
"    }\n"
"    if (p.grain > 0.0) {\n"
"        float n = fract(sin(dot(floor(i.pos.xy) + p.time * float2(37.0, 17.0), float2(12.9898, 78.233))) * 43758.5453);\n"
"        c.rgb = saturate(c.rgb + (n - 0.5) * 0.12 * p.grain);\n"
"    }\n"
"    if (p.gamma != 1.0) c.rgb = pow(max(c.rgb, 0.0), float3(p.gamma));\n"
"    return c;\n"
"}\n"
/* FXAA 3.11, quality preset 29 (Timothy Lottes, public domain), with luma
 * from RGB: the display buffer's alpha is not luma, and it is kept. */
"static float fx_l(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
"constant float FX_P[12] = { 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
"fragment float4 eng_fxaa_fs(BOut i [[stage_in]],\n"
"                            texture2d<float> t [[texture(0)]],\n"
"                            sampler s [[sampler(0)]],\n"
"                            constant float2& rcp [[buffer(0)]]) {\n"
"    const float subpix = 0.75, thr = 0.166, thr_min = 0.0833;\n"
"    float2 posM = i.uv;\n"
"    float4 rgbyM = t.sample(s, posM);\n"
"    float lumaM = fx_l(rgbyM.rgb);\n"
"    float lumaS = fx_l(t.sample(s, posM, int2( 0,  1)).rgb);\n"
"    float lumaE = fx_l(t.sample(s, posM, int2( 1,  0)).rgb);\n"
"    float lumaN = fx_l(t.sample(s, posM, int2( 0, -1)).rgb);\n"
"    float lumaW = fx_l(t.sample(s, posM, int2(-1,  0)).rgb);\n"
"    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));\n"
"    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));\n"
"    float range = rangeMax - rangeMin;\n"
"    if (range < max(thr_min, rangeMax * thr)) return rgbyM;\n"
"    float lumaNW = fx_l(t.sample(s, posM, int2(-1, -1)).rgb);\n"
"    float lumaSE = fx_l(t.sample(s, posM, int2( 1,  1)).rgb);\n"
"    float lumaNE = fx_l(t.sample(s, posM, int2( 1, -1)).rgb);\n"
"    float lumaSW = fx_l(t.sample(s, posM, int2(-1,  1)).rgb);\n"
"    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;\n"
"    float subpixRcpRange = 1.0 / range;\n"
"    float subpixNSWE = lumaNS + lumaWE;\n"
"    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;\n"
"    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;\n"
"    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;\n"
"    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;\n"
"    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);\n"
"    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);\n"
"    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;\n"
"    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;\n"
"    float subpixNWSWNESE = lumaNWSW + lumaNESE;\n"
"    float lengthSign = rcp.x;\n"
"    bool horzSpan = edgeHorz >= edgeVert;\n"
"    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;\n"
"    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }\n"
"    if (horzSpan) lengthSign = rcp.y;\n"
"    float subpixB = subpixA * (1.0 / 12.0) - lumaM;\n"
"    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;\n"
"    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;\n"
"    bool pairN = abs(gradientN) >= abs(gradientS);\n"
"    float gradient = max(abs(gradientN), abs(gradientS));\n"
"    if (pairN) lengthSign = -lengthSign;\n"
"    float subpixC = saturate(abs(subpixB) * subpixRcpRange);\n"
"    float2 posB = posM;\n"
"    float2 offNP = float2(horzSpan ? rcp.x : 0.0, horzSpan ? 0.0 : rcp.y);\n"
"    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;\n"
"    float2 posN = posB - offNP * FX_P[0], posP = posB + offNP * FX_P[0];\n"
"    float subpixD = -2.0 * subpixC + 3.0;\n"
"    float lumaEndN = fx_l(t.sample(s, posN).rgb);\n"
"    float subpixE = subpixC * subpixC;\n"
"    float lumaEndP = fx_l(t.sample(s, posP).rgb);\n"
"    if (!pairN) lumaNN = lumaSS;\n"
"    float gradientScaled = gradient * (1.0 / 4.0);\n"
"    float lumaMM = lumaM - lumaNN * 0.5;\n"
"    float subpixF = subpixD * subpixE;\n"
"    bool lumaMLTZero = lumaMM < 0.0;\n"
"    lumaEndN -= lumaNN * 0.5; lumaEndP -= lumaNN * 0.5;\n"
"    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;\n"
"    if (!doneN) posN -= offNP * FX_P[1];\n"
"    if (!doneP) posP += offNP * FX_P[1];\n"
"    for (int k = 2; k < 12 && (!doneN || !doneP); k++) {\n"
"        if (!doneN) lumaEndN = fx_l(t.sample(s, posN).rgb) - lumaNN * 0.5;\n"
"        if (!doneP) lumaEndP = fx_l(t.sample(s, posP).rgb) - lumaNN * 0.5;\n"
"        doneN = abs(lumaEndN) >= gradientScaled; doneP = abs(lumaEndP) >= gradientScaled;\n"
"        if (!doneN) posN -= offNP * FX_P[k];\n"
"        if (!doneP) posP += offNP * FX_P[k];\n"
"    }\n"
"    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;\n"
"    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;\n"
"    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;\n"
"    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;\n"
"    float spanLengthRcp = 1.0 / (dstP + dstN);\n"
"    bool directionN = dstN < dstP;\n"
"    float dst = min(dstN, dstP);\n"
"    bool goodSpan = directionN ? goodSpanN : goodSpanP;\n"
"    float subpixG = subpixF * subpixF;\n"
"    float pixelOffset = dst * (-spanLengthRcp) + 0.5;\n"
"    float subpixH = subpixG * subpix;\n"
"    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);\n"
"    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;\n"
"    else           posM.y += pixelOffsetSubpix * lengthSign;\n"
"    return float4(t.sample(s, posM).rgb, rgbyM.a);\n"
"}\n"
"fragment float eng_depth_fs(BOut i [[stage_in]],\n"
"                            depth2d<float> src [[texture(0)]],\n"
"                            sampler s [[sampler(0)]]) {\n"
"    /* Pixel for pixel, as eng_depth_rgba8_fs: a copy smaller than the\n"
"     * depth buffer is its top-left corner (a depth texture over part of it,\n"
"     * the engine's eng_zdepth_snapshot_sized); a full-size copy is the same\n"
"     * texel for texel as before. */\n"
"    return src.sample(s, i.pos.xy / float2(src.get_width(), src.get_height()));\n"
"}\n"
"fragment float4 eng_depth_rgba8_fs(BOut i [[stage_in]],\n"
"                                   depth2d<float> src [[texture(0)]],\n"
"                                   texture2d<uint> st [[texture(1)]],\n"
"                                   sampler s [[sampler(0)]]) {\n"
"    /* Pixel for pixel: a target smaller than the depth buffer is its\n"
"     * top-left corner (a texture over part of it), as the stencil read. */\n"
"    float2 uv = i.pos.xy / float2(src.get_width(), src.get_height());\n"
"    uint v = uint(clamp(src.sample(s, uv), 0.0, 1.0) * 16777215.0);\n"
"    /* Z24S8 as A8R8G8B8: A = Z[23:16], R = Z[15:8], G = Z[7:0], B = stencil.\n"
"     * inFAMOUS 2's composite reads its material flags from the stencil. */\n"
"    uint sv = st.read(uint2(i.pos.xy)).x;\n"
"    return float4(float((v >> 8) & 255u), float(v & 255u), float(sv & 255u), float((v >> 16) & 255u)) / 255.0;\n"
"}\n"
/* The engine's GPU light pass (rsx_gpu_light_params): inFAMOUS 2's SPU light
 * job, reproduced from its lifted code. Per pixel: position from depth through
 * the job's matrix (camera-relative, pixel corner), normal and gloss from the
 * normals copy, then for every light a squared falloff between the inner and
 * outer radii (spots: a squared cone term and a near cut), N.L diffuse times
 * colour times the frame's scale, and a Blinn-style specular whose exponent
 * comes from the gloss. Both sums leave in the job's encoding: diffuse as
 * sqrt in RGB over a scale in A (the composite squares RGB*A back), specular
 * as sqrt in A. */
"struct GLight { float4 pos_type; float4 col_inner; float4 oacbn; float4 dir; };\n"
"struct GLParams { float4 m[4]; float4 misc; GLight l[128]; };\n"
"struct GLOut { float4 diff [[color(0)]]; float4 spec [[color(1)]]; };\n"
"fragment GLOut eng_light_fs(BOut i [[stage_in]],\n"
"                            texture2d<float> nt [[texture(0)]],\n"
"                            texture2d<float> zt [[texture(1)]],\n"
"                            constant GLParams& p [[buffer(0)]]) {\n"
"    uint2 px = uint2(i.pos.xy);\n"
"    GLOut o; o.diff = float4(0.0); o.spec = float4(0.0);\n"
"    uint z24 = uint(clamp(zt.read(px).x, 0.0, 1.0) * 16777215.0);\n"
"    if (z24 >= 0xFFFFFFu) return o;\n"
"    float zf = float(z24) / 16777216.0;\n"
"    float u = float(px.x) / p.misc.z, v = float(px.y) / p.misc.w;\n"
"    float4 h = u * p.m[0] + v * p.m[1] + zf * p.m[2] + p.m[3];\n"
"    float3 P = h.xyz / h.w;\n"
"    float4 nr = round(nt.read(px) * 255.0);\n"
"    float3 N = normalize(nr.rgb * (2.0 / 255.0) - 1.0);\n"
"    float s = exp2(nr.a * (8.0 / 255.0) - 0.88896);\n"
"    float3 V = -normalize(P);\n"
"    float3 acc = float3(0.0); float sp = 0.0;\n"
"    int n = int(p.misc.y);\n"
"    for (int k = 0; k < n; k++) {\n"
"        GLight L = p.l[k];\n"
"        float3 Lv = L.pos_type.xyz - P; float d = length(Lv); float3 l = Lv / d;\n"
"        float ndl = max(dot(N, l), 0.0);\n"
"        float att = saturate((L.oacbn.x - d) / (L.oacbn.x - L.col_inner.w)); att *= att;\n"
"        if (L.pos_type.w > 1.5) {\n"
"            float c = saturate((dot(-l, L.dir.xyz) - L.oacbn.z) * -L.oacbn.y);\n"
"            att *= c * c * (d > L.oacbn.w ? 1.0 : 0.0);\n"
"        }\n"
"        float w = att * ndl;\n"
"        if (w <= 0.0) continue;\n"
"        float3 C = L.col_inner.xyz * p.misc.x;\n"
"        float ndh = max(dot(N, normalize(l + V)), 0.0);\n"
"        acc += w * C;\n"
"        sp += w * length(C) * (1.0 + 25.0 / 27.0 * s) * pow(1.0 + s * (1.0 - ndh), -8.0);\n"
"    }\n"
"    const float K = 0.35355339 * 255.0;\n"
"    float3 q = sqrt(acc);\n"
"    float a = min(255.0, floor(max(1.0, max(q.r, max(q.g, q.b))) * K));\n"
"    o.diff = float4(min(float3(255.0), floor(q * K * 255.0 / a)), a) / 255.0;\n"
"    o.spec = float4(0.0, 0.0, 0.0, min(255.0, floor(sqrt(sp) * K)) / 255.0);\n"
"    return o;\n"
"}\n"
/* inFAMOUS 2's SSAO job (jobmod_e6fe749031f8): its five plugin passes,
 * reproduced from the lifted code, SPU reciprocal estimates and rounding
 * included -- half-res linear depth, 12-sample AO, two depth-aware blurs,
 * bilateral 2x upsample (rsx_draw_backend.ssao_pass). */
"\n"
"struct SsaoC {\n"
"    float4 depth_ab;          // E: A, B\n"
"    float  X[12];\n"
"    float  Y[12];\n"
"    float  Z[12];\n"
"    float4 M[3];              // rows (xyz)\n"
"    float4 C0, D0, E0, F0;    // D0 = norm,Kx,Ky,Kz  E0 = e0,e1,dmin,dmax  F0 = scale,bias\n"
"    float4 blur;              // k, w2 (+-2), w1 (+-1)\n"
"    float4 up;                // A, B, k\n"
"    uint4  dims;              // fullW, fullH, halfW, halfH\n"
"};\n"
"\n"
"constant uint ssao_frest_lut[32] = {\n"
"    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,\n"
"    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,\n"
"    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,\n"
"    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07 };\n"
"static inline float ssao_recip(float x) {\n"
"    uint b = as_type<uint>(x);\n"
"    uint sign = b & 0x80000000u, e = (b >> 23) & 0xFFu;\n"
"    uint re = (e == 0u) ? 255u : (e >= 253u ? 0u : 253u - e);\n"
"    uint est = sign | (re << 23) | (ssao_frest_lut[(b >> 18) & 31u] & 0x7FFFFFu);\n"
"    int ee = int((est >> 23) & 0xFFu);\n"
"    if (ee == 0) return 0.0f;\n"
"    ulong base = (est >> 10) & 0x1FFFu, step = est & 0x3FFu, y = b & 0x7FFFFu;\n"
"    ulong m = ((8192ul | base) << 19) - step * y;\n"
"    if (m < (1ul << 32)) { m <<= 1; if (--ee == 0) return 0.0f; }\n"
"    return as_type<float>(sign | (uint(ee) << 23) | uint((m >> 9) & 0x7FFFFFu));\n"
"}\n"
"static inline float ssao_mul_rz(float a, float b) {\n"
"    float p = a * b;\n"
"    return (fma(a, b, -p) < 0.0f && p > 0.0f) ? as_type<float>(as_type<uint>(p) - 1u) : p;\n"
"}\n"
"static inline float ssao_sat(float x) { return x >= 1.0f ? as_type<float>(0x3F7FFFFFu) : max(x, 0.0f); }\n"
"\n"
"static inline float ssao_z24(float d01) { return float(uint(clamp(d01, 0.0f, 1.0f) * 16777215.0f)); }\n"
"static inline float ssao_lin(float z24, float A, float B) {\n"
"    float k = A * as_type<float>(0x33800001u);\n"
"    float pe = fma(A, as_type<float>(0x33800001u), -k);         // round k toward zero like fm\n"
"    if ((pe > 0.0f && k < 0.0f) || (pe < 0.0f && k > 0.0f)) k = as_type<float>(as_type<uint>(k) - 1u);\n"
"    float v = fma(z24, k, B);\n"
"    float r = ssao_recip(v);\n"
"    return fma(fma(-v, r, 1.0f), r, r);\n"
"}\n"
"static inline float ssao_byte(texture2d<float> t, int2 p) { return rint(t.read(uint2(p)).r * 255.0f); }\n"
/* PS3_RES_SCALE: depth and normals are rs times the job's size; the passes
 * stay at the job's size and read the scaled pixel under each of theirs. */
"static inline uint2 ssao_sc(int2 p, float2 rs) { return uint2((float2(p) + 0.5f) * rs); }\n"
"\n"
"fragment float ssao_hd_fs(BOut i [[stage_in]],\n"
"                          texture2d<float> depth [[texture(0)]],\n"
"                          constant SsaoC& c [[buffer(0)]], constant float2& rs [[buffer(1)]]) {\n"
"    uint2 p = ssao_sc(int2(i.pos.xy) * 2, rs);\n"
"    return ssao_lin(ssao_z24(depth.read(p).r), c.depth_ab.x, c.depth_ab.y);\n"
"}\n"
"\n"
"fragment float ssao_ao_fs(BOut i [[stage_in]],\n"
"                          texture2d<float> hd  [[texture(0)]],\n"
"                          texture2d<float> nrm [[texture(1)]],\n"
"                          constant SsaoC& c [[buffer(0)]], constant float2& rs [[buffer(1)]]) {\n"
"    int2 p = int2(i.pos.xy);\n"
"    int2 hm = int2(c.dims.zw) - 1;\n"
"    float d   = hd.read(uint2(p)).r;\n"
"    float dc  = clamp(d, c.E0.z, c.E0.w);\n"
"    float rcp = ssao_recip(dc);\n"
"    float q   = d * rcp;\n"
"    float3 nb = rint(nrm.read(ssao_sc(p * 2, rs)).rgb * 255.0f);\n"
"    float3 n  = fma(nb * (1.0f / 256.0f), float3(2.0078430175781f), float3(-1.0f));\n"
"    float3 n2 = fma(float3(n.z), c.M[2].xyz, fma(float3(n.y), c.M[1].xyz, n.x * c.M[0].xyz));\n"
"    float dref = fma(n2.z, q, d);\n"
"    float bx = fma(n2.x, rcp, 0.5f), by = fma(n2.y, rcp, 0.5f);\n"
"    float sx = c.D0.y * rcp, sy = c.D0.z * rcp;\n"
"    float hz = c.D0.w * q;\n"
"    float e0 = c.E0.x * ssao_recip(q), e1 = c.E0.y * q;\n"
"    float acc = 0.0f;\n"
"    for (int k = 0; k < 12; k++) {\n"
"        int tx = p.x + int(floor(fma(c.X[k], sx, bx)));\n"
"        int ty = p.y + int(floor(fma(c.Y[k], sy, by)));\n"
"        float ds = hd.read(uint2(clamp(int2(tx, ty), int2(0), hm))).r;\n"
"        float h  = c.Z[k] * hz;\n"
"        float t  = (ds - (dref - h)) * 0.5f;\n"
"        acc += fma(ssao_sat(fma(t, e0, e1)), h, clamp(t, 0.0f, h));\n"
"    }\n"
"    acc *= ssao_recip(c.D0.x * q);\n"
"    float v = ssao_sat(fma(acc, c.F0.x, c.F0.y));\n"
"    return floor(ssao_mul_rz(v, 255.0f / 256.0f) * 256.0f) / 255.0f;   // <= 254\n"
"}\n"
"\n"
"static inline float ssao_blur(texture2d<float> ao, texture2d<float> hd, int2 p, int2 dir, constant SsaoC& c) {\n"
"    int2 hm = int2(c.dims.zw) - 1;\n"
"    float d0 = hd.read(uint2(p)).r;\n"
"    float kk = c.blur.x * ssao_recip(d0);\n"
"    float w[5], a[5];\n"
"    for (int o = -2; o <= 2; o++) {\n"
"        int2 s = clamp(p + o * dir, int2(0), hm);\n"
"        a[o + 2] = ssao_byte(ao, s);\n"
"        w[o + 2] = max(0.0f, fma(-fabs(hd.read(uint2(s)).r - d0), kk, 1.0f)) * ((o == -2 || o == 2) ? c.blur.y : c.blur.z);\n"
"    }\n"
"    float num = fma(w[4], a[4], fma(w[3], a[3], fma(w[1], a[1], fma(w[0], a[0], a[2]))));\n"
"    float den = w[4] + (w[3] + (1.0f + (w[0] + w[1])));\n"
"    return floor(ssao_mul_rz(num, ssao_recip(den))) / 255.0f;\n"
"}\n"
"fragment float ssao_blur_h_fs(BOut i [[stage_in]], texture2d<float> ao [[texture(0)]],\n"
"                              texture2d<float> hd [[texture(1)]], constant SsaoC& c [[buffer(0)]]) {\n"
"    return ssao_blur(ao, hd, int2(i.pos.xy), int2(1, 0), c);\n"
"}\n"
"fragment float ssao_blur_v_fs(BOut i [[stage_in]], texture2d<float> ao [[texture(0)]],\n"
"                              texture2d<float> hd [[texture(1)]], constant SsaoC& c [[buffer(0)]]) {\n"
"    return ssao_blur(ao, hd, int2(i.pos.xy), int2(0, 1), c);\n"
"}\n"
"\n"
"fragment float ssao_up_fs(BOut i [[stage_in]],\n"
"                          texture2d<float> ao    [[texture(0)]],\n"
"                          texture2d<float> hd    [[texture(1)]],\n"
"                          texture2d<float> depth [[texture(2)]],\n"
"                          constant SsaoC& c [[buffer(0)]], constant float2& rs [[buffer(1)]]) {\n"
"    int2 P  = int2(i.pos.xy);\n"
"    int2 hm = int2(c.dims.zw) - 1;\n"
"    float Z  = ssao_lin(ssao_z24(depth.read(ssao_sc(P, rs)).r), c.up.x, c.up.y);\n"
"    float kz = c.up.z * ssao_recip(Z);\n"
"    int2 b = P >> 1;\n"
"    int2 s = int2((P.x & 1) ? 1 : -1, (P.y & 1) ? 1 : -1);\n"
"    int2  off[4] = { int2(0, 0), int2(0, s.y), int2(s.x, 0), s };\n"
"    float bw[4]  = { 9.0f, 3.0f, 3.0f, 1.0f };\n"
"    float eps[4] = { as_type<float>(0x3A6BEDFAu), as_type<float>(0x399D4952u), as_type<float>(0x399D4952u), as_type<float>(0x38D1B717u) };\n"
"    float num = 0.0f, den = 0.0f;\n"
"    for (int k = 0; k < 4; k++) {\n"
"        int2 t = clamp(b + off[k], int2(0), hm);\n"
"        float w = fma(max(0.0f, fma(-fabs(hd.read(uint2(t)).r - Z), kz, 1.0f)), bw[k], eps[k]);\n"
"        num = fma(w, ssao_byte(ao, t), num);\n"
"        den += w;\n"
"    }\n"
"    return floor(ssao_mul_rz(num, ssao_recip(den))) / 255.0f;\n"
"}\n"
"\n"
"fragment float4 ssao_up4_fs(BOut i [[stage_in]],\n"
"                          texture2d<float> ao    [[texture(0)]],\n"
"                          texture2d<float> hd    [[texture(1)]],\n"
"                          texture2d<float> depth [[texture(2)]],\n"
"                          constant SsaoC& c [[buffer(0)]], constant float2& rs [[buffer(1)]]) {\n"
"    int2 P  = int2(i.pos.xy);\n"
"    int2 hm = int2(c.dims.zw) - 1;\n"
"    float Z  = ssao_lin(ssao_z24(depth.read(ssao_sc(P, rs)).r), c.up.x, c.up.y);\n"
"    float kz = c.up.z * ssao_recip(Z);\n"
"    int2 b = P >> 1;\n"
"    int2 s = int2((P.x & 1) ? 1 : -1, (P.y & 1) ? 1 : -1);\n"
"    int2  off[4] = { int2(0, 0), int2(0, s.y), int2(s.x, 0), s };\n"
"    float bw[4]  = { 9.0f, 3.0f, 3.0f, 1.0f };\n"
"    float eps[4] = { as_type<float>(0x3A6BEDFAu), as_type<float>(0x399D4952u), as_type<float>(0x399D4952u), as_type<float>(0x38D1B717u) };\n"
"    float num = 0.0f, den = 0.0f;\n"
"    for (int k = 0; k < 4; k++) {\n"
"        int2 t = clamp(b + off[k], int2(0), hm);\n"
"        float w = fma(max(0.0f, fma(-fabs(hd.read(uint2(t)).r - Z), kz, 1.0f)), bw[k], eps[k]);\n"
"        num = fma(w, ssao_byte(ao, t), num);\n"
"        den += w;\n"
"    }\n"
"    return float4(floor(ssao_mul_rz(num, ssao_recip(den))) / 255.0f);\n"
"}\n"
/* PS3_RES_SCALE: the same upsample, run per SCALED pixel (the output is the
 * size of the scaled targets). Each pixel takes its own depth and the
 * half-res AO texel nearest to its centre plus the neighbours on its side --
 * rs is the x and y scale (PS3_ASPECT widens x alone); at rs = 1 exactly
 * ssao_up4_fs's b = P >> 1 and parity sign. The 720p
 * output, stretched over the scaled image, had its depth-aware edges in 720p
 * steps; this one follows the scaled depth. */
"fragment float4 ssao_up4s_fs(BOut i [[stage_in]],\n"
"                          texture2d<float> ao    [[texture(0)]],\n"
"                          texture2d<float> hd    [[texture(1)]],\n"
"                          texture2d<float> depth [[texture(2)]],\n"
"                          constant SsaoC& c [[buffer(0)]], constant float2& rs [[buffer(1)]]) {\n"
"    int2 P  = int2(i.pos.xy);\n"
"    int2 hm = int2(c.dims.zw) - 1;\n"
"    float Z  = ssao_lin(ssao_z24(depth.read(uint2(P)).r), c.up.x, c.up.y);\n"
"    float kz = c.up.z * ssao_recip(Z);\n"
"    float2 h = (float2(P) + 0.5f) / (2.0f * rs) - 0.5f;\n"
"    int2 b = int2(floor(h + 0.5f));\n"
"    int2 s = int2(h.x >= float(b.x) ? 1 : -1, h.y >= float(b.y) ? 1 : -1);\n"
"    int2  off[4] = { int2(0, 0), int2(0, s.y), int2(s.x, 0), s };\n"
"    float bw[4]  = { 9.0f, 3.0f, 3.0f, 1.0f };\n"
"    float eps[4] = { as_type<float>(0x3A6BEDFAu), as_type<float>(0x399D4952u), as_type<float>(0x399D4952u), as_type<float>(0x38D1B717u) };\n"
"    float num = 0.0f, den = 0.0f;\n"
"    for (int k = 0; k < 4; k++) {\n"
"        int2 t = clamp(b + off[k], int2(0), hm);\n"
"        float w = fma(max(0.0f, fma(-fabs(hd.read(uint2(t)).r - Z), kz, 1.0f)), bw[k], eps[k]);\n"
"        num = fma(w, ssao_byte(ao, t), num);\n"
"        den += w;\n"
"    }\n"
"    return float4(floor(ssao_mul_rz(num, ssao_recip(den))) / 255.0f);\n"
"}\n"
"\n";

static u32 eng_obj_add(id<MTLTexture> t)
{
    if (!t) return 0;
    u32 slot;
    if (s_eng_obj_free_count) {
        slot = s_eng_obj_free[--s_eng_obj_free_count];
    } else {
        if (s_eng_obj_count >= ENG_MAX_OBJECTS) {
            static int told;
            if (!told) { told = 1; fprintf(stderr, "[rsx engine/metal] object table full (%d): new textures sample black\n", ENG_MAX_OBJECTS); }
            return 0;
        }
        slot = s_eng_obj_count++;
    }
    s_eng_obj[slot] = t;
    s_eng_obj_scaled[slot] = 0;
    return slot + 1;
}

static id<MTLTexture> eng_obj(u32 handle)
{
    return (handle && handle <= s_eng_obj_count) ? s_eng_obj[handle - 1] : nil;
}

static int eng_obj_is_scaled(u32 handle)
{
    return handle && handle <= s_eng_obj_count && s_eng_obj_scaled[handle - 1];
}

/* eng_obj_add for an object at res_px() of the engine's size. */
static u32 eng_obj_add_scaled(id<MTLTexture> t, int scaled)
{
    const u32 h = eng_obj_add(t);
    if (h) s_eng_obj_scaled[h - 1] = (u8)(scaled && s_res_scaled);
    return h;
}

/* Recorded commands resolve numeric handles during encoding. Do not recycle
 * a released slot until those commands have acquired their Metal resources.
 * Submitted command buffers retain the objects for the GPU's remaining work. */
static void eng_collect_retired_objects(void)
{
    /* Nothing recorded and nothing being encoded: everything retired goes
     * (the rule before the encoder thread). Otherwise only what was retired
     * in a submission already encoded (PS3RECOMP_METAL_ENCODE_THREAD). */
    u32 upto;
    if (!s_eng_rec_count && !eng_enc_busy()) upto = ~0u;
    else {
        upto = __atomic_load_n(&s_enc_done_seq, __ATOMIC_ACQUIRE);
        if (!upto) return;
    }
    for (u32 i = 0; i < s_vbuf_count; ++i)
        if (s_vbuf_retired[i] && s_vbuf_retired[i] <= upto) { s_vbuf_retired[i] = 0; s_vbuf[i] = nil; s_vbuf_free[s_vbuf_free_count++] = i; }
    for (u32 i = 0; i < s_eng_obj_count; ++i) {
        if (!s_eng_obj_retired[i] || s_eng_obj_retired[i] > upto) continue;
        s_eng_obj_retired[i] = 0;
        s_eng_obj[i] = nil;
        s_eng_obj_free[s_eng_obj_free_count++] = i;
    }
}

static void eng_obj_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle > s_eng_obj_count || !s_eng_obj[handle - 1] ||
        s_eng_obj_retired[handle - 1]) return;
    s_eng_obj_retired[handle - 1] = s_rec_seq;
    /* Any view cut from this object stops being valid with it. */
    for (u32 i = 0; i < s_eng_view_count; i++)
        if (s_eng_view[i].surface == handle) {
            eng_obj_release(NULL, s_eng_view[i].view);
            s_eng_view[i] = s_eng_view[--s_eng_view_count];
            i--;
        }
    eng_collect_retired_objects();
}

static MTLPixelFormat eng_pixel_format(rsx_be_format f)
{
    switch (f) {
    case RSX_BE_FMT_R8:              return MTLPixelFormatR8Unorm;
    case RSX_BE_FMT_R8G8:            return MTLPixelFormatRG8Unorm;
    case RSX_BE_FMT_R8G8B8A8:        return MTLPixelFormatRGBA8Unorm;
    case RSX_BE_FMT_BC1:             return MTLPixelFormatBC1_RGBA;
    case RSX_BE_FMT_BC2:             return MTLPixelFormatBC2_RGBA;
    case RSX_BE_FMT_BC3:             return MTLPixelFormatBC3_RGBA;
    case RSX_BE_FMT_R16:             return MTLPixelFormatR16Unorm;
    case RSX_BE_FMT_R16G16:          return MTLPixelFormatRG16Unorm;
    case RSX_BE_FMT_R16G16F:         return MTLPixelFormatRG16Float;
    case RSX_BE_FMT_R16G16B16A16F:   return MTLPixelFormatRGBA16Float;
    case RSX_BE_FMT_R32F:            return MTLPixelFormatR32Float;
    case RSX_BE_FMT_R32G32B32A32F:   return MTLPixelFormatRGBA32Float;
    default:                         return MTLPixelFormatR8Unorm;
    }
}

/* ---- staging ------------------------------------------------------------- */

static void eng_encode_and_commit(id<MTLTexture> present_dst);

/* `headroom`: bytes past the reservation that must also lie inside the slot
 * buffer, though they are not claimed -- for a binding whose declared size
 * reaches past the data actually written there (see
 * eng_bind_vs_constants_range). */
static int eng_stage_reserve_ex(u32 bytes, u32 headroom, u32* out_off)
{
    const u32 start = (s_eng_stage_used + ENG_ALIGN - 1u) & ~(ENG_ALIGN - 1u);
    if ((u64)start + bytes + headroom > ENG_STAGE_MAX) {
        /* Submit and wait rather than drop the rest of the frame; everything
         * already recorded has been encoded, so the arena is free again. */
        eng_encode_and_commit(nil);
        return eng_stage_reserve_ex(bytes, headroom, out_off);
    }
    if (s_eng_slot < 0) {                       /* first data of this submission: take a slot */
        const int k = (int)(s_eng_slot_next++ % ENG_STAGE_SLOTS);
        if (__atomic_load_n(&s_eng_slot_busy[k], __ATOMIC_ACQUIRE)) {
            if ((++s_eng_slot_waits & (s_eng_slot_waits - 1)) == 0)    /* 1, 2, 4, ... */
                fprintf(stderr, "[rsx engine/metal] staging slot still in flight: waited %lu time(s)\n",
                        s_eng_slot_waits);
            while (__atomic_load_n(&s_eng_slot_busy[k], __ATOMIC_ACQUIRE)) usleep(50);
        }
        s_eng_slot = k;
        s_eng_stage = s_eng_slot_buf[k] ? (u8*)[s_eng_slot_buf[k] contents] : NULL;
        s_eng_stage_cap = s_eng_slot_buf[k] ? (u32)[s_eng_slot_buf[k] length] : 0;
    }
    if (start + bytes + headroom > s_eng_stage_cap) {
        /* grow this slot: a new buffer, keeping what is already staged (the
         * old one is not in flight -- this submission has not been committed) */
        u32 cap = s_eng_stage_cap ? s_eng_stage_cap : (4u << 20);
        while (start + bytes + headroom > cap) cap *= 2u;
        id<MTLBuffer> nb = [s_dev newBufferWithLength:cap options:MTLResourceStorageModeShared];
        if (!nb) return 0;
        if (s_eng_stage_used) memcpy([nb contents], s_eng_stage, s_eng_stage_used);
        s_eng_slot_buf[s_eng_slot] = nb;
        s_eng_stage = (u8*)[nb contents];
        s_eng_stage_cap = cap;
    }
    *out_off = start;
    s_eng_stage_used = start + bytes;
    return 1;
}

static int eng_stage_reserve(u32 bytes, u32* out_off)
{
    return eng_stage_reserve_ex(bytes, 0, out_off);
}

static int eng_stage_copy(const void* src, u32 bytes, u32* out_off)
{
    if (!bytes) { *out_off = 0; return 1; }
    if (!eng_stage_reserve(bytes, out_off)) return 0;
    memcpy(s_eng_stage + *out_off, src, bytes);
    return 1;
}

/* ---- lifecycle ----------------------------------------------------------- */

static int eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    if (!s_dev) return -1;
    NSError* err = nil;
    s_eng_helper_lib = [s_dev newLibraryWithSource:kEngHelperMSL options:nil error:&err];
    if (!s_eng_helper_lib) {
        fprintf(stderr, "[rsx engine/metal] helper shader compile failed: %s\n",
                [[err localizedDescription] UTF8String]);
        return -1;
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_eng_point_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    /* FXAA's taps and a present that scales want bilinear. */
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    s_eng_linear_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    return s_eng_point_sampler && s_eng_linear_sampler ? 0 : -1;
}

static void eng_shutdown(void* user)
{
    (void)user;
    eng_enc_join();   /* PS3RECOMP_METAL_ENCODE_THREAD: a present may still be encoding */
    if (s_sc_queue) dispatch_sync(s_sc_queue, ^{});   /* PS3_SHADER_ASYNC builds in flight */
    for (u32 i = 0; i < s_eng_obj_count; i++) s_eng_obj[i] = nil;
    for (u32 i = 0; i < s_eng_pipe_count; i++) {
        s_eng_pipe[i].pso = nil;
        s_eng_pipe[i].ds = nil;
    }
    for (u32 i = 0; i < s_eng_func_count; i++) s_eng_func[i].fn = nil;
    for (u32 i = 0; i < s_eng_samp_count; i++) s_eng_samp[i].samp = nil;
    s_eng_obj_count = s_eng_obj_free_count = 0;
    memset(s_eng_obj_retired, 0, sizeof s_eng_obj_retired);
    s_eng_pipe_count = s_eng_func_count = s_eng_samp_count = s_eng_view_count = 0;
    memset(s_samp_memo, 0, sizeof s_samp_memo);
    s_eng_rec_count = 0;
    for (int k = 0; k < ENG_STAGE_SLOTS; k++) { s_eng_slot_buf[k] = nil; s_eng_slot_busy[k] = 0; }
    s_eng_slot = -1;
    s_eng_stage = NULL;
    s_eng_stage_used = s_eng_stage_cap = 0;
    s_eng_helper_lib = nil;
    s_eng_blit_pso = nil;
    s_eng_depth_pso = nil;
    s_eng_point_sampler = nil;
    s_eng_linear_sampler = nil;
    gfx_post_release();
    s_eng_active = 0;
}

static u32 s_ft_flush_reason;   /* PS3_FTRACE: why the commit in progress was asked for */
static void eng_submit_and_wait(void* user, u32 reason)
{
    (void)user;
    s_ft_flush_reason = reason;
    eng_encode_and_commit(nil);
}

/* ---- resources ----------------------------------------------------------- */

static u32 eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                              u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!s_dev || !w || !h) return 0;
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType      = (faces == 6) ? MTLTextureTypeCube : MTLTextureType2D;
    td.pixelFormat      = eng_pixel_format(fmt);
    td.width            = w;
    td.height           = h;
    td.mipmapLevelCount = mips ? mips : 1;
    td.usage            = MTLTextureUsageShaderRead;
    /* The crossbar's selectors arrive in its own A,R,G,B field order; destR is
     * out[1] .. destA is out[0], as everywhere else in this file. */
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    td.swizzle = MTLTextureSwizzleChannelsMake(
        swizzle_sel(sel[1]), swizzle_sel(sel[2]),
        swizzle_sel(sel[3]), swizzle_sel(sel[0]));
    return eng_obj_add([s_dev newTextureWithDescriptor:td]);
}

static void eng_texture_upload(void* user, u32 handle, u32 face, u32 mip,
                               u32 w, u32 h, const void* src, u32 row_bytes,
                               u32 rows)
{
    (void)user; (void)rows;
    id<MTLTexture> t = eng_obj(handle);
    if (!t || !src || !row_bytes) return;
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h)
         mipmapLevel:mip
               slice:face
           withBytes:src
         bytesPerRow:row_bytes
       bytesPerImage:0];
}

/* 3D textures: one MTLTextureType3D of `depth` slices, uploaded a slice at a
 * time (bytesPerImage spans one slice of decoded rows). */
static u32 eng_texture_create_3d(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 depth, u32 mips, u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!s_dev || !w || !h || !depth) return 0;
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType      = MTLTextureType3D;
    td.pixelFormat      = eng_pixel_format(fmt);
    td.width            = w;
    td.height           = h;
    td.depth            = depth;
    td.mipmapLevelCount = mips ? mips : 1;
    td.usage            = MTLTextureUsageShaderRead;
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    td.swizzle = MTLTextureSwizzleChannelsMake(
        swizzle_sel(sel[1]), swizzle_sel(sel[2]),
        swizzle_sel(sel[3]), swizzle_sel(sel[0]));
    return eng_obj_add([s_dev newTextureWithDescriptor:td]);
}

static void eng_texture_upload_3d(void* user, u32 handle, u32 z, u32 mip,
                                  u32 w, u32 h, const void* src, u32 row_bytes)
{
    (void)user;
    id<MTLTexture> t = eng_obj(handle);
    if (!t || !src || !row_bytes) return;
    [t replaceRegion:MTLRegionMake3D(0, 0, z, w, h, 1)
         mipmapLevel:mip
               slice:0
           withBytes:src
         bytesPerRow:row_bytes
       bytesPerImage:(NSUInteger)row_bytes * h];
}

static u32 eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                   const void* seed, u32 seed_row_bytes)
{
    (void)user;
    if (!s_dev || !w || !h) return 0;
    const u32 sw = res_px_x(w), sh = res_px(h);   /* PS3_RES_SCALE, PS3_ASPECT */
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:eng_pixel_format(fmt)
                                                           width:sw height:sh
                                                       mipmapped:NO];
    /* PixelFormatView because a unit sampling this target wears its own
     * TEXTURE_CONTROL1 crossbar as a view swizzle. */
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
               MTLTextureUsagePixelFormatView;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    if (!t) return 0;
    if (seed && seed_row_bytes && sw == w && sh == h)
        [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
               withBytes:seed bytesPerRow:seed_row_bytes];
    else if (seed && seed_row_bytes) {
        /* The guest's bytes, nearest-neighbour up to the scaled size. */
        const u32 bpp = seed_row_bytes / w;
        u8* up = (bpp && bpp <= 16) ? (u8*)malloc((size_t)sw * sh * bpp) : NULL;
        if (up) {
            for (u32 y = 0; y < sh; y++) {
                u32 gy = (u32)((float)y / s_res_sy); if (gy >= h) gy = h - 1;
                const u8* srow = (const u8*)seed + (size_t)gy * seed_row_bytes;
                u8* drow = up + (size_t)y * sw * bpp;
                for (u32 x = 0; x < sw; x++) {
                    u32 gx = (u32)((float)x / s_res_sx); if (gx >= w) gx = w - 1;
                    memcpy(drow + (size_t)x * bpp, srow + (size_t)gx * bpp, bpp);
                }
            }
            [t replaceRegion:MTLRegionMake2D(0, 0, sw, sh) mipmapLevel:0
                   withBytes:up bytesPerRow:(NSUInteger)sw * bpp];
            free(up);
        }
    }
    return eng_obj_add_scaled(t, 1);
}

static u32 eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    id<MTLTexture> t = eng_obj(surface);
    if (!t) return 0;
    for (u32 i = 0; i < s_eng_view_count; i++)
        if (s_eng_view[i].surface == surface && s_eng_view[i].remap == remap &&
            s_eng_view[i].format == rsx_format)
            return s_eng_view[i].view;
    if (s_eng_view_count >= ENG_MAX_VIEWS) return 0;
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_format & 0x9Fu, sel);
    id<MTLTexture> v =
        [t newTextureViewWithPixelFormat:[t pixelFormat]
                             textureType:MTLTextureType2D
                                  levels:NSMakeRange(0, 1)
                                  slices:NSMakeRange(0, 1)
                                 swizzle:MTLTextureSwizzleChannelsMake(
                                             swizzle_sel(sel[1]), swizzle_sel(sel[2]),
                                             swizzle_sel(sel[3]), swizzle_sel(sel[0]))];
    const u32 handle = eng_obj_add_scaled(v, eng_obj_is_scaled(surface));
    if (!handle) return 0;
    s_eng_view[s_eng_view_count].surface = surface;
    s_eng_view[s_eng_view_count].remap   = remap;
    s_eng_view[s_eng_view_count].format  = rsx_format;
    s_eng_view[s_eng_view_count].view    = handle;
    s_eng_view_count++;
    return handle;
}

static u32 eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!s_dev || !w || !h) return 0;
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTL_DEPTH_FORMAT
                                                           width:res_px_x(w) height:res_px(h)
                                                       mipmapped:NO];
    /* ShaderRead as well as RenderTarget: depth-as-texture resolves through a
     * pass that samples this as a depth2d, so it cannot be write-only.
     * PixelFormatView: the same resolve reads the stencil through an
     * X32_Stencil8 view of it. */
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    td.storageMode = MTLStorageModePrivate;
    return eng_obj_add_scaled([s_dev newTextureWithDescriptor:td], 1);
}

/* The sampleable copy of a depth target. The copy is a record in the stream,
 * not an immediate blit, because it has to observe the depth the passes
 * recorded so far actually wrote. */
static u32 eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    (void)user;
    if (!eng_obj(depth) || !s_eng_helper_lib) return 0;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return 0; }
    if (!s_eng_depth_pso) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_depth_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
        NSError* err = nil;
        s_eng_depth_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_eng_depth_pso) {
            fprintf(stderr, "[rsx engine/metal] depth resolve pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return 0;
        }
    }
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                           width:res_px_x(w) height:res_px(h)
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    const u32 dst = eng_obj_add_scaled([s_dev newTextureWithDescriptor:td], 1);
    if (!dst) return 0;

    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_DEPTH_RESOLVE;
    r->depth = depth;
    r->resolve_dst = dst;
    return dst;
}

/* The sampleable copy of a colour target, one persistent texture per source
 * surface, refreshed by a copy record wherever in the stream it is asked for:
 * the draw after it reads what the passes before it wrote. */
static struct { u32 src, dst; } s_eng_csnap[64];
static u32 s_eng_csnap_count;

static u32 eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    id<MTLTexture> src = eng_obj(surface);
    if (!src) return 0;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return 0; }
    u32 dst = 0;
    for (u32 i = 0; i < s_eng_csnap_count; i++)
        if (s_eng_csnap[i].src == surface) {
            id<MTLTexture> d = eng_obj(s_eng_csnap[i].dst);
            if (d && [d width] == [src width] && [d height] == [src height] &&
                [d pixelFormat] == [src pixelFormat])
                dst = s_eng_csnap[i].dst;
            else
                s_eng_csnap[i].src = 0;   /* stale: the surface was recreated */
            break;
        }
    if (!dst) {
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:[src pixelFormat]
                                                               width:[src width]
                                                              height:[src height]
                                                           mipmapped:NO];
        td.usage       = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        dst = eng_obj_add_scaled([s_dev newTextureWithDescriptor:td], eng_obj_is_scaled(surface));
        if (!dst) return 0;
        u32 slot = s_eng_csnap_count;
        for (u32 i = 0; i < s_eng_csnap_count; i++) if (!s_eng_csnap[i].src) { slot = i; break; }
        if (slot >= 64) return 0;
        if (slot == s_eng_csnap_count) s_eng_csnap_count++;
        s_eng_csnap[slot].src = surface;
        s_eng_csnap[slot].dst = dst;
    }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_COLOR_COPY;
    r->rt[0] = surface;
    r->resolve_dst = dst;
    return dst;
}

/* rsx_draw_backend.light_pass: one full-screen pass, two RGBA8 targets kept
 * across frames, parameters in a small ring of shared buffers (a frame's pass
 * may still be in flight when the next one is recorded). */
static id<MTLRenderPipelineState> s_eng_light_pso;
static id<MTLBuffer> s_eng_light_param[8];
static u32 s_eng_light_param_next;
static u32 s_eng_light_out[2], s_eng_light_w, s_eng_light_h;

static u32 eng_light_pass(void* user, u32 normals, u32 depth, u32 w, u32 h,
                          const void* params, u32 param_bytes, u32* spec_out)
{
    (void)user;
    if (!eng_obj(normals) || !eng_obj(depth) || !s_eng_helper_lib || !w || !h) return 0;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return 0; }
    if (!s_eng_light_pso) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_light_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
        NSError* err = nil;
        s_eng_light_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_eng_light_pso) {
            fprintf(stderr, "[rsx engine/metal] light pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return 0;
        }
    }
    if (!s_eng_light_out[0] || s_eng_light_w != w || s_eng_light_h != h) {
        for (int k = 0; k < 2; k++) {
            if (s_eng_light_out[k]) eng_obj_release(NULL, s_eng_light_out[k]);
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:res_px_x(w) height:res_px(h) mipmapped:NO];
            td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            td.storageMode = MTLStorageModePrivate;
            s_eng_light_out[k] = eng_obj_add_scaled([s_dev newTextureWithDescriptor:td], 1);
            if (!s_eng_light_out[k]) return 0;
        }
        s_eng_light_w = w; s_eng_light_h = h;
    }
    const u32 slot = s_eng_light_param_next++ % 8u;
    if (!s_eng_light_param[slot] || [s_eng_light_param[slot] length] < param_bytes)
        s_eng_light_param[slot] = [s_dev newBufferWithLength:param_bytes options:MTLResourceStorageModeShared];
    memcpy([s_eng_light_param[slot] contents], params, param_bytes);
    /* PS3_RES_SCALE: the pass runs per scaled pixel; u, v = px / size. */
    if (s_res_scaled && param_bytes >= sizeof(rsx_gpu_light_params)) {
        rsx_gpu_light_params* gp = (rsx_gpu_light_params*)[s_eng_light_param[slot] contents];
        gp->width  = (float)res_px_x(w);
        gp->height = (float)res_px(h);
    }

    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_LIGHT;
    r->rt[0] = s_eng_light_out[0];
    r->rt[1] = s_eng_light_out[1];
    r->tex[0] = normals;
    r->tex[1] = depth;
    r->clear_flags = slot;
    *spec_out = s_eng_light_out[1];
    return s_eng_light_out[0];
}

/* rsx_draw_backend.ssao_pass: five full-screen passes over persistent targets
 * (half depth R32F, two R8 AO planes, the RGBA8 output), constants in a ring
 * of shared buffers like the light pass. */
static id<MTLRenderPipelineState> s_eng_ssao_pso[6];   /* [5]: ssao_up4s_fs */
static id<MTLBuffer> s_eng_ssao_param[8];
static u32 s_eng_ssao_param_next;
static u32 s_eng_ssao_tex[4], s_eng_ssao_dims[5];   /* hd, aoA, aoB, out; dims + scaled out */
/* PS3_RES_SSAO_FULL (live, default 1): with PS3_RES_SCALE, the last pass (the
 * depth-aware upsample) writes an AO image of the scaled targets' size instead
 * of the job's 1280x720 one. 0: the 720p output, as before. */
static volatile int s_res_ssao_full = 1;
static void res_ssao_full_set(int id, int v) { (void)id; __atomic_store_n(&s_res_ssao_full, v, __ATOMIC_RELAXED); }

static id<MTLRenderPipelineState> eng_ssao_pso(const char* fn, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
    pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    pd.colorAttachments[0].pixelFormat = fmt;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) fprintf(stderr, "[rsx engine/metal] SSAO pipeline %s failed: %s\n", fn,
                    [[err localizedDescription] UTF8String]);
    return p;
}

static u32 eng_ssao_pass(void* user, u32 depth, u32 normals, const void* consts, u32 const_bytes,
                         u32 fw, u32 fh, u32 hw, u32 hh)
{
    (void)user;
    if (!eng_obj(depth) || !eng_obj(normals) || !s_eng_helper_lib || !fw || !fh || !hw || !hh) return 0;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return 0; }
    if (!s_eng_ssao_pso[0]) {
        static const char* fns[6] = { "ssao_hd_fs", "ssao_ao_fs", "ssao_blur_h_fs", "ssao_blur_v_fs", "ssao_up4_fs",
                                      "ssao_up4s_fs" };
        static const MTLPixelFormat fmts[6] = { MTLPixelFormatR32Float, MTLPixelFormatR8Unorm,
            MTLPixelFormatR8Unorm, MTLPixelFormatR8Unorm, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm };
        ps3_toggle_watch(PS3T_RES_SSAO_FULL, res_ssao_full_set);
        for (int k = 0; k < 6; k++) if (!(s_eng_ssao_pso[k] = eng_ssao_pso(fns[k], fmts[k]))) return 0;
    }
    /* PS3_RES_SCALE: the output at the scaled size (ssao_up4s_fs). */
    const u32 full = s_res_scaled && __atomic_load_n(&s_res_ssao_full, __ATOMIC_RELAXED);
    if (!s_eng_ssao_tex[0] || s_eng_ssao_dims[0] != fw || s_eng_ssao_dims[1] != fh ||
        s_eng_ssao_dims[2] != hw || s_eng_ssao_dims[3] != hh || s_eng_ssao_dims[4] != full) {
        static const MTLPixelFormat fmts[4] = { MTLPixelFormatR32Float, MTLPixelFormatR8Unorm,
                                                MTLPixelFormatR8Unorm, MTLPixelFormatRGBA8Unorm };
        for (int k = 0; k < 4; k++) {
            if (s_eng_ssao_tex[k]) eng_obj_release(NULL, s_eng_ssao_tex[k]);
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmts[k]
                                                                   width:(k == 3 ? (full ? res_px_x(fw) : fw) : hw)
                                                                  height:(k == 3 ? (full ? res_px(fh) : fh) : hh) mipmapped:NO];
            td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            td.storageMode = MTLStorageModePrivate;
            if (!(s_eng_ssao_tex[k] = eng_obj_add_scaled([s_dev newTextureWithDescriptor:td], k == 3 && full))) return 0;
        }
        s_eng_ssao_dims[0] = fw; s_eng_ssao_dims[1] = fh; s_eng_ssao_dims[2] = hw; s_eng_ssao_dims[3] = hh;
        s_eng_ssao_dims[4] = full;
    }
    const u32 slot = s_eng_ssao_param_next++ % 8u;
    if (!s_eng_ssao_param[slot] || [s_eng_ssao_param[slot] length] < const_bytes)
        s_eng_ssao_param[slot] = [s_dev newBufferWithLength:const_bytes options:MTLResourceStorageModeShared];
    memcpy([s_eng_ssao_param[slot] contents], consts, const_bytes);
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_SSAO;
    r->tex[0] = depth; r->tex[1] = normals;
    r->clear_flags = slot;
    return s_eng_ssao_tex[3];
}

static u32 eng_depth_snapshot_rgba8(void* user, u32 depth, u32 w, u32 h)
{
    (void)user;
    if (!eng_obj(depth) || !s_eng_helper_lib) return 0;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return 0; }
    if (!s_eng_depth_rgba_pso) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_depth_rgba8_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        NSError* err = nil;
        s_eng_depth_rgba_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_eng_depth_rgba_pso) {
            fprintf(stderr, "[rsx engine/metal] depth rgba8 pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return 0;
        }
    }
    /* One persistent decode target per depth target: each resolve record
     * re-renders it in stream order, so nothing leaks per frame. The engine
     * releases a copy it stops using (a crop of another size, a newer copy),
     * and a released handle's slot is handed to the next object created: an
     * entry is only reused while its handle still names the texture it made
     * and is not retired. Checked by handle alone, a crop pair alternating at
     * 0x0F770000 got back a slot that had become a 3D LUT, and the colour
     * view cut from it aborted (texture view 2D of a 3D texture). */
    static struct { u32 depth, dst, w, h; const void* obj; } cache[32];
    static u32 ncache;
    u32 dst = 0;
    for (u32 i = 0; i < ncache; i++)
        if (cache[i].depth == depth && cache[i].w == w && cache[i].h == h && cache[i].dst &&
            (__bridge const void*)eng_obj(cache[i].dst) == cache[i].obj && !s_eng_obj_retired[cache[i].dst - 1]) {
            dst = cache[i].dst; break; }
    if (!dst) {
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:res_px_x(w) height:res_px(h)
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        dst = eng_obj_add_scaled([s_dev newTextureWithDescriptor:td], 1);
        if (!dst) return 0;
        u32 slot = ncache;   /* a stale entry for the same key first, then a free one */
        for (u32 i = 0; i < ncache; i++) if (cache[i].depth == depth && cache[i].w == w && cache[i].h == h) { slot = i; break; }
        if (slot == ncache) slot = ncache < 32 ? ncache++ : (depth % 32);
        cache[slot].depth = depth; cache[slot].dst = dst; cache[slot].w = w; cache[slot].h = h;
        cache[slot].obj = (__bridge const void*)eng_obj(dst);
    }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_DEPTH_RESOLVE;
    r->depth = depth;
    r->resolve_dst = dst;
    r->clear_flags = 1;   /* rgba8 decode */
    return dst;
}

/* ---- persistent shader cache ---------------------------------------------
 * PS3_SHADER_CACHE=<dir>: keep the HLSL -> MSL translations across runs. A
 * guest pipeline's first draw decompiles both programs, translates each HLSL
 * to MSL (glslang + spirv-cross), compiles the MSL (newLibraryWithSource) and
 * builds the pipeline state, all on the RSX thread in the middle of a frame.
 * Measured in New Marais (2026-10-22, headless, 218 first uses in 114 s):
 * 2.14 s in all, frames of up to 366 ms; the translation was 83% of it, the
 * Metal library compile ~16%, the pipeline state under 1% (macOS keeps its own
 * cache of those) and the decompilers ~1%. So the translation is what is kept:
 * <dir>/msl-<build>/<key>.msl, keyed by a hash of the HLSL, the stage and the
 * translator's knobs. <build> is the executable's LC_UUID: a rebuild (a new
 * translator, glslang or spirv-cross) starts a fresh directory and removes the
 * others, so stale MSL is never served (a changed decompiler changes the HLSL,
 * which is the key anyway). Unset, empty or 0: off. Deleting <dir> is safe. */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dirent.h>
static int  s_sc_state = -1;              /* -1 unread, 0 off, 1 on */
static char s_sc_dir[1200];               /* <dir>/msl-<build> */
static u64  s_sc_salt;
static u32  s_sc_hits, s_sc_miss;

/* The main executable's LC_UUID as hex, or "nouuid". */
static void sc_build_id(char* out, size_t n)
{
    snprintf(out, n, "nouuid");
    const struct mach_header_64* h = (const struct mach_header_64*)_dyld_get_image_header(0);
    if (!h || h->magic != MH_MAGIC_64) return;
    const struct load_command* lc = (const struct load_command*)(h + 1);
    for (u32 i = 0; i < h->ncmds; i++) {
        if (lc->cmd == LC_UUID) {
            const u8* u = ((const struct uuid_command*)lc)->uuid;
            for (int k = 0; k < 16 && (size_t)(2 * k + 2) < n; k++) snprintf(out + 2 * k, 3, "%02x", u[k]);
            return;
        }
        lc = (const struct load_command*)((const u8*)lc + lc->cmdsize);
    }
}

static int sc_on(void)
{
    if (__builtin_expect(s_sc_state >= 0, 1)) return s_sc_state;
    s_sc_state = 0;
    const char* e = getenv("PS3_SHADER_CACHE");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    char build[40];
    sc_build_id(build, sizeof build);
    snprintf(s_sc_dir, sizeof s_sc_dir, "%s/msl-%s", e, build);
    if (![[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:s_sc_dir]
                                   withIntermediateDirectories:YES attributes:nil error:nil]) {
        fprintf(stderr, "[shader cache] cannot create %s; off\n", s_sc_dir);
        return 0;
    }
    /* Other builds' translations: never read again. */
    u32 removed = 0, kept = 0;
    DIR* d = opendir(e);
    if (d) {
        struct dirent* de;
        char mine[48];
        snprintf(mine, sizeof mine, "msl-%s", build);
        while ((de = readdir(d))) {
            if (strncmp(de->d_name, "msl-", 4) || !strcmp(de->d_name, mine)) continue;
            char old[1400];
            snprintf(old, sizeof old, "%s/%s", e, de->d_name);
            if ([[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:old] error:nil]) removed++;
        }
        closedir(d);
    }
    d = opendir(s_sc_dir);
    if (d) { struct dirent* de; while ((de = readdir(d))) if (strstr(de->d_name, ".msl")) kept++; closedir(d); }
    /* What the translation depends on besides the HLSL. */
    static const char tag[] = "ps3recomp-msl-1";
    s_sc_salt = fnv1a64(tag, sizeof tag, 1469598103934665603ull);
    const char* knobs[] = { "PS3RECOMP_FRAGCOORD_W", "PS3RECOMP_MSL_FAST" };
    for (unsigned i = 0; i < sizeof knobs / sizeof knobs[0]; i++) {
        const char* v = getenv(knobs[i]);
        s_sc_salt = fnv1a64(v ? v : "-", (u32)strlen(v ? v : "-"), s_sc_salt);
    }
    fprintf(stderr, "[shader cache] %s: %u translation(s) cached%s\n", s_sc_dir, kept,
            removed ? " (older builds' caches removed)" : "");
    s_sc_state = 1;
    return 1;
}

static void sc_msl_path(char* out, size_t n, u64 key)
{
    snprintf(out, n, "%s/%016llx.msl", s_sc_dir, (unsigned long long)key);
}

/* The cached translation into `out`, or 0. */
static int sc_msl_get(u64 key, char* out, size_t cap)
{
    char path[1300];
    sc_msl_path(path, sizeof path, key);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(out, 1, cap - 1, f);
    const int whole = feof(f) && !ferror(f);
    fclose(f);
    if (!whole || n == 0) return 0;
    out[n] = 0;
    return 1;
}

/* Written whole and renamed, so a crash or a second instance never leaves a
 * half file under the key. */
static void sc_msl_put(u64 key, const char* msl)
{
    char path[1300], tmp[1320];
    sc_msl_path(path, sizeof path, key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const size_t n = strlen(msl);
    const int ok = fwrite(msl, 1, n, f) == n;
    if (fclose(f) == 0 && ok) rename(tmp, path); else unlink(tmp);
}

/* PS3_SHADER_ASYNC=1: a new pipeline's shaders are translated, compiled and
 * built on a background queue instead of on the RSX thread. The pipeline's
 * handle exists at once; until its state is ready (`ready`) the draws that use
 * it are skipped, so a first sight costs missing objects for a frame or a few
 * instead of a frame of up to ~370 ms (RPCS3's "async shader compilation").
 * One serial queue: eng_function, s_msl/s_log and s_eng_func are then only
 * touched there. Off by default. */
static int s_sc_async = -1;
static int sc_async(void)
{
    if (__builtin_expect(s_sc_async >= 0, 1)) return s_sc_async;
    const char* e = getenv("PS3_SHADER_ASYNC");
    s_sc_async = (e && atoi(e) > 0) ? 1 : 0;
    if (s_sc_async) {
        s_sc_queue = dispatch_queue_create("ps3recomp.shaders",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
        fprintf(stderr, "[shader cache] PS3_SHADER_ASYNC: pipelines build in the background"
                        " (their draws are skipped until ready)\n");
    }
    return s_sc_async;
}

/* ---- pipelines ----------------------------------------------------------- */

static id<MTLFunction> eng_function(const char* hlsl, int stage,
                                    const char* what)
{
    const u64 hash = fnv1a64(hlsl, (u32)strlen(hlsl), 1469598103934665603ull);
    for (u32 i = 0; i < s_eng_func_count; i++)
        if (s_eng_func[i].hash == hash) return s_eng_func[i].fn;
    if (s_eng_func_count >= ENG_MAX_FUNCS) return nil;

    id<MTLFunction> fn = nil;
    const uint64_t ft_c0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    char name[64];
    snprintf(name, sizeof name, "%s_%016llx.hlsl", what, (unsigned long long)hash);
    dump_shader(name, hlsl);
    /* PS3_SHADER_CACHE: the translation from an earlier run. */
    const u64 sc_key = sc_on() ? fnv1a64(&stage, sizeof stage, fnv1a64(hlsl, (u32)strlen(hlsl), s_sc_salt)) : 0;
    int msl_rc;
    if (sc_key && sc_msl_get(sc_key, s_msl, sizeof s_msl)) {
        msl_rc = 0; s_sc_hits++;
    } else {
        msl_rc = rsx_hlsl_to_msl(hlsl, stage, s_msl, sizeof s_msl, s_log, sizeof s_log);
        if (sc_key && msl_rc == 0) { sc_msl_put(sc_key, s_msl); s_sc_miss++; }
    }
    if (ft_c0) ps3_ft_span(PS3_FT_COMPILE, 4, msl_rc == 0, 0, ft_c0, ps3_ft_now());
    if (msl_rc != 0) {
        fprintf(stderr, "[rsx engine/metal] %s %016llx: %s\n", what,
                (unsigned long long)hash, s_log);
    } else {
        snprintf(name, sizeof name, "%s_%016llx.msl", what, (unsigned long long)hash);
        dump_shader(name, s_msl);
        snprintf(name, sizeof name, "%s %016llx", what, (unsigned long long)hash);
        fn = compile_guest_function(s_msl, name);
    }
    if (ft_c0) ps3_ft_span(PS3_FT_COMPILE, stage == RSX_SHADER_STAGE_VERTEX ? 1u : 2u,
                           fn != nil, 0, ft_c0, ps3_ft_now());
    s_eng_func[s_eng_func_count].hash = hash;
    s_eng_func[s_eng_func_count].fn   = fn;
    s_eng_func_count++;
    return fn;
}

/* Engine pipelines take the viewport epilogue from a block of its own:
 * buffer 0 then holds only the 512 rows, which lets a draw stage just the
 * rows its program reads (eng_bind_vs_constants_range). The VPConst text is
 * the one the VP decompiler and the engine's fixed-function shader both
 * emit; anything else is refused rather than run against the wrong layout. */
static const char kEngVPConstJoined[] =
    "cbuffer VPConst : register(b0) {\n"
    "    float4 vp_c[512];\n"
    "    float4 vp_posscale;\n"
    "    float4 vp_posoffset;\n"
    "};\n";
static const char kEngVPConstSplit[] =
    "cbuffer VPConst : register(b0) {\n"
    "    float4 vp_c[512];\n"
    "};\n"
    "cbuffer VPXform : register(b2) {\n"
    "    float4 vp_posscale;\n"
    "    float4 vp_posoffset;\n"
    "};\n";

static char* eng_split_vpconst(const char* hlsl)
{
    const char* at = hlsl ? strstr(hlsl, kEngVPConstJoined) : NULL;
    if (!at) return NULL;
    const size_t head = (size_t)(at - hlsl), jl = sizeof kEngVPConstJoined - 1;
    const size_t sl = sizeof kEngVPConstSplit - 1, rest = strlen(at + jl);
    char* out = (char*)malloc(head + sl + rest + 1);
    if (!out) return NULL;
    memcpy(out, hlsl, head);
    memcpy(out + head, kEngVPConstSplit, sl);
    memcpy(out + head + sl, at + jl, rest + 1);
    return out;
}

static u32 eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                               const rsx_be_render_state* rs,
                               const rsx_vertex_layout_plan* layout,
                               u32 vertex_stride, rsx_be_format rt_fmt,
                               u32 rt_count)
{
    (void)user;
    if (s_dev && !s_guest_shaders) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[rsx engine/metal] pipeline_create: guest shaders are off (%s); "
                    "every guest draw will be dropped. Build with glslang+HLSL "
                    "(cmake -Dglslang_DIR=...) to enable the HLSL->MSL translator.\n",
                    rsx_hlsl_to_msl_available() ? "PS3RECOMP_METAL_FIXED_FUNCTION set"
                                                : "translator not built");
    }
    if (!s_dev || !s_guest_shaders || !vertex_stride) return 0;
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    if (s_eng_pipe_count >= ENG_MAX_PIPES) return 0;
    char* vs_split = eng_split_vpconst(vs_hlsl);
    if (!vs_split) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[rsx engine/metal] pipeline_create: vertex shader has no "
                    "VPConst block in the expected form; refusing it (the viewport "
                    "epilogue is bound separately)\n");
        return 0;
    }
    /* PS3_SHADER_ASYNC: the functions and the state are built on the queue. */
    const int async = sc_async();
    id<MTLFunction> vs = nil, fs = nil;
    char* ps_copy = NULL;
    if (!async) {
        vs = eng_function(vs_split, RSX_SHADER_STAGE_VERTEX, "vp");
        free(vs_split); vs_split = NULL;
        if (!vs) return 0;
        fs = eng_function(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, "fp");
        if (!fs) return 0;
    } else if (!(ps_copy = strdup(ps_hlsl))) {
        free(vs_split);
        return 0;
    }

    /* Attribute index is the layout SLOT, not the guest's ATTRn number.
     * glslang numbers an HLSL input struct's members in DECLARATION order --
     * a compact VSInput declaring ATTR0 and ATTR3 puts a3 at location 1 --
     * and spirv-cross carries those locations into [[attribute(n)]]. The two
     * agree for the all-sixteen layout, where slot is the attribute number,
     * which is why the vtable path above can index by either. */
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (u32 slot = 0; slot < layout->count; slot++) {
        vd.attributes[slot].format      = MTLVertexFormatFloat4;
        vd.attributes[slot].offset      = (NSUInteger)(slot * 16u);
        vd.attributes[slot].bufferIndex = MTL_VB_INDEX;
    }
    vd.layouts[MTL_VB_INDEX].stride = vertex_stride;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = vs;
    pd.fragmentFunction = fs;
    pd.vertexDescriptor = vd;
    pd.depthAttachmentPixelFormat   = MTL_DEPTH_FORMAT;
    pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    /* nv40 COLOR_MASK byte layout: B=[0:7] G=[8:15] R=[16:23] A=[24:31], any
     * nonzero byte turning that channel on. */
    MTLColorWriteMask wm = MTLColorWriteMaskNone;
    if ((rs->color_mask >>  0) & 0xFF) wm |= MTLColorWriteMaskBlue;
    if ((rs->color_mask >>  8) & 0xFF) wm |= MTLColorWriteMaskGreen;
    if ((rs->color_mask >> 16) & 0xFF) wm |= MTLColorWriteMaskRed;
    if ((rs->color_mask >> 24) & 0xFF) wm |= MTLColorWriteMaskAlpha;
    /* Target A takes COLOR_MASK and BLEND_ENABLE; targets B..D their own
     * nibble of COLOR_MASK_MRT (A,R,G,B at bits 0..3) and bit of
     * BLEND_ENABLE_MRT (RPCS3 color_mask_*(index), blend_enabled_surface_*).
     * Giving every target A's state was wrong both ways: inFAMOUS 2's light
     * volumes add into A with B and C masked off, and that added every light
     * into the G-buffer (the blown-white props and the rectangles round each
     * light); its material passes write B and C's alpha, which A's mask left
     * stale. The count is rt_count, the set the engine resolved, and the
     * fragment function's own output count is whatever its program exports;
     * Metal accepts either side being the larger one. */
    for (u32 r = 0; r < rt_count; r++) {
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[r];
        ca.pixelFormat = eng_pixel_format(rt_fmt);
        MTLColorWriteMask twm = wm;
        int tblend = rs->blend_enable != 0;
        /* PS3RECOMP_METAL_MRT_MASK: 1 (default) the hardware rule above;
         * 0 target A's state everywhere, the old behaviour; 2 only a target
         * the guest masks off entirely is off, with per-target blend. Mode 1
         * turned New Marais green only while the RSX did not wait for the
         * SPU light label (GCM_SEMA_ACQUIRE): the composite then multiplied
         * the normals copy by the gloss this rule lets the G-buffer write. */
        static int mrt_mode = -1;
        if (mrt_mode < 0) { const char* e = getenv("PS3RECOMP_METAL_MRT_MASK"); mrt_mode = e ? atoi(e) : 1; }
        if (r > 0 && mrt_mode != 0) {
            const u32 nib = (rs->color_mask_mrt >> (r * 4)) & 0xFu;
            if (mrt_mode == 1 || nib == 0) {
                twm = MTLColorWriteMaskNone;
                if (nib & 1u) twm |= MTLColorWriteMaskAlpha;
                if (nib & 2u) twm |= MTLColorWriteMaskRed;
                if (nib & 4u) twm |= MTLColorWriteMaskGreen;
                if (nib & 8u) twm |= MTLColorWriteMaskBlue;
            }
            tblend = (rs->blend_mrt >> r) & 1u;
        }
        ca.writeMask   = twm;
        if (tblend) {
            ca.blendingEnabled             = YES;
            ca.sourceRGBBlendFactor        = blend_factor_to_metal(rs->sf_rgb);
            ca.destinationRGBBlendFactor   = blend_factor_to_metal(rs->df_rgb);
            ca.sourceAlphaBlendFactor      = blend_factor_to_metal(rs->sf_a);
            ca.destinationAlphaBlendFactor = blend_factor_to_metal(rs->df_a);
            ca.rgbBlendOperation           = blend_equation_to_metal(rs->eq_rgb);
            ca.alphaBlendOperation         = blend_equation_to_metal(rs->eq_a);
        }
    }

    id<MTLRenderPipelineState> pso = nil;
    if (!async) {
        NSError* err = nil;
        const uint64_t ft_p0 = PS3_FT_ON() ? ps3_ft_now() : 0;
        pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (ft_p0) ps3_ft_span(PS3_FT_COMPILE, 3, pso != nil, 0, ft_p0, ps3_ft_now());
        if (!pso) {
            fprintf(stderr, "[rsx engine/metal] pipeline state failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return 0;
        }
    }

    /* Depth and stencil are pipeline state in D3D12 and an encoder object in
     * Metal, so the engine's render state splits here rather than upstream. */
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = rs->depth_test ? gcm_compare_to_metal(rs->depth_func)
                                             : MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = (rs->depth_test && rs->depth_write) ? YES : NO;
    if (rs->stencil_enable) {
        MTLStencilDescriptor* fsd = [MTLStencilDescriptor new];
        fsd.stencilCompareFunction    = gcm_compare_to_metal(rs->s_func);
        fsd.stencilFailureOperation   = gcm_stencil_op_to_metal(rs->s_fail);
        fsd.depthFailureOperation     = gcm_stencil_op_to_metal(rs->s_zfail);
        fsd.depthStencilPassOperation = gcm_stencil_op_to_metal(rs->s_zpass);
        fsd.readMask                  = rs->s_func_mask & 0xFFu;
        fsd.writeMask                 = rs->s_write_mask & 0xFFu;
        dd.frontFaceStencil = fsd;
        if (rs->stencil_two_sided) {
            MTLStencilDescriptor* bsd = [MTLStencilDescriptor new];
            bsd.stencilCompareFunction    = gcm_compare_to_metal(rs->bs_func);
            bsd.stencilFailureOperation   = gcm_stencil_op_to_metal(rs->bs_fail);
            bsd.depthFailureOperation     = gcm_stencil_op_to_metal(rs->bs_zfail);
            bsd.depthStencilPassOperation = gcm_stencil_op_to_metal(rs->bs_zpass);
            bsd.readMask                  = rs->s_func_mask & 0xFFu;
            bsd.writeMask                 = rs->s_write_mask & 0xFFu;
            dd.backFaceStencil = bsd;
        } else {
            /* Two-sided off: nv40 applies the front state to both faces. */
            dd.backFaceStencil = fsd;
        }
    }
    id<MTLDepthStencilState> ds = [s_dev newDepthStencilStateWithDescriptor:dd];
    if (!ds) { free(vs_split); free(ps_copy); return 0; }

    const u32 slot = s_eng_pipe_count++;
    s_eng_pipe[slot].pso = pso;
    s_eng_pipe[slot].ds  = ds;
    __atomic_store_n(&s_eng_pipe[slot].ready, async ? 0 : 1, __ATOMIC_RELEASE);
    /* CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408 (front here,
     * as in the D3D12 engine); FRONT_FACE CW=0x0900 CCW=0x0901. */
    s_eng_pipe[slot].cull = MTLCullModeNone;
    if (rs->cull_enable && rs->cull_face)
        s_eng_pipe[slot].cull = (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u)
                                    ? MTLCullModeFront
                                    : (rs->cull_face == 0x0405u ? MTLCullModeBack
                                                                : MTLCullModeNone);
    snprintf(s_eng_pipe[slot].desc, sizeof s_eng_pipe[slot].desc,
             "z=%d/%X/w%d st=%d/%X/m%X/%X,%X,%X bl=%d/%X,%X a%X,%X eq%X,%X cm=%X at=%d/%X cull=%d/%X",
             rs->depth_test, rs->depth_func, rs->depth_write, rs->stencil_enable, rs->s_func,
             rs->s_func_mask, rs->s_fail, rs->s_zfail, rs->s_zpass, rs->blend_enable,
             rs->sf_rgb, rs->df_rgb, rs->sf_a, rs->df_a, rs->eq_rgb, rs->eq_a,
             rs->color_mask, rs->alpha_test_enable, rs->alpha_func,
             rs->cull_enable, rs->cull_face);
    s_eng_pipe[slot].winding = (rs->front_face == 0x0901u)
                                   ? MTLWindingCounterClockwise
                                   : MTLWindingClockwise;
    if (async) {
        __atomic_add_fetch(&s_sc_async_pending, 1, __ATOMIC_RELAXED);
        dispatch_async(s_sc_queue, ^{
            id<MTLFunction> v = eng_function(vs_split, RSX_SHADER_STAGE_VERTEX, "vp");
            id<MTLFunction> f = v ? eng_function(ps_copy, RSX_SHADER_STAGE_FRAGMENT, "fp") : nil;
            free(vs_split); free(ps_copy);
            id<MTLRenderPipelineState> built = nil;
            if (v && f) {
                pd.vertexFunction = v; pd.fragmentFunction = f;
                NSError* e = nil;
                const uint64_t t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
                built = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
                if (t0) ps3_ft_span(PS3_FT_COMPILE, 3, built != nil, 0, t0, ps3_ft_now());
                if (!built) fprintf(stderr, "[rsx engine/metal] pipeline state failed: %s\n",
                                    [[e localizedDescription] UTF8String]);
            }
            /* A failed build stays not ready: its draws are dropped, as the
             * synchronous path drops a pipeline it could not build. */
            if (built) {
                s_eng_pipe[slot].pso = built;
                __atomic_store_n(&s_eng_pipe[slot].ready, 1, __ATOMIC_RELEASE);
            }
            __atomic_sub_fetch(&s_sc_async_pending, 1, __ATOMIC_RELAXED);
        });
    }
    return slot + 1;
}

static void eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    eng_enc_join();   /* PS3RECOMP_METAL_ENCODE_THREAD: a present may still be encoding */
    if (pipeline && pipeline <= s_eng_pipe_count) {
        s_eng_pipe[pipeline - 1].pso = nil;
        s_eng_pipe[pipeline - 1].ds  = nil;
    }
}

/* ---- per-draw binding ---------------------------------------------------- */

static void eng_samp_memo_set(int id, int v) { (void)id; __atomic_store_n(&s_samp_memo_on, v != 0, __ATOMIC_RELAXED); }
static int eng_sampler_slot(const rsx_be_sampler_desc* d)
{
    const u64 key0 = (u64)d->min_linear | ((u64)d->mag_linear << 1)
                  | ((u64)d->mip_linear << 2) | ((u64)d->mip_present << 3)
                  | ((u64)d->wrap_s << 4) | ((u64)d->wrap_t << 8)
                  | ((u64)d->wrap_r << 12)
                  | ((u64)(u32)(d->min_lod * 256.0f) << 16)
                  | ((u64)(u32)(d->max_lod * 256.0f) << 32)
                  | ((u64)(d->border & 3u) << 56);
    /* PS3_ANISO: only where the guest minifies linearly through mips. Part of
     * the key, so a level set live from PS3_TOGGLE_FILE gets its own samplers
     * (0, the default, leaves the key as it always was). */
    const int aniso = (d->min_linear && d->mip_present) ? __atomic_load_n(&s_gfx_aniso, __ATOMIC_RELAXED) : 0;
    const u64 key = key0 ^ ((u64)(aniso > 1 ? aniso : 0) << 59);
    /* PS3RECOMP_ENG_SAMP_INDEX (live): a direct-mapped memo of key -> slot in
     * front of the scan, which ran per bound unit per draw. 0 = the scan. */
    static int watched;
    if (__builtin_expect(!watched, 0)) { watched = 1; ps3_toggle_watch(PS3T_ENG_SAMP_INDEX, eng_samp_memo_set); }
    const u32 mi = (u32)((key * 0x9E3779B97F4A7C15ull) >> 56);   /* 256 entries */
    if (__atomic_load_n(&s_samp_memo_on, __ATOMIC_RELAXED) && s_samp_memo[mi].slot && s_samp_memo[mi].key == key) return (int)s_samp_memo[mi].slot - 1;
    for (u32 i = 0; i < s_eng_samp_count; i++)
        if (s_eng_samp[i].key == key) {
            s_samp_memo[mi].key = key; s_samp_memo[mi].slot = i + 1;
            return (int)i;
        }
    if (s_eng_samp_count >= ENG_MAX_SAMPLERS) return -1;
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = d->min_linear ? MTLSamplerMinMagFilterLinear
                                 : MTLSamplerMinMagFilterNearest;
    sd.magFilter = d->mag_linear ? MTLSamplerMinMagFilterLinear
                                 : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = !d->mip_present ? MTLSamplerMipFilterNotMipmapped
                 : d->mip_linear   ? MTLSamplerMipFilterLinear
                                   : MTLSamplerMipFilterNearest;
    sd.lodMinClamp = d->min_lod;
    sd.lodMaxClamp = d->max_lod;
    sd.sAddressMode = gcm_wrap_to_metal(d->wrap_s);
    sd.tAddressMode = gcm_wrap_to_metal(d->wrap_t);
    sd.rAddressMode = gcm_wrap_to_metal(d->wrap_r);
    if (d->border == 3) {   /* a border colour Metal cannot express: the edge is nearer than black */
        if (sd.sAddressMode == MTLSamplerAddressModeClampToBorderColor) sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        if (sd.tAddressMode == MTLSamplerAddressModeClampToBorderColor) sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        if (sd.rAddressMode == MTLSamplerAddressModeClampToBorderColor) sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    }
    /* SET_TEXTURE_BORDER_COLOR as far as Metal can say it (see
     * rsx_be_sampler_desc.border): a depth map's 1.0 border is opaque white. */
    sd.borderColor  = d->border == 2 ? MTLSamplerBorderColorOpaqueWhite
                    : d->border == 1 ? MTLSamplerBorderColorOpaqueBlack
                                     : MTLSamplerBorderColorTransparentBlack;
    if (aniso > 1) sd.maxAnisotropy = (NSUInteger)aniso;
    id<MTLSamplerState> s = [s_dev newSamplerStateWithDescriptor:sd];
    if (!s) return -1;
    const int slot = (int)s_eng_samp_count++;
    s_eng_samp[slot].key  = key;
    s_eng_samp[slot].samp = s;
    s_samp_memo[mi].key = key; s_samp_memo[mi].slot = (u32)slot + 1;
    return slot;
}

static void eng_bind_targets(void* user, const u32* surfaces, u32 count,
                             u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_pending.rt[i] = (i < count) ? surfaces[i] : 0;
    s_eng_pending.nrt   = count;
    s_eng_pending.depth = depth;
}

static void eng_bind_pipeline(void* user, u32 pipeline)
{ (void)user; s_eng_pending.pipeline = pipeline; }

/* The engine's VPConst block is 512 rows then the viewport epilogue at row
 * 512. Engine pipelines read the epilogue from its own VPXform block
 * (buffer 2, see eng_split_vpconst), so it travels by setVertexBytes and the
 * rows alone are staged. */
#define ENG_VP_ROWS_BYTES (512u * 16u)
#define ENG_VP_XF_INDEX   2

static void eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    memset(s_eng_pending.vs_xf, 0, sizeof s_eng_pending.vs_xf);
    if (data && bytes >= ENG_VP_ROWS_BYTES + sizeof s_eng_pending.vs_xf) {
        memcpy(s_eng_pending.vs_xf, (const u8*)data + ENG_VP_ROWS_BYTES,
               sizeof s_eng_pending.vs_xf);
        bytes = ENG_VP_ROWS_BYTES;
    }
    if (!eng_stage_copy(data, bytes, &s_eng_pending.vs_cb_off)) bytes = 0;
    s_eng_pending.vs_cb_bytes = bytes;
}

/* Stage only rows [first, first + count), densely, and bind buffer 0 at
 * `rows - 16 * first` so vp_c[first] lands on them. Rows below `first` and
 * above the range then alias whatever else is staged there, which this
 * program never reads.
 *
 * Writing the rows into an 8 KB-per-draw reservation instead -- the whole
 * block's footprint, mostly untouched -- cost more than the copy it saved:
 * every draw's few hundred bytes landed on fresh pages (T-0016). The bind
 * offset stays ENG_ALIGN-aligned, and the slot buffer is kept at least one
 * full block past it, so the binding never runs off the buffer's end. */
static void eng_bind_vs_constants_range(void* user, const void* rows, u32 first,
                                        u32 count, const void* tail,
                                        u32 tail_bytes, u32 total_bytes)
{
    (void)user;
    (void)total_bytes;
    memset(s_eng_pending.vs_xf, 0, sizeof s_eng_pending.vs_xf);
    s_eng_pending.vs_cb_bytes = 0;
    if (first > 512u || count > 512u - first ||
        tail_bytes > sizeof s_eng_pending.vs_xf)
        return;
    memcpy(s_eng_pending.vs_xf, tail, tail_bytes);

    /* A row offset that is congruent to 16*first mod ENG_ALIGN makes the base
     * aligned: base = rows_at - 16*first. */
    const u32 lead = (first * 16u) % ENG_ALIGN;
    const u32 bytes = lead + (count ? count * 16u : 16u);
    u32 at;
    if (!eng_stage_reserve_ex(bytes, ENG_VP_ROWS_BYTES, &at))
        return;
    const u32 rows_at = at + lead;
    if (rows_at < first * 16u) {
        /* Too near the start of the arena to reach back: stage the block
         * whole (rows only), as eng_bind_vs_constants does. */
        u32 off;
        if (!eng_stage_reserve(ENG_VP_ROWS_BYTES, &off)) return;
        memcpy(s_eng_stage + off, rows, ENG_VP_ROWS_BYTES);
        s_eng_pending.vs_cb_off = off;
        s_eng_pending.vs_cb_bytes = ENG_VP_ROWS_BYTES;
        return;
    }
    if (count)
        memcpy(s_eng_stage + rows_at, (const u8*)rows + first * 16u, count * 16u);
    s_eng_pending.vs_cb_off = rows_at - first * 16u;
    s_eng_pending.vs_cb_bytes = ENG_VP_ROWS_BYTES;
}

static void eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!eng_stage_copy(data, bytes, &s_eng_pending.ps_cb_off)) bytes = 0;
    s_eng_pending.ps_cb_bytes = bytes;
}

static void eng_bind_textures(void* user, const u32* textures,
                              const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        s_eng_pending.tex[u]  = ((mask >> u) & 1u) ? textures[u] : 0;
        s_eng_pending.samp[u] = ((mask >> u) & 1u) ? eng_sampler_slot(&samplers[u]) : -1;
    }
}

static void eng_bind_vertex_textures(void* user, const u32* textures,
                                     const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        s_eng_pending.vtex[u]  = ((mask >> u) & 1u) ? textures[u] : 0;
        s_eng_pending.vsamp[u] = ((mask >> u) & 1u) ? eng_sampler_slot(&samplers[u]) : -1;
    }
}

static void eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    const float kx = s_res_sx, ky = s_res_sy;   /* PS3_RES_SCALE, PS3_ASPECT */
    if (s_eng_hud_pin && s_aspect_k != 1.0f) {
        /* The HUD keeps the guest's 16:9: centred in the wide target, its
         * width only s_res_scale times the guest's. */
        const float full = (float)s_eng_hud_w * kx, k = s_res_scale;
        s_eng_pending.vp[0] = (full - (float)s_eng_hud_w * k) * 0.5f + x * k;
        s_eng_pending.vp[2] = w * k;
    } else { s_eng_pending.vp[0] = x * kx; s_eng_pending.vp[2] = w * kx; }
    s_eng_pending.vp[1] = y * ky; s_eng_pending.vp[3] = h * ky;
}

static void eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    /* PS3_RES_SCALE: both edges rounded the way res_px sizes the target, so
     * a scissor that reaches the surface's edge still ends on it. */
    u32 x0 = res_px_x(x), x1 = res_px_x(x + w);
    if (s_eng_hud_pin && s_aspect_k != 1.0f) {   /* PS3_ASPECT: the HUD's 16:9 rectangle */
        const u32 off = (res_px_x(s_eng_hud_w) - res_px(s_eng_hud_w)) / 2u;
        x0 = off + res_px(x); x1 = off + res_px(x + w);
    }
    const u32 y0 = res_px(y);
    s_eng_pending.sc[0] = x0; s_eng_pending.sc[1] = y0;
    s_eng_pending.sc[2] = w ? x1 - x0 : 0; s_eng_pending.sc[3] = h ? res_px(y + h) - y0 : 0;
}

static void eng_set_hud(void* user, int hud, u32 target_w)
{
    (void)user;
    if (hud >= 3) { s_eng_bars_next = hud == 3 && s_aspect_k != 1.0f; return; }
    s_eng_hud_pin = hud == 1 && s_aspect_k != 1.0f;
    if (target_w) s_eng_hud_w = target_w;
}

static void eng_set_stencil_ref(void* user, u32 ref)
{ (void)user; s_eng_pending.stencil_ref = ref; }
/* The polygon offset. Every engine depth target is Depth32Float, whose
 * constant-bias unit is 2^(e-23) for the primitive's largest depth exponent
 * e: the D24 unit (2^-24) for depths in [0.5, 1), smaller below. */
static void eng_set_depth_bias(void* user, float constant, float slope)
{ (void)user; s_eng_pending.zbias[0] = constant; s_eng_pending.zbias[1] = slope; }

static void eng_set_depth_clamp(void* user, int clamp)
{ (void)user; s_eng_pending.zclamp = clamp != 0; }

static void eng_draw(void* user, rsx_topology topology, const void* vertices,
                     u32 vertex_count, u32 stride, const u32* indices,
                     u32 index_count)
{
    (void)user;
    if (!vertex_count || !stride) return;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    u32 vb_off = 0, ib_off = 0;
    if (!eng_stage_copy(vertices, vertex_count * stride, &vb_off)) return;
    if (indices && index_count &&
        !eng_stage_copy(indices, index_count * (u32)sizeof(u32), &ib_off))
        return;

    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    *r = s_eng_pending;
    r->kind         = ENG_REC_DRAW;
    r->topology     = topo_to_metal(topology);
    r->vb_off       = vb_off;
    r->vb_buf       = 0;
    r->stride       = stride;
    r->vertex_count = vertex_count;
    r->ib_off       = ib_off;
    r->ib_buf       = 0;
    r->index_count  = indices ? index_count : 0;
}

static u32 eng_vbuf_create(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_dev || !data || !bytes) return 0;
    u32 slot;
    if (s_vbuf_free_count) slot = s_vbuf_free[--s_vbuf_free_count];
    else { if (s_vbuf_count >= ENG_MAX_VBUFS) return 0; slot = s_vbuf_count++; }
    id<MTLBuffer> b = [s_dev newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
    if (!b) { s_vbuf_free[s_vbuf_free_count++] = slot; return 0; }
    s_vbuf[slot] = b;
    return slot + 1;
}

static void eng_vbuf_release(void* user, u32 vbuf)
{
    (void)user;
    if (!vbuf || vbuf > s_vbuf_count || !s_vbuf[vbuf - 1] || s_vbuf_retired[vbuf - 1]) return;
    s_vbuf_retired[vbuf - 1] = s_rec_seq;
    eng_collect_retired_objects();
}

static void eng_draw_vbuf(void* user, rsx_topology topology, u32 vbuf,
                          u32 vertex_count, u32 stride, const u32* indices,
                          u32 index_count)
{
    (void)user;
    if (!vertex_count || !stride || !vbuf || vbuf > s_vbuf_count || !s_vbuf[vbuf - 1]) return;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    u32 ib_off = 0;
    if (indices && index_count &&
        !eng_stage_copy(indices, index_count * (u32)sizeof(u32), &ib_off))
        return;
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    *r = s_eng_pending;
    r->kind         = ENG_REC_DRAW;
    r->topology     = topo_to_metal(topology);
    r->vb_off       = 0;
    r->vb_buf       = vbuf;
    r->stride       = stride;
    r->vertex_count = vertex_count;
    r->ib_off       = ib_off;
    r->ib_buf       = 0;
    r->index_count  = indices ? index_count : 0;
}

/* draw_vbuf with the indices in a persistent buffer too: nothing staged. */
static void eng_draw_vbuf_ib(void* user, rsx_topology topology, u32 vbuf,
                             u32 vertex_count, u32 stride, u32 ibuf, u32 index_count)
{
    (void)user;
    if (!vertex_count || !stride || !vbuf || vbuf > s_vbuf_count || !s_vbuf[vbuf - 1]) return;
    if (!ibuf || ibuf > s_vbuf_count || !s_vbuf[ibuf - 1] || !index_count) return;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    *r = s_eng_pending;
    r->kind         = ENG_REC_DRAW;
    r->topology     = topo_to_metal(topology);
    r->vb_off       = 0;
    r->vb_buf       = vbuf;
    r->stride       = stride;
    r->vertex_count = vertex_count;
    r->ib_off       = 0;
    r->ib_buf       = ibuf;
    r->index_count  = index_count;
}

static void eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour the guest asked for whether or
     * not the record stream had room for the clear. */
    s_clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                   ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                   ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                    (u32)(rgba[2] * 255.0f + 0.5f);
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind  = ENG_REC_CLEAR_COLOR;
    r->rt[0] = surface;
    r->nrt   = 1;
    memcpy(r->clear_rgba, rgba, sizeof r->clear_rgba);
}

static void eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                    float depth_value, u8 stencil)
{
    (void)user;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_CLEAR_DS;
    r->depth = depth;
    r->clear_flags = flags;
    r->clear_depth = depth_value;
    r->clear_stencil = stencil;
}

/* ---- replay -------------------------------------------------------------- */

/* What a render encoder already has bound, so a draw sets only what differs
 * from the draw before it. Every draw used to rebind all of its state --
 * about fifty encoder calls, sixteen textures and sixteen samplers among them
 * -- which at ~13k draws a frame made encoding the frame's largest CPU cost
 * (T-0016). Reset for every new encoder: a fresh encoder has nothing bound.
 * Objects are compared by address only; the records and caches keep them
 * alive. */
typedef struct {
    const void* pso; const void* ds;
    int cull, winding;
    u32 stencil_ref;
    int have_zbias; float zbias[2];
    int zclamp;                 /* 0 = Clip, a fresh encoder's mode */
    int have_vp, have_sc;
    MTLViewport vp;
    MTLScissorRect sc;
    const void* buf[2]; NSUInteger buf_off[2];   /* [0] MTL_VB_INDEX, [1] index 0 */
    const void* fbuf; NSUInteger fbuf_off;
    int have_xf; float xf[8];
    const void* ftex[RSX_BE_MAX_TEXTURES]; const void* fsamp[RSX_BE_MAX_TEXTURES];
    const void* vtex[RSX_BE_MAX_VERTEX_TEXTURES]; const void* vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
} EngBound;
static EngBound s_eng_bound;
static void eng_bound_reset(void)
{
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    s_eng_bound.cull = s_eng_bound.winding = -1;
    s_eng_bound.stencil_ref = 0xFFFFFFFFu;
}
#define ENG_PTR(o) ((__bridge const void*)(o))

/* PS3RECOMP_METAL_DBG_DUMPTEX=<pipe>:<unit>:<sec>:<path>: once, at the first
 * replay of that pipeline <sec> seconds after its first use, keep the texture
 * bound on <unit>; eng_present then reads it back and writes <path>.ppm (RGB)
 * and <path>.a.ppm (alpha as grey) -- the bytes a pass really samples, not
 * what its shader makes of them. 8-bit RGBA/BGRA textures only. */
static id<MTLTexture> s_dumptex;
static char s_dumptex_path[512];
extern unsigned long long ps3_ms_now(void);
static void eng_dumptex_write(id<MTLTexture> t, const char* path)
{
    eng_enc_join();   /* PS3RECOMP_METAL_ENCODE_THREAD: a present may still be encoding */
    const NSUInteger w = [t width], h = [t height];
    const MTLPixelFormat pf = [t pixelFormat];
    const int bgra = pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB;
    const int r16 = pf == MTLPixelFormatR16Unorm || pf == MTLPixelFormatR16Float || pf == MTLPixelFormatR16Snorm;
    const int r8  = pf == MTLPixelFormatR8Unorm || pf == MTLPixelFormatR8Unorm_sRGB || pf == MTLPixelFormatR8Snorm;
    const int rg16 = pf == MTLPixelFormatRG16Unorm || pf == MTLPixelFormatRG16Float || pf == MTLPixelFormatRG16Snorm;
    /* Block-compressed (DXT1/3/5 = BC1/2/3): the raw blocks go to <path>.bc.bin
     * (8 or 16 bytes per 4x4 block, row-major), to decode offline. */
    const NSUInteger bcb = (pf == MTLPixelFormatBC1_RGBA || pf == MTLPixelFormatBC1_RGBA_sRGB) ? 8
                         : (pf == MTLPixelFormatBC2_RGBA || pf == MTLPixelFormatBC2_RGBA_sRGB ||
                            pf == MTLPixelFormatBC3_RGBA || pf == MTLPixelFormatBC3_RGBA_sRGB) ? 16 : 0;
    if (bcb) {
        const NSUInteger bw = (w + 3) / 4, bh = (h + 3) / 4;
        id<MTLBuffer> buf = [s_dev newBufferWithLength:bw * bh * bcb options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
         destinationBytesPerRow:bw * bcb destinationBytesPerImage:bw * bh * bcb];
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        char name[600]; snprintf(name, sizeof name, "%s.bc.bin", path);
        FILE* fb = fopen(name, "wb");
        if (fb) { fwrite([buf contents], 1, bw * bh * bcb, fb); fclose(fb); }
        fprintf(stderr, "[rsx engine/metal] dumptex: %lux%lu fmt %lu (BC%lu raw blocks) -> %s\n",
                (unsigned long)w, (unsigned long)h, (unsigned long)pf, (unsigned long)(bcb == 8 ? 1 : 3), name);
        return;
    }
    if (!bgra && !r16 && !r8 && !rg16 && pf != MTLPixelFormatRGBA8Unorm && pf != MTLPixelFormatRGBA8Unorm_sRGB) {
        fprintf(stderr, "[rsx engine/metal] dumptex: %lux%lu pixel format %lu is not 8-bit RGBA, RG16, R16 or R8, skipped\n",
                (unsigned long)w, (unsigned long)h, (unsigned long)pf);
        return;
    }
    const NSUInteger bpp = r16 ? 2 : r8 ? 1 : 4;
    id<MTLBuffer> buf = [s_dev newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
     destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const u8* p = (const u8*)[buf contents];
    /* R16: the high byte as grey in both files (a unorm ramp reads as 0..255). */
    u8* expanded = NULL;
    if (r16 || r8) {
        expanded = (u8*)malloc(w * h * 4);
        if (!expanded) return;
        for (NSUInteger i = 0; i < w * h; i++) {
            const u8 hi = r16 ? p[i * 2 + 1] : p[i];
            expanded[i * 4] = expanded[i * 4 + 1] = expanded[i * 4 + 2] = expanded[i * 4 + 3] = hi;
        }
        p = expanded;
    }
    /* RG16 (Y16_X16 wave/normal maps): the raw little-endian pairs go to
     * <path>.rg16.bin for a histogram; the .ppm shows the high bytes as R, G. */
    if (rg16) {
        char bn[600]; snprintf(bn, sizeof bn, "%s.rg16.bin", path);
        FILE* fb = fopen(bn, "wb");
        if (fb) { fwrite(p, 1, w * h * 4, fb); fclose(fb); }
        expanded = (u8*)malloc(w * h * 4);
        if (!expanded) return;
        for (NSUInteger i = 0; i < w * h; i++) {
            expanded[i * 4] = p[i * 4 + 1]; expanded[i * 4 + 1] = p[i * 4 + 3];
            expanded[i * 4 + 2] = 0; expanded[i * 4 + 3] = 255;
        }
        p = expanded;
    }
    char name[600];
    snprintf(name, sizeof name, "%s.ppm", path);
    FILE* f = fopen(name, "wb");
    snprintf(name, sizeof name, "%s.a.ppm", path);
    FILE* fa = fopen(name, "wb");
    if (f)  fprintf(f,  "P6 %lu %lu 255\n", (unsigned long)w, (unsigned long)h);
    if (fa) fprintf(fa, "P6 %lu %lu 255\n", (unsigned long)w, (unsigned long)h);
    for (NSUInteger i = 0; i < w * h; i++) {
        const u8* q = p + i * 4;
        const u8 rgb[3] = { bgra ? q[2] : q[0], q[1], bgra ? q[0] : q[2] };
        const u8 aaa[3] = { q[3], q[3], q[3] };
        if (f)  fwrite(rgb, 1, 3, f);
        if (fa) fwrite(aaa, 1, 3, fa);
    }
    if (f)  fclose(f);
    if (fa) fclose(fa);
    free(expanded);
    fprintf(stderr, "[rsx engine/metal] dumptex: %lux%lu fmt %lu -> %s.ppm and .a.ppm\n",
            (unsigned long)w, (unsigned long)h, (unsigned long)pf, path);
}

static int s_lean_encode = 1;
static void eng_lean_encode_set(int id, int v) { (void)id; __atomic_store_n(&s_lean_encode, v != 0, __ATOMIC_RELAXED); }
static void eng_encode_draw(id<MTLRenderCommandEncoder> enc, const EngRecord* r,
                            id<MTLBuffer> stage)
{
    { static int watched; if (__builtin_expect(!watched, 0)) { watched = 1; ps3_toggle_watch(PS3T_METAL_LEAN_ENCODE, eng_lean_encode_set); } }
    if (!r->pipeline || r->pipeline > s_eng_pipe_count) return;
    { static long dp = -2, du = 0, dsec = 0; static char dpath[512]; static int done;
      if (dp == -2) { const char* e = getenv("PS3RECOMP_METAL_DBG_DUMPTEX"); dp = -1;
          if (e) { char* d; dp = strtol(e, &d, 10);
                   if (*d == ':') du = strtol(d + 1, &d, 10);
                   if (*d == ':') dsec = strtol(d + 1, &d, 10);
                   if (*d == ':') snprintf(dpath, sizeof dpath, "%s", d + 1); } }
      if (dp >= 0 && !done && (long)r->pipeline == dp && du < RSX_BE_MAX_TEXTURES) {
          static unsigned long long t0; if (!t0) t0 = ps3_ms_now();
          /* PS3RECOMP_METAL_DBG_DUMPTEX_SKIP=<n>: pass over n draws of the
           * pipeline first -- a UI pipeline draws several quads a frame with
           * different textures, and only the first was ever reachable. */
          static long dskip = -1; if (dskip < 0) { const char* e = getenv("PS3RECOMP_METAL_DBG_DUMPTEX_SKIP"); dskip = e ? strtol(e, 0, 10) : 0; }
          if (ps3_ms_now() - t0 >= (unsigned long long)dsec * 1000ull) {
              if (dskip > 0) { dskip--; }
              else {
                  id<MTLTexture> t = eng_obj(r->tex[du]);
                  if (t) { s_dumptex = t; snprintf(s_dumptex_path, sizeof s_dumptex_path, "%s", dpath); done = 1; }
              }
          } } }
    /* PS3RECOMP_METAL_SKIP_PIPE=a,b,...: drop draws using those pipelines
     * (pass-log p= numbers) to bisect which pass breaks an image. */
    { static u32 skip[16]; static int nskip = -1;
      if (nskip < 0) { nskip = 0; const char* e = getenv("PS3RECOMP_METAL_SKIP_PIPE");
          while (e && *e && nskip < 16) { skip[nskip++] = (u32)strtoul(e, (char**)&e, 10); if (*e == ',') e++; else break; } }
      for (int k = 0; k < nskip; k++) if (skip[k] == r->pipeline) return; }
    const EngPipeline* p = &s_eng_pipe[r->pipeline - 1];
    if (!__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) { s_sc_async_skipped++; return; }
    if (!p->pso) return;
    EngBound* b = &s_eng_bound;
    if (b->pso != ENG_PTR(p->pso)) { [enc setRenderPipelineState:p->pso]; b->pso = ENG_PTR(p->pso); }
    /* PS3RECOMP_METAL_DBG_NOZ=<handle>: draws into that colour target skip
     * the depth/stencil test and cull (isolates a pass clipped by bad depth). */
    static long noz = -2;
    if (noz == -2) { const char* e = getenv("PS3RECOMP_METAL_DBG_NOZ"); noz = e ? strtol(e, 0, 0) : -1; }
    if (noz >= 0 && r->nrt && r->rt[0] == (u32)noz) {
        static id<MTLDepthStencilState> off;
        if (!off) off = [s_dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        [enc setDepthStencilState:off];
        [enc setCullMode:MTLCullModeNone];
        b->ds = ENG_PTR(off); b->cull = (int)MTLCullModeNone;
    } else {
        if (b->ds != ENG_PTR(p->ds)) { [enc setDepthStencilState:p->ds]; b->ds = ENG_PTR(p->ds); }
        if (b->cull != (int)p->cull) { [enc setCullMode:p->cull]; b->cull = (int)p->cull; }
    }
    if (b->winding != (int)p->winding) { [enc setFrontFacingWinding:p->winding]; b->winding = (int)p->winding; }
    if (b->stencil_ref != r->stencil_ref) { [enc setStencilReferenceValue:r->stencil_ref]; b->stencil_ref = r->stencil_ref; }
    if (!b->have_zbias || b->zbias[0] != r->zbias[0] || b->zbias[1] != r->zbias[1]) {
        [enc setDepthBias:r->zbias[0] slopeScale:r->zbias[1] clamp:0.0f];
        b->zbias[0] = r->zbias[0]; b->zbias[1] = r->zbias[1]; b->have_zbias = 1;
    }
    if (b->zclamp != r->zclamp) {
        [enc setDepthClipMode:r->zclamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        b->zclamp = r->zclamp;
    }
    MTLViewport vp = { r->vp[0], r->vp[1], r->vp[2], r->vp[3], 0.0, 1.0 };
    if (!b->have_vp || memcmp(&b->vp, &vp, sizeof vp)) { [enc setViewport:vp]; b->vp = vp; b->have_vp = 1; }
    if (r->sc[2] && r->sc[3]) {
        const MTLScissorRect sc = { r->sc[0], r->sc[1], r->sc[2], r->sc[3] };
        if (!b->have_sc || memcmp(&b->sc, &sc, sizeof sc)) { [enc setScissorRect:sc]; b->sc = sc; b->have_sc = 1; }
    }
    /* Same buffer, new offset: setVertexBufferOffset is the cheap form. */
    if (r->vb_buf && r->vb_buf <= s_vbuf_count && s_vbuf[r->vb_buf - 1]) {
        id<MTLBuffer> vb = s_vbuf[r->vb_buf - 1];
        if (b->buf[0] != ENG_PTR(vb)) {
            [enc setVertexBuffer:vb offset:0 atIndex:MTL_VB_INDEX];
            b->buf[0] = ENG_PTR(vb); b->buf_off[0] = 0;
        } else if (b->buf_off[0] != 0) {
            [enc setVertexBufferOffset:0 atIndex:MTL_VB_INDEX]; b->buf_off[0] = 0;
        }
    } else if (b->buf[0] != ENG_PTR(stage)) {
        [enc setVertexBuffer:stage offset:r->vb_off atIndex:MTL_VB_INDEX];
        b->buf[0] = ENG_PTR(stage); b->buf_off[0] = r->vb_off;
    } else if (b->buf_off[0] != r->vb_off) {
        [enc setVertexBufferOffset:r->vb_off atIndex:MTL_VB_INDEX]; b->buf_off[0] = r->vb_off;
    }
    if (r->vs_cb_bytes) {
        if (b->buf[1] != ENG_PTR(stage)) {
            [enc setVertexBuffer:stage offset:r->vs_cb_off atIndex:0];
            b->buf[1] = ENG_PTR(stage); b->buf_off[1] = r->vs_cb_off;
        } else if (b->buf_off[1] != r->vs_cb_off) {
            [enc setVertexBufferOffset:r->vs_cb_off atIndex:0]; b->buf_off[1] = r->vs_cb_off;
        }
    }
    if (!b->have_xf || memcmp(b->xf, r->vs_xf, sizeof b->xf)) {
        [enc setVertexBytes:r->vs_xf length:sizeof r->vs_xf atIndex:ENG_VP_XF_INDEX];
        memcpy(b->xf, r->vs_xf, sizeof b->xf); b->have_xf = 1;
    }
    if (r->ps_cb_bytes) {
        if (b->fbuf != ENG_PTR(stage)) {
            [enc setFragmentBuffer:stage offset:r->ps_cb_off atIndex:1];
            b->fbuf = ENG_PTR(stage); b->fbuf_off = r->ps_cb_off;
        } else if (b->fbuf_off != r->ps_cb_off) {
            [enc setFragmentBufferOffset:r->ps_cb_off atIndex:1]; b->fbuf_off = r->ps_cb_off;
        }
    }
    /* PS3RECOMP_METAL_TINT_PIPE=a,b,...: DIAGNOSTIC. Every 2D texture those
     * pipelines (pass-log p= numbers) sample becomes a solid colour, one per
     * list position (magenta, yellow, cyan, green, blue, red, azure, white),
     * so what each pipeline covers on screen is visible in one run --
     * SKIP_PIPE can only show what disappears, and a pass over a black clear
     * shows nothing. Cube units keep their texture (a 2D one is invalid there). */
    static u32 tint_pipe[8]; static int ntint = -1;
    static id<MTLTexture> tint_tex[8];
    if (ntint < 0) {
        ntint = 0;
        const char* e = getenv("PS3RECOMP_METAL_TINT_PIPE");
        static const u32 pal[8] = { 0xFFFF00FFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFF00FF00u,
                                    0xFF0000FFu, 0xFFFF0000u, 0xFF0080FFu, 0xFFFFFFFFu };
        while (e && *e && ntint < 8) {
            tint_pipe[ntint] = (u32)strtoul(e, (char**)&e, 10);
            MTLTextureDescriptor* td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            tint_tex[ntint] = [s_dev newTextureWithDescriptor:td];
            [tint_tex[ntint] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                                 withBytes:&pal[ntint] bytesPerRow:4];
            ntint++;
            if (*e == ',') e++; else break;
        }
    }
    id<MTLTexture> tint = nil;
    for (int k = 0; k < ntint; k++) if (tint_pipe[k] == r->pipeline) tint = tint_tex[k];
    /* PS3RECOMP_METAL_LEAN_ENCODE (live): the bound textures and samplers
     * compared and set through unretained locals. Each strong local below cost
     * an objc_retain/objc_release pair -- 20 units, ~9000 draws a New Marais
     * frame; the objects are alive in s_eng_obj / s_eng_samp throughout. Only
     * without PS3RECOMP_METAL_TINT_PIPE. 0 = the strong locals. */
    if (__atomic_load_n(&s_lean_encode, __ATOMIC_RELAXED) && !tint) {
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            const u32 h = r->tex[u];
            __unsafe_unretained id<MTLTexture> t = (h && h <= s_eng_obj_count) ? s_eng_obj[h - 1] : nil;
            if (!t) t = s_null_tex;
            __unsafe_unretained id<MTLSamplerState> sm = r->samp[u] >= 0 ? s_eng_samp[r->samp[u]].samp : s_default_sampler;
            if (b->ftex[u] != ENG_PTR(t)) { [enc setFragmentTexture:t atIndex:(NSUInteger)u]; b->ftex[u] = ENG_PTR(t); }
            if (b->fsamp[u] != ENG_PTR(sm)) { [enc setFragmentSamplerState:sm atIndex:(NSUInteger)u]; b->fsamp[u] = ENG_PTR(sm); }
        }
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
            const u32 h = r->vtex[u];
            __unsafe_unretained id<MTLTexture> t = (h && h <= s_eng_obj_count) ? s_eng_obj[h - 1] : nil;
            if (!t) t = s_null_tex;
            __unsafe_unretained id<MTLSamplerState> sm = r->vsamp[u] >= 0 ? s_eng_samp[r->vsamp[u]].samp : s_default_sampler;
            if (b->vtex[u] != ENG_PTR(t)) { [enc setVertexTexture:t atIndex:MTL_VTEX_INDEX(u)]; b->vtex[u] = ENG_PTR(t); }
            if (b->vsamp[u] != ENG_PTR(sm)) { [enc setVertexSamplerState:sm atIndex:MTL_VSAMP_INDEX(u)]; b->vsamp[u] = ENG_PTR(sm); }
        }
    } else {
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        id<MTLTexture> t = eng_obj(r->tex[u]);
        if (!t) t = s_null_tex;
        if (tint && [t textureType] == MTLTextureType2D) t = tint;
        id<MTLSamplerState> sm = r->samp[u] >= 0 ? s_eng_samp[r->samp[u]].samp : s_default_sampler;
        if (b->ftex[u] != ENG_PTR(t)) { [enc setFragmentTexture:t atIndex:(NSUInteger)u]; b->ftex[u] = ENG_PTR(t); }
        if (b->fsamp[u] != ENG_PTR(sm)) { [enc setFragmentSamplerState:sm atIndex:(NSUInteger)u]; b->fsamp[u] = ENG_PTR(sm); }
    }
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        id<MTLTexture> t = eng_obj(r->vtex[u]);
        if (!t) t = s_null_tex;
        id<MTLSamplerState> sm = r->vsamp[u] >= 0 ? s_eng_samp[r->vsamp[u]].samp : s_default_sampler;
        if (b->vtex[u] != ENG_PTR(t)) { [enc setVertexTexture:t atIndex:MTL_VTEX_INDEX(u)]; b->vtex[u] = ENG_PTR(t); }
        if (b->vsamp[u] != ENG_PTR(sm)) { [enc setVertexSamplerState:sm atIndex:MTL_VSAMP_INDEX(u)]; b->vsamp[u] = ENG_PTR(sm); }
    }
    }
    /* An indexed draw is really indexed here: the index buffer is what keeps a
     * strip's shared vertices from being fetched and uploaded per triangle.
     * Only a rebuilt triangle list is ever indexed; points and lines arrive as
     * the guest issued them. */
    /* PS3RECOMP_METAL_DBG_SHOWTEX=<pipe>:<unit>: draw that pipeline's records
     * as a full-screen copy of the texture bound on <unit> -- shows exactly
     * what a pass samples, independent of its shader. */
    { static long dp = -2, du = 0;
      if (dp == -2) { const char* e = getenv("PS3RECOMP_METAL_DBG_SHOWTEX"); dp = -1;
          if (e) { char* d; dp = strtol(e, &d, 10); du = (*d == ':') ? strtol(d + 1, 0, 10) : 0; } }
      if (dp >= 0 && (long)r->pipeline == dp && r->nrt) {
          static id<MTLRenderPipelineState> pso; static MTLPixelFormat fmt;
          id<MTLTexture> rt0 = eng_obj(r->rt[0]);
          if (rt0 && (!pso || fmt != [rt0 pixelFormat])) {
              MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
              pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
              pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_blit_fs"];
              pd.colorAttachments[0].pixelFormat = [rt0 pixelFormat];
              pd.depthAttachmentPixelFormat   = MTL_DEPTH_FORMAT;
              pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
              pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:nil];
              fmt = [rt0 pixelFormat];
          }
          if (pso) {
              static id<MTLDepthStencilState> off;
              if (!off) off = [s_dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
              [enc setRenderPipelineState:pso];
              [enc setDepthStencilState:off];
              [enc setCullMode:MTLCullModeNone];
              eng_bound_reset();   /* debug path: forget what is bound */
              id<MTLTexture> t = eng_obj(r->tex[du]);
              [enc setFragmentTexture:(t ? t : s_null_tex) atIndex:0];
              [enc setFragmentSamplerState:s_eng_point_sampler atIndex:0];
              [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
              return;
          }
      } }
    if (r->index_count && r->ib_buf) {
        if (r->ib_buf > s_vbuf_count || !s_vbuf[r->ib_buf - 1]) return;
        [enc drawIndexedPrimitives:r->topology
                        indexCount:r->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:s_vbuf[r->ib_buf - 1]
                 indexBufferOffset:0];
    } else if (r->index_count)
        [enc drawIndexedPrimitives:r->topology
                        indexCount:r->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:stage
                 indexBufferOffset:r->ib_off];
    else
        [enc drawPrimitives:r->topology
                vertexStart:0 vertexCount:r->vertex_count];
}

/* Is this record aimed at exactly these colour attachments, in this order? */
static int eng_record_targets_are(const EngRecord* r, const u32* rt, u32 nrt)
{
    if (r->nrt != nrt) return 0;
    for (u32 k = 0; k < nrt; k++) if (r->rt[k] != rt[k]) return 0;
    return 1;
}

/* A clear names a surface independently of any later draw's attachment set.
 * Replay it alone when it cannot be folded into that draw's load actions. */
static int eng_encode_clear(id<MTLCommandBuffer> cb, const EngRecord* r)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (r->kind == ENG_REC_CLEAR_COLOR) {
        id<MTLTexture> t = eng_obj(r->rt[0]);
        if (!t) return 0;
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(
            r->clear_rgba[0], r->clear_rgba[1], r->clear_rgba[2], r->clear_rgba[3]);
    } else if (r->kind == ENG_REC_CLEAR_DS) {
        id<MTLTexture> t = eng_obj(r->depth);
        if (!t) return 0;
        rp.depthAttachment.texture = t;
        rp.depthAttachment.loadAction = (r->clear_flags & RSX_BE_CLEAR_DEPTH)
            ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = r->clear_depth;
        rp.stencilAttachment.texture = t;
        rp.stencilAttachment.loadAction = (r->clear_flags & RSX_BE_CLEAR_STENCIL)
            ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.clearStencil = r->clear_stencil;
        rp.renderTargetWidth = [t width];
        rp.renderTargetHeight = [t height];
    } else {
        return 0;
    }
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) return 0;
    [enc endEncoding];
    return 1;
}

/* One pass per contiguous run of draws against the same attachments, with any
 * clears immediately ahead of it folded into its load actions. A clear whose
 * targets the following draw does not share becomes a pass of its own, which
 * is what keeps a mid-frame clear ordered against the draws around it. */
static u32 s_ft_passes, s_ft_resolves, s_ft_copies;   /* PS3_FTRACE: per command buffer */
static void eng_encode_records(id<MTLCommandBuffer> cb, id<MTLBuffer> stage)
{
    u32 i = 0;
    s_ft_passes = s_ft_resolves = s_ft_copies = 0;
    while (i < s_enc_count) {
        if (s_enc_rec[i].kind == ENG_REC_COLOR_COPY) {
            const EngRecord* r = &s_enc_rec[i++];
            id<MTLTexture> src = eng_obj(r->rt[0]);
            id<MTLTexture> dst = eng_obj(r->resolve_dst);
            if (!src || !dst) continue;
            s_ft_copies++;
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:src toTexture:dst];
            [b endEncoding];
            continue;
        }
        if (s_enc_rec[i].kind == ENG_REC_SSAO) {
            const EngRecord* r = &s_enc_rec[i++];
            id<MTLTexture> zt = eng_obj(r->tex[0]), nt = eng_obj(r->tex[1]);
            id<MTLTexture> hd = eng_obj(s_eng_ssao_tex[0]), a = eng_obj(s_eng_ssao_tex[1]),
                           b = eng_obj(s_eng_ssao_tex[2]), out = eng_obj(s_eng_ssao_tex[3]);
            id<MTLBuffer> pb = s_eng_ssao_param[r->clear_flags % 8u];
            if (!zt || !nt || !hd || !a || !b || !out || !pb) continue;
            /* hd <- depth; a <- (hd, normals); b <- blur_h(a); a <- blur_v(b); out <- up(a, hd, depth) */
            struct { id<MTLTexture> dst; id<MTLTexture> t0, t1, t2; } steps[5] = {
                { hd, zt, nil, nil }, { a, hd, nt, nil }, { b, a, hd, nil }, { a, b, hd, nil }, { out, a, hd, zt } };
            for (int k = 0; k < 5; k++) {
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = steps[k].dst;
                rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                if (!e) break;
                s_ft_passes++;
                /* The upsample at the scaled size when its output is (ssao_up4s_fs). */
                [e setRenderPipelineState:s_eng_ssao_pso[(k == 4 && ([out width] != s_eng_ssao_dims[0] ||
                                                        [out height] != s_eng_ssao_dims[1])) ? 5 : k]];
                if (steps[k].t0) [e setFragmentTexture:steps[k].t0 atIndex:0];
                if (steps[k].t1) [e setFragmentTexture:steps[k].t1 atIndex:1];
                if (steps[k].t2) [e setFragmentTexture:steps[k].t2 atIndex:2];
                [e setFragmentBuffer:pb offset:0 atIndex:0];
                { const float rs[2] = { s_res_sx, s_res_sy }; [e setFragmentBytes:rs length:sizeof rs atIndex:1]; }
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
            }
            continue;
        }
        if (s_enc_rec[i].kind == ENG_REC_LIGHT) {
            const EngRecord* r = &s_enc_rec[i++];
            id<MTLTexture> nt = eng_obj(r->tex[0]), zt = eng_obj(r->tex[1]);
            id<MTLTexture> od = eng_obj(r->rt[0]), os = eng_obj(r->rt[1]);
            id<MTLBuffer> pb = s_eng_light_param[r->clear_flags % 8u];
            if (!nt || !zt || !od || !os || !pb || !s_eng_light_pso) continue;
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = od;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[1].texture = os;
            rp.colorAttachments[1].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[1].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            if (!e) continue;
            s_ft_passes++;
            [e setRenderPipelineState:s_eng_light_pso];
            [e setFragmentTexture:nt atIndex:0];
            [e setFragmentTexture:zt atIndex:1];
            [e setFragmentBuffer:pb offset:0 atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            continue;
        }
        if (s_enc_rec[i].kind == ENG_REC_DEPTH_RESOLVE) {
            const EngRecord* r = &s_enc_rec[i++];
            id<MTLTexture> src = eng_obj(r->depth);
            id<MTLTexture> dst = eng_obj(r->resolve_dst);
            id<MTLRenderPipelineState> pso = r->clear_flags ? s_eng_depth_rgba_pso : s_eng_depth_pso;
            if (!src || !dst || !pso) continue;
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture     = dst;
            rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            if (!e) continue;
            s_ft_resolves++;
            [e setRenderPipelineState:pso];
            [e setFragmentTexture:src atIndex:0];
            if (r->clear_flags) {
                /* The stencil half of the same target, one view per depth texture. */
                static struct { u32 depth; id<MTLTexture> view; } sv[32]; static u32 nsv;
                id<MTLTexture> view = nil;
                for (u32 k = 0; k < nsv; k++) if (sv[k].depth == r->depth && eng_obj(sv[k].depth) == src) { view = sv[k].view; break; }
                if (!view) {
                    view = [src newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
                    if (view) { const u32 k = nsv < 32 ? nsv++ : (r->depth % 32); sv[k].depth = r->depth; sv[k].view = view; }
                }
                [e setFragmentTexture:(view ? view : src) atIndex:1];
            }
            [e setFragmentSamplerState:s_eng_point_sampler atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            continue;
        }

        /* The clears queued ahead of this pass. A colour clear names one
         * surface and an MRT set arrives as one clear per target, so they are
         * collected rather than compared: they become load actions on the
         * pass below when the draw that follows writes the same targets. */
        const u32 clear_start = i;
        u32 n_cc = 0;
        u32 cc_surf[RSX_BE_MAX_COLOR_TARGETS];
        float cc_rgba[RSX_BE_MAX_COLOR_TARGETS][4];
        int have_clear_ds = 0;
        u32 cd = 0, ds_flags = 0;
        float cdepth = 1.0f;
        u8 cstencil = 0;
        while (i < s_enc_count && s_enc_rec[i].kind != ENG_REC_DRAW &&
               s_enc_rec[i].kind != ENG_REC_DEPTH_RESOLVE &&
               s_enc_rec[i].kind != ENG_REC_COLOR_COPY &&
               s_enc_rec[i].kind != ENG_REC_LIGHT &&
               s_enc_rec[i].kind != ENG_REC_SSAO) {
            const EngRecord* r = &s_enc_rec[i];
            if (r->kind == ENG_REC_CLEAR_COLOR) {
                u32 k = 0;
                while (k < n_cc && cc_surf[k] != r->rt[0]) k++;
                if (k == n_cc) {
                    if (n_cc == RSX_BE_MAX_COLOR_TARGETS) break;
                    cc_surf[n_cc++] = r->rt[0];
                }
                memcpy(cc_rgba[k], r->clear_rgba, sizeof cc_rgba[k]);
            } else {
                if (have_clear_ds && r->depth != cd) break;
                cd = r->depth;
                ds_flags |= r->clear_flags;
                cdepth = r->clear_depth;
                cstencil = r->clear_stencil;
                have_clear_ds = 1;
            }
            i++;
        }

        /* What the pass attaches: the following draw's targets when it writes
         * every surface the clears named, else the cleared surfaces on their
         * own -- which is what keeps a mid-frame clear ordered against the
         * draws around it. */
        u32 rt[RSX_BE_MAX_COLOR_TARGETS];
        u32 nrt = n_cc;
        for (u32 k = 0; k < n_cc; k++) rt[k] = cc_surf[k];
        u32 depth = have_clear_ds ? cd : 0;
        int fold_clears = 0;
        if (i < s_enc_count && s_enc_rec[i].kind == ENG_REC_DRAW &&
            (!have_clear_ds || s_enc_rec[i].depth == cd)) {
            const EngRecord* d = &s_enc_rec[i];
            int covered = 1;
            for (u32 k = 0; k < n_cc && covered; k++) {
                int found = 0;
                for (u32 j = 0; j < d->nrt; j++)
                    if (d->rt[j] == cc_surf[k]) found = 1;
                covered = found;
            }
            if (covered) {
                fold_clears = 1;
                nrt = d->nrt;
                for (u32 k = 0; k < nrt; k++) rt[k] = d->rt[k];
                depth = d->depth;
            }
        }

        /* Without a covering draw these are independent clear operations,
         * not a synthetic MRT. In particular, differently sized surfaces
         * must each be cleared over their full extent, in record order. */
        if (!fold_clears && i > clear_start) {
            for (u32 c = clear_start; c < i; c++) {
                if (!eng_encode_clear(cb, &s_enc_rec[c])) return;
            }
            continue;
        }

        /* Metal wants every attachment the same size, and the engine registers
         * an MRT set's members at target A's, so a set whose members disagree
         * is one this frame cannot honour: it stops at the gap, as the vtable
         * path's begin_pass does. */
        id<MTLTexture> tex[RSX_BE_MAX_COLOR_TARGETS];
        u32 attach = 0;
        for (u32 k = 0; k < nrt; k++) {
            id<MTLTexture> t = eng_obj(rt[k]);
            if (!t) break;
            if (attach && ([t width]  != [tex[0] width] ||
                           [t height] != [tex[0] height])) break;
            tex[attach++] = t;
        }
        id<MTLTexture> zbuf = eng_obj(depth);
        if (!attach && !zbuf) { if (i < s_enc_count && s_enc_rec[i].kind == ENG_REC_DRAW) i++; continue; }
        /* A pipeline always declares the depth attachment, so a draw pass
         * always has to have one; the display's shared buffer is the fallback
         * for a guest that never declared a zeta. */
        if (attach && !zbuf) zbuf = s_depth;

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (u32 k = 0; k < attach; k++) {
            rp.colorAttachments[k].texture     = tex[k];
            rp.colorAttachments[k].storeAction = MTLStoreActionStore;
            int cleared = -1;
            for (u32 c = 0; c < n_cc; c++) if (cc_surf[c] == rt[k]) cleared = (int)c;
            if (cleared >= 0) {
                rp.colorAttachments[k].loadAction = MTLLoadActionClear;
                rp.colorAttachments[k].clearColor =
                    MTLClearColorMake(cc_rgba[cleared][0], cc_rgba[cleared][1],
                                      cc_rgba[cleared][2], cc_rgba[cleared][3]);
            } else {
                rp.colorAttachments[k].loadAction = MTLLoadActionLoad;
            }
        }
        if (zbuf) {
            const int clear_here = have_clear_ds && depth == cd;
            rp.depthAttachment.texture       = zbuf;
            rp.depthAttachment.storeAction   = MTLStoreActionStore;
            rp.depthAttachment.loadAction    =
                (clear_here && (ds_flags & RSX_BE_CLEAR_DEPTH))
                    ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth    = (double)cdepth;
            rp.stencilAttachment.texture     = zbuf;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.loadAction  =
                (clear_here && (ds_flags & RSX_BE_CLEAR_STENCIL))
                    ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = cstencil;
        }
        if (!attach) {
            rp.renderTargetWidth  = [zbuf width];
            rp.renderTargetHeight = [zbuf height];
        }

        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        /* A draw's pipeline declares as many colour attachments as its record
         * named, so a pass that could not bind them all takes its clears and
         * skips those draws rather than encoding a mismatch. */
        if (!enc || attach != nrt) {
            if (enc) [enc endEncoding];
            while (i < s_enc_count && s_enc_rec[i].kind == ENG_REC_DRAW &&
                   eng_record_targets_are(&s_enc_rec[i], rt, nrt)) i++;
            continue;
        }
        eng_bound_reset();
        s_ft_passes++;
        while (i < s_enc_count && s_enc_rec[i].kind == ENG_REC_DRAW &&
               eng_record_targets_are(&s_enc_rec[i], rt, nrt) &&
               (s_enc_rec[i].depth == depth ||
                (!s_enc_rec[i].depth && zbuf == s_depth))) {
            eng_encode_draw(enc, &s_enc_rec[i], stage);
            i++;
        }
        [enc endEncoding];
    }
}

/* PS3_AA=fxaa: the display buffer through FXAA into a texture of its own
 * size and format. nil (and the frame goes on without it) if that fails. */
static id<MTLTexture> gfx_fxaa(id<MTLCommandBuffer> cb, id<MTLTexture> src)
{
    const MTLPixelFormat fmt = [src pixelFormat];
    if (!s_gfx_fxaa_pso || s_gfx_fxaa_pso_fmt != fmt) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_fxaa_fs"];
        pd.colorAttachments[0].pixelFormat = fmt;
        NSError* err = nil;
        s_gfx_fxaa_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_gfx_fxaa_pso) {
            fprintf(stderr, "[rsx engine/metal] FXAA pipeline failed (FXAA off): %s\n",
                    [[err localizedDescription] UTF8String]);
            s_gfx_fxaa = 0;
            return nil;
        }
        s_gfx_fxaa_pso_fmt = fmt;
    }
    if (!s_gfx_fxaa_tex || [s_gfx_fxaa_tex width] != [src width] ||
        [s_gfx_fxaa_tex height] != [src height] || [s_gfx_fxaa_tex pixelFormat] != fmt) {
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:[src width]
                                                              height:[src height] mipmapped:NO];
        td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        s_gfx_fxaa_tex = [s_dev newTextureWithDescriptor:td];
        if (!s_gfx_fxaa_tex) return nil;
    }
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = s_gfx_fxaa_tex;
    rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return nil;
    const float rcp[2] = { 1.0f / (float)[src width], 1.0f / (float)[src height] };
    [e setRenderPipelineState:s_gfx_fxaa_pso];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentSamplerState:s_eng_linear_sampler atIndex:0];
    [e setFragmentBytes:rcp length:sizeof rcp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    return s_gfx_fxaa_tex;
}

/* PS3_AA=smaa: SMAA 1x on the display buffer -- luma edges into an RG8
 * target, blending weights (AreaTex/SearchTex lookups) into an RGBA8 one,
 * then the neighbourhood blend into a texture of src's size and format. nil
 * (and the frame goes on without it) if any step cannot be set up. */
static id<MTLRenderPipelineState> smaa_pso(NSString* vs, NSString* fs, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = [s_smaa_lib newFunctionWithName:vs];
    pd.fragmentFunction = [s_smaa_lib newFunctionWithName:fs];
    pd.colorAttachments[0].pixelFormat = fmt;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) fprintf(stderr, "[rsx engine/metal] SMAA pipeline %s failed (SMAA off): %s\n",
                    [fs UTF8String], [[err localizedDescription] UTF8String]);
    return p;
}
static id<MTLTexture> smaa_target(id<MTLTexture> t, MTLPixelFormat fmt, NSUInteger w, NSUInteger h)
{
    if (t && [t width] == w && [t height] == h && [t pixelFormat] == fmt) return t;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                     width:w height:h mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return [s_dev newTextureWithDescriptor:td];
}
static id<MTLTexture> smaa_lookup(MTLPixelFormat fmt, NSUInteger w, NSUInteger h,
                                  const unsigned char* bytes, NSUInteger pitch)
{
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                     width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bytes bytesPerRow:pitch];
    return t;
}
static int smaa_pass(id<MTLCommandBuffer> cb, id<MTLTexture> dst, id<MTLRenderPipelineState> pso,
                     id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2,
                     id<MTLSamplerState> samp, const float* metrics, int clear)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = dst;
    rp.colorAttachments[0].loadAction  = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return 0;
    [e setRenderPipelineState:pso];
    [e setVertexBytes:metrics length:4 * sizeof(float) atIndex:0];
    [e setFragmentBytes:metrics length:4 * sizeof(float) atIndex:0];
    if (t0) [e setFragmentTexture:t0 atIndex:0];
    if (t1) [e setFragmentTexture:t1 atIndex:1];
    if (t2) [e setFragmentTexture:t2 atIndex:2];
    [e setFragmentSamplerState:samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    return 1;
}
static id<MTLTexture> gfx_smaa(id<MTLCommandBuffer> cb, id<MTLTexture> src)
{
    const MTLPixelFormat fmt = [src pixelFormat];
    if (!s_smaa_lib) {
        NSError* err = nil;
        s_smaa_lib = [s_dev newLibraryWithSource:@(kSmaaMSL) options:nil error:&err];
        if (!s_smaa_lib) {
            fprintf(stderr, "[rsx engine/metal] SMAA shaders failed to compile (SMAA off): %s\n",
                    [[err localizedDescription] UTF8String]);
            s_gfx_smaa = 0;
            return nil;
        }
        s_smaa_edge_pso   = smaa_pso(@"smaa_edge_vs", @"smaa_edge_ps", MTLPixelFormatRG8Unorm);
        s_smaa_weight_pso = smaa_pso(@"smaa_weight_vs", @"smaa_weight_ps", MTLPixelFormatRGBA8Unorm);
        s_smaa_area   = smaa_lookup(MTLPixelFormatRG8Unorm, AREATEX_WIDTH, AREATEX_HEIGHT,
                                    areaTexBytes, AREATEX_PITCH);
        s_smaa_search = smaa_lookup(MTLPixelFormatR8Unorm, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                                    searchTexBytes, SEARCHTEX_PITCH);
        if (!s_smaa_edge_pso || !s_smaa_weight_pso || !s_smaa_area || !s_smaa_search) {
            s_gfx_smaa = 0;
            return nil;
        }
    }
    if (!s_smaa_blend_pso || s_smaa_blend_fmt != fmt) {
        s_smaa_blend_pso = smaa_pso(@"smaa_blend_vs", @"smaa_blend_ps", fmt);
        if (!s_smaa_blend_pso) { s_gfx_smaa = 0; return nil; }
        s_smaa_blend_fmt = fmt;
    }
    const NSUInteger w = [src width], h = [src height];
    s_smaa_edges   = smaa_target(s_smaa_edges, MTLPixelFormatRG8Unorm, w, h);
    s_smaa_weights = smaa_target(s_smaa_weights, MTLPixelFormatRGBA8Unorm, w, h);
    s_smaa_out     = smaa_target(s_smaa_out, fmt, w, h);
    if (!s_smaa_edges || !s_smaa_weights || !s_smaa_out) return nil;
    const float metrics[4] = { 1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h };
    if (!smaa_pass(cb, s_smaa_edges, s_smaa_edge_pso, src, nil, nil, s_eng_point_sampler, metrics, 1) ||
        !smaa_pass(cb, s_smaa_weights, s_smaa_weight_pso, s_smaa_edges, s_smaa_area, s_smaa_search,
                   s_eng_linear_sampler, metrics, 1) ||
        !smaa_pass(cb, s_smaa_out, s_smaa_blend_pso, src, s_smaa_weights, nil,
                   s_eng_linear_sampler, metrics, 0))
        return nil;
    return s_smaa_out;
}

/* PS3_UPSCALE=metalfx: src through a MetalFX spatial scaler into a private
 * texture of the drawable's size. nil when it cannot; a failure that will
 * not go away by itself turns the option off (gfx_upscale_fail). */
static id<MTLTexture> gfx_metalfx(id<MTLCommandBuffer> cb, id<MTLTexture> src,
                                  NSUInteger ow, NSUInteger oh)
{
#if GFX_HAVE_METALFX
    if (@available(macOS 13.0, *)) {
        const MTLPixelFormat fmt = [src pixelFormat];
        id<MTLFXSpatialScaler> sc = (id<MTLFXSpatialScaler>)s_gfx_fx_scaler;
        if (!sc || [sc inputWidth] != [src width] || [sc inputHeight] != [src height] ||
            [sc outputWidth] != ow || [sc outputHeight] != oh || [sc colorTextureFormat] != fmt) {
            s_gfx_fx_scaler = nil; s_gfx_fx_out = nil;
            if (![MTLFXSpatialScalerDescriptor supportsDevice:s_dev]) {
                gfx_upscale_fail("not supported on this GPU");
                return nil;
            }
            MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
            d.inputWidth  = [src width];  d.inputHeight  = [src height];
            d.outputWidth = ow;           d.outputHeight = oh;
            d.colorTextureFormat  = fmt;
            d.outputTextureFormat = fmt;
            /* The display buffer holds gamma-encoded colour. */
            d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            sc = [d newSpatialScalerWithDevice:s_dev];
            if (!sc) { gfx_upscale_fail("could not create the spatial scaler"); return nil; }
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:ow
                                                                  height:oh mipmapped:NO];
            td.usage       = [sc outputTextureUsage] | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> out = [s_dev newTextureWithDescriptor:td];
            if (!out) { gfx_upscale_fail("no memory for its output"); return nil; }
            if (!s_gfx_fx_logged++)
                fprintf(stderr, "[RSX metal] MetalFX spatial upscaling %lux%lu -> %lux%lu\n",
                        (unsigned long)[src width], (unsigned long)[src height],
                        (unsigned long)ow, (unsigned long)oh);
            s_gfx_fx_scaler = sc;
            s_gfx_fx_out    = out;
        }
        if (([src usage] & [sc colorTextureUsage]) != [sc colorTextureUsage]) {
            gfx_upscale_fail("the display buffer's usage does not allow it");
            return nil;
        }
        sc.colorTexture       = src;
        sc.outputTexture      = s_gfx_fx_out;
        sc.inputContentWidth  = [src width];
        sc.inputContentHeight = [src height];
        [sc encodeToCommandBuffer:cb];
        return s_gfx_fx_out;
    }
    gfx_upscale_fail("needs macOS 13");
    return nil;
#else
    (void)cb; (void)src; (void)ow; (void)oh;
    gfx_upscale_fail("built without MetalFX");
    return nil;
#endif
}

/* Put a surface on the drawable. A straight blit would need matching formats
 * and sizes; a full-screen sample needs neither, and the guest's surfaces are
 * routinely a different size from the window.
 *
 * With the graphics options: FXAA at the guest's size, then MetalFX up to the
 * drawable's, then this pass (sharpening + gamma). Each one that is off or
 * fails is skipped and the next one takes what came before it. */
static void eng_blit_to_display(id<MTLCommandBuffer> cb, id<MTLTexture> src,
                                id<MTLTexture> dst)
{
    if (!src || !dst || !s_eng_helper_lib) return;
    if (__builtin_expect(__atomic_load_n(&s_gfx_dirty, __ATOMIC_RELAXED), 0)) {   /* PS3_TOGGLE_FILE */
        __atomic_store_n(&s_gfx_dirty, 0, __ATOMIC_RELAXED);
        gfx_options_read();
    }
    if (s_gfx_fxaa) { id<MTLTexture> t = gfx_fxaa(cb, src); if (t) src = t; }
    else if (s_gfx_smaa) { id<MTLTexture> t = gfx_smaa(cb, src); if (t) src = t; }
    const int scaling = ([src width] != [dst width] || [src height] != [dst height]);
    if (scaling && __atomic_load_n(&s_gfx_upscale_on, __ATOMIC_ACQUIRE) &&
        [dst width] > [src width] && [dst height] > [src height]) {
        id<MTLTexture> t = gfx_metalfx(cb, src, [dst width], [dst height]);
        if (t) src = t;
    }
    if (!s_eng_blit_pso || s_eng_blit_pso_fmt != [dst pixelFormat]) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_present_fs"];
        pd.colorAttachments[0].pixelFormat = [dst pixelFormat];
        NSError* err = nil;
        s_eng_blit_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_eng_blit_pso) {
            fprintf(stderr, "[rsx engine/metal] present pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return;
        }
        s_eng_blit_pso_fmt = [dst pixelFormat];
    }
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = dst;
    rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return;
    [e setRenderPipelineState:s_eng_blit_pso];
    [e setFragmentTexture:src atIndex:0];
    /* Point sampling whenever nothing scales (and without PS3_UPSCALE, as it
     * always was); bilinear when the upscale option has the drawable larger
     * and MetalFX did not fill it. */
    const int lin = s_gfx_upscale &&
                    ([src width] != [dst width] || [src height] != [dst height]);
    [e setFragmentSamplerState:lin ? s_eng_linear_sampler : s_eng_point_sampler atIndex:0];
    /* The video out's gamma (cellVideoOutSetGamma, the title's brightness
     * setting): out = in^gamma, as RPCS3's calibration pass applies it. */
    { extern float cellAvconfExt_display_gamma(void);
      static unsigned frame;
      gfx_lut_select();
      const struct { float gamma, sharp, px[2], vib, con, vig, grain, time, lsz, lamt, pad,
                     csh, cmid, chi, cla; } p = {
          cellAvconfExt_display_gamma(), s_gfx_sharpen,
          { 1.0f / (float)[dst width], 1.0f / (float)[dst height] },
          s_gfx_vibrance, s_gfx_contrast, s_gfx_vignette, s_gfx_grain, (float)(++frame & 1023u),
          s_gfx_lut_size, s_gfx_lut ? s_gfx_lut_strength : 0.0f, 0.0f,
          s_gfx_shadows, s_gfx_midtones, s_gfx_highlights, s_gfx_clarity };
      [e setFragmentBytes:&p length:sizeof p atIndex:0];
      [e setFragmentTexture:s_gfx_lut ? s_gfx_lut : src atIndex:1];
      [e setFragmentSamplerState:s_eng_linear_sampler atIndex:1]; }
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    /* The in-game menu / HUD, over the finished picture at the drawable's size. */
    if (__builtin_expect(g_ps3_overlay_active, 0)) ps3_overlay_render((__bridge void*)cb, (__bridge void*)dst);
}

/* Encode everything recorded so far and commit it.
 *
 * The staging arena is recycled the moment this returns, which is safe
 * without a wait: newBufferWithBytes COPIES, so what the GPU reads is the
 * MTLBuffer and not the arena, and a command buffer retains every resource it
 * uses. So only headless waits, where the readback has to observe a finished
 * frame -- there the stall is the point. A windowed present is paced by the
 * in-flight semaphore instead, the same bound the vtable path uses, because
 * waiting on every frame is the one pattern where a driver cannot hide its
 * encoding work. */
/* PS3RECOMP_ENG_FPSLOG: where a present's time goes, per 10 s. */
static u64 s_pt_ns[4];          /* in-flight wait, nextDrawable, encode, commit */
static u32 s_pt_n;
static u64 eng_pt_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static void eng_pt_report(void)
{
    static int on = -1; static u64 t0;
    if (on < 0) on = getenv("PS3RECOMP_ENG_FPSLOG") ? 1 : 0;
    if (!on) return;
    const u64 now = eng_pt_now();
    if (!t0) t0 = now;
    if (now - t0 < 10000000000ull) return;
    fprintf(stderr, "[rsx present] %u presents: in-flight wait %.0f ms, nextDrawable %.0f ms,"
            " encode %.0f ms, commit %.0f ms; objects %u live of %u\n", s_pt_n,
            s_pt_ns[0] / 1e6, s_pt_ns[1] / 1e6, s_pt_ns[2] / 1e6, s_pt_ns[3] / 1e6,
            s_eng_obj_count - s_eng_obj_free_count, s_eng_obj_count);
    memset(s_pt_ns, 0, sizeof s_pt_ns); s_pt_n = 0; t0 = now;
}

/* One submission's records into a command buffer and commit it: on the RSX
 * thread, or on the encoder thread (PS3RECOMP_METAL_ENCODE_THREAD, worker =
 * 1). Returns 0 when a windowed present found no drawable on the RSX thread
 * (the records stay for the next commit, as they always did). */
static int eng_encode_commit_body(id<MTLTexture> present_dst, const EngRecord* recs, u32 nrec,
                                  int slot_in, u32 stage_used, int bars, int worker)
{
    @autoreleasepool {
        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> dst = nil;
        /* Headless presents used to wait for the GPU every frame (the frame
         * dump needs a finished frame); they now bound in-flight frames like a
         * window does, so headless timings match what a window shows. The
         * dumps fence the queue themselves. PS3RECOMP_METAL_HEADLESS_SYNC=1
         * restores the wait. */
        const int windowed_present = (present_dst && (!s_headless || !eng_headless_sync()));
        u64 pt = eng_pt_now(), pt1;
        if (windowed_present) dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
        pt1 = eng_pt_now(); if (windowed_present) s_pt_ns[0] += pt1 - pt;
        if (windowed_present && PS3_FT_ON() && pt1 - pt >= ps3_ft_min_ns())
            ps3_ft_span(PS3_FT_GPU_WAIT, 0, 2, 0, pt, pt1);
        pt = pt1;
        if (present_dst) {
            if (s_headless) {
                dst = s_offscreen;
            } else {
                drawable = [s_layer nextDrawable];
                pt1 = eng_pt_now(); s_pt_ns[1] += pt1 - pt; pt = pt1;
                if (!drawable && !worker) {   /* compositor is busy; skip */
                    dispatch_semaphore_signal(s_inflight);
                    return 0;
                }
                /* The encoder thread has the records already: encode them
                 * without a present (the in-flight slot is released by the
                 * completion handler as usual). */
                dst = drawable ? [drawable texture] : nil;
            }
        }

        id<MTLBuffer> stage = nil;
        const int slot = slot_in;
        if (stage_used && slot >= 0) stage = s_eng_slot_buf[slot];
        /* PS3RECOMP_METAL_PASS_LOG=1: every 600th present, print the frame's
         * record stream grouped into passes (kind, targets, depth, count, and
         * the textures the first draw samples). */
        /* =2: split a pass at every change of pipeline or bound textures, so
         * each line is one material's run of draws instead of one line per
         * pass with only its first draw described. */
        { static int s_pl = -1; static unsigned s_pl_min = 1000, s_pl_every = 300;
          if (s_pl < 0) { const char* e = getenv("PS3RECOMP_METAL_PASS_LOG"); s_pl = e ? atoi(e) : 0;
              /* _MIN: a frame counts when it has more records than this;
               * _EVERY: print every nth such frame. A cutscene frame has fewer
               * records than a busy menu, so the defaults can skip it. */
              e = getenv("PS3RECOMP_METAL_PASS_LOG_MIN");   if (e && atoi(e) > 0) s_pl_min = (unsigned)atoi(e);
              e = getenv("PS3RECOMP_METAL_PASS_LOG_EVERY"); if (e && atoi(e) > 0) s_pl_every = (unsigned)atoi(e); }
        /* _FLUSH=1: also log the record slices a mid-frame submit_and_wait
         * commits (a guest label wait, the stage arena filling up). Without
         * it only the slice ending at a present is ever seen, and inFAMOUS 2's
         * whole G-buffer pass plus its lighting composite live in the slice a
         * flush ends, so the log never showed one multi-target draw. */
        static int s_pl_flush = -1;
        if (s_pl_flush < 0) s_pl_flush = getenv("PS3RECOMP_METAL_PASS_LOG_FLUSH") ? 1 : 0;
        if ((present_dst || s_pl_flush) && s_pl) {
            static unsigned s_pf;
            if ((s_pass_log_force && nrec > 100) || (nrec > s_pl_min && (s_pf++ % s_pl_every) == 0)) {
                if (s_pass_log_force) s_pass_log_force--;   /* an empty flush does not count as a slice */
                fprintf(stderr, "[pass] ---- frame %u: %u records (%s) ----\n", s_pf, nrec,
                        present_dst ? "present" : "flush");
                u32 i = 0;
                while (i < nrec) {
                    const EngRecord* r = &recs[i];
                    u32 j = i + 1;
                    while (j < nrec && recs[j].kind == r->kind && recs[j].nrt == r->nrt &&
                           !memcmp(recs[j].rt, r->rt, sizeof r->rt) && recs[j].depth == r->depth &&
                           (s_pl < 2 || (recs[j].pipeline == r->pipeline &&
                                         !memcmp(recs[j].tex, r->tex, sizeof r->tex)))) j++;
                    fprintf(stderr, "[pass] %5u x%-4u kind=%d rt=", i, j - i, (int)r->kind);
                    for (u32 k = 0; k < r->nrt; k++) fprintf(stderr, "%s%u", k ? "," : "", r->rt[k]);
                    fprintf(stderr, " depth=%u", r->depth);
                    if (r->kind == ENG_REC_DRAW) {
                        fprintf(stderr, " tex=");
                        for (u32 k = 0; k < RSX_BE_MAX_TEXTURES; k++) if (r->tex[k]) fprintf(stderr, "%u:%u ", k, r->tex[k]);
                        if (r->pipeline && r->pipeline <= ENG_MAX_PIPES)
                            fprintf(stderr, " [p=%u %s ref=%X sc=%u,%u,%u,%u vp=%.0f,%.0f,%.0f,%.0f n=%u/%u]",
                                    r->pipeline, s_eng_pipe[r->pipeline - 1].desc, r->stencil_ref,
                                    r->sc[0], r->sc[1], r->sc[2], r->sc[3],
                                    r->vp[0], r->vp[1], r->vp[2], r->vp[3],
                                    r->vertex_count, r->index_count);
                    }
                    fputc('\n', stderr);
                    i = j;
                }
            }
        } }
        pt = eng_pt_now();
        const u32 ft_nrec = nrec;
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        s_enc_rec = recs; s_enc_count = nrec;
        eng_encode_records(cb, stage);
        /* PS3_ASPECT: a frame of 16:9 overlay only (a menu, a motion comic, a
         * movie) gets black bars beside it; what a clear left there would
         * show otherwise. Written into the surface, so later presents of it
         * keep them. */
        if (present_dst && bars && s_eng_hud_w && [present_dst width] > res_px(s_eng_hud_w)) {
            const NSUInteger W = [present_dst width], H = [present_dst height];
            const NSUInteger side = (W - res_px(s_eng_hud_w)) / 2u;
            const MTLPixelFormat pf = [present_dst pixelFormat];
            if (side && (pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatBGRA8Unorm ||
                         pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm_sRGB)) {
                static id<MTLBuffer> zeros; static NSUInteger zbytes;
                const NSUInteger need = side * 4u * H;
                if (!zeros || zbytes < need) { zeros = [s_dev newBufferWithLength:need options:MTLResourceStorageModePrivate]; zbytes = need;
                    id<MTLBlitCommandEncoder> z = [cb blitCommandEncoder]; [z fillBuffer:zeros range:NSMakeRange(0, need) value:0]; [z endEncoding]; }
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                for (int k = 0; k < 2; k++)
                    [b copyFromBuffer:zeros sourceOffset:0 sourceBytesPerRow:side * 4u sourceBytesPerImage:need
                           sourceSize:MTLSizeMake(side, H, 1) toTexture:present_dst destinationSlice:0
                     destinationLevel:0 destinationOrigin:MTLOriginMake(k ? W - side : 0, 0, 0)];
                [b endEncoding];
            }
        }
        if (present_dst && dst) eng_blit_to_display(cb, present_dst, dst);
        if (drawable) { fps_track_drawable(drawable); [cb presentDrawable:drawable]; }
        if (PS3_FT_ON()) ps3_ft_span(PS3_FT_GPU_ENCODE, 0, present_dst ? 1u : 0u, ft_nrec, pt, eng_pt_now());
        if (windowed_present) { pt1 = eng_pt_now(); s_pt_ns[2] += pt1 - pt; pt = pt1; }
        if (windowed_present) {
            dispatch_semaphore_t sem = s_inflight;
            volatile int* busy = slot >= 0 ? &s_eng_slot_busy[slot] : NULL;
            if (busy) __atomic_store_n(busy, 1, __ATOMIC_RELEASE);
            /* PS3_FTRACE: the GPU's own time for an async present as well
             * (the synchronous paths record it after waitUntilCompleted). */
            const u32 ft_passes = s_ft_passes, ft_rc = s_ft_resolves | (s_ft_copies << 16);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                if (PS3_FT_ON()) {
                    const double gpu_s = [done GPUEndTime] - [done GPUStartTime];
                    const u64 w1 = eng_pt_now();
                    if (gpu_s > 0) ps3_ft_span(PS3_FT_GPU_EXEC, ft_passes, ft_rc, 1u, w1 - (u64)(gpu_s * 1e9), w1);
                }
                if (busy) __atomic_store_n(busy, 0, __ATOMIC_RELEASE);
                dispatch_semaphore_signal(sem);
            }];
            [cb commit];
            s_pt_ns[3] += eng_pt_now() - pt;
            s_pt_n++;
            eng_pt_report();
            if (s_sc_state == 1) { static u32 n, lh = ~0u, lm = ~0u;
                if (++n % 1800 == 0 && (s_sc_hits != lh || s_sc_miss != lm)) {
                    fprintf(stderr, "[shader cache] translations so far: %u from the cache, %u new\n",
                            s_sc_hits, s_sc_miss);
                    lh = s_sc_hits; lm = s_sc_miss; } }
            /* PS3_SHADER_ASYNC: what the background builds cost in draws. */
            if (s_sc_queue) { static u32 n, last_skipped;
                if (++n % 600 == 0 && s_sc_async_skipped != last_skipped) {
                    fprintf(stderr, "[shader cache] async: %u draw(s) skipped waiting for a pipeline"
                                    " in the last 600 presents, %u build(s) queued\n",
                            s_sc_async_skipped - last_skipped,
                            __atomic_load_n(&s_sc_async_pending, __ATOMIC_RELAXED));
                    last_skipped = s_sc_async_skipped; } }
        } else {
            const u64 ft_w0 = PS3_FT_ON() ? eng_pt_now() : 0;
            [cb commit];
            [cb waitUntilCompleted];
            if (ft_w0) {
                const u64 w1 = eng_pt_now();
                ps3_ft_span(PS3_FT_GPU_WAIT, 0, present_dst ? 1u : 0u,
                            present_dst ? 0u : s_ft_flush_reason, ft_w0, w1);
                const double gpu_s = [cb GPUEndTime] - [cb GPUStartTime];
                if (gpu_s > 0)
                    ps3_ft_span(PS3_FT_GPU_EXEC, s_ft_passes, s_ft_resolves | (s_ft_copies << 16),
                                present_dst ? 1u : 0u, w1 - (u64)(gpu_s * 1e9), w1);
            }
            /* A faulted command buffer (GPU hang, a bad resource) completes
             * with an error and nothing says so; the next frame then renders
             * on a device that may have been lost. Name it, with its code. */
            if ([cb status] == MTLCommandBufferStatusError) {
                static int n;
                if (PS3_LOG_FIRST(n, 8))
                    fprintf(stderr, "[rsx engine/metal] command buffer failed: %s (code %ld)\n",
                            [[[cb error] localizedDescription] UTF8String], (long)[[cb error] code]);
            }
        }

    }
    return 1;
}

/* PS3RECOMP_METAL_HEADLESS_SYNC=1: headless presents wait for the GPU. */
static int eng_headless_sync(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PS3RECOMP_METAL_HEADLESS_SYNC") ? 1 : 0;
    return v;
}

/* PS3RECOMP_METAL_ENCODE_THREAD (live): a present hands its records to an
 * encoder thread and the RSX thread goes straight on with the next frame's
 * FIFO, while the other record bank turns into the command buffer. The
 * encode of a New Marais frame (~9000 draws) was ~5 ms of the RSX thread's
 * ~27 ms; the drain and the encode now overlap. One submission at a time is
 * encoded, in submission order: anything else that commits, reads back or
 * fences the queue (a mid-frame flush, a readback, a frame dump) waits for
 * it first (eng_enc_join). Only the asynchronous presents go over; flushes
 * stay on the RSX thread. 0 = encode on the RSX thread, as before. */
static int s_enc_thread = -1;
static void eng_enc_thread_set(int id, int v) { (void)id; __atomic_store_n(&s_enc_thread, v != 0, __ATOMIC_RELAXED); }
static dispatch_queue_t s_enc_queue;
static dispatch_semaphore_t s_enc_idle;     /* 1 token: no job in flight */
static volatile int s_enc_pending;
static int eng_enc_busy(void) { return __atomic_load_n(&s_enc_pending, __ATOMIC_ACQUIRE); }
static void eng_enc_join(void)
{
    if (!s_enc_idle || !eng_enc_busy()) return;
    dispatch_semaphore_wait(s_enc_idle, DISPATCH_TIME_FOREVER);
    dispatch_semaphore_signal(s_enc_idle);
}
static int eng_enc_thread_on(void)
{
    if (__builtin_expect(__atomic_load_n(&s_enc_thread, __ATOMIC_RELAXED) < 0, 0))
        ps3_toggle_watch(PS3T_METAL_ENCODE_THREAD, eng_enc_thread_set);
    if (!__atomic_load_n(&s_enc_thread, __ATOMIC_RELAXED)) return 0;
    if (!s_enc_queue) {
        dispatch_queue_attr_t at = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                           QOS_CLASS_USER_INTERACTIVE, 0);
        s_enc_queue = dispatch_queue_create("ps3recomp.metal.encode", at);
        s_enc_idle = dispatch_semaphore_create(1);
        if (!s_enc_queue || !s_enc_idle) { __atomic_store_n(&s_enc_thread, 0, __ATOMIC_RELAXED); return 0; }
    }
    return 1;
}

static void eng_submission_done_here(void)
{
    if (s_eng_dropped) {
        fprintf(stderr, "[rsx engine/metal] dropped %u record(s) (cap %d)\n",
                s_eng_dropped, ENG_MAX_RECORDS);
        s_eng_dropped = 0;
    }
    s_eng_rec_count  = 0;
    s_eng_stage_used = 0;
    s_eng_slot = -1;                      /* the next submission stages into the next slot */
    s_rec_seq++;
    eng_collect_retired_objects();
}

static void eng_encode_and_commit(id<MTLTexture> present_dst)
{
    const int async_present = present_dst && (!s_headless || !eng_headless_sync());
    if (async_present && eng_enc_thread_on()) {
        dispatch_semaphore_wait(s_enc_idle, DISPATCH_TIME_FOREVER);   /* the previous one is encoded */
        const EngRecord* recs = s_eng_rec;
        const u32 nrec = s_eng_rec_count, used = s_eng_stage_used, seq = s_rec_seq;
        const int slot = s_eng_slot, bars = s_eng_bars_next;
        s_eng_bars_next = 0;
        s_eng_rec = (s_eng_rec == s_eng_rec_bank[0]) ? s_eng_rec_bank[1] : s_eng_rec_bank[0];
        __atomic_store_n(&s_enc_pending, 1, __ATOMIC_RELEASE);
        id<MTLTexture> dst = present_dst;
        dispatch_async(s_enc_queue, ^{
            eng_encode_commit_body(dst, recs, nrec, slot, used, bars, 1);
            __atomic_store_n(&s_enc_done_seq, seq, __ATOMIC_RELEASE);
            __atomic_store_n(&s_enc_pending, 0, __ATOMIC_RELEASE);
            dispatch_semaphore_signal(s_enc_idle);
        });
        eng_submission_done_here();
        return;
    }
    eng_enc_join();
    if (!eng_encode_commit_body(present_dst, s_eng_rec, s_eng_rec_count, s_eng_slot,
                                s_eng_stage_used, s_eng_bars_next, 0))
        return;
    if (present_dst) s_eng_bars_next = 0;
    __atomic_store_n(&s_enc_done_seq, s_rec_seq, __ATOMIC_RELEASE);
    eng_submission_done_here();
}


/* Opt-in readback of the game surface, useful on a locked Mac or in CI.
 * Headless submits have completed; windowed captures fence the same queue. */
static void eng_dump_frame(id<MTLTexture> src)
{
    const char* path = getenv("PS3RECOMP_METAL_FRAME_DUMP");
    if (!path || !*path) return;
    static unsigned frame, every;
    /* PS3RECOMP_METAL_FRAME_DUMP_EVERY=<n>: every n-th frame (default 120). */
    if (!every) { const char* e = getenv("PS3RECOMP_METAL_FRAME_DUMP_EVERY");
        every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 120u; }
    if ((++frame % every) != 0) return;
    if ([src pixelFormat] != MTLPixelFormatRGBA8Unorm &&
        [src pixelFormat] != MTLPixelFormatBGRA8Unorm) return;
    size_t w = [src width], h = [src height];
    if (!w || !h || w > 16384 || h > 16384) return;
    unsigned char* rgba = malloc(w * h * 4);
    if (!rgba) return;
    eng_enc_join();   /* PS3RECOMP_METAL_ENCODE_THREAD: the present must be committed first */
    {   /* the frame may still be in flight (windowed, or headless without HEADLESS_SYNC) */
        id<MTLCommandBuffer> fence = [s_queue commandBuffer];
        [fence commit];
        [fence waitUntilCompleted];
    }
    if ([src storageMode] == MTLStorageModePrivate) {
        id<MTLBuffer> buf = [s_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        memcpy(rgba, [buf contents], w * h * 4);
    } else {
        [src getBytes:rgba bytesPerRow:w * 4
          fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    }
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%zu %zu\n255\n", w, h);
        int bgra = [src pixelFormat] == MTLPixelFormatBGRA8Unorm;
        for (size_t p = 0; p < w * h; ++p) {
            unsigned char rgb[3] = {rgba[p*4 + (bgra ? 2 : 0)], rgba[p*4+1],
                                    rgba[p*4 + (bgra ? 0 : 2)]};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        /* t= (monotonic seconds) turns consecutive capture lines into an fps
         * timeline (tools/profile_run.sh): frames between them / time between them. */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stderr, "[rsx engine/metal] captured frame %u to %s t=%.3f\n", frame, path,
                (double)ts.tv_sec + ts.tv_nsec / 1e9);
    }
    free(rgba);
}

/* PS3RECOMP_METAL_SURF_DUMP=<dir>: every 600th present, write every live
 * render-target texture (8-bit or half-float colour) as <dir>/f<frame>_h<handle>.ppm.
 * The presented surface alone says nothing about which pass went wrong. */
static float eng_half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
    if (e == 0) { float v = m / 1024.0f / 16384.0f; return s ? -v : v; }
    if (e == 31) f = (s << 31) | 0x7F800000u | (m << 13);
    else f = (s << 31) | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}
/* PS3RECOMP_METAL_DUMP_TRIGGER=<path>: while that file exists at a present,
 * remove it and dump once -- every render target (SURF_DUMP dir), the next
 * frame's pass log (PASS_LOG set) -- so a person playing in a window can say
 * "this frame" by touching a file (tools/play_dump.sh). */
static void eng_dump_surfaces(void)
{
    const char* dir = getenv("PS3RECOMP_METAL_SURF_DUMP");
    if (!dir || !*dir) return;
    static unsigned frame, every;
    if (!every) { const char* e = getenv("PS3RECOMP_METAL_SURF_DUMP_EVERY"); every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 300; }
    if (s_dump_force) { s_dump_force = 0; frame++; fprintf(stderr, "[surf-dump] triggered (present %u)\n", frame); } else
    /* PS3RECOMP_METAL_SURF_DUMP_AT=<sec>: instead of every Nth present, one
     * dump that many seconds after the first present (capture.sh's t minus
     * the boot, ~10 s): the frame a screenshot showed, not a frame count. */
    { static long at_sec = -1; static int at_done; static unsigned long long t0;
      if (at_sec < 0) { const char* a = getenv("PS3RECOMP_METAL_SURF_DUMP_AT"); at_sec = a ? atol(a) : 0; }
      if (at_sec > 0) {
          extern unsigned long long ps3_ms_now(void);
          if (!t0) t0 = ps3_ms_now();
          if (at_done || ps3_ms_now() - t0 < (unsigned long long)at_sec * 1000ull) return;
          at_done = 1;
          fprintf(stderr, "[surf-dump] one-shot at %ld s (present %u)\n", at_sec, frame);
      } else if ((frame++ % every) != 0) return; }
    eng_enc_join();   /* PS3RECOMP_METAL_ENCODE_THREAD: a present may still be encoding */
    { id<MTLCommandBuffer> fence = [s_queue commandBuffer]; [fence commit]; [fence waitUntilCompleted]; }
    for (u32 h = 1; h <= s_eng_obj_count; ++h) {
        id<MTLTexture> t = s_eng_obj[h - 1];
        /* PS3RECOMP_METAL_SURF_DUMP_EXTRA=h,h,...: also dump these handles even if
         * they are plain (sampled) textures. */
        int extra = 0;
        { const char* e = getenv("PS3RECOMP_METAL_SURF_DUMP_EXTRA");
          for (const char* q = e; q && *q; ) { char* end; unsigned long v = strtoul(q, &end, 10);
              if (end == q) break; if (v == h) extra = 1; q = *end ? end + 1 : end; } }
        if (!t) { if (extra) fprintf(stderr, "[surf-dump] h%u is empty\n", h); continue; }
        if (!extra && (!([t usage] & MTLTextureUsageRenderTarget) || [t textureType] != MTLTextureType2D)) {
            if (h < 32) fprintf(stderr, "[surf-dump] skip h%u usage=%lu type=%lu fmt=%lu %lux%lu samples=%lu\n", h,
                                (unsigned long)[t usage], (unsigned long)[t textureType], (unsigned long)[t pixelFormat],
                                (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[t sampleCount]);
            continue;
        }
        MTLPixelFormat pf = [t pixelFormat];
        size_t w = [t width], hh = [t height], bpp;
        if (pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatBGRA8Unorm) bpp = 4;
        else if (pf == MTLPixelFormatRGBA16Float) bpp = 8;
        else { fprintf(stderr, "[surf-dump] skip h%u %zux%zu pixelFormat=%lu\n", h, w, hh, (unsigned long)pf); continue; }
        if (w > 4096 || hh > 4096) continue;
        unsigned char* px = malloc(w * hh * bpp);
        if (!px) continue;
        if ([t storageMode] == MTLStorageModePrivate) {
            /* GPU-only target: blit into a shared buffer first. */
            id<MTLBuffer> buf = [s_dev newBufferWithLength:w * hh * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [s_queue commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, hh, 1) toBuffer:buf destinationOffset:0
            destinationBytesPerRow:w * bpp destinationBytesPerImage:w * hh * bpp];
            [bl endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            memcpy(px, [buf contents], w * hh * bpp);
        } else {
            [t getBytes:px bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, hh) mipmapLevel:0];
        }
        char path[1024]; snprintf(path, sizeof path, "%s/f%u_h%u.ppm", dir, frame, h);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%zu %zu\n255\n", w, hh);
            for (size_t p = 0; p < w * hh; ++p) {
                unsigned char rgb[3];
                if (bpp == 4) {
                    int bgra = pf == MTLPixelFormatBGRA8Unorm;
                    rgb[0] = px[p*4 + (bgra ? 2 : 0)]; rgb[1] = px[p*4 + 1]; rgb[2] = px[p*4 + (bgra ? 0 : 2)];
                } else {
                    const uint16_t* q = (const uint16_t*)(px + p * 8);
                    for (int c = 0; c < 3; ++c) { float v = eng_half_to_float(q[c]); v = v < 0 ? 0 : v > 1 ? 1 : v; rgb[c] = (unsigned char)(v * 255.0f + 0.5f); }
                }
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        /* PS3RECOMP_METAL_SURF_DUMP_ALPHA=1: also write the alpha channel as
         * <dir>/f<frame>_h<handle>_a.pgm. A premultiplied particle buffer keeps
         * dark (RGB 0) particles only in alpha. */
        { static int s_alpha = -1;
          if (s_alpha < 0) { const char* e = getenv("PS3RECOMP_METAL_SURF_DUMP_ALPHA"); s_alpha = e && *e == '1'; }
          if (s_alpha) {
              snprintf(path, sizeof path, "%s/f%u_h%u_a.pgm", dir, frame, h);
              FILE* fa = fopen(path, "wb");
              if (fa) {
                  fprintf(fa, "P5\n%zu %zu\n255\n", w, hh);
                  for (size_t p = 0; p < w * hh; ++p) {
                      unsigned char a;
                      if (bpp == 4) a = px[p*4 + 3];
                      else { float v = eng_half_to_float(((const uint16_t*)(px + p * 8))[3]);
                             v = v < 0 ? 0 : v > 1 ? 1 : v; a = (unsigned char)(v * 255.0f + 0.5f); }
                      fputc(a, fa);
                  }
                  fclose(fa);
              }
          } }
        free(px);
    }
    fprintf(stderr, "[rsx engine/metal] dumped render targets for frame %u to %s\n", frame, dir);
}

static void eng_present(void* user, u32 surface)
{
    (void)user;
    id<MTLTexture> src = eng_obj(surface);
    if (!src) { eng_encode_and_commit(nil); return; }
    const u32 nrec = s_eng_rec_count;
    eng_encode_and_commit(src);
    /* PS3_OVERLAY_FORCE_OPEN / _HUD headless: dump the composed target (the
     * menu is drawn there, not on the game's surface). So does
     * PS3RECOMP_METAL_FRAME_DUMP_POST=1: the picture after the present pass
     * (the PS3_SHARPEN .. PS3_LUT_PRESET effects), headless. */
    { static int post = -1;
      if (post < 0) { const char* e = getenv("PS3RECOMP_METAL_FRAME_DUMP_POST"); post = e && *e == '1'; }
      eng_dump_frame((post || ps3_overlay_composes_headless()) && s_offscreen ? s_offscreen : src); }
    { static int dt = -1; if (dt < 0) dt = getenv("PS3RECOMP_METAL_DBG_DUMPTEX") ? 1 : 0;
      if (dt) eng_enc_join(); }   /* s_dumptex is set while encoding */
    if (s_dumptex) { eng_dumptex_write(s_dumptex, s_dumptex_path); s_dumptex = nil; }
    { static unsigned s_sd_min = 1000; static int s_sd_init; static const char* trig;
      if (!s_sd_init) { s_sd_init = 1; const char* e = getenv("PS3RECOMP_METAL_SURF_DUMP_MIN");
          if (e && atoi(e) > 0) s_sd_min = (unsigned)atoi(e);
          trig = getenv("PS3RECOMP_METAL_DUMP_TRIGGER"); }
      const int key = __atomic_exchange_n(&s_capture_req, 0, __ATOMIC_ACQ_REL);
      if (key) fprintf(stderr, "[capture] in-game key\n");
      if (key || (trig && access(trig, F_OK) == 0)) { if (!key) unlink(trig); s_dump_force = 1; s_pass_log_force = 2; eng_dump_surfaces(); }   /* 2: the flush slice and the present slice of the next frame */
      else if (nrec > s_sd_min) eng_dump_surfaces(); }   /* world frames only (_MIN records) */
}

static void eng_readback_body(id<MTLTexture> t, u32 x, u32 y, u32 w, u32 h,
                              void* out, u32 out_pitch);
static void eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                         void* out, u32 out_pitch)
{
    (void)user;
    eng_enc_join();   /* PS3RECOMP_METAL_ENCODE_THREAD: a present may still be encoding */
    id<MTLTexture> t = eng_obj(surface);
    if (!t || !out || !out_pitch || !w || !h) return;
    const u64 ft_t0 = PS3_FT_ON() ? eng_pt_now() : 0;
    if (eng_obj_is_scaled(surface)) {
        /* PS3_RES_SCALE: the guest asks for its own pixels. Read the scaled
         * rectangle and point-sample it down -- the same pixel the guest's
         * would be (no filtering: depth and packed normals must stay exact). */
        const u32 sx0 = res_px_x(x), sy0 = res_px(y), sx1 = res_px_x(x + w), sy1 = res_px(y + h);
        if (sx1 > [t width] || sy1 > [t height] || sx1 <= sx0 || sy1 <= sy0) return;
        const u32 bpp = out_pitch / w, sw = sx1 - sx0, shh = sy1 - sy0;
        u8* big = bpp ? (u8*)malloc((size_t)sw * shh * bpp) : NULL;
        if (!big) return;
        eng_readback_body(t, sx0, sy0, sw, shh, big, sw * bpp);
        for (u32 row = 0; row < h; row++) {
            u32 r = (u32)(((float)row + 0.5f) * s_res_sy); if (r >= shh) r = shh - 1;
            const u8* srow = big + (size_t)r * sw * bpp;
            u8* drow = (u8*)out + (size_t)row * out_pitch;
            for (u32 col = 0; col < w; col++) {
                u32 c = (u32)(((float)col + 0.5f) * s_res_sx); if (c >= sw) c = sw - 1;
                memcpy(drow + (size_t)col * bpp, srow + (size_t)c * bpp, bpp);
            }
        }
        free(big);
        if (ft_t0) ps3_ft_span(PS3_FT_READBACK, 0, w, h, ft_t0, eng_pt_now());
        return;
    }
    if (x + w > [t width] || y + h > [t height]) return;
    eng_readback_body(t, x, y, w, h, out, out_pitch);
    if (ft_t0) ps3_ft_span(PS3_FT_READBACK, 0, w, h, ft_t0, eng_pt_now());
}

static void eng_readback_body(id<MTLTexture> t, u32 x, u32 y, u32 w, u32 h,
                              void* out, u32 out_pitch)
{
    if ([t storageMode] != MTLStorageModePrivate) {
        [t getBytes:out bytesPerRow:out_pitch
         fromRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0];
        return;
    }
    /* A private target has no CPU view: blit it into a shared buffer on the
     * same queue (ordered after everything already committed) and wait. */
    const u32 bpp = (u32)(out_pitch / w);
    id<MTLBuffer> buf = [s_dev newBufferWithLength:(NSUInteger)w * h * bpp
                                           options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
            sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
     destinationBytesPerRow:(NSUInteger)w * bpp destinationBytesPerImage:(NSUInteger)w * h * bpp];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const u8* src = (const u8*)[buf contents];
    for (u32 row = 0; row < h; row++)
        memcpy((u8*)out + (size_t)row * out_pitch, src + (size_t)row * w * bpp, (size_t)w * bpp);
}

/* ---- image files for texture packs (rsx_draw_engine.c, PS3_MODS) --------
 * The samples exactly as the PNG holds them: no colour management, straight
 * alpha, so what PS3_MODS_DUMP writes reads back byte for byte. */
#if !TARGET_OS_IPHONE
static int eng_image_load(void* user, const char* path, u8** out, u32* ow, u32* oh)
{
    (void)user;
    *out = NULL; *ow = *oh = 0;
    @autoreleasepool {
        NSData* d = [NSData dataWithContentsOfFile:@(path)];
        NSBitmapImageRep* rep = d ? [NSBitmapImageRep imageRepWithData:d] : nil;
        if (!rep) return 0;
        const NSInteger w = rep.pixelsWide, h = rep.pixelsHigh;
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return 0;
        u8* px = (u8*)malloc((size_t)w * h * 4);
        if (!px) return 0;
        const NSInteger spp = rep.samplesPerPixel, bpr = rep.bytesPerRow;
        const NSBitmapFormat bf = rep.bitmapFormat;
        const u8* src = rep.bitmapData;
        if (src && rep.bitsPerSample == 8 && !rep.isPlanar && !(bf & NSBitmapFormatFloatingPointSamples) &&
            !(bf & NSBitmapFormatAlphaFirst) && (spp == 1 || spp == 2 || spp == 3 || spp == 4) &&
            (spp == 1 || spp == 3 || (bf & NSBitmapFormatAlphaNonpremultiplied)) &&
            rep.bitsPerPixel == spp * 8) {
            for (NSInteger y = 0; y < h; y++) {
                const u8* r = src + y * bpr;
                u8* o = px + (size_t)y * w * 4;
                for (NSInteger x = 0; x < w; x++, o += 4, r += spp) {
                    if (spp >= 3) { o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; o[3] = spp == 4 ? r[3] : 255; }
                    else          { o[0] = o[1] = o[2] = r[0]; o[3] = spp == 2 ? r[1] : 255; }
                }
            }
        } else {
            /* Anything else (16-bit, palettes, premultiplied): drawn by Core
             * Graphics, which premultiplies, then straightened again. */
            CGImageRef cg = rep.CGImage;
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = cg && cs ? CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                          kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
            if (cs) CGColorSpaceRelease(cs);
            if (!ctx) { free(px); return 0; }
            CGContextSetBlendMode(ctx, kCGBlendModeCopy);
            CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), cg);
            CGContextRelease(ctx);
            for (size_t i = 0; i < (size_t)w * h; i++) {
                u8* o = px + i * 4;
                if (o[3] && o[3] < 255)
                    for (int k = 0; k < 3; k++) { const u32 v = (o[k] * 255u + o[3] / 2u) / o[3]; o[k] = (u8)(v > 255 ? 255 : v); }
            }
        }
        *out = px; *ow = (u32)w; *oh = (u32)h;
        return 1;
    }
}

static int eng_image_save(void* user, const char* path, const u8* rgba, u32 w, u32 h, u32 row_bytes)
{
    (void)user;
    if (!path || !rgba || !w || !h) return 0;
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("ps3recomp.texdump", DISPATCH_QUEUE_SERIAL); });
    NSMutableData* copy = [NSMutableData dataWithLength:(NSUInteger)w * h * 4];
    if (!copy) return 0;
    for (u32 y = 0; y < h; y++)
        memcpy((u8*)copy.mutableBytes + (size_t)y * w * 4, rgba + (size_t)y * row_bytes, (size_t)w * 4);
    NSString* dst = @(path);
    dispatch_async(q, ^{ @autoreleasepool {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
            pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
            bytesPerRow:(NSInteger)w * 4 bitsPerPixel:32];
        if (!rep) return;
        memcpy(rep.bitmapData, copy.bytes, (size_t)w * h * 4);
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:dst atomically:YES])
            fprintf(stderr, "[mods] cannot write %s\n", dst.UTF8String);
    } });
    return 1;
}
#endif

static const rsx_draw_backend s_engine_backend = {
    .user                 = NULL,
#if !TARGET_OS_IPHONE
    .image_load           = eng_image_load,
    .image_save           = eng_image_save,
#endif
    .init                 = eng_init,
    .shutdown             = eng_shutdown,
    .submit_and_wait      = eng_submit_and_wait,
    .texture_create       = eng_texture_create,
    .texture_upload       = eng_texture_upload,
    .texture_release      = eng_obj_release,
    .texture_create_3d    = eng_texture_create_3d,
    .texture_upload_3d    = eng_texture_upload_3d,
    .color_target_create  = eng_color_target_create,
    .color_target_release = eng_obj_release,
    .surface_view         = eng_surface_view,
    .depth_target_create  = eng_depth_target_create,
    .depth_target_release = eng_obj_release,
    .depth_snapshot       = eng_depth_snapshot,
    .color_snapshot       = eng_color_snapshot,
    .depth_snapshot_rgba8 = eng_depth_snapshot_rgba8,
    .light_pass           = eng_light_pass,
    .ssao_pass            = eng_ssao_pass,
    .pipeline_create      = eng_pipeline_create,
    .pipeline_release     = eng_pipeline_release,
    .bind_targets         = eng_bind_targets,
    .bind_pipeline        = eng_bind_pipeline,
    .bind_vs_constants    = eng_bind_vs_constants,
    .bind_vs_constants_range = eng_bind_vs_constants_range,
    .bind_ps_constants    = eng_bind_ps_constants,
    .bind_textures        = eng_bind_textures,
    .bind_vertex_textures = eng_bind_vertex_textures,
    .set_viewport         = eng_set_viewport,
    .set_scissor          = eng_set_scissor,
    .set_stencil_ref      = eng_set_stencil_ref,
    .set_depth_bias       = eng_set_depth_bias,
    .set_depth_clamp      = eng_set_depth_clamp,
    .set_hud              = eng_set_hud,
    .draw                 = eng_draw,
    .vbuf_create          = eng_vbuf_create,
    .vbuf_release         = eng_vbuf_release,
    .draw_vbuf            = eng_draw_vbuf,
    .draw_vbuf_ib         = eng_draw_vbuf_ib,
    .clear_color          = eng_clear_color,
    .clear_depth_stencil  = eng_clear_depth_stencil,
    .present              = eng_present,
    .readback             = eng_readback,
};

/* ---- public API ---------------------------------------------------------- */

int rsx_metal_backend_init(u32 width, u32 height, const char* title)
{
    @autoreleasepool {
        if (width)  s_width  = width;
        if (height) s_height = height;

        const char* hl = getenv("PS3RECOMP_METAL_HEADLESS");
        s_headless = (hl && *hl && *hl != '0');
#if TARGET_OS_IPHONE
        s_headless = 1;
#endif
        gfx_options_load();

        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) {
            fprintf(stderr, "[RSX metal] no Metal device available\n");
            return -1;
        }
        gfx_lut_load(s_dev);
        s_queue = [s_dev newCommandQueue];
        if (!s_queue) {
            fprintf(stderr, "[RSX metal] could not create command queue\n");
            return -1;
        }

        int rc;
#if TARGET_OS_IPHONE
        s_headless = 1;
        rc = create_offscreen();
#else
        rc = s_headless ? create_offscreen() : create_window(title);
#endif
        if (rc != 0) {
            fprintf(stderr, "[RSX metal] surface creation failed\n");
            return -1;
        }
        if (create_depth() != 0) {
            fprintf(stderr, "[RSX metal] depth/stencil buffer creation failed\n");
            return -1;
        }
        if (create_placeholders() != 0) {
            fprintf(stderr, "[RSX metal] placeholder texture creation failed\n");
            return -1;
        }

        s_verts = (MtlVertex*)malloc(sizeof(MtlVertex) * MTL_MAX_VERTS);
        if (!s_verts) {
            fprintf(stderr, "[RSX metal] vertex staging alloc failed\n");
            return -1;
        }

        NSError* serr = nil;
        s_shader_lib = [s_dev newLibraryWithSource:kBuiltinMSL options:nil error:&serr];
        if (!s_shader_lib) {
            fprintf(stderr, "[RSX metal] built-in shader compile failed: %s\n",
                    [[serr localizedDescription] UTF8String]);
            return -1;
        }

        /* PS3RECOMP_METAL_FIXED_FUNCTION=1 pins every draw to the built-in
         * shader: the first switch to flip when a title's draws vanish, to
         * tell a translation problem from a fetch or state one. */
        const char* ff = getenv("PS3RECOMP_METAL_FIXED_FUNCTION");
        s_guest_shaders = rsx_hlsl_to_msl_available() && !(ff && *ff && *ff != '0');

        s_inflight = dispatch_semaphore_create(MTL_MAX_INFLIGHT);
        if (!s_inflight) {
            fprintf(stderr, "[RSX metal] semaphore creation failed\n");
            return -1;
        }

        /* PS3RECOMP_RSX_ENGINE=dispatch drives the register-file draw engine
         * instead of the rsx_state vtable. The vtable stays registered only on
         * the path that uses it: the FIFO walker feeds both models, so leaving
         * both live would record every draw twice. */
        s_ready  = 1;
        s_closed = 0;
        rsx_draw_engine_set_backend(&s_engine_backend);
        /* The engine is this backend's default now that every host mode
         * passes both ways. PS3RECOMP_RSX_ENGINE=vtable is the way back, and
         * it stays that way until the vtable path has nothing left to say. */
        rsx_draw_engine_set_default(1);
        if (rsx_draw_engine_enabled() &&
            rsx_draw_engine_init(s_width, s_height) == 0) {
            s_eng_active = 1;
            rsx_fp_set_wpos_scale_xy(s_res_sx, s_res_sy);   /* PS3_RES_SCALE, PS3_ASPECT */
            if (s_res_scaled)
                fprintf(stderr, "[RSX metal] internal resolution x%.2f, aspect x%.4f: %ux%u targets render at %ux%u\n",
                        (double)s_res_scale, (double)s_aspect_k, s_width, s_height, res_px_x(s_width), res_px(s_height));
        } else {
            if (s_res_scaled) {
                fprintf(stderr, "[RSX metal] PS3_RES_SCALE and PS3_ASPECT need the draw engine; native 16:9\n");
                s_res_scale = 1.0f; s_aspect_k = 1.0f; res_axes_update();
                if (s_headless) create_offscreen();
            }
            rsx_set_backend(&s_backend_vtable);
            /* The post passes live in the engine's present. */
            if (s_gfx_upscale) gfx_upscale_fail("the vtable path has no post passes");
            s_gfx_upscale = 0;
        }
        ps3_overlay_init((__bridge void*)s_dev, s_headless);
        fprintf(stderr, "[RSX metal] %s on %s (%ux%u), guest shaders %s, %s path\n",
                s_headless ? "headless" : "windowed",
                [[s_dev name] UTF8String], s_width, s_height,
                s_guest_shaders ? "on" : (rsx_hlsl_to_msl_available() ? "off (env)" : "off (translator not built)"),
                s_eng_active ? "register-file draw engine" : "vtable");
        return 0;
    }
}

void rsx_metal_backend_shutdown(void)
{
    @autoreleasepool {
        ps3_overlay_shutdown();
        if (s_ready) rsx_set_backend(NULL);
        if (s_eng_active) rsx_draw_engine_shutdown();
        rsx_draw_engine_set_backend(NULL);
        /* Reclaim every in-flight slot so no command buffer is still
         * referencing the device, queue or textures when they are released,
         * then hand the slots back. libdispatch traps if a semaphore is
         * deallocated while its count is below the value it was created with,
         * so draining without restoring is a crash, not a leak. */
        if (s_inflight) {
            for (int i = 0; i < MTL_MAX_INFLIGHT; i++)
                dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
            for (int i = 0; i < MTL_MAX_INFLIGHT; i++)
                dispatch_semaphore_signal(s_inflight);
            s_inflight = nil;
        }
#if !TARGET_OS_IPHONE
        /* Disabled/headless runs never create a tracker and must not gain a
         * dependency on a pumping AppKit main queue during worker teardown. */
        if (fps_session()) {
            if ([NSThread isMainThread]) fps_stop();
            else dispatch_sync(dispatch_get_main_queue(), ^{ fps_stop(); });
        }
        if (s_window) { [s_window close]; s_window = nil; }
#endif
        free(s_verts); s_verts = NULL;
        free(s_cb); s_cb = NULL; s_cb_used = s_cb_cap = 0;
        free(s_tex_staging); s_tex_staging = NULL; s_tex_staging_cap = 0;
        s_vert_count = s_rec_count = 0;
        s_guest_draws = s_last_guest_draws = 0;
        clear_caches();
        release_surfaces();
        s_shader_lib = nil;
        s_null_tex   = nil;
        s_default_sampler = nil;
        s_ds_default = nil;
        s_layer     = nil;
        s_offscreen = nil;
        s_depth     = nil;
        s_queue     = nil;
        s_dev       = nil;
        s_ready     = 0;
    }
}

static int pump_messages_impl(void)
{
#if !TARGET_OS_IPHONE
    if (s_headless || !s_ready) return 0;
    @autoreleasepool {
        for (;;) {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            /* Cmd+Q quits, menu open or not, on the first press. There is no
             * app menu (the host turns a plain run loop) to give AppKit's
             * Quit, and the keyboard pad swallows every Cmd key, so before
             * this nothing happened. It ends the game the way the menu's
             * "Quit game" does (cellSysutil.c ps3_host_quit), without asking:
             * Cmd+Q is a deliberate key on the Mac. */
            if ([ev type] == NSEventTypeKeyDown && ([ev keyCode] & 127u) == 0x0C &&
                ([ev modifierFlags] & NSEventModifierFlagCommand)) {
                if (![ev isARepeat]) {
                    extern void ps3_overlay_quit_notice(void);
                    extern void ps3_host_quit(const char* why);
                    ps3_overlay_quit_notice();
                    ps3_host_quit("Cmd+Q");
                }
                continue;
            }
            /* The in-game menu (ps3_overlay.mm) first: its keys open it, and
             * while it is open it takes the keyboard and the mouse. A mouse
             * event it took still goes to AppKit (focus, the title bar). */
            {   NSView* v = [s_window contentView];
                const NSSize vs = v ? [v bounds].size : NSZeroSize;
                const CGSize ds = s_layer ? s_layer.drawableSize : CGSizeZero;
                if (ps3_overlay_handle_event((__bridge void*)ev, vs.width, vs.height, ds.width, ds.height)) {
                    const NSEventType t = [ev type];
                    if (t != NSEventTypeKeyDown && t != NSEventTypeKeyUp && t != NSEventTypeFlagsChanged)
                        [NSApp sendEvent:ev];
                    continue;
                }
            }
            /* The mouse drives the right stick and R1/R2 (cellPad.c). Motion
             * accumulates as deltas in 1/16 px, which cellPad drains on each
             * poll; buttons count only inside the game view, so dragging the
             * title bar does not fire a bolt. The events still go to AppKit,
             * which is what focuses the window on a click.
             *
             * With capture on (the default while the mouse camera is), the
             * mouse belongs to the game only while captured: a left click in
             * the game captures it (that click fires nothing), Esc, Cmd,
             * another window or another app release it, and in between the
             * cursor is free and moving it turns nothing. */
            {   extern volatile int32_t  g_pad_mouse_dx, g_pad_mouse_dy;
                extern volatile uint32_t g_pad_mouse_buttons;
                extern int cellPad_host_mouse_active(void);
                const NSEventType t = [ev type];
                const int motion = (t == NSEventTypeMouseMoved || t == NSEventTypeLeftMouseDragged ||
                                    t == NSEventTypeRightMouseDragged || t == NSEventTypeOtherMouseDragged);
                /* Uncaptured (PS3_MOUSE_CAPTURE=0), motion counts whenever the
                 * game window is key: a moved event carries no window once the
                 * cursor leaves it, and the camera should keep turning then. */
                const int take_motion = s_cap_on ? (s_captured && [ev timestamp] >= s_cap_since)
                                                 : (s_window && [s_window isKeyWindow]);
                if (motion && take_motion) {
                    __atomic_add_fetch(&g_pad_mouse_dx, (int32_t)lround([ev deltaX] * 16.0), __ATOMIC_RELAXED);
                    __atomic_add_fetch(&g_pad_mouse_dy, (int32_t)lround([ev deltaY] * 16.0), __ATOMIC_RELAXED);
                }
                if (!motion && s_window && [ev window] == s_window) {
                    const NSPoint at = [ev locationInWindow];
                    const int inside = NSPointInRect(at, [[s_window contentView] frame]);
                    uint32_t bit = 0;
                    if (t == NSEventTypeLeftMouseDown  || t == NSEventTypeLeftMouseUp)  bit = 1u;
                    if (t == NSEventTypeRightMouseDown || t == NSEventTypeRightMouseUp) bit = 2u;
                    /* middle = 4, then button 4 = 8, ... (PS3_INPUT_MAP binds them) */
                    if (t == NSEventTypeOtherMouseDown || t == NSEventTypeOtherMouseUp)
                        bit = 1u << ([ev buttonNumber] < 2 ? 2 : [ev buttonNumber] > 7 ? 7 : [ev buttonNumber]);
                    if (bit) {
                        const int down = (t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown ||
                                          t == NSEventTypeOtherMouseDown);
                        if (!down) {
                            __atomic_and_fetch(&g_pad_mouse_buttons, ~bit, __ATOMIC_RELAXED);
                        } else if (!s_cap_on) {
                            if (inside && [s_window isKeyWindow])
                                __atomic_or_fetch(&g_pad_mouse_buttons, bit, __ATOMIC_RELAXED);
                        } else if (s_captured) {
                            __atomic_or_fetch(&g_pad_mouse_buttons, bit, __ATOMIC_RELAXED);
                        } else if (inside && bit == 1u && cellPad_host_mouse_active()) {
                            mouse_capture();
                        }
                    }
                }
                /* Cmd releases the mouse, so Cmd+Tab and the rest of the
                 * system's shortcuts find a cursor. */
                if (t == NSEventTypeFlagsChanged && s_captured &&
                    ([ev modifierFlags] & NSEventModifierFlagCommand))
                    mouse_release();
                /* The wheel, in 1/16 lines with + = turned away from the
                 * player whatever the scroll direction setting; it reaches a
                 * PS3 control only if PS3_INPUT_MAP binds WheelUp/WheelDown. */
                if (t == NSEventTypeScrollWheel &&
                    (s_cap_on ? s_captured : (s_window && [ev window] == s_window && [s_window isKeyWindow]))) {
                    extern volatile int32_t g_pad_mouse_wheel;
                    const double dy = [ev isDirectionInvertedFromDevice] ? -[ev deltaY] : [ev deltaY];
                    __atomic_add_fetch(&g_pad_mouse_wheel, (int32_t)lround(dy * 16.0), __ATOMIC_RELAXED);
                }
            }
            /* Shift, Control and Option are keys of the keyboard pad as well
             * (PS3_INPUT_MAP can bind them): they come as flag changes, and
             * the device-dependent bits tell left from right. The event
             * still goes on to AppKit. */
            if ([ev type] == NSEventTypeFlagsChanged) {
                static const struct { unsigned short vk; NSUInteger bit; } mods[] = {
                    {0x38, 0x02}, {0x3C, 0x04},       /* Shift, right Shift */
                    {0x3B, 0x01}, {0x3E, 0x2000},     /* Control, right Control */
                    {0x3A, 0x20}, {0x3D, 0x40},       /* Option, right Option */
                };
                extern volatile uint32_t g_pad_host_keys[4];
                const unsigned vk = [ev keyCode] & 127u;
                for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++) {
                    if (mods[i].vk != vk) continue;
                    if ([ev modifierFlags] & mods[i].bit)
                        __atomic_or_fetch(&g_pad_host_keys[vk >> 5], 1u << (vk & 31), __ATOMIC_RELAXED);
                    else
                        __atomic_and_fetch(&g_pad_host_keys[vk >> 5], ~(1u << (vk & 31)), __ATOMIC_RELAXED);
                }
            }
            /* Keys drive the keyboard pad (cellPad.c): record and swallow
             * them -- unhandled, AppKit beeps on every press. The window's
             * own shortcuts come first and never reach the pad, and nothing
             * typed with Cmd held does (Cmd+W is not a step forward). */
            if ([ev type] == NSEventTypeKeyDown || [ev type] == NSEventTypeKeyUp) {
                extern volatile uint32_t g_pad_host_keys[4];
                const unsigned vk = [ev keyCode] & 127u;
                if ([ev type] == NSEventTypeKeyDown) {
                    const NSEventModifierFlags m = [ev modifierFlags];
                    const int cmd = (m & NSEventModifierFlagCommand) != 0;
                    const int ctl = (m & NSEventModifierFlagControl) != 0;
                    const int opt = (m & NSEventModifierFlagOption)  != 0;
                    if (vk == 0x03 && cmd) {                                   /* Cmd+F, Cmd+Ctrl+F */
                        if (![ev isARepeat]) wm_toggle(ctl ? WM_FULLSCREEN : s_wm_fs_style);
                        continue;
                    }
                    if ((vk == 0x24 || vk == 0x4C) && opt && !cmd) {           /* Option+Return/Enter */
                        if (![ev isARepeat]) wm_toggle(s_wm_fs_style);
                        continue;
                    }
                    if (vk == 0x35 && s_captured) { mouse_release(); continue; } /* Esc */
                    if (cmd) continue;
                    __atomic_or_fetch(&g_pad_host_keys[vk >> 5], 1u << (vk & 31), __ATOMIC_RELAXED);
                } else {
                    __atomic_and_fetch(&g_pad_host_keys[vk >> 5], ~(1u << (vk & 31)), __ATOMIC_RELAXED);
                }
                continue;
            }
            [NSApp sendEvent:ev];
        }
        /* The menu opened (a key here, or L3+R3 seen by cellPad): the game's
         * keys and mouse let go, and the cursor is free for the menu. */
        {   static int was_open;
            const int open = ps3_overlay_menu_open();
            if (open && !was_open) {
                extern volatile uint32_t g_pad_host_keys[4];
                for (int i = 0; i < 4; i++) g_pad_host_keys[i] = 0;
                extern volatile uint32_t g_pad_mouse_buttons;
                g_pad_mouse_buttons = 0;
                mouse_release();
            }
            was_open = open; }
        /* The menu switched the mouse camera off (or to controller only). */
        { extern int cellPad_host_mouse_active(void);
          if (s_captured && !cellPad_host_mouse_active()) mouse_release(); }
        s_wm_mode_now = s_wm_borderless ? WM_BORDERLESS : wm_native_fullscreen() ? WM_FULLSCREEN : WM_WINDOWED;
        /* No key-ups arrive while another app has focus: release everything. */
        if (![NSApp isActive]) {
            extern volatile uint32_t g_pad_host_keys[4];
            for (int i = 0; i < 4; i++) g_pad_host_keys[i] = 0;
            extern volatile uint32_t g_pad_mouse_buttons;
            g_pad_mouse_buttons = 0;
            extern volatile int32_t g_pad_mouse_wheel;
            g_pad_mouse_wheel = 0;
            mouse_release();
        }
        wm_keep_drawable();
        /* Closing is the delegate's windowWillClose. "Not visible" is not
         * closed: a minimised window is not visible either, and neither is
         * one between Spaces in a fullscreen transition. */
    }
#endif
    return s_closed ? -1 : 0;
}

int rsx_metal_backend_pump_messages(void)
{
    if (s_headless || !s_ready) return s_closed ? -1 : 0;
#if !TARGET_OS_IPHONE
    if ([NSThread isMainThread])
        return pump_messages_impl();
    __block int result = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = pump_messages_impl();
    });
    return result;
#else
    return pump_messages_impl();
#endif
}

void rsx_metal_backend_present(void)
{
    if (!s_ready) return;
    /* Under the draw engine the frame belongs to it: a host that drives the
     * flip itself (the harness, the boot smoke title) calls here, and a title
     * whose FIFO carries 0xE944 presents through the engine's own sink. */
    if (s_eng_active) { rsx_draw_engine_present(); return; }
    /* Block only when MTL_MAX_INFLIGHT frames are already queued. */
    if (!s_headless) dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
    @autoreleasepool {
        id<MTLTexture> target = nil;
        id<CAMetalDrawable> drawable = nil;

        if (s_headless) {
            target = s_offscreen;
        } else {
            drawable = [s_layer nextDrawable];
            if (!drawable) {            /* compositor is busy; skip this frame */
                dispatch_semaphore_signal(s_inflight);
                return;
            }
            target = [drawable texture];
        }
        if (!target) {
            if (!s_headless) dispatch_semaphore_signal(s_inflight);
            return;
        }

        /* The frame's first pass is on the display and clears colour to the
         * last value the guest asked for, which is what this backend has
         * always done and what keeps a frame with no clear in it from
         * presenting an undefined drawable. Depth and stencil start at the
         * nv40 reset values, unless a clear ahead of the first draw says
         * otherwise -- those clears are folded into this pass rather than each
         * opening one of their own. Only display clears fold: one aimed at a
         * surface belongs to a pass somewhere else. */
        MtlClear first;
        memset(&first, 0, sizeof first);
        first.flags   = MTL_CLEAR_COLOR | MTL_CLEAR_DEPTH | MTL_CLEAR_STENCIL;
        first.color   = s_clear_argb;
        first.depth   = 1.0f;
        first.stencil = 0;
        u32 r = 0;
        for (; r < s_rec_count && s_records[r].kind == MTL_REC_CLEAR
                                && s_records[r].u.clear.rt.off == 0; r++) {
            const MtlClear* c = &s_records[r].u.clear;
            if (c->flags & MTL_CLEAR_DEPTH)   first.depth   = c->depth;
            if (c->flags & MTL_CLEAR_STENCIL) first.stencil = c->stencil;
        }
        /* What a pass opened by a target change asks for: no clear flags at
         * all, so every attachment loads what the surface already holds. */
        static const MtlClear load_all = { .flags = 0 };

        /* Designated rather than memset: the buffers are ARC-managed. */
        MtlPassState ps = { .vb = nil, .cbuf = nil };
        if (s_vert_count > 0) {
            ps.vb = [s_dev newBufferWithBytes:s_verts
                                       length:sizeof(MtlVertex) * s_vert_count
                                      options:MTLResourceStorageModeShared];
            if (s_cb_used)
                ps.cbuf = [s_dev newBufferWithBytes:s_cb length:s_cb_used
                                            options:MTLResourceStorageModeShared];
        }

        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = begin_pass(cb, target, &first.rt, &first, &ps);
        for (; enc && r < s_rec_count; r++) {
            const MtlRecord* rec = &s_records[r];
            if (rec->kind == MTL_REC_CLEAR) {
                [enc endEncoding];
                enc = begin_pass(cb, target, &rec->u.clear.rt, &rec->u.clear, &ps);
                continue;
            }
            /* A draw aimed somewhere else ends the pass and opens the next,
             * for the same reason a clear does: an encoder is bound to one set
             * of attachments for its whole life. */
            if (!target_same(&rec->u.draw.rt, &ps.rt)) {
                [enc endEncoding];
                enc = begin_pass(cb, target, &rec->u.draw.rt, &load_all, &ps);
                if (!enc) break;
            }
            encode_draw(enc, &rec->u.draw, &ps);
        }
        if (enc) [enc endEncoding];
        if (__builtin_expect(g_ps3_overlay_active, 0)) ps3_overlay_render((__bridge void*)cb, (__bridge void*)target);

        if (drawable) { fps_track_drawable(drawable); [cb presentDrawable:drawable]; }

        if (s_headless) {
            /* The readback below must see a completed frame. */
            [cb commit];
            [cb waitUntilCompleted];
        } else {
            dispatch_semaphore_t sem = s_inflight;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> _unused) {
                (void)_unused;
                dispatch_semaphore_signal(sem);
            }];
            [cb commit];        /* no wait: the CPU goes on to the next frame */
        }

        if (s_dropped_records) {
            fprintf(stderr, "[RSX metal] dropped %u record(s) this frame (cap %d records / %u verts)\n",
                    s_dropped_records, MTL_MAX_RECORDS, MTL_MAX_VERTS);
            s_dropped_records = 0;
        }
        s_rec_count  = 0;
        s_vert_count = 0;
        s_cb_used    = 0;
        s_last_guest_draws = s_guest_draws;
        s_guest_draws = 0;
        /* The frame's records are gone, so slots may move now. The command
         * buffer keeps its own references to whatever it still runs. */
        if (s_caches_full) {
            fprintf(stderr, "[RSX metal] cache full (%u VP, %u FP, %u PSO, %u textures,"
                            " %u surface views); cleared\n",
                    s_vp_count, s_fp_count, s_pso_count, s_tex_count, s_rtview_count);
            clear_caches();
        }

        if (s_headless && s_offscreen) {
            u32 px = 0;
            MTLRegion r = MTLRegionMake2D(s_width / 2, s_height / 2, 1, 1);
            [s_offscreen getBytes:&px bytesPerRow:4 fromRegion:r mipmapLevel:0];
            s_last_present_bgra = px;
        }
    }
}

u32 rsx_metal_backend_debug_color(void)     { return s_clear_argb; }
/* Under the draw engine the presented image is a surface of the engine's own,
 * so the sample comes from there rather than from the drawable it was blitted
 * onto: it is the same pixel one step earlier, and it needs no window. */
u32 rsx_metal_backend_readback_center(void)
{
    return s_eng_active ? rsx_draw_engine_readback_center() : s_last_present_bgra;
}
/* The draw engine has no fixed-function fallback: every draw it executes runs
 * the guest's own programs, so its own count is the answer under that path. */
u32 rsx_metal_backend_guest_draws(void)
{
    return s_eng_active ? rsx_draw_engine_guest_draws() : s_last_guest_draws;
}

/* ---- live settings for the in-game menu (ps3_overlay.mm) -------------------
 * The options above are read once at init; the menu changes them while the
 * game runs. The render thread reads s_gfx_* at every present; the window's
 * state belongs to the main thread, so those setters hop onto it. */
float rsx_metal_gfx_sharpen(void)          { return s_gfx_sharpen; }
void  rsx_metal_gfx_set_sharpen(float v)   { s_gfx_sharpen = !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v; }
float rsx_metal_gfx_vibrance(void)         { return s_gfx_vibrance; }
void  rsx_metal_gfx_set_vibrance(float v)  { s_gfx_vibrance = !(v == v) ? 0.0f : v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }
float rsx_metal_gfx_vignette(void)         { return s_gfx_vignette; }
void  rsx_metal_gfx_set_vignette(float v)  { s_gfx_vignette = !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v; }
float rsx_metal_gfx_grain(void)            { return s_gfx_grain; }
void  rsx_metal_gfx_set_grain(float v)     { s_gfx_grain = !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v; }
/* -1: no LUT (PS3_LUT unset or unreadable, and no preset). */
float rsx_metal_gfx_lut_strength(void)     { return (s_gfx_lut_file || s_gfx_lut_preset) ? s_gfx_lut_strength : -1.0f; }
float rsx_metal_gfx_curve(int zone)        { return zone == 0 ? s_gfx_shadows : zone == 1 ? s_gfx_midtones : s_gfx_highlights; }
void  rsx_metal_gfx_set_curve(int zone, float v)
{
    v = !(v == v) ? 0.0f : v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    if (zone == 0) s_gfx_shadows = v; else if (zone == 1) s_gfx_midtones = v; else s_gfx_highlights = v;
}
float rsx_metal_gfx_clarity(void)          { return s_gfx_clarity; }
void  rsx_metal_gfx_set_clarity(float v)   { s_gfx_clarity = !(v == v) ? 0.0f : v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }
/* 0 none, 1 warm, 2 cool, 3 film, 4 vivid; the present picks it up. */
int   rsx_metal_gfx_lut_preset(void)       { return s_gfx_lut_preset; }
void  rsx_metal_gfx_set_lut_preset(int p)  { s_gfx_lut_preset = p > 0 && p < GFX_LUT_N ? p : 0; }
/* 1 when PS3_LUT loaded a file (the preset "off" then shows it). */
int   rsx_metal_gfx_lut_file(void)         { return s_gfx_lut_file != nil; }
void  rsx_metal_gfx_set_lut_strength(float v) { s_gfx_lut_strength = !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v; }
float rsx_metal_gfx_contrast(void)         { return s_gfx_contrast; }
void  rsx_metal_gfx_set_contrast(float v)  { s_gfx_contrast = !(v > 0.0f) ? 1.0f : v < 0.5f ? 0.5f : v > 1.5f ? 1.5f : v; }
int   rsx_metal_gfx_fxaa(void)             { return s_gfx_fxaa; }
void  rsx_metal_gfx_set_fxaa(int on)       { s_gfx_fxaa = on != 0; if (on) s_gfx_smaa = 0; }
/* 0 off, 1 FXAA, 2 SMAA. */
int   rsx_metal_gfx_aa(void)               { return s_gfx_fxaa ? 1 : s_gfx_smaa ? 2 : 0; }
void  rsx_metal_gfx_set_aa(int mode)       { s_gfx_fxaa = mode == 1; s_gfx_smaa = mode == 2; }
int   rsx_metal_gfx_aniso(void)            { return s_gfx_aniso; }
/* PS3_RES_SCALE in effect (fixed for the session); -1 on the vtable path. */
float rsx_metal_res_scale(void)            { return s_eng_active ? s_res_scale : -1.0f; }
float rsx_metal_aspect(void)               { return s_eng_active ? s_aspect_k : -1.0f; }
/* MetalFX: 0 off, 1 on, -1 not possible here (headless, vtable path, no
 * MetalFX in this build); asked for but given up shows as off. */
int rsx_metal_gfx_upscale(void)
{
#if GFX_HAVE_METALFX
    if (s_headless || !s_eng_active) return -1;
    return __atomic_load_n(&s_gfx_upscale_on, __ATOMIC_ACQUIRE) ? 1 : 0;
#else
    return -1;
#endif
}
void rsx_metal_gfx_set_upscale(int on)
{
    if (rsx_metal_gfx_upscale() < 0) return;
    s_gfx_upscale = on != 0;
    __atomic_store_n(&s_gfx_upscale_on, s_gfx_upscale, __ATOMIC_RELEASE);
    /* the next pump resizes the drawable (wm_keep_drawable) */
}

/* Cumulative present timings (see eng_pt_report): in-flight wait, nextDrawable,
 * encode, commit in ns, and the presents they cover. */
void rsx_metal_present_times(unsigned long long ns[4], unsigned* n)
{
    for (int i = 0; i < 4; i++) ns[i] = s_pt_ns[i];
    *n = s_pt_n;
}

#if !TARGET_OS_IPHONE
static volatile int s_vsync_on = 1;
int  rsx_metal_vsync(void) { return s_layer ? s_vsync_on : -1; }
void rsx_metal_set_vsync(int on)
{
    if (!s_layer) return;
    s_vsync_on = on != 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (@available(macOS 10.13, *)) if (s_layer) s_layer.displaySyncEnabled = s_vsync_on ? YES : NO;
    });
}
/* 0 windowed, 1 macOS fullscreen, 2 borderless (WM_*); -1 headless. */
int rsx_metal_window_mode(void) { return s_window ? s_wm_mode_now : -1; }
void rsx_metal_set_window_mode(int mode)
{
    if (!s_window || mode < WM_WINDOWED || mode > WM_BORDERLESS) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!s_window) return;
        if (mode != WM_WINDOWED) s_wm_fs_style = mode;   /* what Cmd+F enters from now on */
        if (s_wm_borderless) {
            if (mode == WM_BORDERLESS) return;
            wm_leave_borderless();
            if (mode == WM_FULLSCREEN)
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (s_window && !wm_native_fullscreen()) [s_window toggleFullScreen:nil];
                });
        } else if (wm_native_fullscreen()) {
            if (mode == WM_FULLSCREEN) return;
            s_wm_after_exit = mode;            /* borderless once the animation is over */
            [s_window toggleFullScreen:nil];
        } else if (mode == WM_BORDERLESS) {
            wm_enter_borderless();
        } else if (mode == WM_FULLSCREEN) {
            [s_window toggleFullScreen:nil];
        }
    });
}
#else
int  rsx_metal_vsync(void) { return -1; }
void rsx_metal_set_vsync(int on) { (void)on; }
int  rsx_metal_window_mode(void) { return -1; }
void rsx_metal_set_window_mode(int mode) { (void)mode; }
#endif
void rsx_metal_note_vsync(int on)
{
#if !TARGET_OS_IPHONE
    s_vsync_on = on != 0;
#else
    (void)on;
#endif
}
