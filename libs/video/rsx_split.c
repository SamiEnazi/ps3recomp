/*
 * ps3recomp - (rsx-par) the RSX thread's frame on two threads
 * (PS3RECOMP_RSX_SPLIT, default 1; 0 = one thread, as before).
 *
 * Before: one thread walked the GCM FIFO, ran the register dispatcher on every
 * method word and, at every draw, the draw engine (vertex/index fetch,
 * textures, programs, pipeline, the Vulkan backend's recording). The walk and
 * the dispatch were ~20% of it at the city dock (rsx5 profile b3: walker self
 * 7.6%, dispatch ~13%), the engine the rest.
 *
 * Now the thread that drains (the walker, "A") keeps the walk and runs the
 * dispatcher on an instance of its own; the draw engine runs on an exec thread
 * ("B") on the engine's instance (g.rsx in rsx_draw_engine.c), which follows
 * A's state through a single-producer ring:
 *
 *  - A's instance tracks the 64-byte blocks of regs / vp / constants it writes
 *    (rsx_dispatch.dirty). Before every event, the changed ones are copied into
 *    the ring (a block whose bytes are what B already holds is not sent), so at
 *    every point where B runs engine code, B's regs/vp/constants equal A's --
 *    except the two upload windows' own registers (0x0B80.., 0x1F00..), which
 *    nothing but the dispatcher reads.
 *  - Events are A's dispatcher's sink calls (clear, begin, draw arrays, draw
 *    index array, inline array, end, flip), with the scalars the engine reads
 *    (current_primitive, in_begin_end, pair_packets, vp_gen) and the texture /
 *    misc generation counters (A's instance keeps its own; B writes them into
 *    g_rsx_dsp_tex_gen / g_rsx_dsp_misc_gen before each sink call, so the
 *    engine's caches key on the state they were built from). An inline array's
 *    bytes travel in the event and land in g.rsx.inline_data, where the
 *    engine's sink expects them to stay until END.
 *  - Walker actions (cellGcmSys.c: gcm_split_run) are everything the walk
 *    itself did with a word that is guest-visible -- semaphore releases,
 *    back-end labels, SET_REFERENCE fences, reports, user commands, flip
 *    requests and FIFO flip words, the 2D / memory-copy subchannels. They run on
 *    B in FIFO order, behind the draws before them, so the guest sees a label,
 *    a fence or a flip exactly when everything before it has been executed, as
 *    before.
 *  - A waits for B (rsx_split_wait_labels) before it reads a label for a
 *    semaphore acquire -- only as far as the last queued action, which may be
 *    the release it waits for -- and for everything (rsx_split_sync) at the end
 *    of a drain, so the GET pointer, the fence publication, the ring recycle,
 *    the present, the window pump and every other caller between drains see a
 *    quiet engine, as before.
 *
 * Ownership: the full dispatcher state lives in one instance at a time. A
 * drain that cannot take the split path (a FIFO diagnostic, lean state off,
 * the old live renderer) first gives it back (rsx_split_release: everything
 * queued has run, the whole struct copied over), so the old path continues
 * exactly where the split one stopped; the next split drain takes it again.
 */
#include "rsx_split.h"
#include "rsx_dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* rsx_draw_engine.c */
rsx_dispatch* rsx_draw_engine_dsp(void);
int rsx_draw_engine_ready(void);

void (*g_rsx_drain_end_hook)(void);   /* rsx_split.h */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#pragma comment(lib, "synchronization.lib")   /* WaitOnAddress / WakeByAddressSingle */
#include "../../runtime/platform/host_place.h"

_Static_assert(offsetof(rsx_dispatch, vp) == offsetof(rsx_dispatch, regs) + sizeof(u32) * RSX_DSP_NUM_REGS,
               "rsx_split: regs and vp must be one run");
_Static_assert(offsetof(rsx_dispatch, constants) == offsetof(rsx_dispatch, vp) + sizeof(u32) * RSX_DSP_VP_WORDS,
               "rsx_split: vp and constants must be one run");
_Static_assert(RSX_DSP_DIRTY_WORDS < 64, "rsx_split: the summary word");

#define SP_RING  (16u << 20)     /* bytes; a power of two */
#define SP_PUB   (16u << 10)     /* the walker publishes at least every 16 KB (and at every draw's END) */

enum { SP_WRAP = 0, SP_STATE, SP_EVENT, SP_ACT, SP_LIMIT };
enum { EV_CLEAR = 1, EV_BEGIN, EV_END, EV_ARRAYS, EV_INDEX, EV_INLINE, EV_FLIP };

