/* SPU job record / replay equivalence checker. See spu_jobrec.h for the modes.
 *
 * The unit of recording is one call of a WWS job module by the job manager
 * (pm_wwsjob, the lifted policy module): its `bisle` at LS 0x3214 branches to
 * the job's entry in the job code buffer (0x4000+, the resident job overlay)
 * with interrupts enabled and the return address 0x3218 in r0, and the job runs
 * until it returns there. Everything in between -- the job, the job-manager API
 * calls it makes, and the job manager's interrupt handler, which runs inside the
 * job whenever a DMA-list stall notification is pending -- is one record.
 *
 * What comes from outside the SPU while it runs is recorded and served back on
 * replay: the values of channel reads and counts that other processors or the
 * host clock feed (event status, signal notification, the decrementer, the
 * inbound mailbox, the outbound mailbox counts), the bytes each DMA GET
 * delivered, and each atomic's outcome. Everything else the runtime computes
 * again from the job's own state, and the result is compared with what it
 * computed in the recording. What the job writes out -- channel writes, DMA PUT
 * bytes, atomics' store data -- is compared as it happens; the final local
 * store, register file and runtime context are compared at the end.
 *
 * Interrupts. The handler's operations form their own stream (ctx->irq_saved is
 * set between the take and its iret); each stream is matched in order on its
 * own, so a build whose lifted code takes the same interrupt at another point of
 * the job (merged chunks have fewer interrupt check points) still lines up
 * operation for operation. */
#include "spu_context.h"
#include "spu_dma.h"
#include "spu_jobrec.h"
#include "ps3emu/ps3_ftrace.h"   /* PS3_FT_SPU_JOB: one span per job call (stats mode) */
#include "../platform/win32_compat.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_spu_jobrec_mode = 0;

extern void spu_indirect_branch(spu_context* ctx);
extern void spu_drain_call(spu_context* ctx, uint32_t return_pc);
extern int  spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
extern void spu_halt(spu_context* ctx);
extern void spu_mfc_release(spu_context* ctx);
extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
extern uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa);

/* PS3_JOBREPLAY_INTERP=1: replay on the SPU interpreter (spu_interp.c) instead of
 * the lifted code -- a third opinion on the lifter (control flow, register use,
 * merged chunks), which shares only the instruction helpers with it. The
 * interpreter takes no interrupts, so only recordings that took none are run. */
static int s_interp = 0;

/* ------------------------------------------------------------------------- */
/* Recording format                                                          */
/* ------------------------------------------------------------------------- */
#define JR_MAGIC   "PS3JREC1"
#define JR_VERSION 1u
#define JR_MAX_OVL 512

enum { JR_SKIP_RDCH = 1, JR_SKIP_RCHCNT = 2, JR_SKIP_WRCH = 4, JR_SKIP_ATOM = 8, JR_SKIP_STOP = 16 };

enum { EV_RDCH = 1, EV_RCHCNT = 2, EV_WRCH = 3, EV_DMA = 4, EV_ATOM = 5, EV_STOP = 6, EV_IRQ = 7 };
static const char* const s_ev_name[] = { "?", "rdch", "rchcnt", "wrch", "dma", "atomic", "stop", "irq" };

/* One operation. stream: 0 = the job (and the job-manager code it calls),
 * 1 = inside the interrupt handler, 2 = information only (not matched). */
typedef struct jr_ev {
    uint8_t  type, stream;
    uint16_t ch;          /* channel, MFC command or stop code */
    uint32_t a, b, c, d;  /* per type, see the record functions */
    uint32_t len, off;    /* payload in the data blob */
} jr_ev;

/* The runtime context a job starts from / ends with (everything of
 * spu_context that dispatch, the MFC engine or the job can read, except the
 * register file and local store, which are stored beside it). */
typedef struct jr_state {
    uint32_t pc, status, stop_code, spu_id, spu_group_id;
    int32_t  image_id, resident_ovl, resident_task, module_img_a00, policy_mode, park_on_empty_inmbox;
    uint32_t decrementer, srr0, event_status, event_mask, int_enable, irq_saved, irq_resume_pc;
    uint32_t mfc_lsa, mfc_eah, mfc_eal, mfc_size, mfc_tag, mfc_tag_mask, mfc_tag_status, atomic_stat;
    uint32_t code_base, host_depth, sn_defer, drain_ret_pc, irq_frame_set, ls_is_store;
    uint64_t steps, resv_word;
    uint8_t  resv_line[128];
    struct { uint32_t lsa, size, source_ea; int32_t image_id; uint32_t reloc; } resident_code[SPU_RESIDENT_CODE_SLOTS];
    uint32_t list_stall_mask, list_stall_stat;
    uint32_t list_stall_elem_lsa[32], list_stall_remaining[32], list_stall_dest_lsa[32], list_stall_cmd[32];
    uint64_t list_stall_ea_base[32];
    uint32_t rcv_evt[4];
    int32_t  rcv_evt_n, rcv_evt_i;
    uint32_t out_mbox_count, out_intr_mbox_count, mfc_bytes, pad0;
    uint8_t  mfc[1024];
} jr_state;

typedef struct jr_hdr {
    char     magic[8];
    uint32_t version, hdr_size, state_size, ev_size;
    int32_t  ovl;          /* resident job overlay image id (the module) */
    uint32_t entry_pc, ret_pc, seq, spu_id;
    uint32_t n_ev, data_len, n_end_lines;
    uint64_t t_ms;         /* ms since the process started */
    uint64_t dur_ns;       /* host time of the job call while recorded */
    uint32_t n_irq, n_irq_lr, n_get, n_put, n_atom, n_stop;
    uint64_t get_bytes, put_bytes;
    uint32_t reserved[8];
} jr_hdr;

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */
static char     s_dir[1024];
static uint32_t s_ret_pc = 0x3218;
static long     s_max = 8, s_every = 1;
static uint64_t s_after_ms = 0, s_gap_ms = 0;
static uint8_t  s_want[JR_MAX_OVL];      /* 1 = record this overlay id */
static int      s_stat_sec = 10;
static uint64_t s_qpf = 1, s_qpc0 = 0;

static volatile long long s_calls[JR_MAX_OVL], s_ns[JR_MAX_OVL], s_max_ns[JR_MAX_OVL];
static volatile long      s_recorded[JR_MAX_OVL], s_seen[JR_MAX_OVL];
static volatile long long s_last_rec_ms[JR_MAX_OVL];
static volatile long      s_aborted, s_overflow, s_skipped_nested;
static volatile long long s_last_stat_ms;
static volatile long long s_rec_ns;          /* host time spent recording (capture + file) */

static uint64_t jr_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (uint64_t)q.QuadPart; }
static uint64_t jr_ns(uint64_t d) { return (uint64_t)((double)d * 1e9 / (double)s_qpf); }
static uint64_t jr_ms_now(void) { return (uint64_t)((double)(jr_qpc() - s_qpc0) * 1e3 / (double)s_qpf); }

static void jr_parse_ids(const char* e, uint8_t* set, uint8_t v)
{
    while (e && *e) {
        char* end;
        long id = strtol(e, &end, 0);
        if (end == e) { e++; continue; }
        if (id >= 0 && id < JR_MAX_OVL) set[id] = v;
        e = end;
    }
}

__attribute__((constructor)) static void jr_init(void)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qpf = (uint64_t)f.QuadPart;
    s_qpc0 = jr_qpc();
    const char* rec = getenv("PS3_JOBREC");
    const char* rep = getenv("PS3_JOBREPLAY");
    const char* st  = getenv("PS3_JOBSTAT");
    int mode = 0;
    if (rep && *rep) {
        mode = SPU_JOBREC_REPLAY;
        snprintf(s_dir, sizeof s_dir, "%s", rep);
    } else {
        if (rec && *rec) {
            mode |= SPU_JOBREC_RECORD | SPU_JOBREC_STATS;
            snprintf(s_dir, sizeof s_dir, "%s", rec);
            CreateDirectoryA(s_dir, NULL);
        }
        if (st && *st && *st != '0') mode |= SPU_JOBREC_STATS;
    }
    const char* e;
    if ((e = getenv("PS3_JOBREC_RET")))   s_ret_pc = (uint32_t)strtoul(e, 0, 0) & SPU_LS_MASK;
    if ((e = getenv("PS3_JOBREC_MAX")))   s_max = strtol(e, 0, 0);
    if ((e = getenv("PS3_JOBREC_EVERY"))) s_every = strtol(e, 0, 0) > 0 ? strtol(e, 0, 0) : 1;
    if ((e = getenv("PS3_JOBREC_AFTER"))) s_after_ms = (uint64_t)(atof(e) * 1000.0);
    if ((e = getenv("PS3_JOBREC_GAP_MS"))) s_gap_ms = (uint64_t)strtoull(e, 0, 0);
    if ((e = getenv("PS3_JOBSTAT_SEC")))  s_stat_sec = atoi(e) > 0 ? atoi(e) : 10;
    /* Module filter: ids of the resident job overlay (jobmods_register.c:
     * spu_begin_image(N) per module). Default every module except the two whose
     * work the port moves to the GPU (PS3RECOMP_ENG_GPU_LIGHT/SSAO replace a
     * function of the light job, 219, and of the SSAO job, 226, with host code
     * that reads guest memory directly -- not through DMA -- so a recording of
     * them cannot hold their inputs). PS3_JOBREC_SKIPMOD="" records them too. */
    if ((e = getenv("PS3_JOBREC_MOD")) && *e) jr_parse_ids(e, s_want, 1);
    else memset(s_want, 1, sizeof s_want);
    e = getenv("PS3_JOBREC_SKIPMOD");
    jr_parse_ids(e ? e : "219,226", s_want, 0);
    if (mode & SPU_JOBREC_RECORD)
        fprintf(stderr, "[jobrec] RECORD to %s: max %ld per module, every %ld, after %.1f s, gap %llu ms, ret 0x%X\n",
                s_dir, s_max, s_every, s_after_ms / 1000.0, (unsigned long long)s_gap_ms, s_ret_pc);
    else if (mode & SPU_JOBREC_STATS)
        fprintf(stderr, "[jobrec] STATS: job calls per module every %d s\n", s_stat_sec);
    g_spu_jobrec_mode = mode;
}

