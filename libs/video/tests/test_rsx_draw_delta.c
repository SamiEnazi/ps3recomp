/*
 * ps3recomp - (rnd1) PS3RECOMP_ENG_DELTA: the delta draw engine must hand the
 * backend exactly what the whole way hands it.
 *
 * One deterministic stream of frames -- material changes, runs of draws that
 * change only their transform constants, the same mesh drawn again, a
 * fragment program patched in guest memory, vertex bytes rewritten, clears,
 * a target switch, depth bounds, the shader control and user clip registers,
 * identical re-writes -- through the engine against a backend that logs every
 * call that changes or reads GPU state (pure queries such as surface_view and
 * vbuf_prefetch are not logged: the delta path legitimately skips them).
 * (fastdraw) The binds and sets are not logged as calls: they only change the
 * state the backend holds, and each draw logs that whole state with its own
 * arguments -- PS3RECOMP_ENG_FASTDRAW leaves out the binds that would send
 * what the backend holds already, so a call log would differ while what every
 * draw draws with is the same. Creates, uploads, releases, snapshots, clears,
 * presents and readbacks are still logged as calls.
 *
 * Run it three times with the same arguments, under PS3RECOMP_ENG_DELTA=0,
 * =1, and =1 with PS3RECOMP_ENG_DELTA_VERIFY=1 (and PS3RECOMP_ENG_DELTA_STATS=1
 * for the counters): the three logs must be byte-identical, and the verify
 * run must print no [delta-verify] line. Usage: test_rsx_draw_delta <log>.
 */
#include "rsx_draw_engine.h"
#include "rsx_primitives.h"
#include "rsx_test_programs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ---- guest memory ------------------------------------------------------- */
#define GUEST_BYTES     (16u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;
volatile u32 g_gcm_xlate_gen = 1;

u32 cellGcmResolveLocated(int local, u32 offset) { return local ? GUEST_LOCAL_EA + offset : offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < GUEST_LOCAL_EA ? offset : 0; }

/* ---- the recording backend --------------------------------------------- */
static char* s_log;
static size_t s_len, s_cap;
static u32 s_next = 1;
static int s_bench;   /* DELTA_BENCH=<frames>: no log, that many frames (timing only) */