typedef struct { u16 type, kind; u32 size; } sp_hdr;            /* size: the whole record, 8-aligned */
typedef struct { sp_hdr h; u32 n, pad; } sp_state;              /* then n sp_blk */
typedef struct { u32 id, pad; u8 b[RSX_DSP_BLK_BYTES]; } sp_blk;
typedef struct { u32 current_primitive, in_begin_end, pair_packets, vp_gen, tgen, mgen, const_gen, pad; } sp_scal;   /* const_gen: (merge) rsx-vtx's PS3RECOMP_VK_CONST_REUSE */
typedef struct { sp_hdr h; u32 a, b; sp_scal s; } sp_event_rec; /* EV_INLINE: then a bytes */
typedef struct { sp_hdr h; u32 a, b, c, pad; } sp_act_rec;      /* SP_ACT, SP_LIMIT */

static struct {
    int inited, failed, owner;   /* owner 1: the walker's instance holds the state */
    rsx_dispatch* a;             /* the walker's instance */
    u8* ring;
    u8* sent;                    /* what the engine's instance holds (RSX_DSP_BLK_COUNT blocks) */
    HANDLE thread;
    u64 wl, rc, pub;             /* walker: write position, last seen consumed position, last published */
    u64 label_pos;               /* walker: the end of the last queued action */
    u32 tgen, mgen;              /* the walker's instance's generation counters */
    u64 ticks_us;                /* rdtsc ticks per microsecond */
    /* PS3RECOMP_RSX_SPLIT_STATS: per report, walker side */
    u64 n_rec, n_blk, n_same, n_wait, wait_ticks, n_full, n_wake, n_drain;
    unsigned long long t_report;
} s_sp;
static u64 s_sp_dirty[RSX_DSP_DIRTY_WORDS + 1];
static __declspec(align(64)) volatile LONG64 s_sp_w;     /* published write position (walker) */
static __declspec(align(64)) volatile LONG64 s_sp_r;     /* consumed position (exec thread) */
static __declspec(align(64)) volatile LONG   s_sp_bsleep; /* the exec thread is (about to be) asleep */
static __declspec(align(64)) volatile LONG   s_sp_await;  /* the walker waits for s_sp_r */

extern u32 g_rsx_dsp_tex_gen, g_rsx_dsp_misc_gen;
extern void (*g_rsx_dsp_limit_hook)(int what, u32 n);

static int sp_switch(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_RSX_SPLIT"); on = (e && *e && *e != '0'); }   /* (next) default OFF: the critic measured it a net loss (semaphore stalls, sync points); =1 opts in */
    return on;
}

static inline u64 sp_load_r(void) { return (u64)__atomic_load_n(&s_sp_r, __ATOMIC_ACQUIRE); }


/* ---- (rsx-par2) PS3_STALL_MS: the RSX side's threads -------------------------
 * Each thread that matters registers itself (rsx_diag_thread_here) and marks
 * its long waits (rsx_diag_wait). The stall thread then prints, per thread,
 * the marked wait and its age, the host RIP and the first return addresses on
 * its stack that fall in the exe's code (exe+offset, resolved offline with the
 * PDB). The target is suspended only for the register read and an 8 KB stack
 * copy: nothing that takes a lock or allocates runs while it is stopped. */
static const char* const s_diag_name[RSX_DIAG_N] = { "walker", "rsx_exec", "rsx_vk_record" };
static struct { HANDLE h; DWORD tid; const char* volatile what; volatile ULONGLONG since; } s_diag[RSX_DIAG_N];
static __declspec(thread) int t_diag_who = -1;
int (*g_rsx_diag_backend)(char* buf, int size);   /* rsx_split.h */

void rsx_diag_thread_here(int who)
{
    if (who < 0 || who >= RSX_DIAG_N || s_diag[who].h) { if (who >= 0 && who < RSX_DIAG_N) t_diag_who = who; return; }
    HANDLE h = NULL;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        s_diag[who].tid = GetCurrentThreadId();
        __atomic_store_n(&s_diag[who].h, h, __ATOMIC_RELEASE);
    }
    t_diag_who = who;
}
void rsx_diag_wait(const char* what)
{
    const int who = t_diag_who;
    if (who < 0) return;
    if (what) s_diag[who].since = GetTickCount64();
    __atomic_store_n(&s_diag[who].what, what, __ATOMIC_RELEASE);
}

