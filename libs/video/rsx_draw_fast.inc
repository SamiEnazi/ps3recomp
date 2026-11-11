/*
 * (fastdraw) PS3RECOMP_ENG_FASTDRAW: the short tail for repeated draws.
 * Included by rsx_draw_engine.c after rsx_draw_delta.inc, before sink_end.
 *
 * The delta engine lets a draw take every part of its state from the draw
 * before it -- programs, render state, targets, textures, pipeline (DL_STATE:
 * 78% of the ~8,100 draws a frame at the city dock, 1080p + extras) -- yet such
 * a draw still ran the whole tail of sink_end: copies of the reused parts into
 * locals, some twenty diagnostic switches, every bind and set to the backend
 * (which already holds what they send) and the backend's whole draw. A DL_STATE
 * draw whose vertices and indices are resident (an index-cache list over a
 * vertex-cache buffer: nearly all of them) takes fd_draw instead:
 *  - the engine's own effects the tail has for such a draw, in its order: the
 *    target stamps dl_surf_take moves, the texture flags dl_tex_take leaves,
 *    the pipeline's fragment-constant block, the depth target's first clear,
 *    the guest-draw count, the depth write marks, dl_end;
 *  - sent: the transform constants (every draw: no draw of the 8,100 had the
 *    rows of the draw before), the fragment constants (built as the tail
 *    builds them; the backend keeps its window for the same bytes), and the
 *    scissor, stencil reference, depth bias and depth clamp only when what
 *    they are made from changed -- registers outside the delta engine's
 *    groups and two live switches, which dl_end records (s_dl.dyn);
 *  - not sent: targets, pipeline, textures, vertex textures, viewport and the
 *    HUD flag: the previous draw's, which is what the tail would send again;
 *  - the draw through the backend's draw_vbuf_ib_again when it has one (the
 *    Vulkan backend repeats the previous draw's record there), else
 *    draw_vbuf_ib.
 * The whole tail runs instead while any of its diagnostics is on (fd_init's
 * list), with PS3RECOMP_ENG_DRAWSTATS, PS3RECOMP_ENG_VPC_FULL or a program that
 * reads the whole constant file, a HUD-split wide picture
 * (PS3_ASPECT_HUD_AFTER), a PS3RECOMP_ENG_SKIP_FP list (set, or changed
 * live), and for a draw whose vertices or indices are not resident.
 *
 * PS3RECOMP_ENG_FASTDRAW_VERIFY=1: every draw runs the whole tail (and renders
 * as without the switch). For each draw fd_draw would have taken, what it
 * would have sent is computed first (fd_compute, the code fd_draw runs) and
 * compared with what the tail sent, through a recording copy of the backend's
 * table (fdv_*): each bind fd_draw leaves out must be what the backend held
 * already, each it sends the tail's value, the draw call the same. The first
 * differences print as [fastdraw-verify] lines. The backend checks its own
 * fast record the same way (rsx_vulkan_backend.c, vk_fd_verify).
 * PS3RECOMP_ENG_FASTDRAW_VERIFY=2 also runs the production code itself: for
 * each such draw fd_draw runs first, dry -- its sends are recorded and not
 * forwarded, the engine state it changes is taken and put back -- and then
 * the whole tail; the engine state both leave (target stamps, the depth
 * target, the texture flags, the fragment block, the guest-draw count, the
 * delta engine's record) and what the backend holds after each must be the
 * same (fdv_dry_run, fdv_check). The backend likewise runs its fast record
 * dry (Vulkan commands caught, not recorded, its state put back) before the
 * whole body and compares the two command streams (vk_fd_verify).
 * Counters with the [delta] line (PS3RECOMP_LIMITS=1 / _ENG_DELTA_STATS=1):
 * [fastdraw], the share of draws that took it and why the backend did not.
 * PS3RECOMP_ENG_FASTDRAW=0: every draw through the whole tail, as before.
 */

static int s_fd_on = -1;                  /* 1 on (default), 0 off, -1 not read yet */
/* s_fd_verify (PS3RECOMP_ENG_FASTDRAW_VERIFY) is declared with the pair memo (rsx_draw_engine.c) */
static int s_fd_skipfp;                   /* fd_skipfp_set calls (the first is the registration's own) */
static void fd_skipfp_set(int id, int v) { (void)id; (void)v; __atomic_add_fetch(&s_fd_skipfp, 1, __ATOMIC_RELAXED); }
static struct { u64 state, fast; } s_fdn; /* this window: DL_STATE draws, and of them through fd_draw */
/* The backend's counters ([fastdraw] line): 0 fast records; 1..6 why not
 * (no token, a bind since, the pass ended, sets / objects gone, other, arena
 * full); 7 verify differences; 8 draws verified. */
u64 g_eng_fd_be[10];
/* (fastdraw) PS3RECOMP_FASTDRAW2's backend counters: descriptor sets found in
 * the memo of recent sets, memo-eligible draws; verify checked, differences */
u64 g_eng_fd2[8];   /* [4] / [5]: VERIFY=2, the backend's fast record run dry: checked, differences */

/* What fd_draw sends (fd_compute; VERIFY compares it with the tail's). */
typedef struct {
    float xf[8];                          /* the viewport tail of the transform constants */
    u32 fp_bytes, gen;
    int sc_send, sref_send, bias_send, clamp_send;
    u32 sc[4], sref;
    rsx_topology topology;
    u32 vbuf, vcount, stride, ibuf, icount;
} fd_plan;

static void fdv_install(void);

