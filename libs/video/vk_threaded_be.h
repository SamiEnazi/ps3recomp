/*
 * ps3recomp - (rsx-par) the Vulkan backend's draw work on the recording worker
 * (PS3RECOMP_VK_BT, default 1; 0 = the backend runs on the engine's thread and
 * only the vkCmd* calls go to the worker, as before).
 *
 * Included once by rsx_vulkan_backend.c, after s_vk_engine_backend and
 * vk_threaded_rec.h's ring. Needs threaded recording (PS3RECOMP_VK_THREADED).
 *
 * vk_threaded_rec.h moved the driver's recording cost to a worker; the
 * backend's own per-draw work (vk_eng_draw_ex_body: arenas, the descriptor set,
 * the framebuffer, the pass logic, the state filter -- ~21% of the RSX thread
 * at the city dock, rsx5 profile b3) stayed on the engine's thread. Now the
 * engine calls the backend through the thunks below:
 *
 *  - The per-draw calls (binds, sets, clears, the three draws) and the
 *    releases are records in the same ring: their arguments are copied (a
 *    pointer argument's bytes too), and the worker runs the backend's own
 *    function in ring order. bind_vs_constants_range's rows are copied once
 *    more on the worker, into a buffer that lives until the next bind, because
 *    the backend keeps that pointer until the draw.
 *  - Every call that returns something or hands the backend a pointer it
 *    reads in place (creates, uploads, snapshots, the light/SSAO passes,
 *    pipelines, readbacks, present, submit_and_wait, image load/save) drains
 *    the ring first and then runs on the calling thread, as before -- so the
 *    backend's state is only ever touched by one thread at a time, in program
 *    order.
 *  - Two calls the engine makes too often for a drain run on the engine's
 *    thread without one: vbuf_create (its handle and block lists take a lock,
 *    rsx_vulkan_backend.c) and surface_view, answered from a memo of the views
 *    it returned before, checked against the release counter of the surface
 *    and of the view (s_eobj_sg) and dropped on every release the engine
 *    makes; a miss drains and asks the backend.
 *  - vbuf_prefetch is a no-op (the worker reads the entry, not this thread).
 *  - A bind or set whose arguments equal the last ones sent is not sent again
 *    (sink_end makes all of them every draw; consecutive draws mostly repeat
 *    targets, pipeline, viewport, scissor, stencil, bias, textures and the
 *    fragment constants). Those entry points only store what they are given,
 *    and nothing else writes that state but pipeline_release and the draw's
 *    own save/restore; so every other call but the releases, and the end of
 *    every drain (the frame loop may touch the backend), forgets what was sent
 *    (PS3RECOMP_VK_BT_DEDUP=0: send everything).
 *  - At the end of every FIFO drain the ring is drained (g_rsx_drain_end_hook),
 *    so the present path, the window pump and the overlay -- which touch the
 *    backend directly between drains -- find the worker idle, as before; and
 *    rsx_vulkan_backend_present drains again after the engine's present.
 *
 * A record that cannot be queued (threaded recording off, the producer
 * recording directly, a call on the worker itself, a record too large for the
 * ring -- tq_alloc drained first in that case) runs directly: in order either
 * way.
 */
#ifndef PS3RECOMP_VK_THREADED_BE_H
#define PS3RECOMP_VK_THREADED_BE_H

#ifdef _WIN32

static rsx_draw_backend s_bt_real;   /* the backend's own entry points */
static rsx_draw_backend s_bt_be;     /* the thunks the engine calls */
static int s_bt_on;
static u64 s_bt_sv_hit, s_bt_sv_miss;

static void* bt_rec(void (*run)(const u8*), u32 arg_bytes)
{
    tq_bcall* r = (tq_bcall*)(void*)tq_alloc(TQ_BCALL, (u32)sizeof(tq_bcall) + arg_bytes, VK_NULL_HANDLE);
    if (!r) return NULL;
    r->run = run;
    return r + 1;
}
static void bt_commit(void* args) { tq_commit((const u8*)args - sizeof(tq_bcall)); }
#define BT_A(T, p) const T* a = (const T*)(const void*)(p)

