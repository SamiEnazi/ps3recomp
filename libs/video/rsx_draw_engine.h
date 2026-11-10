/*
 * ps3recomp - platform-neutral RSX draw engine over the register file
 *
 * The vtable backends in rsx_commands.c are driven by rsx_state, a curated
 * dirty-flag struct that models no primitive restart, no vertex textures and
 * no two-sided stencil, and whose setters fire only at SET_BEGIN_END. The
 * register-file dispatcher in rsx_dispatch.c has all of it already, in
 * portable C, behind the rsx_dispatch_sink interface. This engine implements
 * that sink and drives a backend through the record-oriented interface below,
 * so a title's frame reaches the host API the way the hardware issued it.
 *
 * The engine logic is carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has actually shipped on. That file is
 * vendored and is NOT touched: what moves here is the orchestration, and the
 * comments name the reference line numbers so the two can be compared. The
 * mathematics is already shared and already portable -- rsx_vertex_compact.c,
 * rsx_restart_cuts.h, rsx_texture_layout.c, the two decompilers.
 *
 * The split between engine and backend is semantic, at the granularity the
 * engine thinks in, not a 1:1 wrapper over one host API's calls: resources by
 * what they are for, pipelines from the two HLSL strings plus render state
 * plus the vertex layout, and per-draw binding of what a draw names. Resource
 * state tracking, descriptor lifetime and command encoding belong entirely to
 * the backend, which is why nothing here mentions a barrier.
 *
 * Handles are small integers owned by the backend; 0 always means "none".
 */
#ifndef PS3RECOMP_RSX_DRAW_ENGINE_H
#define PS3RECOMP_RSX_DRAW_ENGINE_H

#include <stdio.h>   /* FILE: rsx_lim_summary */
#include "rsx_dispatch.h"
#include "rsx_primitives.h"
#include "rsx_texture_layout.h"
#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the engine hands a backend ------------------------------------ */

/* Surface and texture formats the engine asks for. The texture side is
 * rsx_texfmt's classification, which rsx_texture_decode already produces host
 * rows for, so a backend maps one enum rather than two. */
typedef enum {
    RSX_BE_FMT_NONE = 0,
    RSX_BE_FMT_R8,
    RSX_BE_FMT_R8G8,
    RSX_BE_FMT_R8G8B8A8,      /* decoded colour: always R,G,B,A byte order */
    RSX_BE_FMT_BC1,
    RSX_BE_FMT_BC2,
    RSX_BE_FMT_BC3,
    RSX_BE_FMT_R16,
    RSX_BE_FMT_R16G16,
    RSX_BE_FMT_R16G16F,
    RSX_BE_FMT_R16G16B16A16F, /* SET_SURFACE_FORMAT 0xB, the HDR target    */
    RSX_BE_FMT_R32F,
    RSX_BE_FMT_R32G32B32A32F
} rsx_be_format;

/* One texture unit's sampler, decoded out of SET_TEXTURE_FILTER,
 * SET_TEXTURE_ADDRESS and SET_TEXTURE_CONTROL0 the way rsx_live_draw.c's
 * decode_sampler (1464-1487) reads them. The wrap fields stay as the guest's
 * own 1..8 codes because that is what both backends already translate. */
typedef struct rsx_be_sampler_desc {
    u8  min_linear, mag_linear, mip_linear, mip_present;
    u8  wrap_s, wrap_t, wrap_r;
    /* What BORDER-type wraps (4, 5, 7, 8) return outside 0..1, reduced to
     * the colours every backend can express: SET_TEXTURE_BORDER_COLOR read
     * as the unit's format reads it (a depth format takes its 24-bit depth
     * from the low bits). 0 transparent black (also the old behaviour),
     * 1 opaque black, 2 opaque white, 3 any other colour: the backend clamps
     * to the edge texel instead, which is nearer than black for the colours
     * seen (inFAMOUS 2's video chroma planes border at 0x80 = neutral). */
    u8  border;
    float min_lod, max_lod;
} rsx_be_sampler_desc;