static void fd_init(void)
{
    /* (fastdraw) PS3RECOMP_FASTDRAW2: the pair memo's ways (the backend reads
     * the switch for its set memo, vk_fd_init) */
    { const char* f2 = getenv("PS3RECOMP_FASTDRAW2");
      s_pair_ways = (f2 && *f2 == '0') ? 1u : ENG_PAIR_WAYS;
      fprintf(stderr, "[rsx engine] round 2 (PS3RECOMP_FASTDRAW2): %s -- pair memo %u way(s)\n",
              s_pair_ways > 1u ? "on" : "off", s_pair_ways); }
    const char* e = getenv("PS3RECOMP_ENG_FASTDRAW");
    int on = !(e && *e == '0');
    const char* why = NULL;
    if (on && s_dl_on <= 0) { on = 0; why = "the delta engine is off"; }
    /* the tail's diagnostics (each reads its switch once, so this does too) */
    static const char* const diag[] = {
        "PS3RECOMP_ENG_TEXSTATE", "PS3RECOMP_ENG_REGDUMP_PIPE", "PS3RECOMP_ENG_TEXSTATE_PIPE", "PS3RECOMP_ENG_TEXRAW_FP",
        "PS3RECOMP_ENG_DUMPTEX_FP", "PS3RECOMP_ENG_MRTLOG", "PS3RECOMP_ENG_VPC_FULL", "PS3RECOMP_ENG_CONSTLOG",
        "PS3RECOMP_ENG_FPK_PIPE", "PS3RECOMP_ENG_FPK_FP", "PS3RECOMP_ENG_VPC_WATCH", "PS3RECOMP_ENG_OCCDUMP",
        "PS3RECOMP_ENG_OCCDUMP_TRIGGER", "PS3RECOMP_ENG_HUDLOG", "PS3RECOMP_ENG_HUDLOG_KEY", "PS3RECOMP_FX_LOG",
        "PS3RECOMP_ENG_RESOLVE_LOG", "PS3RECOMP_ENG_DRAWSTATS" };
    for (u32 i = 0; on && i < sizeof diag / sizeof diag[0]; i++) if (getenv(diag[i])) { on = 0; why = diag[i]; }
    if (on) {   /* a fragment-program skip list: the tail's (it may also come from PS3_TOGGLE_FILE) */
        char buf[PS3_TOGGLE_STR];
        const char* l = ps3_toggle_str(PS3T_ENG_SKIP_FP, buf, sizeof buf);
        if (l && *l) { on = 0; why = "PS3RECOMP_ENG_SKIP_FP"; }
        else ps3_toggle_watch(PS3T_ENG_SKIP_FP, fd_skipfp_set);
    }
    const char* v = getenv("PS3RECOMP_ENG_FASTDRAW_VERIFY");
    s_fd_verify = (on && v && *v && *v != '0') ? (atoi(v) >= 2 ? 2 : 1) : 0;   /* 2: the production code run too */
    if (s_fd_verify) fdv_install();
    s_fd_on = on;
    fprintf(stderr, "[rsx engine] fast path for repeated draws: %s%s%s%s (PS3RECOMP_ENG_FASTDRAW=0 = every draw through the whole tail)\n",
            on ? "on" : "off", why ? " -- " : "", why ? why : "",
            s_fd_verify == 2 ? ", VERIFY=2 (the whole tail runs; the fast path's sends and the short tail itself, run dry, are compared)"
            : s_fd_verify ? ", VERIFY (the whole tail runs; what the fast path would send is compared)" : "");
}

/* Takes it: resident vertices and indices (the tail's ic_buf case), the
 * ranged constant upload, no HUD split, no skip list since fd_init. */
static inline int fd_takes(u32 n_draw)
{
    return s_ic_hit && dc.vbuf && s_ic_cur && s_ic_cur->ibuf && s_ic_cur->n_draw == n_draw &&
           g.be->bind_vs_constants_range && !g.vpc_full && (!g.be->set_hud || s_hud_n == 0) &&
           __atomic_load_n(&s_fd_skipfp, __ATOMIC_RELAXED) <= 1;
}

/* The fragment block's size: the program's constants, then fp_alpha (no
 * fp_dbounds: dl_tex_take leaves s_dbz.unit 0). */
static u32 fd_fp_bytes(void)
{
    const u32 nslots = s_dl.fp_constants->count ? s_dl.fp_constants->count : 1u;
    return (nslots + 1u) * 16u;
}

/* The tail's scissor: the guest's intersected with target A, on the scaled
 * grid for PS3RECOMP_FX_RES. */
static void fd_scissor(u32 h, u32 v, u32 out[4])
{
    const u32 target = s_dl.targets[0], tkq = s_dl.tkq;
    u32 sx = 0, sy = 0;
    u32 sw = g.surfaces[target].w, sh = g.surfaces[target].h;
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
    if (tkq > 4u) {
        const u32 r = eng_fxres_px(sx + sw, tkq), b = eng_fxres_px(sy + sh, tkq);
        sx = sx * tkq / 4u; sy = sy * tkq / 4u; sw = r - sx; sh = b - sy;
    }
    out[0] = sx; out[1] = sy; out[2] = sw; out[3] = sh;
}

/* Everything fd_draw sends, computed from the reused state as the tail
 * computes it; the fragment block is written to fp (fd_fp_bytes of room).
 * No effect on the engine or the backend. */
