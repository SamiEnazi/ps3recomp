/* ps3_hitchcatch.c -- PS3RECOMP_HITCHCATCH, the frame-hitch stack catcher.
 * See include/ps3emu/ps3_hitchcatch.h for the switches; tools/hitch_stacks.py
 * reads what it writes.
 *
 * How a sample is taken, and why it cannot hang the game:
 *   1. QueryThreadCycleTime: a thread whose cycle count did not move since the
 *      last pass did not run, so its stack is the one already recorded ('w').
 *      No suspension for it (most guest threads sleep through a freeze). One
 *      exception, measured: suspending a sleeping thread makes the kernel run
 *      the suspend on that thread, which moves its own count. So in the pass
 *      right after it was sampled, a move up to twice what one suspend costs
 *      (CAL line, measured at start on a sleeping helper) is that cost, not a
 *      run. A sleeping thread is re-sampled every 16 passes anyway.
 *   2. SuspendThread, GetThreadContext, memcpy of the top of its stack (at
 *      most 128 KB, never below the TEB's StackLimit, so never a guard page)
 *      and, for an SPU worker, four ints of its spu_context; ResumeThread.
 *      Nothing else runs while it is stopped: no lock, no allocation, no
 *      loader or heap call, no unwind-table lookup (RtlLookupFunctionEntry
 *      may take ntdll's function-table lock, which the stopped thread could
 *      hold). One target at a time; the sampler runs at HIGHEST priority while
 *      sampling so nothing preempts it inside that window.
 *   3. With the thread running again: the copy's pointers into the original
 *      stack are rebased onto the copy, and RtlVirtualUnwind walks it
 *      (SEH-guarded, bounded to the copy). Raw addresses only; modules and
 *      their offsets are written out, names are found offline.
 * The watchdog thread is never registered and skips its own id. Buffers are
 * allocated once, when the play log opens; a frame that outgrows them is cut
 * (and marked so), it never allocates.
 *
 * hitches.tsv lines (tab separated):
 *   #hitchcatch v1 exe= pdb= pdbguid= pdbage= pid= start_ms= keep_ms= period_us= depth= max= buf_mb=
 *   CAL    tsc_hz= suspend_cycles_max= ran_threshold_after_sample=
 *   MOD    <mod> <base hex> <size hex> <path>
 *   THREAD <serial> <PPU|SPU|RSX> <id> <host tid> <name>
 *   HITCH  <n> <start_s> <end_s> <dur_ms> <first_ms> <passes> <cut>
 *            start_s/end_s on the play log's clock: end_s = its HITCH line's t
 *   N      <n> <node> <parent> <mod>:<rva hex>        call-tree node (parent 0 = root)
 *   S      <n> <t_ms> <serial> <st> <node> <rep> <extra>
 *            st: r ran since the last pass, q first sample (unknown), w did not run
 *            (stack = its previous one), x could not be sampled; node = the leaf's node;
 *            rep = that many consecutive passes alike; extra: ovl=,img=,pc=,pm= (SPU) or hle= (PPU)
 *   TCPU   <n> <serial> <cpu_ms> <passes run> <passes seen>
 *   END    <n> <passes> <samples> <waits> <susp_max_us> <susp_avg_us> <pass_avg_us> <sampler_ms> <write_ms> <nodes>
 */
#include "ps3emu/ps3_hitchcatch.h"

#ifndef _WIN32

void ps3_hc_thread_begin(int kind, unsigned id, const char* name, const ps3_hc_spu_view* spu)
{ (void)kind; (void)id; (void)name; (void)spu; }
void ps3_hc_thread_end(void) {}
void ps3_hc_set_hle_table(const char* const volatile* table, unsigned n) { (void)table; (void)n; }
void ps3_hc_session(const char* playlog_path, double t0) { (void)playlog_path; (void)t0; }
void ps3_hc_frame(double now) { (void)now; }
int  ps3_hc_get_stats(ps3_hc_stats* out) { (void)out; return 0; }

#else

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define HC_SLOTS     256
#define HC_MAXDEPTH  32
#define HC_COPY_CAP  (128u * 1024u)       /* stack bytes copied per sample */
#define HC_COPY_PAD  (64u * 1024u)        /* zeroes after it: an unwind that runs past reads 0 */
#define HC_NODES_LOG2 19                  /* call-tree nodes per hitch (512k) */
#define HC_MAX_SAMPLE_S 8.0               /* sampling stops after this long in one frame */
#define HC_FORCE_REAL 16                  /* a sleeping thread is re-sampled every 16 passes */

/* ---- configuration (read once) ---------------------------------------- */
static volatile LONG s_cfg;               /* 0 unread, 1 reading, 2 read */
static int      s_on;
static double   s_start_s = 0.040, s_keep_s = 0.080;
static unsigned s_max_hitches = 200, s_period_us = 1000, s_depth = 24;
static size_t   s_buf_bytes = 16u << 20;
static const uint64_t s_file_cap = 256ull << 20;

static unsigned hc_env_u(const char* name, unsigned def, unsigned lo, unsigned hi)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    const unsigned long v = strtoul(e, NULL, 10);
    return v < lo ? lo : v > hi ? hi : (unsigned)v;
}