/* The NV4097 render state a pipeline is built from: rsx_live_draw.c's
 * render_state_t (1312-1327) with the D3D12 enums left undecoded, since the
 * guest values are the portable form and each backend has its own table.
 * The stencil REFERENCE is deliberately absent -- it is set on the encoder,
 * not baked into the pipeline, and so is not part of the pipeline key. */
typedef struct rsx_be_render_state {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    /* Targets 1..3 of an MRT set: COLOR_MASK_MRT (0x0370), one nibble per
     * target at [4i..4i+3] = A,R,G,B, and BLEND_ENABLE_MRT (0x036C), bit i.
     * Target 0 keeps color_mask/blend_enable. Both reset to 0 (RPCS3). */
    u32 color_mask_mrt, blend_mrt;
    u32 rt_fp16;                    /* the target is an FP16 HDR surface */
    u32 stencil_enable, stencil_two_sided;
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
} rsx_be_render_state;

/* submit_and_wait reasons, for a backend that wants to log or count them. */
#define RSX_BE_FLUSH_VERTEX_RING     0
#define RSX_BE_FLUSH_GUEST_REFERENCE 1
#define RSX_BE_FLUSH_SHUTDOWN        2

/* clear_depth_stencil flags */
#define RSX_BE_CLEAR_DEPTH   0x1u
#define RSX_BE_CLEAR_STENCIL 0x2u

#define RSX_BE_MAX_TEXTURES        RSX_DSP_NUM_TEXTURES
#define RSX_BE_MAX_VERTEX_TEXTURES RSX_DSP_NUM_VERTEX_TEXTURES
/* Colour targets a draw can name: A alone, or an MRT set of A, B, C and D. */
#define RSX_BE_MAX_COLOR_TARGETS   4

/* How many distinct guest textures the engine keeps before it evicts the
 * least recently used one. Public so the eviction test can be exact about
 * where the boundary is rather than assuming a number.
 * (perf, 2026-11-09) 1024 -> 4096: inFAMOUS 2's city seen from above binds more
 * than 1024 distinct textures a frame, and an LRU smaller than the working set
 * misses on every bind -- every frame re-hashed, re-decoded, re-uploaded and
 * re-created every texture (Sami's 55 s at 4-5 fps). The same wall was found
 * and fixed this way in ThiagoFraga761/infamous2-pc (MIT). This is the maximum;
 * PS3RECOMP_ENG_TEXCACHE=<n> runs with fewer (1024 = the old size). */
#define RSX_DRAW_ENGINE_TEXTURE_CACHE 4096

/* Per-frame attribution of slow frames (PS3RECOMP_PLAYLOG's HITCH lines):
 * counters the RSX thread adds to between two presents; the Vulkan backend's
 * play log takes them (and clears the counts) at every present. */
typedef struct rsx_hattr {
    unsigned long long tex_new;     /* cache misses that created a texture       */
    unsigned long long tex_evict;   /* of those, evictions of a cached one       */
    unsigned long long tex_reup;    /* cached textures whose guest bytes changed  */
    unsigned long long tex_bytes;   /* guest bytes those decoded and uploaded     */
    unsigned long long tex_ns;      /* time in the decode + upload of both        */
    unsigned long long hash_ns;     /* full content hashes of new textures        */
    unsigned long long mod_n;       /* texture-pack replacements read from disk   */
    unsigned long long mod_ns;
    unsigned long long tex_cached;  /* gauge: entries in the cache at the present */
} rsx_hattr;
extern rsx_hattr g_rsx_hattr;

/* ---- PS3RECOMP_LIMITS: the renderer's capacities and how close a run came --
 * (rsx5, 2026-11-09, Sami: "stability more than fps" -- the 1024-entry
 * texture cache once turned the city into a 4-10 fps slideshow.) Every
 * fixed-size or capacity-limited structure of the engine and the Vulkan
 * backend has an entry: its capacity (0 = grows without a set bound), the
 * use now, the peak, and the events that mean it was not enough --
 * `full` (a refusal: work dropped, an object not made, a stall to make room)
 * and `evict` (a live entry pushed out to make room, a re-creation later).
 * Cheap: the use is stored where the structure changes, events are counted.
 * The first events of each kind are logged once each ([limits] lines);
 * PS3RECOMP_LIMITS=1 prints a one-line summary every 5 s, and one summary
 * line is always printed at exit. A frame in which an event happened names
 * the structure on its PS3RECOMP_PLAYLOG HITCH line (lim=), and a LIMIT line
 * per second carries the counts even when the frame was not slow. */