static void logf_(const char* fmt, ...)
{
    if (s_bench) return;
    if (s_cap - s_len < 512) {
        s_cap = s_cap ? s_cap * 2 : (1u << 20);
        s_log = (char*)realloc(s_log, s_cap);
        if (!s_log) { printf("[FAIL] out of memory\n"); exit(2); }
    }
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(s_log + s_len, s_cap - s_len, fmt, ap);
    va_end(ap);
    if (k > 0) s_len += (size_t)k;
}
static u64 h64(const void* p, size_t n)
{
    const u8* b = (const u8*)p;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; b && i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static u32 fbits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* the arguments (hashes) are only computed when logging */
#define LOGF(...) do { if (!s_bench) logf_(__VA_ARGS__); } while (0)

static int  r_init(void* u, u32 w, u32 h) { (void)u; LOGF("init %u %u\n", w, h); return 0; }
static void r_shutdown(void* u) { (void)u; }
static void r_submit(void* u, u32 reason) { (void)u; LOGF("submit %u\n", reason); }
static u32  r_tex_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips, u32 faces, u32 remap, u32 rf)
{ (void)u; const u32 hd = s_next++; LOGF("tex_create h%u fmt %d %ux%u mips %u faces %u remap %X rf %X\n", hd, (int)f, w, h, mips, faces, remap, rf); return hd; }
static void r_tex_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h, const void* src, u32 row, u32 rows)
{ (void)u; LOGF("tex_upload h%u %u/%u %ux%u %016llX\n", t, face, mip, w, h, (unsigned long long)h64(src, (size_t)row * rows)); }
static void r_release(void* u, u32 hd) { (void)u; LOGF("release h%u\n", hd); }
static u32  r_color_create(void* u, rsx_be_format f, u32 w, u32 h, const void* seed, u32 row)
{ (void)u; const u32 hd = s_next++; LOGF("color_create h%u fmt %d %ux%u seed %d\n", hd, (int)f, w, h, seed != NULL); (void)row; return hd; }
/* a pure query: a stable view per (surface, remap, format), not logged */
static struct { u32 s, r, f, v; } s_views[256];
static u32 r_surface_view(void* u, u32 s, u32 remap, u32 f)
{
    (void)u;
    for (u32 i = 0; i < 256; i++) if (s_views[i].v && s_views[i].s == s && s_views[i].r == remap && s_views[i].f == f) return s_views[i].v;
    for (u32 i = 0; i < 256; i++) if (!s_views[i].v) { s_views[i].s = s; s_views[i].r = remap; s_views[i].f = f; s_views[i].v = s_next++;
                                                        LOGF("view_create h%u of h%u\n", s_views[i].v, s); return s_views[i].v; }
    return 0;
}
static u32  r_depth_create(void* u, u32 w, u32 h) { (void)u; const u32 hd = s_next++; LOGF("depth_create h%u %ux%u\n", hd, w, h); return hd; }
static u32  r_depth_snapshot(void* u, u32 d, u32 w, u32 h) { (void)u; const u32 hd = s_next++; LOGF("depth_snapshot h%u of h%u %ux%u\n", hd, d, w, h); return hd; }
static u32  r_color_snapshot(void* u, u32 s) { (void)u; const u32 hd = s_next++; LOGF("color_snapshot h%u of h%u\n", hd, s); return hd; }
static u32  r_depth_rgba8(void* u, u32 d, u32 w, u32 h) { (void)u; const u32 hd = s_next++; LOGF("depth_rgba8 h%u of h%u %ux%u\n", hd, d, w, h); return hd; }
static u32  r_pipeline_create(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                              const rsx_vertex_layout_plan* layout, u32 stride, rsx_be_format f, u32 count)
{
    (void)u;
    const u32 hd = s_next++;
    LOGF("pipeline_create h%u vs %016llX ps %016llX rs %016llX mask %X stride %u fmt %d targets %u\n", hd,
          (unsigned long long)h64(vs, strlen(vs)), (unsigned long long)h64(ps, strlen(ps)),
          (unsigned long long)h64(rs, sizeof *rs), layout->mask, stride, (int)f, count);
    return hd;
}
/* (fastdraw) what the backend holds, as its binds and sets left it (the
 * Vulkan backend's rules: a unit not in the mask holds no texture; a sampler
 * is kept only for a unit in the mask), logged whole with each draw */