static uintptr_t s_exe_lo, s_exe_hi, s_exe_base;
static void diag_exe_range(void)
{
    if (s_exe_hi) return;
    const u8* b = (const u8*)GetModuleHandleA(NULL);
    if (!b) return;
    const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)(const void*)b;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(const void*)(b + d->e_lfanew);
    const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            const uintptr_t a = (uintptr_t)b + sh[i].VirtualAddress;
            if (a < lo) lo = a;
            if (a + sh[i].Misc.VirtualSize > hi) hi = a + sh[i].Misc.VirtualSize;
        }
    s_exe_base = (uintptr_t)b; s_exe_lo = lo; s_exe_hi = hi;
}
static int diag_addr(char* buf, int size, uintptr_t a)
{
    if (a >= s_exe_lo && a < s_exe_hi) return snprintf(buf, (size_t)size, "exe+%llX", (unsigned long long)(a - s_exe_base));
    HMODULE m = NULL; char nm[64] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(m, path, sizeof path)) { const char* s = strrchr(path, '\\'); snprintf(nm, sizeof nm, "%s", s ? s + 1 : path); }
    }
    return snprintf(buf, (size_t)size, "%s+%llX", nm, (unsigned long long)(a - (uintptr_t)m));
}

int rsx_stall_snapshot(char* buf, int size)
{
    if (size < 256) return 0;
    diag_exe_range();
    static uint64_t stk[1024];
    int k = snprintf(buf, (size_t)size, "split: %s owner=%d wl=%llu pub=%llu w=%llu r=%llu rc=%llu label_pos=%llu bsleep=%ld await=%ld;",
                     s_sp.inited ? "on" : "off", s_sp.owner, (unsigned long long)s_sp.wl, (unsigned long long)s_sp.pub,
                     (unsigned long long)s_sp_w, (unsigned long long)s_sp_r, (unsigned long long)s_sp.rc,
                     (unsigned long long)s_sp.label_pos, (long)s_sp_bsleep, (long)s_sp_await);
    if (g_rsx_diag_backend && k < size - 200) k += g_rsx_diag_backend(buf + k, size - k - 100);
    const ULONGLONG now = GetTickCount64();
    for (int who = 0; who < RSX_DIAG_N && k < size - 200; who++) {
        HANDLE h = __atomic_load_n(&s_diag[who].h, __ATOMIC_ACQUIRE);
        if (!h) continue;
        const char* what = __atomic_load_n(&s_diag[who].what, __ATOMIC_ACQUIRE);
        k += snprintf(buf + k, (size_t)(size - k), " %s(tid %lu): wait=%s", s_diag_name[who], (unsigned long)s_diag[who].tid,
                      what ? what : "-");
        if (what) k += snprintf(buf + k, (size_t)(size - k), " for %llu ms", (unsigned long long)(now - s_diag[who].since));
        if (s_diag[who].tid == GetCurrentThreadId()) { k += snprintf(buf + k, (size_t)(size - k), ";"); continue; }
        CONTEXT c; memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        SIZE_T got = 0;
        if (SuspendThread(h) == (DWORD)-1) { k += snprintf(buf + k, (size_t)(size - k), " (no suspend);"); continue; }
        const BOOL ok = GetThreadContext(h, &c);
        if (ok) ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)c.Rsp, stk, sizeof stk, &got);
        ResumeThread(h);
        if (!ok) { k += snprintf(buf + k, (size_t)(size - k), " (no context);"); continue; }
        k += snprintf(buf + k, (size_t)(size - k), " rip=");
        k += diag_addr(buf + k, size - k, (uintptr_t)c.Rip);
        k += snprintf(buf + k, (size_t)(size - k), " stack=");
        int n = 0;
        uintptr_t last = 0;
        for (size_t i = 0; i < got / 8 && n < 8 && k < size - 40; i++) {
            const uintptr_t a = (uintptr_t)stk[i];
            if (a < s_exe_lo || a >= s_exe_hi || a == last) continue;
            last = a;
            k += snprintf(buf + k, (size_t)(size - k), "%s", n ? "<" : "");
            k += diag_addr(buf + k, size - k, a);
            n++;
        }
        k += snprintf(buf + k, (size_t)(size - k), ";");
    }
    return k;
}

/* ---- the walker's side ------------------------------------------------- */

static void sp_publish(void)
{
    if (s_sp.pub == s_sp.wl) return;
    s_sp.pub = s_sp.wl;
    InterlockedExchange64(&s_sp_w, (LONG64)s_sp.wl);    /* full barrier: published before the flag is read */
    if (s_sp_bsleep) { s_sp.n_wake++; WakeByAddressSingle((PVOID)&s_sp_w); }
}