enum {
    RSX_LIM_TEXCACHE, RSX_LIM_TEXEVICT_RECENT, RSX_LIM_SURFACES, RSX_LIM_ZDEPTHS, RSX_LIM_PIPELINES,
    RSX_LIM_VCACHE, RSX_LIM_VCACHE_MB, RSX_LIM_ICACHE, RSX_LIM_ICACHE_MB,
    RSX_LIM_DRAW_REFS, RSX_LIM_DRAW_BATCHES, RSX_LIM_INLINE, RSX_LIM_IMM, RSX_LIM_GPU_LIGHTS, RSX_LIM_DROPS,
    RSX_LIM_VK_OBJ, RSX_LIM_VK_PIPE, RSX_LIM_VK_FB, RSX_LIM_VK_SAMP, RSX_LIM_VK_DSETS, RSX_LIM_VK_DEAD_IMG,
    RSX_LIM_VK_DEAD_FB, RSX_LIM_VK_DEAD_BUF, RSX_LIM_VK_VB, RSX_LIM_VK_ARENA_V, RSX_LIM_VK_ARENA_I,
    RSX_LIM_VK_ARENA_U, RSX_LIM_VK_ARENA_S, RSX_LIM_VK_ARENA_G, RSX_LIM_VK_CSNAP, RSX_LIM_VK_ZRGBA,
    RSX_LIM_VK_FBFIN, RSX_LIM_VK_TQ_RING, RSX_LIM_VK_MIDFLUSH, RSX_LIM_VK_IMGPOOL,
    RSX_LIM_GCM_IOMAP, RSX_LIM_GCM_REFQ, RSX_LIM_GCM_UPLOADS, RSX_LIM_GCM_TABLES,
    RSX_LIM_COUNT
};
typedef struct rsx_lim {
    const char* name;
    unsigned long long cap;          /* 0: no fixed bound (grows) */
    unsigned long long cur, peak;
    unsigned long long full, evict;  /* events (see above) */
    unsigned long long frame_ev;     /* events since the last present (HITCH lim=) */
} rsx_lim;
extern rsx_lim g_rsx_lim[RSX_LIM_COUNT];
static inline void rsx_lim_use(int id, unsigned long long cur)
{
    rsx_lim* l = &g_rsx_lim[id];
    l->cur = cur;
    if (cur > l->peak) l->peak = cur;
}
static inline void rsx_lim_cap(int id, unsigned long long cap) { g_rsx_lim[id].cap = cap; }
/* kind 0 = full (refused / dropped / stalled), 1 = evict (a live entry pushed
 * out). `what` is logged with the first events of each structure. */
void rsx_lim_event(int id, int kind, const char* what);
/* Per present (the Vulkan backend's): the PS3RECOMP_LIMITS summary when due.
 * rsx_lim_take_frame: the names of the structures with events since the last
 * call ("" if none) -- the HITCH line's lim= field. */
void rsx_lim_frame(void);
const char* rsx_lim_take_frame(char* buf, unsigned n);
/* One summary line (also registered with atexit by the engine's init). */
void rsx_lim_summary(FILE* f, const char* why);
/* (rsx5, round 3) The same line into buf (no newline); returns its length. */
int rsx_lim_format(char* buf, int size, const char* why);
/* (rsx5, round 3) Where the engine's own log lines ([limits]) go when a backend
 * set it: the Vulkan backend's background writer (PS3RECOMP_LOG_ASYNC), so a
 * slow stderr -- a pipe whose reader is behind, a log on a busy disk -- never
 * holds the RSX thread. NULL: written to stderr directly, as before. Each call
 * is one whole line, '\n' included. */