static void hc_config(void)
{
    if (InterlockedCompareExchange(&s_cfg, 1, 0) != 0) {
        while (s_cfg != 2) YieldProcessor();
        return;
    }
    const char* pl = getenv("PS3RECOMP_PLAYLOG");
    const char* hc = getenv("PS3RECOMP_HITCHCATCH");
    s_on = pl && *pl && !(hc && hc[0] == '0');
    const char* ms = getenv("PS3RECOMP_HITCHCATCH_MS");
    if (ms && *ms) {
        char* end = NULL;
        const double a = strtod(ms, &end);
        if (a > 0.0) s_start_s = a / 1000.0;
        if (end && *end == ',') { const double b = strtod(end + 1, NULL); if (b > 0.0) s_keep_s = b / 1000.0; }
        if (s_keep_s < s_start_s) s_keep_s = s_start_s;
    }
    s_max_hitches = hc_env_u("PS3RECOMP_HITCHCATCH_MAX", 200, 1, 100000);
    s_buf_bytes   = (size_t)hc_env_u("PS3RECOMP_HITCHCATCH_MB", 16, 1, 1024) << 20;
    s_period_us   = hc_env_u("PS3RECOMP_HITCHCATCH_PERIOD_US", 1000, 100, 100000);
    s_depth       = hc_env_u("PS3RECOMP_HITCHCATCH_DEPTH", 24, 1, HC_MAXDEPTH);
    InterlockedExchange(&s_cfg, 2);
}
static int hc_on(void) { if (s_cfg != 2) hc_config(); return s_on; }

/* ---- the clock: the play log's (QueryPerformanceCounter seconds) ------- */
static double s_qpf;
static double hc_now(void)
{
    LARGE_INTEGER c;
    if (s_qpf == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qpf = (double)f.QuadPart; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / s_qpf;
}
static uint64_t hc_ns(void) { return (uint64_t)(hc_now() * 1e9); }
static uint64_t hc_d2u(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }
static double   hc_u2d(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }

/* ---- registered threads ------------------------------------------------ */
enum { HC_FREE = 0, HC_CLAIM = 1, HC_LIVE = 2, HC_DEAD = 3 };
typedef struct hc_slot {
    volatile LONG   state;
    DWORD           tid;
    HANDLE          h;            /* our own handle: only the watchdog closes it */
    ULONG_PTR       stk_lo, stk_hi;
    const NT_TIB*   tib;          /* the thread's TEB: StackLimit at sample time */
    int             kind;
    unsigned        id;
    LONG            serial;       /* one per registration */
    ps3_hc_spu_view spu;
    char            name[48];
} hc_slot;
static hc_slot s_slot[HC_SLOTS];
static volatile LONG s_serial;
static __declspec(thread) int t_slot;   /* this thread's slot + 1 */

/* The watchdog's own per-slot state (never written by other threads). */
typedef struct hc_ss {
    LONG     serial;
    unsigned gen;                 /* the frame it belongs to */
    int      have_cyc;
    ULONG64  cyc_first, cyc_last;
    unsigned last_rec;            /* offset + 1 of its last real record */
    unsigned since_real, ran, seen;
    int      perturbed;           /* sampled in the last pass: its count holds our suspend's cost */
    LONG     written;             /* serial whose THREAD line is in the file */
    /* the run-length line being built (commit) */
    int      pend;
    double   p_t;
    char     p_st;
    unsigned p_node, p_rep;
    char     p_extra[96];
} hc_ss;
static hc_ss s_ss[HC_SLOTS];

static const char* const volatile* s_hle_tbl;
static unsigned s_hle_n;

/* ---- frames (written by the presenting thread) ------------------------- */
static volatile int  s_session;
static double        s_t0;
static uint64_t      s_frame_bits;         /* the current frame's start (double bits) */
static double        s_prev_frame;         /* presenting thread only */
static struct { uint64_t start, end; } s_long[16];   /* frames over the start threshold */
static volatile LONG s_long_n;
static volatile LONG s_rsx_reg;

/* ---- the watchdog ------------------------------------------------------ */
static volatile LONG s_thread_started;
static DWORD    s_self_tid;
static HANDLE   s_self_h;
static HANDLE   s_timer;
static double   s_tsc_hz;
static ULONG64  s_apc_cyc;                  /* what one suspend+resume adds to a sleeping thread's cycles (x2) */
static ULONG64  s_apc_max;
static uint8_t* s_buf;                      /* sample records */
static size_t   s_cur;
static uint8_t* s_copy;                     /* one stack copy */
static unsigned s_gen;
static CONTEXT  s_ctx;                      /* DECLSPEC_ALIGN(16) in winnt.h */
static CONTEXT  s_wctx;
static HANDLE   s_file = INVALID_HANDLE_VALUE;
static unsigned s_kept;
static ps3_hc_stats s_st;
/* this frame's counters */
static unsigned s_f_passes, s_f_samples, s_f_waits;
static uint64_t s_f_susp_max, s_f_susp_sum, s_f_pass_sum;
static int      s_f_cut;
static ULONG64  s_f_cyc0;                   /* the watchdog's own cycles when this frame's sampling began */

typedef struct hc_rec {
    uint32_t    t_us;     /* since the frame's start */
    uint16_t    slot;
    uint8_t     st;       /* 'r' 'q' 'w' 'x' */
    uint8_t     depth;    /* pcs that follow, leaf first */
    uint32_t    ref;      /* 'w' without a stack: offset + 1 of the slot's last real record */
    uint32_t    node;     /* filled in at commit */
    int32_t     ovl, img, pm;
    uint32_t    spc;
    const char* hle;
} hc_rec;

#define HC_ALIGN8(x) (((x) + 7u) & ~(size_t)7u)

/* ---- output ------------------------------------------------------------ */
static char   s_ob[1 << 16];
static size_t s_obn;
static void hc_flush(void)
{
    if (s_obn && s_file != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(s_file, s_ob, (DWORD)s_obn, &w, NULL);
        s_st.bytes_written += w;
    }
    s_obn = 0;
}
static void hc_out(const char* fmt, ...)
{
    if (s_obn > sizeof s_ob - 1024) hc_flush();
    va_list ap; va_start(ap, fmt);
    const int k = vsnprintf(s_ob + s_obn, sizeof s_ob - s_obn, fmt, ap);
    va_end(ap);
    if (k > 0) s_obn += (size_t)k < sizeof s_ob - s_obn ? (size_t)k : sizeof s_ob - s_obn - 1;
}
static void hc_utf8(const WCHAR* w, char* out, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) out[0] = 0;
    for (char* p = out; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
}

/* ---- modules (commit time, nothing suspended) -------------------------- */
#define HC_MODS 512
static struct { uintptr_t base; uint32_t size; int idx; } s_mod[HC_MODS];
static int s_nmod, s_nmod_idx;
static void hc_modules(void)
{
    HMODULE hm[HC_MODS];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &need)) return;
    const int n = (int)(need / sizeof(HMODULE)) < HC_MODS ? (int)(need / sizeof(HMODULE)) : HC_MODS;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi;
        if (!K32GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof mi)) continue;
        const uintptr_t b = (uintptr_t)mi.lpBaseOfDll;
        int j = 0;
        while (j < s_nmod && !(s_mod[j].base == b && s_mod[j].size == mi.SizeOfImage)) j++;
        if (j < s_nmod || s_nmod >= HC_MODS) continue;
        /* keep the table sorted by base */
        int at = s_nmod;
        while (at > 0 && s_mod[at - 1].base > b) { s_mod[at] = s_mod[at - 1]; at--; }
        s_mod[at].base = b; s_mod[at].size = mi.SizeOfImage; s_mod[at].idx = s_nmod_idx++;
        s_nmod++;
        WCHAR wp[MAX_PATH]; char p[MAX_PATH * 3];
        if (!GetModuleFileNameW(hm[i], wp, MAX_PATH)) wp[0] = 0;
        hc_utf8(wp, p, sizeof p);
        hc_out("MOD\t%d\t%llx\t%x\t%s\n", s_mod[at].idx, (unsigned long long)b, (unsigned)mi.SizeOfImage, p);
    }
}
static int hc_mod_of(uint64_t pc, uint32_t* rva)
{
    int lo = 0, hi = s_nmod - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (pc < s_mod[m].base) hi = m - 1;
        else if (pc >= s_mod[m].base + s_mod[m].size) lo = m + 1;
        else { *rva = (uint32_t)(pc - s_mod[m].base); return s_mod[m].idx; }
    }
    return -1;
}

