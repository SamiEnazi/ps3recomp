/*
 * (rnd1) PS3RECOMP_ENG_DELTA: the delta draw engine (phase 1 of the renderer
 * rewrite). Included by rsx_draw_engine.c right before sink_end.
 *
 * Every draw used to re-derive everything -- programs, render state, targets,
 * textures, pipeline -- through memo lookups, although consecutive draws of a
 * material-sorted frame mostly change only the mesh and the transform
 * constants. Now a draw pays only for what changed since the previous draw:
 *
 *  - The engine's dispatcher instance tracks the 64-byte register blocks it is
 *    written (rsx_dispatch.dirty, as rsx-par's walker instance does; with
 *    PS3RECOMP_RSX_SPLIT the exec thread marks each block it applies,
 *    rsx_split.c). At each draw the dirty blocks are compared with a snapshot
 *    of the registers as the previous draw saw them; a register whose value
 *    really changed marks its groups (s_dl_rg, built in dl_map_init from what
 *    each stage of sink_end reads). Writes of the same value mark nothing.
 *  - sink_end keeps each part's outputs (programs and layout, render state,
 *    targets / zeta / surface / viewport, textures and samplers, pipeline, and
 *    the vertex/index result of a cached draw) and takes them again when the
 *    part's groups are clean, its memory inputs provably unchanged and nothing
 *    in the engine moved under it (frame, clears, write-backs, presents, the
 *    target / depth / texture tables, live switches).
 *  - PS3RECOMP_ENG_DELTA_VERIFY=1: every part that could have been reused is
 *    computed the whole way too and compared field by field; a difference
 *    prints a [delta-verify] line naming the part, the fields and the
 *    registers that changed since the previous draw. The whole way's result is
 *    the one used, so verify mode renders exactly as the old path.
 *  - Counters every 5 s ([delta] line) with PS3RECOMP_LIMITS=1,
 *    PS3RECOMP_ENG_DRAWSTATS=1 or PS3RECOMP_ENG_DELTA_STATS=1: draws per frame,
 *    the share of draws per path, why draws went the whole way, and the time
 *    sink_end takes per draw on its thread (also with the switch off, for A/B).
 *
 * PS3RECOMP_ENG_DELTA=0: the old path exactly (no dirty tracking at all).
 * Off by itself while a diagnostic that logs from inside a skipped stage is on
 * (dl_init), or with PS3RECOMP_ENG_FP_KPATCH=1 (that memo refreshes program
 * entries in place).
 */

static int s_dl_on = -1;        /* 1 on (default), 0 off, -1 not initialised */
static int s_dl_verify;         /* PS3RECOMP_ENG_DELTA_VERIFY */
static int s_dl_stats;          /* the 5 s counters */

/* Register groups (what a changed register invalidates). */
enum {
    DLG_VTX  = 1u,    /* vertex arrays: formats, offsets, base offset/index, current values, divider */
    DLG_IDX  = 2u,    /* index array, restart index */
    DLG_TEX  = 4u,    /* fragment + vertex texture units, depth bounds */
    DLG_PROG = 8u,    /* fragment program address, transform program start (+ vp words: vp_gen) */
    DLG_RS   = 16u,   /* blend / alpha / depth / stencil / cull / masks */
    DLG_SURF = 32u,   /* surface format / offsets / pitch / clip / DMA contexts, viewport */
    DLG_PIPE = 64u,   /* pipeline-only inputs: shader control, shader packer, user clip planes */
    DLG_ALL  = 128u   /* catch-all: the whole way */
};
/* The parts a draw can take from the previous draw. */
enum { DL_PROG = 1u, DL_RS = 2u, DL_SURF = 4u, DL_TEX = 8u, DL_PIPE = 16u, DL_CONSTS = 32u };
#define DL_STATE (DL_PROG | DL_RS | DL_SURF | DL_TEX | DL_PIPE)
/* Why a draw went the whole way. */
enum { DLW_INVALID, DLW_FRAME, DLW_EVENT, DLW_KNOB, DLW_TABLES, DLW_SURF, DLW_CATCHALL, DLW_N };

#define DL_NREG  (0x2000u >> 2)           /* the 3D class's registers: methods 0..0x1FFC */
#define DL_NBLK  (DL_NREG / 16u)          /* their 64-byte blocks (the dirty bitmap's first 128) */
static u8  s_dl_rg[DL_NREG];             /* group bits per register (0: no stage of a draw reads it) */
static u8  s_dl_bg[DL_NBLK];             /* ... per block: the union */
static u8  s_dl_out[DL_NREG];           /* 1: outside every group on purpose (verify does not list it) */
static u32 s_dl_snap[DL_NREG];           /* the registers as the previous draw saw them */
static u64 s_dl_dirty[RSX_DSP_DIRTY_WORDS + 1];   /* g.rsx.dirty */

#define DL_KNOBS 20
typedef struct {                          /* a cached draw's vertex / index work (pair-memo hit) */
    int ok;
    u32 prim, n_idx, esz;
    eng_batch idx[4];
    const u8* runs[4];
    eng_ic_entry* ic; u64 ic_key, ic_content;
    u32 lo, span, n_src, n_draw;
    eng_vc_entry* pv; u64 psig;
} dl_dcs;
static struct {
    int valid;                            /* the outputs below are the last completed draw's */
    u32 acc;                              /* groups changed since that draw */
    u32 nregs, regs[24];                  /* ... the registers (verify messages) */
    /* what the reuse rests on, as that draw left it */
    u32 frame, ev, surf_gen, zgen, tex_tgen, tex_gen, misc_gen, vp_gen, icon_gen;
    int kn[DL_KNOBS], kn_now[DL_KNOBS];
    /* PROG */
    const eng_vp_memo* vp; const eng_fp_memo* fp;
    rsx_vertex_layout_plan layout;
    int vpc_full; u32 vpc_lo, vpc_n;
    /* RS */
    rsx_be_render_state rs;
    /* SURF */
    u32 targets[RSX_BE_MAX_COLOR_TARGETS], n_targets, zslot, tkq;
    u32 bumps[8], nbumps;
    int surf_ok;                          /* every lookup found or made its object */
    rsx_dsp_surface sf; rsx_dsp_viewport vpt;
    /* TEX */
    int tex_ok;
    u32 textures[RSX_BE_MAX_TEXTURES]; rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES]; u32 tex_mask;
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES]; rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES]; u32 vtex_mask;
    /* PIPE */
    u32 pipeline, fbf_applied; int pipe_fixed; const rsx_fp_constant_block* fp_constants;
    /* CONSTS */
    dl_dcs dcs;
    /* (fastdraw) what the dynamic state the tail sends last was made from:
     * scissor, stencil reference, polygon offset (registers outside every
     * group) and the depth bias / clamp switches (dl_end) */
    u32 dyn[6]; int dyn_poly, dyn_clamp;
    /* (fastdraw, DELTA_STATS=2) texture units whose registers changed since the
     * last completed draw: any field / image fields / sampler fields */
    u32 tunits, timg, tsmp;
} s_dl;