/* ------------------------------------------------------------------------- */
/* Per-thread state                                                          */
/* ------------------------------------------------------------------------- */
typedef struct jr_file {
    uint8_t*      buf;
    size_t        size;
    const jr_hdr* h;
    const jr_state *st0, *st1;
    const u128   *gpr0, *gpr1;
    const uint8_t* ls0;
    const jr_ev*  ev;
    const uint8_t* data;
    const uint8_t* end_lines;   /* n_end_lines x {u32 lsa; u8[16]} */
    uint32_t*     sidx[2];      /* per stream: indices into ev */
    uint32_t      ns[2];
    char          name[260];
} jr_file;

typedef struct jr_tls {
    int          active;      /* a job of `ctx` is being recorded / replayed / timed */
    unsigned     skip;        /* JR_SKIP_*: the recorder's own call of a hooked operation, consumed by its hook */
    uint32_t     depth0;      /* ctx->host_depth at the job call */
    int          recording, replaying;
    spu_context* ctx;
    /* recording */
    jr_ev*   ev;   uint32_t n_ev, cap_ev;
    uint8_t* data; uint32_t n_data, cap_data;
    uint8_t* ls0;
    u128     gpr0[128];
    jr_state st0, st1;
    int      overflow;
    uint32_t n_irq, n_irq_lr, n_get, n_put, n_atom, n_stop;
    uint64_t get_bytes, put_bytes;
    /* replay */
    const jr_file* f;
    uint32_t cur[2];
    int      failed;
    int      done;        /* the job call came back */
    char     why[1024];
} jr_tls;

static SPU_THREAD_LOCAL jr_tls* t_jr;

static jr_tls* jr_tls_get(void)
{
    if (!t_jr) {
        t_jr = (jr_tls*)calloc(1, sizeof(jr_tls));
        t_jr->ls0 = (uint8_t*)malloc(SPU_LS_SIZE);
    }
    return t_jr;
}

static inline jr_tls* jr_on(spu_context* ctx)
{
    jr_tls* t = t_jr;
    return (t && t->active && t->ctx == ctx && (t->recording || t->replaying)) ? t : NULL;
}

/* The operations themselves, with this thread's hooks bypassed: the hooked
 * functions are the only copies, so the hooks cost one load + branch when off. */
extern u128 spu_rdch(spu_context* ctx, uint32_t channel);
extern uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);
extern void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
extern void spu_stop(spu_context* ctx);
uint32_t spu_rdch_raw(spu_context* ctx, uint32_t ch)
{
    t_jr->skip |= JR_SKIP_RDCH; const u128 v = spu_rdch(ctx, ch);
    return v._u32[0];
}
uint32_t spu_rchcnt_raw(spu_context* ctx, uint32_t ch)
{
    t_jr->skip |= JR_SKIP_RCHCNT; const uint32_t v = spu_rchcnt(ctx, ch);
    return v;
}
void spu_wrch_raw(spu_context* ctx, uint32_t ch, uint32_t v)
{
    u128 x; memset(&x, 0, sizeof x); x._u32[0] = v;
    t_jr->skip |= JR_SKIP_WRCH; spu_wrch(ctx, ch, x);
}
void spu_stop_raw(spu_context* ctx)
{
    t_jr->skip |= JR_SKIP_STOP; spu_stop(ctx);
}

/* ------------------------------------------------------------------------- */
/* Context state capture / restore                                           */
/* ------------------------------------------------------------------------- */
static void jr_capture(spu_context* c, jr_state* s)
{
    memset(s, 0, sizeof *s);
    s->pc = c->pc; s->status = c->status; s->stop_code = c->stop_code;
    s->spu_id = c->spu_id; s->spu_group_id = c->spu_group_id;
    s->image_id = c->image_id; s->resident_ovl = c->resident_ovl; s->resident_task = c->resident_task;
    s->module_img_a00 = c->module_img_a00; s->policy_mode = c->policy_mode;
    s->park_on_empty_inmbox = c->park_on_empty_inmbox;
    s->decrementer = c->decrementer; s->srr0 = c->srr0;
    s->event_status = spu_ev_get(c); s->event_mask = c->event_mask;
    s->int_enable = c->int_enable; s->irq_saved = (uint32_t)c->irq_saved; s->irq_resume_pc = c->irq_resume_pc;
    s->mfc_lsa = c->mfc_lsa; s->mfc_eah = c->mfc_eah; s->mfc_eal = c->mfc_eal; s->mfc_size = c->mfc_size;
    s->mfc_tag = c->mfc_tag; s->mfc_tag_mask = c->mfc_tag_mask; s->mfc_tag_status = c->mfc_tag_status;
    s->atomic_stat = c->atomic_stat;
    s->code_base = c->code_base; s->host_depth = c->host_depth; s->sn_defer = c->sn_defer;
    s->drain_ret_pc = c->drain_ret_pc; s->irq_frame_set = c->irq_frame != NULL;
    s->ls_is_store = c->ls == c->ls_store;
    s->steps = c->steps; s->resv_word = SPU_RESV_LOAD(c);
    memcpy(s->resv_line, c->resv_line, 128);
    for (int i = 0; i < SPU_RESIDENT_CODE_SLOTS; i++) {
        s->resident_code[i].lsa = c->resident_code[i].lsa;
        s->resident_code[i].size = c->resident_code[i].size;
        s->resident_code[i].source_ea = c->resident_code[i].source_ea;
        s->resident_code[i].image_id = c->resident_code[i].image_id;
        s->resident_code[i].reloc = c->resident_code[i].reloc;
    }
    s->list_stall_mask = c->list_stall_mask; s->list_stall_stat = c->list_stall_stat;
    memcpy(s->list_stall_elem_lsa, c->list_stall_elem_lsa, sizeof s->list_stall_elem_lsa);
    memcpy(s->list_stall_remaining, c->list_stall_remaining, sizeof s->list_stall_remaining);
    memcpy(s->list_stall_dest_lsa, c->list_stall_dest_lsa, sizeof s->list_stall_dest_lsa);
    memcpy(s->list_stall_cmd, c->list_stall_cmd, sizeof s->list_stall_cmd);
    memcpy(s->list_stall_ea_base, c->list_stall_ea_base, sizeof s->list_stall_ea_base);
    memcpy(s->rcv_evt, c->rcv_evt, sizeof s->rcv_evt);
    s->rcv_evt_n = c->rcv_evt_n; s->rcv_evt_i = c->rcv_evt_i;
    s->out_mbox_count = spu_channel_count(&c->ch_out_mbox);
    s->out_intr_mbox_count = spu_channel_count(&c->ch_out_intr_mbox);
    s->mfc_bytes = (uint32_t)sizeof(mfc_engine);
    memcpy(s->mfc, spu_jobrec_mfc_engine(c), sizeof(mfc_engine));
}