extern void (*g_rsx_log_hook)(const char* line, unsigned n);
/* (minimap) PS3RECOMP_ENG_HUDLOG_KEY=1: a host key (the Vulkan window's F9, the
 * playlog MARK) sets this; the engine then logs the next two frames' draws into
 * the display buffers in PS3RECOMP_ENG_HUDLOG's format and clears it. */
extern volatile long g_rsx_hudlog_key;

/* ---- the backend interface ---------------------------------------------- */

typedef struct rsx_draw_backend {
    void* user;

    /* Lifecycle and submission. submit_and_wait is fully synchronous: the
     * engine recycles its staging the moment it returns, which is the same
     * contract as rsx_live_draw.c's ld_flush (1659-1736). */
    int  (*init)(void* user, u32 width, u32 height);
    void (*shutdown)(void* user);
    void (*submit_and_wait)(void* user, u32 reason);

    /* Textures. `remap` is the raw TEXTURE_CONTROL1 crossbar and `rsx_fmt`
     * the SET_TEXTURE_FORMAT byte it is decoded against, because the crossbar
     * is a property of the view rather than of the pixels. Levels arrive one
     * upload each, already decoded to host rows by rsx_texture_decode. */
    u32  (*texture_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                           u32 mips, u32 faces, u32 remap, u32 rsx_fmt);
    void (*texture_upload)(void* user, u32 texture, u32 face, u32 mip,
                           u32 w, u32 h, const void* src, u32 row_bytes,
                           u32 rows);
    void (*texture_release)(void* user, u32 texture);

    /* Colour targets. `seed` is the guest's own bytes for the surface,
     * decoded to R,G,B,A rows, or NULL when they could not be resolved: a
     * title CPU-initialises its render targets and then samples them before
     * anything draws into them, which is why the D3D12 backend's off_rt_get
     * seeds a new target the same way. */
    u32  (*color_target_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                                const void* seed, u32 seed_row_bytes);
    void (*color_target_release)(void* user, u32 surface);
    /* A texture handle for a colour target a texture unit names, wearing that
     * unit's TEXTURE_CONTROL1 crossbar. An uploaded texture bakes the crossbar
     * in when it is decoded, so only a surface bind needs this; 0 means the
     * backend has no view and the engine binds the target itself. */
    u32  (*surface_view)(void* user, u32 surface, u32 remap, u32 rsx_format);

    /* One depth target per guest zeta address. */
    u32  (*depth_target_create)(void* user, u32 w, u32 h);
    void (*depth_target_release)(void* user, u32 depth);
    /* Resolve a depth target into a sampleable texture and return it; 0 when
     * the backend cannot. The engine only asks after a depth-writing draw. */
    u32  (*depth_snapshot)(void* user, u32 depth, u32 w, u32 h);
    /* A sampleable copy of a colour target as it stands at this point in the
     * stream, for a draw that samples a surface it is also writing (a
     * read-modify-write post pass: the RSX reads the old texel). 0 when the
     * backend cannot; the engine then binds nothing better than before. */
    u32  (*color_snapshot)(void* user, u32 surface);
    /* A depth target decoded the way the RSX returns one sampled through a
     * colour format (A8R8G8B8 over a Z24S8 buffer): RGBA8 with a = D[23:16],
     * r = D[15:8], g = D[7:0], b = stencil -- RPCS3 decode_depth24. */
    u32  (*depth_snapshot_rgba8)(void* user, u32 depth, u32 w, u32 h);

    /* Pipelines, from the decompilers' HLSL plus the state that selects a
     * variant. This is the one call that hides D3DCompile against glslang
     * plus spirv-cross plus -newLibraryWithSource:, and the input layout with
     * it. 0 means the pair could not be built; the engine caches that.
     *
     * `rt_count` is how many colour attachments the pass will have, and every
     * one of them takes target A's blend and colour mask: a zero-initialised
     * secondary attachment writes nothing at all, which would leave a
     * deferred pass with only its first G-buffer plane. It is part of the
     * engine's pipeline key, because a host API matches a pipeline's
     * attachments against the pass it is encoded in. */
    u32  (*pipeline_create)(void* user, const char* vs_hlsl, const char* ps_hlsl,
                            const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            u32 vertex_stride, rsx_be_format rt_fmt,
                            u32 rt_count);
    void (*pipeline_release)(void* user, u32 pipeline);

    /* Per-draw binding. The backend owns every ring the D3D12 engine used to
     * own itself, so the engine hands over CPU-side arrays and never mentions
     * a descriptor. */
    /* The colour targets this draw writes, and the depth attachment under
     * them. `count` is 1 for an ordinary draw and up to
     * RSX_BE_MAX_COLOR_TARGETS for an MRT set, whose members are all target
     * A's size and format -- the rule both render paths bind by, and the one
     * a host API enforces on a pass's attachments. */
    void (*bind_targets)(void* user, const u32* surfaces, u32 count, u32 depth);
    void (*bind_pipeline)(void* user, u32 pipeline);
    void (*bind_vs_constants)(void* user, const void* data, u32 bytes);
    void (*bind_ps_constants)(void* user, const void* data, u32 bytes);
    void (*bind_textures)(void* user, const u32* textures,
                          const rsx_be_sampler_desc* samplers, u32 mask);
    void (*bind_vertex_textures)(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask);
    void (*set_viewport)(void* user, float x, float y, float w, float h);
    void (*set_scissor)(void* user, u32 x, u32 y, u32 w, u32 h);
    void (*set_stencil_ref)(void* user, u32 ref);
    /* One draw. `indices` is NULL for a non-indexed one. Everything the
     * hardware has and the host does not -- quads, quad strips, fans,
     * polygons -- has already been expanded into a triangle list by the time
     * it gets here, so the topology is only ever one a host API has. */
    void (*draw)(void* user, rsx_topology topology, const void* vertices,
                 u32 vertex_count, u32 stride, const u32* indices,
                 u32 index_count);

    /* Clears, which are ordered against draws rather than folded into a pass:
     * a title draws the world, clears depth, and draws the weapon over it. */
    void (*clear_color)(void* user, u32 surface, const float rgba[4]);
    void (*clear_depth_stencil)(void* user, u32 depth, u32 flags,
                                float depth_value, u8 stencil);

    /* Present the named surface, and read a rectangle of one back. The rows
     * come back in the format the target was created with, so R,G,B,A for an
     * ordinary colour surface. */
    void (*present)(void* user, u32 surface);
    void (*readback)(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                     void* out, u32 out_pitch);

    /* Optional. bind_vs_constants for a block of `total_bytes` (the full
     * VPConst layout) of which only rows [first, first + count) of `rows` and
     * the `tail_bytes` at row 512 (the viewport epilogue) are read by the
     * bound program, so only those need to reach the GPU. A backend that
     * leaves this NULL gets the whole block through bind_vs_constants. */
    void (*bind_vs_constants_range)(void* user, const void* rows, u32 first,
                                    u32 count, const void* tail,
                                    u32 tail_bytes, u32 total_bytes);

    /* Optional. 3D textures (SET_TEXTURE_FORMAT dimension 3, CONTROL3 depth):
     * a volume of `depth` slices per level, each slice one upload of decoded
     * rows. A backend that leaves these NULL gets the first slice as a 2D
     * texture through texture_create/texture_upload, as before. */
    u32  (*texture_create_3d)(void* user, rsx_be_format fmt, u32 w, u32 h,
                              u32 depth, u32 mips, u32 remap, u32 rsx_fmt);
    void (*texture_upload_3d)(void* user, u32 texture, u32 z, u32 mip,
                              u32 w, u32 h, const void* src, u32 row_bytes);

    /* Optional. A title's deferred light pass done on the GPU (see
     * rsx_draw_engine_gpu_light_data): reads a copy of the normals target and
     * a resolved depth, writes the diffuse and specular buffers in the title's
     * byte encoding into two backend-owned RGBA8 textures, recorded in stream
     * order. `params` is rsx_gpu_light_params. Returns the diffuse texture and
     * stores the specular one; 0 when the backend cannot. */
    u32  (*light_pass)(void* user, u32 normals, u32 depth, u32 w, u32 h,
                       const void* params, u32 param_bytes, u32* spec_out);
    /* Optional. A title's SSAO pipeline on the GPU (rsx_draw_engine_gpu_ssao_data):
     * from the resolved depth and the normals copy, through the backend's own
     * passes, into a backend-owned texture whose every channel holds the AO
     * byte (so any crossbar reads it). Returns it, or 0. */
    u32  (*ssao_pass)(void* user, u32 depth, u32 normals, const void* consts, u32 const_bytes,
                      u32 full_w, u32 full_h, u32 half_w, u32 half_h);
    /* Optional. Persistent vertex buffers: the engine keeps the decoded
     * vertices of geometry that did not change since the last frame and draws
     * from them instead of fetching and staging it again. vbuf_create copies
     * `bytes` and returns a handle (0 when it cannot); a released handle stays
     * valid for draws already recorded. draw_vbuf is draw() with the vertices
     * taken from such a buffer. */
    u32  (*vbuf_create)(void* user, const void* data, u32 bytes);
    void (*vbuf_release)(void* user, u32 vbuf);
    void (*draw_vbuf)(void* user, rsx_topology topology, u32 vbuf,
                      u32 vertex_count, u32 stride, const u32* indices,
                      u32 index_count);
    /* Optional. Image files for texture packs (PS3_MODS) and their dump
     * (PS3_MODS_DUMP). image_load reads a PNG into malloc'd, tightly packed
     * R,G,B,A rows with straight (not premultiplied) alpha, the bytes as the
     * file holds them (no colour management); the caller frees them.
     * image_save writes such rows as a PNG; it copies them and may finish
     * later. Both return 1 on success. A backend that leaves them NULL has
     * no texture packs. */
    int  (*image_load)(void* user, const char* path, u8** rgba, u32* w, u32* h);
    int  (*image_save)(void* user, const char* path, const u8* rgba,
                       u32 w, u32 h, u32 row_bytes);
    /* Optional. PS3_ASPECT (anamorphic wide targets): what the draw whose
     * viewport and scissor follow is. `hud` 1: the title's 2D overlay drawn
     * into a display-sized target `target_w` guest pixels wide, which the
     * backend keeps at 16:9, centred, instead of stretching it with the 3D
     * picture; 2: a 3D-frame draw into a display-sized target (it fills the
     * wide picture); 0: anything else. Called before set_viewport on every
     * draw (PS3_ASPECT_HUD_AFTER, rsx_draw_engine.c). Before a present that
     * drew: 3 when the frame was overlay alone (a menu, a comic, a movie:
     * black bars beside its 16:9 picture), 4 otherwise. */
    void (*set_hud)(void* user, int hud, u32 target_w);
    /* Optional. The draw's polygon offset (SET_POLY_OFFSET_FILL_ENABLE,
     * SET_POLYGON_OFFSET_SCALE_FACTOR / _BIAS), 0/0 when off: `constant` in
     * units of the guest depth format's resolution (2^-24 for D24), `slope`
     * times the primitive's depth slope. Encoder state, like set_stencil_ref:
     * called before every draw, never part of the pipeline key. */
    void (*set_depth_bias)(void* user, float constant, float slope);
    /* Optional. 1: clamp the draw's depth to [0, 1] instead of clipping at
     * the near/far planes (NV4097_SET_ZMIN_MAX_CONTROL, see
     * eng_apply_depth_clip). Encoder state, called before every draw. */
    void (*set_depth_clamp)(void* user, int clamp);
    /* Optional. draw_vbuf with the indices taken from a persistent buffer
     * as well (a vbuf_create'd copy of index_count u32 indices): the
     * engine's resident index lists (PS3RECOMP_ENG_ICACHE). */
    void (*draw_vbuf_ib)(void* user, rsx_topology topology, u32 vbuf,
                         u32 vertex_count, u32 stride, u32 ibuf, u32 index_count);
    /* Optional (rsx5). A hint: the draw being prepared will name this
     * persistent buffer (draw_vbuf / draw_vbuf_ib); the backend may prefetch
     * its bookkeeping. No effect on what is drawn (PS3RECOMP_ENG_PREFETCH). */
    void (*vbuf_prefetch)(void* user, u32 vbuf);
    /* Optional (rsx-vtx). Called before bind_vs_constants_range with the
     * generation of the rows it will name (rsx_dispatch.const_gen): an equal
     * generation means the same row contents as at an earlier bind, so a
     * backend may keep using the copy it made then (PS3RECOMP_VK_CONST_REUSE). */
    void (*vs_constants_gen)(void* user, u32 gen);
} rsx_draw_backend;

