#include <sys/stat.h>   /* mkdir: PS3RECOMP_ENG_OCCDUMP_TRIGGER */
#if !defined(_WIN32)
#include <dirent.h>     /* PS3_MODS: the packs' folders */
#endif
#include "../../include/ps3emu/log_first.h"   /* PS3_LOG_FIRST */
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include <time.h>
/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"

#include "rsx_button_icons.h"   /* prompt glyphs that follow the controller */
#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"
#include "rsx_wtrack.h"         /* page-protection write tracking of cached guest bytes */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/ps3emu/ps3_toggles.h"   /* PS3_TOGGLE_FILE: live knobs */
#include "../../include/ps3emu/ps3_ftrace.h"    /* PS3_FTRACE: compile / texture spans */
#include "../../include/ps3emu/ps3_settings.h"  /* the PC menu's live settings (PS3RECOMP_DOF, ...) */

/* Knobs PS3_TOGGLE_FILE can change while the game runs (their watchers only
 * store; the RSX thread reads). */
static int s_texhash_full = -1;   /* PS3RECOMP_ENG_TEXHASH_FULL */
static void eng_texhash_full_set(int id, int on) { (void)id; __atomic_store_n(&s_texhash_full, on != 0, __ATOMIC_RELAXED); }
static int s_skip_fp_dirty;       /* PS3RECOMP_ENG_SKIP_FP changed: re-read the list */
static void eng_skip_fp_set(int id, int v) { (void)id; (void)v; __atomic_store_n(&s_skip_fp_dirty, 1, __ATOMIC_RELAXED); }
static int s_dbounds_on = -1;     /* PS3RECOMP_ENG_DEPTH_BOUNDS (live); -1 not read yet */
static void eng_dbounds_set(int id, int v) { (void)id; __atomic_store_n(&s_dbounds_on, v != 0, __ATOMIC_RELAXED); }
/* PS3RECOMP_ENG_SHADOW_CMP (live): the depth compare's operand order
 * (rsx_fp_set_shadow_rsx_order); the shadow descriptors are part of the
 * program key, so a change recompiles the comparing programs. */
static int s_shadow_epoch;   /* (rsx3) PS3RECOMP_ENG_TEXGEN: the shadow switches changed */
static void eng_shadow_cmp_set(int id, int v) { (void)id; rsx_fp_set_shadow_rsx_order(v != 0); __atomic_fetch_add(&s_shadow_epoch, 1, __ATOMIC_RELAXED); }
/* PS3RECOMP_ENG_SHADOW_D24 (live, diagnostic): the depth compare's precision
 * and filter order (rsx_fp_set_shadow_d24); part of the shadow descriptor,
 * so a change recompiles the comparing programs too. */
static void eng_shadow_d24_set(int id, int v) { (void)id; rsx_fp_set_shadow_d24(v); __atomic_fetch_add(&s_shadow_epoch, 1, __ATOMIC_RELAXED); }
/* PS3RECOMP_ENG_POLY_OFFSET (live): the polygon offset
 * (SET_POLY_OFFSET_FILL_ENABLE 0x0A68, SET_POLYGON_OFFSET_SCALE_FACTOR /
 * _BIAS 0x0A78 / 0x0A7C), which this engine used to ignore. inFAMOUS 2 draws
 * its sun-shadow casters (the 1024x1024 cascades at zeta 0x0F2F0000) with it
 * on, slope 1 and 2000 depth units. Without it a caster that is also a
 * receiver meets its own depth in the map, and since v0.2.12 compares in the
 * RSX's order (strict "stored > reference" = lit) it fails on every other
 * texel: acne, the sawtooth dark bands and dot lattices on walls, trims and
 * foliage under grazing light. 0 = no offset, as before; N > 1 multiplies the
 * constant term (diagnostic). PS3RECOMP_ENG_POLY_OFFSET_UNITS=<k> (read once)
 * scales it too; PS3RECOMP_ENG_POLY_OFFSET_LOG=1 prints, once each, the depth
 * targets written with which cull and offset, and the programs that compare
 * against a depth map (unit, operator, map size). */
static int s_poly_offset = -1;
static void eng_poly_offset_set(int id, int v) { (void)id; __atomic_store_n(&s_poly_offset, v > 0 ? v : 0, __ATOMIC_RELAXED); }
/* PS3RECOMP_ENG_SHADE_BACKFACE (live, after PS3T_ENG_SHADOW_D24; default 1):
 * inFAMOUS 2 draws its world sun-shadow casters "second-depth" (cull FRONT,
 * no polygon offset: the map holds the casters' back faces; game code
 * 0x002F05D0-0x002F0720, mode counter at guest 0x00A8EF50), and its sun mask
 * (FP 55295a0f6cc1985b) biases the reference only for stencil bit 7
 * (characters and some props). A world surface turned away from the sun
 * therefore compares against its own depth: the 2x2 PCF gives a 0..1 ramp
 * per shadow texel, and the deferred composite (FP f384f111ad5956ba) adds
 * mask x sun x N.L unclamped, so the ramp prints as a lattice of dark and
 * ambient-blue dots and stair-steps on every shaded wall at dusk. 1: the
 * composite treats a stencil-bit-7-clear receiver whose N.L < 0 as shadowed
 * (sun mask 0, i.e. ambient only), the look a second-depth map is meant to
 * give; bit-7 receivers keep the game's designed negative N.L. 0 = v0.2.14.
 * PS3RECOMP_ENG_BACKFACE_CASTER_BIAS=<N> (read once, diagnostic): those
 * cull-FRONT depth-only casters get a depth bias of -N x the slope and
 * -1000 x N units (pulled towards the light; < 0 pushes them away). */
static int s_shade_backface = -1;
static void eng_shade_backface_set(int id, int v) { (void)id; __atomic_store_n(&s_shade_backface, v != 0, __ATOMIC_RELAXED); }
#define ENG_SHADE_BACKFACE_FP 0xf384f111ad5956baull
/* PS3RECOMP_DOF=0 / PS3RECOMP_EDGE_BLUR=0 (live, the PC menu's Graphics
 * page): inFAMOUS 2's depth of field and its screen-edge blur turned off.
 * Unset or 1 = the game's own picture.
 *
 * Both are ONE full-screen pass of the game, FX_DOF_BLEND (the post object
 * built by func_002FD1E4 keeps it at +0xA0), FP 59313a119f622eb5 with
 * VP dd2553b92143f18f, drawn into the scene target after the deferred
 * composite (RenderDoc, rd-scout1 frame 47024, event 12578):
 *   unit 0  the depth buffer as RGBA8 -> d; w = c1.x + c1.y * d
 *   unit 1  the sharp scene (a copy)                              = S
 *   unit 2  the scene at 1/4 x 1/4 (320x180), alpha a weight      = Q
 *   unit 3  Q blurred again (320x180), alpha a weight             = B
 *   n = saturate(c2.y / w + c3.x), f = saturate(c2.w / w + c3.z)
 *   a = n * Q.a * c4.x                       (depth of field, first band)
 *   b = max(f * B.a * c5.x, tc0.z)           (second band, or the edge blur)
 *   out = lerp(lerp(S, Q, a), B, b)
 * tc0.z is vertex attribute 8 of the pass's 12-index screen mesh (centre +
 * 4 corners): 0 in the middle, an edge weight at the corners (the TOD
 * EDGE_BLUR_LIMITS). c4.x/c5.x are the depth-of-field strength.
 * Measured (PS3RECOMP_FX_LOG, tw-fxlog1: boot to the docks tutorial and its
 * camera turns): edge weight 0 throughout; DOF strength 0.3-1.0 in the
 * cutscenes, 0.55 in play -- the "motion blur" on distant things when the
 * camera turns is this depth of field (the game has no motion blur pass).
 * Off = those terms replaced by 0 in that program's generated shader: a = 0
 * and b = 0 exactly, so the pass returns S, the sharp scene, bit for bit
 * (every input is a finite UNORM sample). Nothing else in the frame
 * changes (the game still makes the two quarter-size images; they are
 * small). A change builds the other variant of this one pipeline (part of
 * its key). */
#define ENG_DOF_BLEND_FP 0x59313a119f622eb5ull
static int s_fx_dof = -1, s_fx_edge = -1;   /* 1 on (the game's), 0 off, -1 not read yet */
static int eng_fx_on(const char* v) { return !(v && v[0] == '0' && v[1] == 0); }
static int eng_fx_dof_apply(const char* v) { __atomic_store_n(&s_fx_dof, eng_fx_on(v), __ATOMIC_RELAXED); return 1; }
static int eng_fx_edge_apply(const char* v) { __atomic_store_n(&s_fx_edge, eng_fx_on(v), __ATOMIC_RELAXED); return 1; }
/* Replace every `from` in `buf` with `to`; the count, or -1 when it would not fit. */
static int eng_hlsl_replace(char* buf, size_t cap, const char* from, const char* to)
{
    const size_t fl = strlen(from), tl = strlen(to);
    int hits = 0;
    for (char* q = strstr(buf, from); q; q = strstr(q + tl, from)) {
        const size_t len = strlen(buf);
        if (len - fl + tl + 1 > cap) return -1;
        memmove(q + tl, q + fl, len + 1 - (size_t)(q - buf) - fl);
        memcpy(q, to, tl);
        hits++;
    }
    return hits;
}
/* PS3RECOMP_ENG_DEPTH_CLAMP (live): NV4097_SET_ZMIN_MAX_CONTROL (0x1D78,
 * cellGcmSetZMinMaxControl: bits 0-3 cull at the near/far planes, 4-7 depth
 * clamp, 8-11 cull ignores W), which this engine used to ignore (always
 * clip). inFAMOUS 2 draws its sun-shadow casters with (0, 1, 0): no near/far
 * clipping, depth clamped ("pancaking": a caster between the sun and the
 * cascade's near plane lands on depth 0 instead of being cut away). The near
 * cascade's light-space depth range is tight, so with clipping the tall
 * casters (building tops, roofs) fell in front of its near plane and were
 * cut: a building's shadow on the street showed from afar (far cascades,
 * wide range) and vanished up close. As RPCS3: clamp when the clamp bit is
 * set or near/far clipping is off. 0 = always clip, as before. */
static int s_depth_clamp = -1;
static int s_clamp_cull_flip = -1;
static void eng_clamp_cull_flip_set(int id, int v) { (void)id; __atomic_store_n(&s_clamp_cull_flip, v > 0, __ATOMIC_RELAXED); }
static void eng_depth_clamp_set(int id, int v) { (void)id; __atomic_store_n(&s_depth_clamp, v != 0, __ATOMIC_RELAXED); }
/* The current draw's depth-bounds test as the fragment program emulates it
 * (eng_depth_bounds_prepare): unit + 1 of the depth copy it reads, 0 = none. */
static struct { u32 unit; float mn, mx; } s_dbz;

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;

/* ------------------------------------------------------------------------- */

#define ENG_MAX_SURFACES   64
#define ENG_MAX_ZDEPTHS    64
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  8192
#define ENG_MAX_BATCHES    256
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 2) * 16u)
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_POLY_OFFSET_FILL    0x0A68
#define M_POLY_OFFSET_SCALE   0x0A78
#define M_POLY_OFFSET_BIAS    0x0A7C
#define M_ZSTENCIL_CLEAR      0x1D8C

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    rsx_be_format fmt;
    u32 handle;
    u32 stamp;           /* last time it was bound as a target (LRU, newest-wins) */
    u32 wb_stamp;        /* stamp at the last write-back into guest memory */
} eng_surface;
static u32 s_surf_clock;

/* Framebuffer fetch for read-modify-write passes (rsx_fp_apply_fbfetch).
 * inFAMOUS 2's G-buffer decals blend by hand: each one samples the albedo,
 * normals and material targets it is drawing into, at its own pixel, and
 * the engine copied all three whole targets before EVERY such draw (161
 * full-screen copies a frame on the New Marais walk, ~half the GPU frame at
 * PS3_RES_SCALE=1.5). With PS3_RES_SCALE they also read the wrong pixel:
 * the game's screen texcoord is built for its own 1280x720 grid and does not
 * land on the scaled fragment's own texel, so a decal carried a neighbour's
 * albedo/normal across every edge -- the specks and short vertical streaks
 * on faces (eyelids, nostrils, lips, the face's outline). Reading the
 * attachment itself (Metal [[color(k)]]) needs no copy and is exactly the
 * fragment's own pixel. PS3RECOMP_ENG_FBFETCH (live):
 * unset = on when the targets are scaled (PS3_RES_SCALE or PS3_ASPECT not 1),
 * 1 = always, 0 = never (the copies). */
static volatile int s_fbf_mode = -1;
static void eng_fbf_set(int id, int v) { (void)id; __atomic_store_n(&s_fbf_mode, v, __ATOMIC_RELAXED); }
static struct {
    u32 cand;                    /* units that sample one of the draw's own targets */
    u8  att[16];                 /* ... which colour attachment */
    u8  sel[16][4];              /* ... through which channel crossbar */
    u32 slot[16], remap[16], fmt[16];
    u32 applied;                 /* the pipeline read them from the attachment */
} s_fbf;

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    int cleared;
    int had_write;
    int snapshot_valid;
    u32 snapshot_rgba;     /* colour-format (depth bytes) copy, or 0      */
    int snapshot_rgba_valid;
    u32 crop_rgba;         /* top-left crop of the colour-format copy, or 0 */
    u32 crop_w, crop_h;    /* its size (a texture smaller than the buffer)  */
    int crop_valid;
    u32 crop_z;            /* top-left crop of the sampleable copy, or 0    */
    u32 crop_z_w, crop_z_h;
    int crop_z_valid;
    u32 stamp;             /* last depth write (s_surf_clock)             */
    u32 wb_stamp;          /* stamp at the last write-back into guest memory */
    u64 writer_fp;         /* structural hash of the last depth-writing FP (RESOLVE_LOG) */
} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u64 content_hash;
    u64 sparse_hash;            /* eng_texture_sparse_hash, the per-frame check */
    u32 last_hash_frame;
    u32 last_full_frame;        /* the last full content hash (eng_texture_get) */
    u64 last_use_serial;
    u32 levels, span;           /* eng_texture_span for these levels */
    u32 depth;                  /* slices of a 3D texture, else 1 */
    u8 icon_atlas;              /* the button-prompt atlas (rsx_button_icons.c) */
    u32 icon_gen;               /* the glyph set it was drawn with */
    u8 wt_ok;                   /* rsx_wtrack: the span was armed and the upload matches it */
    u64 wt_stamp;               /* ... the page versions it was armed with */
} eng_texture;

/* rsx_button_icons: the glyph set of this frame, and whether the upload
 * that just ran was the prompt atlas. */
static u32 s_icon_gen;
static int s_upload_atlas;
/* Non-zero: eng_texture_upload refills this 2D texture in place instead of
 * creating one (the atlas redraw when the backend has no object to spare). */
static u32 s_upload_into;

typedef struct { u64 key; u32 handle; u8 fixed; u32 fbf; } eng_pipeline;   /* fbf: s_fbf.applied */

typedef struct { u32 first, count; } eng_batch;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 n_pipelines;
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u32 last_flip_buffer;

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    /* The vp_c[] rows the current vertex program can read (eng_vertex_layout):
     * vpc_full when it reads any row (relative addressing, no end flag, or no
     * guest program), else [vpc_lo, vpc_lo + vpc_n). */
    int vpc_full;
    u32 vpc_lo, vpc_n;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    /* The draw's fragment-program constants: the program memo's block (see
     * eng_fp_current), valid until the next draw's lookup. */
    const rsx_fp_constant_block* fp_constants;
} g;

static void eng_apply_depth_bias(u32 zeta_offset, const rsx_be_render_state* rs)
{
    if (!g.be->set_depth_bias) return;
    if (__builtin_expect(__atomic_load_n(&s_poly_offset, __ATOMIC_RELAXED) < 0, 0)) {
        const char* e = getenv("PS3RECOMP_ENG_POLY_OFFSET");
        const int v = e ? atoi(e) : 1;
        __atomic_store_n(&s_poly_offset, v > 0 ? v : 0, __ATOMIC_RELAXED);
        ps3_toggle_watch(PS3T_ENG_POLY_OFFSET, eng_poly_offset_set);
    }
    static float units_k = -1.0f; static int log_on = -1;
    if (units_k < 0.0f) { const char* e = getenv("PS3RECOMP_ENG_POLY_OFFSET_UNITS");
        units_k = e ? (float)atof(e) : 1.0f; if (units_k < 0.0f) units_k = 0.0f;
        e = getenv("PS3RECOMP_ENG_POLY_OFFSET_LOG"); log_on = e && *e == '1'; }
    const u32 en = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL) & 1u;
    const u32 sr = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_SCALE), br = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
    float slope = 0.0f, constant = 0.0f;
    const int mode = __atomic_load_n(&s_poly_offset, __ATOMIC_RELAXED);
    if (en && mode) {
        memcpy(&slope, &sr, 4); memcpy(&constant, &br, 4);
        if (slope != slope) slope = 0.0f;
        if (constant != constant) constant = 0.0f;
        constant *= units_k * (float)mode;   /* mode > 1: diagnostic multiple */
    }
    /* PS3RECOMP_ENG_BACKFACE_CASTER_BIAS (diagnostic, see SHADE_BACKFACE). */
    static int sb = -1000;
    if (sb == -1000) { const char* e = getenv("PS3RECOMP_ENG_BACKFACE_CASTER_BIAS"); sb = e ? atoi(e) : 0; }
    if (sb && !(en && mode) && rs->depth_write && rs->depth_test && rs->color_mask == 0 &&
        rs->cull_enable && rs->cull_face == 0x0404u) {
        slope = -(float)sb;
        constant = -1000.0f * (float)sb;
    }
    if (log_on && rs->depth_write) {
        /* Which depth targets the offset draws write, how they cull. */
        static u32 seen[64][4]; static int nseen;
        const u32 cull = rs->cull_enable ? rs->cull_face : 0;
        int k = 0; for (; k < nseen; k++)
            if (seen[k][0] == zeta_offset && seen[k][1] == (en ? sr : 0xFFFFFFFFu) && seen[k][2] == (en ? br : 0) && seen[k][3] == cull) break;
        if (k == nseen && nseen < 64) { seen[nseen][0] = zeta_offset; seen[nseen][1] = en ? sr : 0xFFFFFFFFu;
            seen[nseen][2] = en ? br : 0; seen[nseen][3] = cull; nseen++;
            float fs, fb; memcpy(&fs, &sr, 4); memcpy(&fb, &br, 4);
            fprintf(stderr, "[rsx engine] zwrite zeta 0x%08X cull 0x%X poly offset %s scale %g bias %g\n",
                    zeta_offset, cull, en ? "ON" : "off", (double)fs, (double)fb); }
    }
    g.be->set_depth_bias(g.be->user, constant, slope);
}

static void eng_apply_depth_clip(u32 zeta_offset, const rsx_be_render_state* rs, float vp_sy)
{
    if (!g.be->set_depth_clamp) return;
    if (__builtin_expect(__atomic_load_n(&s_depth_clamp, __ATOMIC_RELAXED) < 0, 0)) {
        const char* e = getenv("PS3RECOMP_ENG_DEPTH_CLAMP");
        __atomic_store_n(&s_depth_clamp, !(e && *e == '0'), __ATOMIC_RELAXED);
        ps3_toggle_watch(PS3T_ENG_DEPTH_CLAMP, eng_depth_clamp_set);
        e = getenv("PS3RECOMP_ENG_CLAMP_CULL_FLIP");
        __atomic_store_n(&s_clamp_cull_flip, e && atoi(e) > 0, __ATOMIC_RELAXED);
        ps3_toggle_watch(PS3T_ENG_CLAMP_CULL_FLIP, eng_clamp_cull_flip_set);
    }
    const u32 v = rsx_dsp_reg(&g.rsx, 0x1D78);
    const int clamp = __atomic_load_n(&s_depth_clamp, __ATOMIC_RELAXED)
                   && ((v & 0xF0u) != 0 || (v & 0xFu) == 0);
    static int log_on = -1;
    if (log_on < 0) { const char* e = getenv("PS3RECOMP_ENG_POLY_OFFSET_LOG"); log_on = e && *e == '1'; }
    if (log_on) {
        static u32 seen[64][3]; static int nseen;
        const u32 cf = (rs->cull_enable ? rs->cull_face : 0) | (rs->front_face << 16) | (vp_sy < 0.0f ? 0x80000000u : 0);
        int k = 0; for (; k < nseen; k++) if (seen[k][0] == zeta_offset && seen[k][1] == v && seen[k][2] == cf) break;
        if (k == nseen && nseen < 64) { seen[nseen][0] = zeta_offset; seen[nseen][1] = v; seen[nseen][2] = cf; nseen++;
            fprintf(stderr, "[rsx engine] zeta 0x%08X ZMIN_MAX_CONTROL 0x%X -> depth %s; cull 0x%X front 0x%X viewport y %s\n",
                    zeta_offset, v, clamp ? "clamp" : "clip", rs->cull_enable ? rs->cull_face : 0, rs->front_face,
                    vp_sy < 0.0f ? "-" : "+"); }
    }
    g.be->set_depth_clamp(g.be->user, clamp);
}

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    eng_batch arr[ENG_MAX_BATCHES];
    u32 n_arr;
    eng_batch idx[ENG_MAX_BATCHES];
    u32 n_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    int refs_remapped;
    /* An index-cache hit leaves refs[1..n_refs) unwritten (they are span
     * consecutive ids from refs[0], base lazy_base): dc_refs_fill() writes them
     * for the one later reader (a non-indexed refetch). 0 = all written. */
    u32 refs_lazy, lazy_base;
    rsx_vertex_remap ref_remap;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;
    u32 vbuf;          /* a cached persistent vertex buffer this draw uses, else 0 */
    int vbuf_hit;      /* dc.verts was NOT filled: the cache supplied the vertices */

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
} dc;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
}

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) return s_guest_reader(s_guest_user, location, offset, min_bytes);
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    return vm_base + ea;
}

static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

/* ---- render state -------------------------------------------------------- */

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    rs->alpha_ref_format = sf.color_format;
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* PS3RECOMP_ENG_CLAMP_CULL_FLIP=1 (live, DIAGNOSTIC): the opposite
     * winding for draws with the depth clamp set (inFAMOUS 2: the sun-shadow
     * casters), to tell which faces of an object land in the shadow map. */
    if (__atomic_load_n(&s_clamp_cull_flip, __ATOMIC_RELAXED) > 0 && (rsx_dsp_reg(rsx, 0x1D78) & 0xF0u))
        rs->front_face = rs->front_face == 0x0901u ? 0x0900u : 0x0901u;
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->color_mask_mrt = rsx_dsp_reg(rsx, 0x0370) & 0xFFF0u;
    rs->blend_mrt      = rsx_dsp_reg(rsx, 0x036C) & 0xEu;
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
}

u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
#define ENG_HASH_FIELD(name) hash = eng_fnv1a(&rs->name, sizeof(rs->name), hash)
    /* The alpha REFERENCE is deliberately absent: it lives in the fragment
     * constant block, so a title animating a fade must not compile a new
     * pipeline per frame. Enable and compare mode still select a variant.
     * Every field is listed by name so struct padding can never be identity
     * (rsx_live_draw.c:3151-3185). */
    ENG_HASH_FIELD(alpha_test_enable);
    ENG_HASH_FIELD(alpha_func);
    ENG_HASH_FIELD(blend_enable);
    ENG_HASH_FIELD(sf_rgb);
    ENG_HASH_FIELD(df_rgb);
    ENG_HASH_FIELD(sf_a);
    ENG_HASH_FIELD(df_a);
    ENG_HASH_FIELD(eq_rgb);
    ENG_HASH_FIELD(eq_a);
    ENG_HASH_FIELD(depth_test);
    ENG_HASH_FIELD(depth_write);
    ENG_HASH_FIELD(depth_func);
    ENG_HASH_FIELD(cull_enable);
    ENG_HASH_FIELD(cull_face);
    ENG_HASH_FIELD(front_face);
    ENG_HASH_FIELD(color_mask);
    ENG_HASH_FIELD(color_mask_mrt);
    ENG_HASH_FIELD(blend_mrt);
    ENG_HASH_FIELD(rt_fp16);
    ENG_HASH_FIELD(stencil_enable);
    ENG_HASH_FIELD(stencil_two_sided);
    ENG_HASH_FIELD(s_func);
    ENG_HASH_FIELD(s_func_mask);
    ENG_HASH_FIELD(s_write_mask);
    ENG_HASH_FIELD(s_fail);
    ENG_HASH_FIELD(s_zfail);
    ENG_HASH_FIELD(s_zpass);
    ENG_HASH_FIELD(bs_func);
    ENG_HASH_FIELD(bs_fail);
    ENG_HASH_FIELD(bs_zfail);
    ENG_HASH_FIELD(bs_zpass);
#undef ENG_HASH_FIELD
    return hash;
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUAD_STRIP: {
        /* Two vertices per quad after the first pair, so a segment of n
         * carries (n - 2) / 2 quads and an odd trailing vertex is dropped. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 4) total += ((count - 2u) / 2u) * 6u;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS:
        return (source_refs / 4u) * 6u;
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 4) continue;
            /* Every quad is split along the diagonal between the two pairs,
             * so each one keeps the strip's winding without alternating the
             * way a triangle strip has to; a cut simply starts the pairing
             * again from the segment's own first vertex. This is the
             * expansion the vtable path's emit_vertices performs. */
            for (u32 q = 0; q + 3 < count; q += 2) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 q = 0; q < source_refs / 4u; q++) {
            const u32 base = q * 4u;
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed triangle list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips and polygons are
 * this engine's, so their two cases are decided here rather than in the
 * shared helper, which the vendored rsx_live_draw.c also calls. */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_ptr(NULL, location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

/* A 1024-bit filter over the colour targets' (location, offset), rebuilt
 * when a slot is (re)assigned (eng_surface_get is the only writer): 0 means no
 * target is at that address, so a texture unit's search over every target can
 * be skipped. A stale filter only errs towards 1. PS3RECOMP_ENG_SURF_FILTER=1 to
 * use it (default off: no measured gain). rsx3 2026-11-08: default ON (=0 off) -- with
 * LAZY_REFS, FP_MEMO=64, PLAN_LAYOUT and the E-core worker, city heavy view +4.7%. */
static u32 s_surf_gen, s_surf_filter_gen = ~0u;
static u64 s_surf_filter[16];
static u32 eng_surf_hash(u32 location, u32 offset) { return ((offset ^ (location << 30)) * 0x9E3779B1u) >> 22; }
static int eng_surface_maybe(u32 location, u32 offset)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_SURF_FILTER"); on = !(e && e[0] == '0'); }   /* rsx3: default on */
    if (!on) return 1;
    if (s_surf_filter_gen != s_surf_gen) {
        memset(s_surf_filter, 0, sizeof s_surf_filter);
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const u32 h = eng_surf_hash(g.surfaces[i].location, g.surfaces[i].offset);
            s_surf_filter[h >> 6] |= 1ull << (h & 63u);
        }
        s_surf_filter_gen = s_surf_gen;
    }
    const u32 h = eng_surf_hash(location, offset);
    return (int)((s_surf_filter[h >> 6] >> (h & 63u)) & 1u);
}

/* The colour target for this (location, offset), created or reallocated when
 * the size or format moved. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690). */
static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;

    /* One target per (address, size, format). Reallocating the address's
     * target whenever a pass declared a different clip (a 1280x180 strip, a
     * 256x256 tile over a 1280x720 G-buffer) threw away contents a later pass
     * samples; inFamous lit an empty G-buffer and presented black. */
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].handle && g.surfaces[i].location == location &&
            g.surfaces[i].offset == offset && g.surfaces[i].w == want_w &&
            g.surfaces[i].h == want_h && g.surfaces[i].fmt == want_fmt) {
            g.surfaces[i].stamp = ++s_surf_clock;
            return i;
        }

    /* Never create a target because one malformed command decoded a guest
     * pointer as clip dimensions: fall back to the newest at this address. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        u32 best = ENG_INVALID;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == location &&
                g.surfaces[i].offset == offset &&
                (best == ENG_INVALID || g.surfaces[i].stamp > g.surfaces[best].stamp)) best = i;
        static u32 logs = 0;
        if (__atomic_fetch_add(&logs, 1, __ATOMIC_RELAXED) < 8)
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u;"
                            " keeping the %s target\n", offset, want_w, want_h,
                    best != ENG_INVALID ? "existing" : "absent");
        return best;
    }
    /* A free slot, else the least recently bound target. */
    u32 slot = g.n_surfaces;
    for (u32 i = 0; i < g.n_surfaces; i++) if (!g.surfaces[i].handle) { slot = i; break; }
    if (slot >= ENG_MAX_SURFACES) {
        slot = 0;
        for (u32 i = 1; i < g.n_surfaces; i++)
            if (g.surfaces[i].stamp < g.surfaces[slot].stamp) slot = i;
    }

    u32 seed_row = 0;
    const void* seed = eng_surface_seed(location, offset, want_w, want_h,
                                        want_fmt, &seed_row);
    const u32 handle = g.be->color_target_create(g.be->user, want_fmt,
                                                 want_w, want_h, seed, seed_row);
    if (!handle) return ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    s_surf_gen++;                            /* eng_surface_maybe's filter */
    s->location = location; s->offset = offset;
    s->w = want_w; s->h = want_h; s->fmt = want_fmt;
    s->handle = handle;
    s->stamp = ++s_surf_clock;
    if (slot == g.n_surfaces) g.n_surfaces++;
    { static u32 logs = 0, cap = 0;
      if (!cap) cap = getenv("PS3RECOMP_ENG_TEXLOG") ? 256u : 16u;   /* TEXLOG: the whole set */
      if (__atomic_fetch_add(&logs, 1, __ATOMIC_RELAXED) < cap)
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d%s -> h%u\n",
                location, offset, want_w, want_h, (int)want_fmt,
                seed ? " (seeded from guest memory)" : "", handle); }
    return slot;
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0. */
static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 first = eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                                      sf.clip_w, sf.clip_h, fmt);
    if (first == ENG_INVALID) return 0;
    slots[0] = first;
    u32 n = 1;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        const u32 slot = eng_surface_get(sf.color_location[i + 1],
                                         sf.color_offset[i + 1],
                                         sf.clip_w, sf.clip_h, fmt);
        if (slot == ENG_INVALID) break;
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
    }
    return n;
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;

    u32 slot = ENG_MAX_ZDEPTHS;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) return i;
            slot = i;
            if (want_w < z->w) want_w = z->w;
            if (want_h < z->h) want_h = z->h;
            break;
        }
    }
    if (slot == ENG_MAX_ZDEPTHS) {
        if (g.n_zdepths >= ENG_MAX_ZDEPTHS) return ENG_INVALID;
        slot = g.n_zdepths;
    }

    const u32 handle = g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;

    eng_zdepth* z = &g.zdepths[slot];
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    if (z->snapshot && g.be->texture_release) g.be->texture_release(g.be->user, z->snapshot);
    if (z->snapshot_rgba && g.be->texture_release) g.be->texture_release(g.be->user, z->snapshot_rgba);
    z->snapshot_rgba = 0;
    if (z->crop_rgba && g.be->texture_release) g.be->texture_release(g.be->user, z->crop_rgba);
    z->crop_rgba = 0; z->crop_w = z->crop_h = 0;
    if (z->crop_z && g.be->texture_release) g.be->texture_release(g.be->user, z->crop_z);
    z->crop_z = 0; z->crop_z_w = z->crop_z_h = 0; z->crop_z_valid = 0;
    z->location = location; z->offset = offset;
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->snapshot = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->snapshot_rgba_valid = 0; z->crop_valid = 0;
    z->stamp = ++s_surf_clock;
    z->wb_stamp = 0;
    if (slot == g.n_zdepths) g.n_zdepths++;
    return slot;
}

static u64 eng_current_fp_structural(void);   /* defined with the program memo */

/* PS3RECOMP_ENG_RESOLVE_LOG=1: which programs make the engine resolve depth
 * into a sampleable copy. Every resolve ends the render pass it lands in (the
 * attachments are stored and reloaded around a full-screen decode), so a
 * stream that alternates depth writers and depth readers pays one per reader.
 * Counts (reader FP, last writer FP) pairs and prints the top ones every 4096
 * resolves. */
static void eng_resolve_note(const eng_zdepth* z, int rgba)
{
    static int on = -1;
    if (on < 0) on = getenv("PS3RECOMP_ENG_RESOLVE_LOG") ? 1 : 0;
    if (!on) return;
    enum { N = 64 };
    static struct { u64 reader, writer; u32 n; int rgba; } t[N];
    static u32 used, total;
    const u64 reader = eng_current_fp_structural();
    u32 k = 0;
    while (k < used && !(t[k].reader == reader && t[k].writer == z->writer_fp && t[k].rgba == rgba)) k++;
    if (k == used && used < N) { t[k].reader = reader; t[k].writer = z->writer_fp; t[k].rgba = rgba; t[k].n = 0; used++; }
    if (k < used) t[k].n++;
    if (++total % 4096) return;
    fprintf(stderr, "[resolve] %u depth resolves so far; top (reader fp <- last writer fp):\n", total);
    for (int r = 0; r < 8; r++) {
        int best = -1;
        for (u32 j = 0; j < used; j++) if (t[j].n && (best < 0 || t[j].n > t[best].n)) best = (int)j;
        if (best < 0) break;
        fprintf(stderr, "[resolve]   %6u  %s reader %016llX <- writer %016llX\n", t[best].n,
                t[best].rgba ? "rgba8" : "depth", (unsigned long long)t[best].reader,
                (unsigned long long)t[best].writer);
        t[best].n = 0;
    }
    for (u32 j = 0; j < used; j++) t[j].n = 0;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
static u32 eng_zdepth_snapshot(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    if (z->snapshot_valid && z->snapshot) return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    eng_resolve_note(z, 0);
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, z->w, z->h);
    if (!tex) return 0;
    /* The stale copy goes: every depth write invalidates the snapshot, and the
     * sun-shadow cascades read their map after each write, so without this
     * every frame leaked a few copies until the backend's object table filled
     * and new textures sampled black. The backend keeps a released object
     * alive until no recorded draw can still name it. */
    if (z->snapshot && z->snapshot != tex && g.be->texture_release)
        g.be->texture_release(g.be->user, z->snapshot);
    z->snapshot = tex;
    z->snapshot_valid = 1;
    return tex;
}

static u32 eng_zdepth_snapshot_rgba8(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    if (z->snapshot_rgba_valid && z->snapshot_rgba) return z->snapshot_rgba;
    if (!g.be->depth_snapshot_rgba8) return 0;
    eng_resolve_note(z, 1);
    const u32 tex = g.be->depth_snapshot_rgba8(g.be->user, z->handle, z->w, z->h);
    if (!tex) return 0;
    if (z->snapshot_rgba && z->snapshot_rgba != tex && g.be->texture_release)
        g.be->texture_release(g.be->user, z->snapshot_rgba);   /* as above */
    z->snapshot_rgba = tex;
    z->snapshot_rgba_valid = 1;
    return tex;
}

/* The colour-format copy for a texture of w x h at the depth buffer's
 * address. A smaller texture over a bigger buffer is its top-left corner at
 * the buffer's pitch, not the whole buffer scaled down: inFAMOUS 2 keeps a
 * 1280x720 depth at 0x0F770000 and uses 512x288 of it for the menu's half-
 * resolution particle depth and 256x256 for the water reflection; read whole,
 * the particle test saw a 2.5x-shrunk depth (a Cole-shaped hole in the
 * debris). PS3RECOMP_ENG_DEPTH_CROP=0: always the whole buffer (old). */
static u32 eng_zdepth_snapshot_rgba8_sized(u32 slot, u32 w, u32 h)
{
    static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_DEPTH_CROP"); on = e ? atoi(e) : 1; }
    eng_zdepth* z = &g.zdepths[slot];
    if (!on || !w || !h || (w >= z->w && h >= z->h)) return eng_zdepth_snapshot_rgba8(slot);
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!z->handle || !z->had_write || !g.be->depth_snapshot_rgba8) return 0;
    if (z->crop_valid && z->crop_rgba && z->crop_w == w && z->crop_h == h) return z->crop_rgba;
    eng_resolve_note(z, 1);
    const u32 tex = g.be->depth_snapshot_rgba8(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    if (z->crop_rgba && z->crop_rgba != tex && g.be->texture_release)
        g.be->texture_release(g.be->user, z->crop_rgba);
    z->crop_rgba = tex; z->crop_w = w; z->crop_h = h; z->crop_valid = 1;
    return tex;
}

/* The sampleable (R32F) copy for a DEPTH-format texture of w x h over the
 * depth buffer: its top-left corner when the texture is smaller, as the
 * colour-format copy above. A depth buffer grows to the largest size any
 * surface at its address asked for, and inFAMOUS 2's sun-shadow zeta at
 * 0x0F2F0000 (1024x1024 cascades) lives in a 1280-wide buffer; sampled whole,
 * every shadow lookup landed 1.25x too far along x: the shadows of the
 * casters near the camera (Cole's own) fell outside the frame or onto the
 * wrong spot, each cascade mis-scaled differently (shadows doubled where two
 * cascades meet), and shadows of things never near the receiver showed up on
 * it. PS3RECOMP_ENG_ZTEX_CROP=0 (live) samples the whole buffer, as before. */