/* Until the exec thread has consumed everything before `target`. */
static void sp_wait_r(u64 target)
{
    u64 r = sp_load_r();
    if (r >= target) { s_sp.rc = r; return; }
    s_sp.n_wait++;
    const u64 t0 = __rdtsc(), spin = s_sp.ticks_us * 20u;
    rsx_diag_wait("sp_wait_r: the split ring (rsx_exec runs the queue)");
    for (;;) {
        r = sp_load_r();
        if (r >= target) break;
        if (__rdtsc() - t0 < spin) { _mm_pause(); continue; }
        InterlockedExchange(&s_sp_await, 1);
        r = sp_load_r();
        if (r < target) {
            LONG64 seen = (LONG64)r;
            WaitOnAddress((volatile VOID*)&s_sp_r, &seen, sizeof seen, 4);
        }
        InterlockedExchange(&s_sp_await, 0);
    }
    rsx_diag_wait(NULL);
    s_sp.wait_ticks += __rdtsc() - t0;
    s_sp.rc = r;
}

/* Room for a record of `bytes` (header included): its header, size filled in. */
static u8* sp_alloc(u32 bytes)
{
    bytes = (bytes + 7u) & ~7u;
    u64 pos = s_sp.wl;
    const u32 off = (u32)(pos & (SP_RING - 1u));
    const u32 fill = off + bytes > SP_RING ? SP_RING - off : 0u;
    const u64 need = pos + fill + bytes;
    if (need - s_sp.rc > SP_RING) {
        s_sp.rc = sp_load_r();
        if (need - s_sp.rc > SP_RING) { s_sp.n_full++; sp_publish(); sp_wait_r(need - SP_RING); }
    }
    if (fill) {                                   /* the rest of the lap: skipped */
        sp_hdr* f = (sp_hdr*)(s_sp.ring + off);
        f->type = SP_WRAP; f->kind = 0; f->size = fill;
        pos += fill;
        s_sp.wl = pos;
    }
    sp_hdr* h = (sp_hdr*)(s_sp.ring + (pos & (SP_RING - 1u)));
    h->size = bytes;
    return (u8*)h;
}
static void sp_commit(const u8* rec)
{
    s_sp.wl += ((const sp_hdr*)rec)->size;
    s_sp.n_rec++;
    if (s_sp.wl - s_sp.pub >= SP_PUB) sp_publish();
}

static inline int sp_blk_same(const u8* a, const u8* b)
{
    u64 x[8], y[8];
    memcpy(x, a, 64); memcpy(y, b, 64);
    return !((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]) |
             (x[4] ^ y[4]) | (x[5] ^ y[5]) | (x[6] ^ y[6]) | (x[7] ^ y[7]));
}

/* The blocks written since the last flush whose bytes the engine's instance
 * does not hold yet. */
static void sp_flush_state(void)
{
    u64 sum = s_sp_dirty[RSX_DSP_DIRTY_WORDS];
    if (!sum) return;
    s_sp_dirty[RSX_DSP_DIRTY_WORDS] = 0;
    u32 nd = 0;
    for (u64 m = sum; m; m &= m - 1u) nd += (u32)__builtin_popcountll(s_sp_dirty[__builtin_ctzll(m)]);
    sp_state* rec = (sp_state*)sp_alloc((u32)(sizeof(sp_state) + (size_t)nd * sizeof(sp_blk)));
    sp_blk* b = (sp_blk*)(rec + 1);
    const u8* cur = (const u8*)s_sp.a->regs;
    u32 n = 0;
    while (sum) {
        const u32 w = (u32)__builtin_ctzll(sum);
        sum &= sum - 1u;
        u64 m = s_sp_dirty[w];
        s_sp_dirty[w] = 0;
        while (m) {
            const u32 id = w * 64u + (u32)__builtin_ctzll(m);
            m &= m - 1u;
            if (id >= RSX_DSP_BLK_COUNT) continue;
            const u8* src = cur + (size_t)id * RSX_DSP_BLK_BYTES;
            u8* have = s_sp.sent + (size_t)id * RSX_DSP_BLK_BYTES;
            if (sp_blk_same(src, have)) { s_sp.n_same++; continue; }
            memcpy(have, src, RSX_DSP_BLK_BYTES);
            b[n].id = id; b[n].pad = 0;
            memcpy(b[n].b, src, RSX_DSP_BLK_BYTES);
            n++;
        }
    }
    if (!n) return;                               /* nothing changed: not committed */
    rec->h.type = SP_STATE; rec->h.kind = 0;
    rec->h.size = (u32)(sizeof(sp_state) + (size_t)n * sizeof(sp_blk));
    rec->n = n; rec->pad = 0;
    s_sp.n_blk += n;
    sp_commit((const u8*)rec);
}