/* (fastdraw) With the counters on, sink_end's time is also split by the path a
 * draw took (whole way / part reused / all state reused / all state and the
 * vertex + index work reused): a [delta-cost] line per path with the [delta]
 * line. PS3RECOMP_ENG_DELTA_STATS=2 also splits each path's time by stage
 * (DL_MARK in sink_end_body and rsx_draw_fast.inc; the backend's draw by its
 * own marks, g_eng_bst in rsx_vulkan_backend.c) -- ~19 rdtsc marks a draw,
 * ~8 ns each, so only for a diagnosis, never in an fps A/B. */
static int s_dl_cost;                     /* PS3RECOMP_ENG_DELTA_STATS=2: the stage marks */
/* paths: the whole way; part reused split by what was not (textures with the
 * programs reused, programs, anything else); all state; all state + vertices */
enum { DLP_WHOLE, DLP_PTEX, DLP_PPROG, DLP_POTH, DLP_STATE, DLP_SAME, DLP_N };
#define DLP_MAX 8u                        /* g_eng_bst's rows (rsx_vulkan_backend.c declares them so) */
#define DLM_N 12u                         /* engine stages (dl_frame_report_ex names them) */
#define DLB_N 8u                          /* backend draw stages */
static u32 s_dl_path;                     /* this draw's path (dl_begin) */
static u64 s_dl_mt;                       /* the last mark */
u64 g_eng_bst_t;                          /* the backend's last mark (0: no engine draw call open) */
u32 g_eng_bst_on, g_eng_bst_path;
u64 g_eng_bst[DLP_MAX][DLB_N];
/* (fastdraw, DELTA_STATS=2) the backend's descriptor set by path: ticks in the
 * memo check / image infos / key + table probe / allocate + write, how the set
 * was found (memo, table, new), and the sampler searches (calls, entries
 * passed) -- rsx_vulkan_backend.c */
u64 g_eng_bset[DLP_MAX][4], g_eng_bsetn[DLP_MAX][3], g_eng_bsamp[DLP_MAX][2];

static struct {
    u64 draws, full, partial, state, consts, used[6], why[DLW_N], vdiff;
    u64 ticks, timed;                     /* rdtsc ticks in sink_end, draws timed */
    u64 ns0, tsc0; u32 frame0;
    u32 ca_regs[8], n_ca;                 /* catch-all: the first registers that fired it */
    u64 pt[DLP_N], pn[DLP_N], st[DLP_N][DLM_N], cal, caln;   /* (fastdraw) by path and stage; a mark's own cost */
    /* (fastdraw, DELTA_STATS=2) part:textures -- why the textures were not
     * reused (registers / generations / the last draw's were state-dependent),
     * how many units' registers changed (0 1 2 3 4+), whether a sampler field
     * (address, control0, filter, border) or only the image fields changed;
     * by path: vertices by pair memo / vertex-cache probe / fetched, texture
     * units bound through the memo / the probe, pipeline lookups / their
     * texture-mask cache hits / key memo hits */
    u64 ptw[3], ptu[5], pts, pti;
    u64 vx[DLP_N][3], ts[DLP_N][2], pk[DLP_N][3];
} s_dlc;

static inline u64 dl_tsc(void) { return __builtin_ia32_rdtsc(); }
static void fd_report(void);              /* (fastdraw) rsx_draw_fast.inc */
#define DL_MARK(k) do { if (__builtin_expect(s_dl_cost != 0, 0)) { const u64 _t = dl_tsc(); \
    s_dlc.st[s_dl_path][k] += _t - s_dl_mt; s_dl_mt = _t; } } while (0)
/* before the backend's draw call: its marks start here */
#define DL_MARK_DRAW0() do { if (__builtin_expect(s_dl_cost != 0, 0)) { DL_MARK(9); g_eng_bst_t = s_dl_mt; } } while (0)
/* after it: the backend's last part (from its last mark) */
#define DL_MARK_DRAW1() do { if (__builtin_expect(s_dl_cost != 0, 0)) { DL_MARK(10); \
    if (g_eng_bst_t) { g_eng_bst[s_dl_path][DLB_N - 1u] += s_dl_mt - g_eng_bst_t; g_eng_bst_t = 0; } } } while (0)

/* ---- the group map ------------------------------------------------------
 * Built from what sink_end's stages read (rsx_dsp_reg / the rsx_dsp_get_*
 * getters in this file, rsx_dispatch.c and rsx_vertex_compact.c):
 *   PROG  eng_vertex_layout -> eng_guest_programs: eng_vp_current (0x1EA0 and
 *         the vp words, vp_gen), eng_fp_current (0x08E4 + the program bytes).
 *   RS    rsx_draw_engine_decode_render_state: 0x0304..0x0364, 0x036C, 0x0370,
 *         0x0A6C..0x0A74, 0x1830, 0x1834, 0x183C, 0x1D78 (CLAMP_CULL_FLIP),
 *         0x0208 (surface format: alpha_ref_format, rt_fp16).
 *   SURF  eng_current_target_set / rsx_dsp_get_surface: 0x0180..0x01BC (DMA
 *         contexts), 0x0200..0x022C, 0x0280..0x028C; rsx_dsp_get_viewport:
 *         0x0A00..0x0A3C; eng_zdepth_get (zeta from the surface registers).
 *   TEX   sink_bind_textures / eng_draw_textures / rsx_dsp_get_texture:
 *         0x1A00..0x1BFC, 0x1840..0x187C (also g_rsx_dsp_tex_gen);
 *         sink_bind_vertex_textures / eng_vtex_mask: 0x0900..0x097C (also
 *         g_rsx_dsp_misc_gen); eng_depth_bounds_prepare: 0x0380..0x0388.
 *   PIPE  eng_pipeline_get beyond the above: 0x1D60 shader control, 0x1FEC
 *         shader packer, 0x1478 user clip planes (eng_user_clip_key).
 *   VTX   dc_fetch / the fetch plan (rsx_vertex_compact.c): 0x1680..0x16BC,
 *         0x1738, 0x173C, 0x1740..0x177C, 0x1C00..0x1CFC, 0x1FC0.
 *   IDX   dc_fetch / eng_ic_probe: 0x181C, 0x1820, 0x173C, 0x1DAC, 0x1DB0.
 * Read only by code every draw runs anyway (so no reuse depends on them):
 * 0x0334 stencil ref, 0x08C0/0x08C4 scissor, 0x0A68/0x0A78/0x0A7C polygon
 * offset, 0x1D78 depth clamp, 0x1D88 shader window (the WPOS constants); by
 * sink_clear only: 0x1D8C, 0x1D90; by diagnostics only: 0x031C and the
 * REGDUMP of everything (those run every draw too). Every other register of
 * the class -- execution methods (0x1808, 0x1814, 0x1818, 0x1824, 0x1D94),
 * the program / constant upload windows and their pointers, the immediate
 * vertex words, semaphores and reports -- no stage of a draw reads: 0. The
 * catch-all would take a register a stage reads that fits no group; by the
 * list above there is none, so it only fires on a forced whole-way draw. */