static int s_ztex_crop = -1;   /* PS3RECOMP_ENG_ZTEX_CROP (live); -1 not read yet */
static void eng_ztex_crop_set(int id, int v) { (void)id; __atomic_store_n(&s_ztex_crop, v != 0, __ATOMIC_RELAXED); }
static u32 eng_zdepth_snapshot_sized(u32 slot, u32 w, u32 h)
{
    if (__builtin_expect(__atomic_load_n(&s_ztex_crop, __ATOMIC_RELAXED) < 0, 0))
        ps3_toggle_watch(PS3T_ENG_ZTEX_CROP, eng_ztex_crop_set);
    eng_zdepth* z = &g.zdepths[slot];
    if (!__atomic_load_n(&s_ztex_crop, __ATOMIC_RELAXED) || !w || !h || (w >= z->w && h >= z->h))
        return eng_zdepth_snapshot(slot);
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!z->handle || !z->had_write || !g.be->depth_snapshot) return 0;
    if (z->crop_z_valid && z->crop_z && z->crop_z_w == w && z->crop_z_h == h) return z->crop_z;
    eng_resolve_note(z, 0);
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    if (z->crop_z && z->crop_z != tex && g.be->texture_release)
        g.be->texture_release(g.be->user, z->crop_z);   /* as eng_zdepth_snapshot */
    z->crop_z = tex; z->crop_z_w = w; z->crop_z_h = h; z->crop_z_valid = 1;
    return tex;
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* A 3D texture: each level is `depth` slices (halving with the level), one
 * face_bytes each -- consecutive for a linear image, one Morton block for a
 * swizzled one. */
static u32 eng_texture_span_3d(u32 fmt, u32 w, u32 h, u32 depth, u32 levels, u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    u32 total = 0, d = depth ? depth : 1u;
    for (u32 m = 0; m < n; m++) { total += lv[m].tl.face_bytes * d; if (d > 1) d >>= 1; }
    return total;
}

/* Slice z of a swizzled 3D level, de-interleaved into row-major rows of
 * w * bpp bytes, so the 2D decode can convert it as a linear image. */
static void eng_deswizzle_3d_slice(u8* dst, const u8* src, u32 w, u32 h, u32 d, u32 z, u32 bpp)
{
    u32 lw = 0, lh = 0, ld = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    while ((1u << ld) < d) ld++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            memcpy(dst + ((size_t)y * w + x) * bpp,
                   src + (size_t)rsx_swizzle_offset_3d(x, y, z, lw, lh, ld) * bpp, bpp);
}

/* One hash per cached texture per presented frame. Word-at-a-time FNV over
 * the whole span is deliberately cheap: this is a mutation detector, not a
 * content id (rsx_live_draw.c:2002-2026). */
static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span,
                                    int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    /* Eight independent FNV-style lanes: one serial multiply chain capped the
     * hash at ~2 bytes/cycle, and every texture in use is hashed each frame.
     * Four lanes ran 21 GB/s on an M1 Pro, eight 36 GB/s -- about what one
     * core can read (T-0016). The value is only ever compared with another
     * hash from this run. */
    u64 h[8];
    for (int k = 0; k < 8; k++)
        h[k] = 1469598103934665603ull ^ (0x9E3779B97F4A7C15ull * (u64)(k + 1));
    u32 i = 0;
    for (; i + 64 <= span; i += 64) {
        u64 w[8];
        memcpy(w, src + i, sizeof w);
        for (int k = 0; k < 8; k++) h[k] = (h[k] ^ w[k]) * 1099511628211ull;
    }
    u64 hash = 0;
    for (int k = 0; k < 8; k++) hash ^= h[k] * (u64)(2 * k + 1);
    for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
    *readable = 1;
    return hash;
}

/* A cheap per-frame change check: 64 bytes out of every 2 KB plus the last
 * 64. A rewrite of the whole texture (a video frame, a UI redraw, a streamed
 * replacement) changes it at once; a small partial write is caught by the
 * full hash, which each texture still gets every eighth frame. */
static u64 eng_texture_sparse_hash(u32 location, u32 offset, u32 span, int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 h = 1469598103934665603ull;
    /* Under 64 bytes there is no 64-byte block to sample: all of it, bytewise.
     * (This used to report "unreadable", and a tiny texture was then never
     * hashed again after its first upload.) */
    if (span < 64) {
        for (u32 i = 0; i < span; i++) { h ^= src[i]; h *= 1099511628211ull; }
        *readable = 1;
        return h;
    }
    for (u32 i = 0; i + 64 <= span; i += 2048) {
        u64 w[8]; memcpy(w, src + i, sizeof w);
        for (int k = 0; k < 8; k++) h = (h ^ w[k]) * 1099511628211ull;
    }
    { u64 w[8]; memcpy(w, src + span - 64, sizeof w);
      for (int k = 0; k < 8; k++) h = (h ^ w[k]) * 1099511628211ull; }
    *readable = 1;
    return h;
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* PS3RECOMP_ENG_TEXDUMP=<dir>: the decoded first level of every 2D texture
 * the engine uploads (re-uploads too), as it goes to the backend -- RGBA8,
 * R8, block-compressed blocks, ... -- in <dir>/tNNNNN.bin, one line per file
 * in <dir>/index.txt (guest offset, format byte, size, decoded format, row
 * bytes, and the texture-pack key of a colour texture). For finding a
 * texture by its picture offline (the font atlas with the button glyphs).
 * Stops at 3000 files or 1.5 GB. */
static int eng_texdump_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_TEXDUMP"); on = e && *e; }
    return on;
}
static void eng_texdump(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                        const rsx_tex_layout* tl, u64 key)
{
    static int init; static char dir[400]; static u32 n; static u64 bytes;
    if (!init) { init = 1; const char* e = getenv("PS3RECOMP_ENG_TEXDUMP");
                 if (e && *e) { snprintf(dir, sizeof dir, "%s", e); mkdir(dir, 0755); } }
    if (!dir[0] || n >= 3000u) return;
    const u64 sz = (u64)tl->dst_row_bytes * tl->rows;
    if (bytes + sz > 1536ull << 20) return;
    char path[512]; snprintf(path, sizeof path, "%s/t%05u.bin", dir, n);
    FILE* fo = fopen(path, "wb");
    if (!fo) return;
    fwrite(g.tex_staging, 1, (size_t)sz, fo); fclose(fo);
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE* ix = fopen(path, "a");
    if (ix) { fprintf(ix, "t%05u loc %u off 0x%08X fmt 0x%02X %ux%u dfmt %d row %u rows %u compressed %d frame %u",
                      n, location, offset, fmt, w, h, (int)tl->fmt, tl->dst_row_bytes, tl->rows,
                      tl->compressed, g.frames);
              if (key) fprintf(ix, " key %016llx", (unsigned long long)key);
              fputc('\n', ix); fclose(ix); }
    n++; bytes += sz;
}

/* ---- texture packs (PS3_MODS) ---------------------------------------------
 * A pack is a folder of PNGs, each named by the key of the texture it
 * replaces: 16 hex digits, then optionally anything that is not a hex digit
 * (`3f09a1c2b4d5e6f7.png`, `3f09a1c2b4d5e6f7_logo.png`), anywhere below the
 * folder.
 *
 *   PS3_MODS=<pack>,<pack>,..  the packs in use, first wins when two replace
 *                              the same texture; each a folder name inside
 *                              PS3_MODS_DIR, or an absolute path. Unset or
 *                              empty: no packs (nothing below runs).
 *   PS3_MODS_DIR=<dir>         where packs live (run.sh: <data>/mods).
 *   PS3_MODS_DUMP=<dir>        writes every colour texture the game uploads,
 *                              once each, as <dir>/<key>.png, the name a pack
 *                              uses, and a line in <dir>/textures.txt (key,
 *                              size, format, guest address). Dump, edit,
 *                              drop the file into a pack.
 *
 * The key is the texture's content, not where it sits: the engine's content
 * hash (eng_texture_content_hash, over every level of the texture as the
 * guest stored it) mixed with its SET_TEXTURE_FORMAT base and size, so a
 * texture streamed to another address keeps its key, and a texture the game
 * rewrites gets a new one. Changing that hash renames every pack file.
 *
 * Colour textures only: those decoded to RGBA8 (A8R8G8B8, D8R8G8B8, the
 * packed 16-bit formats) or block-compressed (DXT1/3/5). A replacement is
 * uploaded as RGBA8 at its own size (larger is fine: a higher-resolution
 * pack), with a box-filtered mip chain when the original has mips, and keeps
 * the original's sampler state and channel crossbar: the PNG holds the
 * channels as the texture stores them, so one the game reads through a
 * remap looks remapped in the dump too, and the replacement is read the same
 * way. A texture the game samples with unnormalised coordinates (format bit
 * 0x40) must keep its size. Cube maps and volumes are not replaced. */
typedef struct { u64 key; u32 path; u16 pack; u8 logged, bad; } eng_mod_entry;
static struct {
    int init, on;
    eng_mod_entry* e; u32 n, cap;
    u32* idx; u32 idx_size;              /* open addressing, entry index + 1 */
    char* pool; u32 pool_used, pool_cap; /* file paths */
    char pack[16][160]; u32 npacks;
    char dump_dir[512];
    u64* dumped; u32 dumped_n, dumped_size;
    u32 applied;
} s_mods;

static u64 eng_texmod_key(u64 content, u32 fmt, u32 w, u32 h)
{
    u64 k = content ^ (((((u64)(fmt & 0x9Fu)) << 48) | ((u64)w << 24) | (u64)h) * 0x9E3779B97F4A7C15ull);
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull; k ^= k >> 33;
    return k ? k : 1u;
}

static int eng_hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The key a pack file's name gives, or 0. */
static u64 eng_mod_name_key(const char* name)
{
    u64 k = 0;
    for (int i = 0; i < 16; i++) {
        const int v = eng_hexval((unsigned char)name[i]);
        if (v < 0) return 0;
        k = (k << 4) | (u64)v;
    }
    if (eng_hexval((unsigned char)name[16]) >= 0) return 0;
    const size_t n = strlen(name);
    if (n < 20 || name[n - 4] != '.' || (name[n - 3] | 32) != 'p' || (name[n - 2] | 32) != 'n' ||
        (name[n - 1] | 32) != 'g') return 0;
    return k ? k : 1u;
}

static const eng_mod_entry* eng_mod_find(u64 key)
{
    if (!s_mods.idx_size) return NULL;
    for (u32 p = (u32)(key ^ (key >> 32)) & (s_mods.idx_size - 1u); s_mods.idx[p];
         p = (p + 1) & (s_mods.idx_size - 1u))
        if (s_mods.e[s_mods.idx[p] - 1].key == key) return &s_mods.e[s_mods.idx[p] - 1];
    return NULL;
}

static void eng_mod_index_rebuild(void)
{
    u32 size = 64;
    while (size < s_mods.n * 2u + 2u) size <<= 1;
    u32* idx = (u32*)calloc(size, sizeof *idx);
    if (!idx) return;
    free(s_mods.idx);
    s_mods.idx = idx; s_mods.idx_size = size;
    for (u32 i = 0; i < s_mods.n; i++) {
        u32 p = (u32)(s_mods.e[i].key ^ (s_mods.e[i].key >> 32)) & (size - 1u);
        while (idx[p]) p = (p + 1) & (size - 1u);
        idx[p] = i + 1;
    }
}

/* One file of pack `pack`: kept unless an earlier pack (or an earlier file of
 * this one) already replaces that texture. */
static int eng_mod_add(u64 key, const char* path, u32 pack)
{
    if (eng_mod_find(key)) return 0;
    const u32 len = (u32)strlen(path) + 1u;
    if (s_mods.pool_used + len > s_mods.pool_cap) {
        u32 cap = s_mods.pool_cap ? s_mods.pool_cap * 2u : 65536u;
        while (cap < s_mods.pool_used + len) cap *= 2u;
        char* np = (char*)realloc(s_mods.pool, cap);
        if (!np) return 0;
        s_mods.pool = np; s_mods.pool_cap = cap;
    }
    if (s_mods.n == s_mods.cap) {
        const u32 cap = s_mods.cap ? s_mods.cap * 2u : 256u;
        eng_mod_entry* ne = (eng_mod_entry*)realloc(s_mods.e, cap * sizeof *ne);
        if (!ne) return 0;
        s_mods.e = ne; s_mods.cap = cap;
    }
    memcpy(s_mods.pool + s_mods.pool_used, path, len);
    eng_mod_entry* m = &s_mods.e[s_mods.n++];
    memset(m, 0, sizeof *m);
    m->key = key; m->path = s_mods.pool_used; m->pack = (u16)pack;
    s_mods.pool_used += len;
    if (s_mods.n * 2u + 2u > s_mods.idx_size) eng_mod_index_rebuild();
    else {
        u32 p = (u32)(key ^ (key >> 32)) & (s_mods.idx_size - 1u);
        while (s_mods.idx[p]) p = (p + 1) & (s_mods.idx_size - 1u);
        s_mods.idx[p] = s_mods.n;
    }
    return 1;
}

static void eng_mod_scan(const char* dir, u32 pack, int depth, u32* files, u32* shadowed)
{
#if !defined(_WIN32)
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) eng_mod_scan(path, pack, depth + 1, files, shadowed);
            continue;
        }
        const u64 key = eng_mod_name_key(de->d_name);
        if (!key) continue;
        if (eng_mod_add(key, path, pack)) (*files)++; else (*shadowed)++;
    }
    closedir(d);
#else
    (void)dir; (void)pack; (void)depth; (void)files; (void)shadowed;
#endif
}

static void eng_mkdirs(const char* dir)
{
    char b[512];
    snprintf(b, sizeof b, "%s", dir);
    for (char* q = b + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(b, 0755); *q = '/'; }
    mkdir(b, 0755);
}

/* Read the knobs and index the packs, once (on the first upload). */
static void eng_mods_init(void)
{
    if (s_mods.init) return;
    s_mods.init = 1;
    const char* dump = getenv("PS3_MODS_DUMP");
    if (dump && *dump) {
        if (!g.be->image_save) fprintf(stderr, "[mods] PS3_MODS_DUMP: this backend cannot write images; no dump\n");
        else { snprintf(s_mods.dump_dir, sizeof s_mods.dump_dir, "%s", dump); eng_mkdirs(dump);
               fprintf(stderr, "[mods] dumping colour textures to %s (<key>.png, textures.txt)\n", dump); }
    }
    const char* list = getenv("PS3_MODS");
    if (!list || !*list) return;
    if (!g.be->image_load) { fprintf(stderr, "[mods] PS3_MODS: this backend cannot read images; no texture packs\n"); return; }
#if defined(_WIN32)
    fprintf(stderr, "[mods] PS3_MODS: texture packs are not supported on this platform\n");
    return;
#endif
    const char* dir = getenv("PS3_MODS_DIR");
    const char* q = list;
    while (*q && s_mods.npacks < 16) {
        while (*q == ',' || *q == ' ') q++;
        const char* e = q;
        while (*e && *e != ',') e++;
        const char* t = e;
        while (t > q && t[-1] == ' ') t--;
        if (t > q) {
            char name[160];
            snprintf(name, sizeof name, "%.*s", (int)(t - q), q);
            char path[600];
            if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
            else if (dir && *dir) snprintf(path, sizeof path, "%s/%s", dir, name);
            else { fprintf(stderr, "[mods] pack \"%s\": PS3_MODS_DIR is not set; skipped\n", name); q = e; continue; }
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "[mods] pack \"%s\": no folder %s; skipped\n", name, path);
            } else {
                const u32 id = s_mods.npacks++;
                snprintf(s_mods.pack[id], sizeof s_mods.pack[id], "%s", name);
                u32 files = 0, shadowed = 0;
                eng_mod_scan(path, id, 0, &files, &shadowed);
                fprintf(stderr, "[mods] pack %u \"%s\": %u texture(s)%s", id + 1, name, files,
                        shadowed ? "" : "\n");
                if (shadowed) fprintf(stderr, ", %u left to an earlier pack\n", shadowed);
            }
        }
        q = e;
    }
    s_mods.on = s_mods.n > 0;
}

static int eng_mods_wanted(void)
{
    if (__builtin_expect(!s_mods.init, 0)) eng_mods_init();
    return s_mods.on || s_mods.dump_dir[0] || eng_texdump_on();
}

/* Halve an RGBA8 image (box filter; an odd edge repeats its last texel). */
static void eng_rgba_half(const u8* src, u32 w, u32 h, u8* dst, u32 nw, u32 nh)
{
    for (u32 y = 0; y < nh; y++)
        for (u32 x = 0; x < nw; x++) {
            const u32 x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0;
            const u32 y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
            const u8* a = src + ((size_t)y0 * w + x0) * 4; const u8* b = src + ((size_t)y0 * w + x1) * 4;
            const u8* c = src + ((size_t)y1 * w + x0) * 4; const u8* d = src + ((size_t)y1 * w + x1) * 4;
            u8* o = dst + ((size_t)y * nw + x) * 4;
            for (int k = 0; k < 4; k++) o[k] = (u8)((a[k] + b[k] + c[k] + d[k] + 2) >> 2);
        }
}

/* The pack's replacement for `key` as a backend texture, or 0 (none, or it
 * cannot be used: then the original is uploaded as always). */
static u32 eng_texmod_apply(u64 key, u32 fmt, u32 w, u32 h, u32 nlv, u32 remap)
{
    eng_mod_entry* m = (eng_mod_entry*)eng_mod_find(key);
    if (!m || m->bad) return 0;
    const char* path = s_mods.pool + m->path;
    u8* px = NULL; u32 pw = 0, ph = 0;
    if (!g.be->image_load(g.be->user, path, &px, &pw, &ph) || !px || !pw || !ph) {
        fprintf(stderr, "[mods] %s: cannot read the image; the original is used\n", path);
        free(px); m->bad = 1; return 0;
    }
    if (pw > 8192u || ph > 8192u) {
        fprintf(stderr, "[mods] %s is %ux%u: at most 8192x8192; the original is used\n", path, pw, ph);
        free(px); m->bad = 1; return 0;
    }
    if ((fmt & 0x40u) && (pw != w || ph != h)) {
        fprintf(stderr, "[mods] %s is %ux%u: the game reads this texture in texels, so it must stay %ux%u;"
                        " the original is used\n", path, pw, ph, w, h);
        free(px); m->bad = 1; return 0;
    }
    u32 mips = 1;
    if (nlv > 1) { u32 big = pw > ph ? pw : ph; while (big > 1) { big >>= 1; mips++; } }
    const u32 handle = g.be->texture_create(g.be->user, RSX_BE_FMT_R8G8B8A8, pw, ph, mips, 1, remap, fmt);
    if (!handle) { free(px); return 0; }
    g.be->texture_upload(g.be->user, handle, 0, 0, pw, ph, px, pw * 4u, ph);
    u32 cw = pw, chh = ph;
    for (u32 lvl = 1; lvl < mips; lvl++) {
        const u32 nw = cw > 1 ? cw / 2 : 1, nh = chh > 1 ? chh / 2 : 1;
        u8* half = (u8*)malloc((size_t)nw * nh * 4);
        if (!half) break;
        eng_rgba_half(px, cw, chh, half, nw, nh);
        free(px); px = half; cw = nw; chh = nh;
        g.be->texture_upload(g.be->user, handle, 0, lvl, cw, chh, px, cw * 4u, chh);
    }
    free(px);
    s_mods.applied++;
    if (!m->logged) {
        m->logged = 1;
        fprintf(stderr, "[mods] %016llx %ux%u fmt 0x%02X <- \"%s\" %s (%ux%u)\n", (unsigned long long)key,
                w, h, fmt, s_mods.pack[m->pack], path, pw, ph);
    }
    return handle;
}

/* DXT blocks (BC1/2/3, host byte order as rsx_texture_decode leaves them) to
 * RGBA8, for the dump. */
static void eng_bc_color(const u8* b, u8 out[16][4], int punch)
{
    u8 pal[4][4];
    const u32 c[2] = { (u32)b[0] | ((u32)b[1] << 8), (u32)b[2] | ((u32)b[3] << 8) };
    for (int k = 0; k < 2; k++) {
        pal[k][0] = (u8)(((c[k] >> 11) & 31u) * 255u / 31u);
        pal[k][1] = (u8)(((c[k] >> 5) & 63u) * 255u / 63u);
        pal[k][2] = (u8)((c[k] & 31u) * 255u / 31u);
        pal[k][3] = 255;
    }
    for (int ch = 0; ch < 3; ch++) {
        if (!punch || c[0] > c[1]) {
            pal[2][ch] = (u8)((2 * pal[0][ch] + pal[1][ch] + 1) / 3);
            pal[3][ch] = (u8)((pal[0][ch] + 2 * pal[1][ch] + 1) / 3);
        } else {
            pal[2][ch] = (u8)((pal[0][ch] + pal[1][ch] + 1) / 2);
            pal[3][ch] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (!punch || c[0] > c[1]) ? 255 : 0;
    const u32 idx = (u32)b[4] | ((u32)b[5] << 8) | ((u32)b[6] << 16) | ((u32)b[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3u], 4);
}
static void eng_bc_decode(const u8* blocks, u32 row_bytes, u32 w, u32 h, rsx_texfmt f, u8* rgba)
{
    const u32 bb = f == RSX_TEXFMT_BC1 ? 8u : 16u;
    for (u32 by = 0; by * 4 < h; by++)
        for (u32 bx = 0; bx * 4 < w; bx++) {
            const u8* b = blocks + (size_t)by * row_bytes + (size_t)bx * bb;
            u8 t[16][4];
            eng_bc_color(f == RSX_TEXFMT_BC1 ? b : b + 8, t, f == RSX_TEXFMT_BC1);
            if (f == RSX_TEXFMT_BC2) {
                for (int i = 0; i < 16; i++) t[i][3] = (u8)(((b[i / 2] >> ((i & 1) * 4)) & 15u) * 17u);
            } else if (f == RSX_TEXFMT_BC3) {
                u8 a[8]; a[0] = b[0]; a[1] = b[1];
                if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = (u8)(((7 - k) * a[0] + k * a[1] + 3) / 7);
                else { for (int k = 1; k < 5; k++) a[k + 1] = (u8)(((5 - k) * a[0] + k * a[1] + 2) / 5);
                       a[6] = 0; a[7] = 255; }
                u64 bits = 0;
                for (int k = 0; k < 6; k++) bits |= (u64)b[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++) t[i][3] = a[(bits >> (3 * i)) & 7u];
            }
            for (u32 i = 0; i < 16; i++) {
                const u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(rgba + ((size_t)y * w + x) * 4, t[i], 4);
            }
        }
}

/* PS3_MODS_DUMP: level 0 of the texture with this key, decoded (in
 * g.tex_staging, laid out as `tl` says), as <dir>/<key>.png -- once a key. */
static void eng_texmod_dump(u64 key, u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            const rsx_tex_layout* tl)
{
    if (!s_mods.dump_dir[0] || !key) return;
    if (s_mods.dumped_n * 2u + 2u > s_mods.dumped_size) {
        if (s_mods.dumped_size >= (1u << 20)) return;          /* a million: enough */
        const u32 size = s_mods.dumped_size ? s_mods.dumped_size * 2u : 4096u;
        u64* nd = (u64*)calloc(size, sizeof *nd);
        if (!nd) return;
        for (u32 i = 0; i < s_mods.dumped_size; i++)
            if (s_mods.dumped[i]) {
                u32 p = (u32)(s_mods.dumped[i] ^ (s_mods.dumped[i] >> 32)) & (size - 1u);
                while (nd[p]) p = (p + 1) & (size - 1u);
                nd[p] = s_mods.dumped[i];
            }
        free(s_mods.dumped); s_mods.dumped = nd; s_mods.dumped_size = size;
    }
    u32 p = (u32)(key ^ (key >> 32)) & (s_mods.dumped_size - 1u);
    for (; s_mods.dumped[p]; p = (p + 1) & (s_mods.dumped_size - 1u))
        if (s_mods.dumped[p] == key) return;
    s_mods.dumped[p] = key;
    s_mods.dumped_n++;
    char path[640];
    snprintf(path, sizeof path, "%s/%016llx.png", s_mods.dump_dir, (unsigned long long)key);
    int ok = 0;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        ok = g.be->image_save(g.be->user, path, g.tex_staging, w, h, tl->dst_row_bytes);
    } else {
        u8* rgba = (u8*)calloc((size_t)w * h, 4);
        if (!rgba) return;
        eng_bc_decode(g.tex_staging, tl->dst_row_bytes, w, h, tl->fmt, rgba);
        ok = g.be->image_save(g.be->user, path, rgba, w, h, w * 4u);
        free(rgba);
    }
    if (!ok) return;
    static const char* const fname[] = { "R8", "R8G8", "RGBA8", "DXT1", "DXT3", "DXT5" };
    snprintf(path, sizeof path, "%s/textures.txt", s_mods.dump_dir);
    FILE* ix = fopen(path, "a");
    if (ix) {
        fprintf(ix, "%016llx %4ux%-4u %-5s fmt 0x%02X loc %u off 0x%08X frame %u\n", (unsigned long long)key,
                w, h, (unsigned)tl->fmt < 6u ? fname[tl->fmt] : "?", fmt, location, offset, g.frames);
        fclose(ix);
    }
}

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. */
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    s_upload_atlas = 0;
    if (!nlv || !lv[0].tl.face_bytes) return 0;
    /* A volume: every slice of every level decoded and uploaded on its own.
     * inFAMOUS 2's occlusion volumes read an 8x8x8 cone LUT; uploaded as its
     * first slice and sampled in 2D it darkened whole boxes round every prop. */
    if (depth > 1 && !cube && g.be->texture_create_3d && g.be->texture_upload_3d &&
        !lv[0].tl.compressed && lv[0].tl.bytes_per_texel) {
        const u32 span = eng_texture_span_3d(fmt, w, h, depth, levels, pitch);
        const u8* src = eng_guest_ptr(NULL, location, offset, span);
        if (!src) return 0;
        const u32 handle = g.be->texture_create_3d(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                   w, h, depth, nlv, remap, fmt);
        if (!handle) return 0;
        const u8* level_src = src;
        u32 d = depth;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            const u32 lin_bytes = tl->swizzled ? tl->row_bytes * tl->rows : 0u;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows + lin_bytes)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            for (u32 z = 0; z < d; z++) {
                if (tl->swizzled) {
                    u8* lin = g.tex_staging + tl->dst_row_bytes * tl->rows;
                    eng_deswizzle_3d_slice(lin, level_src, lv[m].w, lv[m].h, d, z, tl->bytes_per_texel);
                    rsx_tex_layout ltl = *tl; ltl.swizzled = 0;
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes, lin, lv[m].w, lv[m].h,
                                       &ltl, rsx_texture_argb_is_rgba());
                } else {
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                                       level_src + (size_t)z * tl->face_bytes, lv[m].w, lv[m].h,
                                       tl, rsx_texture_argb_is_rgba());
                }
                g.be->texture_upload_3d(g.be->user, handle, z, m, lv[m].w, lv[m].h,
                                        g.tex_staging, tl->dst_row_bytes);
            }
            level_src += (size_t)tl->face_bytes * d;
            if (d > 1) d >>= 1;
        }
        return handle;
    }
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_ptr(NULL, location, offset, span);
    if (!src) return 0;

    /* Texture packs and their dump (PS3_MODS, PS3_MODS_DUMP), by content. */
    u64 mod_key = 0;
    if (!cube && !s_upload_into && eng_mods_wanted()) {
        const rsx_texfmt tf = lv[0].tl.fmt;
        if (tf == RSX_TEXFMT_R8G8B8A8 || tf == RSX_TEXFMT_BC1 || tf == RSX_TEXFMT_BC2 || tf == RSX_TEXFMT_BC3) {
            int readable = 0;
            const u64 content = eng_texture_content_hash(location, offset, span, &readable);
            if (readable) mod_key = eng_texmod_key(content, fmt, w, h);
        }
        if (mod_key && s_mods.on) {
            const u32 hnd = eng_texmod_apply(mod_key, fmt, w, h, nlv, remap);
            if (hnd) {
                if (s_mods.dump_dir[0] && eng_staging_reserve(lv[0].tl.dst_row_bytes * lv[0].tl.rows)) {
                    rsx_texture_decode(g.tex_staging, lv[0].tl.dst_row_bytes, src + lv[0].offset,
                                       lv[0].w, lv[0].h, &lv[0].tl, rsx_texture_argb_is_rgba());
                    eng_texmod_dump(mod_key, location, offset, fmt, w, h, &lv[0].tl);
                }
                return hnd;
            }
        }
    }

    const u32 handle = s_upload_into ? s_upload_into
                     : g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                            w, h, nlv, faces, remap, fmt);
    if (!handle) return 0;

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                if (!s_upload_into) g.be->texture_release(g.be->user, handle);
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            if (f == 0 && m == 0) {
                eng_texdump(location, offset, fmt, w, h, tl, mod_key);
                if (mod_key) eng_texmod_dump(mod_key, location, offset, fmt, w, h, tl);
                /* The button-prompt atlas: redrawn for the controller in use
                 * (one 512x256 ARGB compare for anything else). */
                if (!cube && tl->fmt == RSX_TEXFMT_R8G8B8A8 && w == 512u && h == 256u)
                    s_upload_atlas = rsx_button_icons_upload(fmt, w, h, nlv, g.tex_staging, tl->dst_row_bytes);
            }
            g.be->texture_upload(g.be->user, handle, f, m, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
/* Open-addressed index over g.textures (value = entry index + 1): a bind
 * used to scan the whole cache, per draw and per unit. Rebuilt whenever an
 * entry is added or replaced, which is rare once the cache is warm. */
#define ENG_TIDX_SIZE (ENG_MAX_TEXTURES * 4u)
static u32 s_tidx[ENG_TIDX_SIZE];
static inline u32 eng_tkey(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                           u32 pitch, u32 remap, u32 cube, u32 depth)
{
    u64 k = ((u64)offset << 3) ^ location ^ ((u64)fmt << 40) ^ ((u64)w << 20)
          ^ ((u64)h << 52) ^ ((u64)pitch << 28) ^ ((u64)remap * 0x9E3779B97F4A7C15ull) ^ cube
          ^ ((u64)depth << 44);
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33;
    return (u32)k & (ENG_TIDX_SIZE - 1u);
}
static void eng_tidx_rebuild(void)
{
    memset(s_tidx, 0, sizeof s_tidx);
    for (u32 i = 0; i < g.n_textures; i++) {
        const eng_texture* e = &g.textures[i];
        u32 p = eng_tkey(e->location, e->offset, e->format, e->width, e->height,
                         e->pitch, e->remap, e->cubemap, e->depth);
        while (s_tidx[p]) p = (p + 1) & (ENG_TIDX_SIZE - 1u);
        s_tidx[p] = i + 1;
    }
}

static u32 eng_texture_slot_impl(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                                 u32 levels, u32 pitch, int cube, u32 remap, u32 depth);

/* A unit whose bytes could not become a texture samples the null (black)
 * texture, which is what a black object usually is. The first 48 distinct
 * failures are logged with everything the registers said (PS3RECOMP_ENG_TEXFAIL=0 silences). */
static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    const u32 handle = eng_texture_slot_impl(location, offset, fmt, w, h, levels, pitch, cube, remap, depth);
    if (!handle) {
        static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_TEXFAIL"); on = e ? atoi(e) : 1; }
        static u64 seen[48]; static u32 ns;
        const u64 k = ((u64)location << 60) ^ ((u64)offset << 24) ^ ((u64)fmt << 16) ^ ((u64)w << 8) ^ h;
        u32 i = 0; while (i < ns && seen[i] != k) i++;
        if (on && i == ns && ns < 48) { seen[ns++] = k;
            fprintf(stderr, "[rsx engine] texture bind FAILED: loc %u off 0x%08X fmt 0x%02X %ux%u mips %u pitch %u cube %d depth %u remap 0x%04X -> black\n",
                    location, offset, fmt, w, h, levels, pitch, cube, depth, remap); }
    }
    return handle;
}

/* Drawstats: the once-a-frame content re-hash of cached textures. */
static u64 ds_th_ns, ds_th_bytes, ds_th_count, ds_th_changed;
static u64 ds_th_wt_clean, ds_vc_wt_clean, ds_ic_wt_clean;   /* rsx_wtrack: checks skipped */
/* Drawstats and state of the vertex cache (defined with it, below dc_reset). */
static u64 ds_vc_hit, ds_vc_miss, ds_vc_skip, ds_vc_new, ds_vc_hashed, ds_vc_ns;
/* ... and of the resident index buffers (PS3RECOMP_ENG_ICACHE, below the vertex cache). */
static u32 s_ic_used;
static u64 s_ic_bytes;
static u64 ds_ic_hit, ds_ic_miss, ds_ic_new, ds_ic_hashed;
static u32 s_vc_used;
static u64 s_vc_bytes;
static int eng_ds_enabled(void);
static u64 eng_ds_now(void);

static u32 eng_texture_slot_impl(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                                 u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) return 0;
    if (!depth) depth = 1;

    /* The span (a mip-chain walk) is kept with the entry: the key holds
     * everything it depends on except the level count, which is checked. */
    for (u32 p = eng_tkey(location, offset, fmt, w, h, pitch, remap, (u32)(cube != 0), depth);
         s_tidx[p]; p = (p + 1) & (ENG_TIDX_SIZE - 1u)) {
        eng_texture* e = &g.textures[s_tidx[p] - 1];
        if (e->location != location || e->offset != offset ||
            e->format != fmt || e->width != w || e->height != h ||
            e->pitch != pitch || e->remap != remap ||
            e->cubemap != (u32)(cube != 0) || e->depth != depth)
            continue;
        if (e->levels != levels) {
            e->levels = levels;
            e->span = depth > 1 ? eng_texture_span_3d(fmt, w, h, depth, levels, pitch)
                                : eng_texture_span(fmt, w, h, levels, pitch, cube);
        }
        const u32 span = e->span;
        if (!span) return 0;
        /* rsx_wtrack (PS3RECOMP_WTRACK): pages still write-protected since the
         * last check left the texture as uploaded -- nothing to hash. A span
         * that is not clean is armed first and then hashed in full (exact);
         * one that cannot be armed (pages that keep being written) takes the
         * sparse/budgeted path below as before. */
        int wt_mode = 0, wt_clean = 0, wt_armed = 0;
        u64 wt_st = 0;
        const u8* wt_src = NULL;
        if (e->handle && e->last_hash_frame != g.frames && (wt_mode = rsx_wt_mode()) != 0 &&
            (wt_src = eng_guest_ptr(NULL, location, offset, span)) != NULL) {
            wt_clean = e->wt_ok && rsx_wt_query(wt_src, span, &wt_st) && wt_st == e->wt_stamp;
            if (wt_clean && wt_mode == 1) { e->last_hash_frame = g.frames; ds_th_wt_clean++; }
            else if (!wt_clean) { wt_st = 0; wt_armed = rsx_wt_arm(wt_src, span, &wt_st); }
        }
        if (e->handle && e->last_hash_frame != g.frames) {
            /* Every cached texture used to be hashed in full every frame:
             * ~950 textures, 88 MB, 2.5 ms of the RSX thread a frame in New
             * Marais, for 4 that changed. Now a sparse check every frame and
             * the full hash every eighth (staggered by slot), or always for
             * a small texture. PS3RECOMP_ENG_TEXHASH_FULL=1: full every frame. */
            if (__builtin_expect(s_texhash_full < 0, 0))
                ps3_toggle_watch(PS3T_ENG_TEXHASH_FULL, eng_texhash_full_set);
            const int full_always = __atomic_load_n(&s_texhash_full, __ATOMIC_RELAXED);
            /* Due by age, not by (frame + slot) & 7: a texture sampled only on
             * alternate frames could miss its turn for ever. */
            int full = full_always || span <= 16384u || wt_armed || wt_clean;
            /* The age-due full hashes are budgeted per frame: textures that
             * come into view together (a camera cut) fall due together, every
             * eighth frame -- 50-73 MB of hashing, 4-7 ms of the RSX thread,
             * in one frame of the Empire City cutscene. Past the budget a
             * texture stays due and goes next frame; the first one of a frame
             * always goes, so no size starves. A sparse-hash change is never
             * held back. PS3RECOMP_ENG_TEXHASH_BUDGET_MB (default 12, 0 = no
             * budget). */
            if (!full && g.frames - e->last_full_frame >= 8u) {
                static long budget_mb = -1;
                static u32 budget_frame = ~0u;
                static u64 budget_left;
                if (budget_mb < 0) { const char* be = getenv("PS3RECOMP_ENG_TEXHASH_BUDGET_MB"); budget_mb = be ? atol(be) : 12; }
                const u64 budget = (u64)budget_mb << 20;
                if (budget_frame != g.frames) { budget_frame = g.frames; budget_left = budget; }
                if (!budget_mb || budget_left == budget || span <= budget_left) {
                    full = 1;
                    budget_left = span <= budget_left ? budget_left - span : 0;
                }
            }
            int readable = 0;
            const u64 th0 = eng_ds_enabled() ? eng_ds_now() : 0;
            const u64 sparse = eng_texture_sparse_hash(location, offset, span, &readable);
            u64 hash = e->content_hash;
            const u64 pre_hash = e->content_hash;
            if (readable && (full || sparse != e->sparse_hash)) {
                hash = eng_texture_content_hash(location, offset, span, &readable);
                e->last_full_frame = g.frames;
            }
            if (th0) { ds_th_ns += eng_ds_now() - th0; ds_th_count++;
                       ds_th_bytes += (full || sparse != e->sparse_hash) ? span : span / 32u;
                       ds_th_changed += readable && hash != e->content_hash; }
            e->last_hash_frame = g.frames;
            if (readable) e->sparse_hash = sparse;
            if (readable && hash != e->content_hash) {
                const uint64_t ft_u0 = PS3_FT_ON() ? ps3_ft_now() : 0;
                const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                     levels, pitch, cube, remap, depth);
                if (ft_u0) ps3_ft_span(PS3_FT_TEXUP, 0, span >> 10, 1, ft_u0, ps3_ft_now());
                if (fresh) {
                    g.be->texture_release(g.be->user, e->handle);
                    e->handle = fresh;
                    e->content_hash = hash;
                    e->icon_atlas = (u8)s_upload_atlas;
                    e->icon_gen = s_icon_gen;
                }
            }
            if (wt_clean) {                     /* shadow mode: was the tracker right? */
                if (readable && hash != pre_hash) { rsx_wt_report_miss("texture", wt_src, span); e->wt_ok = 0; }
                else rsx_wt_note_verified();
            } else if (wt_src) {
                /* Clean from here on only if armed and the upload is current. */
                e->wt_ok = (u8)(wt_armed && readable && hash == e->content_hash);
                e->wt_stamp = wt_st;
            }
        }
        /* The prompt glyphs changed (another controller, the menu, rebound
         * controls): the atlas alone is decoded and redrawn again. */
        if (__builtin_expect(e->icon_atlas && e->icon_gen != s_icon_gen, 0) && e->handle) {
            const u32 old = e->handle;
            u32 fresh = eng_texture_upload(location, offset, fmt, w, h, levels, pitch, cube, remap, depth);
            if (fresh) { g.be->texture_release(g.be->user, e->handle); e->handle = fresh; }
            else {
                /* No new texture to be had: in New Marais the backend's
                 * object table is full (4096) and creations fail until
                 * something is freed. Redraw the old one in place instead;
                 * at worst a frame already queued samples it half-done. */
                s_upload_into = old;
                fresh = eng_texture_upload(location, offset, fmt, w, h, levels, pitch, cube, remap, depth);
                s_upload_into = 0;
            }
            e->icon_gen = s_icon_gen;   /* (failing both ways: given up until the next change) */
            { static int on = -1; if (on < 0) on = getenv("PS3RECOMP_ENG_TEXLOG") ? 1 : 0;
              if (on) fprintf(stderr, "[rsx texlog] button atlas h%u -> h%u redrawn%s (loc %u off 0x%08X, glyph set %u, atlas %d)\n",
                              old, fresh, fresh == old ? " in place" : "", location, offset, s_icon_gen, s_upload_atlas); }
        }
        e->last_use_serial = ++g.texture_use_serial;
        return e->handle;
    }

    const u32 span = depth > 1 ? eng_texture_span_3d(fmt, w, h, depth, levels, pitch)
                               : eng_texture_span(fmt, w, h, levels, pitch, cube);
    if (!span) return 0;

    u32 index;
    u32 evicted = 0;
    if (g.n_textures < ENG_MAX_TEXTURES) {
        index = g.n_textures++;
    } else {
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial < g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].handle;
    }

    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0); e.depth = depth;
    e.levels = levels; e.span = span;
    e.last_hash_frame = g.frames;
    e.last_full_frame = g.frames - (index & 7u);   /* staggered: not all due on one frame */
    e.last_use_serial = ++g.texture_use_serial;
    const uint64_t ft_u0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    int new_readable = 0, new_armed = 0;
    u64 new_st = 0;
    if (rsx_wt_mode()) {                         /* armed before the hash (rsx_wtrack.h) */
        const u8* src = eng_guest_ptr(NULL, location, offset, span);
        if (src) new_armed = rsx_wt_arm(src, span, &new_st);
    }
    { e.content_hash = eng_texture_content_hash(location, offset, span, &new_readable);
      int readable = 0;
      e.sparse_hash = eng_texture_sparse_hash(location, offset, span, &readable); }
    e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                  cube, remap, depth);
    e.wt_ok = (u8)(new_armed && new_readable && e.handle);
    e.wt_stamp = new_st;
    e.icon_atlas = (u8)s_upload_atlas;
    e.icon_gen = s_icon_gen;
    if (ft_u0) ps3_ft_span(PS3_FT_TEXUP, 0, span >> 10, evicted ? 2u : 0u, ft_u0, ps3_ft_now());
    /* PS3RECOMP_ENG_TEXLOG=1: one line per texture the engine creates (guest
     * location/offset, format byte, size, levels, pitch, cube, crossbar) and
     * the backend handle it got, so a pass log's tex= numbers can be read. */
    { static int on = -1; if (on < 0) on = getenv("PS3RECOMP_ENG_TEXLOG") ? 1 : 0;
      if (on) fprintf(stderr, "[rsx texlog] h%u <- loc %u off 0x%08X fmt 0x%02X %ux%u mips %u pitch %u cube %d remap 0x%04X span %u\n",
                      e.handle, location, offset, fmt, w, h, levels, pitch, cube, remap, span); }
    if (e.handle && evicted) g.be->texture_release(g.be->user, evicted);
    if (e.handle || !evicted) { g.textures[index] = e; eng_tidx_rebuild(); }
    return e.handle;
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point. RSX's LOD
 * bias, SET_TEXTURE_FILTER [12:0], is not applied: the reference engine
 * leaves it at zero too. */