static void sp_event(u32 kind, u32 a, u32 b, const u8* data, u32 bytes)
{
    sp_flush_state();
    sp_event_rec* e = (sp_event_rec*)sp_alloc((u32)sizeof(sp_event_rec) + bytes);
    e->h.type = SP_EVENT; e->h.kind = (u16)kind;
    e->a = a; e->b = b;
    const rsx_dispatch* r = s_sp.a;
    e->s.current_primitive = r->current_primitive;
    e->s.in_begin_end = (u32)r->in_begin_end;
    e->s.pair_packets = r->pair_packets;
    e->s.vp_gen = r->vp_gen;
    e->s.tgen = s_sp.tgen;
    e->s.mgen = s_sp.mgen;
    e->s.const_gen = r->const_gen; e->s.pad = 0;
    if (bytes) memcpy(e + 1, data, bytes);
    sp_commit((const u8*)e);
    if (kind == EV_END || kind == EV_CLEAR || kind == EV_FLIP) sp_publish();   /* one draw's worth at a time */
}

/* The walker's instance's sinks: the engine's, as events. */
static void rec_clear(void* u, const rsx_dispatch* r, u32 mask) { (void)u; (void)r; sp_event(EV_CLEAR, mask, 0, NULL, 0); }
static void rec_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; sp_event(EV_BEGIN, prim, 0, NULL, 0); }
static void rec_end(void* u, const rsx_dispatch* r) { (void)u; (void)r; sp_event(EV_END, 0, 0, NULL, 0); }
static void rec_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count) { (void)u; (void)r; sp_event(EV_ARRAYS, first, count, NULL, 0); }
static void rec_index(void* u, const rsx_dispatch* r, u32 first, u32 count) { (void)u; (void)r; sp_event(EV_INDEX, first, count, NULL, 0); }
static void rec_inline(void* u, const rsx_dispatch* r, const u8* data, u32 bytes)
{
    (void)u; (void)r;
    if (bytes > RSX_DSP_INLINE_MAX_BYTES) bytes = RSX_DSP_INLINE_MAX_BYTES;   /* cannot happen: the dispatcher's cap */
    sp_event(EV_INLINE, bytes, 0, data, bytes);
}
static void rec_flip(void* u, const rsx_dispatch* r, u32 arg) { (void)u; (void)r; sp_event(EV_FLIP, arg, 0, NULL, 0); }
static void rec_limit(int what, u32 n)
{
    sp_act_rec* a = (sp_act_rec*)sp_alloc((u32)sizeof(sp_act_rec));
    a->h.type = SP_LIMIT; a->h.kind = 0;
    a->a = (u32)what; a->b = n; a->c = 0; a->pad = 0;
    sp_commit((const u8*)a);
}

/* ---- the exec thread ---------------------------------------------------- */