static void fd_compute(fd_plan* p, rsx_topology topology, const rsx_vertex_layout_plan* layout, u32 n_draw, u8* fp)
{
    const rsx_dsp_surface* sf = &s_dl.sf;
    const rsx_dsp_viewport* vp = &s_dl.vpt;
    const float W = sf->clip_w ? (float)sf->clip_w : (float)g.width;
    const float H = sf->clip_h ? (float)sf->clip_h : (float)g.height;
    p->xf[0] = 1; p->xf[1] = 1; p->xf[2] = 1; p->xf[3] = 0; p->xf[4] = 0; p->xf[5] = 0; p->xf[6] = 0; p->xf[7] = 0;
    if (vp->scale[0] != 0.0f || vp->translate[0] != 0.0f) {
        p->xf[0] = vp->scale[0] / (W * 0.5f);
        p->xf[1] = -(vp->scale[1] / (H * 0.5f));
        p->xf[2] = vp->scale[2];
        p->xf[4] = (vp->translate[0] - W * 0.5f) / (W * 0.5f);
        p->xf[5] = -((vp->translate[1] - H * 0.5f) / (H * 0.5f));
        p->xf[6] = vp->translate[2];
    }
    const rsx_fp_constant_block* fc = s_dl.fp_constants;
    const u32 nslots = fc->count ? fc->count : 1u;
    p->fp_bytes = (nslots + 1u) * 16u;
    memset(fp, 0, p->fp_bytes);
    if (fc->count) memcpy(fp, fc->values, fc->count * 16u);
    { float* alpha = (float*)(fp + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(s_dl.rs.alpha_ref_raw, s_dl.rs.alpha_ref_format);
      const u32 win = rsx_dsp_reg(&g.rsx, 0x1D88);
      const int bottom = (win >> 12) & 1;
      alpha[1] = bottom ? -1.0f : 1.0f;
      alpha[2] = bottom ? (float)(win & 0xFFFu) : 0.0f;
      alpha[3] = 0.0f; }
    /* the dynamic state: sent again only when what it is made from changed */
    const u32 sh = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), sv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
    p->sc_send = sh != s_dl.dyn[0] || sv != s_dl.dyn[1];
    if (p->sc_send) fd_scissor(sh, sv, p->sc);
    else p->sc[0] = p->sc[1] = p->sc[2] = p->sc[3] = 0;
    const u32 sr = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF);
    p->sref = sr & 0xFFu;
    p->sref_send = sr != s_dl.dyn[2];
    p->bias_send = g.be->set_depth_bias &&
        (rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL) != s_dl.dyn[3] || rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_SCALE) != s_dl.dyn[4] ||
         rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS) != s_dl.dyn[5] || __atomic_load_n(&s_poly_offset, __ATOMIC_RELAXED) != s_dl.dyn_poly);
    p->clamp_send = g.be->set_depth_clamp && __atomic_load_n(&s_depth_clamp, __ATOMIC_RELAXED) != s_dl.dyn_clamp;
    p->gen = g.rsx.const_gen;
    p->topology = topology; p->vbuf = dc.vbuf; p->vcount = dc.n_verts; p->stride = layout->stride;
    p->ibuf = s_ic_cur->ibuf; p->icount = n_draw;
}

/* The short tail (see the top of this file). */
static void fd_draw(u32 prim, rsx_topology topology, const rsx_vertex_layout_plan* layout, u32 n_draw)
{
    /* the tail's effects for a reused draw, in its order: dl_surf_take's
     * stamps, dl_tex_take's flags, the pipeline's fragment-constant block */
    for (u32 i = 0; i < s_dl.nbumps; i++) g.surfaces[s_dl.bumps[i]].stamp = ++s_surf_clock;
    s_dbz.unit = 0; s_fbf.cand = 0; s_fbf.applied = 0; s_tex_volatile = 0;
    s_fbf.applied = s_dl.fbf_applied; g.fp_constants = s_dl.fp_constants;
    const u32 fp_bytes = fd_fp_bytes();
    if (g.fp_cb_cap < fp_bytes) {
        u8* nb = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!nb) { eng_drop("n0", prim); return; }
        g.fp_cb = nb;
        g.fp_cb_cap = fp_bytes;
    }
    fd_plan p;
    fd_compute(&p, topology, layout, n_draw, g.fp_cb);
    DL_MARK(7);
    /* the depth attachment's first clear, as the tail */
    const u32 zslot = s_dl.zslot;
    if (zslot != ENG_INVALID && !g.zdepths[zslot].cleared) {
        g.be->clear_depth_stencil(g.be->user, g.zdepths[zslot].handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0);
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }
    if (g.be->vs_constants_gen) g.be->vs_constants_gen(g.be->user, p.gen);
    g.be->bind_vs_constants_range(g.be->user, g.rsx.constants, g.vpc_lo, g.vpc_n, p.xf, sizeof p.xf, ENG_VP_CB_BYTES);
    g.be->bind_ps_constants(g.be->user, g.fp_cb, p.fp_bytes);
    if (p.sc_send) g.be->set_scissor(g.be->user, p.sc[0], p.sc[1], p.sc[2], p.sc[3]);
    if (p.sref_send) g.be->set_stencil_ref(g.be->user, p.sref);
    if (p.bias_send) eng_apply_depth_bias(s_dl.sf.zeta_offset, &s_dl.rs);
    if (p.clamp_send) eng_apply_depth_clip(s_dl.sf.zeta_offset, &s_dl.rs, s_dl.vpt.scale[1]);
    DL_MARK(8);
    DL_MARK_DRAW0();
    if (g.be->draw_vbuf_ib_again)
        g.be->draw_vbuf_ib_again(g.be->user, p.topology, p.vbuf, p.vcount, p.stride, p.ibuf, p.icount);
    else
        g.be->draw_vbuf_ib(g.be->user, p.topology, p.vbuf, p.vcount, p.stride, p.ibuf, p.icount);
    DL_MARK_DRAW1();
    if (!s_dl.pipe_fixed) g.guest_draws++;
    if (zslot != ENG_INVALID && s_dl.rs.depth_test && s_dl.rs.depth_write) {   /* as the tail */
        g.zdepths[zslot].had_write = 1;
        g.zdepths[zslot].stamp = ++s_surf_clock;
        g.zdepths[zslot].kq = (u8)s_dl.tkq;
        g.zdepths[zslot].snapshot_valid = 0; g.zdepths[zslot].crop_z_valid = 0;
        g.zdepths[zslot].snapshot_rgba_valid = 0; g.zdepths[zslot].crop_valid = 0;
    }
    s_fdn.fast++;
    dl_end();
}