static void jr_restore(spu_context* c, const jr_state* s)
{
    c->pc = s->pc; c->status = s->status; c->stop_code = s->stop_code;
    c->spu_id = s->spu_id; c->spu_group_id = s->spu_group_id;
    c->image_id = s->image_id; c->resident_ovl = s->resident_ovl; c->resident_task = s->resident_task;
    c->module_img_a00 = s->module_img_a00; c->policy_mode = s->policy_mode;
    c->park_on_empty_inmbox = s->park_on_empty_inmbox;
    c->decrementer = s->decrementer; c->srr0 = s->srr0;
    c->event_status = s->event_status; c->event_mask = s->event_mask;
    c->int_enable = s->int_enable; c->irq_saved = (int)s->irq_saved; c->irq_resume_pc = s->irq_resume_pc;
    c->mfc_lsa = s->mfc_lsa; c->mfc_eah = s->mfc_eah; c->mfc_eal = s->mfc_eal; c->mfc_size = s->mfc_size;
    c->mfc_tag = s->mfc_tag; c->mfc_tag_mask = s->mfc_tag_mask; c->mfc_tag_status = s->mfc_tag_status;
    c->atomic_stat = s->atomic_stat;
    c->code_base = s->code_base; c->host_depth = s->host_depth; c->sn_defer = s->sn_defer;
    c->drain_ret_pc = s->drain_ret_pc; c->irq_frame = NULL;
    c->steps = s->steps; c->resv_word = s->resv_word;
    memcpy(c->resv_line, s->resv_line, 128);
    for (int i = 0; i < SPU_RESIDENT_CODE_SLOTS; i++) {
        c->resident_code[i].lsa = s->resident_code[i].lsa;
        c->resident_code[i].size = s->resident_code[i].size;
        c->resident_code[i].source_ea = s->resident_code[i].source_ea;
        c->resident_code[i].image_id = s->resident_code[i].image_id;
        c->resident_code[i].reloc = s->resident_code[i].reloc;
    }
    c->list_stall_mask = s->list_stall_mask; c->list_stall_stat = s->list_stall_stat;
    memcpy(c->list_stall_elem_lsa, s->list_stall_elem_lsa, sizeof s->list_stall_elem_lsa);
    memcpy(c->list_stall_remaining, s->list_stall_remaining, sizeof s->list_stall_remaining);
    memcpy(c->list_stall_dest_lsa, s->list_stall_dest_lsa, sizeof s->list_stall_dest_lsa);
    memcpy(c->list_stall_cmd, s->list_stall_cmd, sizeof s->list_stall_cmd);
    memcpy(c->list_stall_ea_base, s->list_stall_ea_base, sizeof s->list_stall_ea_base);
    memcpy(c->rcv_evt, s->rcv_evt, sizeof s->rcv_evt);
    c->rcv_evt_n = s->rcv_evt_n; c->rcv_evt_i = s->rcv_evt_i;
    memcpy(spu_jobrec_mfc_engine(c), s->mfc, sizeof(mfc_engine));
}

/* ------------------------------------------------------------------------- */
/* Recording                                                                 */
/* ------------------------------------------------------------------------- */
#define JR_MAX_DATA (64u << 20)   /* a job that moves more than this is not recorded */

static jr_ev* jr_push(jr_tls* t, int type, uint32_t ch, const void* p, uint32_t len)
{
    if (t->overflow) return NULL;
    if (t->n_ev == t->cap_ev) {
        uint32_t nc = t->cap_ev ? t->cap_ev * 2 : 4096;
        jr_ev* ne = (jr_ev*)realloc(t->ev, (size_t)nc * sizeof(jr_ev));
        if (!ne) { t->overflow = 1; return NULL; }
        t->ev = ne; t->cap_ev = nc;
    }
    if (len) {
        if (t->n_data + len > JR_MAX_DATA) { t->overflow = 1; return NULL; }
        if (t->n_data + len > t->cap_data) {
            uint32_t nc = t->cap_data ? t->cap_data : (1u << 20);
            while (nc < t->n_data + len) nc *= 2;
            uint8_t* nd = (uint8_t*)realloc(t->data, nc);
            if (!nd) { t->overflow = 1; return NULL; }
            t->data = nd; t->cap_data = nc;
        }
        memcpy(t->data + t->n_data, p, len);
    }
    jr_ev* e = &t->ev[t->n_ev++];
    memset(e, 0, sizeof *e);
    e->type = (uint8_t)type;
    e->stream = (uint8_t)(t->ctx->irq_saved ? 1 : 0);
    e->ch = (uint16_t)ch;
    e->len = len; e->off = len ? t->n_data : 0;
    t->n_data += len;
    return e;
}

static void jr_rec_begin(jr_tls* t, spu_context* ctx)
{
    t->n_ev = 0; t->n_data = 0; t->overflow = 0;
    t->n_irq = t->n_irq_lr = t->n_get = t->n_put = t->n_atom = t->n_stop = 0;
    t->get_bytes = t->put_bytes = 0;
    jr_capture(ctx, &t->st0);
    memcpy(t->gpr0, ctx->gpr, sizeof t->gpr0);
    memcpy(t->ls0, ctx->ls, SPU_LS_SIZE);
}

static int jr_write_all(FILE* f, const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }

static void jr_rec_end(jr_tls* t, spu_context* ctx, int ovl, uint32_t entry, uint64_t dur_ns, uint64_t t_ms)
{
    if (t->overflow) { __atomic_add_fetch(&s_overflow, 1, __ATOMIC_RELAXED); return; }
    jr_capture(ctx, &t->st1);
    /* end local store: the 16-byte lines that differ from the start */
    uint32_t n_lines = 0;
    for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16)
        if (memcmp(ctx->ls + a, t->ls0 + a, 16)) n_lines++;
    const long seq = __atomic_fetch_add(&s_recorded[ovl], 1, __ATOMIC_RELAXED);
    char path[1200];
    snprintf(path, sizeof path, "%s\\ovl%03d_%05X_%04ld_spu%X.jrec", s_dir, ovl, entry, seq, ctx->spu_id);
    FILE* f = fopen(path, "wb");
    if (!f) { static int _n; if (_n++ < 4) fprintf(stderr, "[jobrec] cannot write %s\n", path); return; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    jr_hdr h; memset(&h, 0, sizeof h);
    memcpy(h.magic, JR_MAGIC, 8);
    h.version = JR_VERSION; h.hdr_size = sizeof h; h.state_size = sizeof(jr_state); h.ev_size = sizeof(jr_ev);
    h.ovl = ovl; h.entry_pc = entry; h.ret_pc = s_ret_pc; h.seq = (uint32_t)seq; h.spu_id = ctx->spu_id;
    h.n_ev = t->n_ev; h.data_len = t->n_data; h.n_end_lines = n_lines;
    h.t_ms = t_ms; h.dur_ns = dur_ns;
    h.n_irq = t->n_irq; h.n_irq_lr = t->n_irq_lr; h.n_get = t->n_get; h.n_put = t->n_put;
    h.n_atom = t->n_atom; h.n_stop = t->n_stop; h.get_bytes = t->get_bytes; h.put_bytes = t->put_bytes;
    int ok = jr_write_all(f, &h, sizeof h) && jr_write_all(f, &t->st0, sizeof t->st0) &&
             jr_write_all(f, t->gpr0, sizeof t->gpr0) && jr_write_all(f, t->ls0, SPU_LS_SIZE) &&
             jr_write_all(f, t->ev, (size_t)t->n_ev * sizeof(jr_ev)) &&
             (!t->n_data || jr_write_all(f, t->data, t->n_data)) &&
             jr_write_all(f, &t->st1, sizeof t->st1) && jr_write_all(f, ctx->gpr, sizeof t->gpr0);
    for (uint32_t a = 0; ok && a < SPU_LS_SIZE; a += 16)
        if (memcmp(ctx->ls + a, t->ls0 + a, 16))
            ok = jr_write_all(f, &a, 4) && jr_write_all(f, ctx->ls + a, 16);
    ok = ok && jr_write_all(f, "JEND", 4);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(path); fprintf(stderr, "[jobrec] write failed: %s\n", path); }
}

/* ------------------------------------------------------------------------- */
/* Statistics                                                                */
/* ------------------------------------------------------------------------- */
static void jr_stat_print(uint64_t now_ms)
{
    char path[1200];
    FILE* f = NULL;
    if (s_dir[0]) { snprintf(path, sizeof path, "%s\\jobstat.tsv", s_dir); f = fopen(path, "w"); }
    long long tot_ns = 0;
    for (int i = 0; i < JR_MAX_OVL; i++) tot_ns += s_ns[i];
    fprintf(stderr, "[jobstat] t=%.1fs job calls since start (ovl calls host_ms share%% avg_us max_us rec):",
            now_ms / 1000.0);
    if (f) fprintf(f, "t_s\t%.1f\nrec_ms\t%.1f\naborted\t%ld\noverflow\t%ld\n"
                      "ovl\tcalls\thost_ms\tshare_pct\tavg_us\tmax_us\trecorded\n",
                   now_ms / 1000.0, s_rec_ns / 1e6, s_aborted, s_overflow);
    for (int i = 0; i < JR_MAX_OVL; i++) {
        if (!s_calls[i]) continue;
        const double ms = s_ns[i] / 1e6, sh = tot_ns ? 100.0 * s_ns[i] / tot_ns : 0;
        const double avg = s_ns[i] / 1e3 / s_calls[i], mx = s_max_ns[i] / 1e3;
        fprintf(stderr, " | %d %lld %.0f %.1f%% %.1f %.0f %ld", i, s_calls[i], ms, sh, avg, mx, s_recorded[i]);
        if (f) fprintf(f, "%d\t%lld\t%.3f\t%.2f\t%.2f\t%.1f\t%ld\n", i, s_calls[i], ms, sh, avg, mx, s_recorded[i]);
    }
    fprintf(stderr, "\n");
    if (f) fclose(f);
}

