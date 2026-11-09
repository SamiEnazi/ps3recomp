/*
 * ps3_lightfx -- the PC lighting options (the in-game menu's "Lighting" page,
 * libs/video/vk_overlay_light.inc). Each one improves a part of the frame
 * this port computes itself; every one is OFF by default, and off is the
 * game exactly as before (same shaders, same passes, same bytes).
 *
 *   PS3RECOMP_SOFT_SHADOWS=0..3   filter of the shadow-map compares
 *       (rsx_fp_decompiler.c, fp_shadow_helper): 0 = the RSX's own 2x2
 *       bilinear PCF, as the PS3 shows it; 1/2/3 = a tent filter over 4x4 /
 *       6x6 / 8x8 shadow texels (4 / 9 / 16 gathers). inFAMOUS 2's sun
 *       shadows are one such lookup per pixel into a 1024x1024 cascade, so
 *       their edges are stair-stepped at 720p; the tent smooths them.
 *       Filtered and point (one hard compare) lookups alike.
 *   PS3RECOMP_SSAO=0..2           ambient occlusion (rsx_vulkan_backend.c,
 *       vk_eng_ssao_pass; needs the GPU SSAO path, PS3RECOMP_ENG_GPU_SSAO):
 *       0 = the game's half-resolution AO (12 samples, bit-exact with the
 *       SPU job); 1 = the same occlusion formula at full resolution, the
 *       sample pattern rotated per pixel and a wider depth-aware blur (finer
 *       contact shadows, no blocky halos); 2 = 1 with 24 samples.
 *   PS3RECOMP_SSAO_STRENGTH=0..300  AO strength in percent (100 = the
 *       game's): occlusion o becomes o * strength / 100. Any path.
 *   PS3RECOMP_LIGHT_DITHER=0|1    the point/spot light buffer (8 bits per
 *       channel, as the composite reads it) dithered instead of truncated:
 *       no banding in the soft light falloffs on walls at night, same mean
 *       brightness. Needs the GPU light path (PS3RECOMP_ENG_GPU_LIGHT).
 *   PS3RECOMP_TONE_SHADOWS / _MIDTONES / _HIGHLIGHTS=-100..100  a tone curve
 *       on the finished frame (rsx_vulkan_backend.c, vk_lfx_tone; the Metal
 *       backend's PS3_SHADOWS / PS3_MIDTONES / PS3_HIGHLIGHTS ported): each
 *       lifts (or, negative, lowers) its third of the range by up to 0.1
 *       while black and white stay put; 0 = off.
 *   PS3RECOMP_CLARITY=-100..100   local contrast (Metal's PS3_CLARITY): the
 *       luminance minus a wide blur of it (~1.1% of the frame height), added
 *       back towards the midtones; 0 = off. These four change the look: a PC
 *       extra, clearly labelled as one in the menu.
 *
 * The module that owns an effect registers its variable with
 * ps3_setting_register (ps3_settings.h) and calls ps3_lightfx_set() from the
 * applier; hot paths read ps3_lightfx_get() (one relaxed load).
 *
 * Tests: with PS3_TOGGLE_FILE set, ps3_lightfx_test_poll() (the draw engine
 * calls it once a frame) also takes these names from the toggle file, so one
 * run can A/B them at known moments (tools/toggle_schedule.ps1). Without
 * PS3_TOGGLE_FILE it returns after one load.
 *
 * Header-only state (one shared object, as in ps3_settings.h).
 */
#ifndef PS3EMU_PS3_LIGHTFX_H
#define PS3EMU_PS3_LIGHTFX_H

#include <stdlib.h>
#include <string.h>
#include "ps3_toggles.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_LIGHTFX_SHARED __declspec(selectany)
#else
#  define PS3_LIGHTFX_SHARED __attribute__((weak))
#endif

enum ps3_lightfx_id {
    PS3_LFX_SOFT_SHADOWS = 0,   /* 0..3 */
    PS3_LFX_SSAO,               /* 0..2 */
    PS3_LFX_SSAO_STRENGTH,      /* percent, 0..300 */
    PS3_LFX_LIGHT_DITHER,       /* 0|1 */
    PS3_LFX_TONE_SHADOWS,       /* percent, -100..100 */
    PS3_LFX_TONE_MIDTONES,
    PS3_LFX_TONE_HIGHLIGHTS,
    PS3_LFX_CLARITY,
    PS3_LFX_COUNT
};

static inline const char* ps3_lightfx_name(int id)
{
    static const char* const n[PS3_LFX_COUNT] = {
        "PS3RECOMP_SOFT_SHADOWS", "PS3RECOMP_SSAO", "PS3RECOMP_SSAO_STRENGTH", "PS3RECOMP_LIGHT_DITHER",
        "PS3RECOMP_TONE_SHADOWS", "PS3RECOMP_TONE_MIDTONES", "PS3RECOMP_TONE_HIGHLIGHTS", "PS3RECOMP_CLARITY" };
    return (id >= 0 && id < PS3_LFX_COUNT) ? n[id] : "";
}