/* ---- VERIFY: a recording copy of the backend's table ------------------------ */
typedef struct {
    int tgt_ok; u32 tgt[RSX_BE_MAX_COLOR_TARGETS], ntgt, tdepth;
    int pipe_ok; u32 pipe;
    int tex_ok; u32 tex[RSX_BE_MAX_TEXTURES], tmask; rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    int vtex_ok; u32 vtex[RSX_BE_MAX_VERTEX_TEXTURES], vmask;
    int vp_ok; float vp[4];
    int sc_ok; u32 sc[4];
    int sref_ok; u32 sref;
    int bias_ok; float bias[2];
    int clamp_ok, clamp;
    int hud_ok, hud;
} fdv_state;                              /* what the backend holds, as last sent */
static struct {
    const rsx_draw_backend* real;
    rsx_draw_backend be;
    fdv_state sent, before;
    int armed;                            /* the draw being run is one fd_draw would have taken */
    int dry;                              /* VERIFY=2: fd_draw's dry run -- record, do not forward */
    u32 n_clear;                          /* clear_depth_stencil calls of this draw */
    fd_plan plan;
    u8* ps; u32 ps_cap;                   /* the plan's fragment block */
    u8* psc; u32 psc_cap;                 /* the tail's */
    /* this draw's sends */
    u32 n_gen, gen, n_vsr, n_vsf, n_ps, ps_bytes, n_draw, kind;
    const void* rows; u32 first, count, tail_bytes, total; u8 tail[64];
    rsx_topology topo; u32 vbuf, vcount, stride, ibuf, icount;
    u64 checked, diffs;
} s_fdv;
#define FDV_REAL s_fdv.real
static void fdv_bind_targets(void* u, const u32* s, u32 n, u32 d)
{
    fdv_state* t = &s_fdv.sent;
    const u32 c = n > RSX_BE_MAX_COLOR_TARGETS ? RSX_BE_MAX_COLOR_TARGETS : n;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) t->tgt[i] = (i < c && s) ? s[i] : 0u;
    t->ntgt = c; t->tdepth = d; t->tgt_ok = 1;
    if (!s_fdv.dry) FDV_REAL->bind_targets(u, s, n, d);
}
static void fdv_bind_pipeline(void* u, u32 p) { s_fdv.sent.pipe = p; s_fdv.sent.pipe_ok = 1; if (!s_fdv.dry) FDV_REAL->bind_pipeline(u, p); }
static void fdv_vs_constants_gen(void* u, u32 gen) { s_fdv.n_gen++; s_fdv.gen = gen; if (!s_fdv.dry) FDV_REAL->vs_constants_gen(u, gen); }
static void fdv_bind_vs_constants(void* u, const void* d, u32 b) { s_fdv.n_vsf++; if (!s_fdv.dry) FDV_REAL->bind_vs_constants(u, d, b); }
static void fdv_clear_depth_stencil(void* u, u32 d, u32 f, float z, u8 st)
{ s_fdv.n_clear++; if (!s_fdv.dry) FDV_REAL->clear_depth_stencil(u, d, f, z, st); }
static void fdv_bind_vs_constants_range(void* u, const void* rows, u32 first, u32 count, const void* tail, u32 tb, u32 total)
{
    s_fdv.n_vsr++; s_fdv.rows = rows; s_fdv.first = first; s_fdv.count = count; s_fdv.total = total; s_fdv.tail_bytes = tb;
    memset(s_fdv.tail, 0, sizeof s_fdv.tail);
    if (tail) memcpy(s_fdv.tail, tail, tb < sizeof s_fdv.tail ? tb : sizeof s_fdv.tail);
    if (!s_fdv.dry) FDV_REAL->bind_vs_constants_range(u, rows, first, count, tail, tb, total);
}
static void fdv_bind_ps_constants(void* u, const void* d, u32 b)
{
    s_fdv.n_ps++; s_fdv.ps_bytes = b;
    if (b > s_fdv.psc_cap) { u8* nb = (u8*)realloc(s_fdv.psc, b); if (nb) { s_fdv.psc = nb; s_fdv.psc_cap = b; } }
    if (d && b <= s_fdv.psc_cap) memcpy(s_fdv.psc, d, b);
    if (!s_fdv.dry) FDV_REAL->bind_ps_constants(u, d, b);
}
static void fdv_bind_textures(void* u, const u32* tex, const rsx_be_sampler_desc* smp, u32 mask)
{
    fdv_state* t = &s_fdv.sent;   /* what the backend keeps of it: handles of the enabled units, their samplers */
    for (u32 i = 0; i < RSX_BE_MAX_TEXTURES; i++) {
        const int on = (mask >> i) & 1u;
        t->tex[i] = on ? tex[i] : 0u;
        if (on && smp) t->samp[i] = smp[i];
    }
    t->tmask = mask; t->tex_ok = 1;
    if (!s_fdv.dry) FDV_REAL->bind_textures(u, tex, smp, mask);
}
static void fdv_bind_vertex_textures(void* u, const u32* tex, const rsx_be_sampler_desc* smp, u32 mask)
{
    fdv_state* t = &s_fdv.sent;
    for (u32 i = 0; i < RSX_BE_MAX_VERTEX_TEXTURES; i++) t->vtex[i] = (((mask >> i) & 1u) && tex) ? tex[i] : 0u;
    t->vmask = mask; t->vtex_ok = 1;
    if (!s_fdv.dry) FDV_REAL->bind_vertex_textures(u, tex, smp, mask);
}
static void fdv_set_viewport(void* u, float x, float y, float w, float h)
{
    fdv_state* t = &s_fdv.sent; t->vp[0] = x; t->vp[1] = y; t->vp[2] = w; t->vp[3] = h; t->vp_ok = 1;
    if (!s_fdv.dry) FDV_REAL->set_viewport(u, x, y, w, h);
}
static void fdv_set_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    fdv_state* t = &s_fdv.sent; t->sc[0] = x; t->sc[1] = y; t->sc[2] = w; t->sc[3] = h; t->sc_ok = 1;
    if (!s_fdv.dry) FDV_REAL->set_scissor(u, x, y, w, h);
}
static void fdv_set_stencil_ref(void* u, u32 r) { s_fdv.sent.sref = r; s_fdv.sent.sref_ok = 1; if (!s_fdv.dry) FDV_REAL->set_stencil_ref(u, r); }
static void fdv_set_hud(void* u, int hud, u32 w)
{
    if (hud < 3) { s_fdv.sent.hud = hud; s_fdv.sent.hud_ok = 1; }   /* 3 / 4: the present's, not a draw's */
    if (!s_fdv.dry) FDV_REAL->set_hud(u, hud, w);
}
static void fdv_set_depth_bias(void* u, float c, float s)
{ s_fdv.sent.bias[0] = c; s_fdv.sent.bias[1] = s; s_fdv.sent.bias_ok = 1; if (!s_fdv.dry) FDV_REAL->set_depth_bias(u, c, s); }
static void fdv_set_depth_clamp(void* u, int c) { s_fdv.sent.clamp = c; s_fdv.sent.clamp_ok = 1; if (!s_fdv.dry) FDV_REAL->set_depth_clamp(u, c); }
static void fdv_draw(void* u, rsx_topology t, const void* v, u32 vc, u32 st, const u32* idx, u32 ic)
{ s_fdv.n_draw++; s_fdv.kind = 1; if (!s_fdv.dry) FDV_REAL->draw(u, t, v, vc, st, idx, ic); }
static void fdv_draw_vbuf(void* u, rsx_topology t, u32 vb, u32 vc, u32 st, const u32* idx, u32 ic)
{ s_fdv.n_draw++; s_fdv.kind = 2; if (!s_fdv.dry) FDV_REAL->draw_vbuf(u, t, vb, vc, st, idx, ic); }
static void fdv_note_draw(u32 kind, rsx_topology t, u32 vb, u32 vc, u32 st, u32 ib, u32 ic)
{
    s_fdv.n_draw++; s_fdv.kind = kind;
    s_fdv.topo = t; s_fdv.vbuf = vb; s_fdv.vcount = vc; s_fdv.stride = st; s_fdv.ibuf = ib; s_fdv.icount = ic;
}
/* the tail's draw of a draw fd_draw would have taken goes to the backend's
 * draw_vbuf_ib_again, so the backend's own check runs on the draws it gets */