static struct {
    u32 tgt[RSX_BE_MAX_COLOR_TARGETS], ntgt, depth, pipe, gen;
    int ranged; u32 vs_first, vs_count, vs_total, vs_n; u64 vs_h, vs_tail;
    u32 ps_n; u64 ps_h;
    u32 tmask, tex[RSX_BE_MAX_TEXTURES]; u64 samp[RSX_BE_MAX_TEXTURES];
    u32 vmask, vtex[RSX_BE_MAX_VERTEX_TEXTURES]; u64 vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
    u32 vp[4], sc[4], sref, bias[2]; int clamp, hud; u32 hud_w;
} s_st;
static void r_bind_targets(void* u, const u32* s, u32 n, u32 d)
{
    (void)u;
    if (n > RSX_BE_MAX_COLOR_TARGETS) n = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) s_st.tgt[i] = (i < n && s) ? s[i] : 0u;
    s_st.ntgt = n; s_st.depth = d;
}
static void r_bind_pipeline(void* u, u32 p) { (void)u; s_st.pipe = p; }
static void r_bind_vs(void* u, const void* d, u32 n) { (void)u; s_st.ranged = 0; s_st.vs_n = n; s_st.vs_h = h64(d, n); }
static void r_bind_ps(void* u, const void* d, u32 n) { (void)u; s_st.ps_n = n; s_st.ps_h = h64(d, n); }
static void r_bind_vs_range(void* u, const void* rows, u32 first, u32 count, const void* tail, u32 tb, u32 total)
{
    (void)u;
    s_st.ranged = 1; s_st.vs_first = first; s_st.vs_count = count; s_st.vs_total = total;
    s_st.vs_h = h64((const u8*)rows + (size_t)first * 16u, (size_t)count * 16u); s_st.vs_tail = h64(tail, tb);
}
static void r_vs_gen(void* u, u32 gen) { (void)u; s_st.gen = gen; }
static void r_bind_textures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    (void)u;
    for (u32 i = 0; i < RSX_BE_MAX_TEXTURES; i++) {
        const int on = (mask >> i) & 1u;
        s_st.tex[i] = on ? t[i] : 0u;
        if (on && s) s_st.samp[i] = h64(&s[i], sizeof s[i]);
    }
    s_st.tmask = mask;
}
static void r_bind_vtextures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    (void)u;
    for (u32 i = 0; i < RSX_BE_MAX_VERTEX_TEXTURES; i++) {
        const int on = (mask >> i) & 1u;
        s_st.vtex[i] = (on && t) ? t[i] : 0u;
        if (on && s) s_st.vsamp[i] = h64(&s[i], sizeof s[i]);
    }
    s_st.vmask = mask;
}
static void r_viewport(void* u, float x, float y, float w, float h)
{ (void)u; s_st.vp[0] = fbits(x); s_st.vp[1] = fbits(y); s_st.vp[2] = fbits(w); s_st.vp[3] = fbits(h); }
static void r_scissor(void* u, u32 x, u32 y, u32 w, u32 h) { (void)u; s_st.sc[0] = x; s_st.sc[1] = y; s_st.sc[2] = w; s_st.sc[3] = h; }
static void r_stencil(void* u, u32 r) { (void)u; s_st.sref = r; }
static void r_bias(void* u, float c, float s) { (void)u; s_st.bias[0] = fbits(c); s_st.bias[1] = fbits(s); }
static void r_clamp(void* u, int c) { (void)u; s_st.clamp = c; }
static void r_hud(void* u, int hud, u32 w) { (void)u; if (hud < 3) { s_st.hud = hud; s_st.hud_w = w; } else LOGF("hud %d %u\n", hud, w); }
/* the state a draw draws with */
static void log_state(void)
{
    if (s_bench) return;
    LOGF("  with targets %u:", s_st.ntgt);
    for (u32 i = 0; i < s_st.ntgt; i++) LOGF(" h%u", s_st.tgt[i]);
    LOGF(" z h%u pipeline h%u", s_st.depth, s_st.pipe);
    if (s_st.ranged) LOGF(" vs_range gen %u %u+%u %016llX tail %016llX total %u", s_st.gen, s_st.vs_first, s_st.vs_count,
                          (unsigned long long)s_st.vs_h, (unsigned long long)s_st.vs_tail, s_st.vs_total);
    else LOGF(" vs_consts %u %016llX", s_st.vs_n, (unsigned long long)s_st.vs_h);
    LOGF(" ps %u %016llX textures %X:", s_st.ps_n, (unsigned long long)s_st.ps_h, s_st.tmask);
    for (u32 i = 0; i < RSX_BE_MAX_TEXTURES; i++) if (s_st.tmask >> i & 1u) LOGF(" %u=h%u/%016llX", i, s_st.tex[i], (unsigned long long)s_st.samp[i]);
    LOGF(" vtextures %X:", s_st.vmask);
    for (u32 i = 0; i < RSX_BE_MAX_VERTEX_TEXTURES; i++) if (s_st.vmask >> i & 1u) LOGF(" %u=h%u/%016llX", i, s_st.vtex[i], (unsigned long long)s_st.vsamp[i]);
    LOGF(" viewport %08X %08X %08X %08X scissor %u %u %u %u stencil %u bias %08X %08X clamp %d hud %d %u\n",
         s_st.vp[0], s_st.vp[1], s_st.vp[2], s_st.vp[3], s_st.sc[0], s_st.sc[1], s_st.sc[2], s_st.sc[3], s_st.sref,
         s_st.bias[0], s_st.bias[1], s_st.clamp, s_st.hud, s_st.hud_w);
}
static void r_draw(void* u, rsx_topology t, const void* v, u32 nv, u32 stride, const u32* idx, u32 ni)
{ (void)u; LOGF("draw %d %u x %u %016llX idx %u %016llX\n", (int)t, nv, stride, (unsigned long long)h64(v, (size_t)nv * stride),
                 ni, (unsigned long long)h64(idx, (size_t)ni * 4u)); log_state(); }