/* SET_TEXTURE_BORDER_COLOR as rsx_be_sampler_desc.border. inFAMOUS 2's sun
 * shadow maps (DEPTH24_D8 1024x1024, wrap BORDER, compare GREATER) set
 * 0x00FFFFFF -- depth 1.0 -- so a lookup outside the map compares against
 * the far plane; read as transparent black it compared against 0 instead and
 * flipped the shadow term for everything the map does not cover. */
static u8 eng_border_class(u32 format, u32 border)
{
    static int on = -1;   /* PS3RECOMP_ENG_BORDER=0: always transparent black (the old behaviour) */
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_BORDER"); on = e ? atoi(e) : 1; }
    if (!on) return 0u;
    const u32 base = format & 0x9Fu;
    if (base >= 0x90u && base <= 0x93u) {          /* DEPTH24_D8 / _FLOAT, DEPTH16 / _FLOAT */
        const u32 d = border & 0x00FFFFFFu;
        return d == 0x00FFFFFFu ? 2u : 0u;          /* 1.0 -> white (r = 1); 0 -> black */
    }
    if (border == 0x00000000u) return 0u;
    if (border == 0xFF000000u) return 1u;
    if (border == 0xFFFFFFFFu) return 2u;
    return 3u;                                      /* not expressible: edge clamp */
}

static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6);
    out->mag_linear  = (u8)(magf == 2);
    out->mip_present = (u8)(minf >= 3);
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    /* The three fields rsx_dsp_get_vertex_texture would decode (enable =
     * CONTROL0 bit 31, width/height = IMAGE_RECT) read directly: this runs
     * twice a draw and the full decode (a memset and eleven fields per unit)
     * showed in the RSX thread's profile. */
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        const u32 base = 0x0900u + u * 0x20u;    /* NV4097_SET_VERTEX_TEXTURE_OFFSET + unit block */
        const u32 ctl0 = rsx_dsp_reg(&g.rsx, base + 0x0Cu), rect = rsx_dsp_reg(&g.rsx, base + 0x18u);
        if ((ctl0 >> 31) && (rect >> 16) && (rect & 0xFFFFu)) mask |= 1u << u;
    }
    return mask;
}

/* Units bound to a 3D texture (SET_TEXTURE_FORMAT dimension 3 with a
 * depth), declared Texture3D and sampled with three coordinates. Only when
 * the backend can create one: otherwise the first slice stays a 2D texture
 * and the program samples it in 2D, as before. */
static u32 eng_volume_mask(const rsx_dsp_texture tex[RSX_DSP_NUM_TEXTURES])
{
    if (!g.be->texture_create_3d || !g.be->texture_upload_3d) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        const rsx_dsp_texture* t = &tex[u];
        if (t->enabled && !t->cubemap && t->dimension == 3 && t->depth > 1 && t->width && t->height)
            mask |= 1u << u;
    }
    return mask;
}

static u32 eng_cube_mask(const rsx_dsp_texture tex[RSX_DSP_NUM_TEXTURES])
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        const rsx_dsp_texture* t = &tex[u];
        /* A cube face is square by construction, so a non-square image
         * cannot be one; the fragment program is compiled against this mask
         * and a texturecube slot filled with a 2D texture is a validation
         * failure, not a wrong pixel. */
        if (t->enabled && t->cubemap && t->width && t->width == t->height)
            mask |= 1u << u;
    }
    return mask;
}

/* ---- program memo ------------------------------------------------------ */

/* Everything a draw derives from its programs' bytes -- the size, the input
 * and constant analyses, the pipeline-key hashes, the FP's inline constants
 * -- is a function of those bytes alone. A draw nearly always runs a program
 * an earlier draw ran, so each memo entry keeps an exact copy of the bytes it
 * was derived from: when the program's bytes compare equal the derivation is
 * reused, and any difference at all (a patched constant, a new program at the
 * same address) recomputes it. An identical prefix up to the stored size also
 * means an identical size, since the size is found by scanning those bytes
 * for the end flag. */
#define ENG_PROG_MEMO 8

typedef struct {
    u32 start, instrs;          /* instrs == 0: unused entry */
    u32 input_mask;             /* rsx_vp_analyze_inputs, 0xFFFF when unsure */
    int vpc_full;
    u32 vpc_lo, vpc_n;
    u64 hash;                   /* of the program bytes */
    u32 used;                   /* recency */
    u8 bytes[RSX_DSP_VP_INSTR * 16u];
} eng_vp_memo;

typedef struct {
    u32 loc, off, size;         /* size == 0: unused entry */
    u64 structural;             /* rsx_fp_structural_hash; 0 = will not hash */
    int constants_ok;
    u32 tex_mask;               /* rsx_fp_texture_mask */
    /* Per-program halves of the per-draw unit masks, so a draw does not walk
     * the microcode three times: the units any fetch samples
     * (rsx_fp_texture_units), and those whose fetch could decode gamma or
     * expand signs if the bound texture asks for it (the scans with every
     * unit's mask set). */
    u32 sampled_units, gamma_cand, expand_cand;
    u32 used;
    u8* bytes;
    rsx_fp_constant_block constants;
} eng_fp_memo;

static eng_vp_memo s_vp_memo[ENG_PROG_MEMO];
/* PS3RECOMP_ENG_VP_GEN: the entry the last lookup found, and the dispatcher's
 * vp_gen and program start it was found under. */
static eng_vp_memo* s_vpgen_memo;
static u32 s_vpgen_gen, s_vpgen_start;
static int s_vpgen_on = -1;
static void eng_vpgen_set(int id, int v) { (void)id; __atomic_store_n(&s_vpgen_on, v != 0, __ATOMIC_RELAXED); }
/* PS3RECOMP_ENG_FP_MEMO (1..64, default 64 since rsx3; 8 = the old size): how many
 * fragment programs are kept. A heavy frame runs far more than eight, so the
 * old table missed and re-analysed (copy, structural hash, constants, three
 * microcode walks) on many draws. The lookup scans compact key arrays, not the
 * 32 KB entries. */
#define ENG_FP_MEMO_MAX 64u
static eng_fp_memo s_fp_memo[ENG_FP_MEMO_MAX];
static u64 s_fp_key[ENG_FP_MEMO_MAX];           /* loc << 32 | off */
static u32 s_fp_sz[ENG_FP_MEMO_MAX];            /* 0: unused */
static u32 s_fp_use[ENG_FP_MEMO_MAX];           /* recency */
static u32 s_fp_cap;
static u32 s_memo_clock;
static u64 ds_memo_hit[2], ds_memo_miss[2];     /* drawstats: vp, fp */

static u64 eng_fnv1a(const void* data, u32 n, u64 hash);

static u32 eng_memo_victim_vp(void)
{
    u32 v = 0;
    for (u32 i = 1; i < ENG_PROG_MEMO; i++)
        if (s_vp_memo[i].used < s_vp_memo[v].used) v = i;
    return v;
}

/* The resident vertex program's memo entry, or NULL when there is none. */
static const eng_vp_memo* eng_vp_current(void)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) return NULL;
    const u8* uc = (const u8*)(g.rsx.vp + start * 4);
    /* PS3RECOMP_ENG_VP_GEN (live): no word of the transform program has
     * changed since the last lookup (the dispatcher's vp_gen) and the start
     * is the same, so the entry that matched then still matches -- without
     * comparing up to 8 KB of microcode a draw. 0 = always compare. */
    if (__builtin_expect(s_vpgen_on < 0, 0)) ps3_toggle_watch(PS3T_ENG_VP_GEN, eng_vpgen_set);
    if (s_vpgen_on && s_vpgen_memo && s_vpgen_gen == g.rsx.vp_gen && s_vpgen_start == start &&
        s_vpgen_memo->instrs && s_vpgen_memo->start == start) {
        s_vpgen_memo->used = ++s_memo_clock;
        ds_memo_hit[0]++;
        return s_vpgen_memo;
    }
    s_vpgen_memo = NULL;
    for (u32 i = 0; i < ENG_PROG_MEMO; i++) {
        eng_vp_memo* m = &s_vp_memo[i];
        if (m->instrs && m->start == start && !memcmp(m->bytes, uc, m->instrs * 16u)) {
            m->used = ++s_memo_clock;
            ds_memo_hit[0]++;
            s_vpgen_memo = m; s_vpgen_gen = g.rsx.vp_gen; s_vpgen_start = start;
            return m;
        }
    }
    ds_memo_miss[0]++;
    const u32 instrs = rsx_vp_program_size_instrs(uc, (RSX_DSP_VP_INSTR - start) * 16u);
    if (!instrs) return NULL;
    eng_vp_memo* m = &s_vp_memo[eng_memo_victim_vp()];
    m->start = start;
    m->instrs = instrs;
    memcpy(m->bytes, uc, instrs * 16u);
    m->used = ++s_memo_clock;
    m->hash = eng_fnv1a(uc, instrs * 16u, 1469598103934665603ull);

    m->input_mask = 0xFFFFu;
    rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
    if (rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
        analysis.exact && analysis.input_mask)
        m->input_mask = analysis.input_mask;
    m->vpc_full = 1;
    m->vpc_lo = m->vpc_n = 0;
    rsx_vp_const_usage cu;
    if (rsx_vp_analyze_constants(uc, instrs * 16u, &cu) >= 0 && cu.valid && !cu.relative) {
        m->vpc_full = 0;
        m->vpc_lo = cu.lo <= cu.hi ? cu.lo : 0;
        m->vpc_n = cu.lo <= cu.hi ? cu.hi - cu.lo + 1u : 0;
    }
    s_vpgen_memo = m; s_vpgen_gen = g.rsx.vp_gen; s_vpgen_start = start;
    return m;
}

/* The bound fragment program's memo entry, or NULL when there is none (no
 * program, unmapped, or no end flag within ENG_FP_MAX_BYTES). */
static const eng_fp_memo* eng_fp_current(void)
{
    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    if (!rsx_dsp_reg(&g.rsx, 0x08E4)) return NULL;
    u32 loc = 0;
    const u32 off = rsx_dsp_fragment_program(&g.rsx, &loc);
    if (__builtin_expect(!s_fp_cap, 0)) {
        const char* e = getenv("PS3RECOMP_ENG_FP_MEMO");
        const long n = e ? strtol(e, NULL, 10) : 64L;   /* rsx3: default 64 (=8 the old size) */
        s_fp_cap = n < 1 ? 1u : n > (long)ENG_FP_MEMO_MAX ? ENG_FP_MEMO_MAX : (u32)n;
    }
    const u64 key = ((u64)loc << 32) | off;
    for (u32 i = 0; i < s_fp_cap; i++) {
        if (!s_fp_sz[i] || s_fp_key[i] != key) continue;
        eng_fp_memo* m = &s_fp_memo[i];
        const u8* uc = eng_guest_ptr(NULL, loc, off, m->size);
        if (uc && !memcmp(m->bytes, uc, m->size)) {
            s_fp_use[i] = m->used = ++s_memo_clock;
            ds_memo_hit[1]++;
            return m;
        }
    }
    ds_memo_miss[1]++;
    const u8* uc = eng_guest_ptr(NULL, loc, off, 16);
    u32 size = uc ? rsx_fp_program_size(uc, ENG_FP_MAX_BYTES) : 0;
    if (size) uc = eng_guest_ptr(NULL, loc, off, size);
    if (!uc || !size) return NULL;

    u32 v = 0;
    for (u32 i = 1; i < s_fp_cap; i++)
        if (s_fp_use[i] < s_fp_use[v]) v = i;
    eng_fp_memo* m = &s_fp_memo[v];
    u8* copy = (u8*)realloc(m->bytes, size);
    if (!copy) return NULL;
    m->bytes = copy;
    memcpy(m->bytes, uc, size);
    m->loc = loc;
    m->off = off;
    m->size = size;
    m->used = ++s_memo_clock;
    s_fp_key[v] = key; s_fp_sz[v] = size; s_fp_use[v] = m->used;
    m->structural = rsx_fp_structural_hash(uc, size, 1469598103934665603ull);
    m->constants.count = 0;
    m->constants_ok = rsx_fp_collect_constants(uc, size, &m->constants) >= 0;
    m->tex_mask = rsx_fp_texture_mask(uc, size);
    static const u8 all[RSX_DSP_NUM_TEXTURES] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
    m->sampled_units = rsx_fp_texture_units(uc, size);
    m->gamma_cand = rsx_fp_texture_gamma_units(uc, size, all);
    m->expand_cand = rsx_fp_texture_expand_units(uc, size, all);
    return m;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads. */
/* sink_end bumps this per draw. The programs cannot change within one draw,
 * and a draw asks for them several times (pipeline, constants, every texture
 * unit's mask, diagnostics), each a memo scan plus a memcmp of both programs'
 * microcode: answer from the first lookup of the draw. */
static u32 s_draw_serial = 1;

static int eng_guest_programs(const eng_vp_memo** out_vp, const eng_fp_memo** out_fp)
{
    static u32 serial;
    static const eng_vp_memo* c_vp;
    static const eng_fp_memo* c_fp;
    if (serial != s_draw_serial) {
        c_vp = eng_vp_current();
        c_fp = c_vp ? eng_fp_current() : NULL;
        serial = s_draw_serial;
    }
    const eng_vp_memo* vp = c_vp;
    const eng_fp_memo* fp = c_fp;
    if (out_vp) *out_vp = vp;
    if (out_fp) *out_fp = fp;
    return vp && fp;
}

static u64 eng_current_fp_structural(void)
{
    const eng_fp_memo* fpm = NULL;
    return eng_guest_programs(NULL, &fpm) && fpm ? fpm->structural : 0;
}

/* PS3RECOMP_ENG_TEXREG_ONCE (live): the sixteen texture units decoded from
 * the register file once per draw. sink_bind_textures and eng_pipeline_get
 * each decoded all of them, and nothing writes a register between the two.
 * 0 = each decodes its own. */
static int s_texreg_once = -1;
static int s_texgen_on = -1;   /* PS3RECOMP_ENG_TEXGEN, eng_draw_textures */
static u32 s_tex_dec_gen;      /* the texture generation the decoded units are (0: not tracked) */
static void eng_texreg_once_set(int id, int v) { (void)id; __atomic_store_n(&s_texreg_once, v != 0, __ATOMIC_RELAXED); }
/* Read-only: the decoded units themselves, not a copy (copying all sixteen
 * twice a draw was ~0.17 ms of the RSX thread a frame in the pier view). */
static const rsx_dsp_texture* eng_draw_textures(void)
{
    static u32 serial;
    static rsx_dsp_texture c[RSX_DSP_NUM_TEXTURES];
    if (__builtin_expect(s_texreg_once < 0, 0)) ps3_toggle_watch(PS3T_ENG_TEXREG_ONCE, eng_texreg_once_set);
    if (!__atomic_load_n(&s_texreg_once, __ATOMIC_RELAXED)) {
        for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) rsx_dsp_get_texture(&g.rsx, u, &c[u]);
        serial = 0;
        s_tex_dec_gen = 0;
        return c;
    }
    if (serial != s_draw_serial) {
        /* PS3RECOMP_ENG_TEXGEN (rsx3, default 1; 0 = decode every draw): only
         * when a texture register changed since the last decode. */
        static u32 gen;
        if (__builtin_expect(s_texgen_on < 0, 0)) { const char* e = getenv("PS3RECOMP_ENG_TEXGEN"); s_texgen_on = !(e && e[0] == '0'); }
        const u32 now = g_rsx_dsp_tex_gen;
        if (!s_texgen_on || !gen || gen != now) {
            for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) rsx_dsp_get_texture(&g.rsx, u, &c[u]);
            gen = now;
        }
        s_tex_dec_gen = s_texgen_on ? gen : 0;
        serial = s_draw_serial;
    }
    return c;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    const eng_vp_memo* vp = NULL;
    g.vpc_full = 1;
    if (eng_guest_programs(&vp, NULL)) {
        mask = vp->input_mask;
        g.vpc_full = vp->vpc_full;
        g.vpc_lo = vp->vpc_lo;
        g.vpc_n = vp->vpc_n;
    }
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    Out.t8 = (float4)0; Out.t9 = (float4)0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* User clip planes. NV4097_SET_USER_CLIP_PLANE_CONTROL (0x1478) holds one
 * 4-bit op per plane (bits 0..23, planes 0..5): 0 disabled, 1 ENABLE_LT, 2
 * ENABLE_GE. The vertex program writes the six distances into o[5].yzw and
 * o[6].yzw (o[5].x is fog, o[6].x the point size). As RPCS3 does
 * (fill_user_clip_data + the VP reg_table): GE keeps d >= 0 (factor +1), LT
 * keeps d <= 0 (factor -1), a disabled plane gets 0.5 so it never clips.
 * inFAMOUS 2's planar water reflection (256x256 at 0x0F6F0000) clips the
 * mirrored scene at the water plane this way; without it the riverbed and
 * everything under the surface was drawn into the reflection.
 * Returns the op per plane, two bits each (0 = no plane enabled), the form
 * rsx_vp_apply_user_clip takes. PS3RECOMP_ENG_USER_CLIP=0 ignores the
 * register (the old behaviour). */
static u32 eng_user_clip_key(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_USER_CLIP"); on = e ? atoi(e) : 1; }
    if (!on) return 0;
    const u32 ctl = rsx_dsp_reg(&g.rsx, 0x1478u);
    u32 key = 0;
    for (u32 p = 0; p < 6; p++) {
        const u32 op = (ctl >> (p * 4u)) & 0xFu;
        if (op == 1u || op == 2u) key |= op << (p * 2u);
    }
    return key;
}

/* Drawstats split of eng_pipeline_get (defined with the rest of drawstats
 * below): programs + FP constants, key hashing, table scan. */
static u64 ds_pk_ns[3], ds_pk_scanned;
static int eng_ds_enabled(void);
static u64 eng_ds_now(void);

/* Open-addressed index over g.pipelines by key (value = entry index + 1).
 * Entries are only ever appended, so it catches up with the table when it is
 * next used; a table that shrank (shutdown, re-init) is indexed again. */
#define ENG_PIDX_SIZE (ENG_MAX_PIPELINES * 2u)
static u32 s_pidx[ENG_PIDX_SIZE];
static u32 s_pidx_n;
static int s_pidx_on = -1;
static void eng_pidx_set(int id, int v) { (void)id; __atomic_store_n(&s_pidx_on, v != 0, __ATOMIC_RELAXED); }
static inline u32 eng_pidx_slot(u64 key)
{
    key ^= key >> 33; key *= 0xFF51AFD7ED558CCDull; key ^= key >> 33;
    return (u32)key & (ENG_PIDX_SIZE - 1u);
}
static void eng_pidx_sync(void)
{
    if (s_pidx_n == g.n_pipelines) return;
    if (s_pidx_n > g.n_pipelines) { memset(s_pidx, 0, sizeof s_pidx); s_pidx_n = 0; }
    for (; s_pidx_n < g.n_pipelines; s_pidx_n++) {
        u32 p = eng_pidx_slot(g.pipelines[s_pidx_n].key);
        while (s_pidx[p]) p = (p + 1) & (ENG_PIDX_SIZE - 1u);
        s_pidx[p] = s_pidx_n + 1;
    }
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. */
static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int* out_fixed)
{
    *out_fixed = 1;
    const eng_vp_memo* vp = NULL;
    const eng_fp_memo* fp = NULL;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const u64 pk0 = eng_ds_enabled() ? eng_ds_now() : 0;
    const int fixed = !eng_guest_programs(&vp, &fp);
    const u8* vp_uc = fixed ? NULL : (const u8*)(g.rsx.vp + vp->start * 4);
    const u8* fp_uc = fixed ? NULL : eng_guest_ptr(NULL, fp->loc, fp->off, fp->size);
    const u32 vp_instrs = fixed ? 0 : vp->instrs, fp_size = fixed ? 0 : fp->size;

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    /* Integer framebuffer sRGB output (upstream PR #304, ported): a nonzero
     * SET_SHADER_PACKER asks the ROP for linear-to-sRGB on 8-bit colour
     * targets written from the half register file. */
    const u32 srgb_output = !fixed && rsx_fp_srgb_output_enabled(
        rsx_dsp_reg(&g.rsx, 0x1FECu), fp_ctrl, rs->alpha_ref_format & 0x1Fu);
    { static int said = 0;
      if (srgb_output && !said) { said = 1;
          fprintf(stderr, "[rsx engine] sRGB framebuffer output in use (packer=0x%X, fmt=0x%X)\n",
                  rsx_dsp_reg(&g.rsx, 0x1FECu), rs->alpha_ref_format & 0x1Fu); } }
    /* Every unit decoded once for the masks below (they took five decodes
     * of all sixteen units and three microcode walks per draw). */
    const rsx_dsp_texture* tex = eng_draw_textures();
    const u32 vtex_mask = eng_vtex_mask();
    /* (rsx3) PS3RECOMP_ENG_TEXGEN: everything below is a function of the
     * decoded units, the fragment program's analyses and the shadow switches
     * alone -- reused while none of them changed (no texture register write
     * since the decode, same program memo, same switch epoch). ~3% of the RSX
     * thread a draw in the city. */
    static struct {
        int valid; u32 gen, epoch; const eng_fp_memo* fp; u64 fps; u32 gc, ec, su;
        u32 cube, vol, gamma_units, expand_units, shadow_mask;
        u8 gm[RSX_DSP_NUM_TEXTURES], em[RSX_DSP_NUM_TEXTURES];
        rsx_fp_shadow_desc sh[RSX_DSP_NUM_TEXTURES];
    } dm;
    { static int watched; if (!watched) { watched = 1; ps3_toggle_watch(PS3T_ENG_SHADOW_CMP, eng_shadow_cmp_set);
                                          ps3_toggle_watch(PS3T_ENG_SHADOW_D24, eng_shadow_d24_set); } }
    const int dm_hit = s_texgen_on > 0 && s_tex_dec_gen && dm.valid && dm.gen == s_tex_dec_gen &&
                       dm.epoch == __atomic_load_n(&s_shadow_epoch, __ATOMIC_RELAXED) && dm.fp == fp &&
                       (fixed || (dm.fps == fp->structural && dm.gc == fp->gamma_cand &&
                                  dm.ec == fp->expand_cand && dm.su == fp->sampled_units));
    u32 cube_mask, vol_mask, gamma_units = 0, expand_units = 0, shadow_mask = 0;
    u8 gamma_masks[RSX_DSP_NUM_TEXTURES] = {0};
    u8 expand_masks[RSX_DSP_NUM_TEXTURES] = {0};
    rsx_fp_shadow_desc shadow[RSX_DSP_NUM_TEXTURES] = {{0}};
    if (dm_hit) {
        cube_mask = dm.cube; vol_mask = dm.vol;
        gamma_units = dm.gamma_units; expand_units = dm.expand_units; shadow_mask = dm.shadow_mask;
        memcpy(gamma_masks, dm.gm, sizeof gamma_masks);
        memcpy(expand_masks, dm.em, sizeof expand_masks);
        memcpy(shadow, dm.sh, sizeof shadow);
    } else {
    const u32 dm_epoch = __atomic_load_n(&s_shadow_epoch, __ATOMIC_RELAXED);
    cube_mask = eng_cube_mask(tex);
    vol_mask = eng_volume_mask(tex);
    /* Texture input gamma (upstream PR #305, ported): TEXTURE_ADDRESS bits
     * 20..23 ask for sRGB-to-linear on the sampled lanes. Only units the
     * program actually fetches through count, so an idle unit's state does
     * not fork the pipeline. */
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        const rsx_dsp_texture* t = &tex[u];
        if (t->enabled && (fp->gamma_cand & (1u << u)))
            gamma_masks[u] = rsx_texture_gamma_mask(t->format, t->remap, t->filter, t->wrap);
        if (gamma_masks[u]) gamma_units |= 1u << u;
    }
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(gamma_units & (1u << u))) gamma_masks[u] = 0;
    /* Texture sign expansion: SET_TEXTURE_ADDRESS's UNSIGNED_REMAP_BIASED
     * asks that every fetch come back re-centred to -1..1 (EXP_TEX fetches
     * as x*2-1). inFAMOUS 2's deferred composite reads its normals G-buffer
     * that way. Same rule as gamma: only units the program samples count. */
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        const rsx_dsp_texture* t = &tex[u];
        if (t->enabled && (fp->expand_cand & (1u << u)))
            expand_masks[u] = rsx_texture_expand_mask(t->format, t->remap, t->filter, t->wrap);
        if (expand_masks[u]) expand_units |= 1u << u;
    }
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(expand_units & (1u << u))) expand_masks[u] = 0;
    /* Depth textures with comparison enabled return visibility, not raw
     * depth (upstream PR #302, adapted: this engine has no UN-unit scaling). */
    const u32 sampled_units = fixed ? 0 : fp->sampled_units;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        if (!(sampled_units & (1u << u))) continue;
        const rsx_dsp_texture* t = &tex[u];
        rsx_fp_shadow_desc_from_texture(&shadow[u], t->format, t->dimension,
            t->cubemap, t->mipmaps, t->filter, t->wrap, t->control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    if (s_texgen_on > 0 && s_tex_dec_gen) {
        dm.valid = 1; dm.gen = s_tex_dec_gen; dm.epoch = dm_epoch; dm.fp = fp;
        dm.fps = fixed ? 0 : fp->structural; dm.gc = fixed ? 0 : fp->gamma_cand;
        dm.ec = fixed ? 0 : fp->expand_cand; dm.su = fixed ? 0 : fp->sampled_units;
        dm.cube = cube_mask; dm.vol = vol_mask; dm.gamma_units = gamma_units;
        dm.expand_units = expand_units; dm.shadow_mask = shadow_mask;
        memcpy(dm.gm, gamma_masks, sizeof dm.gm); memcpy(dm.em, expand_masks, sizeof dm.em);
        memcpy(dm.sh, shadow, sizeof dm.sh);
    }
    }
    { static int said = 0;
      if (expand_units && !said) { said = 1;
          fprintf(stderr, "[rsx engine] texture sign expansion in use (units 0x%X)\n", expand_units); } }
    { static int said = 0;
      if (gamma_units && !said) { said = 1;
          fprintf(stderr, "[rsx engine] texture gamma decode in use (units 0x%X)\n", gamma_units); } }

    static const rsx_fp_constant_block no_constants;
    g.fp_constants = fixed ? &no_constants : &fp->constants;
    if (!fixed && (!fp->constants_ok || !fp_uc)) return 0;

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    const u64 pk1 = pk0 ? eng_ds_now() : 0;
    /* Consecutive draws nearly always repeat every input of the key: reuse
     * the last key when they all compare equal. The render state is zeroed
     * before it is decoded (rsx_draw_engine_decode_render_state), so a byte
     * compare of it is a field compare. The hash over the render state was
     * 2.7% of the RSX thread per draw (T-0016). */
    static struct {
        int valid, fixed;
        u64 vp_hash, fp_struct;
        u32 fp_ctrl_key, cube_mask, vol_mask, vtex_mask, layout_mask, layout_stride, rt_count;
        u32 shadow_mask, srgb_output, gamma_units, expand_units, ucp, dbz, shade_bf, fx_off;
        u32 fbf_cand; u8 fbf_att[16]; u8 fbf_sel[16][4];
        u8 gamma_masks[RSX_DSP_NUM_TEXTURES];
        u8 expand_masks[RSX_DSP_NUM_TEXTURES];
        rsx_fp_shadow_desc shadow[RSX_DSP_NUM_TEXTURES];
        rsx_be_format rt_fmt;
        rsx_be_render_state rs;
        u64 key;
    } kmemo;
    /* SHADER_CONTROL bits 1..3: the program replaces the fragment depth with
     * r1.z (CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT). inFAMOUS 2 downsamples its
     * depth buffer that way (a full-screen pass with the colour mask off,
     * title screen p104, SHADER_CONTROL 0x2500844E, into the 512x288 zeta
     * at 0x0F770000) for the soft particles to test and fade against
     * (FP 169a61d2fe6f3f76 reads it back as A8R8G8B8); without the export
     * that buffer held the full-screen triangle's own z instead of the
     * scene's depth.
     * PS3RECOMP_ENG_DEPTH_EXPORT=0 ignores the bits (the old behaviour). */
    static int depth_export_on = -1;
    if (depth_export_on < 0) { const char* e = getenv("PS3RECOMP_ENG_DEPTH_EXPORT"); depth_export_on = e ? atoi(e) : 1; }
    const u32 fp_ctrl_key = fp_ctrl & (depth_export_on ? (0x40u | RSX_FP_CTRL_DEPTH_EXPORT) : 0x40u);
    const u64 in_vp = fixed ? 0 : vp->hash, in_fp = fixed ? 0 : fp->structural;
    if (!fixed && !in_fp) return 0;
    const u32 ucp = fixed ? 0 : eng_user_clip_key();
    const u32 dbz = fixed ? 0 : s_dbz.unit;   /* depth-bounds emulation: the copy's unit + 1 */
    /* PS3RECOMP_ENG_SHADE_BACKFACE: the composite's back-face fix is part of
     * that program's identity, so a live change builds the other variant. */
    if (__builtin_expect(__atomic_load_n(&s_shade_backface, __ATOMIC_RELAXED) < 0, 0)) {
        const char* e = getenv("PS3RECOMP_ENG_SHADE_BACKFACE");
        __atomic_store_n(&s_shade_backface, !(e && *e == '0'), __ATOMIC_RELAXED);
        ps3_toggle_watch(PS3T_ENG_SHADE_BACKFACE, eng_shade_backface_set);
    }
    const u32 shade_bf = (!fixed && in_fp == ENG_SHADE_BACKFACE_FP)
                       ? (u32)__atomic_load_n(&s_shade_backface, __ATOMIC_RELAXED) : 0u;
    /* PS3RECOMP_DOF / PS3RECOMP_EDGE_BLUR: which terms of the game's
     * DOF/edge-blur pass are off (bit 0 depth of field, bit 1 edge blur). */
    if (__builtin_expect(__atomic_load_n(&s_fx_dof, __ATOMIC_RELAXED) < 0, 0)) {
        __atomic_store_n(&s_fx_edge, eng_fx_on(getenv("PS3RECOMP_EDGE_BLUR")), __ATOMIC_RELAXED);
        __atomic_store_n(&s_fx_dof, eng_fx_on(getenv("PS3RECOMP_DOF")), __ATOMIC_RELAXED);
    }
    const u32 fx_off = (!fixed && in_fp == ENG_DOF_BLEND_FP)
                     ? (u32)(!__atomic_load_n(&s_fx_dof, __ATOMIC_RELAXED)) |
                       (u32)(!__atomic_load_n(&s_fx_edge, __ATOMIC_RELAXED)) << 1 : 0u;
    u64 key;
    if (kmemo.valid && kmemo.fixed == fixed && kmemo.vp_hash == in_vp && kmemo.fp_struct == in_fp &&
        kmemo.fp_ctrl_key == fp_ctrl_key && kmemo.cube_mask == cube_mask && kmemo.vol_mask == vol_mask &&
        kmemo.vtex_mask == vtex_mask && kmemo.layout_mask == layout->mask &&
        kmemo.layout_stride == layout->stride && kmemo.rt_count == rt_count &&
        kmemo.rt_fmt == rt_fmt && !memcmp(&kmemo.rs, rs, sizeof *rs) &&
        kmemo.shadow_mask == shadow_mask && kmemo.srgb_output == srgb_output &&
        kmemo.gamma_units == gamma_units && kmemo.ucp == ucp && kmemo.dbz == dbz &&
        kmemo.shade_bf == shade_bf && kmemo.fx_off == fx_off &&
        (!gamma_units || !memcmp(kmemo.gamma_masks, gamma_masks, sizeof gamma_masks)) &&
        kmemo.expand_units == expand_units &&
        (!expand_units || !memcmp(kmemo.expand_masks, expand_masks, sizeof expand_masks)) &&
        (!shadow_mask || !memcmp(kmemo.shadow, shadow, sizeof shadow)) &&
        kmemo.fbf_cand == s_fbf.cand &&
        (!s_fbf.cand || (!memcmp(kmemo.fbf_att, s_fbf.att, sizeof s_fbf.att) &&
                         !memcmp(kmemo.fbf_sel, s_fbf.sel, sizeof s_fbf.sel)))) {
        key = kmemo.key;
        goto key_done;
    }
    key = 1469598103934665603ull;
    if (fixed) {
        static const u32 fixed_tag = 0x4E464958u;   /* "XIFN" */
        key = eng_fnv1a(&fixed_tag, sizeof fixed_tag, key);
    } else {
        key = eng_fnv1a(&vp->hash, sizeof vp->hash, key);
        key = eng_fnv1a(&fp->structural, sizeof fp->structural, key);
        key = eng_fnv1a(&fp_ctrl_key, sizeof fp_ctrl_key, key);
        if (srgb_output) key = eng_fnv1a(&srgb_output, sizeof srgb_output, key);
        if (gamma_units) key = eng_fnv1a(gamma_masks, sizeof gamma_masks, key);
        if (expand_units) key = eng_fnv1a(expand_masks, sizeof expand_masks, key);
        key = eng_fnv1a(&cube_mask, sizeof cube_mask, key);
        if (vol_mask) key = eng_fnv1a(&vol_mask, sizeof vol_mask, key);
        if (shadow_mask) {
            key = eng_fnv1a(&shadow_mask, sizeof shadow_mask, key);
            key = eng_fnv1a(shadow, sizeof shadow, key);
        }
        key = eng_fnv1a(&vtex_mask, sizeof vtex_mask, key);
        if (ucp) key = eng_fnv1a(&ucp, sizeof ucp, key);
        if (s_fbf.cand) {   /* PS3RECOMP_ENG_FBFETCH */
            key = eng_fnv1a(&s_fbf.cand, sizeof s_fbf.cand, key);
            key = eng_fnv1a(s_fbf.att, sizeof s_fbf.att, key);
            key = eng_fnv1a(s_fbf.sel, sizeof s_fbf.sel, key);
        }
        if (dbz) { const u32 tag = 0x5A424400u | dbz; key = eng_fnv1a(&tag, sizeof tag, key); }
        if (shade_bf) { const u32 tag = 0x53424631u; key = eng_fnv1a(&tag, sizeof tag, key); }   /* "SBF1" */
        if (fx_off) { const u32 tag = 0x46584F00u | fx_off; key = eng_fnv1a(&tag, sizeof tag, key); }   /* "FXO" */
    }
    key = eng_fnv1a(&layout->mask, sizeof layout->mask, key);
    key = eng_fnv1a(&layout->stride, sizeof layout->stride, key);
    key = eng_fnv1a(&rt_fmt, sizeof rt_fmt, key);
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    key = eng_fnv1a(&rt_count, sizeof rt_count, key);
    key = rsx_draw_engine_hash_render_state(rs, key);
    kmemo.valid = 1; kmemo.fixed = fixed; kmemo.vp_hash = in_vp; kmemo.fp_struct = in_fp;
    kmemo.fp_ctrl_key = fp_ctrl_key; kmemo.cube_mask = cube_mask; kmemo.vol_mask = vol_mask; kmemo.vtex_mask = vtex_mask;
    kmemo.layout_mask = layout->mask; kmemo.layout_stride = layout->stride;
    kmemo.rt_count = rt_count; kmemo.rt_fmt = rt_fmt; memcpy(&kmemo.rs, rs, sizeof *rs);
    kmemo.shadow_mask = shadow_mask; memcpy(kmemo.shadow, shadow, sizeof shadow);
    kmemo.srgb_output = srgb_output;
    kmemo.gamma_units = gamma_units; kmemo.expand_units = expand_units; kmemo.ucp = ucp; kmemo.dbz = dbz;
    kmemo.shade_bf = shade_bf; kmemo.fx_off = fx_off;
    memcpy(kmemo.expand_masks, expand_masks, sizeof expand_masks); memcpy(kmemo.gamma_masks, gamma_masks, sizeof gamma_masks);
    kmemo.fbf_cand = s_fbf.cand; memcpy(kmemo.fbf_att, s_fbf.att, sizeof s_fbf.att); memcpy(kmemo.fbf_sel, s_fbf.sel, sizeof s_fbf.sel);
    kmemo.key = key;