static void fdv_draw_vbuf_ib(void* u, rsx_topology t, u32 vb, u32 vc, u32 st, u32 ib, u32 ic)
{
    fdv_note_draw(3, t, vb, vc, st, ib, ic);
    if (s_fdv.dry) return;
    if (s_fdv.armed && FDV_REAL->draw_vbuf_ib_again) FDV_REAL->draw_vbuf_ib_again(u, t, vb, vc, st, ib, ic);
    else FDV_REAL->draw_vbuf_ib(u, t, vb, vc, st, ib, ic);
}
static void fdv_draw_vbuf_ib_again(void* u, rsx_topology t, u32 vb, u32 vc, u32 st, u32 ib, u32 ic)
{ fdv_note_draw(4, t, vb, vc, st, ib, ic); if (!s_fdv.dry) FDV_REAL->draw_vbuf_ib_again(u, t, vb, vc, st, ib, ic); }

static void fdv_install(void)
{
    s_fdv.real = g.be;
    s_fdv.be = *g.be;
#define FDV_SET(name) if (g.be->name) s_fdv.be.name = fdv_##name;
    FDV_SET(bind_targets) FDV_SET(bind_pipeline) FDV_SET(vs_constants_gen) FDV_SET(bind_vs_constants)
    FDV_SET(bind_vs_constants_range) FDV_SET(bind_ps_constants) FDV_SET(bind_textures) FDV_SET(bind_vertex_textures)
    FDV_SET(set_viewport) FDV_SET(set_scissor) FDV_SET(set_stencil_ref) FDV_SET(set_hud) FDV_SET(set_depth_bias)
    FDV_SET(set_depth_clamp) FDV_SET(draw) FDV_SET(draw_vbuf) FDV_SET(draw_vbuf_ib) FDV_SET(draw_vbuf_ib_again)
    FDV_SET(clear_depth_stencil)
#undef FDV_SET
    g.be = &s_fdv.be;
}