/* What the worker's backend state holds, as last sent (valid flags 0 = unknown). */
#define BT_PS_CACHE 1024u
static struct {
    int tgt_ok, pipe_ok, vp_ok, sc_ok, sref_ok, bias_ok, clamp_ok, ps_ok, tex_ok, vtex_ok;
    u32 tgt[RSX_BE_MAX_COLOR_TARGETS], ntgt, tdepth, thas;
    u32 pipe, sref; int clamp;
    float vp[4], bias[2];
    u32 sc[4];
    u32 ps_bytes;
    u8 ps[BT_PS_CACHE];
    u32 tex[RSX_BE_MAX_TEXTURES]; rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES]; u32 tmask, thas_t, thas_s;
    u32 vtex[RSX_BE_MAX_VERTEX_TEXTURES]; rsx_be_sampler_desc vsamp[RSX_BE_MAX_VERTEX_TEXTURES]; u32 vmask, vhas_t, vhas_s;
} s_bt_last;
static u64 s_bt_dedup_n;
static int bt_dedup_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3RECOMP_VK_BT_DEDUP"); v = !(e && e[0] == '0'); }
    return v;
}
static void bt_forget(void)
{
    s_bt_last.tgt_ok = s_bt_last.pipe_ok = s_bt_last.vp_ok = s_bt_last.sc_ok = s_bt_last.sref_ok = 0;
    s_bt_last.bias_ok = s_bt_last.clamp_ok = s_bt_last.ps_ok = s_bt_last.tex_ok = s_bt_last.vtex_ok = 0;
}

/* ---- surface_view memo ---------------------------------------------------- */
typedef struct { u32 surface, remap, fmt, view, sg_s, sg_v; } bt_sv_ent;
static bt_sv_ent s_bt_sv[256];
static void bt_sv_forget(u32 h)          /* a release the engine makes */
{
    if (!h) return;
    for (u32 i = 0; i < 256u; i++)
        if (s_bt_sv[i].view && (s_bt_sv[i].surface == h || s_bt_sv[i].view == h)) s_bt_sv[i].view = 0;
}
static inline u32 bt_sg(u32 h) { return h < VK_ENG_MAX_OBJ ? __atomic_load_n(&s_eobj_sg[h], __ATOMIC_ACQUIRE) : 0u; }
static u32 bt_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    bt_sv_ent* e = &s_bt_sv[(surface * 0x9E3779B1u ^ remap * 0x85EBCA77u ^ rsx_format * 0xC2B2AE35u) >> 24];
    if (e->view && e->surface == surface && e->remap == remap && e->fmt == rsx_format &&
        bt_sg(surface) == e->sg_s && bt_sg(e->view) == e->sg_v) {
        s_bt_sv_hit++;
        return e->view;
    }
    s_bt_sv_miss++;
    bt_forget();
    vk_tq_sync();
    const u32 v = s_bt_real.surface_view(s_bt_real.user, surface, remap, rsx_format);
    e->surface = surface; e->remap = remap; e->fmt = rsx_format;
    e->sg_s = bt_sg(surface); e->sg_v = bt_sg(v);
    e->view = v;
    return v;
}

/* ---- calls that drain, then run here ------------------------------------- */
static void bt_submit_and_wait(void* u, u32 reason) { (void)u; bt_forget(); vk_tq_sync(); s_bt_real.submit_and_wait(s_bt_real.user, reason); }
static u32 bt_texture_create(void* u, rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.texture_create(s_bt_real.user, fmt, w, h, mips, faces, remap, rsx_fmt); }
static void bt_texture_upload(void* u, u32 texture, u32 face, u32 mip, u32 w, u32 h, const void* src, u32 row_bytes, u32 rows)
{ (void)u; bt_forget(); vk_tq_sync(); s_bt_real.texture_upload(s_bt_real.user, texture, face, mip, w, h, src, row_bytes, rows); }
static u32 bt_color_target_create(void* u, rsx_be_format fmt, u32 w, u32 h, const void* seed, u32 seed_row_bytes)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.color_target_create(s_bt_real.user, fmt, w, h, seed, seed_row_bytes); }
static u32 bt_depth_target_create(void* u, u32 w, u32 h) { (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.depth_target_create(s_bt_real.user, w, h); }
static u32 bt_depth_snapshot(void* u, u32 depth, u32 w, u32 h) { (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.depth_snapshot(s_bt_real.user, depth, w, h); }
static u32 bt_color_snapshot(void* u, u32 surface) { (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.color_snapshot(s_bt_real.user, surface); }
static u32 bt_depth_snapshot_rgba8(void* u, u32 depth, u32 w, u32 h) { (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.depth_snapshot_rgba8(s_bt_real.user, depth, w, h); }
static u32 bt_pipeline_create(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                              const rsx_vertex_layout_plan* layout, u32 stride, rsx_be_format rt_fmt, u32 rt_count)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.pipeline_create(s_bt_real.user, vs, ps, rs, layout, stride, rt_fmt, rt_count); }
static void bt_present(void* u, u32 surface) { (void)u; bt_forget(); vk_tq_sync(); s_bt_real.present(s_bt_real.user, surface); }
static void bt_readback(void* u, u32 surface, u32 x, u32 y, u32 w, u32 h, void* out, u32 out_pitch)
{ (void)u; bt_forget(); vk_tq_sync(); s_bt_real.readback(s_bt_real.user, surface, x, y, w, h, out, out_pitch); }
static u32 bt_texture_create_3d(void* u, rsx_be_format fmt, u32 w, u32 h, u32 depth, u32 mips, u32 remap, u32 rsx_fmt)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.texture_create_3d(s_bt_real.user, fmt, w, h, depth, mips, remap, rsx_fmt); }
static void bt_texture_upload_3d(void* u, u32 texture, u32 z, u32 mip, u32 w, u32 h, const void* src, u32 row_bytes)
{ (void)u; bt_forget(); vk_tq_sync(); s_bt_real.texture_upload_3d(s_bt_real.user, texture, z, mip, w, h, src, row_bytes); }
static u32 bt_light_pass(void* u, u32 normals, u32 depth, u32 w, u32 h, const void* params, u32 param_bytes, u32* spec_out)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.light_pass(s_bt_real.user, normals, depth, w, h, params, param_bytes, spec_out); }
static u32 bt_ssao_pass(void* u, u32 depth, u32 normals, const void* consts, u32 const_bytes,
                        u32 full_w, u32 full_h, u32 half_w, u32 half_h)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.ssao_pass(s_bt_real.user, depth, normals, consts, const_bytes, full_w, full_h, half_w, half_h); }