static void dl_map(u32 m0, u32 m1, u8 grp) { for (u32 m = m0; m <= m1; m += 4u) s_dl_rg[m >> 2] |= grp; }
static void dl_map_init(void)
{
    memset(s_dl_rg, 0, sizeof s_dl_rg);
    dl_map(0x0180, 0x01BC, DLG_SURF);     /* DMA contexts: colour A..D, zeta */
    dl_map(0x0200, 0x022C, DLG_SURF);     /* clip, format, pitch A/Z, offsets A/B/Z, colour target */
    dl_map(0x0208, 0x0208, DLG_RS);       /* the surface format is render state too */
    dl_map(0x0280, 0x028C, DLG_SURF);     /* pitch / offset C, D */
    dl_map(0x0304, 0x0318, DLG_RS);       /* alpha test, blend enable / factors */
    dl_map(0x0320, 0x0330, DLG_RS);       /* blend equation, colour mask, stencil */
    dl_map(0x0338, 0x0364, DLG_RS);       /* stencil, two-sided, back stencil */
    dl_map(0x036C, 0x0370, DLG_RS);       /* MRT blend / colour mask */
    dl_map(0x0380, 0x0388, DLG_TEX);      /* depth bounds: a texture unit + a program variant */
    dl_map(0x08E4, 0x08E4, DLG_PROG);     /* SET_SHADER_PROGRAM */
    dl_map(0x0900, 0x097C, DLG_TEX);      /* vertex texture units */
    dl_map(0x0A00, 0x0A3C, DLG_SURF);     /* viewport */
    dl_map(0x0A6C, 0x0A74, DLG_RS);       /* depth func / write / test */
    dl_map(0x1478, 0x1478, DLG_PIPE);     /* user clip planes */
    dl_map(0x1680, 0x16BC, DLG_VTX);      /* vertex array offsets */
    dl_map(0x1738, 0x1738, DLG_VTX);      /* vertex data base offset */
    dl_map(0x173C, 0x173C, DLG_VTX | DLG_IDX);   /* base index */
    dl_map(0x1740, 0x177C, DLG_VTX);      /* vertex array formats */
    dl_map(0x181C, 0x1820, DLG_IDX);      /* index array address / format */
    dl_map(0x1830, 0x1834, DLG_RS);       /* cull face, front face */
    dl_map(0x183C, 0x183C, DLG_RS);       /* cull enable */
    dl_map(0x1840, 0x187C, DLG_TEX);      /* texture control 3 (pitch, depth) */
    dl_map(0x1A00, 0x1BFC, DLG_TEX);      /* texture units */
    dl_map(0x1C00, 0x1CFC, DLG_VTX);      /* current vertex attribute values */
    dl_map(0x1D60, 0x1D60, DLG_PIPE);     /* shader control */
    dl_map(0x1D78, 0x1D78, DLG_RS);       /* ZMIN_MAX_CONTROL (PS3RECOMP_ENG_CLAMP_CULL_FLIP) */
    dl_map(0x1DAC, 0x1DB0, DLG_IDX);      /* restart index enable / value */
    dl_map(0x1EA0, 0x1EA0, DLG_PROG);     /* transform program start */
    dl_map(0x1FC0, 0x1FC0, DLG_VTX);      /* frequency divider operation */
    dl_map(0x1FEC, 0x1FEC, DLG_PIPE);     /* shader packer (sRGB output) */
    /* outside every group on purpose: execution methods, the upload windows
     * and their pointers, immediate vertex words, semaphores, clear values, and
     * the registers only code every draw runs reads (dl_map_init's comment) */
    static const u32 out[][2] = {
        { 0x0100, 0x0110 }, { 0x0334, 0x0334 }, { 0x031C, 0x031C }, { 0x08C0, 0x08C4 }, { 0x0A68, 0x0A68 },
        { 0x0A78, 0x0A7C }, { 0x0A80, 0x0AFC }, { 0x0B80, 0x0BFC }, { 0x1500, 0x15BC }, { 0x1714, 0x1714 },
        { 0x1808, 0x1808 }, { 0x1814, 0x1818 }, { 0x1824, 0x1824 }, { 0x1880, 0x19FC }, { 0x1D6C, 0x1D74 },
        { 0x1D88, 0x1D94 }, { 0x1E40, 0x1E7C }, { 0x1E9C, 0x1E9C }, { 0x1EFC, 0x1F7C } };
    memset(s_dl_out, 0, sizeof s_dl_out);
    for (u32 i = 0; i < sizeof out / sizeof out[0]; i++)
        for (u32 m = out[i][0]; m <= out[i][1]; m += 4u) s_dl_out[m >> 2] = 1;
    for (u32 b = 0; b < DL_NBLK; b++) {
        u8 u = 0;
        for (u32 i = 0; i < 16u; i++) u |= s_dl_rg[b * 16u + i];
        s_dl_bg[b] = u;
    }
}

static void dl_init(void)
{
    const char* e = getenv("PS3RECOMP_ENG_DELTA");
    s_dl_on = !(e && *e == '0');   /* (m16) default ON: verify run nt-DV-city clean (0 lines), +2..3% in the nt-* A/B; 0 = the old path */
    { const char* l = getenv("PS3RECOMP_LIMITS");
      s_dl_stats = (l && *l && *l != '0') || getenv("PS3RECOMP_ENG_DRAWSTATS") || getenv("PS3RECOMP_ENG_DELTA_STATS");
      const char* c = getenv("PS3RECOMP_ENG_DELTA_STATS"); s_dl_cost = c && atoi(c) >= 2; }
    /* diagnostics that log from inside a stage a reused draw skips */
    static const char* const diag[] = { "PS3RECOMP_ENG_TEXLOG", "PS3RECOMP_ENG_CSNAP_LOG", "PS3RECOMP_ENG_RES_LOG",
                                        "PS3RECOMP_ENG_TEXLOG_RT", "PS3RECOMP_ENG_ZTEX_LOG", "PS3RECOMP_ENG_SIGNLOG",
                                        "PS3RECOMP_ENG_DBOUNDS_LOG" };
    const char* why = NULL;
    for (u32 i = 0; s_dl_on && i < sizeof diag / sizeof diag[0]; i++) if (getenv(diag[i])) { s_dl_on = 0; why = diag[i]; }
    { const char* k = getenv("PS3RECOMP_ENG_FP_KPATCH"); if (s_dl_on && k && *k == '1') { s_dl_on = 0; why = "PS3RECOMP_ENG_FP_KPATCH=1"; } }
    { const char* v = getenv("PS3RECOMP_ENG_DELTA_VERIFY"); s_dl_verify = s_dl_on && v && *v && *v != '0'; }
    memset(&s_dl, 0, sizeof s_dl);
    memset(&s_dlc, 0, sizeof s_dlc);
    memset(s_dl_dirty, 0, sizeof s_dl_dirty);
    memset(g_eng_bst, 0, sizeof g_eng_bst);
    g_eng_bst_on = s_dl_cost ? 1u : 0u; g_eng_bst_t = 0;
    dl_map_init();
    if (s_dl_on) {
        memcpy(s_dl_snap, g.rsx.regs, sizeof s_dl_snap);
        g.rsx.dirty = s_dl_dirty;
    } else g.rsx.dirty = NULL;
    fprintf(stderr, "[rsx engine] delta draw engine: %s%s%s%s (PS3RECOMP_ENG_DELTA=0 = every draw the whole way)\n",
            s_dl_on ? "on" : "off", why ? " -- " : "", why ? why : "", s_dl_verify ? ", VERIFY (every reused part also computed and compared)" : "");
}