key_done:;

    const u64 pk2 = pk0 ? eng_ds_now() : 0;
    /* Consecutive draws mostly share a pipeline: try the last hit first. */
    static u32 last_hit;
    /* PS3RECOMP_ENG_PIPE_INDEX (live): a miss on last_hit used to scan the
     * whole table, and New Marais builds ~1000 pipelines; the open-addressed
     * index finds the entry by key instead. 0 = the scan. */
    if (__builtin_expect(s_pidx_on < 0, 0)) ps3_toggle_watch(PS3T_ENG_PIPE_INDEX, eng_pidx_set);
    if (s_pidx_on) {
        eng_pidx_sync();
        u32 i = ENG_INVALID;
        if (last_hit < g.n_pipelines && g.pipelines[last_hit].key == key) i = last_hit;
        else for (u32 p = eng_pidx_slot(key); s_pidx[p]; p = (p + 1) & (ENG_PIDX_SIZE - 1u))
            if (g.pipelines[s_pidx[p] - 1].key == key) { i = s_pidx[p] - 1; break; }
        if (i != ENG_INVALID) {
            last_hit = i;
            *out_fixed = g.pipelines[i].fixed;
            s_fbf.applied = g.pipelines[i].fbf;
            if (pk0) {
                const u64 pk3 = eng_ds_now();
                ds_pk_ns[0] += pk1 - pk0; ds_pk_ns[1] += pk2 - pk1; ds_pk_ns[2] += pk3 - pk2;
                ds_pk_scanned += 1;
            }
            return g.pipelines[i].handle;
        }
    } else
    for (u32 n = 0; n <= g.n_pipelines; n++) {   /* n = 0 is last_hit, then 0.. */
        const u32 i = n ? n - 1u : last_hit;
        if (n && i == last_hit) continue;
        if (i < g.n_pipelines && g.pipelines[i].key == key) {
            last_hit = i;
            *out_fixed = g.pipelines[i].fixed;
            s_fbf.applied = g.pipelines[i].fbf;
            if (pk0) {
                const u64 pk3 = eng_ds_now();
                ds_pk_ns[0] += pk1 - pk0; ds_pk_ns[1] += pk2 - pk1; ds_pk_ns[2] += pk3 - pk2;
                ds_pk_scanned += n + 1;
            }
            return g.pipelines[i].handle;
        }
    }
    if (g.n_pipelines >= ENG_MAX_PIPELINES) return 0;

    const uint64_t ft_c0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        vi = rsx_vp_decompile_compact_ex(vp_uc, vp_instrs * 16u, vtex_mask,
                                         layout->mask, s_vs_hlsl,
                                         sizeof s_vs_hlsl);
        if (vi > 0 && ucp) {
            if (rsx_vp_apply_user_clip(s_vs_hlsl, sizeof s_vs_hlsl, ucp) < 0) vi = -1;
            static u32 said;
            if (said++ < 8)
                fprintf(stderr, "[rsx engine] user clip planes 0x%06X (ops 0x%03X) in a vertex program%s\n",
                        rsx_dsp_reg(&g.rsx, 0x1478u), ucp, vi > 0 ? "" : ": NOT applied (unexpected text)");
        }
        fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp_uc, fp_size, fp_ctrl, cube_mask, vol_mask,
                shadow_mask ? shadow : NULL, gamma_units ? gamma_masks : NULL,
                expand_units ? expand_masks : NULL,
                s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        if (fi > 0 && nconst != g.fp_constants->count) fi = -1;
        if (fi > 0 && srgb_output &&
            rsx_fp_apply_srgb_output(s_ps_hlsl, sizeof s_ps_hlsl) < 0)
            fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
        if (fi > 0 && (fp_ctrl_key & RSX_FP_CTRL_DEPTH_EXPORT)) {
            if (rsx_fp_apply_depth_export(s_ps_hlsl, sizeof s_ps_hlsl) < 0) fi = -1;
            static u32 said;
            if (said++ < 8)
                fprintf(stderr, "[rsx engine] depth export (shader control 0x%08X) in a fragment program%s\n",
                        fp_ctrl, fi > 0 ? "" : ": NOT applied (unexpected text)");
        }
        if (fi > 0 && dbz && rsx_fp_apply_depth_bounds(s_ps_hlsl, sizeof s_ps_hlsl, dbz - 1u) < 0) {
            static u32 said;   /* drawn unbounded rather than dropped */
            if (said++ < 8)
                fprintf(stderr, "[rsx engine] depth bounds NOT applied to fp %016llx (unexpected text)\n",
                        (unsigned long long)in_fp);
        }
    }
    /* PS3RECOMP_ENG_FP_PATCH=<fp hash>:<from>:<to>[;<fp hash>:<from>:<to>...]:
     * DIAGNOSTIC. Replace every <from> with <to> in that fragment program's
     * generated HLSL before it is compiled -- a hypothesis about one program's
     * math tested in a run, without a rebuild. */
    if (fi > 0 && !fixed) { const char* e = getenv("PS3RECOMP_ENG_FP_PATCH");
        while (e && *e) {
            char* d; const u64 h = strtoull(e, &d, 16);
            if (*d != ':') break;
            const char* from = d + 1; const char* c1 = strchr(from, ':'); if (!c1) break;
            const char* to = c1 + 1; const char* c2 = strchr(to, ';');
            const size_t fl = (size_t)(c1 - from), tl = c2 ? (size_t)(c2 - to) : strlen(to);
            if (h == in_fp && fl) {
                static char tmp[sizeof s_ps_hlsl]; size_t o = 0; u32 hits = 0;
                for (const char* q = s_ps_hlsl; *q && o + tl + 1 < sizeof tmp; ) {
                    if (!strncmp(q, from, fl)) { memcpy(tmp + o, to, tl); o += tl; q += fl; hits++; }
                    else tmp[o++] = *q++;
                }
                tmp[o] = 0; memcpy(s_ps_hlsl, tmp, o + 1);
                fprintf(stderr, "[rsx engine] FP_PATCH %016llx: %u replacement(s)\n", (unsigned long long)h, hits);
            }
            e = c2 ? c2 + 1 : NULL;
        } }
    /* PS3RECOMP_ENG_SHADE_BACKFACE: right after the composite reads the sun
     * mask (unit 2's R into h4), a receiver without stencil bit 7 (cc0.w, the
     * program's own flag) whose N.L (h5.z, normal . sun) is negative counts as
     * shadowed. Sampled after h5.z and cc0.w are set, before h4.x is used. */
    if (fi > 0 && shade_bf) {
        static const char anchor[] = "h[4].xyzw = _v.xyzw; }";
        static const char fix[] = "\n    /* PS3RECOMP_ENG_SHADE_BACKFACE */ if (cc0.w == 0.0 && h[5].z < 0.0) h[4].x = 0.0;";
        char* at = strstr(s_ps_hlsl, "rsx_tex2.Sample(");
        at = at ? strstr(at, anchor) : NULL;
        const size_t len = strlen(s_ps_hlsl);
        if (at && len + sizeof fix < sizeof s_ps_hlsl && strstr(s_ps_hlsl, "cc0.w = _v.w;") && strstr(s_ps_hlsl, "h[5].z = _v.z;")) {
            at += sizeof anchor - 1;
            memmove(at + sizeof fix - 1, at, len + 1 - (size_t)(at - s_ps_hlsl));
            memcpy(at, fix, sizeof fix - 1);
            static u32 said; if (said++ < 2) fprintf(stderr, "[rsx engine] shade-backface: composite %016llx patched\n", (unsigned long long)in_fp);
        } else { static u32 said; if (said++ < 2) fprintf(stderr, "[rsx engine] shade-backface: composite %016llx NOT patched (unexpected text)\n", (unsigned long long)in_fp); }
    }
    /* PS3RECOMP_DOF=0 / PS3RECOMP_EDGE_BLUR=0: the DOF/edge-blur pass with
     * those weights at 0 (see ENG_DOF_BLEND_FP). Each term must appear
     * exactly once in the text the decompiler made, else it is left alone. */
    if (fi > 0 && fx_off) {
        static char tmp[sizeof s_ps_hlsl];
        int ok_dof = 1, ok_edge = 1;
        memcpy(tmp, s_ps_hlsl, strlen(s_ps_hlsl) + 1);
        if (fx_off & 1u)
            ok_dof = eng_hlsl_replace(tmp, sizeof tmp, "((fp_constants[4]).xxxx)", "((float4)0)") == 1 &&
                     eng_hlsl_replace(tmp, sizeof tmp, "((fp_constants[5]).xxxx)", "((float4)0)") == 1;
        if (ok_dof && (fx_off & 2u))
            ok_edge = eng_hlsl_replace(tmp, sizeof tmp, "((input.tc0).zzzz)", "((float4)0)") == 1;
        if (ok_dof && ok_edge) memcpy(s_ps_hlsl, tmp, strlen(tmp) + 1);
        static u32 said;
        if (said++ < 8)
            fprintf(stderr, "[rsx engine] game post fx: depth of field %s, edge blur %s (fp %016llx)%s\n",
                    (fx_off & 1u) ? "OFF" : "on", (fx_off & 2u) ? "OFF" : "on", (unsigned long long)in_fp,
                    ok_dof && ok_edge ? "" : ": NOT applied (unexpected text)");
    }
    /* PS3RECOMP_ENG_FBFETCH: the units that read their own target. */
    u32 fbf_applied = 0;
    static char fbf_orig[sizeof s_ps_hlsl];
    if (fi > 0 && !fixed && s_fbf.cand) {
        memcpy(fbf_orig, s_ps_hlsl, strlen(s_ps_hlsl) + 1);
        if (rsx_fp_apply_fbfetch(s_ps_hlsl, sizeof s_ps_hlsl, s_fbf.cand, s_fbf.att, s_fbf.sel, &fbf_applied) < 0)
            fbf_applied = 0;
    }
    if (vi > 0 && fi > 0)
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    if (!handle && fbf_applied && vi > 0 && fi > 0) {
        /* The framebuffer-fetch variant did not build: the copies instead. */
        static u32 said;
        if (said++ < 4) fprintf(stderr, "[rsx engine] framebuffer fetch pipeline failed (fp %016llx); copies instead\n",
                                (unsigned long long)in_fp);
        memcpy(s_ps_hlsl, fbf_orig, strlen(fbf_orig) + 1);
        fbf_applied = 0;
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    }
    s_fbf.applied = fbf_applied;
    if (fbf_applied) { static u32 said;
        if (said++ < 8) fprintf(stderr, "[rsx engine] framebuffer fetch: fp %016llx units 0x%X read their own targets\n",
                                (unsigned long long)in_fp, fbf_applied); }
    if (ft_c0) ps3_ft_span(PS3_FT_COMPILE, 0, handle != 0, 0, ft_c0, ps3_ft_now());
    /* PS3RECOMP_ENG_SHADER_DUMP=<dir>: each new pipeline's shaders, named by
     * the backend handle the Metal pass log prints as p=. */
    if (handle) { static const char* dir = (const char*)-1;
        if (dir == (const char*)-1) dir = getenv("PS3RECOMP_ENG_SHADER_DUMP");
        if (dir) { char path[512];
            snprintf(path, sizeof path, "%s/p%u_vs.hlsl", dir, handle);
            FILE* f = fopen(path, "w"); if (f) { fputs(s_vs_hlsl, f); fclose(f); }
            snprintf(path, sizeof path, "%s/p%u_ps.hlsl", dir, handle);
            f = fopen(path, "w"); if (f) { fputs(s_ps_hlsl, f); fclose(f); }
            /* manifest.txt: p= to the programs' hashes, so a pass found in one
             * run can be skipped by PS3RECOMP_ENG_SKIP_FP in the next. */
            snprintf(path, sizeof path, "%s/manifest.txt", dir);
            f = fopen(path, "a");
            if (f) { fprintf(f, "p=%u fp=%016llx vp=%016llx targets=%u %s\n", handle,
                             (unsigned long long)in_fp, (unsigned long long)in_vp, rt_count,
                             fixed ? "built-in" : "guest"); fclose(f); } } }
    /* PS3RECOMP_ENG_POLY_OFFSET_LOG=1: which programs compare against a
     * depth map, through which unit, operator (helper order) and map size. */
    if (shadow_mask) { static int on = -1; static u32 n;
        if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_POLY_OFFSET_LOG"); on = e && *e == '1'; }
        if (on && n++ < 256)
            for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) if (shadow_mask & (1u << u))
                fprintf(stderr, "[rsx engine] shadow fp=%016llx p=%u unit %u func %u linear %u grid %u mode %u %ux%u\n",
                        (unsigned long long)in_fp, handle, u, shadow[u].func, shadow[u].linear,
                        shadow[u].grid, shadow[u].mode,
                        tex[u].width, tex[u].height); }
    { static u32 logs = 0; if (__atomic_fetch_add(&logs, 1, __ATOMIC_RELAXED) < 32)
        fprintf(stderr, "[rsx engine] pipeline %016llx: %s vp %d, fp %d,"
                        " %u constants -> %s\n",
                (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, fi, nconst, handle ? "ok" : "FAILED (draw dropped)"); }

    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    g.pipelines[g.n_pipelines].fbf = fbf_applied;
    g.n_pipelines++;
    *out_fixed = fixed;
    return handle;
}

/* ---- index decoding --------------------------------------------------------- */

/* A mapped run of big-endian indices into dc.refs (room already reserved),
 * restart indices becoming cuts at the reference position they fall on.
 *
 * Out of line and specialised per width and restart mode: inlined into
 * sink_end, the loop's base index, width and shift lived on the stack, the
 * width was tested per index and each index was assembled byte by byte --
 * 11.6% of the RSX thread in inFamous, for reading indices (T-0016). */
#define DC_DECODE(T, BSWAP, RESTART)                                              \
    for (u32 i = 0; i < count; i++) {                                             \
        T raw; memcpy(&raw, run + (size_t)i * sizeof(T), sizeof(T));             \
        const u32 index = (u32)BSWAP(raw);                                        \
        if (RESTART && index == restart_val) {                                    \
            if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) {   \
                dc.n_refs = n; return 0;                                          \
            }                                                                     \
            continue;                                                             \
        }                                                                         \
        out[n].vertex_id = index;                                                 \
        out[n].base_index = base_index;                                           \
        n++;                                                                      \
    }
static __attribute__((noinline)) int dc_decode_indices(const u8* run, u32 count, int is_u32,
                                                       int restart_en, u32 restart_val,
                                                       u32 base_index)
{
    rsx_vertex_ref* const out = dc.refs;
    u32 n = dc.n_refs;
    if (is_u32) {
        if (restart_en) { DC_DECODE(u32, __builtin_bswap32, 1) }
        else            { DC_DECODE(u32, __builtin_bswap32, 0) }
    } else {
        if (restart_en) { DC_DECODE(u16, __builtin_bswap16, 1) }
        else            { DC_DECODE(u16, __builtin_bswap16, 0) }
    }
    dc.n_refs = n;
    return 1;
}
#undef DC_DECODE

/* ---- draw accumulation --------------------------------------------------- */

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_remapped = 0;
    dc.vbuf = 0;
    dc.vbuf_hit = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL;
    dc.inl_bytes = 0;
}

static int dc_reserve_refs(u64 need)
{
    if (need <= dc.cap_refs) return 1;
    if (need > 0xFFFFFFFFull) return 0;
    u64 next = dc.cap_refs ? dc.cap_refs : 4096u;
    while (next < need) next *= 2u;
    if (next > 0xFFFFFFFFull) next = need;
    rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                 (size_t)next * sizeof(*r));
    if (!r) return 0;
    dc.refs = r;
    dc.cap_refs = (u32)next;
    return 1;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs && !dc_reserve_refs((u64)dc.n_refs + 1u))
        return 0;
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* PS3RECOMP_ENG_DRAWSTATS=1: per world frame (every 30th), what the draw
 * path does and where its time goes -- the numbers T-0016's vertex work is
 * prioritised by. Draws and references, how much the dedup collapses, the
 * attribute formats actually fetched (weighted by unique vertices), the bytes
 * handed to the backend, how many draws reused the previous draw's vertex
 * constants unchanged, and wall time per stage of sink_end. */
typedef struct {
    u64 draws, refs, uniq, slots, vbytes, ibytes, cbytes, cb_same, cb_range;
    u64 fmt[8][5];                       /* [type][size], unique-vertex weighted */
    u64 ns_refs, ns_remap, ns_plan, ns_fetch, ns_rest;
    u64 ns_layout, ns_tex, ns_pipe, ns_bind, ns_draw;   /* parts of the above + layout */
    u64 span_draws, span_sum, span_uniq, span_refs, span_dense;   /* index-range density */
} eng_drawstats;
static eng_drawstats ds;
static int ds_on = -1;
static int eng_ds_enabled(void)
{
    if (ds_on < 0) ds_on = getenv("PS3RECOMP_ENG_DRAWSTATS") ? 1 : 0;
    return ds_on;
}
static u64 eng_ds_now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static void eng_ds_frame_end(u32 frame_draws)
{
    static u32 wf;
    if (!eng_ds_enabled()) return;
    if (frame_draws <= 1000) { memset(&ds, 0, sizeof ds); memset(ds_pk_ns, 0, sizeof ds_pk_ns); ds_pk_scanned = 0; memset(ds_memo_hit, 0, sizeof ds_memo_hit); memset(ds_memo_miss, 0, sizeof ds_memo_miss); ds_th_ns = ds_th_bytes = ds_th_count = ds_th_changed = 0; ds_th_wt_clean = ds_vc_wt_clean = ds_ic_wt_clean = 0; ds_vc_hit = ds_vc_miss = ds_vc_skip = ds_vc_new = ds_vc_hashed = ds_vc_ns = 0; ds_ic_hit = ds_ic_miss = ds_ic_new = ds_ic_hashed = 0; return; }
    if (wf++ % 30 == 0 && ds.draws) {
        const double us = 1e-3;
        fprintf(stderr,
                "[drawstats] draws=%llu refs=%llu unique=%llu (%.0f%%) attrs/vtx=%.2f"
                " staged: vtx %.1f MB idx %.1f MB vs-const %.1f MB (ranged %llu); vs-const unchanged %llu/%llu\n",
                (unsigned long long)ds.draws, (unsigned long long)ds.refs,
                (unsigned long long)ds.uniq, ds.refs ? 100.0 * ds.uniq / ds.refs : 0.0,
                ds.uniq ? (double)ds.slots / ds.uniq : 0.0,
                ds.vbytes / 1e6, ds.ibytes / 1e6, ds.cbytes / 1e6,
                (unsigned long long)ds.cb_range, (unsigned long long)ds.cb_same, (unsigned long long)ds.draws);
        fprintf(stderr,
                "[drawstats] time/frame: refs %.0f us, remap %.0f us, plan %.0f us,"
                " fetch+decode %.0f us, rest of sink_end %.0f us\n",
                ds.ns_refs * us, ds.ns_remap * us, ds.ns_plan * us,
                ds.ns_fetch * us, ds.ns_rest * us);
        fprintf(stderr,
                "[drawstats] time/frame: layout+analysis %.0f us; in rest: textures %.0f us,"
                " pipeline %.0f us, constants+binds %.0f us, backend draw %.0f us\n",
                ds.ns_layout * us, ds.ns_tex * us, ds.ns_pipe * us, ds.ns_bind * us,
                ds.ns_draw * us);
        fprintf(stderr,
                "[drawstats] pipeline hits: programs+fp-consts %.0f us, key %.0f us,"
                " scan %.0f us (%.1f entries/draw of %u); program memo vp %llu/%llu fp %llu/%llu hits\n",
                ds_pk_ns[0] * us, ds_pk_ns[1] * us, ds_pk_ns[2] * us,
                ds.draws ? (double)ds_pk_scanned / ds.draws : 0.0, g.n_pipelines,
                (unsigned long long)ds_memo_hit[0], (unsigned long long)(ds_memo_hit[0] + ds_memo_miss[0]),
                (unsigned long long)ds_memo_hit[1], (unsigned long long)(ds_memo_hit[1] + ds_memo_miss[1]));
        fprintf(stderr, "[drawstats] texture re-hash: %llu textures, %.1f MB, %.0f us, %llu changed; write-tracked clean: %llu textures, %llu vertex, %llu index\n",
                (unsigned long long)ds_th_count, ds_th_bytes / 1e6, ds_th_ns * us,
                (unsigned long long)ds_th_changed, (unsigned long long)ds_th_wt_clean,
                (unsigned long long)ds_vc_wt_clean, (unsigned long long)ds_ic_wt_clean);
        fprintf(stderr, "[drawstats] vertex cache: %llu hits, %llu misses, %llu resting, %llu new; hashed %.1f MB in %.0f us; %u keys, %.0f MB resident\n",
                (unsigned long long)ds_vc_hit, (unsigned long long)ds_vc_miss, (unsigned long long)ds_vc_skip,
                (unsigned long long)ds_vc_new, ds_vc_hashed / 1e6, ds_vc_ns * us, s_vc_used, s_vc_bytes / 1e6);
        fprintf(stderr, "[drawstats] index cache: %llu hits, %llu misses, %llu new; hashed %.1f MB; %u keys, %.1f MB resident\n",
                (unsigned long long)ds_ic_hit, (unsigned long long)ds_ic_miss, (unsigned long long)ds_ic_new,
                ds_ic_hashed / 1e6, s_ic_used, s_ic_bytes / 1e6);
        if (ds.span_draws)
            fprintf(stderr, "[drawstats] index ranges: %llu draws, span %.0f vs unique %.0f vertices"
                    " (%.2fx), refs %.0f; %llu draws (%.0f%%) dense (span <= 1.25 x unique)\n",
                    (unsigned long long)ds.span_draws, (double)ds.span_sum, (double)ds.span_uniq,
                    ds.span_uniq ? (double)ds.span_sum / ds.span_uniq : 0.0, (double)ds.span_refs,
                    (unsigned long long)ds.span_dense, 100.0 * ds.span_dense / ds.span_draws);
        static const char* tn[8] = { "?", "snorm16", "float", "half", "unorm8", "sint16", "cmp32", "uint8" };
        char line[600]; int p = snprintf(line, sizeof line, "[drawstats] formats (unique vtx):");
        for (int t = 0; t < 8; t++) for (int z = 1; z <= 4; z++)
            if (ds.fmt[t][z]) p += snprintf(line + p, sizeof line - p, " %sx%d=%llu", tn[t], z,
                                            (unsigned long long)ds.fmt[t][z]);
        fprintf(stderr, "%s\n", line);
    }
    memset(&ds, 0, sizeof ds); memset(ds_pk_ns, 0, sizeof ds_pk_ns); ds_pk_scanned = 0; memset(ds_memo_hit, 0, sizeof ds_memo_hit); memset(ds_memo_miss, 0, sizeof ds_memo_miss); ds_th_ns = ds_th_bytes = ds_th_count = ds_th_changed = 0; ds_th_wt_clean = ds_vc_wt_clean = ds_ic_wt_clean = 0; ds_vc_hit = ds_vc_miss = ds_vc_skip = ds_vc_new = ds_vc_hashed = ds_vc_ns = 0; ds_ic_hit = ds_ic_miss = ds_ic_new = ds_ic_hashed = 0;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
/* ---- vertex cache ---------------------------------------------------------
 * Most of a New Marais frame's ~1 M fetched vertices are static geometry whose
 * source bytes do not change from one frame to the next, yet every draw
 * fetched, decoded and staged them again (fetch+decode ~5.4 ms of the RSX
 * thread a frame, plus the staging copy). A draw over one consecutive range
 * whose every attribute maps a stable guest span is keyed on its layout and
 * spans and checked by a hash of those source bytes; when the same bytes are
 * seen in two different frames the decoded vertices go into a persistent
 * backend buffer, and later draws with the same key and bytes draw from it.
 * A key whose bytes change three times running (skinned or streamed
 * geometry) is not hashed again for a while. PS3RECOMP_ENG_VCACHE=0 = off. */
typedef struct {
    u64 key, content;
    u32 vbuf, bytes, n, stride;
    u32 last_frame, seen_frame, skip_until;
    u8 seen, changes;
    u8 wt_ok;                    /* rsx_wtrack: spans armed, wt_content is their hash */
    u64 wt_stamp, wt_content;
} eng_vc_entry;
#define ENG_VC_SIZE   65536u
#define ENG_VC_BUDGET (512ull << 20)
static eng_vc_entry s_vc[ENG_VC_SIZE];
static u32 s_vc_cursor;
/* PS3RECOMP_ENG_PREFETCH (rsx3, default 1; 0 = off): a frame draws the same
 * objects in the same order as the frame before, so the cache entry the next
 * draw will probe is, nearly always, the one that followed this one last time.
 * Each table keeps that successor per entry (a parallel array: the entries
 * stay 64 bytes) and prefetches it while this draw is still being processed --
 * the probes of the 4 MB vertex and 1 MB index tables were cache misses, ~4.5%
 * of the RSX thread in the city. Only a hint: a wrong or stale successor (the
 * tables are rebuilt on a prune) costs one useless prefetch. */
static u32 s_vc_next[ENG_VC_SIZE], s_vc_prev;
static int s_prefetch_on = -1;
static inline void eng_cache_predict(const void* table, size_t esz, u32* next, u32* prev, u32 idx)
{
    if (__builtin_expect(s_prefetch_on < 0, 0)) {
        const char* e = getenv("PS3RECOMP_ENG_PREFETCH");
        s_prefetch_on = !(e && e[0] == '0');
    }
    if (!s_prefetch_on) return;
    const u32 me = idx + 1u;
    if (*prev && *prev != me) next[*prev - 1u] = me;
    *prev = me;
    const u32 nx = next[idx];
    if (nx) {
        const char* p = (const char*)table + (size_t)(nx - 1u) * esz;
        __builtin_prefetch(p, 0, 3);
        __builtin_prefetch(p + esz - 1u, 0, 3);   /* an entry that straddles two lines */
    }
}

/* PS3_TOGGLE_FILE can switch it live: the watcher only records the wish, and
 * the RSX thread (the cache's only user) acts on it at its next draw. Off
 * drops every persistent buffer (the backend retires them once the GPU is past
 * them, as the "full: start over" path does); on starts from an empty cache. */
static int s_vc_want = -1, s_vc_applied = -2;   /* -2: not used yet */
static void eng_vc_set(int id, int on) { (void)id; __atomic_store_n(&s_vc_want, on != 0, __ATOMIC_RELAXED); }
static void eng_vc_drop(eng_vc_entry* e);
static __attribute__((noinline)) int eng_vc_switch(void)
{
    if (__atomic_load_n(&s_vc_want, __ATOMIC_RELAXED) < 0) ps3_toggle_watch(PS3T_ENG_VCACHE, eng_vc_set);
    const int want = __atomic_load_n(&s_vc_want, __ATOMIC_RELAXED);
    if (s_vc_applied >= 0) {   /* a change, not the first use */
        for (u32 i = 0; i < ENG_VC_SIZE; i++) eng_vc_drop(&s_vc[i]);
        memset(s_vc, 0, sizeof s_vc); s_vc_used = 0;
    }
    s_vc_applied = want;
    return want;
}
static int eng_vc_on(void)
{
    int on = s_vc_applied;
    if (__builtin_expect(on != __atomic_load_n(&s_vc_want, __ATOMIC_RELAXED), 0)) on = eng_vc_switch();
    return on && g.be->vbuf_create && g.be->draw_vbuf && g.be->vbuf_release;
}

static u64 eng_hash_span(const u8* src, u64 n, u64 seed)
{
    u64 h[8];
    for (int k = 0; k < 8; k++) h[k] = seed ^ (0x9E3779B97F4A7C15ull * (u64)(k + 1));
    u64 i = 0;
    for (; i + 64 <= n; i += 64) {
        u64 w[8]; memcpy(w, src + i, sizeof w);
        for (int k = 0; k < 8; k++) h[k] = (h[k] ^ w[k]) * 1099511628211ull;
    }
    u64 r = n;
    for (int k = 0; k < 8; k++) r ^= h[k] * (u64)(2 * k + 1);
    for (; i < n; i++) { r ^= src[i]; r *= 1099511628211ull; }
    return r;
}

/* Word-at-a-time mix for the cache key (the byte-wise FNV of ~200 bytes of
 * attribute description per draw was most of the probe's cost). */
static inline u64 eng_vc_mix(u64 h, u64 v)
{
    h ^= v * 0x9E3779B97F4A7C15ull;
    h = (h << 27) | (h >> 37);
    return h * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
}

static void eng_vc_drop(eng_vc_entry* e)
{
    if (e->vbuf) { g.be->vbuf_release(g.be->user, e->vbuf); s_vc_bytes -= e->bytes; }
    e->vbuf = 0; e->bytes = 0;
}

/* The table full: keep what was drawn recently, forget the rest. Skinned and
 * streamed geometry lands at a new address every frame, so New Marais adds
 * ~300 keys a frame that are never seen again, and the table used to fill
 * every 5-9 s of the walk and be dropped whole -- the static geometry's
 * persistent buffers with it, which the next two frames then fetched,
 * decoded and staged again. Now the entries not drawn in the last
 * ENG_VC_KEEP frames are dropped and the rest are re-inserted; only when that
 * frees too little does the cache start over as before.
 * PS3RECOMP_ENG_VCACHE_PRUNE=0 (environment or PS3_TOGGLE_FILE, read at
 * each fill) = start over every time, the old way.
 *
 * In a dense street (Erik's dusk session near the Club Bar, 2026-10-24) the
 * table filled every 60-130 frames and twice kept more than half of it
 * (36574 and 46205 keys drawn in the last 60 frames) -- so it started over,
 * dropping every persistent buffer, at 20 fps. Most of those keys were the
 * one-frame ones (skinned characters, streamed geometry: ~300-700 new keys a
 * frame there): an entry without a persistent buffer that was not drawn in
 * the last ENG_VC_KEEP_NEW frames cannot become persistent any more (that
 * takes the same bytes in two different frames in a row) and now goes too.
 * PS3RECOMP_ENG_VCACHE_PRUNE=1 = keep every key drawn in the last 60 frames
 * (v0.2.13); 2 (default) = this. */
#define ENG_VC_KEEP 60u
#define ENG_VC_KEEP_NEW 2u
static eng_vc_entry s_vc_tmp[ENG_VC_SIZE];
static int eng_vc_prune(void)
{
    static int env = -1;
    if (env < 0) { const char* e = getenv("PS3RECOMP_ENG_VCACHE_PRUNE"); env = e ? atoi(e) : 2; }
    const int mode = ps3_toggle_get("PS3RECOMP_ENG_VCACHE_PRUNE", env);
    if (!mode) return 0;
    u32 kept = 0;
    memcpy(s_vc_tmp, s_vc, sizeof s_vc);
    memset(s_vc, 0, sizeof s_vc);
    for (u32 i = 0; i < ENG_VC_SIZE; i++) {
        eng_vc_entry* o = &s_vc_tmp[i];
        if (!o->key) continue;
        const u32 keep = (mode >= 2 && !o->vbuf) ? ENG_VC_KEEP_NEW : ENG_VC_KEEP;
        if (o->last_frame + keep < g.frames && o->skip_until <= g.frames) { eng_vc_drop(o); continue; }
        u32 p = (u32)(o->key ^ (o->key >> 32)) & (ENG_VC_SIZE - 1u);
        while (s_vc[p].key) p = (p + 1) & (ENG_VC_SIZE - 1u);
        s_vc[p] = *o;
        kept++;
    }
    s_vc_used = kept;
    { static int n = 0; if (PS3_LOG_FIRST(n, 16))
        fprintf(stderr, "[rsx engine] vertex cache full: kept %u recent keys (%.0f MB resident), frame %u\n",
                kept, s_vc_bytes / 1e6, g.frames); }
    return kept <= ENG_VC_SIZE / 2u;
}

/* Key, then (unless the key is resting) the content hash of the spans. NULL:
 * this draw is not cached. */
static eng_vc_entry* eng_vc_probe(const rsx_vertex_layout_plan* layout, u64* content)
{
    const rsx_vertex_fetch_plan* pl = &dc.fetch_plan;
    if (pl->inline_data || !dc.n_refs) return NULL;
    u64 key = eng_vc_mix(0x243F6A8885A308D3ull, ((u64)layout->count << 32) | layout->stride);
    key = eng_vc_mix(key, dc.n_refs);
    const u8* lo[RSX_DSP_NUM_VERTEX_ATTR]; const u8* hi[RSX_DSP_NUM_VERTEX_ATTR];
    u32 nr = 0;
    for (u32 slot = 0; slot < layout->count; slot++) {
        const u32 a = layout->attrs[slot];
        const rsx_vertex_fetch_attr* f = &pl->attr[a];
        key = eng_vc_mix(key, ((u64)a << 56) ^ ((u64)f->desc.type << 48) ^ ((u64)f->desc.size << 40) ^
                              ((u64)((pl->divider_mask >> a) & 1u) << 39) ^ ((u64)f->desc.frequency << 16) ^
                              ((u64)f->stride << 8) ^ f->elem_size);
        if (f->desc.type && f->desc.size && f->elem_size) {
            if (!f->stable_base) return NULL;          /* resolver path: not a plain span */
            key = eng_vc_mix(key, (u64)(uintptr_t)f->stable_base);
            key = eng_vc_mix(key, ((u64)f->stable_first << 32) | f->stable_last);
            lo[nr] = f->stable_base;
            hi[nr] = f->stable_base + (u64)(f->stable_last - f->stable_first) * f->stride + f->elem_size;
            nr++;
        } else {
            u64 dv[2]; memcpy(dv, f->default_value, sizeof dv);
            key = eng_vc_mix(eng_vc_mix(key, dv[0]), dv[1]);
        }
    }
    if (!key) key = 1;
    u32 p = (u32)(key ^ (key >> 32)) & (ENG_VC_SIZE - 1u);
    eng_vc_entry* e = NULL;
    for (u32 probe = 0; probe < 32; probe++, p = (p + 1) & (ENG_VC_SIZE - 1u)) {
        if (s_vc[p].key == key) { e = &s_vc[p]; break; }
        if (!s_vc[p].key) {
            if (s_vc_used >= ENG_VC_SIZE / 4u * 3u) {   /* full: prune, else start over */
                if (eng_vc_prune()) return NULL;
                { static int n = 0; if (PS3_LOG_FIRST(n, 64))
                    fprintf(stderr, "[rsx engine] vertex cache: %u keys, %.0f MB resident -> start over (frame %u)\n",
                            s_vc_used, s_vc_bytes / 1e6, g.frames); }
                for (u32 i = 0; i < ENG_VC_SIZE; i++) eng_vc_drop(&s_vc[i]);
                memset(s_vc, 0, sizeof s_vc); s_vc_used = 0;
                return NULL;
            }
            e = &s_vc[p]; memset(e, 0, sizeof *e); e->key = key; s_vc_used++;
            break;
        }
    }
    if (!e) return NULL;
    eng_cache_predict(s_vc, sizeof *e, s_vc_next, &s_vc_prev, (u32)(e - s_vc));
    if (e->skip_until > g.frames) { ds_vc_skip++; return NULL; }
    /* Hash the union of the attribute spans: interleaved attributes share one. */
    for (u32 i = 1; i < nr; i++)            /* insertion sort by start */
        for (u32 j = i; j > 0 && lo[j] < lo[j - 1]; j--) {
            const u8* t = lo[j]; lo[j] = lo[j - 1]; lo[j - 1] = t;
            t = hi[j]; hi[j] = hi[j - 1]; hi[j - 1] = t;
        }
    const u8* ms[RSX_DSP_NUM_VERTEX_ATTR]; u64 mn[RSX_DSP_NUM_VERTEX_ATTR]; u32 nm = 0;
    for (u32 i = 0; i < nr; ) {
        const u8* a0 = lo[i]; const u8* a1 = hi[i];
        for (i++; i < nr && lo[i] <= a1 + 64; i++) if (hi[i] > a1) a1 = hi[i];
        ms[nm] = a0; mn[nm] = (u64)(a1 - a0); nm++;
    }
    /* rsx_wtrack: spans whose pages stayed write-protected hash as they did. */
    const int wt = rsx_wt_mode();
    int wt_clean = 0;
    if (wt && e->wt_ok) {
        u64 st = 0; wt_clean = 1;
        for (u32 k = 0; k < nm && wt_clean; k++) wt_clean = rsx_wt_query(ms[k], mn[k], &st);
        wt_clean = wt_clean && st == e->wt_stamp;
        if (wt_clean && wt == 1) { *content = e->wt_content; ds_vc_wt_clean++; return e; }
    }
    u64 st = 0; int armed = wt && !wt_clean;
    if (armed) for (u32 k = 0; k < nm; k++) armed &= rsx_wt_arm(ms[k], mn[k], &st);
    u64 c = 0x51ED270B27A3F5E1ull;
    for (u32 k = 0; k < nm; k++) {
        c = eng_hash_span(ms[k], mn[k], c);
        ds_vc_hashed += mn[k];
    }
    if (wt_clean) {                               /* shadow mode */
        if (c != e->wt_content) { rsx_wt_report_miss("vertices", ms[0], (size_t)mn[0]); e->wt_ok = 0; }
        else rsx_wt_note_verified();
    } else if (wt) { e->wt_ok = (u8)armed; e->wt_stamp = st; e->wt_content = c; }
    *content = c;
    return e;
}

/* After a fetch: a second sighting of the same bytes in a later frame makes
 * the decoded vertices persistent; changed bytes drop them. */
static void eng_vc_after_fetch(eng_vc_entry* e, u64 content, u32 stride)
{
    const u32 bytes = dc.n_refs * stride;
    if (e->seen && e->content == content && e->seen_frame != g.frames) {
        if (!e->vbuf) {
            while (s_vc_bytes + bytes > ENG_VC_BUDGET) {      /* evict the stale */
                u32 tries = 0;
                for (; tries < ENG_VC_SIZE; tries++, s_vc_cursor = (s_vc_cursor + 1) & (ENG_VC_SIZE - 1u))
                    if (s_vc[s_vc_cursor].vbuf && s_vc[s_vc_cursor].last_frame + 2 < g.frames) break;
                if (tries == ENG_VC_SIZE) break;
                eng_vc_drop(&s_vc[s_vc_cursor]);
            }
            if (s_vc_bytes + bytes <= ENG_VC_BUDGET) {
                e->vbuf = g.be->vbuf_create(g.be->user, dc.verts, bytes);
                if (e->vbuf) { e->bytes = bytes; e->n = dc.n_refs; e->stride = stride; s_vc_bytes += bytes; ds_vc_new++; }
            }
        }
        e->changes = 0;
    } else if (e->seen && e->content != content) {
        eng_vc_drop(e);
        if (++e->changes >= 3) { e->skip_until = g.frames + 240u; e->changes = 0; }
    }
    e->content = content; e->seen = 1; e->seen_frame = g.frames; e->last_frame = g.frames;
}

/* ---- resident index buffers -------------------------------------------------
 * A vertex-cache hit still decoded the draw's guest indices into references
 * (dc_decode_indices), found their range (rsx_vertex_remap_build_range),
 * rebuilt the triangle list (rsx_draw_engine_write_topology_indices) and
 * copied it into the frame's staging arena -- four passes over ~1.9 M
 * references a New Marais frame for index lists that do not change. An
 * indexed draw is keyed on everything its index list derives from (index
 * array offset, location and width, restart state, base index, primitive,
 * the index batches) and checked by a hash of the raw guest indices; when
 * the same bytes come back in a later frame the rebuilt list goes into a
 * persistent backend buffer (the vertex cache's vbuf_create), and later
 * draws with the same key and bytes skip all four passes when their vertices
 * also come from the vertex cache. Only draws whose indices form one range
 * with no restart cut qualify. PS3RECOMP_ENG_ICACHE=0 (live) = off. */
typedef struct {
    u64 key, content;
    u32 ibuf, n_draw;            /* persistent rebuilt index list, its length */
    u32 lo, span, n_src;         /* first vertex id, unique vertices, source references */
    u32 last_frame, seen_frame, skip_until;
    u8 seen, changes;
    u8 wt_ok;                    /* rsx_wtrack: runs armed, wt_content is their hash */
    u64 wt_stamp, wt_content;
} eng_ic_entry;
#define ENG_IC_SIZE   16384u
#define ENG_IC_BUDGET (128ull << 20)
static eng_ic_entry s_ic[ENG_IC_SIZE];
static u32 s_ic_next[ENG_IC_SIZE], s_ic_prev;   /* PS3RECOMP_ENG_PREFETCH (eng_cache_predict) */
static int s_ic_want = -1, s_ic_applied = -2;
/* The current draw: the entry its key found (and the content it hashed to),
 * and whether the index work was skipped for it. */
static eng_ic_entry* s_ic_cur;
static u64 s_ic_cur_content;
static int s_ic_hit, s_ic_ranged;
static void eng_ic_set(int id, int on) { (void)id; __atomic_store_n(&s_ic_want, on != 0, __ATOMIC_RELAXED); }
static void eng_ic_drop(eng_ic_entry* e)
{
    if (e->ibuf) { g.be->vbuf_release(g.be->user, e->ibuf); s_ic_bytes -= (u64)e->n_draw * 4u; }
    e->ibuf = 0;
}
static void eng_ic_clear(void)
{
    for (u32 i = 0; i < ENG_IC_SIZE; i++) eng_ic_drop(&s_ic[i]);
    memset(s_ic, 0, sizeof s_ic); s_ic_used = 0;
}
static __attribute__((noinline)) int eng_ic_switch(void)
{
    if (__atomic_load_n(&s_ic_want, __ATOMIC_RELAXED) < 0) ps3_toggle_watch(PS3T_ENG_ICACHE, eng_ic_set);
    const int want = __atomic_load_n(&s_ic_want, __ATOMIC_RELAXED);
    if (s_ic_applied >= 0) eng_ic_clear();
    s_ic_applied = want;
    return want;
}
static int eng_ic_on(void)
{
    int on = s_ic_applied;
    if (__builtin_expect(on != __atomic_load_n(&s_ic_want, __ATOMIC_RELAXED), 0)) on = eng_ic_switch();
    return on && g.be->vbuf_create && g.be->vbuf_release && g.be->draw_vbuf_ib;
}
/* Full: keep the entries drawn in the last ENG_VC_KEEP frames, else start over. */
static void eng_ic_prune(void)
{
    static eng_ic_entry tmp[ENG_IC_SIZE];
    memcpy(tmp, s_ic, sizeof s_ic);
    memset(s_ic, 0, sizeof s_ic);
    u32 kept = 0;
    for (u32 i = 0; i < ENG_IC_SIZE; i++) {
        eng_ic_entry* o = &tmp[i];
        if (!o->key) continue;
        const u32 keep = o->ibuf ? 60u : 2u;
        if (o->last_frame + keep < g.frames && o->skip_until <= g.frames) { eng_ic_drop(o); continue; }
        u32 p = (u32)(o->key ^ (o->key >> 32)) & (ENG_IC_SIZE - 1u);
        while (s_ic[p].key) p = (p + 1) & (ENG_IC_SIZE - 1u);
        s_ic[p] = *o; kept++;
    }
    s_ic_used = kept;
    if (kept > ENG_IC_SIZE / 2u) {
        { static int n = 0; if (PS3_LOG_FIRST(n, 16))
            fprintf(stderr, "[rsx engine] index cache: %u keys -> start over (frame %u)\n", kept, g.frames); }
        eng_ic_clear();
    }
}
/* The current indexed draw's entry, NULL when it does not qualify. *content
 * is the hash of its raw guest indices. */
static eng_ic_entry* eng_ic_probe(const rsx_dsp_index_array* ia, u32 ia_loc, int restart_en,
                                  u32 restart_val, u32 base_index, u64* content)
{
    const u32 esz = ia->is_u32 ? 4u : 2u;
    u64 key = eng_vc_mix(0x452821E638D01377ull, ((u64)ia->offset << 32) | ((u64)ia_loc << 4) |
                         ((u64)ia->is_u32 << 1) | (u64)(restart_en != 0));
    key = eng_vc_mix(key, ((u64)g.rsx.current_primitive << 32) | base_index);
    key = eng_vc_mix(key, restart_en ? restart_val : 0u);
    key = eng_vc_mix(key, dc.n_idx);
    for (u32 b = 0; b < dc.n_idx; b++) {
        const u32 first = dc.idx[b].first, count = dc.idx[b].count;
        key = eng_vc_mix(key, ((u64)first << 32) | count);
        const u64 start = (u64)ia->offset + (u64)first * esz, bytes = (u64)count * esz;
        if (!count || start + bytes > 0x100000000ull) return NULL;
        if (!eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes)) return NULL;
    }
    if (!key) key = 1;
    u32 p = (u32)(key ^ (key >> 32)) & (ENG_IC_SIZE - 1u);
    eng_ic_entry* e = NULL;
    for (u32 probe = 0; probe < 32; probe++, p = (p + 1) & (ENG_IC_SIZE - 1u)) {
        if (s_ic[p].key == key) { e = &s_ic[p]; break; }
        if (!s_ic[p].key) {
            if (s_ic_used >= ENG_IC_SIZE / 4u * 3u) { eng_ic_prune(); return NULL; }
            e = &s_ic[p];
            memset(e, 0, sizeof *e); e->key = key; s_ic_used++;
            break;
        }
    }
    if (!e) return NULL;
    eng_cache_predict(s_ic, sizeof *e, s_ic_next, &s_ic_prev, (u32)(e - s_ic));
    /* The raw indices' hash -- or, rsx_wtrack, the one they had when their
     * pages were armed, while those stay unwritten. */
    const int wt = rsx_wt_mode();
    int wt_clean = 0;
    if (wt && e->wt_ok) {
        u64 st = 0; wt_clean = 1;
        for (u32 b = 0; b < dc.n_idx && wt_clean; b++) {
            const u64 start = (u64)ia->offset + (u64)dc.idx[b].first * esz, bytes = (u64)dc.idx[b].count * esz;
            wt_clean = rsx_wt_query(eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes), (size_t)bytes, &st);
        }
        wt_clean = wt_clean && st == e->wt_stamp;
        if (wt_clean && wt == 1) { *content = e->wt_content; ds_ic_wt_clean++; return e; }
    }
    u64 st = 0; int armed = wt && !wt_clean;
    u64 c = 0x2B992DDFA23249D6ull;
    if (armed)
        for (u32 b = 0; b < dc.n_idx; b++) {
            const u64 start = (u64)ia->offset + (u64)dc.idx[b].first * esz, bytes = (u64)dc.idx[b].count * esz;
            armed &= rsx_wt_arm(eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes), (size_t)bytes, &st);
        }
    for (u32 b = 0; b < dc.n_idx; b++) {
        const u64 start = (u64)ia->offset + (u64)dc.idx[b].first * esz, bytes = (u64)dc.idx[b].count * esz;
        c = eng_hash_span(eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes), bytes, c);
        ds_ic_hashed += bytes;
    }
    if (wt_clean) {                               /* shadow mode */
        if (c != e->wt_content) { rsx_wt_report_miss("indices", eng_guest_ptr(NULL, ia_loc, ia->offset, 4), 4); e->wt_ok = 0; }
        else rsx_wt_note_verified();
    } else if (wt) { e->wt_ok = (u8)armed; e->wt_stamp = st; e->wt_content = c; }
    *content = c;
    return e;
}
/* After an indexed draw went the whole way (the rebuilt list is in
 * g.indices): a second sighting of the same indices in a later frame makes
 * the list resident; changed indices drop it. */