static int bt_image_load(void* u, const char* path, u8** rgba, u32* w, u32* h)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.image_load(s_bt_real.user, path, rgba, w, h); }
static int bt_image_save(void* u, const char* path, const u8* rgba, u32 w, u32 h, u32 row_bytes)
{ (void)u; bt_forget(); vk_tq_sync(); return s_bt_real.image_save(s_bt_real.user, path, rgba, w, h, row_bytes); }

/* ---- the engine's thread, no drain ---------------------------------------- */
static u32 bt_vbuf_create(void* u, const void* data, u32 bytes) { (void)u; return s_bt_real.vbuf_create(s_bt_real.user, data, bytes); }
static void bt_vbuf_prefetch(void* u, u32 vbuf) { (void)u; (void)vbuf; }

/* ---- queued calls ----------------------------------------------------------- */
typedef struct { u32 h; } bt_a_u32;
/* A release changes no bound state (the next bind sets the same handles it
 * would have set), except pipeline_release, which unbinds a bound pipeline. */
#define BT_RELEASE(name, forget)                                                                     \
    static void bt_x_##name(const u8* p) { BT_A(bt_a_u32, p); s_bt_real.name(s_bt_real.user, a->h); } \
    static void bt_##name(void* u, u32 h)                                                    \
    {                                                                                        \
        (void)u;                                                                             \
        bt_sv_forget(h); if (forget) bt_forget();                                                   \
        bt_a_u32* a = (bt_a_u32*)bt_rec(bt_x_##name, sizeof *a);                             \
        if (!a) { s_bt_real.name(s_bt_real.user, h); return; }                               \
        a->h = h; bt_commit(a);                                                              \
    }
BT_RELEASE(texture_release, 0)
BT_RELEASE(color_target_release, 0)
BT_RELEASE(depth_target_release, 0)
BT_RELEASE(vbuf_release, 0)
BT_RELEASE(pipeline_release, 1)
#undef BT_RELEASE

/* (merge rsx-vtx + rsx-par) vs_constants_gen (PS3RECOMP_VK_CONST_REUSE) sets the
 * backend state that the queued bind_vs_constants_range reads (s_e2.vs_gen):
 * called straight from the engine thread it would race the worker and could
 * pair a draw with the NEXT draw's generation (stale constants). Ring order. */
static void bt_x_vs_constants_gen(const u8* p) { BT_A(bt_a_u32, p); s_bt_real.vs_constants_gen(s_bt_real.user, a->h); }
static void bt_vs_constants_gen(void* u, u32 gen)
{
    (void)u;
    bt_a_u32* a = (bt_a_u32*)bt_rec(bt_x_vs_constants_gen, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.vs_constants_gen(s_bt_real.user, gen); return; }
    a->h = gen; bt_commit(a);
}

static void bt_x_bind_pipeline(const u8* p) { BT_A(bt_a_u32, p); s_bt_real.bind_pipeline(s_bt_real.user, a->h); }
static void bt_bind_pipeline(void* u, u32 pipeline)
{
    (void)u;
    if (s_bt_last.pipe_ok && s_bt_last.pipe == pipeline) { s_bt_dedup_n++; return; }
    bt_a_u32* a = (bt_a_u32*)bt_rec(bt_x_bind_pipeline, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.bind_pipeline(s_bt_real.user, pipeline); return; }
    a->h = pipeline; bt_commit(a);
    s_bt_last.pipe = pipeline; s_bt_last.pipe_ok = bt_dedup_on();
}
static void bt_x_set_stencil_ref(const u8* p) { BT_A(bt_a_u32, p); s_bt_real.set_stencil_ref(s_bt_real.user, a->h); }
static void bt_set_stencil_ref(void* u, u32 ref)
{
    (void)u;
    if (s_bt_last.sref_ok && s_bt_last.sref == ref) { s_bt_dedup_n++; return; }
    bt_a_u32* a = (bt_a_u32*)bt_rec(bt_x_set_stencil_ref, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.set_stencil_ref(s_bt_real.user, ref); return; }
    a->h = ref; bt_commit(a);
    s_bt_last.sref = ref; s_bt_last.sref_ok = bt_dedup_on();
}
static void bt_x_set_depth_clamp(const u8* p) { BT_A(bt_a_u32, p); s_bt_real.set_depth_clamp(s_bt_real.user, (int)a->h); }
static void bt_set_depth_clamp(void* u, int clamp)
{
    (void)u;
    if (s_bt_last.clamp_ok && s_bt_last.clamp == clamp) { s_bt_dedup_n++; return; }
    bt_a_u32* a = (bt_a_u32*)bt_rec(bt_x_set_depth_clamp, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.set_depth_clamp(s_bt_real.user, clamp); return; }
    a->h = (u32)clamp; bt_commit(a);
    s_bt_last.clamp = clamp; s_bt_last.clamp_ok = bt_dedup_on();
}

typedef struct { u32 s[RSX_BE_MAX_COLOR_TARGETS]; u32 n, depth, has; } bt_a_targets;
static void bt_x_bind_targets(const u8* p)
{ BT_A(bt_a_targets, p); s_bt_real.bind_targets(s_bt_real.user, a->has ? a->s : NULL, a->n, a->depth); }
static void bt_bind_targets(void* u, const u32* surfaces, u32 count, u32 depth)
{
    (void)u;
    u32 t[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) t[i] = (surfaces && i < count) ? surfaces[i] : 0u;
    const u32 has = surfaces != NULL;
    if (s_bt_last.tgt_ok && s_bt_last.ntgt == count && s_bt_last.tdepth == depth && s_bt_last.thas == has &&
        !memcmp(s_bt_last.tgt, t, sizeof t)) { s_bt_dedup_n++; return; }
    bt_a_targets* a = (bt_a_targets*)bt_rec(bt_x_bind_targets, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.bind_targets(s_bt_real.user, surfaces, count, depth); return; }
    memcpy(a->s, t, sizeof t);
    a->n = count; a->depth = depth; a->has = has;
    bt_commit(a);
    memcpy(s_bt_last.tgt, t, sizeof t); s_bt_last.ntgt = count; s_bt_last.tdepth = depth; s_bt_last.thas = has;
    s_bt_last.tgt_ok = bt_dedup_on();
}

typedef struct { float v[4]; } bt_a_f4;
static void bt_x_set_viewport(const u8* p) { BT_A(bt_a_f4, p); s_bt_real.set_viewport(s_bt_real.user, a->v[0], a->v[1], a->v[2], a->v[3]); }
static void bt_set_viewport(void* u, float x, float y, float w, float h)
{
    (void)u;
    const float v[4] = { x, y, w, h };
    if (s_bt_last.vp_ok && !memcmp(s_bt_last.vp, v, sizeof v)) { s_bt_dedup_n++; return; }   /* bitwise: -0, NaN as given */
    bt_a_f4* a = (bt_a_f4*)bt_rec(bt_x_set_viewport, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.set_viewport(s_bt_real.user, x, y, w, h); return; }
    memcpy(a->v, v, sizeof v); bt_commit(a);
    memcpy(s_bt_last.vp, v, sizeof v); s_bt_last.vp_ok = bt_dedup_on();
}
static void bt_x_set_depth_bias(const u8* p) { BT_A(bt_a_f4, p); s_bt_real.set_depth_bias(s_bt_real.user, a->v[0], a->v[1]); }
static void bt_set_depth_bias(void* u, float constant, float slope)
{
    (void)u;
    const float v[2] = { constant, slope };
    if (s_bt_last.bias_ok && !memcmp(s_bt_last.bias, v, sizeof v)) { s_bt_dedup_n++; return; }
    bt_a_f4* a = (bt_a_f4*)bt_rec(bt_x_set_depth_bias, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.set_depth_bias(s_bt_real.user, constant, slope); return; }
    a->v[0] = constant; a->v[1] = slope; a->v[2] = a->v[3] = 0.0f; bt_commit(a);
    memcpy(s_bt_last.bias, v, sizeof v); s_bt_last.bias_ok = bt_dedup_on();
}
typedef struct { u32 v[4]; } bt_a_u4;
static void bt_x_set_scissor(const u8* p) { BT_A(bt_a_u4, p); s_bt_real.set_scissor(s_bt_real.user, a->v[0], a->v[1], a->v[2], a->v[3]); }
static void bt_set_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    const u32 v[4] = { x, y, w, h };
    if (s_bt_last.sc_ok && !memcmp(s_bt_last.sc, v, sizeof v)) { s_bt_dedup_n++; return; }
    bt_a_u4* a = (bt_a_u4*)bt_rec(bt_x_set_scissor, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.set_scissor(s_bt_real.user, x, y, w, h); return; }
    memcpy(a->v, v, sizeof v); bt_commit(a);
    memcpy(s_bt_last.sc, v, sizeof v); s_bt_last.sc_ok = bt_dedup_on();
}
typedef struct { int hud; u32 w; } bt_a_hud;
static void bt_x_set_hud(const u8* p) { BT_A(bt_a_hud, p); s_bt_real.set_hud(s_bt_real.user, a->hud, a->w); }
static void bt_set_hud(void* u, int hud, u32 target_w)
{
    (void)u;
    bt_a_hud* a = (bt_a_hud*)bt_rec(bt_x_set_hud, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.set_hud(s_bt_real.user, hud, target_w); return; }
    a->hud = hud; a->w = target_w; bt_commit(a);
}