/* The dirty blocks against the snapshot: the groups whose registers changed. */
static u32 dl_scan(void)
{
    u64 sum = s_dl_dirty[RSX_DSP_DIRTY_WORDS];
    if (!sum) return 0;
    s_dl_dirty[RSX_DSP_DIRTY_WORDS] = 0;
    u32 grp = 0;
    const u32* const cur = g.rsx.regs;
    while (sum) {
        const u32 w = (u32)__builtin_ctzll(sum);
        sum &= sum - 1u;
        u64 m = s_dl_dirty[w];
        s_dl_dirty[w] = 0;
        if (w >= DL_NBLK / 64u) continue;   /* past the class: the high registers, vp words, constants */
        while (m) {
            const u32 b = w * 64u + (u32)__builtin_ctzll(m);
            m &= m - 1u;
            if (!s_dl_bg[b] && !s_dl_verify) continue;   /* verify: every change, to name it */
            const u32* c = cur + (size_t)b * 16u;
            u32* sn = s_dl_snap + (size_t)b * 16u;
            if (!memcmp(c, sn, 64)) continue;
            for (u32 i = 0; i < 16u; i++) {
                if (c[i] == sn[i]) continue;
                const u8 rg = s_dl_rg[b * 16u + i];
                /* bit 31: in no group (a register the map says no stage of a draw reads) */
                if (s_dl.nregs < 24u && (rg || (s_dl_verify && !s_dl_out[b * 16u + i])))
                    s_dl.regs[s_dl.nregs++] = ((b * 16u + i) * 4u) | (rg ? 0u : 0x80000000u);
                if (rg) {
                    grp |= rg;
                    if (__builtin_expect(s_dl_cost != 0, 0) && (rg & DLG_TEX)) {   /* (fastdraw) which unit, which field */
                        const u32 reg = (b * 16u + i) * 4u;
                        if (reg >= 0x1A00u && reg < 0x1C00u) {
                            const u32 u = (reg - 0x1A00u) >> 5, f = reg & 0x1Fu;
                            s_dl.tunits |= 1u << u;
                            if (f == 0x08u || f == 0x0Cu || f == 0x14u || f == 0x1Cu) s_dl.tsmp |= 1u << u;
                            else s_dl.timg |= 1u << u;
                        } else if (reg >= 0x1840u && reg < 0x1880u) {
                            s_dl.tunits |= 1u << ((reg - 0x1840u) >> 2); s_dl.timg |= 1u << ((reg - 0x1840u) >> 2);
                        }
                    }
                    if ((rg & DLG_ALL) && s_dlc.n_ca < 8u) s_dlc.ca_regs[s_dlc.n_ca++] = (b * 16u + i) * 4u;
                }
                sn[i] = c[i];
            }
        }
    }
    return grp;
}

/* The live switches the reused parts read (a change: the whole way). */
static void dl_knobs(int* k)
{
#define DL_LD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
    k[0] = DL_LD(s_clamp_cull_flip);  k[1] = DL_LD(s_fxres_q);     k[2] = (int)DL_LD(s_fxres_nblock);
    k[3] = DL_LD(s_fbf_mode);         k[4] = DL_LD(s_texreg_once); k[5] = DL_LD(s_dbounds_on);
    k[6] = DL_LD(s_shadow_epoch);     k[7] = DL_LD(s_shade_backface);
    k[8] = DL_LD(s_fx_dof);           k[9] = DL_LD(s_fx_edge);     k[10] = DL_LD(s_pidx_on);
    k[11] = DL_LD(s_vpgen_on);        k[12] = (int)(u8)s_mods.dump_dir[0];
    k[13] = DL_LD(s_vc_want);         k[14] = DL_LD(s_ic_want);    k[15] = DL_LD(s_bmerge_on);
    k[16] = (int)__atomic_load_n(&g_gcm_xlate_gen, __ATOMIC_ACQUIRE);
    k[17] = s_guest_reader != NULL;   k[18] = rsx_wt_mode();       k[19] = DL_LD(s_tmemo_on);
#undef DL_LD
}

/* The programs the lookups would find are the last draw's: the registers and
 * the transform program are as they were (checked by the caller and vp_gen),
 * and the fragment program's guest bytes are still the memo's copy (the full
 * lookup compares the same bytes; an address keeps one entry per content). */
static int dl_prog_same(void)
{
    if (g.rsx.vp_gen != s_dl.vp_gen) return 0;
    if (!s_dl.vp) return 1;               /* no transform program: the registers and vp words decide */
    const eng_fp_memo* fp = s_dl.fp;
    if (!fp) return rsx_dsp_reg(&g.rsx, 0x08E4) == 0;   /* unreadable then may be readable now */
    const u8* uc = eng_guest_ptr(NULL, fp->loc, fp->off, fp->size);
    return uc && !memcmp(fp->bytes, uc, fp->size);
}

/* The last draw's vertex and index work holds for this draw: same packets,
 * primitive and registers, the index runs and the vertex spans still unwritten
 * (rsx_wtrack, as the index cache and the pair memo themselves ask) -- the same
 * checks dc_fetch's pair-memo hit makes, without its key, table probe and maps. */
static int dl_consts_ok(u32 prim)
{
    const dl_dcs* d = &s_dl.dcs;
    if (!d->ok || d->prim != prim || dc.n_idx != d->n_idx || dc.n_arr || dc.inl || !dc.fetch_ok) return 0;
    if (memcmp(dc.idx, d->idx, (size_t)d->n_idx * sizeof dc.idx[0])) return 0;
    if (!s_dl.layout.stride || !eng_pair_on() || !eng_ic_on() || !eng_vc_on()) return 0;
    const eng_ic_entry* e = d->ic;
    if (e->key != d->ic_key || !e->ibuf || !e->wt_ok || e->unhashed || !e->seen || e->skip_until > g.frames ||
        e->content != e->wt_content || e->content != d->ic_content || e->lo != d->lo || e->span != d->span ||
        e->n_src != d->n_src || e->n_draw != d->n_draw)
        return 0;
    u64 st = 0;
    for (u32 b = 0; b < d->n_idx; b++)
        if (!rsx_wt_query(d->runs[b], (size_t)dc.idx[b].count * d->esz, &st)) return 0;
    if (st != e->wt_stamp) return 0;
    return eng_pair_try(e, d->psig, &s_dl.layout) == d->pv;
}

static void dl_count(u32 use, int why)
{
    s_dlc.draws++;
    if (why >= 0) { s_dlc.why[why]++; s_dlc.full++; return; }
    if ((use & DL_STATE) == DL_STATE) s_dlc.state++; else s_dlc.partial++;
    if (use & DL_CONSTS) s_dlc.consts++;
    for (u32 i = 0; i < 6u; i++) if (use & (1u << i)) s_dlc.used[i]++;
}

/* At the start of a draw: the parts it may take from the previous draw. In
 * verify mode none is taken (*chk says which to compare instead). */