static u32  r_vbuf_create(void* u, const void* d, u32 n) { (void)u; const u32 hd = s_next++; LOGF("vbuf_create h%u %u %016llX\n", hd, n, (unsigned long long)h64(d, n)); return hd; }
static void r_draw_vbuf(void* u, rsx_topology t, u32 vb, u32 nv, u32 stride, const u32* idx, u32 ni)
{ (void)u; LOGF("draw_vbuf %d h%u %u x %u idx %u %016llX\n", (int)t, vb, nv, stride, ni, (unsigned long long)h64(idx, (size_t)ni * 4u)); log_state(); }
static void r_draw_vbuf_ib(void* u, rsx_topology t, u32 vb, u32 nv, u32 stride, u32 ib, u32 ni)
{ (void)u; LOGF("draw_vbuf_ib %d h%u %u x %u ib h%u %u\n", (int)t, vb, nv, stride, ib, ni); log_state(); }
static void r_prefetch(void* u, u32 vb) { (void)u; (void)vb; }
static void r_clear_color(void* u, u32 s, const float c[4]) { (void)u; LOGF("clear_color h%u %08X %08X %08X %08X\n", s, fbits(c[0]), fbits(c[1]), fbits(c[2]), fbits(c[3])); }
static void r_clear_ds(void* u, u32 d, u32 f, float z, u8 s) { (void)u; LOGF("clear_ds h%u %u %08X %u\n", d, f, fbits(z), s); }
static void r_present(void* u, u32 s) { (void)u; LOGF("present h%u\n", s); }
static void r_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{ (void)u; LOGF("readback h%u %u %u %u %u\n", s, x, y, w, h); if (out) for (u32 r = 0; r < h; r++) memset((u8*)out + (size_t)r * pitch, 0x40, (size_t)w * 4u); }

static const rsx_draw_backend g_rec = {
    .init = r_init, .shutdown = r_shutdown, .submit_and_wait = r_submit,
    .texture_create = r_tex_create, .texture_upload = r_tex_upload, .texture_release = r_release,
    .color_target_create = r_color_create, .color_target_release = r_release, .surface_view = r_surface_view,
    .depth_target_create = r_depth_create, .depth_target_release = r_release, .depth_snapshot = r_depth_snapshot,
    .color_snapshot = r_color_snapshot, .depth_snapshot_rgba8 = r_depth_rgba8,
    .pipeline_create = r_pipeline_create, .pipeline_release = r_release,
    .bind_targets = r_bind_targets, .bind_pipeline = r_bind_pipeline, .bind_vs_constants = r_bind_vs,
    .bind_ps_constants = r_bind_ps, .bind_textures = r_bind_textures, .bind_vertex_textures = r_bind_vtextures,
    .set_viewport = r_viewport, .set_scissor = r_scissor, .set_stencil_ref = r_stencil, .draw = r_draw,
    .clear_color = r_clear_color, .clear_depth_stencil = r_clear_ds, .present = r_present, .readback = r_readback,
    .bind_vs_constants_range = r_bind_vs_range,
    .vbuf_create = r_vbuf_create, .vbuf_release = r_release, .draw_vbuf = r_draw_vbuf,
    .set_hud = r_hud, .set_depth_bias = r_bias, .set_depth_clamp = r_clamp,
    .draw_vbuf_ib = r_draw_vbuf_ib, .vbuf_prefetch = r_prefetch, .vs_constants_gen = r_vs_gen,
    .draw_vbuf_ib_again = r_draw_vbuf_ib,   /* (fastdraw) the same draw: logged the same */
};

/* ---- the stream ----------------------------------------------------------- */
static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }
static u32 s_rng = 0x12345678u;
static u32 rnd(u32 n) { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) % n; }

#define VTX_MAIN   0x00010000u     /* float4 position x 96, then float4 colour x 96 */
#define IDX_MAIN   0x00030000u     /* u16 indices */
#define TEX0_MAIN  0x00040000u
#define TEX1_MAIN  0x00041000u
#define FP_A_LOCAL 0x00001000u
#define FP_B_LOCAL 0x00001100u
#define FP_C_LOCAL 0x00001200u
#define RT_A_LOCAL 0x00040000u
#define RT_B_LOCAL 0x000C0000u
#define ZETA_LOCAL 0x00100000u
#define NVERT 96u