typedef struct { u32 surface; float rgba[4]; } bt_a_clearc;
static void bt_x_clear_color(const u8* p) { BT_A(bt_a_clearc, p); s_bt_real.clear_color(s_bt_real.user, a->surface, a->rgba); }
static void bt_clear_color(void* u, u32 surface, const float rgba[4])
{
    (void)u;
    bt_forget();
    bt_a_clearc* a = (bt_a_clearc*)bt_rec(bt_x_clear_color, sizeof *a);
    if (!a) { s_bt_real.clear_color(s_bt_real.user, surface, rgba); return; }
    a->surface = surface; memcpy(a->rgba, rgba, sizeof a->rgba); bt_commit(a);
}
typedef struct { u32 depth, flags; float value; u32 stencil; } bt_a_clearz;
static void bt_x_clear_depth_stencil(const u8* p)
{ BT_A(bt_a_clearz, p); s_bt_real.clear_depth_stencil(s_bt_real.user, a->depth, a->flags, a->value, (u8)a->stencil); }
static void bt_clear_depth_stencil(void* u, u32 depth, u32 flags, float depth_value, u8 stencil)
{
    (void)u;
    bt_forget();
    bt_a_clearz* a = (bt_a_clearz*)bt_rec(bt_x_clear_depth_stencil, sizeof *a);
    if (!a) { s_bt_real.clear_depth_stencil(s_bt_real.user, depth, flags, depth_value, stencil); return; }
    a->depth = depth; a->flags = flags; a->value = depth_value; a->stencil = stencil; bt_commit(a);
}