/* The light pass's constants, as the backend's shader reads them. */
/* (rsx5) 128 before: the city's light list reached 92 (limits audit) --
 * lights past the bound go unlit. The shader's array is VK_LIGHT_GLSL's l[]. */
#define RSX_GPU_LIGHT_MAX 512
typedef struct {
    float m[16];            /* depth -> camera-relative position: h = u*m[0..3] + v*m[4..7] + z*m[8..11] + m[12..15] */
    float scale;            /* colour multiplier (inFAMOUS 2: 1.6) */
    float count;            /* lights used */
    float width, height;
    struct {
        float pos[3], type;             /* type 1 point, 2 spot */
        float col[3], inner;
        float outer, cone_a, cone_b, near_;
        float dir[3], pad;
    } light[RSX_GPU_LIGHT_MAX];
} rsx_gpu_light_params;

/* ---- the engine --------------------------------------------------------- */

/* Register the backend the engine drives. Passing NULL unregisters it. */
void rsx_draw_engine_set_backend(const rsx_draw_backend* backend);

/* Is the engine the path this run takes? True when PS3RECOMP_RSX_ENGINE names
 * "dispatch" (or the backend asked for it by default) and a backend has been
 * registered. Read once and cached, so it is cheap in the FIFO walker. */
int  rsx_draw_engine_enabled(void);