/* ------------------------------------------------------------------------- */
/* The job call                                                              */
/* ------------------------------------------------------------------------- */
static void jr_call_job(spu_context* ctx, uint32_t ret)
{
    spu_indirect_branch(ctx);     /* the job's entry (this hook passes: t->active) */
    spu_drain_call(ctx, ret);     /* ... and on until it returns to the job manager */
}

int spu_jobrec_job_call(spu_context* ctx)
{
    const int mode = g_spu_jobrec_mode;
    if (!(mode & (SPU_JOBREC_RECORD | SPU_JOBREC_STATS))) return 0;
    const uint32_t pc = ctx->pc & SPU_LS_MASK;
    if ((ctx->gpr[0]._u32[0] & SPU_LS_MASK) != s_ret_pc || ctx->resident_ovl < 200 ||
        ctx->resident_ovl >= JR_MAX_OVL || pc < 0x4000u)
        return 0;
    /* an iret dispatch (irq_saved until its restore) is never a job call */
    if (ctx->irq_saved || ctx->ls != ctx->ls_store || ctx->tramp_fn) return 0;
    jr_tls* t = jr_tls_get();
    if (t->active) {
        if (t->ctx != ctx) { __atomic_add_fetch(&s_skipped_nested, 1, __ATOMIC_RELAXED); return 0; }
        /* An indirect branch inside the open job (a job may branch through a
         * register before it has changed r0) -- or the wrapper's own entry
         * dispatch. Only a call below the open job's depth means that job was
         * unwound without coming back (halts and restarts also say so through
         * spu_jobrec_unwound). */
        if (ctx->host_depth >= t->depth0) return 0;
        __atomic_add_fetch(&s_aborted, 1, __ATOMIC_RELAXED);
        t->active = 0;
    }
    const int ovl = ctx->resident_ovl;
    const uint64_t now_ms = jr_ms_now();
    int rec = 0;
    if ((mode & SPU_JOBREC_RECORD) && s_want[ovl] && now_ms >= s_after_ms &&
        (s_max <= 0 || s_recorded[ovl] < s_max)) {
        const long n = __atomic_fetch_add(&s_seen[ovl], 1, __ATOMIC_RELAXED);
        if (n % s_every == 0 &&
            (!s_gap_ms || now_ms - (uint64_t)s_last_rec_ms[ovl] >= s_gap_ms || !s_last_rec_ms[ovl])) {
            s_last_rec_ms[ovl] = (long long)now_ms;
            rec = 1;
        }
    }
    t->active = 1; t->ctx = ctx; t->recording = rec; t->replaying = 0; t->depth0 = ctx->host_depth; t->skip = 0;
    uint64_t r0 = 0, r1 = 0;
    if (rec) { r0 = jr_qpc(); jr_rec_begin(t, ctx); r1 = jr_qpc(); }
    const uint64_t q0 = jr_qpc();
    jr_call_job(ctx, s_ret_pc);
    const uint64_t q1 = jr_qpc();
    const uint64_t ns = jr_ns(q1 - q0);
    if (PS3_FT_ON() && ns >= ps3_ft_min_ns())   /* PS3_FTRACE + PS3_JOBSTAT: the job timeline */
        ps3_ft_span(PS3_FT_SPU_JOB, ctx->spu_id, (uint32_t)ovl, pc, jr_ns(q0), jr_ns(q1));
    if (rec && t->active) {
        const uint64_t w0 = jr_qpc();
        jr_rec_end(t, ctx, ovl, pc, ns, now_ms);
        __atomic_add_fetch(&s_rec_ns, (long long)jr_ns(jr_qpc() - w0 + (r1 - r0)), __ATOMIC_RELAXED);
    }
    t->active = 0; t->recording = 0;
    __atomic_add_fetch(&s_calls[ovl], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_ns[ovl], (long long)ns, __ATOMIC_RELAXED);
    if ((long long)ns > s_max_ns[ovl]) s_max_ns[ovl] = (long long)ns;
    const long long last = s_last_stat_ms;
    if (now_ms - (uint64_t)last >= (uint64_t)s_stat_sec * 1000u &&
        __atomic_compare_exchange_n(&s_last_stat_ms, (long long*)&last, (long long)now_ms, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        jr_stat_print(now_ms);
    return 1;
}

void spu_jobrec_unwound(spu_context* ctx)
{
    jr_tls* t = t_jr;
    if (t && t->active && t->ctx == ctx) {
        t->skip = 0;   /* an unwind out of a bypassed call (a stop that exits) */
        if (t->recording) __atomic_add_fetch(&s_aborted, 1, __ATOMIC_RELAXED);
        if (!t->replaying) t->active = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Replay: matching                                                          */
/* ------------------------------------------------------------------------- */
static __attribute__((noreturn)) void jr_fail(jr_tls* t, const char* fmt, ...);

static void jr_vfail(jr_tls* t, const char* fmt, va_list ap)
{
    if (!t->failed) {
        int s = t->ctx->irq_saved ? 1 : 0;
        int n = snprintf(t->why, sizeof t->why, "[stream %d op %u, pc 0x%05X] ", s, t->cur[s],
                         (uint32_t)t->ctx->pc & SPU_LS_MASK);
        vsnprintf(t->why + n, sizeof t->why - (size_t)n, fmt, ap);
        t->failed = 1;
    }
}

static void jr_fail(jr_tls* t, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); jr_vfail(t, fmt, ap); va_end(ap);
    t->ctx->status = SPU_STATUS_STOPPED_BY_HALT;
    spu_halt(t->ctx);
    for (;;) abort();   /* spu_halt returns only when no landing pad is armed */
}

static const jr_ev* jr_next(jr_tls* t, int type, uint32_t ch)
{
    const int s = t->ctx->irq_saved ? 1 : 0;
    if (t->cur[s] >= t->f->ns[s])
        jr_fail(t, "extra %s ch/cmd 0x%X: the recording's stream %d has no more operations",
                s_ev_name[type], ch, s);
    const jr_ev* e = &t->f->ev[t->f->sidx[s][t->cur[s]]];
    if (e->type != type || e->ch != ch)
        jr_fail(t, "operation differs: replay %s 0x%X, recording %s 0x%X (a=0x%X b=0x%X)",
                s_ev_name[type], ch, s_ev_name[e->type < 8 ? e->type : 0], e->ch, e->a, e->b);
    t->cur[s]++;
    return e;
}

/* Channels whose value comes from outside the SPU: served from the recording. */
static int jr_served_rdch(uint32_t ch)
{
    return ch == SPU_RdEventStat || ch == SPU_RdSigNotify1 || ch == SPU_RdSigNotify2 ||
           ch == SPU_RdDec || ch == SPU_RdInMbox;
}
static int jr_served_rchcnt(uint32_t ch)
{
    return ch == SPU_RdEventStat || ch == SPU_RdSigNotify1 || ch == SPU_RdSigNotify2 ||
           ch == SPU_WrOutMbox || ch == SPU_RdInMbox || ch == SPU_WrOutIntrMbox;
}
/* Event bits the SPU raises itself (tag-status update, DMA-list stall-and-notify);
 * the rest (lock-line reservation lost) arrive from other processors. */
#define JR_EV_INTERNAL 0x3u

/* ------------------------------------------------------------------------- */
/* The hooks                                                                 */
/* ------------------------------------------------------------------------- */
int spu_jobrec_rdch(spu_context* ctx, uint32_t ch, uint32_t* out)
{
    jr_tls* t = jr_on(ctx);
    if (!t) return 0;
    if (t->skip & JR_SKIP_RDCH) { t->skip &= ~(unsigned)JR_SKIP_RDCH; return 0; }   /* the recorder's own call */
    if (t->recording) {
        const uint32_t v = spu_rdch_raw(ctx, ch);
        jr_ev* e = jr_push(t, EV_RDCH, ch, NULL, 0);
        if (e) { e->a = v; e->b = spu_ev_get(ctx); }
        *out = v;
        return 1;
    }
    const jr_ev* e = jr_next(t, EV_RDCH, ch);
    if (jr_served_rdch(ch)) {
        if (ch == SPU_RdEventStat) {
            const uint32_t mine = spu_ev_get(ctx);
            if ((mine ^ e->a) & JR_EV_INTERNAL)
                jr_fail(t, "rdch SPU_RdEventStat: own event bits 0x%X, recording 0x%X", mine & JR_EV_INTERNAL,
                        e->a & JR_EV_INTERNAL);
            spu_ev_raise(ctx, e->a & ~JR_EV_INTERNAL);   /* what other processors had raised */
        }
        *out = e->a;
        return 1;
    }
    const uint32_t v = spu_rdch_raw(ctx, ch);
    if (v != e->a) jr_fail(t, "rdch ch%u = 0x%08X, recording 0x%08X", ch, v, e->a);
    *out = v;
    return 1;
}

int spu_jobrec_rchcnt(spu_context* ctx, uint32_t ch, uint32_t* out)
{
    jr_tls* t = jr_on(ctx);
    if (!t) return 0;
    if (t->skip & JR_SKIP_RCHCNT) { t->skip &= ~(unsigned)JR_SKIP_RCHCNT; return 0; }   /* the recorder's own call */
    if (t->recording) {
        const uint32_t v = spu_rchcnt_raw(ctx, ch);
        jr_ev* e = jr_push(t, EV_RCHCNT, ch, NULL, 0);
        if (e) { e->a = v; e->b = spu_ev_get(ctx); }
        *out = v;
        return 1;
    }
    const jr_ev* e = jr_next(t, EV_RCHCNT, ch);
    if (jr_served_rchcnt(ch)) {
        if (ch == SPU_RdEventStat) {
            spu_ev_raise(ctx, e->b & ~JR_EV_INTERNAL);
            const uint32_t mine = (spu_ev_get(ctx) & ctx->event_mask) != 0;
            if (mine != e->a)
                jr_fail(t, "rchcnt SPU_RdEventStat = %u, recording %u (events 0x%X mask 0x%X, recorded events 0x%X)",
                        mine, e->a, spu_ev_get(ctx), ctx->event_mask, e->b);
        }
        *out = e->a;
        return 1;
    }
    const uint32_t v = spu_rchcnt_raw(ctx, ch);
    if (v != e->a) jr_fail(t, "rchcnt ch%u = %u, recording %u", ch, v, e->a);
    *out = v;
    return 1;
}

int spu_jobrec_wrch(spu_context* ctx, uint32_t ch, uint32_t v)
{
    jr_tls* t = jr_on(ctx);
    if (!t) return 0;
    if (t->skip & JR_SKIP_WRCH) { t->skip &= ~(unsigned)JR_SKIP_WRCH; return 0; }   /* the recorder's own call */
    if (t->recording) {
        jr_ev* e = jr_push(t, EV_WRCH, ch, NULL, 0);
        if (e) e->a = v;
        spu_wrch_raw(ctx, ch, v);
        return 1;
    }
    const jr_ev* e = jr_next(t, EV_WRCH, ch);
    if (v != e->a) jr_fail(t, "wrch ch%u value 0x%08X, recording 0x%08X", ch, v, e->a);
    /* The interrupt mailbox goes to the PPU (an event queue push): nothing to deliver it to. */
    if (ch != SPU_WrOutIntrMbox) spu_wrch_raw(ctx, ch, v);
    return 1;
}

/* mfc_do_transfer_raw's early outs, so the replay knows whether the recorded
 * transfer reached its copy (and the overlay note that follows a GET's copy). */
static int jr_dma_copies(uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd, int rc)
{
    static int s_lax = -1;
    if (s_lax < 0) s_lax = getenv("SPU_DMA_LAX") ? 1 : 0;
    if (rc != 0 || size == 0) return 0;
    if (!s_lax && ((size > 0x4000) || (size >= 16 && (size & 15)) || (((lsa ^ (uint32_t)ea) & 15) != 0) ||
                   (mfc_is_put(cmd) && (uint32_t)ea < 0x10000u)))
        return 0;
    return mfc_is_get(cmd) || mfc_is_put(cmd);
}

int spu_jobrec_dma(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd, int* rc)
{
    jr_tls* t = jr_on(ctx);
    if (!t) return 0;
    const uint32_t l = lsa & SPU_LS_MASK;
    const uint32_t n = size > SPU_LS_SIZE - l ? SPU_LS_SIZE - l : size;
    if (t->recording) {
        jr_ev* e;
        if (mfc_is_put(cmd)) {
            e = jr_push(t, EV_DMA, cmd & 0xFFFF, ctx->ls + l, n);
            *rc = mfc_do_transfer_raw(ctx, lsa, ea, size, cmd);
            t->n_put++; t->put_bytes += n;
        } else {
            *rc = mfc_do_transfer_raw(ctx, lsa, ea, size, cmd);
            const int copied = jr_dma_copies(lsa, ea, size, cmd, *rc);
            e = jr_push(t, EV_DMA, cmd & 0xFFFF, ctx->ls + l, copied ? n : 0);
            t->n_get++; t->get_bytes += n;
        }
        if (e) { e->a = lsa; e->b = (uint32_t)ea; e->c = size; e->d = (uint32_t)*rc; }
        return 1;
    }
    const jr_ev* e = jr_next(t, EV_DMA, cmd & 0xFFFF);
    if (e->a != lsa || e->b != (uint32_t)ea || e->c != size)
        jr_fail(t, "DMA cmd 0x%X request lsa=0x%05X ea=0x%08X size=0x%X, recording lsa=0x%05X ea=0x%08X size=0x%X",
                cmd, lsa, (uint32_t)ea, size, e->a, e->b, e->c);
    *rc = (int)e->d;
    if (mfc_is_put(cmd)) {
        const uint8_t* rec = t->f->data + e->off;
        if (e->len != n) jr_fail(t, "PUT size %u, recording %u", n, e->len);
        for (uint32_t i = 0; i < n; i++)
            if (ctx->ls[l + i] != rec[i])
                jr_fail(t, "PUT ea=0x%08X size=0x%X: byte +0x%X (LS 0x%05X) is %02X, recording %02X",
                        (uint32_t)ea, size, i, l + i, ctx->ls[l + i], rec[i]);
    } else if (e->len) {
        memcpy(ctx->ls + l, t->f->data + e->off, e->len);
        if (mfc_is_get(cmd))
            spu_overlay_note_get(ctx, (uint32_t)ea, ctx->ls + l, size);
    }
    return 1;
}

static int jr_is_atomic(uint32_t cmd)
{
    return cmd == MFC_GETLLAR_CMD || cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD || cmd == MFC_PUTQLLUC_CMD;
}

/* payload: LS line before (128), LS line after (128), resv_line after (128), resv_word after (8) */
int spu_jobrec_atomic(spu_context* ctx, uint32_t cmd, int* handled)
{
    jr_tls* t = jr_on(ctx);
    if (!t) return 0;
    if (t->skip & JR_SKIP_ATOM) { t->skip &= ~(unsigned)JR_SKIP_ATOM; return 0; }   /* the recorder's own call */
    if (!jr_is_atomic(cmd)) { *handled = 0; return 1; }
    const uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK & ~127u;
    const uint32_t lin = ctx->mfc_lsa & SPU_LS_MASK;
    const uint32_t ea = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    (void)lsa;
    if (t->recording) {
        uint8_t buf[392];
        memcpy(buf, ctx->ls + lin, 128);
        t->skip |= JR_SKIP_ATOM; *handled = spu_mfc_atomic_raw(ctx, cmd);
        memcpy(buf + 128, ctx->ls + lin, 128);
        memcpy(buf + 256, ctx->resv_line, 128);
        const uint64_t rw = SPU_RESV_LOAD(ctx);
        memcpy(buf + 384, &rw, 8);
        jr_ev* e = jr_push(t, EV_ATOM, cmd, buf, sizeof buf);
        if (e) { e->a = ctx->mfc_lsa; e->b = ea; e->c = ctx->atomic_stat; e->d = (uint32_t)*handled; }
        t->n_atom++;
        return 1;
    }
    const jr_ev* e = jr_next(t, EV_ATOM, cmd);
    if (e->a != ctx->mfc_lsa || e->b != ea)
        jr_fail(t, "atomic 0x%X lsa=0x%05X ea=0x%08X, recording lsa=0x%05X ea=0x%08X", cmd, ctx->mfc_lsa, ea, e->a, e->b);
    const uint8_t* p = t->f->data + e->off;
    if (cmd != MFC_GETLLAR_CMD)
        for (int i = 0; i < 128; i++)
            if (ctx->ls[lin + i] != p[i])
                jr_fail(t, "atomic 0x%X ea=0x%08X store data byte +0x%X is %02X, recording %02X",
                        cmd, ea, i, ctx->ls[lin + i], p[i]);
    memcpy(ctx->ls + lin, p + 128, 128);
    memcpy(ctx->resv_line, p + 256, 128);
    uint64_t rw; memcpy(&rw, p + 384, 8);
    ctx->resv_word = rw;
    ctx->atomic_stat = e->c;
    *handled = (int)e->d;
    return 1;
}

/* payload: status after, rcv_evt[4], rcv_evt_n, rcv_evt_i */
int spu_jobrec_stop(spu_context* ctx)
{
    jr_tls* t = jr_on(ctx);
    if (!t) return 0;
    if (t->skip & JR_SKIP_STOP) { t->skip &= ~(unsigned)JR_SKIP_STOP; return 0; }   /* the recorder's own call */
    if (t->recording) {
        jr_ev* e = jr_push(t, EV_STOP, ctx->stop_code & 0xFFFF, NULL, 0);
        const uint32_t idx = e ? (uint32_t)(e - t->ev) : 0;
        t->n_stop++;
        spu_stop_raw(ctx);   /* may unwind (exit stop): the recording is then abandoned */
        if (e) {
            e = &t->ev[idx];
            e->a = ctx->status;
            e->b = (uint32_t)ctx->rcv_evt_n; e->c = (uint32_t)ctx->rcv_evt_i;
            e->d = ctx->rcv_evt[0];
        }
        return 1;
    }
    const jr_ev* e = jr_next(t, EV_STOP, ctx->stop_code & 0xFFFF);
    if (e->b != (uint32_t)ctx->rcv_evt_n)   /* an lv2 stop that queued a reply: not modelled */
        jr_fail(t, "stop 0x%X queued a mailbox reply in the recording (not replayable)", ctx->stop_code);
    ctx->status = e->a;
    return 1;
}

int spu_jobrec_no_lifted(const spu_context* ctx)
{
    const jr_tls* t = t_jr;
    return s_interp && t && t->active && t->replaying && t->ctx == ctx;
}

void spu_jobrec_irq(spu_context* ctx, uint32_t pending)
{
    jr_tls* t = jr_on(ctx);
    if (!t || !t->recording) return;
    t->n_irq++;
    if (pending & ~JR_EV_INTERNAL) t->n_irq_lr++;
    jr_ev* e = jr_push(t, EV_IRQ, 0, NULL, 0);
    if (e) { e->stream = 2; e->a = ctx->pc; e->b = pending; }
}

/* ------------------------------------------------------------------------- */
/* Replay driver                                                             */
/* ------------------------------------------------------------------------- */
static void jr_file_free(jr_file* f)
{
    free(f->buf); free(f->sidx[0]); free(f->sidx[1]);
    memset(f, 0, sizeof *f);
}

static int jr_file_load(const char* path, jr_file* f, char* err, size_t errn)
{
    memset(f, 0, sizeof *f);
    FILE* fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errn, "cannot open"); return 0; }
    _fseeki64(fp, 0, SEEK_END);
    const long long sz = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    f->buf = (uint8_t*)malloc((size_t)sz);
    f->size = (size_t)sz;
    const int rd = f->buf && fread(f->buf, 1, (size_t)sz, fp) == (size_t)sz;
    fclose(fp);
    if (!rd) { snprintf(err, errn, "read failed"); return 0; }
    const jr_hdr* h = (const jr_hdr*)f->buf;
    if (sz < (long long)sizeof *h || memcmp(h->magic, JR_MAGIC, 8) || h->version != JR_VERSION ||
        h->hdr_size != sizeof *h || h->state_size != sizeof(jr_state) || h->ev_size != sizeof(jr_ev)) {
        snprintf(err, errn, "not a version-%u recording of this build's layout", JR_VERSION);
        return 0;
    }
    size_t o = sizeof *h;
    f->h = h;
    f->st0 = (const jr_state*)(f->buf + o); o += sizeof(jr_state);
    f->gpr0 = (const u128*)(f->buf + o);    o += 128 * sizeof(u128);
    f->ls0 = f->buf + o;                    o += SPU_LS_SIZE;
    f->ev = (const jr_ev*)(f->buf + o);     o += (size_t)h->n_ev * sizeof(jr_ev);
    f->data = f->buf + o;                   o += h->data_len;
    f->st1 = (const jr_state*)(f->buf + o); o += sizeof(jr_state);
    f->gpr1 = (const u128*)(f->buf + o);    o += 128 * sizeof(u128);
    f->end_lines = f->buf + o;              o += (size_t)h->n_end_lines * 20;
    if (o + 4 != f->size || memcmp(f->buf + o, "JEND", 4)) { snprintf(err, errn, "truncated"); return 0; }
    for (int s = 0; s < 2; s++) f->sidx[s] = (uint32_t*)malloc(((size_t)h->n_ev + 1) * 4);
    for (uint32_t i = 0; i < h->n_ev; i++) {
        const int s = f->ev[i].stream;
        if (s < 2) f->sidx[s][f->ns[s]++] = i;
    }
    const char* b = strrchr(path, '\\');
    snprintf(f->name, sizeof f->name, "%s", b ? b + 1 : path);
    return 1;
}

typedef struct jr_result {
    int  pass;       /* 1 pass, 0 fail, -1 not loadable */
    int  ovl;
    uint32_t entry;
    char name[260];
    char why[1100];
    double us;       /* replay host time */
    double rec_us;   /* recorded job time */
    uint64_t hash;   /* final-state hash (determinism across repeats) */
} jr_result;

static uint64_t jr_fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

/* The end-of-job context fields that must match. */
typedef struct { const char* name; size_t off, size; uint32_t mask; int dispatch; } jr_field;
#define JRF(f, m) { #f, offsetof(jr_state, f), sizeof(((jr_state*)0)->f), m, 0 }
#define JRD(f, m) { #f, offsetof(jr_state, f), sizeof(((jr_state*)0)->f), m, 1 }   /* lifted dispatch state */
static const jr_field s_end_fields[] = {
    JRF(pc, 0xFFFFFFFFu), JRF(status, 0xFFFFFFFFu), JRF(stop_code, 0xFFFFFFFFu),
    JRD(image_id, 0xFFFFFFFFu), JRF(resident_ovl, 0xFFFFFFFFu), JRD(resident_task, 0xFFFFFFFFu),
    JRD(module_img_a00, 0xFFFFFFFFu), JRF(decrementer, 0xFFFFFFFFu), JRF(srr0, 0xFFFFFFFFu),
    JRF(event_status, JR_EV_INTERNAL), JRF(event_mask, 0xFFFFFFFFu), JRF(int_enable, 0xFFFFFFFFu),
    JRF(irq_saved, 0xFFFFFFFFu), JRF(mfc_lsa, 0xFFFFFFFFu), JRF(mfc_eah, 0xFFFFFFFFu), JRF(mfc_eal, 0xFFFFFFFFu),
    JRF(mfc_size, 0xFFFFFFFFu), JRF(mfc_tag, 0xFFFFFFFFu), JRF(mfc_tag_mask, 0xFFFFFFFFu),
    JRF(mfc_tag_status, 0xFFFFFFFFu), JRF(atomic_stat, 0xFFFFFFFFu), JRD(code_base, 0xFFFFFFFFu),
    JRD(host_depth, 0xFFFFFFFFu), JRF(resv_line, 0), JRF(resident_code, 0),
    JRF(list_stall_mask, 0xFFFFFFFFu), JRF(list_stall_stat, 0xFFFFFFFFu), JRF(list_stall_elem_lsa, 0),
    JRF(list_stall_remaining, 0), JRF(list_stall_dest_lsa, 0), JRF(list_stall_cmd, 0),
    JRF(list_stall_ea_base, 0), JRF(rcv_evt_n, 0xFFFFFFFFu), JRF(mfc, 0),
};

/* 1 = equal; else describes the first difference into why */
static int jr_compare_end(const jr_file* f, spu_context* ctx, const uint8_t* rec_ls, char* why, size_t wn)
{
    jr_state now; jr_capture(ctx, &now);
    for (size_t i = 0; i < sizeof s_end_fields / sizeof s_end_fields[0]; i++) {
        const jr_field* d = &s_end_fields[i];
        if (d->dispatch && s_interp) continue;
        const uint8_t* a = (const uint8_t*)&now + d->off;
        const uint8_t* b = (const uint8_t*)f->st1 + d->off;
        size_t sz = d->size;
        if (d->off == offsetof(jr_state, mfc)) sz = f->st1->mfc_bytes;
        if (d->mask && sz == 4) {
            uint32_t x, y; memcpy(&x, a, 4); memcpy(&y, b, 4);
            if ((x ^ y) & d->mask) {
                snprintf(why, wn, "end context %s = 0x%X, recording 0x%X", d->name, x & d->mask, y & d->mask);
                return 0;
            }
        } else if (memcmp(a, b, sz)) {
            size_t k = 0; while (a[k] == b[k]) k++;
            snprintf(why, wn, "end context %s differs at byte %zu (%02X, recording %02X)", d->name, k, a[k], b[k]);
            return 0;
        }
    }
    for (int r = 0; r < 128; r++)
        if (memcmp(&ctx->gpr[r], &f->gpr1[r], 16)) {
            int nr = 0; for (int q = 0; q < 128; q++) nr += memcmp(&ctx->gpr[q], &f->gpr1[q], 16) != 0;
            snprintf(why, wn, "end register r%d = %08X %08X %08X %08X, recording %08X %08X %08X %08X (%d registers differ)",
                     r, ctx->gpr[r]._u32[0], ctx->gpr[r]._u32[1], ctx->gpr[r]._u32[2], ctx->gpr[r]._u32[3],
                     f->gpr1[r]._u32[0], f->gpr1[r]._u32[1], f->gpr1[r]._u32[2], f->gpr1[r]._u32[3], nr);
            return 0;
        }
    if (memcmp(ctx->ls, rec_ls, SPU_LS_SIZE)) {
        uint32_t first = 0, lines = 0;
        for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16)
            if (memcmp(ctx->ls + a, rec_ls + a, 16)) { if (!lines) first = a; lines++; }
        uint32_t k = first; while (ctx->ls[k] == rec_ls[k]) k++;
        int n = snprintf(why, wn, "end local store: %u line(s) differ, first at LS 0x%05X; line 0x%05X replay/recording/start:",
                         lines, k, first);
        for (int row = 0; row < 3 && n < (int)wn - 60; row++) {
            const uint8_t* src = row == 0 ? ctx->ls : row == 1 ? rec_ls : f->ls0;
            n += snprintf(why + n, wn - (size_t)n, " ");
            for (int i = 0; i < 16; i++) n += snprintf(why + n, wn - (size_t)n, "%02X", src[first + i]);
        }
        return 0;
    }
    return 1;
}