/* ---- call-tree nodes (per hitch) ---------------------------------------- */
typedef struct { uint64_t pc; uint32_t parent, id, gen, pad; } hc_node_t;
static hc_node_t* s_nodes;
static uint32_t s_nnodes, s_node_gen;
static int      s_nodes_full;
static uint32_t hc_node(uint32_t parent, uint64_t pc)
{
    const uint32_t mask = (1u << HC_NODES_LOG2) - 1u;
    uint64_t hsh = (pc ^ ((uint64_t)parent << 40) ^ ((uint64_t)parent * 0x9E3779B97F4A7C15ull)) * 0xff51afd7ed558ccdull;
    uint32_t i = (uint32_t)(hsh >> 40) & mask;
    for (;;) {
        hc_node_t* e = &s_nodes[i];
        if (e->gen != s_node_gen) {
            if (s_nnodes >= (mask + 1u) / 4u * 3u) { s_nodes_full = 1; return 0; }
            e->gen = s_node_gen; e->pc = pc; e->parent = parent; e->id = ++s_nnodes;
            uint32_t rva = 0;
            const int m = hc_mod_of(pc, &rva);
            if (m >= 0) hc_out("N\t%u\t%u\t%u\t%d:%x\n", s_kept, e->id, parent, m, rva);
            else        hc_out("N\t%u\t%u\t%u\t?:%llx\n", s_kept, e->id, parent, (unsigned long long)pc);
            return e->id;
        }
        if (e->pc == pc && e->parent == parent) return e->id;
        i = (i + 1u) & mask;
    }
}