static void eng_ic_after_draw(eng_ic_entry* e, u64 content, u32 n_draw)
{
    if (e->skip_until > g.frames) return;
    if (e->seen && e->content == content && e->seen_frame != g.frames) {
        if (!e->ibuf && s_ic_ranged && !dc.n_cuts && dc.refs_remapped && dc.n_refs &&
            s_ic_bytes + (u64)n_draw * 4u <= ENG_IC_BUDGET) {
            e->ibuf = g.be->vbuf_create(g.be->user, g.indices, n_draw * 4u);
            if (e->ibuf) {
                e->n_draw = n_draw; e->lo = dc.refs[0].vertex_id;
                e->span = dc.n_refs; e->n_src = dc.n_source_refs;
                s_ic_bytes += (u64)n_draw * 4u; ds_ic_new++;
            }
        }
        e->changes = 0;
    } else if (e->seen && e->content != content) {
        eng_ic_drop(e);
        if (++e->changes >= 3) { e->skip_until = g.frames + 240u; e->changes = 0; }
    }
    e->content = content; e->seen = 1; e->seen_frame = g.frames; e->last_frame = g.frames;
}

/* The draw's fetch plan. PS3RECOMP_ENG_PLAN_LAYOUT=1 (default 0 = all
 * sixteen attributes decoded and the plan cleared, as before): only the
 * layout's attributes (rsx_vertex_fetch_plan_init_layout). */
static void eng_plan_init(const rsx_vertex_layout_plan* layout)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_PLAN_LAYOUT"); on = !(e && e[0] == '0'); }   /* rsx3: default on */
    if (on) rsx_vertex_fetch_plan_init_layout(&dc.fetch_plan, &g.rsx, layout, eng_guest_ptr, NULL);
    else    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, eng_guest_ptr, NULL);
}
/* Write the references an index-cache hit left out (dc.refs_lazy). */
static void dc_refs_fill(void)
{
    if (!dc.refs_lazy) return;
    const u32 lo = dc.refs[0].vertex_id;
    for (u32 i = 1; i < dc.n_refs; i++) { dc.refs[i].vertex_id = lo + i; dc.refs[i].base_index = dc.lazy_base; }
    dc.refs_lazy = 0;
}
static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap)
{
    s_ic_cur = NULL; s_ic_hit = 0; s_ic_ranged = 0; dc.refs_lazy = 0;
    /* PS3RECOMP_ENG_ICACHE: an index-only draw whose rebuilt list is resident
     * and whose vertices the vertex cache holds skips the index work. */
    if (dc.n_idx && !dc.n_arr && !dc.inl && dc.fetch_ok && allow_remap && layout->stride &&
        eng_ic_on() && eng_vc_on()) {
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        s_ic_cur = eng_ic_probe(&ia, ia_loc, restart_en, rsx_dsp_restart_index(&g.rsx), base_index,
                                &s_ic_cur_content);
        eng_ic_entry* e = s_ic_cur;
        if (e && e->ibuf && e->content == s_ic_cur_content && e->skip_until <= g.frames &&
            dc_reserve_refs(e->span)) {
            /* The references the range remap would have left: span
             * consecutive vertex ids from lo. */
            dc.n_refs = e->span; dc.n_source_refs = e->n_src; dc.refs_remapped = 1; dc.n_cuts = 0;
            dc.layout = *layout;
            eng_plan_init(layout);
            rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, NULL, 0);
            /* PS3RECOMP_ENG_LAZY_REFS=1 (default off: no measured gain): the plan below reads only
             * refs[0] of a consecutive run unless an attribute has a frequency
             * divider; a hit never reads the rest (writing them was ~0.3 ms of
             * the RSX thread a frame in the pier view). */
            static int lazy_on = -1;
            if (lazy_on < 0) { const char* ev = getenv("PS3RECOMP_ENG_LAZY_REFS"); lazy_on = !(ev && ev[0] == '0'); }   /* rsx3: default on */
            /* ... and only while the run does not wrap the 20-bit element index
             * (prepare walks every reference then). */
            int lazy = lazy_on && e->span > 1 &&
                       (u64)rsx_vertex_element_index(e->lo, base_index, 0, 0) + e->span - 1u <= 0x000FFFFFu;
            for (u32 slot = 0; lazy && slot < dc.fetch_plan.layout.count; slot++)
                if (dc.fetch_plan.attr[dc.fetch_plan.layout.attrs[slot]].desc.frequency > 1) lazy = 0;
            const u32 nw = lazy ? 1u : e->span;
            for (u32 i = 0; i < nw; i++) { dc.refs[i].vertex_id = e->lo + i; dc.refs[i].base_index = base_index; }
            dc.refs_lazy = lazy; dc.lazy_base = base_index;
            rsx_vertex_fetch_plan_prepare_ex(&dc.fetch_plan, dc.refs, dc.n_refs, 1);
            u64 vc_content = 0;
            eng_vc_entry* ve = eng_vc_probe(layout, &vc_content);
            if (ve && ve->vbuf && ve->content == vc_content &&
                ve->n == dc.n_refs && ve->stride == layout->stride) {
                ve->last_frame = g.frames;
                dc.vbuf = ve->vbuf; dc.vbuf_hit = 1; dc.n_verts = dc.n_refs;
                e->last_frame = g.frames;
                s_ic_hit = 1; ds_ic_hit++; ds_vc_hit++;
                if (eng_ds_enabled()) { ds.refs += dc.n_source_refs; ds.uniq += dc.n_refs; }
                return;
            }
            /* The vertices changed: the whole way, as if nothing were cached. */
            dc.n_refs = dc.n_source_refs = 0; dc.refs_remapped = 0; dc.refs_lazy = 0;
        }
        if (s_ic_cur) ds_ic_miss++;
    }

    const int dso = eng_ds_enabled();
    u64 t0 = dso ? eng_ds_now() : 0, t1;
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++)
        for (u32 i = 0; i < dc.arr[b].count && dc.fetch_ok; i++)
            if (!dc_push_ref(dc.arr[b].first + i, 0)) dc.fetch_ok = 0;

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;

        for (u32 b = 0; b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            /* The common case -- the whole run mapped -- in one tight loop
             * per index width: room for every reference reserved up front,
             * no per-index resolver or capacity check. Cuts land at the
             * same reference positions as below. */
            if (run && count) {
                if (!dc_reserve_refs((u64)dc.n_refs + count)) { dc.fetch_ok = 0; break; }
                if (!dc_decode_indices(run, count, ia.is_u32, restart_en, restart_val, base_index))
                    dc.fetch_ok = 0;
                continue;
            }
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_guest_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) return;

    dc.n_source_refs = dc.n_refs;
    if (dso) { t1 = eng_ds_now(); ds.ns_refs += t1 - t0; t0 = t1; }
    /* Only a topology this engine rebuilds through an index buffer may share
     * repeated references. A line strip or a point list is drawn in the order
     * it arrived, so collapsing duplicates would reorder it. */
    int consecutive = 0;   /* refs = one run of vertex ids (the ranged remap) */
    if (allow_remap && dc.n_refs > 1) {
        u32 unique = dc.n_refs;
        /* Drawstats: how dense each draw's index range is -- the span from its
         * lowest to highest vertex against the unique vertices it uses. A
         * dense range could be fetched whole without deduplicating. */
        u32 span_lo = ~0u, span_hi = 0; int span_same_base = 1;
        if (dso) {
            for (u32 i = 0; i < dc.n_refs; i++) {
                const u32 v = dc.refs[i].vertex_id;
                if (v < span_lo) span_lo = v;
                if (v > span_hi) span_hi = v;
                span_same_base &= dc.refs[i].base_index == dc.refs[0].base_index;
            }
        }
        /* The draw's whole index range first: meshes cover it (see
         * rsx_vertex_remap_build_range), and fetching it in address order
         * skips deduplication, 11.6% of the RSX thread (T-0016).
         * PS3RECOMP_ENG_DEDUP=1 deduplicates always, for an A/B. */
        static int dedup = -1;
        if (dedup < 0) dedup = getenv("PS3RECOMP_ENG_DEDUP") ? 1 : 0;
        u32 ranged = 0;
        if (!dedup && rsx_vertex_remap_build_range(&dc.ref_remap, dc.refs, dc.n_refs, &ranged)) {
            dc.refs_remapped = 1;
            dc.n_refs = ranged;
            consecutive = 1;
            s_ic_ranged = 1;
        } else if (rsx_vertex_remap_build(&dc.ref_remap, dc.refs, dc.n_refs, &unique)) {
            if (dso && span_same_base) {
                const u64 span = (u64)span_hi - span_lo + 1u;
                ds.span_draws++; ds.span_sum += span; ds.span_uniq += unique;
                ds.span_refs += dc.n_source_refs;
                if (span <= (u64)unique + unique / 4u) ds.span_dense++;
            }
            dc.refs_remapped = unique < dc.n_refs;
            dc.n_refs = unique;
        }
    }

    if (dso) { t1 = eng_ds_now(); ds.ns_remap += t1 - t0; t0 = t1; }
    dc.layout = *layout;
    eng_plan_init(layout);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare_ex(&dc.fetch_plan, dc.refs, dc.n_refs, consecutive);
    if (dso) { t1 = eng_ds_now(); ds.ns_plan += t1 - t0; t0 = t1; }
    eng_vc_entry* ve = NULL;
    u64 vc_content = 0;
    if (consecutive && layout->stride && eng_vc_on()) {
        const u64 v0 = dso ? eng_ds_now() : 0;
        ve = eng_vc_probe(layout, &vc_content);
        if (dso) ds_vc_ns += eng_ds_now() - v0;
        if (ve && ve->vbuf && ve->content == vc_content &&
            ve->n == dc.n_refs && ve->stride == layout->stride) {
            ve->last_frame = g.frames;
            dc.layout = *layout;
            dc.vbuf = ve->vbuf;
            dc.vbuf_hit = 1;
            dc.n_verts = dc.n_refs;
            ds_vc_hit++;
            if (dso) { t1 = eng_ds_now(); ds.ns_fetch += t1 - t0;
                       ds.refs += dc.n_source_refs; ds.uniq += dc.n_refs; }
            return;
        }
        if (ve) ds_vc_miss++;
    }
    if (!dc_reserve_verts((u64)dc.n_refs * layout->stride)) {
        dc.fetch_ok = 0;
        return;
    }
    /* Attribute by attribute with per-format loops; bit-identical to
     * rsx_vertex_fetch_one per reference (tests/test_vertex_fetch.c). */
    if (!rsx_vertex_fetch_all(&dc.fetch_plan, dc.refs, dc.n_refs,
                              layout->stride ? dc.verts : NULL)) {
        dc.fetch_ok = 0;
        return;
    }
    dc.n_verts = dc.n_refs;
    if (ve) eng_vc_after_fetch(ve, vc_content, layout->stride);
    if (dso) {
        ds.ns_fetch += eng_ds_now() - t0;
        ds.refs += dc.n_source_refs;
        ds.uniq += dc.n_refs;
        ds.slots += (u64)dc.n_refs * layout->count;
        for (u32 slot = 0; slot < layout->count; slot++) {
            const rsx_vertex_fetch_attr* f = &dc.fetch_plan.attr[layout->attrs[slot]];
            const u32 t = f->desc.type < 8 ? f->desc.type : 0, z = f->desc.size <= 4 ? f->desc.size : 0;
            ds.fmt[t][z] += dc.n_refs;
        }
    }
}

/* ---- sink ---------------------------------------------------------------- */

/* PS3RECOMP_ENG_VPLOG=1: print each distinct (viewport y scale sign, y
 * offset, SHADER_WINDOW 0x1D88, color offset 0x0210, surface clip 0x0200)
 * combination the first time a draw uses it -- which way each pass maps
 * NDC +y to rows. */
static void eng_vplog(const rsx_dispatch* r)
{
    static int on = -1;
    if (on < 0) on = getenv("PS3RECOMP_ENG_VPLOG") ? 1 : 0;
    if (!on) return;
    rsx_dsp_viewport vp; rsx_dsp_get_viewport(r, &vp);
    const u32 win = rsx_dsp_reg(r, 0x1D88), coff = rsx_dsp_reg(r, 0x0210), clip = rsx_dsp_reg(r, 0x0200);
    static u32 seen[64][4]; static int n;
    const u32 key[4] = { vp.scale[1] < 0.0f, win, coff, (u32)vp.translate[1] };
    for (int i = 0; i < n; i++) if (!memcmp(seen[i], key, sizeof key)) return;
    if (n >= 64) return;
    memcpy(seen[n++], key, sizeof key);
    fprintf(stderr, "[eng-vp] scale=(%.1f,%.1f) trans=(%.1f,%.1f) vp=%ux%u@%u,%u win=0x%08X color_off=0x%08X clip=0x%08X\n",
            vp.scale[0], vp.scale[1], vp.translate[0], vp.translate[1], vp.w, vp.h, vp.x, vp.y, win, coff, clip);
}

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)prim;
    eng_vplog(r);
    dc_reset();
}

/* PS3RECOMP_ENG_BATCH_MERGE (live): a DRAW_ARRAYS / DRAW_INDEX_ARRAY word
 * covers at most 256 vertices, so a big mesh arrives as a run of packets
 * that each continue the last. One that does (first == previous first +
 * count) extends it instead of taking a batch: the reference stream is the
 * same concatenation either way, there are fewer runs to walk, and a mesh
 * of more than ENG_MAX_BATCHES packets is no longer cut short. 0 = one batch
 * per packet. */
static int s_bmerge_on = -1;
static u32 s_batch_lost;   /* packets dropped for want of a batch (logged) */
static void eng_bmerge_set(int id, int v) { (void)id; __atomic_store_n(&s_bmerge_on, v != 0, __ATOMIC_RELAXED); }
static int eng_batch_merge(eng_batch* b, u32 n, u32 first, u32 count)
{
    if (__builtin_expect(s_bmerge_on < 0, 0)) ps3_toggle_watch(PS3T_ENG_BATCH_MERGE, eng_bmerge_set);
    if (!s_bmerge_on || !n) return 0;
    eng_batch* l = &b[n - 1];
    if ((u64)l->first + l->count != first || (u64)l->count + count > 0xFFFFFFFFull) return 0;
    l->count += count;
    return 1;
}
static void eng_batch_lost(void)
{
    s_batch_lost++;
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[rsx engine] draw has more than %u packets: packet dropped (%u so far)\n",
                ENG_MAX_BATCHES, s_batch_lost);
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (eng_batch_merge(dc.arr, dc.n_arr, first, count)) return;
    if (dc.n_arr >= ENG_MAX_BATCHES) { eng_batch_lost(); return; }
    dc.arr[dc.n_arr].first = first;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) {
        static int n;
        if (getenv("PS3RECOMP_ENG_DROPLOG") && PS3_LOG_FIRST(n, 6))
            fprintf(stderr, "[rsx engine] inline array dropped: stride=%u bytes=%u\n", stride, bytes);
        return;
    }
    dc.inl = data;
    dc.inl_bytes = bytes;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

static void eng_constlog_draw(u32 rt, u32 cw, u32 ch, u32 n, int indexed);

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (eng_batch_merge(dc.idx, dc.n_idx, first, count)) return;
    if (dc.n_idx >= ENG_MAX_BATCHES) { eng_batch_lost(); return; }
    dc.idx[dc.n_idx].first = first;
    dc.idx[dc.n_idx].count = count;
    dc.n_idx++;
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
/* Texture units the current guest fragment program samples: a unit can stay
 * enabled with a stale binding (a shadow pass keeps its own map bound), and a
 * snapshot is only worth taking for one the program reads. */
/* PS3RECOMP_ENG_BIND_UNREAD=1 binds every enabled unit again, read or not (A/B). */
static int eng_bind_unread(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PS3RECOMP_ENG_BIND_UNREAD") ? 1 : 0;
    return on;
}

/* ---- GPU light pass (PS3RECOMP_ENG_GPU_LIGHT=<diffuse off>:<specular off>) ----
 * inFAMOUS 2 lights its frame on the SPUs: the RSX blits the normals target and
 * the depth buffer into two main-memory buffers, an SPU job turns them into
 * diffuse and specular light in place, and the composite samples those two
 * buffers back. Here the blits become GPU copies, the light is computed by the
 * backend's light_pass with the job's own constants (handed over by the port
 * through rsx_draw_engine_gpu_light_data), and the composite's two texture
 * units get its outputs: no write-back flush, no readback, no SPU work. */
static struct {
    int on;                      /* -1 unknown */
    u32 diff_off, spec_off;      /* main-memory offsets of the two buffers */
    u32 n_tex, z_tex, w, h;      /* inputs captured at the blits */
    u32 have;                    /* bit 0 normals, bit 1 depth (this frame) */
    u32 in_seq, out_seq;
    u32 out_diff, out_spec;
    rsx_gpu_light_params params; /* last data from the port */
    int have_params;
} s_gl = { -1 };
static pthread_mutex_t s_gl_lock = PTHREAD_MUTEX_INITIALIZER;

static int eng_gpu_light_on(void)
{
    if (s_gl.on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_GPU_LIGHT");
        s_gl.on = 0;
        if (e && *e) {
            char* d; s_gl.diff_off = (u32)strtoul(e, &d, 16);
            if (*d == ':') { s_gl.spec_off = (u32)strtoul(d + 1, 0, 16); s_gl.on = 1; }
        }
        if (s_gl.on) fprintf(stderr, "[rsx engine] GPU light pass: diffuse 0x%08X specular 0x%08X\n",
                             s_gl.diff_off, s_gl.spec_off);
    }
    return s_gl.on && g.be && g.be->light_pass && g.be->color_snapshot && g.be->depth_snapshot;
}

static float eng_half(const u8* p)
{
    const u32 h = ((u32)p[0] << 8) | p[1];
    const u32 s = h >> 15, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0 ? (float)m / 1024.0f / 16384.0f
            : e == 31 ? 65504.0f
            : ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
    return s ? -v : v;
}
static float eng_bef(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    float f; memcpy(&f, &w, 4); return f;
}

void rsx_draw_engine_gpu_light_data(const void* frame_be, const void* lights_be, u32 count)
{
    const u8* f = (const u8*)frame_be;
    const u8* l = (const u8*)lights_be;
    if (!f || (!l && count)) return;
    if (count > RSX_GPU_LIGHT_MAX) count = RSX_GPU_LIGHT_MAX;
    rsx_gpu_light_params p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 16; i++) p.m[i] = eng_bef(f + 0x10 + i * 4);
    p.scale = eng_bef(f + 0x98);
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        const u8* r = l + i * 0x30;
        const u32 type = ((u32)r[0x14] << 24) | ((u32)r[0x15] << 16) | ((u32)r[0x16] << 8) | r[0x17];
        if (type != 1 && type != 2) continue;      /* the job ignores any other type */
        p.light[n].pos[0] = eng_bef(r + 0); p.light[n].pos[1] = eng_bef(r + 4); p.light[n].pos[2] = eng_bef(r + 8);
        p.light[n].type = (float)type;
        p.light[n].col[0] = eng_half(r + 0x0C); p.light[n].col[1] = eng_half(r + 0x0E); p.light[n].col[2] = eng_half(r + 0x10);
        p.light[n].inner  = eng_half(r + 0x20); p.light[n].outer  = eng_half(r + 0x22);
        p.light[n].cone_a = eng_half(r + 0x24); p.light[n].cone_b = eng_half(r + 0x26);
        p.light[n].dir[0] = eng_half(r + 0x28); p.light[n].dir[1] = eng_half(r + 0x2A); p.light[n].dir[2] = eng_half(r + 0x2C);
        p.light[n].near_  = eng_half(r + 0x2E);
        n++;
    }
    p.count = (float)n;
    pthread_mutex_lock(&s_gl_lock);
    s_gl.params = p;
    s_gl.have_params = 1;
    pthread_mutex_unlock(&s_gl_lock);
}

/* At an NV3089 blit: is it one of the light job's inputs? Then take a GPU copy
 * of its source; returns 1 when the guest write-back can be skipped too. */
static int eng_gpu_light_blit(u32 location, u32 offset, u32 pitch, int dst_local, u32 dst_offset)
{
    if (dst_local != 0 || !eng_gpu_light_on()) return 0;
    const int is_n = dst_offset >= s_gl.diff_off && dst_offset < s_gl.diff_off + pitch;
    const int is_z = dst_offset >= s_gl.spec_off && dst_offset < s_gl.spec_off + pitch;
    if (!is_n && !is_z) return 0;
    if (is_n && dst_offset == s_gl.diff_off) {
        u32 best = ENG_INVALID;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const eng_surface* sf = &g.surfaces[i];
            if (!sf->handle || sf->location != location || sf->fmt != RSX_BE_FMT_R8G8B8A8) continue;
            const u32 row = pitch ? pitch : sf->w * 4u;
            if (offset < sf->offset || offset >= sf->offset + row) continue;
            if (best == ENG_INVALID || sf->stamp > g.surfaces[best].stamp) best = i;
        }
        if (best == ENG_INVALID) return 0;
        const u32 snap = g.be->color_snapshot(g.be->user, g.surfaces[best].handle);
        if (!snap) return 0;
        s_gl.n_tex = snap; s_gl.w = g.surfaces[best].w; s_gl.h = g.surfaces[best].h;
        s_gl.have |= 1u;
    }
    if (is_z && dst_offset == s_gl.spec_off) {
        for (u32 i = 0; i < g.n_zdepths; i++) {
            eng_zdepth* z = &g.zdepths[i];
            if (!z->handle || z->location != location) continue;
            const u32 row = pitch ? pitch : z->w * 4u;
            if (offset < z->offset || offset >= z->offset + row) continue;
            const u32 snap = g.be->depth_snapshot(g.be->user, z->handle, z->w, z->h);
            if (!snap) return 0;
            if (s_gl.z_tex && g.be->texture_release) g.be->texture_release(g.be->user, s_gl.z_tex);
            s_gl.z_tex = snap;
            s_gl.have |= 2u;
            break;
        }
    }
    if (s_gl.have == 3u) { s_gl.have = 0; s_gl.in_seq++; }
    /* The copies are read by more than the light job (the SSAO job reads all
     * 720 depth rows, and runs with the light pass on hung after melee hits
     * when the normals copy was left stale), so both are still written back;
     * PS3RECOMP_ENG_GPU_LIGHT_NOWB=1 skips the normals one (A/B). */
    /* With the SSAO on the GPU too (PS3RECOMP_ENG_GPU_SSAO), no SPU job reads
     * either copy any more, so neither needs the flush and the readback. */
    { static int nowb = -1, ssao = -1;
      if (nowb < 0) nowb = getenv("PS3RECOMP_ENG_GPU_LIGHT_NOWB") ? 1 : 0;
      if (ssao < 0) { const char* e = getenv("PS3RECOMP_ENG_GPU_SSAO"); ssao = (e && *e) ? 1 : 0; }
      return ssao || (nowb && is_n); }
}

/* At an NV3089 blit: may the guest-memory copy itself be skipped? With both GPU
 * passes on, eng_gpu_light_blit already skips the write-back of the source
 * target, so the blit would copy the same stale VRAM bytes into the light job's
 * two buffers every frame -- 2 x 3.6 MB of memmove on the RSX thread (~9% of it
 * in the docks walk, 2026-11-08 ETW profile) -- for buffers that no SPU job
 * reads any more and whose texture units the engine binds to the GPU outputs.
 * PS3RECOMP_ENG_GPU_LIGHT_SKIPCOPY=0 keeps the copy. */
/* set by rsx_draw_engine_write_back_to: its last call skipped the write-back for a light input */
static int s_gl_wb_skipped;
int rsx_draw_engine_gpu_light_dst_dead(int dst_local, u32 dst_offset, u32 pitch)
{
    static int on = -1, ssao = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_GPU_LIGHT_SKIPCOPY"); on = e ? atoi(e) : 1; }
    if (ssao < 0) { const char* e = getenv("PS3RECOMP_ENG_GPU_SSAO"); ssao = (e && *e) ? 1 : 0; }
    if (!on || !ssao || !s_gl_wb_skipped || dst_local != 0 || !g.ready || !eng_gpu_light_on()) return 0;
    return (dst_offset >= s_gl.diff_off && dst_offset < s_gl.diff_off + pitch) ||
           (dst_offset >= s_gl.spec_off && dst_offset < s_gl.spec_off + pitch);
}

/* ---- GPU SSAO (PS3RECOMP_ENG_GPU_SSAO=<local offset of the AO texture>) ----
 * Same idea for the title's SSAO job: the port hands over the constants of its
 * passes, the backend runs them from the light path's depth and normals
 * copies, and the texture unit that samples the AO buffer gets the result. */
static struct {
    int on;
    u32 off;
    u8 consts[512]; u32 bytes, fw, fh, hw, hh;
    int have;
    u32 out, out_seq;
} s_gs = { -1 };

void rsx_draw_engine_gpu_ssao_data(const void* consts, u32 bytes, u32 full_w, u32 full_h,
                                   u32 half_w, u32 half_h)
{
    if (!consts || !bytes || bytes > sizeof s_gs.consts) return;
    pthread_mutex_lock(&s_gl_lock);
    memcpy(s_gs.consts, consts, bytes);
    s_gs.bytes = bytes; s_gs.fw = full_w; s_gs.fh = full_h; s_gs.hw = half_w; s_gs.hh = half_h;
    s_gs.have = 1;
    pthread_mutex_unlock(&s_gl_lock);
}

static u32 eng_gpu_ssao_texture(const rsx_dsp_texture* t)
{
    if (s_gs.on < 0) {
        const char* e = getenv("PS3RECOMP_ENG_GPU_SSAO");
        s_gs.on = (e && *e) ? 1 : 0;
        if (s_gs.on) { s_gs.off = (u32)strtoul(e, 0, 16);
                       fprintf(stderr, "[rsx engine] GPU SSAO: AO texture at local 0x%08X\n", s_gs.off); }
    }
    if (!s_gs.on || t->location != RSX_LOCATION_LOCAL || t->offset != s_gs.off) return 0;
    if (!eng_gpu_light_on() || !g.be->ssao_pass || !s_gl.in_seq || !s_gl.n_tex || !s_gl.z_tex) return 0;
    if (s_gs.out_seq != s_gl.in_seq) {
        u8 c[512]; u32 bytes, fw, fh, hw, hh;
        pthread_mutex_lock(&s_gl_lock);
        const int have = s_gs.have;
        memcpy(c, s_gs.consts, sizeof c); bytes = s_gs.bytes;
        fw = s_gs.fw; fh = s_gs.fh; hw = s_gs.hw; hh = s_gs.hh;
        pthread_mutex_unlock(&s_gl_lock);
        if (!have) return 0;
        const u32 out = g.be->ssao_pass(g.be->user, s_gl.z_tex, s_gl.n_tex, c, bytes, fw, fh, hw, hh);
        if (!out) return 0;
        s_gs.out = out; s_gs.out_seq = s_gl.in_seq;
    }
    return s_gs.out;
}