/* Called by a backend that wants the engine to be the default path with no
 * environment set. PS3RECOMP_RSX_ENGINE=vtable still turns it off. */
void rsx_draw_engine_set_default(int on);

/* A runner with its own RSX IO map supplies its address resolver before init.
 * NULL restores the toolkit's cellGcm mapping. */
void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user);

int  rsx_draw_engine_init(u32 width, u32 height);
void rsx_draw_engine_shutdown(void);

/* One FIFO method, with its subchannel bits still on: the engine masks them
 * out itself, as rsx_live_draw_method (6902-6976) does, because a title whose
 * SPU-built command lists bind NV4097 to another subchannel would otherwise
 * write its state into the wrong register bank. */
void rsx_draw_engine_method(u32 method, u32 arg);
/* count words of one FIFO header (guest byte order): method + 4 i (incr) or
 * method each time; method + 4 (count - 1) must stay below 0x2000. */
void rsx_draw_engine_burst(u32 method, int incr, const u32* be_words, u32 count);

/* cellGcmSetDisplayBuffer, so the flip can resolve a buffer id to a surface
 * the engine has actually rendered rather than to whatever is bound. */
void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height);

/* The guest's SET_REFERENCE sync point: submit and wait, so a title spinning
 * on its fence sees the GPU catch up. */