static void put_be32(u8* p, u32 w) { p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w; }
static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void upload_vp(u32 slot, const u8* words, u32 instrs)
{
    m(0x1E9C, slot);                                   /* TRANSFORM_PROGRAM_LOAD */
    for (u32 i = 0; i < instrs * 4u; i++) { u32 w; memcpy(&w, words + i * 4u, 4); m(0x0B80 + (i & 3u) * 4u, w); }
}
static void bind_tex(u32 unit, u32 offset, u32 wh)
{
    const u32 b = 0x1A00 + unit * 0x20;
    m(b + 0x00, offset);
    m(b + 0x04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));   /* main, 2D, A8R8G8B8 linear, 1 level */
    m(b + 0x08, 0x00030303u);
    m(b + 0x0C, 0x80000000u);
    m(b + 0x10, 0xAAE4u);
    m(b + 0x14, (2u << 24) | (2u << 16));              /* linear filters */
    m(b + 0x18, (wh << 16) | wh);
    m(0x1840 + unit * 4u, wh * 4u);                    /* pitch */
}
static void unbind_tex(u32 unit) { m(0x1A0C + unit * 0x20, 0); }
static void constants(u32 base, u32 rows)
{
    m(0x1EFC, base);
    for (u32 r = 0; r < rows; r++)
        for (u32 c = 0; c < 4; c++) m(0x1F00 + (r * 4u + c) * 4u, f2u((float)(rnd(1000)) / 500.0f - 1.0f));
}
static void draw_mesh(u32 prim, u32 first, u32 count)
{
    m(0x1808, prim);
    m(0x1824, first | ((count - 1u) << 24));
    m(0x1808, 0);
}
static void draw_arrays(u32 prim, u32 first, u32 count)
{
    m(0x1808, prim);
    m(0x1814, first | ((count - 1u) << 24));
    m(0x1808, 0);
}
static void surface(u32 color_offset)
{
    m(0x0194, 0xFEED0000u); m(0x0198, 0xFEED0000u);   /* colour A, zeta: local */
    m(0x0200, 256u << 16); m(0x0204, 256u << 16);
    m(0x0208, 5u | (2u << 5) | (1u << 8));             /* A8R8G8B8, Z24S8, linear */
    m(0x020C, 1024u); m(0x022C, 1024u);
    m(0x0210, color_offset); m(0x0214, ZETA_LOCAL);
    m(0x0220, 1u);
    m(0x0A00, 256u << 16); m(0x0A04, 256u << 16);
    m(0x0A20, f2u(128.0f)); m(0x0A24, f2u(128.0f)); m(0x0A28, f2u(0.5f)); m(0x0A2C, 0);
    m(0x0A30, f2u(128.0f)); m(0x0A34, f2u(-128.0f)); m(0x0A38, f2u(0.5f)); m(0x0A3C, 0);
}
static void material(u32 k)
{
    static const u32 fps[5] = { FP_A_LOCAL | 1u, FP_B_LOCAL | 1u, FP_B_LOCAL | 1u, FP_C_LOCAL | 1u, FP_A_LOCAL | 1u };
    m(0x08E4, fps[k % 5u]);
    if (k % 5u == 1u) bind_tex(0, TEX0_MAIN, 4);
    else if (k % 5u == 2u) bind_tex(0, TEX1_MAIN, 8);
    else if (k % 5u == 3u) { bind_tex(1, TEX0_MAIN, 4); bind_tex(0, TEX1_MAIN, 8); }
    else unbind_tex(0);
    m(0x0310, k & 1u);                                 /* blend enable */
    m(0x0314, 0x03020302u); m(0x0318, 0x03030303u);    /* src alpha / one minus src alpha */
    m(0x0A6C, 0x0203u);                                /* depth func LEQUAL */
    m(0x0A74, 1u); m(0x0A70, (k & 2u) ? 0u : 1u);      /* depth test, write */
}