/* ---- the PE's own CodeView record (which PDB names these addresses) ----- */
static void hc_pdb_info(char* path, int n, char* guid, int gn, unsigned* age)
{
    path[0] = 0; guid[0] = 0; *age = 0;
    const uint8_t* b = (const uint8_t*)GetModuleHandleW(NULL);
    if (!b) return;
    const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)b;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + d->e_lfanew);
    const IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const IMAGE_DEBUG_DIRECTORY* e = (const IMAGE_DEBUG_DIRECTORY*)(b + dd.VirtualAddress);
    for (DWORD i = 0; dd.VirtualAddress && i < dd.Size / sizeof *e; i++) {
        if (e[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !e[i].AddressOfRawData) continue;
        const uint8_t* cv = b + e[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4)) continue;
        const GUID* g = (const GUID*)(cv + 4);
        snprintf(guid, (size_t)gn, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
                 (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
                 g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
        memcpy(age, cv + 20, 4);
        snprintf(path, (size_t)n, "%s", (const char*)(cv + 24));
        for (char* p = path; *p; p++) if (*p == '\t' || *p == '\n') *p = ' ';
        return;
    }
}

/* ---- registration -------------------------------------------------------- */
static DWORD WINAPI hc_main(LPVOID p);
static void hc_start_thread(void)
{
    if (InterlockedCompareExchange(&s_thread_started, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 256u * 1024u, hc_main, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

void ps3_hc_thread_begin(int kind, unsigned id, const char* name, const ps3_hc_spu_view* spu)
{
    if (!hc_on() || t_slot) return;
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                         FALSE, 0))
        return;
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR lo = 0;
    if (VirtualQuery((LPCVOID)&tib, &mbi, sizeof mbi)) lo = (ULONG_PTR)mbi.AllocationBase;   /* the stack's reservation */
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        if (InterlockedCompareExchange(&s->state, HC_CLAIM, HC_FREE) != HC_FREE) continue;
        s->tid = GetCurrentThreadId();
        s->h = h;
        s->tib = tib;
        s->stk_hi = (ULONG_PTR)tib->StackBase;
        s->stk_lo = lo ? lo : (ULONG_PTR)tib->StackLimit;
        s->kind = kind;
        s->id = id;
        s->serial = InterlockedIncrement(&s_serial);
        memset(&s->spu, 0, sizeof s->spu);
        if (spu) s->spu = *spu;
        snprintf(s->name, sizeof s->name, "%s", name ? name : "");
        for (char* q = s->name; *q; q++) if (*q == '\t' || *q == '\n') *q = ' ';
        InterlockedExchange(&s->state, HC_LIVE);
        t_slot = i + 1;
        hc_start_thread();
        return;
    }
    CloseHandle(h);   /* the table is full: this thread goes unsampled */
}

void ps3_hc_thread_end(void)
{
    const int i = t_slot - 1;
    if (i < 0) return;
    t_slot = 0;
    InterlockedExchange(&s_slot[i].state, HC_DEAD);   /* the watchdog closes the handle */
}

void ps3_hc_set_hle_table(const char* const volatile* table, unsigned n)
{
    s_hle_n = 0;
    s_hle_tbl = table;
    s_hle_n = table ? n : 0;
}

/* ---- session and frames ---------------------------------------------------- */
void ps3_hc_session(const char* playlog_path, double t0)
{
    (void)playlog_path;
    if (!hc_on() || s_session) return;
    WCHAR path[MAX_PATH + 32];
    DWORD n = GetEnvironmentVariableW(L"PS3RECOMP_HITCHCATCH_FILE", path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        n = GetEnvironmentVariableW(L"PS3RECOMP_PLAYLOG", path, MAX_PATH);
        if (!n || n >= MAX_PATH) { s_on = 0; return; }
        WCHAR* cut = NULL;
        for (WCHAR* q = path; *q; q++) if (*q == L'\\' || *q == L'/') cut = q;
        if (cut) cut[1] = 0; else path[0] = 0;
        wcscat_s(path, sizeof path / sizeof path[0], L"hitches.tsv");
    }
    s_buf   = (uint8_t*)VirtualAlloc(NULL, s_buf_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_copy  = (uint8_t*)VirtualAlloc(NULL, HC_COPY_CAP + HC_COPY_PAD, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_nodes = (hc_node_t*)VirtualAlloc(NULL, sizeof(hc_node_t) << HC_NODES_LOG2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_file  = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (!s_buf || !s_copy || !s_nodes || s_file == INVALID_HANDLE_VALUE) {
        if (s_file != INVALID_HANDLE_VALUE) CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
        s_on = 0;
        return;
    }
    s_t0 = t0;
    WCHAR wexe[MAX_PATH]; char exe[MAX_PATH * 3], pdb[MAX_PATH], guid[40];
    unsigned age = 0;
    if (!GetModuleFileNameW(NULL, wexe, MAX_PATH)) wexe[0] = 0;
    hc_utf8(wexe, exe, sizeof exe);
    hc_pdb_info(pdb, sizeof pdb, guid, sizeof guid, &age);
    hc_out("#hitchcatch\tv1\texe=%s\tpdb=%s\tpdbguid=%s\tpdbage=%u\tpid=%lu\tstart_ms=%.1f\tkeep_ms=%.1f"
           "\tperiod_us=%u\tdepth=%u\tmax=%u\tbuf_mb=%u\n",
           exe, pdb, guid, age, (unsigned long)GetCurrentProcessId(), s_start_s * 1000.0, s_keep_s * 1000.0,
           s_period_us, s_depth, s_max_hitches, (unsigned)(s_buf_bytes >> 20));
    hc_flush();
    __atomic_store_n(&s_session, 1, __ATOMIC_RELEASE);
    hc_start_thread();
}

void ps3_hc_frame(double now)
{
    if (!__atomic_load_n(&s_session, __ATOMIC_ACQUIRE)) return;
    if (!s_rsx_reg) { s_rsx_reg = 1; ps3_hc_thread_begin(PS3_HC_RSX, 0, "present", NULL); }
    const double prev = s_prev_frame;
    s_prev_frame = now;
    if (prev > 0.0 && now - prev >= s_start_s) {   /* exact end of a long frame for the watchdog */
        const LONG k = s_long_n & 15;
        __atomic_store_n(&s_long[k].start, hc_d2u(prev), __ATOMIC_RELAXED);
        __atomic_store_n(&s_long[k].end, hc_d2u(now), __ATOMIC_RELAXED);
        InterlockedIncrement(&s_long_n);
    }
    __atomic_store_n(&s_frame_bits, hc_d2u(now), __ATOMIC_RELEASE);
}

static double hc_frame_start(void) { return hc_u2d(__atomic_load_n(&s_frame_bits, __ATOMIC_ACQUIRE)); }
static double hc_frame_end(double start)
{
    const uint64_t sb = hc_d2u(start);
    for (int k = 0; k < 16; k++)
        if (__atomic_load_n(&s_long[k].start, __ATOMIC_ACQUIRE) == sb)
            return hc_u2d(__atomic_load_n(&s_long[k].end, __ATOMIC_ACQUIRE));
    return hc_frame_start();
}

int ps3_hc_get_stats(ps3_hc_stats* out)
{
    if (!hc_on()) return 0;
    *out = s_st;
    if (s_self_h) { ULONG64 c = 0; if (QueryThreadCycleTime(s_self_h, &c)) out->sampler_cycles = c; }
    int live = 0;
    for (int i = 0; i < HC_SLOTS; i++) live += s_slot[i].state == HC_LIVE;
    out->threads_live = live;
    return 1;
}

/* ---- the watchdog ----------------------------------------------------------- */
static void hc_wait_us(double us)
{
    if (us < 50.0) us = 50.0;
    if (s_timer) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(us * 10.0);
        if (SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(s_timer, INFINITE); return; }
    }
    Sleep((DWORD)(us / 1000.0) ? (DWORD)(us / 1000.0) : 1);
}

static void hc_reap(void)
{
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        LONG st = s->state;
        if (st == HC_LIVE && WaitForSingleObject(s->h, 0) == WAIT_OBJECT_0)   /* ended without saying so */
            st = InterlockedCompareExchange(&s->state, HC_DEAD, HC_LIVE) == HC_LIVE ? HC_DEAD : s->state;
        if (st != HC_DEAD) continue;
        CloseHandle(s->h);
        s->h = NULL;
        InterlockedExchange(&s->state, HC_FREE);
    }
}

/* Unwind a stack copy [lo, hi). c->Rsp and the copy's own pointers are already
 * on the copy. A caller is kept only when its return-address slot (Rsp - 8
 * after the unwind) lies inside the copy. */
static int hc_walk(CONTEXT* c, uintptr_t lo, uintptr_t hi, uint64_t* out, int max)
{
    int n = 0;
    __try {
        while (n < max) {
            const DWORD64 pc = c->Rip, sp = c->Rsp;
            if (!pc) break;
            out[n++] = pc;
            if (sp < lo || sp + 8 > hi) break;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL);
            if (f) {
                PVOID hd = NULL; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, c, &hd, &est, NULL);
            } else {
                if (n > 1) break;          /* only the leaf may be a frameless function */
                c->Rip = *(const DWORD64*)(uintptr_t)sp;
                c->Rsp = sp + 8;
            }
            if (c->Rsp < lo + 8 || c->Rsp > hi || (c->Rsp <= sp && c->Rip == pc)) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

/* One thread, stopped for the context + stack copy only. Returns the depth, -1 = not sampled. */
static int hc_sample(hc_slot* s, hc_rec* r, uint64_t* pcs)
{
    if (SuspendThread(s->h) == (DWORD)-1) return -1;
    const uint64_t t0 = hc_ns();
    s_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    size_t len = 0;
    uintptr_t rsp = 0;
    int live = 0;
    if (GetThreadContext(s->h, &s_ctx) && __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == HC_LIVE) {
        live = 1;
        rsp = (uintptr_t)s_ctx.Rsp;
        __try {
            if (s->spu.ovl)    r->ovl = *s->spu.ovl;
            if (s->spu.img)    r->img = *s->spu.img;
            if (s->spu.pc)     r->spc = *s->spu.pc;
            if (s->spu.policy) r->pm  = *s->spu.policy;
            if (s->kind == PS3_HC_PPU && s_hle_tbl && s->id < s_hle_n) r->hle = s_hle_tbl[s->id];
            /* never below StackLimit: the guard page is under it, and touching
             * it from here would take the thread's stack growth away */
            const uintptr_t lim = (uintptr_t)s->tib->StackLimit;
            if (rsp >= lim && rsp >= s->stk_lo && rsp < s->stk_hi) {
                len = s->stk_hi - rsp;
                if (len > HC_COPY_CAP) len = HC_COPY_CAP;
                memcpy(s_copy, (const void*)rsp, len);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            len = 0;
        }
    }
    ResumeThread(s->h);
    const uint64_t d = hc_ns() - t0;
    s_st.susp_ns_total += d; s_f_susp_sum += d;
    if (d > s_st.susp_ns_max) s_st.susp_ns_max = d;
    if (d > s_f_susp_max) s_f_susp_max = d;
    if (!live) return -1;
    if (!len) { pcs[0] = s_ctx.Rip; return 1; }
    /* rebase: pointers into [rsp, stack top) now point into the copy */
    const uintptr_t olo = rsp, ohi = s->stk_hi;
    const intptr_t delta = (intptr_t)s_copy - (intptr_t)rsp;
    memset(s_copy + len, 0, HC_COPY_PAD);   /* an unwind that runs past the copy reads zeroes */
    uint64_t* w = (uint64_t*)s_copy;
    for (size_t i = 0; i < len / 8; i++)
        if (w[i] >= olo && w[i] < ohi) w[i] = (uint64_t)((intptr_t)w[i] + delta);
    s_wctx = s_ctx;
    DWORD64* regs[] = { &s_wctx.Rax, &s_wctx.Rcx, &s_wctx.Rdx, &s_wctx.Rbx, &s_wctx.Rbp, &s_wctx.Rsi,
                        &s_wctx.Rdi, &s_wctx.R8, &s_wctx.R9, &s_wctx.R10, &s_wctx.R11, &s_wctx.R12,
                        &s_wctx.R13, &s_wctx.R14, &s_wctx.R15 };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if (*regs[i] >= olo && *regs[i] < ohi) *regs[i] = (DWORD64)((intptr_t)*regs[i] + delta);
    s_wctx.Rsp = (DWORD64)(uintptr_t)s_copy;
    const int n = hc_walk(&s_wctx, (uintptr_t)s_copy, (uintptr_t)s_copy + len, pcs, (int)s_depth);
    if (n <= 0) { pcs[0] = s_ctx.Rip; return 1; }
    return n;
}

static void hc_pass(double st, double now)
{
    const uint64_t p0 = hc_ns();
    const uint32_t t_us = (uint32_t)((now - st) * 1e6);
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != HC_LIVE || s->tid == s_self_tid) continue;
        hc_ss* ss = &s_ss[i];
        if (ss->serial != s->serial || ss->gen != s_gen) {
            const LONG written = ss->serial == s->serial ? ss->written : 0;
            memset(ss, 0, sizeof *ss);
            ss->serial = s->serial; ss->gen = s_gen; ss->written = written;
        }
        const size_t need = HC_ALIGN8(sizeof(hc_rec) + (size_t)s_depth * 8u);
        if (s_cur + need > s_buf_bytes) { s_f_cut |= 1; return; }
        hc_rec* r = (hc_rec*)(s_buf + s_cur);
        memset(r, 0, sizeof *r);
        r->t_us = t_us; r->slot = (uint16_t)i;
        ULONG64 cyc = 0;
        const BOOL cok = QueryThreadCycleTime(s->h, &cyc);
        /* ran = executed since the last pass. Right after we sampled it, its count
         * also holds what our own suspend cost it (the kernel runs the suspend
         * APC on the thread itself): up to s_apc_cyc of that is not a run. */
        const ULONG64 dc = cyc - ss->cyc_last;
        const int ran = cok && ss->have_cyc && dc != 0 && !(ss->perturbed && dc <= s_apc_cyc);
        if (cok) { if (!ss->have_cyc) ss->cyc_first = cyc; ss->cyc_last = cyc; ss->have_cyc = 1; }
        ss->seen++;
        if (ran) ss->ran++;
        if (cok && !ran && ss->last_rec && ss->since_real < HC_FORCE_REAL) {
            /* did not run since the last pass: its stack cannot have changed */
            r->st = 'w'; r->ref = ss->last_rec;
            ss->since_real++;
            ss->perturbed = 0;
            s_cur += HC_ALIGN8(sizeof(hc_rec));
            s_f_waits++; s_st.waits_skipped++;
            continue;
        }
        const int n = hc_sample(s, r, (uint64_t*)(r + 1));
        if (n < 0) {
            r->st = 'x';
            s_cur += HC_ALIGN8(sizeof(hc_rec));
            s_st.failed++;
            ss->perturbed = 1;
            continue;
        }
        r->st = (!cok || ss->seen == 1) ? 'q' : ran ? 'r' : 'w';
        r->depth = (uint8_t)n;
        ss->last_rec = (unsigned)s_cur + 1u;
        ss->since_real = 0;
        ss->perturbed = 1;
        s_cur += HC_ALIGN8(sizeof(hc_rec) + (size_t)n * 8u);
        s_f_samples++; s_st.samples++;
    }
    const uint64_t d = hc_ns() - p0;
    s_f_pass_sum += d; s_st.pass_ns_total += d;
    if (d > s_st.pass_ns_max) s_st.pass_ns_max = d;
    s_f_passes++; s_st.passes++;
}

static void hc_extra(const hc_slot* s, const hc_rec* r, char* out, size_t n)
{
    if (s->kind == PS3_HC_SPU)
        snprintf(out, n, "ovl=%d,img=%d,pc=%X,pm=%d", r->ovl, r->img, r->spc, r->pm);
    else if (s->kind == PS3_HC_PPU) {
        char nm[64] = "-";
        if (r->hle) {
            __try {
                size_t k = 0;
                while (k < sizeof nm - 1 && r->hle[k] && r->hle[k] != '\t' && r->hle[k] != '\n' && r->hle[k] != ',') { nm[k] = r->hle[k]; k++; }
                nm[k] = 0;
                if (!k) snprintf(nm, sizeof nm, "-");
            } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(nm, sizeof nm, "?"); }
        }
        snprintf(out, n, "hle=%s", nm);
    } else
        snprintf(out, n, "-");
}

static void hc_line_flush(hc_ss* ss, LONG serial)
{
    if (!ss->pend) return;
    hc_out("S\t%u\t%.3f\t%ld\t%c\t%u\t%u\t%s\n", s_kept, ss->p_t, (long)serial, ss->p_st, ss->p_node, ss->p_rep, ss->p_extra);
    ss->pend = 0;
}

static void hc_commit(double st, double end, double first)
{
    const uint64_t w0 = hc_ns();
    s_kept++;
    hc_modules();
    hc_out("HITCH\t%u\t%.4f\t%.4f\t%.1f\t%.1f\t%u\t%s\n", s_kept, st - s_t0, end - s_t0, (end - st) * 1000.0,
           first * 1000.0, s_f_passes, s_f_cut ? "cut" : "-");
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_ss* ss = &s_ss[i];
        const hc_slot* s = &s_slot[i];
        if (ss->gen != s_gen || !ss->seen || ss->written == ss->serial) continue;
        ss->written = ss->serial;
        hc_out("THREAD\t%ld\t%s\t%u\t%lu\t%s\n", (long)ss->serial,
               s->kind == PS3_HC_SPU ? "SPU" : s->kind == PS3_HC_PPU ? "PPU" : "RSX",
               s->id, (unsigned long)s->tid, s->name);
    }
    s_node_gen++; s_nnodes = 0; s_nodes_full = 0;
    if (!s_node_gen) s_node_gen = 1;
    for (size_t off = 0; off < s_cur; ) {
        hc_rec* r = (hc_rec*)(s_buf + off);
        const hc_slot* s = &s_slot[r->slot];
        hc_ss* ss = &s_ss[r->slot];
        const uint64_t* pcs = (const uint64_t*)(r + 1);
        const hc_rec* src = r;
        if (r->depth) {
            uint32_t node = 0;
            for (int k = r->depth - 1; k >= 0; k--)
                if (!(node = hc_node(node, pcs[k]))) break;   /* table full: no stack, never a wrong one */
            r->node = node;
        } else if (r->ref) {
            src = (const hc_rec*)(s_buf + r->ref - 1);
            r->node = src->node;
        }
        char extra[96];
        if (r->st == 'x') snprintf(extra, sizeof extra, "-");
        else hc_extra(s, src, extra, sizeof extra);
        const double t = r->t_us / 1000.0;
        if (ss->pend && ss->p_st == (char)r->st && ss->p_node == r->node && !strcmp(ss->p_extra, extra)) ss->p_rep++;
        else {
            hc_line_flush(ss, ss->serial);
            ss->pend = 1; ss->p_t = t; ss->p_st = (char)r->st; ss->p_node = r->node; ss->p_rep = 1;
            memcpy(ss->p_extra, extra, sizeof extra);
        }
        off += HC_ALIGN8(sizeof(hc_rec) + (size_t)r->depth * 8u);
    }
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_ss* ss = &s_ss[i];
        if (ss->gen != s_gen || !ss->seen) continue;
        hc_line_flush(ss, ss->serial);
        const double cpu_ms = s_tsc_hz > 0.0 ? (double)(ss->cyc_last - ss->cyc_first) / s_tsc_hz * 1000.0 : -1.0;
        hc_out("TCPU\t%u\t%ld\t%.2f\t%u\t%u\n", s_kept, (long)ss->serial, cpu_ms, ss->ran, ss->seen);
    }
    ULONG64 my = 0;
    QueryThreadCycleTime(s_self_h, &my);
    const uint64_t wns = hc_ns() - w0;
    s_st.write_ns_total += wns;
    hc_out("END\t%u\t%u\t%u\t%u\t%.1f\t%.1f\t%.1f\t%.2f\t%.2f\t%u%s\n", s_kept, s_f_passes, s_f_samples, s_f_waits,
           s_f_susp_max / 1000.0, s_f_samples ? s_f_susp_sum / 1000.0 / s_f_samples : 0.0,
           s_f_passes ? s_f_pass_sum / 1000.0 / s_f_passes : 0.0,
           s_tsc_hz > 0.0 ? (double)(my - s_f_cyc0) / s_tsc_hz * 1000.0 : -1.0, wns / 1e6, s_nnodes,
           s_nodes_full ? "\tnodes_full" : "");
    hc_flush();
    s_st.frames_kept++;
}