void rsx_draw_engine_flush(void);

/* GPU -> guest memory. The 2D engine's reads and the SPUs see guest memory,
 * which never holds what the GPU drew. Write the newest colour target or the
 * depth buffer whose rows cover (local, offset) back into it as the RSX would
 * have stored it -- A8R8G8B8 as bytes A,R,G,B, Z24S8 as the big-endian
 * D << 8 | S word -- `pitch` bytes per row (0: the target's own width). Pending
 * work is submitted and waited for first. A target already written back since
 * it was last drawn is skipped. Returns 1 when guest memory was written.
 * inFAMOUS 2 NV3089-copies its normal and depth buffers to main memory every
 * frame for the SPUs' deferred lighting; without this the SPUs lit nothing. */
int rsx_draw_engine_write_back(int local, u32 offset, u32 pitch);
/* Same, knowing where the blit lands (-1/0 when unknown): a blit into a buffer
 * the GPU light pass replaces needs no write-back (PS3RECOMP_ENG_GPU_LIGHT). */
int rsx_draw_engine_write_back_to(int local, u32 offset, u32 pitch, int dst_local, u32 dst_offset);
/* After rsx_draw_engine_write_back_to: 1 when that blit's destination is one of
 * the GPU light pass's input buffers that nothing reads any more (both GPU
 * passes on), so the guest copy can be skipped (PS3RECOMP_ENG_GPU_LIGHT_SKIPCOPY). */