static void run(void)
{
    rsx_draw_engine_set_backend(&g_rec);
    if (rsx_draw_engine_init(1280, 720) != 0) { printf("[FAIL] engine init\n"); exit(2); }

    /* programs: VP_A at slot 0 reads v0 and v3; VP_B at slot 16 reads v0 */
    u8 vpa[32], vpb[16];
    rsx_test_vp_mov_out(vpa + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vpa + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_vp_mov_out(vpb, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    upload_vp(0, vpa, 2);
    upload_vp(16, vpb, 1);
    m(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + GUEST_LOCAL_EA + FP_A_LOCAL, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(vm_base + GUEST_LOCAL_EA + FP_B_LOCAL, 0);
    rsx_test_fp_tex_r0_tc0(vm_base + GUEST_LOCAL_EA + FP_C_LOCAL, 1);

    /* vertex arrays: attr 0 position, attr 3 colour (float4) */
    for (u32 i = 0; i < NVERT; i++)
        for (u32 c = 0; c < 4; c++) {
            put_be32(vm_base + VTX_MAIN + i * 16u + c * 4u, f2u((float)((i * 7u + c * 3u) % 17u) / 8.0f - 1.0f));
            put_be32(vm_base + VTX_MAIN + NVERT * 16u + i * 16u + c * 4u, f2u((float)((i + c) % 5u) / 4.0f));
        }
    /* six meshes of four triangles, each over its own run of eight vertices
     * (a dense range: what the index cache and the vertex cache keep) */
    static const u8 quad2[12] = { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7 };
    for (u32 i = 0; i < 72u; i++) { const u32 v = (i / 12u) * 8u + quad2[i % 12u]; vm_base[IDX_MAIN + i * 2u] = (u8)(v >> 8); vm_base[IDX_MAIN + i * 2u + 1u] = (u8)v; }
    for (u32 i = 0; i < 4096u; i++) { vm_base[TEX0_MAIN + i] = (u8)(i * 13u); vm_base[TEX1_MAIN + i] = (u8)(i * 7u + 1u); }
    m(0x1740 + 0 * 4u, 2u | (4u << 4) | (16u << 8));
    m(0x1740 + 3 * 4u, 2u | (4u << 4) | (16u << 8));
    m(0x1680 + 0 * 4u, 0x80000000u | VTX_MAIN);
    m(0x1680 + 3 * 4u, 0x80000000u | (VTX_MAIN + NVERT * 16u));
    m(0x181C, IDX_MAIN);
    m(0x1820, 1u | (1u << 4));                          /* main, u16 */
    surface(RT_A_LOCAL);

    const u32 frames = s_bench ? (u32)s_bench : 24u;
    for (u32 frame = 0; frame < frames; frame++) {
        if (frame % 4u == 0u) m(0x1D94, 0xF3u);         /* clear colour + depth + stencil */
        for (u32 d = 0; d < 48u; d++) {
            if (d % 8u == 0u) material(d / 8u + (frame & 1u));
            /* events */
            const u32 e = rnd(100);
            if (e < 6) material(d / 8u + (frame & 1u));                 /* the same values again */
            else if (e < 9) m(0x0334, rnd(4));                          /* stencil ref */
            else if (e < 11) { m(0x08C0, (rnd(128) << 16) | rnd(64)); m(0x08C4, (rnd(128) << 16) | rnd(64)); }
            else if (e < 12) {                                          /* patch FP_A in memory */
                rsx_test_fp_mov_r0_col0(vm_base + GUEST_LOCAL_EA + FP_A_LOCAL,
                                        rnd(2) ? RSX_TEST_FP_SWZ_IDENT : RSX_TEST_FP_SWZ(2, 1, 0, 3));
            } else if (e < 13) {                                        /* rewrite some vertex bytes */
                const u32 v = rnd(NVERT);
                put_be32(vm_base + VTX_MAIN + v * 16u, f2u((float)rnd(100) / 50.0f - 1.0f));
            } else if (e < 14) m(0x1D94, 0x01u);                        /* clear depth */
            else if (e < 15) m(0x1EA0, rnd(2) ? 16u : 0u);              /* the other transform program */
            else if (e < 16) m(0x1478, rnd(2) ? 0x2u : 0u);             /* user clip plane 0 */
            else if (e < 17) m(0x1D60, rnd(2) ? 0x40u : 0u);            /* shader control */
            else if (e < 18) { m(0x0380, 1u); m(0x0384, f2u(0.1f)); m(0x0388, f2u(0.9f)); }   /* depth bounds */
            else if (e < 20) m(0x0380, 0u);
            else if (e < 21) surface(rnd(2) ? RT_B_LOCAL : RT_A_LOCAL); /* another target */
            else if (e < 22) m(0x0A6C, rnd(2) ? 0x0201u : 0x0203u);     /* depth func */
            else if (e < 23) m(0x1680, 0x80000000u | (VTX_MAIN + rnd(2) * 64u));   /* vertex array moved */
            else if (e < 24) { m(0x1808, RSX_PRIMITIVE_TRIANGLES); m(0x1808, 0); } /* an empty pair */
            else if (e < 25) {                                          /* rewrite index bytes (same or other) */
                const u32 i = rnd(72);
                const u32 v = rnd(2) ? (i / 12u) * 8u + quad2[i % 12u] : (i / 12u) * 8u + rnd(8);
                vm_base[IDX_MAIN + i * 2u] = (u8)(v >> 8); vm_base[IDX_MAIN + i * 2u + 1u] = (u8)v;
            } else if (e < 26) vm_base[TEX0_MAIN + rnd(64)] ^= 0x5A;    /* texture bytes */
            else if (e < 27) {                                          /* unit 2 on the zeta (colour read of depth) */
                bind_tex(2, ZETA_LOCAL, 256); m(0x1A04 + 2 * 0x20, 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
            } else if (e < 28) {                                        /* unit 0 on target B (a render-to-texture read) */
                bind_tex(0, RT_B_LOCAL, 256); m(0x1A04, 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
            } else if (e < 29) {                                        /* unit 0 on the target being drawn */
                bind_tex(0, RT_A_LOCAL, 256); m(0x1A04, 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
            } else if (e < 30) upload_vp(16, rnd(2) ? vpb : vpa, 1);    /* re-upload (same or other words) */
            else if (e < 31) m(0x08E4, rnd(2) ? 0u : (FP_C_LOCAL | 1u));   /* no program / another */
            else if (e < 32) {                                          /* MRT A+B, or A alone */
                m(0x018C, 0xFEED0000u); m(0x0218, RT_B_LOCAL); m(0x021C, 1024u); m(0x0220, rnd(2) ? 0x13u : 1u);
            } else if (e < 33) unbind_tex(2);
            else if (e < 34) m(0x1A00 + 0x20 * 5u, rnd(1024));          /* an unread unit's register */
            else if (e < 35) m(0x1FEC, rnd(2));                         /* shader packer */
            /* the draw: constants every draw, a mesh per pair of draws (repeats) */
            constants(rnd(4) * 4u, 4);
            const u32 mesh = (d / 2u) % 6u;
            if (d % 11u == 5u) draw_arrays(RSX_PRIMITIVE_TRIANGLE_STRIP, (mesh * 7u) % 60u, 8);
            else draw_mesh(RSX_PRIMITIVE_TRIANGLES, mesh * 12u, 12);
        }
        rsx_draw_engine_present();
    }
    rsx_draw_engine_shutdown();
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: test_rsx_draw_delta <log file> [seed]\n"); return 2; }
    if (argc > 2) s_rng = (u32)strtoul(argv[2], NULL, 0);
    { const char* b = getenv("DELTA_BENCH"); s_bench = b ? atoi(b) : 0; }
#ifdef _WIN32
    vm_base = (u8*)VirtualAlloc(NULL, GUEST_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    vm_base = (u8*)calloc(1, GUEST_BYTES);
#endif
    if (!vm_base) { printf("[FAIL] guest memory\n"); return 2; }
    ppu_vm_size = GUEST_BYTES;
    run();
    FILE* f = fopen(argv[1], "wb");
    if (!f) { printf("[FAIL] cannot write %s\n", argv[1]); return 2; }
    fwrite(s_log, 1, s_len, f);
    fclose(f);
    u32 lines = 0; for (size_t i = 0; i < s_len; i++) lines += s_log[i] == '\n';
    printf("%u backend calls, log %016llX -> %s\n", lines, (unsigned long long)h64(s_log, s_len), argv[1]);
    return 0;
}