/* The texture a composite unit gets instead of the guest buffer, or 0. */
static u32 eng_gpu_light_texture(const rsx_dsp_texture* t)
{
    if (t->location != RSX_LOCATION_MAIN || !eng_gpu_light_on()) return 0;
    const int want_spec = t->offset == s_gl.spec_off;
    if (t->offset != s_gl.diff_off && !want_spec) return 0;
    if (!s_gl.in_seq || !s_gl.n_tex || !s_gl.z_tex) return 0;
    if (s_gl.out_seq != s_gl.in_seq) {
        rsx_gpu_light_params p;
        pthread_mutex_lock(&s_gl_lock);
        const int have = s_gl.have_params;
        p = s_gl.params;
        pthread_mutex_unlock(&s_gl_lock);
        if (!have) return 0;
        p.width = (float)s_gl.w; p.height = (float)s_gl.h;
        u32 spec = 0;
        const u32 diff = g.be->light_pass(g.be->user, s_gl.n_tex, s_gl.z_tex, s_gl.w, s_gl.h,
                                          &p, (u32)sizeof p, &spec);
        if (!diff || !spec) return 0;
        s_gl.out_diff = diff; s_gl.out_spec = spec; s_gl.out_seq = s_gl.in_seq;
    }
    const u32 tex = want_spec ? s_gl.out_spec : s_gl.out_diff;
    const u32 view = g.be->surface_view ? g.be->surface_view(g.be->user, tex, t->remap & 0xFFFFu, t->format) : 0;
    return view ? view : tex;
}

static u32 eng_fp_tex_mask(void)
{
    const eng_fp_memo* fp = NULL;
    if (!eng_guest_programs(NULL, &fp)) return 0xFFFFu;
    return fp->tex_mask;
}

/* PS3RECOMP_ENG_FBFETCH: is unit u (sampling the draw's own target
 * target_slots[k] == own_slot) one the pipeline may read from attachment k?
 * Records it in s_fbf and returns 1 (the unit stays unbound for now). */
static int eng_fbf_candidate(u32 u, const rsx_dsp_texture* t, const u32* target_slots,
                             u32 n_targets, u32 own_slot)
{
    int mode = __atomic_load_n(&s_fbf_mode, __ATOMIC_RELAXED);
    { static int watched; if (!watched) { watched = 1; ps3_toggle_watch(PS3T_ENG_FBFETCH, eng_fbf_set);
                                          mode = __atomic_load_n(&s_fbf_mode, __ATOMIC_RELAXED); } }
    /* Unset: on when the targets are scaled on either axis (PS3_RES_SCALE,
     * PS3_ASPECT) -- rsx_fp_wpos_scale() is 1 only when both axes are. */
    if (mode < 0) mode = rsx_fp_wpos_scale() != 1.0f;
    if (!mode || !g.be->surface_view || u >= 16 || t->cubemap || t->dimension != 2) return 0;
    u32 k = 0;
    while (k < n_targets && target_slots[k] != own_slot) k++;
    if (k >= n_targets || k > 3) return 0;
    s_fbf.cand |= 1u << u;
    s_fbf.att[u] = (u8)k;
    rsx_texture_component_remap(t->remap & 0xFFFFu, t->format & 0x9Fu, s_fbf.sel[u]);
    s_fbf.slot[u] = own_slot; s_fbf.remap[u] = t->remap & 0xFFFFu; s_fbf.fmt[u] = t->format;
    return 1;
}

/* After eng_pipeline_get: the candidates the program did not let read the
 * attachment get the copy, as before. */
static void eng_fbf_fallback(u32 textures[RSX_BE_MAX_TEXTURES])
{
    const u32 left = s_fbf.cand & ~s_fbf.applied;
    if (!left) return;
    for (u32 u = 0; u < 16; u++) {
        if (!((left >> u) & 1u)) continue;
        const u32 snap = g.be->color_snapshot(g.be->user, g.surfaces[s_fbf.slot[u]].handle);
        if (!snap) continue;
        const u32 view = g.be->surface_view(g.be->user, snap, s_fbf.remap[u], s_fbf.fmt[u]);
        textures[u] = view ? view : snap;
    }
}

/* The depth-bounds test (NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE 0x0380, MIN
 * 0x0384, MAX 0x0388): a fragment survives only where the depth ALREADY
 * STORED at its pixel lies in [min, max] -- the fragment's own depth plays no
 * part, and the depth test need not be on (RPCS3 binds the zeta for it even
 * then). inFAMOUS 2 bounds its sun-visibility mask, one full-screen triangle
 * per shadow cascade (fp 55295a0f6cc1985b; [0, .987], [.986, .996], [.995, 1]
 * in New Marais), and every light volume and occlusion volume to the depth
 * span of its box. Ignored, those passes reached every pixel their geometry
 * covers: shadow and light shapes far from where they belong.
 *
 * Metal has no usable depth bounds (setDepthTestMinBound:maxBound: is macOS
 * 26+ and traps on Apple M4 GPUs), so the fragment program does the test:
 * the depth buffer's 24-bit copy -- the A8R8G8B8 one the light and occlusion
 * volumes already read, so no extra resolve (an R32F copy cost ~9% of the
 * dusk walk's frame rate) -- goes on a texture unit the program does not sample
 * (rsx_fp_apply_depth_bounds reads it at the pixel and discards), the bounds
 * on the float4 after fp_alpha. A bound of [0, 1] is no bound. Returns the
 * texture mask with that unit added; s_dbz says what eng_pipeline_get and the
 * constants need. PS3RECOMP_ENG_DEPTH_BOUNDS=0 (live) ignores the test, as
 * before. PS3RECOMP_ENG_DBOUNDS_LOG=1: each distinct (fp, min, max), once. */
static u32 eng_depth_bounds_prepare(u32 zslot, const rsx_be_render_state* rs, u32 target, u32 n_verts,
                                    u32 tex_mask, u32 textures[RSX_BE_MAX_TEXTURES],
                                    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    s_dbz.unit = 0;
    if (__builtin_expect(__atomic_load_n(&s_dbounds_on, __ATOMIC_RELAXED) < 0, 0))
        ps3_toggle_watch(PS3T_ENG_DEPTH_BOUNDS, eng_dbounds_set);
    if (!(rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_ENABLE) & 1u) || zslot == ENG_INVALID ||
        !__atomic_load_n(&s_dbounds_on, __ATOMIC_RELAXED))
        return tex_mask;
    float mn, mx;
    { const u32 a = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN), b = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
      memcpy(&mn, &a, 4); memcpy(&mx, &b, 4); }
    /* Saturated into the depth format's range, as the hardware does (RPCS3
     * clamp_depth_bounds_value); NaN reads as 0. */
    mn = mn > 0.0f ? (mn < 1.0f ? mn : 1.0f) : 0.0f;
    mx = mx > 0.0f ? (mx < 1.0f ? mx : 1.0f) : 0.0f;
    if (mn <= 0.0f && mx >= 1.0f) return tex_mask;
    const eng_fp_memo* fpm = NULL;
    if (!eng_guest_programs(NULL, &fpm) || !fpm) return tex_mask;
    { static int lg = -1; if (lg < 0) lg = getenv("PS3RECOMP_ENG_DBOUNDS_LOG") ? 1 : 0;
      if (lg) {
          static u64 seen[512]; static u32 ns;
          u32 a, b; memcpy(&a, &mn, 4); memcpy(&b, &mx, 4);
          const u64 key = fpm->structural ^ ((u64)a << 32) ^ b;
          u32 k = 0; while (k < ns && seen[k] != key) k++;
          if (k == ns && ns < 512) { seen[ns++] = key;
              fprintf(stderr, "[dbounds] frame %u fp %016llx min %.7f max %.7f ztest %u zwrite %u target 0x%08X %ux%u verts %u\n",
                      g.frames, (unsigned long long)fpm->structural, mn, mx, rs->depth_test, rs->depth_write,
                      g.surfaces[target].offset, g.surfaces[target].w, g.surfaces[target].h, n_verts); } } }
    /* A unit the program neither samples nor has declared as a cube or a
     * volume (rsx_fp_apply_depth_bounds reads it as a Texture2D). */
    rsx_dsp_texture tex[RSX_DSP_NUM_TEXTURES];
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) rsx_dsp_get_texture(&g.rsx, u, &tex[u]);
    const u32 busy = fpm->sampled_units | eng_cube_mask(tex) | eng_volume_mask(tex);
    int unit = -1;
    for (int u = RSX_BE_MAX_TEXTURES - 1; u >= 0 && unit < 0; u--) if (!((busy >> u) & 1u)) unit = u;
    /* The copy of what the passes so far stored; a depth-bounded draw that
     * also writes depth is tested against the depth before it, as one draw
     * on the hardware is. No copy (nothing drawn since the clear): unbounded. */
    const u32 snap = unit >= 0 ? eng_zdepth_snapshot_rgba8(zslot) : 0;
    if (!snap) {
        static u32 said;
        if (said++ < 4) fprintf(stderr, "[rsx engine] depth bounds %.4f..%.4f not applied (fp %016llx: %s)\n",
                                mn, mx, (unsigned long long)fpm->structural, unit < 0 ? "no free texture unit" : "no depth copy");
        return tex_mask;
    }
    textures[unit] = snap;
    memset(&samplers[unit], 0, sizeof samplers[unit]);
    s_dbz.unit = (u32)unit + 1u; s_dbz.mn = mn; s_dbz.mx = mx;
    return tex_mask | (1u << unit);
}

static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    u32 mask = 0;
    s_fbf.cand = 0; s_fbf.applied = 0;
    const u32 fp_reads = eng_bind_unread() ? 0xFFFFu : eng_fp_tex_mask();
    const rsx_dsp_texture* units = eng_draw_textures();
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        rsx_dsp_texture t = units[u];
        /* A unit left enabled by an earlier draw but not sampled by this
         * program is skipped (as upstream's sink_bind_textures does): binding
         * it costs a cache check and a hash, and a unit left on the depth
         * buffer a depth resolve. Since each depth write invalidates the
         * resolved copy, inFAMOUS 2's forward materials (FP 3e92fd7ffdfc0f5f,
         * tex0 x constant, depth writes on, a stale unit on the zeta) paid a
         * render-pass break and a full-screen decode per draw: ~1000 a frame,
         * ~45 ms of GPU. */
        if (!t.enabled || !(fp_reads >> u & 1u)) continue;
        mask |= 1u << u;
        eng_decode_sampler(t.filter, t.wrap, t.control0, &samplers[u]);
        samplers[u].border = eng_border_class(t.format, t.border_color);
        /* Wrap modes that read SET_TEXTURE_BORDER_COLOR (4 BORDER, 5 CLAMP,
         * 7 MIRROR_ONCE_BORDER, 8 MIRROR_ONCE_CLAMP) with a colour the
         * backends cannot express (they sample one of transparent black,
         * opaque black, opaque white): each distinct (program, unit, format,
         * wrap, border) is reported once. PS3RECOMP_ENG_BORDERLOG=0 silences,
         * =2 also reports the expressible ones. */
        { static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_BORDERLOG"); on = e ? atoi(e) : 1; }
          const u32 ws = t.wrap & 0xFu, wt = (t.wrap >> 8) & 0xFu, wr = (t.wrap >> 16) & 0xFu;
          #define ENG_WRAP_USES_BORDER(w) ((w) == 4u || (w) == 5u || (w) == 7u || (w) == 8u)
          const u32 b = t.border_color, cls = samplers[u].border;
          const int exact = cls == 1 || cls == 2 ||
                            (cls == 0 && (b == 0 || ((t.format & 0x9Fu) >= 0x90u && (t.format & 0x9Fu) <= 0x93u && !(b & 0xFFFFFFu))));
          if (on && (!exact || on >= 2) && (ENG_WRAP_USES_BORDER(ws) || ENG_WRAP_USES_BORDER(wt) ||
                     (t.dimension == 3 && ENG_WRAP_USES_BORDER(wr)))) {
              static u64 seen[64]; static u32 ns;
              const u64 fk = eng_current_fp_structural();
              const u64 key = fk ^ ((u64)u << 59) ^ ((u64)t.format << 48) ^ ((u64)(t.wrap & 0xF0F0Fu) << 20) ^ b;
              u32 k = 0; while (k < ns && seen[k] != key) k++;
              if (k == ns && ns < 64) { seen[ns++] = key;
                  fprintf(stderr, "[rsx engine] border wrap: fp %016llx unit %u fmt 0x%02X %ux%u addr 0x%08X border 0x%08X -> %s\n",
                          (unsigned long long)fk, u, t.format, t.width, t.height, t.wrap, b,
                          cls == 2 ? "opaque white" : cls == 1 ? "opaque black" : cls == 3 ? "edge clamp (INEXACT)" : exact ? "transparent black" : "transparent black (INEXACT)"); } }
          #undef ENG_WRAP_USES_BORDER
        }
        /* PS3RECOMP_ENG_SIGNLOG=1: units bound with SET_TEXTURE_FILTER's
         * signed bits (a,r,g,b at [28..31]) or SET_TEXTURE_ADDRESS's
         * UNSIGNED_REMAP_BIASED (bit 12), once per (unit, format, address). */
        { static int on = -1; if (on < 0) on = getenv("PS3RECOMP_ENG_SIGNLOG") ? 1 : 0;
          if (on && ((t.filter >> 28) || ((t.wrap >> 12) & 1u))) {
              static u64 seen[256]; static u32 ns;
              const u64 key = ((u64)u << 56) ^ ((u64)(t.filter >> 28) << 48) ^ ((u64)((t.wrap >> 12) & 1u) << 44) ^ ((u64)t.format << 32) ^ t.offset;
              u32 k = 0; while (k < ns && seen[k] != key) k++;
              if (k == ns && ns < 256) { seen[ns++] = key;
                  fprintf(stderr, "[sign] unit %u signed=0x%X biased=%u fmt 0x%02X %ux%u loc %u off 0x%08X remap 0x%08X filter 0x%08X addr 0x%08X\n",
                          u, t.filter >> 28, (t.wrap >> 12) & 1u, t.format, t.width, t.height, t.location, t.offset, t.remap, t.filter, t.wrap); } } }

        /* A depth-format texture over a depth buffer reads that depth buffer,
         * even when a colour target is bound at the same address too: a
         * depth-only pass (inFAMOUS 2's sun shadow cascades) leaves a dummy
         * colour target there with its colour mask off. The colour search
         * below took that never-written target first, so the shadow maps read
         * as 0, every compare passed, the sun-visibility mask (display buffer
         * R) came out 1 everywhere, and every surface facing away from the sun
         * got the composite's unclamped sun x N.L -- negative -- on top of its
         * ambient: the black facades and dark Cole at dusk.
         * PS3RECOMP_ENG_DEPTH_TEX_FIRST=0 restores the old order. */
        { static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_DEPTH_TEX_FIRST"); on = e ? atoi(e) : 1; }
          const u32 zb = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
          if (on && zb >= RSX_TEX_FMT_DEPTH24_D8 && zb <= RSX_TEX_FMT_DEPTH24_D8 + 3u) {
              for (u32 i = 0; i < g.n_zdepths; i++)
                  if (g.zdepths[i].handle && g.zdepths[i].location == t.location &&
                      g.zdepths[i].offset == t.offset && current_zslot != i) {
                      const u32 snap = eng_zdepth_snapshot_sized(i, t.width, t.height);
                      if (snap) textures[u] = snap;
                      break;
                  }
              if (textures[u]) continue;
          } }
        /* The same for a colour-format read (A8R8G8B8 / the 0x9E variant) of a
         * depth buffer that a colour target shares the address with: the
         * memory holds whichever was written last. inFAMOUS 2's menu builds a
         * half-resolution depth (0x0F770000) by depth export with a dummy
         * colour target there (colour mask 0), then its lit particles -- the
         * debris round Cole -- test themselves against those depth bytes by
         * hand. The colour search below took the never-written target, the
         * depth came out 0 and every debris particle discarded itself. A depth
         * buffer written after the colour target was last bound wins.
         * PS3RECOMP_ENG_DEPTH_COLOR_NEWEST=0 restores colour-first. */
        { static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_DEPTH_COLOR_NEWEST"); on = e ? atoi(e) : 1; }
          const u32 cb = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
          if (on && (cb == 0x85u || cb == 0x9Eu)) {
              for (u32 i = 0; i < g.n_zdepths; i++) {
                  const eng_zdepth* z = &g.zdepths[i];
                  if (!z->handle || !z->had_write || z->location != t.location || z->offset != t.offset) continue;
                  int newest = 1;
                  for (u32 k = 0; k < g.n_surfaces && newest; k++)
                      if (g.surfaces[k].handle && g.surfaces[k].location == t.location &&
                          g.surfaces[k].offset == t.offset && g.surfaces[k].stamp > z->stamp) newest = 0;
                  if (newest) {
                      const u32 snap = eng_zdepth_snapshot_rgba8_sized(i, t.width, t.height);
                      if (snap) {
                          const u32 view = g.be->surface_view
                              ? g.be->surface_view(g.be->user, snap, t.remap & 0xFFFFu, t.format) : 0;
                          textures[u] = view ? view : snap;
                          static u32 seen[16]; static u32 ns;
                          u32 k = 0; while (k < ns && seen[k] != (t.offset ^ u)) k++;
                          if (k == ns && ns < 16) { seen[ns++] = t.offset ^ u;
                              fprintf(stderr, "[rsx engine] unit %u (fmt 0x%02X %ux%u) at loc %u 0x%08X reads the depth buffer"
                                              " written after the colour target there (fp %016llx, depth %ux%u)\n",
                                      u, t.format, t.width, t.height, t.location, t.offset,
                                      (unsigned long long)eng_current_fp_structural(), z->w, z->h); }
                      }
                  }
                  break;
              }
              if (textures[u]) continue;
          } }
        /* Several targets can share an address (one per size/format). Take
         * the one whose size matches the texture, else the newest; never one
         * this draw writes -- reading a target a pass is writing is undefined
         * on every API, and an MRT set writes more than one of them. */
        int sampled = -1, own_hit = 0, own_slot = -1;
        /* eng_surface_maybe: no target at this address at all -> the search
         * below finds nothing (most units sample plain textures). */
        if (eng_surface_maybe(t.location, t.offset))
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (!g.surfaces[i].handle || g.surfaces[i].location != t.location ||
                g.surfaces[i].offset != t.offset)
                continue;
            int own = 0;
            for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
            if (own) {
                own_hit = 1;
                if (g.surfaces[i].w == t.width && g.surfaces[i].h == t.height) own_slot = (int)i;
                continue;
            }
            const int fits = g.surfaces[i].w == t.width && g.surfaces[i].h == t.height;
            if (sampled < 0) { sampled = (int)i; continue; }
            const int best_fits = g.surfaces[sampled].w == t.width && g.surfaces[sampled].h == t.height;
            if (fits > best_fits || (fits == best_fits && g.surfaces[i].stamp > g.surfaces[sampled].stamp))
                sampled = (int)i;
        }
        if (own_hit && sampled >= 0 && !(g.surfaces[sampled].w == t.width && g.surfaces[sampled].h == t.height))
            sampled = -1;   /* only a stale alias of the target being written */
        /* A read-modify-write pass samples the very target it draws into;
         * bind a copy taken just before this draw instead of guest memory,
         * which never sees GPU-rendered pixels. */
        if (sampled < 0 && own_slot >= 0 && g.be->color_snapshot && (eng_fp_tex_mask() >> u & 1u)) {
            /* PS3RECOMP_ENG_FBFETCH: read the attachment itself if the
             * program allows (decided with the pipeline; the copy is taken
             * after eng_pipeline_get otherwise -- eng_fbf_fallback). */
            if (eng_fbf_candidate(u, &t, target_slots, n_targets, (u32)own_slot)) continue;
            const u32 snap = g.be->color_snapshot(g.be->user, g.surfaces[own_slot].handle);
            /* PS3RECOMP_ENG_CSNAP_LOG=1: which programs copy their own target
             * (first 400 such binds in world frames), with scissor and mask. */
            { static int on = -1, n; if (on < 0) on = getenv("PS3RECOMP_ENG_CSNAP_LOG") ? 1 : 0;
              if (on && n < 400 && g.guest_draws > 1000) { n++;
                  const u32 sh = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), sv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
                  fprintf(stderr, "[csnap] fp %016llx unit %u target 0x%08X %ux%u scissor %u,%u %ux%u cmask 0x%08X\n",
                          (unsigned long long)eng_current_fp_structural(), u, g.surfaces[own_slot].offset,
                          g.surfaces[own_slot].w, g.surfaces[own_slot].h, sh & 0xFFFFu, sv & 0xFFFFu, sh >> 16, sv >> 16,
                          rsx_dsp_reg(&g.rsx, M_COLOR_MASK)); } }
            if (snap) {
                const u32 view = g.be->surface_view
                    ? g.be->surface_view(g.be->user, snap, t.remap & 0xFFFFu, t.format)
                    : 0;
                textures[u] = view ? view : snap;
                continue;
            }
        }
        if (sampled >= 0) {
            const u32 view = g.be->surface_view
                ? g.be->surface_view(g.be->user, g.surfaces[sampled].handle,
                                     t.remap & 0xFFFFu, t.format)
                : 0;
            textures[u] = view ? view : g.surfaces[sampled].handle;
            /* PS3RECOMP_ENG_TEXLOG=1 also names the first 300 render-target
             * reads: which target a unit's address resolved to, and whether
             * the texture's size matched it. */
            { static int on = -1; static int n;
              if (on < 0) on = getenv("PS3RECOMP_ENG_TEXLOG") ? 1 : 0;
              if (on && n < 300) { n++;
                  fprintf(stderr, "[rsx texlog] draw into 0x%08X: unit %u fmt 0x%02X %ux%u remap 0x%04X loc %u off 0x%08X -> surface %ux%u h%u view h%u%s\n",
                          n_targets ? g.surfaces[target_slots[0]].offset : 0u, u, t.format, t.width, t.height,
                          t.remap & 0xFFFFu, t.location, t.offset, g.surfaces[sampled].w, g.surfaces[sampled].h,
                          g.surfaces[sampled].handle, view,
                          (g.surfaces[sampled].w == t.width && g.surfaces[sampled].h == t.height) ? "" : " SIZE MISMATCH"); } }
            continue;
        }
        const u32 base_fmt = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
        { static int dbg = -1; if (dbg < 0) dbg = getenv("PS3RECOMP_ENG_ZTEX_LOG") ? 1 : 0;
          if (dbg) for (u32 i = 0; i < g.n_zdepths; i++)
              if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) {
                  static u32 seen[32]; static u32 ns;
                  const u32 key = (u << 16) ^ t.format ^ (t.remap << 8);
                  u32 k = 0; while (k < ns && seen[k] != key) k++;
                  if (k == ns && ns < 32) { seen[ns++] = key;
                      fprintf(stderr, "[rsx engine] depth-address texture unit=%u fmt=0x%02X base=0x%02X remap=0x%04X %ux%u zcur=%d\n",
                              u, t.format, base_fmt, t.remap & 0xFFFF, t.width, t.height, current_zslot == i); }
              } }
        if (base_fmt == RSX_TEX_FMT_DEPTH24_D8) {
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset && current_zslot != i) {
                    const u32 snap = eng_zdepth_snapshot_sized(i, t.width, t.height);
                    if (snap) textures[u] = snap;
                    break;
                }
            if (textures[u]) continue;
        }
        /* A colour format over a depth buffer reads the depth bytes (fog,
         * depth-of-field, SSAO); the copy reflects depth before this draw, so
         * the current zeta qualifies too. */
        if (base_fmt == 0x85u || base_fmt == 0x9Eu) {
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) {
                    const u32 snap = eng_zdepth_snapshot_rgba8_sized(i, t.width, t.height);
                    if (snap) {
                        const u32 view = g.be->surface_view
                            ? g.be->surface_view(g.be->user, snap, t.remap & 0xFFFFu, t.format)
                            : 0;
                        textures[u] = view ? view : snap;
                    }
                    break;
                }
            if (textures[u]) continue;
        }
        /* CONTROL1's high word is the remap override the two-channel 16-bit
         * formats read (rsx_texture_component_remap); it rides along in the
         * cache key like the crossbar itself. */
        if ((textures[u] = eng_gpu_light_texture(&t)) != 0) continue;
        if ((textures[u] = eng_gpu_ssao_texture(&t)) != 0) continue;
        textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, t.remap & 0x1FFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
        /* PS3RECOMP_ENG_RES_LOG=1: guest-memory textures of a screen-buffer
         * size (the display's, half, quarter) -- under PS3_RES_SCALE these
         * stay at the guest's resolution while the targets are scaled. Once
         * per (program, unit, address). */
        { static int on = -1; if (on < 0) on = getenv("PS3RECOMP_ENG_RES_LOG") ? 1 : 0;
          if (on && n_targets && t.width && t.height &&
              ((t.width == g.width && t.height == g.height) || (t.width * 2 == g.width && t.height * 2 == g.height) ||
               (t.width * 4 == g.width && t.height * 4 == g.height))) {
              static u64 seen[256]; static u32 ns;
              const u64 fk = eng_current_fp_structural();
              const u64 key = fk ^ ((u64)u << 59) ^ ((u64)t.location << 58) ^ t.offset;
              u32 k = 0; while (k < ns && seen[k] != key) k++;
              if (k == ns && ns < 256) { seen[ns++] = key;
                  fprintf(stderr, "[res-log] fp %016llx unit %u guest texture loc %u off 0x%08X fmt 0x%02X %ux%u filter 0x%08X"
                                  " (draw into 0x%08X %ux%u)\n",
                          (unsigned long long)fk, u, t.location, t.offset, t.format, t.width, t.height, t.filter,
                          g.surfaces[target_slots[0]].offset, g.surfaces[target_slots[0]].w, g.surfaces[target_slots[0]].h); } } }
        /* PS3RECOMP_ENG_TEXLOG_RT=<hex>: guest-memory bindings of draws into
         * colour target A at that offset (first 40). */
        { static long trt = -2; static int n;
          if (trt == -2) { const char* e = getenv("PS3RECOMP_ENG_TEXLOG_RT"); trt = e ? (long)strtoul(e, 0, 16) : -1; }
          if (trt >= 0 && n < 40 && u == 2 && n_targets && g.surfaces[target_slots[0]].offset == (u32)trt) { n++;
              fprintf(stderr, "[rsx texlog] unit %u guest loc %u off 0x%08X fmt 0x%02X %ux%u mips %u pitch %u cube %d remap 0x%04X -> h%u\n",
                      u, t.location, t.offset, t.format, t.width, t.height, t.mipmaps, t.pitch, (int)t.cubemap,
                      t.remap & 0xFFFF, textures[u]); } }
    }
    { static long trt = -2; static int n;
      if (trt == -2) { const char* e = getenv("PS3RECOMP_ENG_TEXLOG_RT"); trt = e ? (long)strtoul(e, 0, 16) : -1; }
      if (trt >= 0 && n < 60 && g.rsx.pair_packets == 0 && n_targets && g.surfaces[target_slots[0]].offset == (u32)trt && (mask & 4u)) {
          rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, 2, &t); n++;
          fprintf(stderr, "[rsx texlog2] unit 2 loc %u off 0x%08X fmt 0x%02X %ux%u -> h%u\n",
                  t.location, t.offset, t.format, t.width, t.height, textures[2]); } }
    return mask;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES])
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, &samplers[u]);
        samplers[u].border = eng_border_class(vt.format, vt.border_color);
        /* A vertex unit has no crossbar, so its remap is the identity. */
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, 0, 0xAAE4u, 1u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

/* PS3RECOMP_ENG_DROPLOG=1: name each draw sink_end drops (first 6 per
 * reason), with the primitive, packet/vertex counts and target address. */
static void eng_drop(const char* why, u32 prim)
{
    static int on = -1; static u32 from;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_DROPLOG"); on = e ? 1 : 0; from = e ? (u32)atoi(e) : 0; }
    if (!on || g.frames < from) return;   /* =<frame>: start counting there */
    static struct { const char* why; u32 n; } seen[16];
    u32 k = 0;
    while (k < 16 && seen[k].why && seen[k].why != why) k++;
    if (k == 16) return;
    seen[k].why = why;
    if (seen[k].n++ >= 6) return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    fprintf(stderr, "[rsx engine] drop #%u (%s): prim=%u packets=%u arr=%u idx=%u verts=%u fetch_ok=%d"
                    " rt=0x%X clip=%ux%u frame=%u\n", seen[k].n, why, prim, dc.n_packets, dc.n_arr,
            dc.n_idx, dc.n_verts, dc.fetch_ok, sf.color_offset[0], sf.clip_w, sf.clip_h, g.frames);
}

static u32 uploaded_hud_n(u32 n_verts, u32 n_draw, int indexed) { return indexed ? n_verts : n_draw; }

/* PS3_ASPECT_HUD_AFTER (see sink_end): the composite programs, the frame the
 * last draw was in, a count of frames that drew ("slices"), the slice that
 * last drew a composite (0 none yet) and whether this one has. */
static u64 s_hud_fp[8];
static int s_hud_n = -1, s_hud_after;
static u32 s_hud_frame = ~0u, s_hud_slice, s_hud_seen;

static void sink_end(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (!g.ready) return;
    if (++s_draw_serial == 0) s_draw_serial = 1;
    const u32 prim = g.rsx.current_primitive;
    if (!dc.n_packets) { eng_drop("no_packets", prim); return; }

    /* Everything that becomes triangles -- lists, strips, fans, quads, quad
     * strips and polygons -- is REBUILT into one triangle list through an
     * index buffer. A host strip topology cannot express a restart cut, which
     * is the whole reason the reference engine rebuilds them, and rebuilding
     * is also what lets repeated references share an uploaded vertex. Points
     * and lines pass through as the guest issued them, and anything left that
     * would need an expansion this engine does not have is dropped rather
     * than drawn as something else. */
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (!rebuild) {
        if (rsx_primitive_needs_expansion(prim)) { eng_drop("needs_expansion", prim); return; }
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) { eng_drop("unsupported_topology", prim); return; }
    }

    rsx_vertex_layout_plan layout;
    const u64 ds_tl = eng_ds_enabled() ? eng_ds_now() : 0;
    eng_vertex_layout(&layout);
    if (ds_tl) ds.ns_layout += eng_ds_now() - ds_tl;
    dc_fetch(&layout, rebuild);
    const u64 ds_t0 = eng_ds_enabled() ? eng_ds_now() : 0;
    u64 ds_m = ds_t0;   /* stage marks within the rest of sink_end */