static void sp_exec(const u8* p, rsx_dispatch* e)
{
    const sp_hdr* h = (const sp_hdr*)p;
    switch (h->type) {
    case SP_WRAP: break;
    case SP_STATE: {
        const sp_state* r = (const sp_state*)p;
        const sp_blk* b = (const sp_blk*)(r + 1);
        u8* base = (u8*)e->regs;
        u64* const d = e->dirty;   /* (rnd1) PS3RECOMP_ENG_DELTA: the engine's instance tracks them too */
        for (u32 i = 0; i < r->n; i++) {
            const u32 id = b[i].id;
            memcpy(base + (size_t)id * RSX_DSP_BLK_BYTES, b[i].b, RSX_DSP_BLK_BYTES);
            if (d) { d[id >> 6] |= 1ull << (id & 63u); d[RSX_DSP_DIRTY_WORDS] |= 1ull << (id >> 6); }
        }
        break; }
    case SP_EVENT: {
        const sp_event_rec* r = (const sp_event_rec*)p;
        e->current_primitive = r->s.current_primitive;
        e->in_begin_end = (int)r->s.in_begin_end;
        e->pair_packets = r->s.pair_packets;
        e->vp_gen = r->s.vp_gen;
        e->const_gen = r->s.const_gen;   /* the walker's instance bumped it (constants arrive as blocks) */
        g_rsx_dsp_tex_gen = r->s.tgen;
        g_rsx_dsp_misc_gen = r->s.mgen;
        const rsx_dispatch_sink* k = &e->sink;
        switch (h->kind) {
        case EV_CLEAR:  if (k->clear) k->clear(k->user, e, r->a); break;
        case EV_BEGIN:  if (k->begin) k->begin(k->user, e, r->a); break;
        case EV_END:    if (k->end) k->end(k->user, e); break;
        case EV_ARRAYS: if (k->draw_arrays) k->draw_arrays(k->user, e, r->a, r->b); break;
        case EV_INDEX:  if (k->draw_index_array) k->draw_index_array(k->user, e, r->a, r->b); break;
        case EV_INLINE: {
            /* where the dispatcher keeps it: the engine's sink holds the pointer until END */
            const u32 n = r->a <= RSX_DSP_INLINE_MAX_BYTES ? r->a : RSX_DSP_INLINE_MAX_BYTES;
            memcpy(e->inline_data, r + 1, n);
            if (k->inline_array) k->inline_array(k->user, e, e->inline_data, n);
            break; }
        case EV_FLIP:   if (k->flip) k->flip(k->user, e, r->a); break;
        default: break;
        }
        break; }
    case SP_ACT: {
        const sp_act_rec* r = (const sp_act_rec*)p;
        gcm_split_run(h->kind, r->a, r->b, r->c);
        break; }
    case SP_LIMIT: {
        const sp_act_rec* r = (const sp_act_rec*)p;
        if (g_rsx_dsp_limit_hook) g_rsx_dsp_limit_hook((int)r->a, r->b);
        break; }
    default: break;
    }
}

static DWORD WINAPI sp_exec_thread(LPVOID arg)
{
    (void)arg;
    /* the draw engine's thread: the RSX role (PS3_CPU_PLACE / PS3_HIGHQOS / PS3_PLACE_PRIO) */
    ps3_host_place_self_once(PS3_PLACE_RSX);
    rsx_diag_thread_here(RSX_DIAG_EXEC);       /* (rsx-par2) PS3_STALL_MS sees this thread */
    rsx_dispatch* e = rsx_draw_engine_dsp();
    /* the walker writes s_sp all the time: what this thread needs of it, once */
    const u8* const ring = s_sp.ring;
    u64 r = (u64)s_sp_r;
    const u64 spin = s_sp.ticks_us * 30u;
    for (;;) {
        u64 w = (u64)__atomic_load_n(&s_sp_w, __ATOMIC_ACQUIRE);
        if (w == r) {                             /* idle: spin a little, then sleep until published */
            const u64 t0 = __rdtsc();
            while (w == r && __rdtsc() - t0 < spin) {
                _mm_pause();
                w = (u64)__atomic_load_n(&s_sp_w, __ATOMIC_ACQUIRE);
            }
            if (w == r) {
                rsx_diag_wait("idle: no records");
                InterlockedExchange(&s_sp_bsleep, 1);
                w = (u64)__atomic_load_n(&s_sp_w, __ATOMIC_ACQUIRE);
                if (w == r) {
                    LONG64 seen = (LONG64)r;
                    WaitOnAddress((volatile VOID*)&s_sp_w, &seen, sizeof seen, 100);
                }
                InterlockedExchange(&s_sp_bsleep, 0);
                rsx_diag_wait(NULL);
                continue;
            }
        }
        while (r != w) {
            const u8* p = ring + (r & (SP_RING - 1u));
            const sp_hdr h = *(const sp_hdr*)p;
            sp_exec(p, e);
            r += h.size;
            if (h.type == SP_EVENT && h.kind == EV_END) {   /* a draw done: the walker may be waiting on it */
                __atomic_store_n(&s_sp_r, (LONG64)r, __ATOMIC_RELEASE);
                if (s_sp_await) WakeByAddressSingle((PVOID)&s_sp_r);
            }
        }
        InterlockedExchange64(&s_sp_r, (LONG64)r);   /* full barrier: before the walker's flag is read */
        if (s_sp_await) WakeByAddressSingle((PVOID)&s_sp_r);
    }
    return 0;
}

/* ---- ownership ---------------------------------------------------------- */

/* dst's whole dispatcher state from src, keeping dst's own sinks and hooks */
static void sp_copy_state(rsx_dispatch* dst, const rsx_dispatch* src)
{
    const rsx_dispatch_sink sk = dst->sink;
    u64* const d = dst->dirty;
    u32* const tg = dst->tgen;
    u32* const mg = dst->mgen;
    void (*const lh)(int, u32) = dst->limit_hook;
    memcpy(dst, src, sizeof *dst);
    dst->sink = sk; dst->dirty = d; dst->tgen = tg; dst->mgen = mg; dst->limit_hook = lh;
}