/* VERIFY=2: what fd_draw itself left, run dry (fdv_dry_run), for fdv_check */
static struct {
    int on;                               /* this draw's dry run happened */
    u32 surf_clock, nb, stamp[8], guest_draws, fp_bytes, fp_cap;
    int zvalid, tex_volatile;
    eng_zdepth z;
    __typeof__(s_dbz) dbz;
    __typeof__(s_fbf) fbf;
    const rsx_fp_constant_block* fpc;
    __typeof__(s_dl) dl;
    u8* fp;
    fdv_state sent;
    u32 n_gen, gen, n_vsr, n_vsf, n_ps, ps_bytes, n_draw, kind, n_clear;
    const void* rows; u32 first, count, tail_bytes, total; u8 tail[64];
    u8* ps; u32 ps_cap;
    rsx_topology topo; u32 vbuf, vcount, stride, ibuf, icount;
    u64 runs, skipped, diffs;
} s_fdv2;
static __typeof__(s_dl) s_fdv2_dl0;       /* the delta engine's record before the dry run */
static void fdv_dry_run(rsx_topology topology, const rsx_vertex_layout_plan* layout, u32 n_draw)
{
    s_fdv2.on = 0;
    if (s_dl.nbumps > 8u) { s_fdv2.skipped++; return; }
    const u32 zslot = s_dl.zslot, nb = s_dl.nbumps;
    /* what fd_draw may change */
    const u32 clock0 = s_surf_clock, gd0 = g.guest_draws;
    u32 st0[8];
    for (u32 i = 0; i < nb; i++) st0[i] = g.surfaces[s_dl.bumps[i]].stamp;
    eng_zdepth z0;
    if (zslot != ENG_INVALID) z0 = g.zdepths[zslot];
    const __typeof__(s_dbz) dbz0 = s_dbz;
    const __typeof__(s_fbf) fbf0 = s_fbf;
    const int tv0 = s_tex_volatile;
    const rsx_fp_constant_block* fpc0 = g.fp_constants;
    s_fdv2_dl0 = s_dl;
    const __typeof__(s_fdn) fdn0 = s_fdn;
    const fdv_state sent0 = s_fdv.sent;
    /* the production code, its sends recorded and not forwarded */
    s_fdv.n_gen = s_fdv.n_vsr = s_fdv.n_vsf = s_fdv.n_ps = s_fdv.n_draw = s_fdv.n_clear = 0; s_fdv.kind = 0;
    s_fdv.dry = 1;
    fd_draw(g.rsx.current_primitive, topology, layout, n_draw);
    s_fdv.dry = 0;
    /* what it left */
    s_fdv2.surf_clock = s_surf_clock; s_fdv2.guest_draws = g.guest_draws; s_fdv2.nb = nb;
    for (u32 i = 0; i < nb; i++) s_fdv2.stamp[i] = g.surfaces[s_fdv2_dl0.bumps[i]].stamp;
    s_fdv2.zvalid = zslot != ENG_INVALID;
    if (s_fdv2.zvalid) s_fdv2.z = g.zdepths[zslot];
    s_fdv2.dbz = s_dbz; s_fdv2.fbf = s_fbf; s_fdv2.tex_volatile = s_tex_volatile; s_fdv2.fpc = g.fp_constants;
    s_fdv2.dl = s_dl;
    s_fdv2.fp_bytes = 0;
    if (g.fp_constants) {
        const u32 fb = (g.fp_constants->count ? g.fp_constants->count : 1u) * 16u + 16u;
        if (fb > s_fdv2.fp_cap) { u8* nbuf = (u8*)realloc(s_fdv2.fp, fb); if (nbuf) { s_fdv2.fp = nbuf; s_fdv2.fp_cap = fb; } }
        if (fb <= s_fdv2.fp_cap && fb <= g.fp_cb_cap) { memcpy(s_fdv2.fp, g.fp_cb, fb); s_fdv2.fp_bytes = fb; }
    }
    s_fdv2.sent = s_fdv.sent;
    s_fdv2.n_gen = s_fdv.n_gen; s_fdv2.gen = s_fdv.gen; s_fdv2.n_vsr = s_fdv.n_vsr; s_fdv2.n_vsf = s_fdv.n_vsf;
    s_fdv2.rows = s_fdv.rows; s_fdv2.first = s_fdv.first; s_fdv2.count = s_fdv.count;
    s_fdv2.tail_bytes = s_fdv.tail_bytes; s_fdv2.total = s_fdv.total; memcpy(s_fdv2.tail, s_fdv.tail, sizeof s_fdv2.tail);
    s_fdv2.n_ps = s_fdv.n_ps; s_fdv2.ps_bytes = s_fdv.ps_bytes;
    if (s_fdv.n_ps && s_fdv.ps_bytes <= s_fdv.psc_cap) {
        if (s_fdv.ps_bytes > s_fdv2.ps_cap) { u8* nbuf = (u8*)realloc(s_fdv2.ps, s_fdv.ps_bytes); if (nbuf) { s_fdv2.ps = nbuf; s_fdv2.ps_cap = s_fdv.ps_bytes; } }
        if (s_fdv.ps_bytes <= s_fdv2.ps_cap) memcpy(s_fdv2.ps, s_fdv.psc, s_fdv.ps_bytes);
    }
    s_fdv2.n_draw = s_fdv.n_draw; s_fdv2.kind = s_fdv.kind; s_fdv2.n_clear = s_fdv.n_clear;
    s_fdv2.topo = s_fdv.topo; s_fdv2.vbuf = s_fdv.vbuf; s_fdv2.vcount = s_fdv.vcount; s_fdv2.stride = s_fdv.stride;
    s_fdv2.ibuf = s_fdv.ibuf; s_fdv2.icount = s_fdv.icount;
    /* back to the state before it: the whole tail runs from there */
    s_surf_clock = clock0; g.guest_draws = gd0;
    for (u32 i = nb; i-- > 0;) g.surfaces[s_fdv2_dl0.bumps[i]].stamp = st0[i];
    if (zslot != ENG_INVALID) g.zdepths[zslot] = z0;
    s_dbz = dbz0; s_fbf = fbf0; s_tex_volatile = tv0; g.fp_constants = fpc0;
    s_dl = s_fdv2_dl0; s_fdn = fdn0; s_fdv.sent = sent0;
    s_fdv2.on = 1; s_fdv2.runs++;
}
/* After the whole tail: what it left against what fd_draw left (VERIFY=2). */
static void fdv_dry_check(void)
{
    s_fdv2.on = 0;
    const fdv_state* a = &s_fdv.sent;
    const fdv_state* b = &s_fdv2.sent;
    char w[400]; int wp = 0; w[0] = 0;
#define FDV_W(cond, name) do { if (cond) wp += snprintf(w + wp, sizeof w - (size_t)wp, "%s%s", wp ? " " : "", name); } while (0)
    int st_same = s_surf_clock == s_fdv2.surf_clock;
    for (u32 i = 0; i < s_fdv2.nb && st_same; i++) st_same = g.surfaces[s_dl.bumps[i]].stamp == s_fdv2.stamp[i];
    FDV_W(!st_same, "target-stamps");
    FDV_W(s_fdv2.zvalid && memcmp(&g.zdepths[s_dl.zslot], &s_fdv2.z, sizeof s_fdv2.z), "depth-target");
    FDV_W(g.guest_draws != s_fdv2.guest_draws, "guest-draws");
    FDV_W(memcmp(&s_dbz, &s_fdv2.dbz, sizeof s_dbz) || memcmp(&s_fbf, &s_fdv2.fbf, sizeof s_fbf) ||
          s_tex_volatile != s_fdv2.tex_volatile, "texture-flags");
    FDV_W(g.fp_constants != s_fdv2.fpc || (s_fdv2.fp_bytes && memcmp(g.fp_cb, s_fdv2.fp, s_fdv2.fp_bytes)), "fragment-block");
    FDV_W(memcmp(&s_dl, &s_fdv2.dl, sizeof s_dl), "delta-record");
    FDV_W(a->tgt_ok != b->tgt_ok || a->ntgt != b->ntgt || a->tdepth != b->tdepth || memcmp(a->tgt, b->tgt, sizeof a->tgt), "targets");
    FDV_W(a->pipe_ok != b->pipe_ok || a->pipe != b->pipe, "pipeline");
    int tex_same = a->tex_ok == b->tex_ok && a->tmask == b->tmask && !memcmp(a->tex, b->tex, sizeof a->tex);
    for (u32 i = 0; tex_same && i < RSX_BE_MAX_TEXTURES; i++)
        if (((a->tmask >> i) & 1u) && memcmp(&a->samp[i], &b->samp[i], sizeof a->samp[i])) tex_same = 0;
    FDV_W(!tex_same, "textures");
    FDV_W(a->vtex_ok != b->vtex_ok || a->vmask != b->vmask || memcmp(a->vtex, b->vtex, sizeof a->vtex), "vertex-textures");
    FDV_W(a->vp_ok != b->vp_ok || memcmp(a->vp, b->vp, sizeof a->vp), "viewport");
    FDV_W(a->sc_ok != b->sc_ok || memcmp(a->sc, b->sc, sizeof a->sc), "scissor");
    FDV_W(a->sref_ok != b->sref_ok || a->sref != b->sref, "stencil-ref");
    FDV_W(a->bias_ok != b->bias_ok || memcmp(a->bias, b->bias, sizeof a->bias), "depth-bias");
    FDV_W(a->clamp_ok != b->clamp_ok || a->clamp != b->clamp, "depth-clamp");
    FDV_W(a->hud_ok != b->hud_ok || a->hud != b->hud, "hud");
    FDV_W(s_fdv.n_gen != s_fdv2.n_gen || (s_fdv2.n_gen && s_fdv.gen != s_fdv2.gen), "constant-generation");
    FDV_W(s_fdv.n_vsr != s_fdv2.n_vsr || s_fdv.n_vsf != s_fdv2.n_vsf || s_fdv.rows != s_fdv2.rows || s_fdv.first != s_fdv2.first ||
          s_fdv.count != s_fdv2.count || s_fdv.tail_bytes != s_fdv2.tail_bytes || s_fdv.total != s_fdv2.total ||
          memcmp(s_fdv.tail, s_fdv2.tail, sizeof s_fdv.tail), "vertex-constants");
    FDV_W(s_fdv.n_ps != s_fdv2.n_ps || s_fdv.ps_bytes != s_fdv2.ps_bytes || s_fdv.ps_bytes > s_fdv.psc_cap ||
          s_fdv2.ps_bytes > s_fdv2.ps_cap || memcmp(s_fdv.psc, s_fdv2.ps, s_fdv.ps_bytes), "fragment-constants");
    FDV_W(s_fdv.n_clear != s_fdv2.n_clear, "depth-clear");
    FDV_W(s_fdv.n_draw != 1 || s_fdv2.n_draw != 1 || s_fdv.topo != s_fdv2.topo || s_fdv.vbuf != s_fdv2.vbuf ||
          s_fdv.vcount != s_fdv2.vcount || s_fdv.stride != s_fdv2.stride || s_fdv.ibuf != s_fdv2.ibuf ||
          s_fdv.icount != s_fdv2.icount, "draw");
#undef FDV_W
    if (!wp) return;
    s_fdv2.diffs++;
    static u32 n;
    if (n++ >= 64u && (n & 1023u)) return;
    char line[600];
    const int k = snprintf(line, sizeof line, "[fastdraw-verify] dry #%u frame %u draw %u: the short tail itself left"
                           " something else than the whole tail (%s)\n", n, g.frames, s_draw_serial, w);
    rsx_log_line(line, k, (int)sizeof line);
}