static u32 dl_begin(u32 prim, u32* chk)
{
    s_dl.acc |= dl_scan();
    dl_knobs(s_dl.kn_now);
    const int was = s_dl.valid;
    s_dl.valid = 0;                       /* until this draw completes */
    int why = -1;
    u32 use = 0;
    if (!was) why = DLW_INVALID;
    else if (s_dl.frame != g.frames) why = DLW_FRAME;
    else if (s_dl.ev != s_dl_ev) why = DLW_EVENT;
    else if (memcmp(s_dl.kn, s_dl.kn_now, sizeof s_dl.kn)) why = DLW_KNOB;
    else if (s_dl.surf_gen != s_surf_gen || s_dl.zgen != s_dl_zgen) why = DLW_TABLES;
    else if (s_dl.acc & DLG_ALL) why = DLW_CATCHALL;
    else if ((s_dl.acc & DLG_SURF) || !s_dl.surf_ok) why = DLW_SURF;
    if (why < 0) {
        use = DL_SURF;
        if (!(s_dl.acc & DLG_RS)) use |= DL_RS;
        if (!(s_dl.acc & DLG_PROG) && dl_prog_same()) use |= DL_PROG;
        if ((use & DL_PROG) && s_dl.tex_ok && !(s_dl.acc & DLG_TEX) &&
            s_dl.tex_gen == g_rsx_dsp_tex_gen && s_dl.misc_gen == g_rsx_dsp_misc_gen &&
            s_dl.tex_tgen == s_tex_tgen && s_dl.icon_gen == s_icon_gen)
            use |= DL_TEX;
        if ((use & (DL_PROG | DL_RS | DL_TEX)) == (DL_PROG | DL_RS | DL_TEX) && !(s_dl.acc & DLG_PIPE))
            use |= DL_PIPE;
        if ((use & DL_PIPE) && !(s_dl.acc & (DLG_VTX | DLG_IDX)) && dl_consts_ok(prim))
            use |= DL_CONSTS;
    }
    if (s_dl_stats) {
        dl_count(use, why);
        s_dl_path = why >= 0 ? DLP_WHOLE
                  : (use & DL_STATE) == DL_STATE ? ((use & DL_CONSTS) ? DLP_SAME : DLP_STATE)
                  : !(use & DL_PROG) ? DLP_PPROG : !(use & DL_TEX) ? DLP_PTEX : DLP_POTH;
        if (s_dl_cost && s_dl_path == DLP_PTEX) {   /* (fastdraw) why, and how much changed */
            s_dlc.ptw[(s_dl.acc & DLG_TEX) ? 0 : !s_dl.tex_ok ? 2 : 1]++;
            const u32 nu = (u32)__builtin_popcount(s_dl.tunits);
            s_dlc.ptu[nu < 4u ? nu : 4u]++;
            if (s_dl.tsmp) s_dlc.pts++; else if (s_dl.timg) s_dlc.pti++;
        }
        g_eng_bst_path = s_dl_path;
    }
    if (s_dl_verify) { *chk = use; return 0; }
    *chk = 0;
    return use;
}

/* A completed draw: its outputs (stored by each part as it ran) are the next
 * draw's to take. */
static void dl_end(void)
{
    s_dl.valid = 1; s_dl.acc = 0; s_dl.nregs = 0; s_dl.tunits = s_dl.timg = s_dl.tsmp = 0;
    s_dl.frame = g.frames; s_dl.ev = s_dl_ev;
    s_dl.surf_gen = s_surf_gen; s_dl.zgen = s_dl_zgen;
    s_dl.tex_tgen = s_tex_tgen; s_dl.tex_gen = g_rsx_dsp_tex_gen; s_dl.misc_gen = g_rsx_dsp_misc_gen;
    s_dl.vp_gen = g.rsx.vp_gen; s_dl.icon_gen = s_icon_gen;
    memcpy(s_dl.kn, s_dl.kn_now, sizeof s_dl.kn);
    /* (fastdraw) nothing writes a register within a draw: these are the
     * values its scissor, stencil reference and depth bias were made from */
    s_dl.dyn[0] = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL); s_dl.dyn[1] = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
    s_dl.dyn[2] = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF);   s_dl.dyn[3] = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL);
    s_dl.dyn[4] = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_SCALE);  s_dl.dyn[5] = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
    s_dl.dyn_poly = __atomic_load_n(&s_poly_offset, __ATOMIC_RELAXED);
    s_dl.dyn_clamp = __atomic_load_n(&s_depth_clamp, __ATOMIC_RELAXED);
}

/* ---- verify ---------------------------------------------------------------- */
static void dl_vfail(const char* part, const char* what)
{
    s_dlc.vdiff++;
    static u32 n;
    if (n++ >= 64u && (n & 1023u)) return;
    char regs[200]; int p = 0; regs[0] = 0;
    for (u32 i = 0; i < s_dl.nregs && p < (int)sizeof regs - 8; i++)
        p += snprintf(regs + p, sizeof regs - (size_t)p, " %04X%s", s_dl.regs[i] & 0xFFFFu, (s_dl.regs[i] >> 31) ? "?" : "");
    char line[640];
    const int k = snprintf(line, sizeof line, "[delta-verify] #%u frame %u draw %u: %s differs (%s); changed since the "
                           "previous draw: groups 0x%02X, registers (? = in no group)%s%s\n", n, g.frames, g.guest_draws, part, what,
                           s_dl.acc, p ? regs : " none", s_dl.nregs >= 24u ? " ..." : "");
    rsx_log_line(line, k, (int)sizeof line);
}
#define DL_W(cond, name) do { if (cond) wp += snprintf(w + wp, sizeof w - (size_t)wp, "%s%s", wp ? " " : "", name); } while (0)

static void dl_prog_restore(rsx_vertex_layout_plan* layout)
{
    eng_guest_programs_carry(s_dl.vp, s_dl.fp);
    *layout = s_dl.layout;
    g.vpc_full = s_dl.vpc_full; g.vpc_lo = s_dl.vpc_lo; g.vpc_n = s_dl.vpc_n;
}
static void dl_prog_note(const rsx_vertex_layout_plan* layout, int check)
{
    if (check) {
        char w[160]; int wp = 0; w[0] = 0;
        DL_W(s_gp_vp != s_dl.vp, "vp-memo");
        DL_W(s_gp_fp != s_dl.fp, "fp-memo");
        DL_W(layout->mask != s_dl.layout.mask || layout->stride != s_dl.layout.stride ||
             layout->count != s_dl.layout.count || memcmp(layout->attrs, s_dl.layout.attrs, sizeof layout->attrs) ||
             memcmp(layout->slot_by_attr, s_dl.layout.slot_by_attr, sizeof layout->slot_by_attr), "layout");
        DL_W(g.vpc_full != s_dl.vpc_full || g.vpc_lo != s_dl.vpc_lo || g.vpc_n != s_dl.vpc_n, "vp-constant-range");
        if (wp) dl_vfail("programs", w);
    }
    s_dl.vp = s_gp_vp; s_dl.fp = s_gp_fp;
    s_dl.layout = *layout;
    s_dl.vpc_full = g.vpc_full; s_dl.vpc_lo = g.vpc_lo; s_dl.vpc_n = g.vpc_n;
}

static void dl_rs_note(const rsx_be_render_state* rs, int check)
{
    if (check && memcmp(rs, &s_dl.rs, sizeof *rs)) dl_vfail("render state", "rs");
    s_dl.rs = *rs;
}