int rsx_draw_engine_gpu_light_dst_dead(int dst_local, u32 dst_offset, u32 pitch);
/* PS3RECOMP_ENG_GPU_LIGHT: hand the engine the light job's frame block (its
 * 0x100 bytes, big-endian: matrix at +0x10, scale at +0x98) and light records
 * (0x30 bytes each), as the SPU job would read them this frame. Any thread. */
void rsx_draw_engine_gpu_light_data(const void* frame_be, const void* lights_be, u32 count);
/* PS3RECOMP_ENG_GPU_SSAO=<local offset>: hand the engine the SSAO shader
 * constants for this frame (an opaque block for the backend's ssao_pass, at
 * most 512 bytes) and the full/half sizes. Needs PS3RECOMP_ENG_GPU_LIGHT's
 * input copies. Any thread. */
void rsx_draw_engine_gpu_ssao_data(const void* consts, u32 bytes, u32 full_w, u32 full_h,
                                   u32 half_w, u32 half_h);

/* Present whatever the frame has recorded, for a host that drives the flip
 * itself rather than through the FIFO's 0xE944. */
void rsx_draw_engine_present(void);
/* A runner retiring a queued flip names the buffer explicitly. */
void rsx_draw_engine_present_buffer(u32 buffer_id);

/* --- test hooks ---------------------------------------------------------- */

/* Draws that ran the guest's OWN programs in the last presented frame. A draw
 * through the built-in pair is not one of them. */
u32  rsx_draw_engine_guest_draws(void);

/* The centre pixel of the surface the last flip presented, as A8R8G8B8 --
 * the convention the RSX clear colour arrives in, so a host can compare the
 * two. Zero when nothing has been presented or the surface is not an
 * 8-bit-per-channel one. */
u32  rsx_draw_engine_readback_center(void);

/* ---- pieces the engine exposes for its own tests ------------------------ */
/*
 * These are the neutral parts the unit tests in libs/video/tests exercise
 * without a GPU: the surface set's keying, the texture cache's revalidation
 * and eviction, the pipeline key's stability, and restart-cut expansion.
 */

/* Decode the NV4097 render state out of the register file. */
void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* out);

/* Fold the structural render state into a pipeline key. The alpha REFERENCE
 * and everything else that lives in a constant buffer are deliberately left
 * out, so a title changing constants per draw does not multiply pipelines --
 * rsx_live_draw.c's ld_hash_structural_render_state (3148-3187). */
u64  rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash);

/* How many indices the topology expansion produces for `source_refs`
 * references cut at `cuts`, and the indices themselves. `indices` must have
 * room for the count. Triangle strips alternate winding per segment; a quad
 * strip pairs its vertices from each segment's own start, and a polygon is a
 * fan around each segment's own first vertex. */
u32  rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                          const u32* cuts, u32 cut_count);
void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices);

/* Texture packs (PS3_MODS) for the PC menu: the packs folder (PS3_MODS_DIR,
 * or a "texpacks" folder found beside the executable or two folders up; ""
 * when none), and what is loaded (packs and replacement files in use, uploads
 * replaced so far, whether a dump runs). */
const char* rsx_texpack_dir(void);
void rsx_texpack_status(unsigned* packs, unsigned* files, unsigned* applied, int* dumping);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_DRAW_ENGINE_H */