/* constants: the bytes follow */
typedef struct { u32 bytes, has; } bt_a_bytes;
static void bt_x_bind_vs_constants(const u8* p)
{ BT_A(bt_a_bytes, p); s_bt_real.bind_vs_constants(s_bt_real.user, a->has ? (const void*)(a + 1) : NULL, a->bytes); }
static void bt_bind_vs_constants(void* u, const void* data, u32 bytes)
{
    (void)u;
    bt_a_bytes* a = (bt_a_bytes*)bt_rec(bt_x_bind_vs_constants, (u32)sizeof *a + (data ? bytes : 0u));
    if (!a) { bt_forget(); s_bt_real.bind_vs_constants(s_bt_real.user, data, bytes); return; }
    a->bytes = bytes; a->has = data != NULL;
    if (data && bytes) memcpy(a + 1, data, bytes);
    bt_commit(a);
}
static void bt_x_bind_ps_constants(const u8* p)
{ BT_A(bt_a_bytes, p); s_bt_real.bind_ps_constants(s_bt_real.user, a->has ? (const void*)(a + 1) : NULL, a->bytes); }
static void bt_bind_ps_constants(void* u, const void* data, u32 bytes)
{
    (void)u;
    if (s_bt_last.ps_ok && data && s_bt_last.ps_bytes == bytes && !memcmp(s_bt_last.ps, data, bytes)) { s_bt_dedup_n++; return; }
    bt_a_bytes* a = (bt_a_bytes*)bt_rec(bt_x_bind_ps_constants, (u32)sizeof *a + (data ? bytes : 0u));
    if (!a) { bt_forget(); s_bt_real.bind_ps_constants(s_bt_real.user, data, bytes); return; }
    a->bytes = bytes; a->has = data != NULL;
    if (data && bytes) memcpy(a + 1, data, bytes);
    bt_commit(a);
    s_bt_last.ps_ok = 0;
    if (data && bytes <= BT_PS_CACHE) {
        memcpy(s_bt_last.ps, data, bytes); s_bt_last.ps_bytes = bytes; s_bt_last.ps_ok = bt_dedup_on();
    }
}