/* the engine's instance -> the walker's (the exec thread is idle) */
static void sp_take(void)
{
    const rsx_dispatch* e = rsx_draw_engine_dsp();
    sp_copy_state(s_sp.a, e);
    memcpy(s_sp.sent, e->regs, (size_t)RSX_DSP_BLK_COUNT * RSX_DSP_BLK_BYTES);
    memset(s_sp_dirty, 0, sizeof s_sp_dirty);
    /* past every value the engine has cached: its first split draw re-derives */
    s_sp.tgen = g_rsx_dsp_tex_gen + 1u;
    s_sp.mgen = g_rsx_dsp_misc_gen + 1u;
    s_sp.owner = 1;
}

/* the walker's instance -> the engine's (everything queued has run) */
static void sp_give(void)
{
    rsx_dispatch* e = rsx_draw_engine_dsp();
    sp_copy_state(e, s_sp.a);
    if (e->dirty) {   /* (rnd1) PS3RECOMP_ENG_DELTA: every block may have changed */
        for (u32 w = 0; w < RSX_DSP_DIRTY_WORDS; w++) e->dirty[w] = ~0ull;
        e->dirty[RSX_DSP_DIRTY_WORDS] = (1ull << RSX_DSP_DIRTY_WORDS) - 1u;
    }
    g_rsx_dsp_tex_gen = s_sp.tgen + 1u;
    g_rsx_dsp_misc_gen = s_sp.mgen + 1u;
    s_sp.owner = 0;
}