/* Before the tail of a draw fd_draw would have taken: the plan, and what the
 * backend holds now (VERIFY=2: fd_draw itself run dry first). */
static void fdv_arm(rsx_topology topology, const rsx_vertex_layout_plan* layout, u32 n_draw)
{
    if (s_fd_verify >= 2) fdv_dry_run(topology, layout, n_draw);
    const u32 b = fd_fp_bytes();
    if (b > s_fdv.ps_cap) {
        u8* nb = (u8*)realloc(s_fdv.ps, b);
        if (!nb) return;
        s_fdv.ps = nb; s_fdv.ps_cap = b;
    }
    fd_compute(&s_fdv.plan, topology, layout, n_draw, s_fdv.ps);
    s_fdv.before = s_fdv.sent;
    s_fdv.n_gen = s_fdv.n_vsr = s_fdv.n_vsf = s_fdv.n_ps = s_fdv.n_draw = s_fdv.n_clear = 0;
    s_fdv.kind = 0;
    s_fdv.armed = 1;
}

/* After that tail (sink_end): what it sent against the plan. */
static void fdv_check(void)
{
    s_fdv.armed = 0;
    s_fdv.checked++;
    if (s_fdv2.on) fdv_dry_check();   /* VERIFY=2 */
    const fd_plan* p = &s_fdv.plan;
    const fdv_state* a = &s_fdv.sent;
    const fdv_state* b = &s_fdv.before;
    char w[400]; int wp = 0; w[0] = 0;
#define FDV_W(cond, name) do { if (cond) wp += snprintf(w + wp, sizeof w - (size_t)wp, "%s%s", wp ? " " : "", name); } while (0)
    FDV_W(s_fdv.n_draw != 1 || (s_fdv.kind != 3 && s_fdv.kind != 4), "draw-call");
    FDV_W(s_fdv.n_draw == 1 && (s_fdv.kind == 3 || s_fdv.kind == 4) &&
          (s_fdv.topo != p->topology || s_fdv.vbuf != p->vbuf || s_fdv.vcount != p->vcount || s_fdv.stride != p->stride ||
           s_fdv.ibuf != p->ibuf || s_fdv.icount != p->icount), "draw-arguments");
    FDV_W(a->tgt_ok != b->tgt_ok || a->ntgt != b->ntgt || a->tdepth != b->tdepth || memcmp(a->tgt, b->tgt, sizeof a->tgt), "targets");
    FDV_W(a->pipe_ok != b->pipe_ok || a->pipe != b->pipe, "pipeline");
    int tex_same = a->tex_ok == b->tex_ok && a->tmask == b->tmask && !memcmp(a->tex, b->tex, sizeof a->tex);
    for (u32 i = 0; tex_same && i < RSX_BE_MAX_TEXTURES; i++)
        if (((a->tmask >> i) & 1u) && memcmp(&a->samp[i], &b->samp[i], sizeof a->samp[i])) tex_same = 0;
    FDV_W(!tex_same, "textures");
    FDV_W(a->vtex_ok != b->vtex_ok || a->vmask != b->vmask || memcmp(a->vtex, b->vtex, sizeof a->vtex), "vertex-textures");
    FDV_W(a->vp_ok != b->vp_ok || memcmp(a->vp, b->vp, sizeof a->vp), "viewport");
    FDV_W(a->hud_ok != b->hud_ok || a->hud != b->hud, "hud");
    if (p->sc_send) FDV_W(!a->sc_ok || memcmp(a->sc, p->sc, sizeof p->sc), "scissor");
    else FDV_W(a->sc_ok != b->sc_ok || memcmp(a->sc, b->sc, sizeof a->sc), "scissor-not-sent");
    if (p->sref_send) FDV_W(!a->sref_ok || a->sref != p->sref, "stencil-ref");
    else FDV_W(a->sref_ok != b->sref_ok || a->sref != b->sref, "stencil-ref-not-sent");
    if (!p->bias_send) FDV_W(a->bias_ok != b->bias_ok || memcmp(a->bias, b->bias, sizeof a->bias), "depth-bias-not-sent");
    if (!p->clamp_send) FDV_W(a->clamp_ok != b->clamp_ok || a->clamp != b->clamp, "depth-clamp-not-sent");
    FDV_W(FDV_REAL->vs_constants_gen ? (s_fdv.n_gen != 1 || s_fdv.gen != p->gen) : s_fdv.n_gen != 0, "constant-generation");
    FDV_W(s_fdv.n_vsf || s_fdv.n_vsr != 1 || s_fdv.rows != (const void*)g.rsx.constants || s_fdv.first != g.vpc_lo ||
          s_fdv.count != g.vpc_n || s_fdv.tail_bytes != sizeof p->xf || memcmp(s_fdv.tail, p->xf, sizeof p->xf) ||
          s_fdv.total != ENG_VP_CB_BYTES, "vertex-constants");
    FDV_W(s_fdv.n_ps != 1 || s_fdv.ps_bytes != p->fp_bytes || s_fdv.ps_bytes > s_fdv.psc_cap ||
          memcmp(s_fdv.psc, s_fdv.ps, p->fp_bytes), "fragment-constants");
#undef FDV_W
    if (!wp) return;
    s_fdv.diffs++;
    static u32 n;
    if (n++ >= 64u && (n & 1023u)) return;
    char line[600];
    const int k = snprintf(line, sizeof line, "[fastdraw-verify] #%u frame %u draw %u: the tail sent what the fast path "
                           "would not have (%s)\n", n, g.frames, s_draw_serial, w);
    rsx_log_line(line, k, (int)sizeof line);
}