/* The backend keeps `rows` until the draw: on the worker, the copied rows go
 * into a buffer of its own (at their row offsets), live until the next bind. */
#define BT_VS_ROWS 1024u
static __declspec(align(16)) u8 s_bt_vsrows[BT_VS_ROWS * 16u];
typedef struct { u32 first, count, tail_bytes, total_bytes, has; u32 pad; u8 tail[64]; } bt_a_vsr;   /* then count rows */
static void bt_x_bind_vs_constants_range(const u8* p)
{
    BT_A(bt_a_vsr, p);
    if (a->has && a->count) memcpy(s_bt_vsrows + (size_t)a->first * 16u, a + 1, (size_t)a->count * 16u);
    s_bt_real.bind_vs_constants_range(s_bt_real.user, a->has ? (const void*)s_bt_vsrows : NULL, a->first, a->count,
                                      a->tail_bytes ? (const void*)a->tail : NULL, a->tail_bytes, a->total_bytes);
}
static void bt_bind_vs_constants_range(void* u, const void* rows, u32 first, u32 count,
                                       const void* tail, u32 tail_bytes, u32 total_bytes)
{
    (void)u;
    /* what the worker's buffer cannot hold, as before (drained: in order; the
     * backend rejects such a range anyway -- its window is 528 rows, 64 tail bytes) */
    if ((rows && (first > BT_VS_ROWS || count > BT_VS_ROWS - first)) || tail_bytes > sizeof(((bt_a_vsr*)0)->tail) ||
        (tail_bytes && !tail)) {
        bt_forget();
        vk_tq_sync();
        s_bt_real.bind_vs_constants_range(s_bt_real.user, rows, first, count, tail, tail_bytes, total_bytes);
        return;
    }
    const u32 nrow = rows ? count : 0u;
    bt_a_vsr* a = (bt_a_vsr*)bt_rec(bt_x_bind_vs_constants_range, (u32)sizeof *a + nrow * 16u);
    if (!a) { bt_forget(); s_bt_real.bind_vs_constants_range(s_bt_real.user, rows, first, count, tail, tail_bytes, total_bytes); return; }
    a->first = first; a->count = count; a->tail_bytes = tail_bytes; a->total_bytes = total_bytes;
    a->has = rows != NULL; a->pad = 0;
    if (tail_bytes) memcpy(a->tail, tail, tail_bytes);
    if (nrow) memcpy(a + 1, (const u8*)rows + (size_t)first * 16u, (size_t)nrow * 16u);
    bt_commit(a);
}