static u32 dl_surf_take(u32 targets[RSX_BE_MAX_COLOR_TARGETS])
{
    memcpy(targets, s_dl.targets, sizeof s_dl.targets);
    /* the lookups' own effect: each target slot they touched is the newest bound */
    for (u32 i = 0; i < s_dl.nbumps; i++) g.surfaces[s_dl.bumps[i]].stamp = ++s_surf_clock;
    return s_dl.n_targets;
}
static void dl_surf_note(const u32* targets, u32 n_targets, const rsx_dsp_surface* sf, const rsx_dsp_viewport* vp,
                         u32 tkq, u32 zslot, int check)
{
    if (check) {
        char w[160]; int wp = 0; w[0] = 0;
        DL_W(n_targets != s_dl.n_targets || memcmp(targets, s_dl.targets, n_targets * sizeof *targets), "targets");
        DL_W(s_dl_nbumps != s_dl.nbumps || memcmp(s_dl_bumps, s_dl.bumps, (s_dl_nbumps < 8u ? s_dl_nbumps : 8u) * 4u), "target-stamps");
        DL_W(memcmp(sf, &s_dl.sf, sizeof *sf), "surface");
        DL_W(memcmp(vp, &s_dl.vpt, sizeof *vp), "viewport");
        DL_W(tkq != s_dl.tkq, "fx-res-scale");
        DL_W(zslot != s_dl.zslot, "zeta");
        if (wp) dl_vfail("targets", w);
    }
    memset(s_dl.targets, 0, sizeof s_dl.targets);
    memcpy(s_dl.targets, targets, n_targets * sizeof *targets);
    s_dl.n_targets = n_targets;
    s_dl.nbumps = s_dl_nbumps;
    memcpy(s_dl.bumps, s_dl_bumps, sizeof s_dl.bumps);
    s_dl.sf = *sf; s_dl.vpt = *vp; s_dl.tkq = tkq; s_dl.zslot = zslot;
    /* a failed creation is tried again by the next lookup, and a replay holds 8 */
    s_dl.surf_ok = !s_dl_surf_fail && zslot != ENG_INVALID && s_dl_nbumps <= 8u;
}

static void dl_tex_note(const u32* textures, const rsx_be_sampler_desc* samplers, u32 tex_mask,
                        const u32* vtextures, const rsx_be_sampler_desc* vsamplers, u32 vtex_mask,
                        int vtex_all, int check)
{
    if (check) {
        char w[160]; int wp = 0; w[0] = 0;
        DL_W(tex_mask != s_dl.tex_mask, "mask");
        DL_W(memcmp(textures, s_dl.textures, sizeof s_dl.textures), "handles");
        DL_W(memcmp(samplers, s_dl.samplers, sizeof s_dl.samplers), "samplers");
        DL_W(vtex_mask != s_dl.vtex_mask || memcmp(vtextures, s_dl.vtextures, sizeof s_dl.vtextures) ||
             memcmp(vsamplers, s_dl.vsamplers, sizeof s_dl.vsamplers), "vertex-textures");
        DL_W(s_tex_volatile, "now-state-dependent");
        DL_W(s_dbz.unit != 0, "depth-bounds");
        DL_W(s_fbf.cand != 0, "framebuffer-fetch");
        if (wp) dl_vfail("textures", w);
    }
    memcpy(s_dl.textures, textures, sizeof s_dl.textures);
    memcpy(s_dl.samplers, samplers, sizeof s_dl.samplers);
    s_dl.tex_mask = tex_mask;
    memcpy(s_dl.vtextures, vtextures, sizeof s_dl.vtextures);
    memcpy(s_dl.vsamplers, vsamplers, sizeof s_dl.vsamplers);
    s_dl.vtex_mask = vtex_mask;
    s_dl.tex_ok = !s_tex_volatile && !s_dbz.unit && !s_fbf.cand && !s_mods.dump_dir[0] && vtex_all;
}
static void dl_tex_take(u32* textures, rsx_be_sampler_desc* samplers, u32* tex_mask)
{
    memcpy(textures, s_dl.textures, sizeof s_dl.textures);
    memcpy(samplers, s_dl.samplers, sizeof s_dl.samplers);
    *tex_mask = s_dl.tex_mask;
    /* what sink_bind_textures and eng_depth_bounds_prepare leave for a draw
     * whose units all came from the plain paths (dl_tex_note's tex_ok) */
    s_dbz.unit = 0; s_fbf.cand = 0; s_fbf.applied = 0; s_tex_volatile = 0;
}

static void dl_pipe_note(u32 pipeline, int fixed, int check)
{
    if (check) {
        char w[160]; int wp = 0; w[0] = 0;
        DL_W(pipeline != s_dl.pipeline, "handle");
        DL_W(fixed != s_dl.pipe_fixed, "fixed");
        DL_W(s_fbf.applied != s_dl.fbf_applied, "framebuffer-fetch");
        DL_W(g.fp_constants != s_dl.fp_constants, "fp-constants");
        if (wp) dl_vfail("pipeline", w);
    }
    s_dl.pipeline = pipeline; s_dl.pipe_fixed = fixed;
    s_dl.fbf_applied = s_fbf.applied; s_dl.fp_constants = g.fp_constants;
}