/* ---- the [fastdraw] line (with the [delta] line) ----------------------------- */
static void fd_report(void)
{
    if (s_fd_on <= 0) return;
    const double d = s_dlc.timed ? 100.0 / (double)s_dlc.timed : 0.0;
    const double s = s_fdn.state ? 100.0 / (double)s_fdn.state : 0.0;
    u64 bn = 0;
    for (u32 i = 0; i <= 6u; i++) bn += g_eng_fd_be[i];
    const double bd = bn ? 100.0 / (double)bn : 0.0;
    char line[1600];
    int k = snprintf(line, sizeof line, "[fastdraw] short tail %.1f%% of draws (%.1f%% of the all-state ones);"
                     " backend repeated the record for %.1f%% of them, else: no token %.1f%%, a bind since %.1f%%,"
                     " pass ended %.1f%%, sets/objects changed %.1f%%, other %.1f%%, arena full %.1f%%",
                     (double)s_fdn.fast * d, (double)s_fdn.fast * s, (double)g_eng_fd_be[0] * bd,
                     (double)g_eng_fd_be[1] * bd, (double)g_eng_fd_be[2] * bd, (double)g_eng_fd_be[3] * bd,
                     (double)g_eng_fd_be[4] * bd, (double)g_eng_fd_be[5] * bd, (double)g_eng_fd_be[6] * bd);
    { const double pn = (double)(s_fd2n.way0 + s_fd2n.wayn + s_fd2n.miss);   /* (fastdraw) round 2 */
      if (k > 0 && k < (int)sizeof line - 200)
          k += snprintf(line + k, sizeof line - (size_t)k, "; pair memo: way 0 %.1f%%, later ways %.1f%%, missed %.1f%%;"
                        " set memo of recent sets: %.1f%% of %llu",
                        pn > 0 ? 100.0 * s_fd2n.way0 / pn : 0.0, pn > 0 ? 100.0 * s_fd2n.wayn / pn : 0.0,
                        pn > 0 ? 100.0 * s_fd2n.miss / pn : 0.0,
                        g_eng_fd2[1] ? 100.0 * g_eng_fd2[0] / g_eng_fd2[1] : 0.0, (unsigned long long)g_eng_fd2[1]); }
    if (s_fd_verify && k > 0 && k < (int)sizeof line - 400)
        k += snprintf(line + k, sizeof line - (size_t)k, "; VERIFY: %llu draws checked, %llu differences;"
                      " backend %llu checked, %llu differences; pair ways %llu checked, %llu differences;"
                      " set memo %llu checked, %llu differences; VERIFY=2 runs of the production code: short tail %llu"
                      " (%llu skipped), %llu differences, fast record %llu, %llu differences (totals)",
                      (unsigned long long)s_fdv.checked, (unsigned long long)s_fdv.diffs,
                      (unsigned long long)g_eng_fd_be[8], (unsigned long long)g_eng_fd_be[7],
                      (unsigned long long)s_fd2n.vchk, (unsigned long long)s_fd2n.vdiff,
                      (unsigned long long)g_eng_fd2[2], (unsigned long long)g_eng_fd2[3],
                      (unsigned long long)s_fdv2.runs, (unsigned long long)s_fdv2.skipped, (unsigned long long)s_fdv2.diffs,
                      (unsigned long long)g_eng_fd2[4], (unsigned long long)g_eng_fd2[5]);
    if (k > 0 && k < (int)sizeof line - 1) { line[k++] = '\n'; line[k] = 0; }
    rsx_log_line(line, k, (int)sizeof line);
    memset(&s_fdn, 0, sizeof s_fdn);
    for (u32 i = 0; i <= 6u; i++) g_eng_fd_be[i] = 0;
    s_fd2n.way0 = s_fd2n.wayn = s_fd2n.miss = 0; g_eng_fd2[0] = g_eng_fd2[1] = 0;
}