typedef struct { u32 tex[RSX_BE_MAX_TEXTURES]; rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES]; u32 mask, has_t, has_s; } bt_a_tex;
static void bt_x_bind_textures(const u8* p)
{ BT_A(bt_a_tex, p); s_bt_real.bind_textures(s_bt_real.user, a->has_t ? a->tex : NULL, a->has_s ? a->samp : NULL, a->mask); }
static void bt_bind_textures(void* u, const u32* textures, const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)u;
    if (s_bt_last.tex_ok && textures && samplers && s_bt_last.thas_t && s_bt_last.thas_s && s_bt_last.tmask == mask &&
        !memcmp(s_bt_last.tex, textures, sizeof s_bt_last.tex) && !memcmp(s_bt_last.samp, samplers, sizeof s_bt_last.samp))
        { s_bt_dedup_n++; return; }
    bt_a_tex* a = (bt_a_tex*)bt_rec(bt_x_bind_textures, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.bind_textures(s_bt_real.user, textures, samplers, mask); return; }
    if (textures) memcpy(a->tex, textures, sizeof a->tex);
    if (samplers) memcpy(a->samp, samplers, sizeof a->samp);
    a->mask = mask; a->has_t = textures != NULL; a->has_s = samplers != NULL;
    bt_commit(a);
    s_bt_last.tex_ok = 0;
    if (textures && samplers) {
        memcpy(s_bt_last.tex, textures, sizeof s_bt_last.tex); memcpy(s_bt_last.samp, samplers, sizeof s_bt_last.samp);
        s_bt_last.tmask = mask; s_bt_last.thas_t = s_bt_last.thas_s = 1; s_bt_last.tex_ok = bt_dedup_on();
    }
}
typedef struct { u32 tex[RSX_BE_MAX_VERTEX_TEXTURES]; rsx_be_sampler_desc samp[RSX_BE_MAX_VERTEX_TEXTURES]; u32 mask, has_t, has_s; } bt_a_vtex;
static void bt_x_bind_vertex_textures(const u8* p)
{ BT_A(bt_a_vtex, p); s_bt_real.bind_vertex_textures(s_bt_real.user, a->has_t ? a->tex : NULL, a->has_s ? a->samp : NULL, a->mask); }
static void bt_bind_vertex_textures(void* u, const u32* textures, const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)u;
    if (s_bt_last.vtex_ok && textures && samplers && s_bt_last.vmask == mask &&
        !memcmp(s_bt_last.vtex, textures, sizeof s_bt_last.vtex) && !memcmp(s_bt_last.vsamp, samplers, sizeof s_bt_last.vsamp))
        { s_bt_dedup_n++; return; }
    bt_a_vtex* a = (bt_a_vtex*)bt_rec(bt_x_bind_vertex_textures, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.bind_vertex_textures(s_bt_real.user, textures, samplers, mask); return; }
    if (textures) memcpy(a->tex, textures, sizeof a->tex);
    if (samplers) memcpy(a->samp, samplers, sizeof a->samp);
    a->mask = mask; a->has_t = textures != NULL; a->has_s = samplers != NULL;
    bt_commit(a);
    s_bt_last.vtex_ok = 0;
    if (textures && samplers) {
        memcpy(s_bt_last.vtex, textures, sizeof s_bt_last.vtex); memcpy(s_bt_last.vsamp, samplers, sizeof s_bt_last.vsamp);
        s_bt_last.vmask = mask; s_bt_last.vtex_ok = bt_dedup_on();
    }
}

/* draws: vertices (vertex_count * stride bytes) then indices (index_count u32s) follow */
typedef struct { u32 topology, vbuf, vertex_count, stride, index_count, ibuf, has_v, has_i; } bt_a_draw;
static void bt_x_draw(const u8* p)
{
    BT_A(bt_a_draw, p);
    const u8* v = (const u8*)(a + 1);
    const u32 vb = a->has_v ? ((a->vertex_count * a->stride + 7u) & ~7u) : 0u;
    s_bt_real.draw(s_bt_real.user, (rsx_topology)a->topology, a->has_v ? (const void*)v : NULL, a->vertex_count, a->stride,
                   a->has_i ? (const u32*)(const void*)(v + vb) : NULL, a->index_count);
}
static void bt_draw(void* u, rsx_topology topology, const void* vertices, u32 vertex_count, u32 stride,
                    const u32* indices, u32 index_count)
{
    (void)u;
    const u64 vbytes = vertices ? (u64)vertex_count * stride : 0u, ibytes = indices ? (u64)index_count * 4u : 0u;
    if (vbytes + ibytes > (TQ_RING_BYTES / 8u) - 4096u) {     /* (tq_alloc would drain; say so here) */
        vk_tq_sync();
        s_bt_real.draw(s_bt_real.user, topology, vertices, vertex_count, stride, indices, index_count);
        return;
    }
    const u32 vb = (u32)((vbytes + 7u) & ~7ull);
    bt_a_draw* a = (bt_a_draw*)bt_rec(bt_x_draw, (u32)sizeof *a + vb + (u32)ibytes);
    if (!a) { bt_forget(); s_bt_real.draw(s_bt_real.user, topology, vertices, vertex_count, stride, indices, index_count); return; }
    a->topology = (u32)topology; a->vbuf = 0; a->vertex_count = vertex_count; a->stride = stride;
    a->index_count = index_count; a->ibuf = 0; a->has_v = vertices != NULL; a->has_i = indices != NULL;
    u8* q = (u8*)(a + 1);
    if (vbytes) memcpy(q, vertices, (size_t)vbytes);
    if (ibytes) memcpy(q + vb, indices, (size_t)ibytes);
    bt_commit(a);
}
static void bt_x_draw_vbuf(const u8* p)
{
    BT_A(bt_a_draw, p);
    s_bt_real.draw_vbuf(s_bt_real.user, (rsx_topology)a->topology, a->vbuf, a->vertex_count, a->stride,
                        a->has_i ? (const u32*)(const void*)(a + 1) : NULL, a->index_count);
}
static void bt_draw_vbuf(void* u, rsx_topology topology, u32 vbuf, u32 vertex_count, u32 stride,
                         const u32* indices, u32 index_count)
{
    (void)u;
    const u64 ibytes = indices ? (u64)index_count * 4u : 0u;
    if (ibytes > (TQ_RING_BYTES / 8u) - 4096u) {
        vk_tq_sync();
        s_bt_real.draw_vbuf(s_bt_real.user, topology, vbuf, vertex_count, stride, indices, index_count);
        return;
    }
    bt_a_draw* a = (bt_a_draw*)bt_rec(bt_x_draw_vbuf, (u32)sizeof *a + (u32)ibytes);
    if (!a) { bt_forget(); s_bt_real.draw_vbuf(s_bt_real.user, topology, vbuf, vertex_count, stride, indices, index_count); return; }
    a->topology = (u32)topology; a->vbuf = vbuf; a->vertex_count = vertex_count; a->stride = stride;
    a->index_count = index_count; a->ibuf = 0; a->has_v = 0; a->has_i = indices != NULL;
    if (ibytes) memcpy(a + 1, indices, (size_t)ibytes);
    bt_commit(a);
}
static void bt_x_draw_vbuf_ib(const u8* p)
{
    BT_A(bt_a_draw, p);
    s_bt_real.draw_vbuf_ib(s_bt_real.user, (rsx_topology)a->topology, a->vbuf, a->vertex_count, a->stride, a->ibuf, a->index_count);
}
static void bt_draw_vbuf_ib(void* u, rsx_topology topology, u32 vbuf, u32 vertex_count, u32 stride, u32 ibuf, u32 index_count)
{
    (void)u;
    bt_a_draw* a = (bt_a_draw*)bt_rec(bt_x_draw_vbuf_ib, sizeof *a);
    if (!a) { bt_forget(); s_bt_real.draw_vbuf_ib(s_bt_real.user, topology, vbuf, vertex_count, stride, ibuf, index_count); return; }
    a->topology = (u32)topology; a->vbuf = vbuf; a->vertex_count = vertex_count; a->stride = stride;
    a->index_count = index_count; a->ibuf = ibuf; a->has_v = 0; a->has_i = 0;
    bt_commit(a);
}