static void hc_sample_frame(double st)
{
    s_st.frames_sampled++;
    s_gen++;
    s_cur = 0;
    s_f_passes = s_f_samples = s_f_waits = 0;
    s_f_susp_max = s_f_susp_sum = s_f_pass_sum = 0;
    s_f_cut = 0;
    s_f_cyc0 = 0;
    QueryThreadCycleTime(s_self_h, &s_f_cyc0);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const double first = hc_now() - st;
    const double period = s_period_us / 1e6;
    double next = hc_now();
    int sampling = 1;
    while (hc_frame_start() == st) {
        const double now = hc_now();
        if (sampling && (now - st > HC_MAX_SAMPLE_S || (s_f_cut & 1))) {
            sampling = 0;
            if (now - st > HC_MAX_SAMPLE_S) s_f_cut |= 2;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
        if (sampling) {
            hc_pass(st, now);
            next += period;
            const double after = hc_now();
            if (next < after) next = after + period * 0.25;   /* behind: no catch-up burst */
            hc_wait_us((next - after) * 1e6);
        } else {
            hc_wait_us(10000.0);
        }
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const double end = hc_frame_end(st);
    if (end - st >= s_keep_s && s_kept < s_max_hitches && s_st.bytes_written < s_file_cap)
        hc_commit(st, end, first);
}

/* What a suspend + GetThreadContext + resume costs a sleeping thread in its own
 * cycle count (the kernel runs the suspend on it): measured once on a helper
 * that sleeps the way a guest thread does. */
static DWORD WINAPI hc_sleeper(LPVOID ev) { WaitForSingleObject((HANDLE)ev, INFINITE); return 0; }
static void hc_calibrate(void)
{
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE t = ev ? CreateThread(NULL, 64u * 1024u, hc_sleeper, ev, 0, NULL) : NULL;
    ULONG64 mx = 0;
    if (t) {
        hc_wait_us(5000.0);
        for (int i = 0; i < 32; i++) {
            ULONG64 a = 0, b = 0;
            QueryThreadCycleTime(t, &a);
            if (SuspendThread(t) != (DWORD)-1) {
                s_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                GetThreadContext(t, &s_ctx);
                ResumeThread(t);
            }
            hc_wait_us(1000.0);
            QueryThreadCycleTime(t, &b);
            if (b - a > mx) mx = b - a;
        }
        SetEvent(ev);
        WaitForSingleObject(t, 1000);
        CloseHandle(t);
    }
    if (ev) CloseHandle(ev);
    s_apc_max = mx;
    s_apc_cyc = mx ? mx * 2u : 200000u;
}

static DWORD WINAPI hc_main(LPVOID p)
{
    (void)p;
    s_self_tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_self_h,
                    THREAD_QUERY_INFORMATION, FALSE, 0);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    { typedef HRESULT (WINAPI *sd_fn)(HANDLE, PCWSTR);
      sd_fn sd = (sd_fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
      if (sd) sd(GetCurrentThread(), L"hitchcatch"); }
    s_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_timer) s_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    ULONGLONG next_reap = GetTickCount64() + 1000;
    double done = -1.0;
    for (;;) {
        s_st.wakeups++;
        if (GetTickCount64() >= next_reap) { hc_reap(); next_reap = GetTickCount64() + 1000; }
        if (!__atomic_load_n(&s_session, __ATOMIC_ACQUIRE)) { hc_wait_us(50000.0); continue; }
        if (s_tsc_hz == 0.0) {   /* QueryThreadCycleTime counts TSC ticks: their rate, once */
            const double a = hc_now(); const uint64_t ta = __rdtsc();
            hc_wait_us(50000.0);
            const double b = hc_now(); const uint64_t tb = __rdtsc();
            s_tsc_hz = b > a ? (double)(tb - ta) / (b - a) : -1.0;
            hc_calibrate();
            hc_out("CAL\ttsc_hz=%.0f\tsuspend_cycles_max=%llu\tran_threshold_after_sample=%llu\n",
                   s_tsc_hz, (unsigned long long)s_apc_max, (unsigned long long)s_apc_cyc);
            hc_flush();
            continue;
        }
        const double st = hc_frame_start();
        if (st <= 0.0) { hc_wait_us(20000.0); continue; }
        if (st == done) { hc_wait_us(20000.0); continue; }
        const double age = hc_now() - st;
        if (age < s_start_s) { hc_wait_us((s_start_s - age) * 1e6 + 100.0); continue; }
        if (s_kept >= s_max_hitches || s_st.bytes_written >= s_file_cap) { done = st; continue; }
        hc_sample_frame(st);
        done = st;
    }
}

#endif /* _WIN32 */