static void jr_replay_entry(spu_context* ctx)
{
    jr_tls* t = t_jr;
    /* spu_run_with_halt zeroed these: the job runs at the depth it ran at */
    ctx->host_depth = t->f->st0->host_depth;
    ctx->steps = t->f->st0->steps;
    ctx->drain_ret_pc = t->f->st0->drain_ret_pc;
    if (s_interp) {
        spu_interp_run_until(ctx, ctx->pc, t->f->h->ret_pc);
        if ((ctx->pc & SPU_LS_MASK) != t->f->h->ret_pc || ctx->status != SPU_STATUS_RUNNING) return;
    } else {
        jr_call_job(ctx, t->f->h->ret_pc);
    }
    t->done = 1;
}

static int s_break_get = 0;   /* PS3_JOBREPLAY_BREAK=get: corrupt one served GET byte (checker self-test) */

/* A replay that runs away (a broken build looping) is stopped by the
 * watchdog in spu_jobreplay_main: stop_request is checked at trampoline
 * transfers and channel waits. */
typedef struct jr_slot { SRWLOCK lock; spu_context* ctx; uint64_t t0_ms; } jr_slot;

static void jr_replay_one(const jr_file* f, jr_result* r, uint8_t* rec_ls, jr_slot* slot)
{
    jr_tls* t = jr_tls_get();
    spu_context* ctx = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
    spu_context_init(ctx, f->h->spu_id);
    jr_restore(ctx, f->st0);
    memcpy(ctx->gpr, f->gpr0, sizeof ctx->gpr);
    memcpy(ctx->ls, f->ls0, SPU_LS_SIZE);
    t->active = 1; t->ctx = ctx; t->recording = 0; t->replaying = 1; t->skip = 0;
    t->f = f; t->cur[0] = t->cur[1] = 0; t->failed = 0; t->done = 0; t->why[0] = 0;
    AcquireSRWLockExclusive(&slot->lock); slot->ctx = ctx; slot->t0_ms = jr_ms_now(); ReleaseSRWLockExclusive(&slot->lock);
    const uint64_t q0 = jr_qpc();
    const int halted = spu_run_with_halt(jr_replay_entry, ctx);
    AcquireSRWLockExclusive(&slot->lock); slot->ctx = NULL; ReleaseSRWLockExclusive(&slot->lock);
    r->us = jr_ns(jr_qpc() - q0) / 1e3;
    t->active = 0; t->replaying = 0;
    r->rec_us = f->h->dur_ns / 1e3;
    if (t->failed) {
        r->pass = 0; snprintf(r->why, sizeof r->why, "%s", t->why);
    } else if (halted || !t->done) {
        r->pass = 0;
        snprintf(r->why, sizeof r->why, "the job did not return (halted=%d stop_request=%u status=0x%X pc=0x%05X)",
                 halted, ctx->stop_request, ctx->status, ctx->pc & SPU_LS_MASK);
    } else if (t->cur[0] != f->ns[0] || t->cur[1] != f->ns[1]) {
        r->pass = 0;
        const int s = t->cur[0] != f->ns[0] ? 0 : 1;
        const jr_ev* e = &f->ev[f->sidx[s][t->cur[s]]];
        snprintf(r->why, sizeof r->why, "missing operations: stream %d replayed %u of %u; next recorded %s 0x%X a=0x%X b=0x%X",
                 s, t->cur[s], f->ns[s], s_ev_name[e->type < 8 ? e->type : 0], e->ch, e->a, e->b);
    } else {
        memcpy(rec_ls, f->ls0, SPU_LS_SIZE);
        for (uint32_t i = 0; i < f->h->n_end_lines; i++) {
            uint32_t a; memcpy(&a, f->end_lines + 20 * i, 4);
            memcpy(rec_ls + (a & SPU_LS_MASK & ~15u), f->end_lines + 20 * i + 4, 16);
        }
        r->pass = jr_compare_end(f, ctx, rec_ls, r->why, sizeof r->why);
    }
    uint64_t h = 0xCBF29CE484222325ull;
    h = jr_fnv(h, ctx->gpr, sizeof ctx->gpr); h = jr_fnv(h, ctx->ls, SPU_LS_SIZE);
    h = jr_fnv(h, &r->pass, sizeof r->pass); h = jr_fnv(h, r->why, strlen(r->why));
    r->hash = h;
    spu_mfc_release(ctx);
    _aligned_free(ctx);
}