#define DS_MARK(field) do { if (ds_t0) { const u64 _t = eng_ds_now(); ds.field += _t - ds_m; ds_m = _t; } } while (0)
    if (!dc.n_verts || !dc.fetch_ok) { eng_drop("fetch", prim); return; }

    int indexed = 0;
    u32 n_draw = dc.n_source_refs;
    if (rebuild) {
        if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) { eng_drop("rebuild", prim); return; }
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                   dc.cuts, dc.n_cuts)
            : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    if (!n_draw) { eng_drop("n_draw0", prim); return; }
    /* The cache hands over a buffer of unique vertices for an indexed draw;
     * one that ended up non-indexed needs dc.verts after all. */
    if (dc.vbuf_hit && !indexed) {
        dc_refs_fill();
        if (!dc_reserve_verts((u64)dc.n_refs * dc.layout.stride) ||
            !rsx_vertex_fetch_all(&dc.fetch_plan, dc.refs, dc.n_refs, dc.verts)) {
            eng_drop("fetch", prim); return; }
        dc.vbuf = 0; dc.vbuf_hit = 0;
    }

    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);

    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) { eng_drop("no_targets", prim); return; }
    const u32 target = targets[0];

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);

    /* Textures before any pipeline or target binding: a cache refresh can
     * replace a resource, and the backend is free to submit while doing it. */
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    DS_MARK(ns_bind);   /* render state, targets, zeta: counted with binds */
    u32 tex_mask = sink_bind_textures(targets, n_targets, zslot,
                                      textures, samplers);
    tex_mask = eng_depth_bounds_prepare(zslot, &rs, target, dc.n_verts, tex_mask, textures, samplers);
    /* PS3RECOMP_ENG_TEXSTATE=1: for a full-screen draw (<= 4 vertices) into
     * a 1280-wide target, every enabled unit's raw state and the backend
     * handle it resolved to, once per fragment program -- which buffer a
     * composite really reads, by address, location, format and flags. */
    { static int on = -1; if (on < 0) on = getenv("PS3RECOMP_ENG_TEXSTATE") ? 1 : 0;
      if (on && n_targets && g.surfaces[targets[0]].w >= 1280 && dc.n_verts <= 4) {
          const eng_fp_memo* fpm = NULL;
          static u64 seen[64]; static u32 ns;
          const u64 fk = eng_guest_programs(NULL, &fpm) && fpm ? fpm->structural : 0;
          u32 k = 0; while (k < ns && seen[k] != fk) k++;
          if (k == ns && ns < 64) { seen[ns++] = fk;
              fprintf(stderr, "[texstate] fp=0x%016llX draw into 0x%08X (h%u) verts=%u\n",
                      (unsigned long long)fk, g.surfaces[targets[0]].offset, g.surfaces[targets[0]].handle, dc.n_verts);
              for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
                  rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
                  if (!t.enabled) continue;
                  fprintf(stderr, "[texstate]   unit %2u -> h%-5u loc %u off 0x%08X fmt 0x%02X %ux%u mips %u pitch %u cube %u remap 0x%08X filter 0x%08X ctl0 0x%08X addr 0x%08X\n",
                          u, textures[u], t.location, t.offset, t.format, t.width, t.height, t.mipmaps, t.pitch,
                          t.cubemap, t.remap, t.filter, t.control0, t.wrap);
              } } } }

    /* PS3RECOMP_ENG_SKIP_FP=<hex>[,<hex>...]: drop draws whose fragment
     * program has that structural hash (TEXSTATE's fp=). Pipeline numbers
     * drift between runs with asset streaming; the hash does not. */
    { static u64 skipfp[32]; static int nskip = -1;
      /* Re-read when PS3_TOGGLE_FILE changes the list (eng_skip_fp_set). */
      if (__builtin_expect(nskip < 0 || __atomic_load_n(&s_skip_fp_dirty, __ATOMIC_RELAXED), 0)) {
          if (nskip < 0) ps3_toggle_watch(PS3T_ENG_SKIP_FP, eng_skip_fp_set);
          __atomic_store_n(&s_skip_fp_dirty, 0, __ATOMIC_RELAXED);
          char buf[PS3_TOGGLE_STR]; const char* e = ps3_toggle_str(PS3T_ENG_SKIP_FP, buf, sizeof buf);
          nskip = 0;
          while (e && *e && nskip < 32) { skipfp[nskip++] = strtoull(e, (char**)&e, 16); if (*e == ',') e++; else break; } }
      if (nskip) { const eng_fp_memo* fpm = NULL;
          const u64 fk = eng_guest_programs(NULL, &fpm) && fpm ? fpm->structural : 0;
          for (int k = 0; k < nskip; k++) if (fk && skipfp[k] == fk) return; } }

    const u32 vtex_mask = sink_bind_vertex_textures(eng_vtex_mask(), vtextures,
                                                    vsamplers);

    /* A guest program pair that will not translate has no fallback: the draw
     * is dropped, as the reference engine drops it. Substituting the built-in
     * program would draw the geometry in the wrong colours, which is harder to
     * see than a missing object and hides the translation failure. */
    DS_MARK(ns_tex);
    int pipeline_is_fixed = 1;
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_surface_format(sf.color_format),
                                          n_targets, &pipeline_is_fixed);
    if (!pipeline) { eng_drop("pipeline", prim); return; }
    eng_fbf_fallback(textures);   /* PS3RECOMP_ENG_FBFETCH */
    DS_MARK(ns_pipe);
    /* PS3RECOMP_ENG_REGDUMP_PIPE=<p>[,<p>..]: the whole RSX register file (the
     * non-zero words) at the first draw of each listed pipeline, so two passes
     * can be diffed register by register -- which state a pass sets that the
     * engine may not honour (a mirrored render target, a clip plane). */
    { static const char* rd = (const char*)-1; static u32 done_mask;
      if (rd == (const char*)-1) rd = getenv("PS3RECOMP_ENG_REGDUMP_PIPE");
      if (rd) {
          const char* q = rd; int idx = 0, hit = -1;
          while (*q) { char* e; long v = strtol(q, &e, 10); if (e == q) break;
                       if (v == (long)pipeline) { hit = idx; break; } idx++; q = *e == ',' ? e + 1 : e; }
          if (hit >= 0 && hit < 32 && !(done_mask & (1u << hit))) {
              done_mask |= 1u << hit;
              const eng_fp_memo* fpm = NULL;
              const u64 fk = eng_guest_programs(NULL, &fpm) && fpm ? fpm->structural : 0;
              fprintf(stderr, "[regdump] p=%u fp=0x%016llX frame=%u begin\n", pipeline, (unsigned long long)fk, g.frames);
              for (u32 a = 0; a < RSX_DSP_NUM_REGS; a++) {
                  const u32 v = g.rsx.regs[a];
                  if (v) fprintf(stderr, "[regdump] p=%u 0x%04X=0x%08X\n", pipeline, a * 4u, v);
              }
              fprintf(stderr, "[regdump] p=%u end\n", pipeline);
          } } }
    /* PS3RECOMP_ENG_TEXSTATE_PIPE=<p>: the texture registers of every enabled
     * unit of draws through that pipeline, once per fragment program, with
     * the border colour (SET_TEXTURE_BORDER_COLOR, 0x1A1C + 0x20*unit) that
     * wrap mode BORDER returns outside 0..1. */
    { static long tp = -2; if (tp == -2) { const char* e = getenv("PS3RECOMP_ENG_TEXSTATE_PIPE"); tp = e ? strtol(e, 0, 10) : -1; }
      if (tp >= 0 && pipeline == (u32)tp) {
          const eng_fp_memo* fpm = NULL;
          static u64 seen[16]; static u32 ns;
          const u64 fk = eng_guest_programs(NULL, &fpm) && fpm ? fpm->structural : 0;
          u32 k = 0; while (k < ns && seen[k] != fk) k++;
          if (k == ns && ns < 16) { seen[ns++] = fk;
              fprintf(stderr, "[texstate] p=%u fp=0x%016llX verts=%u\n", pipeline, (unsigned long long)fk, dc.n_verts);
              for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
                  rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
                  if (!t.enabled) continue;
                  fprintf(stderr, "[texstate]   unit %2u -> h%-5u loc %u off 0x%08X fmt 0x%02X %ux%u mips %u pitch %u cube %u remap 0x%08X filter 0x%08X ctl0 0x%08X addr 0x%08X border 0x%08X dim %u depth %u (FORMAT 0x%08X CTL3 0x%08X)\n",
                          u, textures[u], t.location, t.offset, t.format, t.width, t.height, t.mipmaps, t.pitch,
                          t.cubemap, t.remap, t.filter, t.control0, t.wrap, rsx_dsp_reg(&g.rsx, 0x1A1Cu + 0x20u * u),
                          (rsx_dsp_reg(&g.rsx, 0x1A04u + 0x20u * u) >> 4) & 0xFu, rsx_dsp_reg(&g.rsx, 0x1840u + 4u * u) >> 20,
                          rsx_dsp_reg(&g.rsx, 0x1A04u + 0x20u * u), rsx_dsp_reg(&g.rsx, 0x1840u + 4u * u));
              } } } }
    /* PS3RECOMP_ENG_TEXRAW_FP=<hex>:<unit>:<dir>: the guest bytes of the texture
     * bound on <unit> for draws of that fragment program, raw (before any
     * decode), once per distinct texture offset (first 16), with its registers
     * in <dir>/index.txt -- data for an offline check of how a program reads
     * a lookup texture (the occlusion volumes' 8x8x8 cone LUTs). */
    { static u64 rfp; static long runit = -1; static char rdir[400]; static int rinit;
      static u32 rseen[16]; static u32 rn;
      if (!rinit) { rinit = 1; const char* e = getenv("PS3RECOMP_ENG_TEXRAW_FP");
          if (e) { char* d; rfp = strtoull(e, &d, 16);
                   if (*d == ':') runit = strtol(d + 1, &d, 10);
                   if (*d == ':') snprintf(rdir, sizeof rdir, "%s", d + 1); } }
      if (rfp && runit >= 0 && runit < (long)RSX_BE_MAX_TEXTURES && rn < 16 && rdir[0]) {
          const eng_fp_memo* fpm = NULL;
          if (eng_guest_programs(NULL, &fpm) && fpm && fpm->structural == rfp) {
              rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, (u32)runit, &t);
              const u32 key = t.offset ^ (t.location << 31);
              u32 k = 0; while (k < rn && rseen[k] != key) k++;
              if (k == rn && t.width && t.height) {
                  rseen[rn++] = key;
                  const u32 depth = (t.dimension == 3 && !t.cubemap && t.depth) ? t.depth : 1u;
                  const u32 span = depth > 1 ? eng_texture_span_3d(t.format, t.width, t.height, depth, t.mipmaps, t.pitch)
                                             : eng_texture_span(t.format, t.width, t.height, t.mipmaps, t.pitch, (int)t.cubemap);
                  const u8* src = span ? eng_guest_ptr(NULL, t.location, t.offset, span) : NULL;
                  char path[512]; snprintf(path, sizeof path, "%s/tex%02u.bin", rdir, rn - 1);
                  FILE* f = src ? fopen(path, "wb") : NULL;
                  if (f) { fwrite(src, 1, span, f); fclose(f); }
                  snprintf(path, sizeof path, "%s/index.txt", rdir);
                  FILE* ix = fopen(path, "a");
                  if (ix) { fprintf(ix, "tex%02u loc %u off 0x%08X fmt 0x%02X %ux%u depth %u dim %u mips %u pitch %u remap 0x%08X filter 0x%08X addr 0x%08X ctl0 0x%08X border 0x%08X span %u frame %u\n",
                                    rn - 1, t.location, t.offset, t.format, t.width, t.height, depth, t.dimension, t.mipmaps,
                                    t.pitch, t.remap, t.filter, t.wrap, t.control0, rsx_dsp_reg(&g.rsx, 0x1A1Cu + 0x20u * (u32)runit),
                                    span, g.frames); fclose(ix); }
              } } } }
    /* PS3RECOMP_ENG_DUMPTEX_FP=<hex>:<unit>:<sec>:<path>: once, at the first
     * draw of that fragment program <sec> seconds after its first draw, read
     * the texture bound on <unit> back (8-bit RGBA only) to <path>.ppm and
     * <path>.a.ppm -- the backend's DUMPTEX by p= number, keyed on the
     * program hash that survives streaming order. */
    { static u64 dfp; static long dunit, dsec; static char dpath[512]; static int dinit, ddone;
      if (!dinit) { dinit = 1; const char* e = getenv("PS3RECOMP_ENG_DUMPTEX_FP");
          if (e) { char* d; dfp = strtoull(e, &d, 16);
                   if (*d == ':') dunit = strtol(d + 1, &d, 10);
                   if (*d == ':') dsec = strtol(d + 1, &d, 10);
                   if (*d == ':') snprintf(dpath, sizeof dpath, "%s", d + 1); } }
      if (dfp && !ddone && dunit >= 0 && dunit < (long)RSX_BE_MAX_TEXTURES && g.be->readback) {
          const eng_fp_memo* fpm = NULL;
          if (eng_guest_programs(NULL, &fpm) && fpm && fpm->structural == dfp) {
              static unsigned long long t0; extern unsigned long long ps3_ms_now(void);
              if (!t0) t0 = ps3_ms_now();
              rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, (u32)dunit, &t);
              if (ps3_ms_now() - t0 >= (unsigned long long)dsec * 1000ull && textures[dunit] && t.width && t.height) {
                  ddone = 1;
                  const u32 tw = t.width, th = t.height;
                  u8* px = (u8*)malloc((size_t)tw * th * 4);
                  if (px) {
                      g.be->readback(g.be->user, textures[dunit], 0, 0, tw, th, px, tw * 4);
                      char name[600]; snprintf(name, sizeof name, "%s.ppm", dpath);
                      FILE* f = fopen(name, "wb");
                      snprintf(name, sizeof name, "%s.a.ppm", dpath);
                      FILE* fa = fopen(name, "wb");
                      if (f)  fprintf(f,  "P6 %u %u 255\n", tw, th);
                      if (fa) fprintf(fa, "P6 %u %u 255\n", tw, th);
                      for (u32 i = 0; i < tw * th; i++) {
                          const u8 aaa[3] = { px[i * 4 + 3], px[i * 4 + 3], px[i * 4 + 3] };
                          if (f)  fwrite(px + i * 4, 1, 3, f);
                          if (fa) fwrite(aaa, 1, 3, fa);
                      }
                      if (f) fclose(f); if (fa) fclose(fa);
                      free(px);
                      fprintf(stderr, "[rsx engine] dumptex_fp %016llx unit %ld h%u %ux%u -> %s.ppm\n",
                              (unsigned long long)dfp, dunit, textures[dunit], tw, th, dpath);
                  } } } } }
    /* PS3RECOMP_ENG_MRTLOG=1: each distinct (pipeline, targets, COLOR_MASK,
     * COLOR_MASK_MRT 0x0370, BLEND_ENABLE, BLEND_ENABLE_MRT 0x036C) once --
     * whether an MRT pass gives its extra targets their own mask/blend. */
    /* =2 also logs single-target passes, and every line names each target's
     * guest offset, the engine surface (handle, size) it resolved to and the
     * surface format -- who writes which channel of which G-buffer plane. */
    { static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_MRTLOG"); on = e ? atoi(e) : 0; }
      /* =3: also one line per frame that had multi-target draws, with the
       * count -- when the title renders its G-buffer at all. */
      if (on >= 3 && n_targets > 1) {
          static u32 mf_last = ~0u, mf_n, mf_lines;
          if (g.frames != mf_last) {
              if (mf_n && mf_lines++ < 4000) fprintf(stderr, "[mrtf] frame=%u multi_draws=%u\n", mf_last, mf_n);
              mf_last = g.frames; mf_n = 0;
          }
          mf_n++;
      }
      if (on && (n_targets > 1 || on >= 2)) {
          static u64 seen[2048]; static u32 ns;
          const u32 cm = rsx_dsp_reg(&g.rsx, 0x0324), cmm = rsx_dsp_reg(&g.rsx, 0x0370);
          const u32 be = rsx_dsp_reg(&g.rsx, 0x0310), bem = rsx_dsp_reg(&g.rsx, 0x036C);
          rsx_dsp_surface msf; rsx_dsp_get_surface(&g.rsx, &msf);
          u64 key = ((u64)pipeline << 40) ^ ((u64)n_targets << 36) ^ ((u64)cmm << 16) ^ ((u64)bem << 8) ^ (be & 1) ^ ((u64)cm * 0x9E3779B1u << 1);
          for (u32 t = 0; t < n_targets; t++) key = key * 1099511628211ull ^ g.surfaces[targets[t]].offset;
          key = key * 1099511628211ull ^ msf.color_target;
          u32 k = 0; while (k < ns && seen[k] != key) k++;
          if (k == ns && ns < 2048) { seen[ns++] = key;
              char tl[256]; int tn = 0; tl[0] = 0;
              for (u32 t = 0; t < n_targets && tn < (int)sizeof tl - 48; t++) {
                  const eng_surface* es = &g.surfaces[targets[t]];
                  tn += snprintf(tl + tn, sizeof tl - tn, " %c=0x%08X/h%u/%ux%u", 'A' + (int)t, es->offset, es->handle, es->w, es->h);
              }
              /* rt_en is the raw SET_SURFACE_COLOR_TARGET; the four guest
               * offsets/locations say which of the targets it names the
               * engine did not resolve (same address twice, or absent). */
              fprintf(stderr, "[mrt] frame=%u p=%u targets=%u rt_en=0x%02X fmt=%u clip=%ux%u offs=%X/%X/%X/%X locs=%u%u%u%u cmask=0x%08X cmask_mrt=0x%04X blend=%u blend_mrt=0x%X%s\n",
                      g.frames, pipeline, n_targets, msf.color_target, msf.color_format, msf.clip_w, msf.clip_h,
                      msf.color_offset[0], msf.color_offset[1], msf.color_offset[2], msf.color_offset[3],
                      msf.color_location[0], msf.color_location[1], msf.color_location[2], msf.color_location[3],
                      cm, cmm, be & 1, bem, tl); } } }

    /* PS3RECOMP_ENG_ICACHE: the rebuilt list is resident (dc_fetch). */
    const u32 ic_buf = (s_ic_hit && indexed && dc.vbuf && s_ic_cur && s_ic_cur->n_draw == n_draw)
                     ? s_ic_cur->ibuf : 0u;
    if (s_ic_hit && !ic_buf) {   /* cannot happen: the entry was made from this same state */
        static int n; if (PS3_LOG_FIRST(n, 4))
            fprintf(stderr, "[rsx engine] index cache: entry disagrees with the draw (n %u vs %u) -- dropped\n",
                    n_draw, s_ic_cur ? s_ic_cur->n_draw : 0u);
        if (s_ic_cur) eng_ic_drop(s_ic_cur);
        eng_drop("icache", prim); return;
    }
    if (indexed && !ic_buf) {
        if (!dc_reserve_indices(n_draw)) { eng_drop("reserve_indices", prim); return; }
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL,
            g.indices);
        if (s_ic_cur && dc.vbuf) eng_ic_after_draw(s_ic_cur, s_ic_cur_content, n_draw);
    }

    /* The transform constant block, then the viewport epilogue the vertex
     * program's tail multiplies by: RSX window coordinates mapped into the
     * host's clip space (rsx_live_draw.c:6143-6156). */
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        /* As RPCS3's Vulkan backend (fill_scale_offset_data, flip_y=false):
         * the window origin does not enter the position transform -- only
         * WPOS (see fp_alpha) -- and FRONT_FACE maps directly. */
        xf[0] = vp.scale[0] / (W * 0.5f);
        xf[1] = -(vp.scale[1] / (H * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - W * 0.5f) / (W * 0.5f);
        xf[5] = -((vp.translate[1] - H * 0.5f) / (H * 0.5f));
        xf[6] = vp.translate[2];
    }
    /* Only the rows the program reads go to the GPU when the backend can
     * take a range: copying all 512 rows twice per draw was ~240 MB of
     * memcpy per world frame in inFamous (T-0016). */
    /* PS3RECOMP_ENG_VPC_FULL=1: always upload the whole block (A/B, bisect). */
    static int vpc_force_full = -1;
    if (vpc_force_full < 0) vpc_force_full = getenv("PS3RECOMP_ENG_VPC_FULL") ? 1 : 0;
    const int vpc_range = g.be->bind_vs_constants_range && !g.vpc_full && !vpc_force_full;
    if (!vpc_range) {
        if (ds_t0 && !memcmp(g.vp_cb, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16u)) ds.cb_same++;
        memcpy(g.vp_cb, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16u);
        memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);
    }
    eng_constlog_draw(sf.color_offset[0], sf.clip_w, sf.clip_h, n_draw, indexed);

    /* The buffered fragment constants, then fp_alpha: the layout
     * rsx_fp_decompile_buffered_ex compiled the shader against. */
    const u32 nslots = g.fp_constants->count ? g.fp_constants->count : 1u;
    /* + fp_dbounds after fp_alpha for a depth-bounded draw. */
    const u32 fp_bytes = (nslots + (s_dbz.unit ? 2u : 1u)) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) { eng_drop("n0", prim); return; }
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants->count)
        memcpy(g.fp_cb, g.fp_constants->values, g.fp_constants->count * 16u);
    /* PS3RECOMP_ENG_FPK_PIPE=<p>: the fragment constants each draw through
     * pipeline <p> (pass-log p= number) receives, first N draws per 600
     * frames (PS3RECOMP_ENG_FPK_N, default 8) -- tells "the shader math is
     * wrong" from "it was handed zeros". PS3RECOMP_ENG_FPK_AFTER=<sec> waits
     * that long after the pipeline's first draw (a scene, not the boot), and
     * the vertex constants the program reads ([vpk]) come with each dump. */
    { static long fpk = -2; static u32 fpk_n; static long after; static u64 fpk_fp;
      if (fpk == -2) { const char* e = getenv("PS3RECOMP_ENG_FPK_PIPE"); fpk = e ? strtol(e, 0, 10) : -1;
                       const char* c = getenv("PS3RECOMP_ENG_FPK_N"); fpk_n = c ? (u32)atoi(c) : 8u;
                       const char* a = getenv("PS3RECOMP_ENG_FPK_AFTER"); after = a ? strtol(a, 0, 10) : 0;
                       /* PS3RECOMP_ENG_FPK_FP=<hex>: by fragment program hash instead of p= */
                       const char* h = getenv("PS3RECOMP_ENG_FPK_FP"); fpk_fp = h ? strtoull(h, 0, 16) : 0;
                       if (fpk_fp) fpk = 0x7FFFFFFFL; }
      int fpk_hit = fpk >= 0 && pipeline == (u32)fpk;
      if (fpk_fp) { const eng_fp_memo* fpm = NULL;
          fpk_hit = eng_guest_programs(NULL, &fpm) && fpm && fpm->structural == fpk_fp; }
      if (fpk_hit) {
          static u32 n, win = ~0u; static unsigned long long t0;
          extern unsigned long long ps3_ms_now(void);
          if (!t0) t0 = ps3_ms_now();
          if (g.frames / 600 != win) { win = g.frames / 600; n = 0; }
          if (ps3_ms_now() - t0 >= (unsigned long long)after * 1000ull && n++ < fpk_n) {
              const float* k = (const float*)g.fp_cb;
              fprintf(stderr, "[fpk] p=%u frame=%u draw=%u n=%u consts=%u fp=%u:0x%08X\n", pipeline,
                      g.frames, g.guest_draws, n_draw, g.fp_constants->count,
                      rsx_dsp_reg(&g.rsx, 0x08E4) & 3u, rsx_dsp_reg(&g.rsx, 0x08E4) & ~3u);
              for (u32 q = 0; q < g.fp_constants->count; q++)
                  fprintf(stderr, "[fpk]   k[%2u] = (%g %g %g %g)\n", q,
                          k[q * 4], k[q * 4 + 1], k[q * 4 + 2], k[q * 4 + 3]);
              for (u32 c = g.vpc_lo; c < g.vpc_lo + g.vpc_n && c < RSX_DSP_NUM_CONSTANTS; c++) {
                  float v[4]; memcpy(v, g.rsx.constants[c], sizeof v);
                  if (!(v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f)) continue;
                  fprintf(stderr, "[vpk]   c[%3u] = (%g %g %g %g)\n", c, v[0], v[1], v[2], v[3]);
              }
          }
      } }
    /* PS3RECOMP_ENG_VPC_WATCH=<p>:<c>: how often draws through pipeline <p>
     * run with vertex constant <c> all zero, per 600 frames -- a state
     * register that is sometimes unset at draw time is a FIFO ordering or
     * sync problem, not a shader one. */
    { static long wp = -2, wc; static u32 zero, total, win = ~0u;
      if (wp == -2) { const char* e = getenv("PS3RECOMP_ENG_VPC_WATCH"); wp = -1;
          if (e) { char* d; wp = strtol(e, &d, 10); wc = (*d == ':') ? strtol(d + 1, 0, 10) : 0; } }
      if (wp >= 0 && pipeline == (u32)wp && wc >= 0 && wc < RSX_DSP_NUM_CONSTANTS) {
          float v[4]; memcpy(v, g.rsx.constants[wc], sizeof v);
          total++; if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f) { zero++;
              static u32 said; if (said++ < 40)
                  fprintf(stderr, "[vpcwatch] ZERO c[%ld] at frame %u draw %u (pipeline %ld)\n", wc, g.frames, g.guest_draws, wp); }
          if (g.frames / 600 != win) { if (win != ~0u)
              fprintf(stderr, "[vpcwatch] p=%ld c[%ld] zero at %u of %u draws (frames %u..%u)\n", wp, wc, zero, total, win * 600, g.frames);
              win = g.frames / 600; zero = total = 0; } } }
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      /* WPOS y = pos.y * alpha[1] + alpha[2]: SHADER_WINDOW (0x1D88) bits
       * 0..11 height, bit 12 origin (1 = bottom: y counts up from the
       * bottom row). */
      const u32 win = rsx_dsp_reg(&g.rsx, 0x1D88);
      const int bottom = (win >> 12) & 1;
      alpha[1] = bottom ? -1.0f : 1.0f;
      alpha[2] = bottom ? (float)(win & 0xFFFu) : 0.0f;
      alpha[3] = 0.0f;
      if (s_dbz.unit) { alpha[4] = s_dbz.mn; alpha[5] = s_dbz.mx; alpha[6] = alpha[7] = 0.0f; } }

    /* PS3RECOMP_ENG_OCCDUMP=<sec>:<dir>[:<hex>,<hex>..]: DIAGNOSTIC for the
     * occlusion volumes (default programs 5f740845528b0e62 props and
     * 7875289ea6b856a6 characters). In one frame, <sec> seconds after their
     * first draw: the render target's bytes before every volume draw and
     * after the last one (rt_NNN.rgba), the depth snapshot and normals they
     * sample (unit0.rgba / unit1.rgba), each cone LUT raw (lut_<off>.bin) and
     * every draw's constants and state (draws.txt) -- so the per-draw alpha
     * the GPU produced can be checked against the program's math offline.
     * Every dump flushes and waits: one frame, debugging only.
     * PS3RECOMP_ENG_OCCDUMP_TRIGGER=<file>: instead of the timer, dump the next
     * frame each time <file> appears (it is removed), into <dir>/dNN -- for a
     * windowed session at the spot that looks wrong (tools/play_dump.sh). */
    { static int oinit; static u64 ofp[4] = { 0x5f740845528b0e62ull, 0x7875289ea6b856a6ull }; static int nofp = 2;
      static long osec = -1; static char odir[400]; static unsigned long long ot0;
      static u32 oframe = ~0u, on_draw, olast_h, olast_w, olast_hh; static int ostate; /* 0 wait, 1 capturing, 2 done */
      static u32 oluts[64], nluts; static int odone_u[16]; static char ocur[460]; static u32 ondump; static u32 ounits = 3;
      static const char* otrig;
      if (!oinit) { oinit = 1; const char* e = getenv("PS3RECOMP_ENG_OCCDUMP");
          otrig = getenv("PS3RECOMP_ENG_OCCDUMP_TRIGGER");
          /* PS3RECOMP_ENG_OCCDUMP_UNITS=<hex mask>: which texture units of the
           * listed programs to read back (default 3: depth snapshot, normals);
           * only 8-bit ARGB images (a cube or another format is skipped). With
           * the composite's hash (f384f111ad5956ba) and 0x3F, every input of
           * the lighting composite in one frame. */
          { const char* m = getenv("PS3RECOMP_ENG_OCCDUMP_UNITS"); if (m) ounits = (u32)strtoul(m, 0, 16); }
          if (e) { char* d; osec = strtol(e, &d, 10);
                   if (*d == ':') { const char* p = d + 1; const char* c = strchr(p, ':');
                       size_t n = c ? (size_t)(c - p) : strlen(p); if (n >= sizeof odir) n = sizeof odir - 1;
                       memcpy(odir, p, n); odir[n] = 0;
                       if (c) { nofp = 0; c++; while (*c && nofp < 4) { ofp[nofp++] = strtoull(c, (char**)&c, 16); if (*c == ',') c++; else break; } } } } }
      if (!ocur[0] && odir[0]) snprintf(ocur, sizeof ocur, "%s", odir);
      if ((osec >= 0 || otrig) && odir[0] && (ostate < 2 || otrig) && g.be->readback && g.be->submit_and_wait) {
          if (otrig && ostate == 2) ostate = 0;   /* ready for the next trigger */
          const u64 fk = eng_current_fp_structural();
          int vol = 0; for (int k = 0; k < nofp; k++) if (fk && fk == ofp[k]) vol = 1;
          extern unsigned long long ps3_ms_now(void);
          if (vol && !ot0) ot0 = ps3_ms_now();
          char path[512];
          /* PS3RECOMP_ENG_OCCDUMP_SPAN=1: the listed programs' draws are
           * spread over the frame (the sun mask: one draw after each shadow
           * cascade), so the dump goes on to the frame's end, and the first
           * other draw after each listed one dumps the target as that draw
           * left it (rt_NNN_after.rgba, NNN = the listed draw). */
          static int ospan = -1, olastvol;
          if (ospan < 0) { const char* e = getenv("PS3RECOMP_ENG_OCCDUMP_SPAN"); ospan = e ? atoi(e) : 0; }
          if (ospan && ostate == 1 && !vol && olastvol && g.frames == oframe && olast_h) {
              u8* px = (u8*)malloc((size_t)olast_w * olast_hh * 4);
              if (px) { g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
                  g.be->readback(g.be->user, olast_h, 0, 0, olast_w, olast_hh, px, olast_w * 4);
                  snprintf(path, sizeof path, "%s/rt_%03u_after.rgba", ocur, on_draw - 1u);
                  FILE* f = fopen(path, "wb"); if (f) { fwrite(px, 1, (size_t)olast_w * olast_hh * 4, f); fclose(f); }
                  free(px); }
          }
          if (!vol) olastvol = 0;
          /* the frame ended, or a non-volume draw follows the volumes: final dump */
          if (ostate == 1 && ((!vol && !ospan) || g.frames != oframe) && olast_h) {
              u8* px = (u8*)malloc((size_t)olast_w * olast_hh * 4);
              if (px) { g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
                  g.be->readback(g.be->user, olast_h, 0, 0, olast_w, olast_hh, px, olast_w * 4);
                  snprintf(path, sizeof path, "%s/rt_%03u.rgba", ocur, on_draw);
                  FILE* f = fopen(path, "wb"); if (f) { fwrite(px, 1, (size_t)olast_w * olast_hh * 4, f); fclose(f); }
                  free(px); }
              fprintf(stderr, "[occdump] frame %u: %u volume draws dumped to %s\n", oframe, on_draw, ocur);
              ostate = 2;
          }
          int arm = 0;
          static u32 ochk = ~0u;   /* the trigger file is looked for once a frame */
          if (otrig) { FILE* tf = (ostate == 0 && g.frames != ochk) ? fopen(otrig, "r") : NULL; ochk = g.frames;
              if (tf) { fclose(tf); remove(otrig); arm = 1;
                  snprintf(ocur, sizeof ocur, "%s/d%02u", odir, ++ondump); mkdir(ocur, 0755); } }
          else arm = vol && ps3_ms_now() - ot0 >= (unsigned long long)osec * 1000ull;
          if (ostate == 0 && arm) { ostate = 3; on_draw = 0; olast_h = 0; nluts = 0; memset(odone_u, 0, sizeof odone_u); }
          if (vol && ostate == 3) { ostate = 1; oframe = g.frames; }
          if (vol && ostate == 1 && g.frames == oframe && n_targets) {
              const eng_surface* es = &g.surfaces[targets[0]];
              g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
              u8* px = (u8*)malloc((size_t)es->w * es->h * 4);
              if (px) { g.be->readback(g.be->user, es->handle, 0, 0, es->w, es->h, px, es->w * 4);
                  snprintf(path, sizeof path, "%s/rt_%03u.rgba", ocur, on_draw);
                  FILE* f = fopen(path, "wb"); if (f) { fwrite(px, 1, (size_t)es->w * es->h * 4, f); fclose(f); }
                  free(px); }
              olast_h = es->handle; olast_w = es->w; olast_hh = es->h;
              for (u32 u = 0; u < 16; u++) {
                  if (!(ounits >> u & 1u)) continue;
                  rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
                  if (ospan) snprintf(path, sizeof path, "%s/unit%u_d%03u.rgba", ocur, u, on_draw);
                  else snprintf(path, sizeof path, "%s/unit%u.rgba", ocur, u);
                  const u32 obase = t.format & 0x9Fu;
                  if (!t.enabled || !textures[u] || !t.width || !t.height || odone_u[u] || t.cubemap ||
                      (u >= 2 && obase != 0x85u && obase != 0x81u)) continue;
                  const u32 obpp = obase == 0x81u ? 1u : 4u;   /* B8 (the SSAO term) reads back one byte a texel */
                  if (obpp == 1u) snprintf(path, sizeof path, "%s/unit%u.r8", ocur, u);
                  if (!ospan) odone_u[u] = 1;   /* SPAN: every draw's inputs */
                  { char ip[512]; snprintf(ip, sizeof ip, "%s/units.txt", ocur); FILE* fi = fopen(ip, "a");
                    if (fi) { fprintf(fi, "unit %u fp %016llx loc %u off 0x%08X fmt 0x%02X %ux%u remap 0x%08X addr 0x%08X\n", u,
                                      (unsigned long long)fk, t.location, t.offset, t.format, t.width, t.height, t.remap, t.wrap); fclose(fi); } }
                  u8* tp = (u8*)malloc((size_t)t.width * t.height * 4);
                  if (!tp) continue;
                  g.be->readback(g.be->user, textures[u], 0, 0, t.width, t.height, tp, t.width * obpp);
                  FILE* f = fopen(path, "wb"); if (f) { fwrite(tp, 1, (size_t)t.width * t.height * obpp, f); fclose(f); }
                  free(tp);
              }
              rsx_dsp_texture lt; rsx_dsp_get_texture(&g.rsx, 2, &lt);
              if (lt.enabled && lt.width) {
                  const u32 key = lt.offset ^ (lt.location << 31);
                  u32 k = 0; while (k < nluts && oluts[k] != key) k++;
                  if (k == nluts && nluts < 64) { oluts[nluts++] = key;
                      const u32 depth = (lt.dimension == 3 && !lt.cubemap && lt.depth) ? lt.depth : 1u;
                      const u32 span = depth > 1 ? eng_texture_span_3d(lt.format, lt.width, lt.height, depth, lt.mipmaps, lt.pitch)
                                                 : eng_texture_span(lt.format, lt.width, lt.height, lt.mipmaps, lt.pitch, (int)lt.cubemap);
                      const u8* src = span ? eng_guest_ptr(NULL, lt.location, lt.offset, span) : NULL;
                      snprintf(path, sizeof path, "%s/lut_%u_%08X.bin", ocur, lt.location, lt.offset);
                      FILE* f = src ? fopen(path, "wb") : NULL; if (f) { fwrite(src, 1, span, f); fclose(f); } } }
              snprintf(path, sizeof path, "%s/draws.txt", ocur);
              FILE* f = fopen(path, "a");
              if (f) {
                  fprintf(f, "draw %u frame %u fp %016llx p %u n %u rt h%u off 0x%08X %ux%u ztest %u zfunc 0x%X zwrite %u cull %u/0x%X front 0x%X stencil %u func 0x%X ref 0x%02X mask 0x%02X blend 0x%08X cmask 0x%08X\n",
                          on_draw, g.frames, (unsigned long long)fk, pipeline, n_draw, es->handle, es->offset, es->w, es->h,
                          rs.depth_test, rs.depth_func, rs.depth_write, rs.cull_enable, rs.cull_face, rs.front_face,
                          rs.stencil_enable, rs.s_func, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu,
                          rsx_dsp_reg(&g.rsx, 0x0330) & 0xFFu, rsx_dsp_reg(&g.rsx, 0x0314), rsx_dsp_reg(&g.rsx, 0x0324));
                  for (u32 u = 0; u < 3; u++) { rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
                      fprintf(f, "tex %u en %u h%u loc %u off 0x%08X fmt 0x%02X %ux%u depth %u dim %u remap 0x%08X filter 0x%08X addr 0x%08X\n",
                              u, t.enabled, textures[u], t.location, t.offset, t.format, t.width, t.height, t.depth, t.dimension,
                              t.remap, t.filter, t.wrap); }
                  fprintf(f, "xf %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", xf[0], xf[1], xf[2], xf[3], xf[4], xf[5], xf[6], xf[7]);
                  const float* k = (const float*)g.fp_cb;
                  for (u32 q = 0; q < g.fp_constants->count; q++)
                      fprintf(f, "k %u %.9g %.9g %.9g %.9g\n", q, k[q * 4], k[q * 4 + 1], k[q * 4 + 2], k[q * 4 + 3]);
                  for (u32 c = 0; c < RSX_DSP_NUM_CONSTANTS; c++) {
                      float v[4]; memcpy(v, g.rsx.constants[c], sizeof v);
                      if (!(v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f)) continue;
                      if (!(c >= 256 && c <= 272) && c != 466 && c != 467) continue;
                      fprintf(f, "c %u %.9g %.9g %.9g %.9g\n", c, v[0], v[1], v[2], v[3]);
                  }
                  fclose(f);
              }
              on_draw++; olastvol = 1;
          }
      } }

    /* The depth attachment is cleared the first time it is bound, so a title
     * that never clears its shadow zeta still tests against something. */
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    if (zslot != ENG_INVALID && !g.zdepths[zslot].cleared) {
        g.be->clear_depth_stencil(g.be->user, depth_handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                  1.0f, 0);
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }

    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) handles[i] = g.surfaces[targets[i]].handle;
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    if (vpc_range)
        g.be->bind_vs_constants_range(g.be->user, g.rsx.constants, g.vpc_lo,
                                      g.vpc_n, xf, sizeof xf, ENG_VP_CB_BYTES);
    else
        g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
    g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
    g.be->bind_textures(g.be->user, textures, samplers, tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    /* PS3RECOMP_ENG_HUDLOG=<sec>: DIAGNOSTIC (PS3_ASPECT). Every draw without
     * a depth test into a single target, for the first world frame <sec>
     * seconds after the first draw and the two after it (a game frame spans
     * two present slices): programs, vertex count, the
     * position attribute's range and the guest viewport -- what tells the
     * title's 2D overlay from its full-screen passes. */
    { static long hsec = -2; static u32 hframe = ~0u, hdone, hafter_frame = ~0u; static unsigned long long ht0;
      static u64 hafter;   /* PS3RECOMP_ENG_HUDLOG_AFTER=<fp hex>: also every draw after it in its frame */
      /* PS3RECOMP_ENG_HUDLOG=<sec>[,<sec>...]: one three-frame window per time
       * (A/B inside one run: a working scene, then a broken one). */
      static long hlist[8]; static u32 hn, hi;
      if (hsec == -2) { const char* e = getenv("PS3RECOMP_ENG_HUDLOG");
                        while (e && *e && hn < 8) { char* q; const long v = strtol(e, &q, 10);
                                                    if (q == e) break; hlist[hn++] = v; e = *q == ',' ? q + 1 : q; }
                        hsec = hn ? hlist[0] : -1;
                        const char* f = getenv("PS3RECOMP_ENG_HUDLOG_AFTER"); hafter = f ? strtoull(f, 0, 16) : 0; }
      if (hdone && hi + 1u < hn) { hsec = hlist[++hi]; hdone = 0; hframe = ~0u; }
      if (hsec >= 0 && !hdone) {
          extern unsigned long long ps3_ms_now(void);
          if (!ht0) ht0 = ps3_ms_now();
          if (hframe == ~0u && g.guest_draws > 1000 && ps3_ms_now() - ht0 >= (unsigned long long)hsec * 1000ull) hframe = g.frames;
          if (hframe != ~0u && g.frames > hframe + 2u) hdone = 1;
          const eng_vp_memo* vpm = NULL; const eng_fp_memo* fpm = NULL;
          if (hframe != ~0u && g.frames - hframe <= 2u) eng_guest_programs(&vpm, &fpm);
          const int after = hafter_frame == g.frames;
          if (hafter && fpm && fpm->structural == hafter) hafter_frame = g.frames;
          /* PS3RECOMP_ENG_HUDLOG_RT=<hex offset>: every draw into that colour
           * target too, depth-tested or not (light volumes, MRT passes). */
          static long hrt = -2;
          if (hrt == -2) { const char* e = getenv("PS3RECOMP_ENG_HUDLOG_RT"); hrt = e ? (long)strtoul(e, 0, 16) : -1; }
          const int rt_match = hrt >= 0 && n_targets && g.surfaces[target].offset == (u32)hrt;
          if (hframe != ~0u && g.frames - hframe <= 2u && (after || rt_match || (n_targets == 1 && !rs.depth_test))) {
              float mn[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
              const s8 ps = layout.slot_by_attr[0];
              if (ps >= 0 && dc.verts && !(dc.vbuf_hit && indexed))
                  for (u32 v = 0; v < uploaded_hud_n(dc.n_verts, n_draw, indexed); v++) {
                      const float* q = (const float*)((const u8*)dc.verts + (size_t)v * layout.stride + (size_t)ps * 16u);
                      for (int c = 0; c < 4; c++) { if (q[c] < mn[c]) mn[c] = q[c]; if (q[c] > mx[c]) mx[c] = q[c]; } }
              fprintf(stderr, "[hudlog] f=%u d=%u nt=%u z=%u/0x%X rt=0x%08X %ux%u fp=%016llx vp=%016llx vpc=%u+%u n=%u blend=%u cmask=0x%08X vps=(%g %g) vpt=(%g %g) pos x[%g,%g] y[%g,%g] z[%g,%g] w[%g,%g]\n",
                      g.frames, g.guest_draws, n_targets, rs.depth_test, rs.depth_func, g.surfaces[target].offset, g.surfaces[target].w, g.surfaces[target].h,
                      (unsigned long long)(fpm ? fpm->structural : 0), (unsigned long long)(vpm ? vpm->hash : 0),
                      g.vpc_lo, g.vpc_n, dc.n_verts, rs.blend_enable, rsx_dsp_reg(&g.rsx, 0x0324),
                      vp.scale[0], vp.scale[1], vp.translate[0], vp.translate[1],
                      mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]);
              /* The blend itself (SFACTOR/DFACTOR/EQUATION, A<<16|RGB) and the
               * blend colour 0x0310+.. -- what a fade or composite does with
               * the frame under it. */
              fprintf(stderr, "[hudlog]   blend sf=0x%08X df=0x%08X eq=0x%08X color=0x%08X tex_mask=0x%X\n",
                      rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR), rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR),
                      rsx_dsp_reg(&g.rsx, M_BLEND_EQUATION), rsx_dsp_reg(&g.rsx, 0x031C), tex_mask);
              /* What can reject every fragment of a draw whose depth test is
               * off: the stencil test, the scissor, depth bounds. */
              fprintf(stderr, "[hudlog]   stencil en=%u func=0x%X ref=0x%02X fmask=0x%02X wmask=0x%02X ops=0x%X/0x%X/0x%X two=%u dbounds=%u scissor=0x%08X/0x%08X zslot=%d vpz=(%g %g) zminmax=0x%X\n",
                      rsx_dsp_reg(&g.rsx, M_STENCIL_TEST_ENABLE) & 1u, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC),
                      rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_MASK) & 0xFFu,
                      rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFFu, rsx_dsp_reg(&g.rsx, M_STENCIL_OP_FAIL),
                      rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZFAIL), rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZPASS),
                      rsx_dsp_reg(&g.rsx, M_TWO_SIDED_STENCIL) & 1u, rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_ENABLE) & 1u,
                      rsx_dsp_reg(&g.rsx, 0x08C0), rsx_dsp_reg(&g.rsx, 0x08C4), zslot == ENG_INVALID ? -1 : (int)zslot,
                      vp.scale[2], vp.translate[2], rsx_dsp_reg(&g.rsx, 0x1D78));
              { const float* k = (const float*)g.fp_cb;   /* the fragment constants handed over */
                const u32 nk = g.fp_constants->count;
                for (u32 i = 0; i < nk && i < 32u; i++)
                    fprintf(stderr, "[hudlog]   fpc[%u] = (%g %g %g %g)\n", i, k[i*4], k[i*4+1], k[i*4+2], k[i*4+3]);
                if (!nk) fprintf(stderr, "[hudlog]   fpc: none\n");
                char tb[256]; int tp = 0;
                for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES && tp < (int)sizeof tb - 16; u++)
                    if (tex_mask & (1u << u)) tp += snprintf(tb + tp, sizeof tb - (size_t)tp, " u%u=h%u", u, textures[u]);
                fprintf(stderr, "[hudlog]   textures:%s\n", tb);
                /* Each sampled unit's raw state: what a read returning black
                 * was given (format, remap, filter, wrap, LOD control). */
                for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
                    if (!(tex_mask & (1u << u))) continue;
                    rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
                    fprintf(stderr, "[hudlog]     u%u en %u loc %u off 0x%08X fmt 0x%02X %ux%u mips %u pitch %u cube %u dim %u remap 0x%08X filter 0x%08X ctl0 0x%08X wrap 0x%08X\n",
                            u, t.enabled, t.location, t.offset, t.format, t.width, t.height, t.mipmaps, t.pitch,
                            t.cubemap, t.dimension, t.remap, t.filter, t.control0, t.wrap);
                } }
              for (u32 c = g.vpc_lo; c < g.vpc_lo + g.vpc_n && c < g.vpc_lo + 6u && c < RSX_DSP_NUM_CONSTANTS; c++) {
                  float v4[4]; memcpy(v4, g.rsx.constants[c], sizeof v4);
                  fprintf(stderr, "[hudlog]   c[%u] = (%g %g %g %g)\n", c, v4[0], v4[1], v4[2], v4[3]); }
              /* The rows full-screen passes take clip z/w and texcoord
               * transforms from (463..467), whatever the program's range. */
              if (g.vpc_lo + g.vpc_n > 463u && g.vpc_lo < 458u)
                  for (u32 c = 463; c < 468; c++) {
                      float v4[4]; memcpy(v4, g.rsx.constants[c], sizeof v4);
                      fprintf(stderr, "[hudlog]   c[%u] = (%g %g %g %g)\n", c, v4[0], v4[1], v4[2], v4[3]); }
          } } }
    /* PS3RECOMP_FX_LOG=1 (diagnostic): once a second, how strong the game's
     * DOF/edge-blur pass is (ENG_DOF_BLEND_FP): the largest edge weight of its
     * screen mesh (vertex attribute 8, the tc0.z the pass reads; "cached" =
     * the vertices did not change since they were last read), and the depth
     * of field strengths c4.x / c5.x. */
    { static int on = -1; if (on < 0) on = getenv("PS3RECOMP_FX_LOG") ? 1 : 0;
      const eng_fp_memo* xfm = NULL;
      if (on && eng_guest_programs(NULL, &xfm) && xfm && xfm->structural == ENG_DOF_BLEND_FP) {
          extern unsigned long long ps3_ms_now(void);
          static unsigned long long t0, last; static float emax = -1.0f, dmax; static u32 draws, nread;
          if (!t0) t0 = ps3_ms_now();
          const s8 es = layout.slot_by_attr[8];
          if (es >= 0 && dc.verts && !(dc.vbuf_hit && indexed)) {
              float e = 0.0f;
              for (u32 v = 0; v < uploaded_hud_n(dc.n_verts, n_draw, indexed); v++) {
                  const float* q = (const float*)((const u8*)dc.verts + (size_t)v * layout.stride + (size_t)es * 16u);
                  if (fabsf(q[0]) > e) e = fabsf(q[0]);
              }
              if (e > emax) emax = e;
              nread++;
          }
          if (g.fp_constants->count > 5u) { const float* k = (const float*)g.fp_cb;
              if (k[16] > dmax) dmax = k[16]; if (k[20] > dmax) dmax = k[20]; }
          draws++;
          const unsigned long long now = ps3_ms_now();
          if (now - last >= 1000ull) {
              if (emax >= 0.0f) fprintf(stderr, "[fxlog] t=%.0f s (clock %.1f) DOF pass x%u: edge weight max %.3f, dof strength max %.3f\n",
                                        (double)(now - t0) / 1000.0, (double)now / 1000.0, draws, emax, dmax);
              else fprintf(stderr, "[fxlog] t=%.0f s (clock %.1f) DOF pass x%u: edge weight cached, dof strength max %.3f\n",
                           (double)(now - t0) / 1000.0, (double)now / 1000.0, draws, dmax);
              last = now; emax = -1.0f; dmax = 0.0f; draws = nread = 0;
          } } }
    /* PS3_ASPECT_HUD_AFTER=<fp hex>[,<fp hex>..]: the title's 2D overlay for
     * a backend rendering wide (PS3_ASPECT, set_hud). The listed fragment
     * programs are the title's last full-screen pass of a 3D frame (its
     * composite); the draws after it in that frame into a display-sized
     * target are the HUD, and so is every display-sized draw of a frame that
     * has no composite at all (menus, motion comics, movies, loading screens;
     * judged by the frame before, since draws are recorded in order). Those
     * keep 16:9 in the middle of the wide picture; the 3D frame up to and
     * including the composite fills it. Frames are counted when they draw
     * (the host clock's repeat presents do not). Unset: nothing is flagged
     * and everything stretches with the 3D picture. */
    if (g.be->set_hud) {
        if (s_hud_n < 0) { s_hud_n = 0;
            /* Only while a wide picture is asked for: unset (or 16:9) costs nothing. */
            const char* asp = getenv("PS3_ASPECT");
            const char* e = (asp && *asp && strcmp(asp, "16:9")) ? getenv("PS3_ASPECT_HUD_AFTER") : NULL;
            while (e && *e && s_hud_n < 8) { s_hud_fp[s_hud_n++] = strtoull(e, (char**)&e, 16); if (*e == ',') e++; else break; } }
        int hud = 0, display = 0;
        if (s_hud_n) {
            if (g.frames != s_hud_frame) { s_hud_frame = g.frames; s_hud_slice++; s_hud_after = 0; }
            const eng_fp_memo* hfm = NULL;
            const u64 fk = eng_guest_programs(NULL, &hfm) && hfm ? hfm->structural : 0;
            int composite = 0;
            for (int k = 0; k < s_hud_n; k++) if (fk && fk == s_hud_fp[k]) composite = 1;
            display = g.surfaces[target].w == g.width && g.surfaces[target].h == g.height;
            if (composite) { s_hud_after = 1; s_hud_seen = s_hud_slice; }
            else if (display) hud = s_hud_after || !s_hud_seen || s_hud_slice - s_hud_seen > 1u;
        }
        g.be->set_hud(g.be->user, hud ? 1 : (s_hud_n && display ? 2 : 0), g.surfaces[target].w);
    }
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W, H);

    /* The guest scissor intersected with the surface. The nv40 reset is a
     * full 4096x4096 window and a never-written register reads 0 here, which
     * the w == 0 test treats as no scissor, so only a real game scissor
     * narrows anything (rsx_live_draw.c:6180-6197). */
    {
        u32 sx = 0, sy = 0;
        u32 sw = g.surfaces[target].w, sh = g.surfaces[target].h;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const u32 gx = h & 0xFFFFu, gw = h >> 16;
        const u32 gy = v & 0xFFFFu, gh = v >> 16;
        if (gw > 0 && gh > 0) {
            u32 right = sx + sw, bottom = sy + sh;
            if (gx > sx) sx = gx;
            if (gy > sy) sy = gy;
            if (gx + gw < right)  right = gx + gw;
            if (gy + gh < bottom) bottom = gy + gh;
            sw = right > sx ? right - sx : 0;
            sh = bottom > sy ? bottom - sy : 0;
        }
        g.be->set_scissor(g.be->user, sx, sy, sw, sh);
    }
    /* Dynamic, and deliberately outside the pipeline key. */
    g.be->set_stencil_ref(g.be->user,
                          rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);
    eng_apply_depth_bias(sf.zeta_offset, &rs);
    eng_apply_depth_clip(sf.zeta_offset, &rs, vp.scale[1]);

    const u32 uploaded = indexed ? dc.n_verts : n_draw;
    DS_MARK(ns_bind);
    if (ic_buf)
        g.be->draw_vbuf_ib(g.be->user, topology, dc.vbuf, uploaded, layout.stride,
                           ic_buf, n_draw);
    else if (dc.vbuf && indexed)
        g.be->draw_vbuf(g.be->user, topology, dc.vbuf, uploaded, layout.stride,
                        g.indices, n_draw);
    else
        g.be->draw(g.be->user, topology, dc.verts, uploaded, layout.stride,
                   indexed ? g.indices : NULL, indexed ? n_draw : 0);
    DS_MARK(ns_draw);