/* The pair-memo hit's outputs (dc_fetch), taken again. */
static void dl_consts_take(void)
{
    const dl_dcs* d = &s_dl.dcs;
    eng_ic_entry* e = d->ic;
    eng_vc_entry* pv = d->pv;
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    s_ic_cur = e; s_ic_hit = 1; s_ic_ranged = 0; s_ic_cur_content = e->wt_content;
    ENG_CACHE_PREDICT(s_ic, s_ic_prev, e);
    ds_ic_wt_clean++;
    dc.n_refs = e->span; dc.n_source_refs = e->n_src; dc.refs_remapped = 1; dc.n_cuts = 0;
    dc.layout = s_dl.layout;
    dc.consecutive = 1;
    ENG_CACHE_PREDICT(s_vc, s_vc_prev, pv);
    dc.refs[0].vertex_id = e->lo; dc.refs[0].base_index = base_index;
    dc.refs_lazy = e->span > 1u; dc.lazy_base = base_index;
    dc.plan_stale = 1;
    pv->last_frame = g.frames;
    dc.vbuf = pv->vbuf; dc.vbuf_hit = 1; dc.n_verts = dc.n_refs;
    e->last_frame = g.frames;
    s_dl_pv = pv; s_dl_psig = d->psig; s_icp_n = d->n_idx;
    ds_ic_hit++; ds_vc_hit++; ds_pair_hit++;
    if (eng_ds_enabled()) { ds.refs += dc.n_source_refs; ds.uniq += dc.n_refs; }
}
/* Verify: what dl_consts_take would have given, against the whole way's. */
typedef struct { u32 n_refs, n_src, vid, vbase, lazy, vbuf; const eng_ic_entry* ic; u64 content; } dl_cpred;
static void dl_consts_predict(dl_cpred* p)
{
    const dl_dcs* d = &s_dl.dcs;
    p->ic = d->ic; p->content = d->ic->wt_content;
    p->n_refs = d->ic->span; p->n_src = d->ic->n_src; p->vid = d->ic->lo;
    p->vbase = rsx_dsp_vertex_data_base_index(&g.rsx); p->lazy = d->ic->span > 1u; p->vbuf = d->pv->vbuf;
}
static void dl_consts_check(const dl_cpred* p)
{
    char w[200]; int wp = 0; w[0] = 0;
    DL_W(!s_ic_hit || s_ic_cur != p->ic || s_ic_cur_content != p->content, "index-cache-entry");
    DL_W(dc.n_refs != p->n_refs || dc.n_source_refs != p->n_src || !dc.refs_remapped || dc.n_cuts, "references");
    DL_W(!dc.n_refs || dc.refs[0].vertex_id != p->vid || dc.refs[0].base_index != p->vbase ||
         (u32)dc.refs_lazy != p->lazy || dc.lazy_base != p->vbase || !dc.consecutive || !dc.plan_stale, "reference-run");
    DL_W(!dc.vbuf_hit || dc.vbuf != p->vbuf || dc.n_verts != dc.n_refs, "vertex-buffer");
    DL_W(dc.layout.mask != s_dl.layout.mask || dc.layout.stride != s_dl.layout.stride, "layout");
    if (wp) dl_vfail("vertices/indices", w);
}
/* After this draw's dc_fetch: its result, when the next draw may take it. */
static void dl_consts_note(u32 prim)
{
    dl_dcs* d = &s_dl.dcs;
    d->ok = 0;
    if (!s_ic_hit || !dc.vbuf_hit || !dc.plan_stale || !s_dl_pv || !s_ic_cur || dc.n_arr || dc.inl ||
        !dc.n_idx || dc.n_idx > 4u || s_icp_n != dc.n_idx)
        return;
    const eng_ic_entry* e = s_ic_cur;
    d->prim = prim; d->n_idx = dc.n_idx;
    memcpy(d->idx, dc.idx, (size_t)dc.n_idx * sizeof dc.idx[0]);
    memcpy(d->runs, s_icp_runs, (size_t)dc.n_idx * sizeof s_icp_runs[0]);
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(&g.rsx, &ia);
    d->esz = ia.is_u32 ? 4u : 2u;
    d->ic = s_ic_cur; d->ic_key = e->key; d->ic_content = s_ic_cur_content;
    d->lo = e->lo; d->span = e->span; d->n_src = e->n_src; d->n_draw = e->n_draw;
    d->pv = s_dl_pv; d->psig = s_dl_psig;
    d->ok = 1;
}
#undef DL_W