/* ---- worker pool ---- */
typedef struct {
    char (*paths)[1200];
    int n, repeat;
    jr_result* res;        /* n * repeat */
    volatile long next;
    volatile long done;
    volatile long nslot;
    jr_slot slot[64];
} jr_job_set;

static DWORD WINAPI jr_worker(LPVOID arg)
{
    jr_job_set* js = (jr_job_set*)arg;
    jr_slot* slot = &js->slot[__atomic_fetch_add(&js->nslot, 1, __ATOMIC_RELAXED) & 63];
    uint8_t* rec_ls = (uint8_t*)malloc(SPU_LS_SIZE);
    for (;;) {
        const long i = __atomic_fetch_add(&js->next, 1, __ATOMIC_RELAXED);
        if (i >= js->n) break;
        jr_file f; char err[256];
        if (!jr_file_load(js->paths[i], &f, err, sizeof err)) {
            for (int k = 0; k < js->repeat; k++) {
                jr_result* r = &js->res[(size_t)i * js->repeat + k];
                r->pass = -1; snprintf(r->why, sizeof r->why, "%s", err);
                const char* b = strrchr(js->paths[i], '\\');
                snprintf(r->name, sizeof r->name, "%s", b ? b + 1 : js->paths[i]);
            }
            jr_file_free(&f);
            __atomic_add_fetch(&js->done, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (s_break_get)   /* self-test: flip one bit in the middle of the first GET payload */
            for (uint32_t e = 0; e < f.h->n_ev; e++)
                if (f.ev[e].type == EV_DMA && f.ev[e].len && mfc_is_get(f.ev[e].ch)) {
                    ((uint8_t*)f.data)[f.ev[e].off + f.ev[e].len / 2] ^= 0x01;
                    break;
                }
        for (int k = 0; k < js->repeat; k++) {
            jr_result* r = &js->res[(size_t)i * js->repeat + k];
            r->ovl = f.h->ovl; r->entry = f.h->entry_pc;
            snprintf(r->name, sizeof r->name, "%s", f.name);
            if (s_interp && (f.h->n_irq || f.h->n_stop)) {
                r->pass = -2;
                snprintf(r->why, sizeof r->why, "skipped: %u interrupt(s), %u stop(s) (the interpreter takes no interrupts)",
                         f.h->n_irq, f.h->n_stop);
                continue;
            }
            jr_replay_one(&f, r, rec_ls, slot);
        }
        jr_file_free(&f);
        __atomic_add_fetch(&js->done, 1, __ATOMIC_RELAXED);
    }
    free(rec_ls);
    return 0;
}

static int jr_cmp_path(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

int spu_jobreplay_main(void)
{
    if (!(g_spu_jobrec_mode & SPU_JOBREC_REPLAY)) return -1;
    const char* e;
    int threads = (e = getenv("PS3_JOBREPLAY_THREADS")) ? atoi(e) : 2;
    int repeat = (e = getenv("PS3_JOBREPLAY_REPEAT")) ? atoi(e) : 1;
    const uint64_t timeout_ms = (e = getenv("PS3_JOBREPLAY_TIMEOUT_MS")) ? strtoull(e, 0, 0) : 20000;
    long per_mod = (e = getenv("PS3_JOBREPLAY_MAX")) ? strtol(e, 0, 0) : 0;
    s_break_get = (e = getenv("PS3_JOBREPLAY_BREAK")) && !strcmp(e, "get");
    s_interp = (e = getenv("PS3_JOBREPLAY_INTERP")) && *e == '1';
    uint8_t want[JR_MAX_OVL];
    if ((e = getenv("PS3_JOBREPLAY_MOD")) && *e) { memset(want, 0, sizeof want); jr_parse_ids(e, want, 1); }
    else memset(want, 1, sizeof want);
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (repeat < 1) repeat = 1;

    /* the recordings, sorted by name (= module, then sequence) */
    char pat[1200]; snprintf(pat, sizeof pat, "%s\\*.jrec", s_dir);
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd);
    int cap = 1024, n = 0;
    char (*paths)[1200] = malloc((size_t)cap * sizeof *paths);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (n == cap) { cap *= 2; paths = realloc(paths, (size_t)cap * sizeof *paths); }
            snprintf(paths[n++], 1200, "%s\\%s", s_dir, fd.cFileName);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    qsort(paths, (size_t)n, sizeof *paths, jr_cmp_path);
    /* module filter + per-module cap, by the ovl id in the file name */
    int m = 0; long per[JR_MAX_OVL]; memset(per, 0, sizeof per);
    for (int i = 0; i < n; i++) {
        const char* b = strrchr(paths[i], '\\'); b = b ? b + 1 : paths[i];
        int ovl = (!strncmp(b, "ovl", 3)) ? atoi(b + 3) : 0;
        if (ovl < 0 || ovl >= JR_MAX_OVL || !want[ovl]) continue;
        if (per_mod > 0 && per[ovl] >= per_mod) continue;
        per[ovl]++;
        if (m != i) memcpy(paths[m], paths[i], sizeof *paths);
        m++;
    }
    n = m;
    printf("[jobreplay] %d recording(s) in %s, %d thread(s), %d repeat(s)%s%s%s\n", n, s_dir, threads, repeat,
           s_break_get ? ", BREAK=get (self-test: every replay must fail)" : "",
           s_interp ? ", on the SPU INTERPRETER" : "",
           getenv("SPU_FLOAT_RZ") ? " (SPU_FLOAT_RZ set)" : "");
    fflush(stdout);
    jr_job_set js; memset(&js, 0, sizeof js);
    js.paths = paths; js.n = n; js.repeat = repeat;
    js.res = (jr_result*)calloc((size_t)n * repeat + 1, sizeof(jr_result));
    for (int i = 0; i < 64; i++) InitializeSRWLock(&js.slot[i].lock);
    const uint64_t q0 = jr_qpc();
    HANDLE* th = (HANDLE*)calloc((size_t)threads, sizeof(HANDLE));
    for (int i = 0; i < threads; i++)
        th[i] = CreateThread(NULL, 64u << 20, jr_worker, &js, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    long last = -1;
    uint64_t last_print = 0;
    while (WaitForMultipleObjects((DWORD)threads, th, TRUE, 250) == WAIT_TIMEOUT) {
        const uint64_t now = jr_ms_now();
        for (int i = 0; i < threads; i++) {   /* watchdog */
            jr_slot* sl = &js.slot[i];
            AcquireSRWLockExclusive(&sl->lock);
            if (sl->ctx && now - sl->t0_ms > timeout_ms)
                __atomic_store_n(&sl->ctx->stop_request, 1u, __ATOMIC_RELEASE);
            ReleaseSRWLockExclusive(&sl->lock);
        }
        long d = js.done;
        if (d != last && now - last_print >= 2000) {
            printf("[jobreplay] %ld / %d\n", d, n); fflush(stdout); last = d; last_print = now;
        }
    }
    for (int i = 0; i < threads; i++) CloseHandle(th[i]);
    const double secs = jr_ns(jr_qpc() - q0) / 1e9;

    /* report */
    char rp[1200];
    if ((e = getenv("PS3_JOBREPLAY_REPORT")) && *e) snprintf(rp, sizeof rp, "%s", e);
    else snprintf(rp, sizeof rp, "%s\\replay_report.txt", s_dir);
    FILE* out = fopen(rp, "w");
    long np = 0, nf = 0, nbad = 0, ndet = 0, nskip = 0;
    /* best = per recording the fastest of its repeats, averaged over the module's recordings:
     * the speed metric that other load on the machine disturbs least */
    struct { long n, pass, fail, nrec; double us, rec_us, best; int first_fail; uint32_t entry; } mod[JR_MAX_OVL];
    memset(mod, 0, sizeof mod);
    for (int i = 0; i < JR_MAX_OVL; i++) mod[i].first_fail = -1;
    for (int i = 0; i < n; i++) {
        const jr_result* r0 = &js.res[(size_t)i * repeat];
        for (int k = 1; k < repeat; k++)
            if (js.res[(size_t)i * repeat + k].hash != r0->hash || js.res[(size_t)i * repeat + k].pass != r0->pass) ndet++;
        { double b = -1; int o = 0;
          for (int k = 0; k < repeat; k++) {
              const jr_result* r = &js.res[(size_t)i * repeat + k];
              if (r->pass < 0) continue;
              o = r->ovl >= 0 && r->ovl < JR_MAX_OVL ? r->ovl : 0;
              if (b < 0 || r->us < b) b = r->us;
          }
          if (b >= 0) { mod[o].best += b; mod[o].nrec++; } }
        for (int k = 0; k < repeat; k++) {
            const jr_result* r = &js.res[(size_t)i * repeat + k];
            if (r->pass == -2) { nskip++; continue; }
            if (r->pass < 0) { nbad++; continue; }
            const int o = r->ovl >= 0 && r->ovl < JR_MAX_OVL ? r->ovl : 0;
            mod[o].n++; mod[o].entry = r->entry;
            mod[o].us += r->us; mod[o].rec_us += r->rec_us;
            if (r->pass) { mod[o].pass++; np++; }
            else { mod[o].fail++; nf++; if (mod[o].first_fail < 0) mod[o].first_fail = i * repeat + k; }
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        FILE* w = pass ? out : stdout;
        if (!w) continue;
        fprintf(w, "== job replay: %d recording(s) x %d, %.1f s, %d thread(s)%s%s\n", n, repeat, secs, threads,
                getenv("SPU_FLOAT_RZ") ? " [SPU_FLOAT_RZ set]" : "", s_interp ? " [SPU interpreter]" : "");
        fprintf(w, "%-6s %-8s %6s %6s %6s %10s %10s %10s\n", "ovl", "entry", "jobs", "pass", "FAIL", "rec_us", "replay_us", "best_us");
        for (int o = 0; o < JR_MAX_OVL; o++) {
            if (!mod[o].n) continue;
            fprintf(w, "%-6d 0x%05X  %6ld %6ld %6ld %10.1f %10.1f %10.1f\n", o, mod[o].entry, mod[o].n, mod[o].pass,
                    mod[o].fail, mod[o].rec_us / mod[o].n, mod[o].us / mod[o].n, mod[o].nrec ? mod[o].best / mod[o].nrec : 0.0);
        }
        fprintf(w, "TOTAL: %ld passed, %ld FAILED, %ld unreadable, %ld nondeterministic (repeat differs), %ld skipped\n",
                np, nf, nbad, ndet, nskip);
        for (int o = 0; o < JR_MAX_OVL; o++) {
            if (mod[o].first_fail < 0) continue;
            const jr_result* r = &js.res[mod[o].first_fail];
            fprintf(w, "first FAIL of ovl %d: %s: %s\n", o, r->name, r->why);
        }
        if (pass) {   /* the file gets every result */
            for (int i = 0; i < n * repeat; i++) {
                const jr_result* r = &js.res[i];
                fprintf(w, "%s\t%s\t%.1f\t%.1f\t%016llX\t%s\n",
                        r->pass > 0 ? "PASS" : r->pass == 0 ? "FAIL" : r->pass == -2 ? "SKIP" : "BAD",
                        r->name, r->rec_us, r->us, (unsigned long long)r->hash, r->why);
            }
        }
        fflush(w);
    }
    if (out) fclose(out);
    printf("[jobreplay] report: %s\n", rp);
    fflush(stdout);
    return (nf || nbad || ndet || n == 0) ? 1 : 0;
}