static int sp_init(void)
{
    s_sp.a = (rsx_dispatch*)calloc(1, sizeof(rsx_dispatch));
    s_sp.sent = (u8*)malloc((size_t)RSX_DSP_BLK_COUNT * RSX_DSP_BLK_BYTES);
    s_sp.ring = (u8*)VirtualAlloc(NULL, SP_RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_sp.a || !s_sp.sent || !s_sp.ring) goto fail;
    {   /* the walker's instance: the engine's sinks as events (only those the engine has) */
        const rsx_dispatch* e = rsx_draw_engine_dsp();
        rsx_dispatch_sink sk;
        memset(&sk, 0, sizeof sk);
        if (e->sink.clear)            sk.clear = rec_clear;
        if (e->sink.begin)            sk.begin = rec_begin;
        if (e->sink.end)              sk.end = rec_end;
        if (e->sink.draw_arrays)      sk.draw_arrays = rec_arrays;
        if (e->sink.draw_index_array) sk.draw_index_array = rec_index;
        if (e->sink.inline_array)     sk.inline_array = rec_inline;
        if (e->sink.flip)             sk.flip = rec_flip;
        rsx_dispatch_init(s_sp.a, &sk);
        s_sp.a->dirty = s_sp_dirty;
        s_sp.a->tgen = &s_sp.tgen;
        s_sp.a->mgen = &s_sp.mgen;
        s_sp.a->limit_hook = rec_limit;
    }
    {   /* rdtsc ticks per microsecond, over 2 ms */
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        const u64 c0 = __rdtsc();
        do { QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 500 < f.QuadPart);
        const double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
        s_sp.ticks_us = (u64)((double)(__rdtsc() - c0) / us);
        if (!s_sp.ticks_us) s_sp.ticks_us = 3000;
    }
    s_sp_w = s_sp_r = 0;
    s_sp.wl = s_sp.rc = s_sp.pub = s_sp.label_pos = 0;
    s_sp.thread = CreateThread(NULL, 0, sp_exec_thread, NULL, 0, NULL);
    if (!s_sp.thread) goto fail;
    SetThreadPriority(s_sp.thread, GetThreadPriority(GetCurrentThread()));
    { typedef HRESULT (WINAPI *set_desc_fn)(HANDLE, PCWSTR);
      set_desc_fn sd = (set_desc_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
      if (sd) sd(s_sp.thread, L"rsx_exec"); }
    s_sp.inited = 1;
    fprintf(stderr, "[rsx split] draw engine on its own thread (hosttid %lu), FIFO walker + dispatch on hosttid %lu, %u MB ring "
                    "(PS3RECOMP_RSX_SPLIT=0 = one thread)\n",
            (unsigned long)GetThreadId(s_sp.thread), (unsigned long)GetCurrentThreadId(), SP_RING >> 20);
    return 1;
fail:
    fprintf(stderr, "[rsx split] could not start (memory or thread): one thread, as before\n");
    free(s_sp.a); free(s_sp.sent);
    if (s_sp.ring) VirtualFree(s_sp.ring, 0, MEM_RELEASE);
    s_sp.a = NULL; s_sp.sent = NULL; s_sp.ring = NULL;
    s_sp.failed = 1;
    return 0;
}

/* PS3RECOMP_RSX_SPLIT_STATS=1: every ~5 s, the walker's side of the ring. */
static void sp_stats(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PS3RECOMP_RSX_SPLIT_STATS") ? 1 : 0;
    if (!on) return;
    const unsigned long long now = GetTickCount64();
    if (!s_sp.t_report) { s_sp.t_report = now; return; }
    if (now - s_sp.t_report < 5000) return;
    const double sec = (double)(now - s_sp.t_report) / 1000.0;
    static u64 wl0;
    fprintf(stderr, "[rsx split] per s: %.0f drains, %.0f records, %.1f MB, %.0f blocks sent (%.0f unchanged), "
                    "walker waits %.0f (%.2f ms), ring full %.0f, wakes %.0f\n",
            s_sp.n_drain / sec, s_sp.n_rec / sec, (double)(s_sp.wl - wl0) / 1e6 / sec, s_sp.n_blk / sec, s_sp.n_same / sec,
            s_sp.n_wait / sec, (double)s_sp.wait_ticks / (double)s_sp.ticks_us / 1000.0 / sec,
            s_sp.n_full / sec, s_sp.n_wake / sec);
    s_sp.n_drain = s_sp.n_rec = s_sp.n_blk = s_sp.n_same = s_sp.n_wait = s_sp.wait_ticks = s_sp.n_full = s_sp.n_wake = 0;
    s_sp.t_report = now;
    wl0 = s_sp.wl;
}

/* ---- the walker's API ---------------------------------------------------- */

int rsx_split_drain_begin(void)
{
    if (!sp_switch() || s_sp.failed || !rsx_draw_engine_ready()) { rsx_split_release(); return 0; }
    if (!s_sp.inited && !sp_init()) return 0;
    if (!s_sp.owner) sp_take();
    s_sp.n_drain++;
    sp_stats();
    return 1;
}

void rsx_split_release(void)
{
    if (!s_sp.owner) return;
    rsx_split_sync();
    sp_give();
}

void rsx_split_burst(u32 method, int incr, const u32* be_words, u32 count)
{
    rsx_dispatch_burst(s_sp.a, method & 0x1FFCu, incr, be_words, count);
}

void rsx_split_method(u32 method, u32 arg)
{
    /* as rsx_draw_engine_method masks it */
    const u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(s_sp.a, m, arg);
}

void rsx_split_act(u32 kind, u32 a, u32 b, u32 c)
{
    sp_flush_state();                 /* the engine's state is the walker's when the action runs */
    sp_act_rec* r = (sp_act_rec*)sp_alloc((u32)sizeof(sp_act_rec));
    r->h.type = SP_ACT; r->h.kind = (u16)kind;
    r->a = a; r->b = b; r->c = c; r->pad = 0;
    sp_commit((const u8*)r);
    s_sp.label_pos = s_sp.wl;
}

void rsx_split_wait_labels(void)
{
    if (s_sp.rc >= s_sp.label_pos) return;
    sp_publish();
    sp_wait_r(s_sp.label_pos);
}

void rsx_split_sync(void)
{
    if (!s_sp.inited) return;
    sp_flush_state();
    sp_publish();
    sp_wait_r(s_sp.wl);
}

#else   /* not Windows: one thread, as before */
int (*g_rsx_diag_backend)(char* buf, int size);
void rsx_diag_thread_here(int who) { (void)who; }
void rsx_diag_wait(const char* what) { (void)what; }
int  rsx_stall_snapshot(char* buf, int size) { (void)buf; (void)size; return 0; }
int  rsx_split_drain_begin(void) { return 0; }
void rsx_split_release(void) {}
void rsx_split_burst(u32 method, int incr, const u32* be_words, u32 count) { (void)method; (void)incr; (void)be_words; (void)count; }
void rsx_split_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_split_act(u32 kind, u32 a, u32 b, u32 c) { (void)kind; (void)a; (void)b; (void)c; }
void rsx_split_wait_labels(void) {}
void rsx_split_sync(void) {}
#endif