/* ---- counters (eng_present: once a frame) ------------------------------------ */
/* final: the window so far, whatever its length (engine shutdown). */
static void dl_frame_report_ex(int final)
{
    if (s_dl_on < 0 || !s_dl_stats) return;
    const u64 now = eng_ds_now(), tsc = dl_tsc();
    if (!s_dlc.ns0) {   /* the first present opens the first window */
        if (final) return;
        memset(&s_dlc, 0, sizeof s_dlc);
        memset(g_eng_bst, 0, sizeof g_eng_bst);
        s_dlc.ns0 = now; s_dlc.tsc0 = tsc; s_dlc.frame0 = g.frames;
        return;
    }
    static u64 period;   /* PS3RECOMP_LIMITS_S=<s> (as the [limits] line), default 5 */
    if (!period) { const char* e = getenv("PS3RECOMP_LIMITS_S"); const double v = e ? atof(e) : 0.0;
                   period = v >= 1.0 ? (u64)(v * 1e9) : 5000000000ull; }
    if (!final && now - s_dlc.ns0 < period) return;
    if (final && !s_dlc.timed) return;
    const double tpns = now > s_dlc.ns0 ? (double)(tsc - s_dlc.tsc0) / (double)(now - s_dlc.ns0) : 1.0;
    const double us = s_dlc.timed && tpns > 0.0 ? (double)s_dlc.ticks / tpns / 1000.0 / (double)s_dlc.timed : 0.0;
    const u32 frames = g.frames - s_dlc.frame0;
    const double sec = (double)(now - s_dlc.ns0) / 1e9;
    char line[900];
    int k;
    if (s_dl_on > 0) {
        const double d = s_dlc.draws ? 100.0 / (double)s_dlc.draws : 0.0;
        char ca[64]; int cp = 0; ca[0] = 0;
        for (u32 i = 0; i < s_dlc.n_ca && cp < (int)sizeof ca - 8; i++) cp += snprintf(ca + cp, sizeof ca - (size_t)cp, " %04X", s_dlc.ca_regs[i]);
        k = snprintf(line, sizeof line,
                     "[delta] %.1f s: %.0f draws/frame (%u frames); whole way %.1f%%, part reused %.1f%%, all state reused %.1f%%"
                     " (constants-only %.1f%%); reused: programs %.0f%% render-state %.0f%% targets %.0f%% textures %.0f%%"
                     " pipeline %.0f%%; whole way because: first/dropped %.1f%% frame %.1f%% clear/write-back %.1f%%"
                     " switch %.1f%% tables %.1f%% targets %.1f%% catch-all %.1f%%%s%s; sink_end %.3f us/draw",
                     sec, frames ? (double)s_dlc.draws / frames : 0.0, frames,
                     s_dlc.full * d, s_dlc.partial * d, s_dlc.state * d, s_dlc.consts * d,
                     s_dlc.used[0] * d, s_dlc.used[1] * d, s_dlc.used[2] * d, s_dlc.used[3] * d, s_dlc.used[4] * d,
                     s_dlc.why[DLW_INVALID] * d, s_dlc.why[DLW_FRAME] * d, s_dlc.why[DLW_EVENT] * d, s_dlc.why[DLW_KNOB] * d,
                     s_dlc.why[DLW_TABLES] * d, s_dlc.why[DLW_SURF] * d, s_dlc.why[DLW_CATCHALL] * d,
                     cp ? " (registers" : "", cp ? ca : "", us);
        if (cp && k > 0 && k < (int)sizeof line - 2) k += snprintf(line + k, sizeof line - (size_t)k, ")");
        if (s_dl_verify && k > 0 && k < (int)sizeof line - 40)
            k += snprintf(line + k, sizeof line - (size_t)k, "; verify: %llu differences", (unsigned long long)s_dlc.vdiff);
    } else {
        k = snprintf(line, sizeof line, "[delta] %.1f s: OFF (PS3RECOMP_ENG_DELTA=0); %.0f draws/frame (%u frames); sink_end %.3f us/draw",
                     sec, frames ? (double)s_dlc.timed / frames : 0.0, frames, us);
    }
    if (k > 0 && k < (int)sizeof line - 1) { line[k++] = '\n'; line[k] = 0; }
    rsx_log_line(line, k, (int)sizeof line);
    /* (fastdraw) [delta-cost]: per path, its share of the draws, its time per
     * draw and that time by stage (ns per draw of the path; each stage holds
     * about one mark's own cost, given as "mark") */
    if (tpns > 0.0 && s_dlc.timed) {
        static const char* const pname[DLP_N] = { "whole-way", "part:textures", "part:programs", "part:other",
                                                  "all-state", "all-state+vertices" };
        static const char* const sname[DLM_N] = { "begin", "layout", "vertices", "state+targets", "tex-units", "tex-rest",
                                                  "pipeline", "diag+indices+constants", "binds", "dynamic", "draw", "tail" };
        static const char* const bname[DLB_N] = { "entry", "targets", "pipe+pass+fb", "arenas+set", "constants",
                                                  "pass", "records", "rest" };
        const double mark = s_dlc.caln ? (double)s_dlc.cal / (double)s_dlc.caln / tpns : 0.0;
        for (u32 p = 0; p < DLP_N; p++) {
            if (!s_dlc.pn[p]) continue;
            const double n = (double)s_dlc.pn[p];
            char cl[900];
            int c = snprintf(cl, sizeof cl, "[delta-cost] %s %.1f%% of draws, %.3f us/draw", pname[p],
                             100.0 * n / (double)s_dlc.timed, (double)s_dlc.pt[p] / tpns / 1000.0 / n);
            if (s_dl_cost && c > 0 && c < (int)sizeof cl - 40)
                c += snprintf(cl + c, sizeof cl - (size_t)c, " (mark %.0f ns); ns/draw:", mark);
            for (u32 s = 0; s_dl_cost && s < DLM_N && c > 0 && c < (int)sizeof cl - 40; s++)
                c += snprintf(cl + c, sizeof cl - (size_t)c, " %s %.0f", sname[s], (double)s_dlc.st[p][s] / tpns / n);
            if (s_dl_cost && c > 0 && c < (int)sizeof cl - 20) c += snprintf(cl + c, sizeof cl - (size_t)c, "; backend draw:");
            for (u32 s = 0; s_dl_cost && s < DLB_N && c > 0 && c < (int)sizeof cl - 40; s++)
                c += snprintf(cl + c, sizeof cl - (size_t)c, " %s %.0f", bname[s], (double)g_eng_bst[p][s] / tpns / n);
            if (c > 0 && c < (int)sizeof cl - 1) { cl[c++] = '\n'; cl[c] = 0; }
            rsx_log_line(cl, c, (int)sizeof cl);
        }
    }
    if (s_dl_cost && tpns > 0.0 && s_dlc.timed) {   /* (fastdraw) [delta-part]: what the part-reused draws pay */
        static const char* const pn2[DLP_N] = { "whole", "p:tex", "p:prog", "p:other", "state", "state+vx" };
        char cl[2600];
        const double pt = (double)(s_dlc.ptw[0] + s_dlc.ptw[1] + s_dlc.ptw[2]);
        const double q = pt > 0.0 ? 100.0 / pt : 0.0;
        int c = snprintf(cl, sizeof cl, "[delta-part] part:textures because: registers %.0f%% generations %.0f%% last draw's"
                         " state-dependent %.0f%%; units changed 0/1/2/3/4+: %.0f/%.0f/%.0f/%.0f/%.0f%%; a sampler field %.0f%%,"
                         " image fields only %.0f%% |", s_dlc.ptw[0] * q, s_dlc.ptw[1] * q, s_dlc.ptw[2] * q,
                         s_dlc.ptu[0] * q, s_dlc.ptu[1] * q, s_dlc.ptu[2] * q, s_dlc.ptu[3] * q, s_dlc.ptu[4] * q,
                         s_dlc.pts * q, s_dlc.pti * q);
        for (u32 p = 0; p < DLP_N && c > 0 && c < (int)sizeof cl - 260; p++) {
            if (!s_dlc.pn[p]) continue;
            const double n = (double)s_dlc.pn[p];
            const double sn = (double)(g_eng_bsetn[p][0] + g_eng_bsetn[p][1] + g_eng_bsetn[p][2]);
            c += snprintf(cl + c, sizeof cl - (size_t)c, " %s: vx pair/probe/fetch %.0f/%.0f/%.0f%%, units/draw memo %.2f probe %.2f,"
                          " pipe lookups/draw %.2f (tex-masks hit %.0f%% key hit %.0f%%), set memo/table/new %.0f/%.0f/%.0f%%"
                          " ns/draw %.0f/%.0f/%.0f/%.0f, samplers/draw %.1f x %.0f entries |", pn2[p],
                          100.0 * s_dlc.vx[p][0] / n, 100.0 * s_dlc.vx[p][1] / n, 100.0 * s_dlc.vx[p][2] / n,
                          s_dlc.ts[p][0] / n, s_dlc.ts[p][1] / n, s_dlc.pk[p][0] / n,
                          s_dlc.pk[p][0] ? 100.0 * s_dlc.pk[p][1] / s_dlc.pk[p][0] : 0.0,
                          s_dlc.pk[p][0] ? 100.0 * s_dlc.pk[p][2] / s_dlc.pk[p][0] : 0.0,
                          sn > 0 ? 100.0 * g_eng_bsetn[p][0] / sn : 0.0, sn > 0 ? 100.0 * g_eng_bsetn[p][1] / sn : 0.0,
                          sn > 0 ? 100.0 * g_eng_bsetn[p][2] / sn : 0.0,
                          g_eng_bset[p][0] / tpns / n, g_eng_bset[p][1] / tpns / n, g_eng_bset[p][2] / tpns / n,
                          g_eng_bset[p][3] / tpns / n, g_eng_bsamp[p][0] / n,
                          g_eng_bsamp[p][0] ? (double)g_eng_bsamp[p][1] / (double)g_eng_bsamp[p][0] : 0.0);
        }
        if (c > 0 && c < (int)sizeof cl - 200)
            c += snprintf(cl + c, sizeof cl - (size_t)c, " pair memo misses: none recorded %llu, signature %llu (the one"
                          " before last %llu), remap %llu, entry %llu, written %llu, stamp %llu; not recorded: no signature %llu,"
                          " untracked %llu, no spans %llu, >2 spans %llu (spans %llu), mode %llu, long %llu",
                          (unsigned long long)s_dlx_pm[0], (unsigned long long)s_dlx_pm[1], (unsigned long long)s_dlx_pm[7],
                          (unsigned long long)s_dlx_pm[2], (unsigned long long)s_dlx_pm[3], (unsigned long long)s_dlx_pm[4],
                          (unsigned long long)s_dlx_pm[5], (unsigned long long)s_dlx_pn[0], (unsigned long long)s_dlx_pn[1],
                          (unsigned long long)s_dlx_pn[2], (unsigned long long)s_dlx_pn[3], (unsigned long long)s_dlx_pn[7],
                          (unsigned long long)s_dlx_pn[4], (unsigned long long)s_dlx_pn[5]);
        if (c > 0 && c < (int)sizeof cl - 1) { cl[c++] = '\n'; cl[c] = 0; }
        rsx_log_line(cl, c, (int)sizeof cl);
    }
    memset(s_dlx_pm, 0, sizeof s_dlx_pm); memset(s_dlx_pn, 0, sizeof s_dlx_pn);
    memset(g_eng_bset, 0, sizeof g_eng_bset); memset(g_eng_bsetn, 0, sizeof g_eng_bsetn); memset(g_eng_bsamp, 0, sizeof g_eng_bsamp);
    fd_report();   /* (fastdraw) the [fastdraw] line (rsx_draw_fast.inc) */
    memset(g_eng_bst, 0, sizeof g_eng_bst);
    const u32 f0 = g.frames;
    memset(&s_dlc, 0, sizeof s_dlc);
    s_dlc.ns0 = now; s_dlc.tsc0 = tsc; s_dlc.frame0 = f0;
}
static void dl_frame_report(void) { dl_frame_report_ex(0); }