/* The end of a FIFO drain (rsx_split.h g_rsx_drain_end_hook): the worker idle
 * before the frame loop touches the backend directly. */
static void vk_bt_quiesce(void) { vk_tq_sync(); bt_forget(); }

/* The table the engine gets: the backend's own, each entry it has through a
 * thunk. NULL when the switch is off or recording is not threaded. */
static const rsx_draw_backend* vk_bt_install(const rsx_draw_backend* real)
{
    if (!vk_bt_env() || !s_tq.running || !real) return NULL;
    s_bt_real = *real;
    s_bt_be = *real;
#define BT_SET(name) if (real->name) s_bt_be.name = bt_##name;
    BT_SET(submit_and_wait) BT_SET(texture_create) BT_SET(texture_upload) BT_SET(texture_release)
    BT_SET(color_target_create) BT_SET(color_target_release) BT_SET(surface_view)
    BT_SET(depth_target_create) BT_SET(depth_target_release) BT_SET(depth_snapshot) BT_SET(color_snapshot)
    BT_SET(depth_snapshot_rgba8) BT_SET(pipeline_create) BT_SET(pipeline_release)
    BT_SET(bind_targets) BT_SET(bind_pipeline) BT_SET(bind_vs_constants) BT_SET(bind_ps_constants)
    BT_SET(bind_textures) BT_SET(bind_vertex_textures) BT_SET(set_viewport) BT_SET(set_scissor)
    BT_SET(set_stencil_ref) BT_SET(draw) BT_SET(clear_color) BT_SET(clear_depth_stencil) BT_SET(present)
    BT_SET(readback) BT_SET(bind_vs_constants_range) BT_SET(texture_create_3d) BT_SET(texture_upload_3d)
    BT_SET(light_pass) BT_SET(ssao_pass) BT_SET(vbuf_create) BT_SET(vbuf_release) BT_SET(draw_vbuf)
    BT_SET(image_load) BT_SET(image_save) BT_SET(set_hud) BT_SET(set_depth_bias) BT_SET(set_depth_clamp)
    BT_SET(draw_vbuf_ib) BT_SET(vbuf_prefetch) BT_SET(vs_constants_gen)
#undef BT_SET
    /* init / shutdown stay the backend's own: they run before and after */
    s_bt_on = 1;
    g_rsx_drain_end_hook = vk_bt_quiesce;
    VK_LOG("backend draw work on the recording worker (PS3RECOMP_VK_BT=0 = on this thread)\n");
    return &s_bt_be;
}

#else   /* not Windows: no recording worker */
static const rsx_draw_backend* vk_bt_install(const rsx_draw_backend* real) { (void)real; return NULL; }
static int s_bt_on;
#endif

#endif /* PS3RECOMP_VK_THREADED_BE_H */