typedef struct {
    int seeded;                 /* 0 no, 1 seeding, 2 done */
    int value[PS3_LFX_COUNT];
    int gen;                    /* bumped on every change */
    int test;                   /* 0 not read yet, 1 no PS3_TOGGLE_FILE, 2 poll it */
} ps3_lightfx_state;

PS3_LIGHTFX_SHARED ps3_lightfx_state g_ps3_lightfx = { 0 };

/* The value a setting's text means, clamped to its range ("" or junk = off,
 * the strength's off = 100). */
static inline int ps3_lightfx_parse(int id, const char* v)
{
    const int def = id == PS3_LFX_SSAO_STRENGTH ? 100 : 0;
    if (!v || !*v) return def;
    char* end = NULL;
    const double d = strtod(v, &end);
    if (end == v) return def;
    int x = (int)(d + (d >= 0 ? 0.5 : -0.5));
    switch (id) {
    case PS3_LFX_SOFT_SHADOWS:  return x < 0 ? 0 : x > 3 ? 3 : x;
    case PS3_LFX_SSAO:          return x < 0 ? 0 : x > 2 ? 2 : x;
    case PS3_LFX_SSAO_STRENGTH: return x < 0 ? 0 : x > 300 ? 300 : x;
    case PS3_LFX_LIGHT_DITHER:  return x != 0;
    case PS3_LFX_TONE_SHADOWS: case PS3_LFX_TONE_MIDTONES: case PS3_LFX_TONE_HIGHLIGHTS: case PS3_LFX_CLARITY:
                                return x < -100 ? -100 : x > 100 ? 100 : x;
    default:                    return def;
    }
}

static inline void ps3_lightfx_seed(void)
{
    ps3_lightfx_state* s = &g_ps3_lightfx;
    if (__builtin_expect(__atomic_load_n(&s->seeded, __ATOMIC_ACQUIRE) == 2, 1)) return;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->seeded, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&s->seeded, __ATOMIC_ACQUIRE) != 2) { }
        return;
    }
    for (int id = 0; id < PS3_LFX_COUNT; id++)
        s->value[id] = ps3_lightfx_parse(id, getenv(ps3_lightfx_name(id)));
    __atomic_store_n(&s->seeded, 2, __ATOMIC_RELEASE);
}

static inline int ps3_lightfx_get(int id)
{
    ps3_lightfx_seed();
    return __atomic_load_n(&g_ps3_lightfx.value[id], __ATOMIC_RELAXED);
}

static inline int ps3_lightfx_gen(void)
{
    return __atomic_load_n(&g_ps3_lightfx.gen, __ATOMIC_ACQUIRE);
}

/* Store a parsed value; 1 when it changed. */
static inline int ps3_lightfx_store(int id, int x)
{
    ps3_lightfx_seed();
    if (id < 0 || id >= PS3_LFX_COUNT) return 0;
    if (__atomic_exchange_n(&g_ps3_lightfx.value[id], x, __ATOMIC_ACQ_REL) == x) return 0;
    __atomic_fetch_add(&g_ps3_lightfx.gen, 1, __ATOMIC_ACQ_REL);
    return 1;
}

/* For a module's applier: the menu's text. 1 when it changed. */
static inline int ps3_lightfx_set(int id, const char* v)
{
    return ps3_lightfx_store(id, ps3_lightfx_parse(id, v));
}

/* PS3_TOGGLE_FILE runs only: take the names from the toggle file (a name
 * the file does not set keeps its value). Returns a bit per changed id. */
static inline unsigned ps3_lightfx_test_poll(void)
{
    /* test: 0 not read yet (the zero-initialised state), 1 no toggle file, 2 poll it */
    int t = __atomic_load_n(&g_ps3_lightfx.test, __ATOMIC_RELAXED);
    if (__builtin_expect(t == 1, 1)) return 0;
    if (t == 0) {
        const char* e = getenv("PS3_TOGGLE_FILE");
        t = (e && *e) ? 2 : 1;
        __atomic_store_n(&g_ps3_lightfx.test, t, __ATOMIC_RELAXED);
        if (t == 1) return 0;
    }
    unsigned changed = 0;
    for (int id = 0; id < PS3_LFX_COUNT; id++) {
        const int v = ps3_toggle_get(ps3_lightfx_name(id), -99999);
        if (v == -99999) continue;
        char b[16];
        snprintf(b, sizeof b, "%d", v);
        if (ps3_lightfx_set(id, b)) changed |= 1u << id;
    }
    return changed;
}

#ifdef __cplusplus
}
#endif

#endif