#undef DS_MARK
    /* Counted only when the draw ran the guest's OWN programs, which is what
     * the test hook means: a draw through the built-in pair is a draw, not
     * evidence that the decompile-translate-compile path worked. */
    if (!pipeline_is_fixed) g.guest_draws++;
    if (ds_t0) {
        ds.draws++;
        ds.vbytes += (u64)uploaded * layout.stride;
        ds.ibytes += (indexed && !ic_buf) ? (u64)n_draw * 4u : 0;
        ds.cbytes += vpc_range ? g.vpc_n * 16u + sizeof xf : ENG_VP_CB_BYTES;
        if (vpc_range) ds.cb_range++;
        ds.ns_rest += eng_ds_now() - ds_t0;
    }

    if (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) {
        g.zdepths[zslot].had_write = 1;
        g.zdepths[zslot].stamp = ++s_surf_clock;
        /* A published depth image is only good until the next depth write:
         * a reader later in the stream resolves again. Kept across writes,
         * inFAMOUS 2's occlusion volumes (drawn after the G-buffer) sampled a
         * snapshot an earlier reader had frozen and reconstructed positions
         * from the wrong depth: flat dark boxes around props and characters. */
        g.zdepths[zslot].snapshot_valid = 0; g.zdepths[zslot].crop_z_valid = 0;
        g.zdepths[zslot].snapshot_rgba_valid = 0; g.zdepths[zslot].crop_valid = 0;
        { static int on = -1;
          if (on < 0) on = getenv("PS3RECOMP_ENG_RESOLVE_LOG") ? 1 : 0;
          if (on) g.zdepths[zslot].writer_fp = eng_current_fp_structural(); }
    }
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (!g.ready) return;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;

    /* PS3RECOMP_ENG_MRTLOG=2: each distinct colour clear -- which channels
     * CLEAR_SURFACE names, the value, and the targets it lands on. */
    { static int on = -1; if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_MRTLOG"); on = e ? atoi(e) : 0; }
      if (on >= 2 && (mask & 0xF0u)) {
          static u64 seen[512]; static u32 ns;
          u64 key = ((u64)(mask & 0xF3u) << 56) ^ rsx_dsp_clear_color(&g.rsx);
          for (u32 t = 0; t < n_targets; t++) key = key * 1099511628211ull ^ g.surfaces[targets[t]].offset;
          u32 k = 0; while (k < ns && seen[k] != key) k++;
          if (k == ns && ns < 512) { seen[ns++] = key;
              fprintf(stderr, "[clr] mask=0x%02X color=0x%08X targets=%u", mask & 0xF3u,
                      rsx_dsp_clear_color(&g.rsx), n_targets);
              for (u32 t = 0; t < n_targets; t++)
                  fprintf(stderr, " %c=0x%08X/h%u", 'A' + (int)t, g.surfaces[targets[t]].offset,
                          g.surfaces[targets[t]].handle);
              fprintf(stderr, "\n"); } } }
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G |
                RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float rgba[4] = {
            (float)((c >> 16) & 0xFF) / 255.0f,
            (float)((c >>  8) & 0xFF) / 255.0f,
            (float)( c        & 0xFF) / 255.0f,
            (float)((c >> 24) & 0xFF) / 255.0f,
        };
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. */
        for (u32 i = 0; i < n_targets; i++)
            g.be->clear_color(g.be->user, g.surfaces[targets[i]].handle, rgba);
    }
    if (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                         sf.clip_w, sf.clip_h);
        if (zslot == ENG_INVALID) return;
        u32 flags = 0;
        if (mask & RSX_CLEAR_DEPTH)   flags |= RSX_BE_CLEAR_DEPTH;
        if (mask & RSX_CLEAR_STENCIL) flags |= RSX_BE_CLEAR_STENCIL;
        /* ZSTENCIL_CLEAR_VALUE is Z24S8: depth24 << 8 | stencil8, with the
         * nv40 reset 0xFFFFFF00 seeded by rsx_dispatch_init, so a stream that
         * never writes it still clears to 1.0 / 0. */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        g.be->clear_depth_stencil(g.be->user, g.zdepths[zslot].handle, flags,
                                  zs ? (float)(zs >> 8) / 16777215.0f : 1.0f,
                                  (u8)(zs & 0xFFu));
        g.zdepths[zslot].cleared = 1;
        if (mask & RSX_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written pass exactly once. */
            g.zdepths[zslot].had_write = 0;
            g.zdepths[zslot].snapshot_valid = 0; g.zdepths[zslot].crop_z_valid = 0;
            g.zdepths[zslot].snapshot_rgba_valid = 0; g.zdepths[zslot].crop_valid = 0;
        }
    }
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
static u32 eng_present_surface(u32 buffer_id)
{
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const eng_display_buffer* d = &g.display_buffers[buffer_id];
        u32 best = ENG_INVALID;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == d->location &&
                g.surfaces[i].offset == d->offset &&
                (best == ENG_INVALID || g.surfaces[i].stamp > g.surfaces[best].stamp))
                best = i;
        if (best != ENG_INVALID) return best;
    }
    return eng_current_surface();
}

/* PS3RECOMP_ENG_CONSTLOG=<file>: one line per draw (colour target, clip,
 * count; every non-zero transform constant for the first 4 draws) for world
 * frames (>5000 draws), every 50th such frame, at most 3. Same shape as
 * RPCS3's RPCS3_DRAWLOG with RPCS3_DRAWLOG_CONSTS=4, so the camera matrices
 * can be compared. */
static char*  s_cl_buf;
static size_t s_cl_len, s_cl_cap;
static u32    s_cl_n;
static FILE* eng_constlog_file(void)
{
    static FILE* f = (FILE*)-1;
    if (f == (FILE*)-1) { const char* e = getenv("PS3RECOMP_ENG_CONSTLOG"); f = e ? fopen(e, "w") : NULL; }
    return f;
}
static void eng_constlog_draw(u32 rt, u32 cw, u32 ch, u32 n, int indexed)
{
    if (!eng_constlog_file()) return;
    if (s_cl_cap - s_cl_len < 65536) {
        size_t nc = s_cl_cap ? s_cl_cap * 2 : (1u << 20);
        char* nb = (char*)realloc(s_cl_buf, nc);
        if (!nb) return;
        s_cl_buf = nb; s_cl_cap = nc;
    }
    int k = snprintf(s_cl_buf + s_cl_len, s_cl_cap - s_cl_len, "D%u rt=%x clip=%ux%u n=%u idx=%d",
                     s_cl_n++, rt, cw, ch, n, indexed);
    s_cl_len += (size_t)k;
    for (u32 c = 0; c < RSX_DSP_NUM_CONSTANTS && s_cl_n <= 4; c++) {
        if (!(g.rsx.constants[c][0] | g.rsx.constants[c][1] | g.rsx.constants[c][2] | g.rsx.constants[c][3])) continue;
        if (s_cl_cap - s_cl_len < 128) break;
        float v[4]; memcpy(v, g.rsx.constants[c], 16);
        k = snprintf(s_cl_buf + s_cl_len, s_cl_cap - s_cl_len, " c%u=%g,%g,%g,%g", c, v[0], v[1], v[2], v[3]);
        s_cl_len += (size_t)k;
    }
    s_cl_buf[s_cl_len++] = '\n';
}
static void eng_constlog_frame_end(void)
{
    static u32 big, dumped;
    FILE* f = eng_constlog_file();
    if (!f) return;
    if (s_cl_n > 5000 && dumped < 3 && (big++ % 50) == 0) {
        fprintf(f, "---- frame %u draws=%u ----\n", g.frames, s_cl_n);
        /* Where does the camera live? Find a constant row's xyz (default c268,
         * PS3RECOMP_ENG_CONSTLOG_FIND=<n>) as the guest stored it (big-endian
         * floats) in guest memory. */
        { extern uint8_t* vm_base;
          uint8_t pat[12];
          for (int k = 0; k < 3; k++) {
              static int s_row = -1;
              if (s_row < 0) { const char* e = getenv("PS3RECOMP_ENG_CONSTLOG_FIND"); s_row = e ? atoi(e) : 268; }
              u32 w = g.rsx.constants[s_row][k];
              pat[k*4] = (uint8_t)(w >> 24); pat[k*4+1] = (uint8_t)(w >> 16);
              pat[k*4+2] = (uint8_t)(w >> 8); pat[k*4+3] = (uint8_t)w;
          }
          int hits = 0;
#if defined(__APPLE__)
          /* Only readable host regions: most of the 4 GB window is PROT_NONE. */
          if (vm_base && (pat[0] | pat[1] | pat[4] | pat[5])) {
              mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)vm_base;
              const mach_vm_address_t end = addr + 0x100000000ull;
              while (addr < end && hits < 24) {
                  mach_vm_size_t size = 0; vm_region_basic_info_data_64_t info;
                  mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
                  if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                                     (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
                  if (addr >= end) break;
                  if (info.protection & VM_PROT_READ) {
                      const uint8_t* p0 = (const uint8_t*)(uintptr_t)addr;
                      mach_vm_size_t lim = (addr + size > end ? end - addr : size);
                      for (mach_vm_size_t o = 0; o + 12 <= lim && hits < 24; o += 4)
                          if (p0[o] == pat[0] && !memcmp(p0 + o, pat, 12)) {
                              fprintf(f, "  find-row xyz found at EA 0x%08X\n",
                                      (u32)((uintptr_t)(p0 + o) - (uintptr_t)vm_base));
                              hits++;
                          }
                  }
                  addr += size;
              }
          }
#endif
        }
        fwrite(s_cl_buf, 1, s_cl_len, f);
        fflush(f);
        dumped++;
    }
    s_cl_len = 0; s_cl_n = 0;
}

/* FPSLOG: where presents come from. 0 = the FIFO's flip (sink_flip), 1 = a
 * host call (rsx_draw_engine_present / _present_buffer). */
static int s_present_src;
static u32 s_pres_n[2], s_pres_drew[2];
static u64 s_pres_ns[2];
static u64 eng_ds_now(void);

static void eng_present(u32 buffer_id)
{
    if (!g.ready) return;
    const u32 target = eng_present_surface(buffer_id);
    if (target == ENG_INVALID) return;
    if (getenv("PS3RECOMP_METAL_FRAME_DUMP") && g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture] surface %u loc=%u offset=%08X size=%ux%u handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].handle);
    }
    g.last_present_surface = target;
    /* PS3RECOMP_ENG_PRESENT_OFFSET=<hex>: present the registered surface at
     * that offset instead (as it stands at the end of the frame), to look
     * at an intermediate render target. */
    { static int on = -1; static u32 off;
      if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_PRESENT_OFFSET");
                    on = e ? 1 : 0; if (e) off = (u32)strtoul(e, NULL, 16); }
      if (on) for (u32 i = 0; i < g.n_surfaces; ++i)
          if (g.surfaces[i].offset == off) { g.be->present(g.be->user, g.surfaces[i].handle); goto presented; } }
    /* PS3_ASPECT_HUD_AFTER: a frame of overlay alone (no composite in it or
     * the one before) is presented with black bars beside its 16:9. */
    if (g.be->set_hud && s_hud_n > 0 && g.guest_draws && s_hud_frame == g.frames)
        g.be->set_hud(g.be->user, (!s_hud_after && (!s_hud_seen || s_hud_slice - s_hud_seen > 1u)) ? 3 : 4, 0);
    { const u64 t = eng_ds_now();
      g.be->present(g.be->user, g.surfaces[target].handle);
      const int src = s_present_src;
      s_pres_n[src]++; s_pres_drew[src] += g.guest_draws != 0;
      s_pres_ns[src] += eng_ds_now() - t; }
presented:;
    /* PS3RECOMP_ENG_FPSLOG=1: per 10 s, the frame rate -- presents that drew
     * something since the previous present -- and every present bucketed by
     * how many draws preceded it. A present with no draws re-shows an image
     * the title already presented, so it is not a frame; "world" (>1000
     * draws) separates a full 3D frame from a light 2D one. */
    { static int on = -1; static u32 pf, b0, b1, bw; static time_t t0;
      if (on < 0) on = getenv("PS3RECOMP_ENG_FPSLOG") ? 1 : 0;
      if (on) { pf++;
          if (!g.guest_draws) b0++; else if (g.guest_draws > 1000) bw++; else b1++;
          time_t now = time(NULL); if (!t0) t0 = now;
          if (now - t0 >= 10) {
              const double dt = (double)(now - t0);
              fprintf(stderr, "[rsx fps] %.2f fps (%u frames in %lds: %u world, %u light);"
                      " %u presents, %u with no draws\n",
                      (b1 + bw) / dt, b1 + bw, (long)(now - t0), bw, b1, pf, b0);
              fprintf(stderr, "[rsx fps] presents by source: fifo flip %u (%u drew, %.1f ms in present),"
                      " host %u (%u drew, %.1f ms in present)\n",
                      s_pres_n[0], s_pres_drew[0], s_pres_ns[0] / 1e6,
                      s_pres_n[1], s_pres_drew[1], s_pres_ns[1] / 1e6);
              memset(s_pres_n, 0, sizeof s_pres_n); memset(s_pres_drew, 0, sizeof s_pres_drew);
              memset(s_pres_ns, 0, sizeof s_pres_ns);
              pf = b0 = b1 = bw = 0; t0 = now; } } }
    /* PS3RECOMP_ENG_MEMSNAP=<sec>[/<sec>..]:<dir>:<hexEA>:<hexLen>[,<hexEA>:<hexLen>..]:
     * DIAGNOSTIC. Raw guest bytes of every listed range, written to
     * <dir>/t<sec>_<EA>.bin at each listed time (seconds after the first
     * draw), so heap structures can be walked offline (pointers followed
     * within the dumped ranges) and compared between game states. */
    { static int sinit; static u32 ssec[8], nsec, sdone; static char sdir[400];
      static u32 sea[32], slen[32], nr; static struct timespec st0;
      if (!sinit) { sinit = 1; const char* e = getenv("PS3RECOMP_ENG_MEMSNAP");
          if (e) { char* d = (char*)e;
              while (nsec < 8) { ssec[nsec++] = (u32)strtoul(d, &d, 10); if (*d == '/') d++; else break; }
              if (*d == ':') { d++; const char* c = strchr(d, ':'); size_t n = c ? (size_t)(c - d) : 0;
                  if (n && n < sizeof sdir) { memcpy(sdir, d, n); sdir[n] = 0; d = (char*)c + 1;
                      while (*d && nr < 32) { sea[nr] = (u32)strtoul(d, &d, 16); if (*d != ':') break;
                          slen[nr] = (u32)strtoul(d + 1, &d, 16); nr++; if (*d == ',') d++; else break; } } }
              clock_gettime(CLOCK_MONOTONIC, &st0); } }
      if (nr && sdone < nsec) {
          struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
          const double el = (double)(now.tv_sec - st0.tv_sec) + (double)(now.tv_nsec - st0.tv_nsec) * 1e-9;
          if (el >= (double)ssec[sdone]) {
              extern uint8_t* vm_base;
              for (u32 i = 0; i < nr && vm_base; i++) {
                  char path[512]; snprintf(path, sizeof path, "%s/t%u_%08X.bin", sdir, ssec[sdone], sea[i]);
                  FILE* f = fopen(path, "wb"); if (f) { fwrite(vm_base + sea[i], 1, slen[i], f); fclose(f); } }
              fprintf(stderr, "[rsx memsnap] t=%us: %u range(s) -> %s\n", ssec[sdone], nr, sdir);
              sdone++;
          } } }
    /* PS3RECOMP_ENG_MEMDUMP=<hexEA>:<hexLen>:<worldframe>[:file]: at that world
     * frame, write the guest range as hex words, 16 bytes per line -- the same
     * format as the RPCS3 oracle's RPCS3_LP_DUMP, so the two diff directly.
     * "<n>s" in place of the frame dumps n seconds after the first frame
     * (world-frame counts drift with load times; capture.sh timings do not). */
    { static const char* md = (const char*)-1; static u32 dea, dlen, dat, dn, dsec; static char dpath[512];
      static struct timespec dt0;
      if (md == (const char*)-1) { md = getenv("PS3RECOMP_ENG_MEMDUMP");
          if (md) { char* d; dea = (u32)strtoul(md, &d, 16); dlen = (u32)strtoul(d + 1, &d, 16);
                    dat = (u32)strtoul(d + 1, &d, 10);
                    if (*d == 's') { dsec = dat; dat = 0; d++; }
                    snprintf(dpath, sizeof dpath, "%s", *d == ':' ? d + 1 : "/tmp/our_memdump.txt");
                    clock_gettime(CLOCK_MONOTONIC, &dt0); } }
      int due = 0;
      if (md && dsec && !dn) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
          if ((double)(now.tv_sec - dt0.tv_sec) + (double)(now.tv_nsec - dt0.tv_nsec) * 1e-9 >= (double)dsec) { dn = 1; due = 1; } }
      else if (md && !dsec && g.guest_draws > 1000 && ++dn == dat) due = 1;
      if (due) {
          fprintf(stderr, "[rsx memdump] 0x%08X+0x%X -> %s\n", dea, dlen, dpath);
          extern uint8_t* vm_base;
          FILE* f = fopen(dpath, "w");
          if (f && vm_base) {
              for (u32 o = 0; o < dlen; o += 16) {
                  fprintf(f, "%08x:", dea + o);
                  for (u32 k = 0; k < 16; k += 4) {
                      const uint8_t* q = vm_base + dea + o + k;
                      fprintf(f, " %02x%02x%02x%02x", q[0], q[1], q[2], q[3]);
                  }
                  fprintf(f, "\n");
              }
          }
          if (f) fclose(f);
      } }
    /* PS3RECOMP_ENG_MEMPROBE=<loc>:<hex offset>:<bytes>: on world frames, how
     * much of that guest range is nonzero (is a texture's source populated?). */
    { static const char* me = (const char*)-1; static u32 mloc, moff, mlen;
      if (me == (const char*)-1) { me = getenv("PS3RECOMP_ENG_MEMPROBE");
          if (me) { char* d; mloc = (u32)strtoul(me, &d, 10); moff = (u32)strtoul(d + 1, &d, 16); mlen = (u32)strtoul(d + 1, 0, 0); } }
      if (me && g.guest_draws > 1000) { static u32 n; if ((n++ % 20) == 0) {
          const u8* q = eng_guest_ptr(NULL, mloc, moff, mlen);
          u32 nz = 0, hsh = 2166136261u; if (q) for (u32 k = 0; k < mlen; k++) { nz += q[k] != 0; hsh = (hsh ^ q[k]) * 16777619u; }
          fprintf(stderr, "[rsx memprobe] world frame %u: loc %u off 0x%08X: %u/%u nonzero hash %08X first %02X%02X%02X%02X%s\n",
                  n - 1, mloc, moff, nz, mlen, hsh, q && mlen >= 4 ? q[0] : 0, q && mlen >= 4 ? q[1] : 0,
                  q && mlen >= 4 ? q[2] : 0, q && mlen >= 4 ? q[3] : 0, q ? "" : " (unmapped)"); } } }
    /* PS3RECOMP_ENG_PROBE=<hex offset>[,<file.ppm>]: on world frames, read
     * back the newest target at that guest address and print its mean --
     * handles are not stable across runs, addresses are. */
    { static const char* pe = (const char*)-1; static u32 paddr; static const char* ppath;
      if (pe == (const char*)-1) { pe = getenv("PS3RECOMP_ENG_PROBE");
          if (pe) { char* d; paddr = (u32)strtoul(pe, &d, 16); ppath = (*d == ',') ? d + 1 : NULL; } }
      if (pe && g.guest_draws > 1000 && g.be->readback) {
          static u32 nprobe;
          if ((nprobe++ % 20) == 0) {
              u32 best = ENG_INVALID;
              for (u32 i = 0; i < g.n_surfaces; i++)
                  if (g.surfaces[i].handle && g.surfaces[i].offset == paddr &&
                      (best == ENG_INVALID || g.surfaces[i].w * g.surfaces[i].h > g.surfaces[best].w * g.surfaces[best].h)) best = i;
              if (best != ENG_INVALID) {
                  const u32 w = g.surfaces[best].w, h = g.surfaces[best].h;
                  u8* px = (u8*)malloc((size_t)w * h * 4);
                  if (px) {
                      g.be->readback(g.be->user, g.surfaces[best].handle, 0, 0, w, h, px, w * 4);
                      unsigned long long sum = 0;
                      for (u32 k = 0; k < w * h; k++) sum += px[k * 4] + px[k * 4 + 1] + px[k * 4 + 2];
                      fprintf(stderr, "[rsx probe] world frame %u: 0x%08X %ux%u fmt %d mean rgb %.2f\n",
                              nprobe - 1, paddr, w, h, (int)g.surfaces[best].fmt, (double)sum / (3.0 * w * h));
                      if (ppath) { FILE* f = fopen(ppath, "wb");
                          if (f) { fprintf(f, "P6\n%u %u\n255\n", w, h);
                              for (u32 k = 0; k < w * h; k++) fwrite(px + k * 4, 1, 3, f); fclose(f); } }
                      free(px);
                  }
              }
          } } }
    eng_constlog_frame_end();
    /* PS3RECOMP_ENG_WORLD_MARK=<path>: create the file on the first world
     * frame (a frame with more than 2500 guest draws), for PAD_SCRIPT_WAIT. */
    { static const char* mark = (const char*)-1; static int done; static long after; static unsigned long long t0;
      if (mark == (const char*)-1) { mark = getenv("PS3RECOMP_ENG_WORLD_MARK");
          /* PS3RECOMP_ENG_WORLD_MARK_AFTER=<sec>: not before that many seconds
           * after the first present -- the in-engine cutscene that opens the
           * save is a world frame too; the pad script's X presses end it. */
          const char* a = getenv("PS3RECOMP_ENG_WORLD_MARK_AFTER"); after = a ? atol(a) : 0; }
      if (mark && !done) {
          extern unsigned long long ps3_ms_now(void);
          if (!t0) t0 = ps3_ms_now();
          if (g.guest_draws > 2500 && ps3_ms_now() - t0 >= (unsigned long long)after * 1000ull) {
              done = 1; FILE* f = fopen(mark, "w"); if (f) fclose(f);
              fprintf(stderr, "[rsx engine] world mark: frame %u, %u draws -> %s\n", g.frames, g.guest_draws, mark); } } }
    g.frames++;
    if (rsx_wt_mode()) rsx_wt_frame(g.frames);
    s_icon_gen = rsx_button_icons_frame();
    eng_ds_frame_end(g.guest_draws);
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    if (!g.be) return -1;
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.flip             = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);
    s_vpgen_memo = NULL;   /* PS3RECOMP_ENG_VP_GEN: vp_gen starts over */
    /* The game's post effects the PC menu turns off (ENG_DOF_BLEND_FP). */
    ps3_setting_register("PS3RECOMP_DOF", eng_fx_dof_apply);
    ps3_setting_register("PS3RECOMP_EDGE_BLUR", eng_fx_edge_apply);
    g.ready = 1;
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    if (!g.be) return;
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    rsx_draw_engine_set_guest_memory(NULL, NULL);
    free(dc.refs); free(dc.cuts); free(dc.verts);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
    eng_tidx_rebuild();
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(&g.rsx, m, arg);
}

/* rsx_draw_engine_method for a whole header (cellGcmSys's walker: only for
 * methods whose mask leaves them unchanged over the burst, < 0x2000). */
void rsx_draw_engine_burst(u32 method, int incr, const u32* be_words, u32 count)
{
    if (!g.ready) return;
    rsx_dispatch_burst(&g.rsx, method & 0x1FFCu, incr, be_words, count);
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

/* ---- GPU -> guest memory ------------------------------------------------- */

/* Host R,G,B,A rows into the guest's A8R8G8B8 (bytes A,R,G,B, the big-endian
 * 0xAARRGGBB word). The depth decode uses the same shuffle: its RGBA8 is
 * a = D[23:16], r = D[15:8], g = D[7:0], b = stencil, which lands as the
 * big-endian Z24S8 word D << 8 | S. */
static void eng_rgba_to_guest_argb(u8* dst, u32 dst_pitch, const u8* src,
                                   u32 w, u32 h)
{
    for (u32 y = 0; y < h; y++) {
        const u8* sp = src + (size_t)y * w * 4u;
        u8* dp = dst + (size_t)y * dst_pitch;
        for (u32 x = 0; x < w; x++, sp += 4, dp += 4) {
            dp[0] = sp[3]; dp[1] = sp[0]; dp[2] = sp[1]; dp[3] = sp[2];
        }
    }
}

int rsx_draw_engine_write_back(int local, u32 offset, u32 pitch)
{
    return rsx_draw_engine_write_back_to(local, offset, pitch, -1, 0);
}

int rsx_draw_engine_write_back_to(int local, u32 offset, u32 pitch, int dst_local, u32 dst_offset)
{
    s_gl_wb_skipped = 0;
    if (!g.ready || !g.be->readback || !g.be->submit_and_wait) return 0;
    if (eng_gpu_light_blit(local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN, offset, pitch,
                           dst_local, dst_offset)) { s_gl_wb_skipped = 1; return 0; }
    /* PS3RECOMP_ENG_WRITEBACK=0 turns the write-back off, 1 writes colour
     * targets only, 2 depth only (bisecting); unset or 3 is both. */
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_ENG_WRITEBACK"); on = e ? atoi(e) : 3; }
    if (!on) return 0;
    const u32 location = local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    int wrote = 0;
    static int log_on = -1, logged;
    if (log_on < 0) log_on = getenv("PS3RECOMP_ENG_TEXLOG") ? 1 : 0;

    /* The newest colour target that starts at the offset, or within whose
     * first row it lies: a blit may start inside a row (inFAMOUS 2 copies its
     * 1280-wide G-buffer as 1024 + 256 columns) but never deeper, and a looser
     * match let the phantom 1280x720 target at offset 0 (the zeroed surface
     * registers at boot) claim the depth buffer at 0x10000 and overwrite VRAM
     * 0..0x384000. Only 8-bit targets are stored the way the guest expects. */
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* sf = &g.surfaces[i];
        if (!sf->handle || sf->location != location || sf->fmt != RSX_BE_FMT_R8G8B8A8) continue;
        const u32 row = pitch ? pitch : sf->w * 4u;
        if (offset < sf->offset || offset >= sf->offset + row) continue;
        if (best == ENG_INVALID || sf->stamp > g.surfaces[best].stamp) best = i;
    }
    if ((on & 1) && best != ENG_INVALID && g.surfaces[best].wb_stamp != g.surfaces[best].stamp) {
        eng_surface* sf = &g.surfaces[best];
        const u32 row = pitch ? pitch : sf->w * 4u;
        u8* dst = (u8*)eng_guest_ptr(NULL, location, sf->offset, row * sf->h);
        if (dst && eng_staging_reserve(sf->w * sf->h * 4u)) {
            g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
            g.be->readback(g.be->user, sf->handle, 0, 0, sf->w, sf->h, g.tex_staging, sf->w * 4u);
            eng_rgba_to_guest_argb(dst, row, g.tex_staging, sf->w, sf->h);
            sf->wb_stamp = sf->stamp;
            wrote = 1;
            if (log_on && logged < 40) { logged++;
                fprintf(stderr, "[rsx engine] write-back colour %u:0x%08X %ux%u pitch %u -> guest\n",
                        location, sf->offset, sf->w, sf->h, row); }
        }
    }

    /* The depth buffer at the offset, stored as the RSX stores Z24S8. */
    for (u32 i = 0; (on & 2) && i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (!z->handle || !z->had_write || z->location != location) continue;
        const u32 row = pitch ? pitch : z->w * 4u;
        if (offset < z->offset || offset >= z->offset + row) continue;
        if (z->wb_stamp == z->stamp) break;
        z->snapshot_rgba_valid = 0; z->crop_valid = 0;   /* the decode must follow the latest depth write */
        const u32 snap = eng_zdepth_snapshot_rgba8(i);   /* queues the decode */
        u8* dst = (u8*)eng_guest_ptr(NULL, location, z->offset, row * z->h);
        if (!snap || !dst || !eng_staging_reserve(z->w * z->h * 4u)) break;
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, snap, 0, 0, z->w, z->h, g.tex_staging, z->w * 4u);
        eng_rgba_to_guest_argb(dst, row, g.tex_staging, z->w, z->h);
        /* This decode served the write-back only: a texture consumer later in
         * the frame must resolve at its own point in the stream, after the
         * depth written in between, as it did before. */
        z->snapshot_rgba_valid = 0; z->crop_valid = 0;
        z->wb_stamp = z->stamp;
        wrote = 1;
        if (log_on && logged < 40) { logged++;
            fprintf(stderr, "[rsx engine] write-back depth %u:0x%08X %ux%u pitch %u -> guest\n",
                    location, z->offset, z->w, z->h, row); }
        break;
    }
    return wrote;
}

void rsx_draw_engine_flush(void)
{
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    /* A host that drives the flip itself gets whichever buffer the guest's
     * own last flip named, and display buffer 0 before there has been one:
     * a runner that both consumes 0xE944 and calls here would otherwise
     * re-present a double-buffered title's OTHER scanout. Presenting twice is
     * harmless either way -- the engine never clears the surface, so a second
     * present just blits the same image again. */
    s_present_src = 1;
    eng_present(g.last_flip_buffer);
    s_present_src = 0;
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
    s_present_src = 1;
    eng_present(g.last_flip_buffer);
    s_present_src = 0;
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

u32 rsx_draw_engine_readback_center(void)
{
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}
