/* rsx_wtrack.c -- see rsx_wtrack.h. Windows only; elsewhere every query says
 * "not clean" and the caches hash as before. */
#include "rsx_wtrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The hook the kernel-write paths (cellFs / sys_fs reads, socket receives)
 * call, defined in ppu_loader.cpp: (ea, len, 0) before the call, (ea, len, 1)
 * after it. */
#ifdef __cplusplus
extern "C" {
#endif
extern void (*g_vm_host_io_hook)(uint32_t ea, uint32_t len, int end);
extern uint8_t* vm_base;
#ifdef __cplusplus
}
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WT_SHIFT  12u
#define WT_PAGE   (1u << WT_SHIFT)
#define WT_PAGES  (1u << (32u - WT_SHIFT))   /* the 4 GB guest space */
#define WT_REST   240u                       /* frames a hot page is left alone */
/* Faults this close together (frames) count toward "hot".
 * (rsx5) PS3RECOMP_WTRACK_HOT_GAP (default 16; 4 = before), read at init: a
 * page the SPUs rewrite on a cycle longer than 4 frames (edge2: Edge job 223's
 * vertex output) never counted as hot, so it was protected, faulted on and
 * protected again for ever. Resting a page only means its spans are hashed. */
static uint32_t s_hot_gap = 16u;

/* Per-page state (lazily committed zero pages: only pages in use cost):
 * ver   bumped after every unprotect (fault or kernel write);
 * prot  bit 0 = protected by us now, bit 1 = ever protected by us;
 * pin   kernel writes in flight (not to be protected);
 * lastf / streak / rest: the hot-page heuristic. */
static volatile LONG*  s_ver;
static volatile char*  s_prot;
static volatile SHORT* s_pin;
static uint32_t*       s_lastf;
static uint8_t*        s_streak;
static uint32_t*       s_rest;
static volatile LONG   s_frame = 1;
static SRWLOCK s_lock = SRWLOCK_INIT;    /* arm vs kernel-write bracketing */
static int s_mode = -1;
static volatile LONG s_faults, s_arms, s_rests, s_misses, s_ios, s_verified;

static LONG CALLBACK wt_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
        er->ExceptionInformation[0] != 1 /* write */ || !vm_base)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)er->ExceptionInformation[1], base = (uintptr_t)vm_base;
    if (a < base || a - base >= ((uint64_t)WT_PAGES << WT_SHIFT)) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t P = (uint32_t)((a - base) >> WT_SHIFT);
    if (!(s_prot[P] & 2)) return EXCEPTION_CONTINUE_SEARCH;   /* never ours */
    DWORD old = 0;
    if (!VirtualProtect(vm_base + ((size_t)P << WT_SHIFT), WT_PAGE, PAGE_READWRITE, &old))
        return EXCEPTION_CONTINUE_SEARCH;
    if (old != PAGE_READONLY && old != PAGE_READWRITE) {      /* not our protection: put it back */
        DWORD o2; VirtualProtect(vm_base + ((size_t)P << WT_SHIFT), WT_PAGE, old, &o2);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedIncrement(&s_ver[P]);
    __atomic_fetch_and(&s_prot[P], (char)~1, __ATOMIC_SEQ_CST);
    InterlockedIncrement(&s_faults);
    /* Hot page: three faults with no more than s_hot_gap frames between
     * them (double-buffered data is written every other frame, a ring of
     * dynamic buffers every third or fourth). */
    const uint32_t f = (uint32_t)s_frame;
    if (s_lastf[P] && f - s_lastf[P] <= s_hot_gap) { if (++s_streak[P] >= 3u) { s_rest[P] = f + WT_REST; s_streak[P] = 0; InterlockedIncrement(&s_rests); } }
    else s_streak[P] = 1;
    s_lastf[P] = f;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void* wt_zalloc(size_t bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static void wt_hook(uint32_t ea, uint32_t len, int end)
{
    if (end) rsx_wt_host_io_end(ea, len);
    else     rsx_wt_host_io_begin(ea, len);
}

int rsx_wt_mode(void)
{
    if (__builtin_expect(s_mode >= 0, 1)) return s_mode;
    const char* e = getenv("PS3RECOMP_WTRACK");
    int m = e ? atoi(e) : 1;
    { const char* g = getenv("PS3RECOMP_WTRACK_HOT_GAP"); if (g && *g) { long v = atol(g); s_hot_gap = v < 1 ? 1u : v > 240 ? 240u : (uint32_t)v; } }
    if (m < 0 || m > 2) m = 1;
    if (m && vm_base) {
        s_ver = (volatile LONG*)wt_zalloc((size_t)WT_PAGES * 4u);
        s_prot = (volatile char*)wt_zalloc(WT_PAGES);
        s_pin = (volatile SHORT*)wt_zalloc((size_t)WT_PAGES * 2u);
        s_lastf = (uint32_t*)wt_zalloc((size_t)WT_PAGES * 4u);
        s_streak = (uint8_t*)wt_zalloc(WT_PAGES);
        s_rest = (uint32_t*)wt_zalloc((size_t)WT_PAGES * 4u);
        if (!s_ver || !s_prot || !s_pin || !s_lastf || !s_streak || !s_rest) m = 0;
        /* First in the chain: ahead of the guest VM's commit-on-access handler,
         * which would otherwise re-commit (and so unprotect) a whole 64 KB. */
        else if (!AddVectoredExceptionHandler(1, wt_veh)) m = 0;
    } else m = 0;
    fprintf(stderr, "[wtrack] page-protection write tracking: %s\n",
            m == 2 ? "SHADOW (track and hash, report misses)" : m ? "on" : "off");
    __atomic_store_n(&s_mode, m, __ATOMIC_SEQ_CST);
    /* After the mode: a kernel write bracketed through the hook sees it on. */
    if (m) __atomic_store_n(&g_vm_host_io_hook, wt_hook, __ATOMIC_SEQ_CST);
    return m;
}

static inline int wt_range(const void* p, size_t n, uint32_t* p0, uint32_t* p1)
{
    if (!n || !vm_base) return 0;
    const uintptr_t a = (uintptr_t)p, base = (uintptr_t)vm_base;
    if (a < base || a - base + n > ((uint64_t)WT_PAGES << WT_SHIFT)) return 0;
    *p0 = (uint32_t)((a - base) >> WT_SHIFT);
    *p1 = (uint32_t)((a - base + n - 1u) >> WT_SHIFT);
    return 1;
}

int rsx_wt_query(const void* p, size_t n, uint64_t* stamp)
{
    uint32_t p0, p1;
    if (s_mode <= 0 || !wt_range(p, n, &p0, &p1)) return 0;
    uint64_t s = 0;
    for (uint32_t P = p0; P <= p1; P++) {
        if (!(__atomic_load_n(&s_prot[P], __ATOMIC_SEQ_CST) & 1)) return 0;
        s += (uint32_t)__atomic_load_n(&s_ver[P], __ATOMIC_SEQ_CST);
    }
    *stamp += s;
    return 1;
}

int rsx_wt_arm(const void* p, size_t n, uint64_t* stamp)
{
    uint32_t p0, p1;
    if (s_mode <= 0 || !wt_range(p, n, &p0, &p1)) return 0;
    const uint32_t f = (uint32_t)s_frame;
    /* A resting page means the span cannot be clean: protect none of it. */
    for (uint32_t P = p0; P <= p1; P++) if (s_rest[P] > f) return 0;
    int all = 1;
    AcquireSRWLockExclusive(&s_lock);
    for (uint32_t P = p0; P <= p1; ) {
        if (s_pin[P] || s_rest[P] > f) { all = 0; P++; continue; }
        if (s_prot[P] & 1) { P++; continue; }
        /* a run of pages to protect: one call */
        uint32_t Q = P;
        while (Q + 1u <= p1 && !s_pin[Q + 1u] && s_rest[Q + 1u] <= f && !(s_prot[Q + 1u] & 1)) Q++;
        for (uint32_t k = P; k <= Q; k++) __atomic_fetch_or(&s_prot[k], (char)3, __ATOMIC_SEQ_CST);
        DWORD old;
        if (!VirtualProtect(vm_base + ((size_t)P << WT_SHIFT), (size_t)(Q - P + 1u) << WT_SHIFT, PAGE_READONLY, &old)) {
            /* A run over an uncommitted page (or another allocation): page by page. */
            for (uint32_t k = P; k <= Q; k++)
                if (!VirtualProtect(vm_base + ((size_t)k << WT_SHIFT), WT_PAGE, PAGE_READONLY, &old)) {
                    __atomic_fetch_and(&s_prot[k], (char)~1, __ATOMIC_SEQ_CST);
                    all = 0;
                }
        }
        InterlockedIncrement(&s_arms);
        P = Q + 1u;
    }
    ReleaseSRWLockExclusive(&s_lock);
    uint64_t s = 0;
    for (uint32_t P = p0; P <= p1; P++) s += (uint32_t)__atomic_load_n(&s_ver[P], __ATOMIC_SEQ_CST);
    *stamp += s;
    return all;
}

void rsx_wt_host_io_begin(uint32_t ea, uint32_t len)
{
    if (s_mode <= 0 || !len) return;
    const uint64_t last = (uint64_t)ea + len - 1u;
    const uint32_t p0 = ea >> WT_SHIFT, p1 = last >> 32 ? WT_PAGES - 1u : (uint32_t)(last >> WT_SHIFT);
    InterlockedIncrement(&s_ios);
    AcquireSRWLockExclusive(&s_lock);
    for (uint32_t P = p0; P <= p1; P++) {
        __atomic_fetch_add(&s_pin[P], (SHORT)1, __ATOMIC_SEQ_CST);
        if (s_prot[P] & 1) {
            DWORD old;
            VirtualProtect(vm_base + ((size_t)P << WT_SHIFT), WT_PAGE, PAGE_READWRITE, &old);
            InterlockedIncrement(&s_ver[P]);
            __atomic_fetch_and(&s_prot[P], (char)~1, __ATOMIC_SEQ_CST);
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void rsx_wt_host_io_end(uint32_t ea, uint32_t len)
{
    if (s_mode <= 0 || !len) return;
    const uint64_t last = (uint64_t)ea + len - 1u;
    const uint32_t p0 = ea >> WT_SHIFT, p1 = last >> 32 ? WT_PAGES - 1u : (uint32_t)(last >> WT_SHIFT);
    AcquireSRWLockExclusive(&s_lock);
    for (uint32_t P = p0; P <= p1; P++) {
        InterlockedIncrement(&s_ver[P]);       /* the bytes changed under no fault */
        if (s_pin[P] > 0) __atomic_fetch_sub(&s_pin[P], (SHORT)1, __ATOMIC_SEQ_CST);
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void rsx_wt_frame(uint32_t frame)
{
    InterlockedExchange(&s_frame, (LONG)(frame ? frame : 1u));
    /* PS3RECOMP_WTRACK_STATS=1: every 600 frames, what the tracker did. */
    static int stats = -1;
    if (stats < 0) stats = getenv("PS3RECOMP_WTRACK_STATS") ? 1 : 0;
    if (stats && s_mode > 0 && frame % 600u == 0u) {
        fprintf(stderr, "[wtrack] frame %u: %ld write faults, %ld protect calls, %ld pages rested, %ld kernel writes, %ld shadow misses, %ld shadow checks passed\n",
                frame, (long)s_faults, (long)s_arms, (long)s_rests, (long)s_ios, (long)s_misses, (long)s_verified);
        s_faults = s_arms = s_rests = s_ios = s_verified = 0;
    }
}

void rsx_wt_note_verified(void) { InterlockedIncrement(&s_verified); }

void rsx_wt_report_miss(const char* what, const void* p, size_t n)
{
    const long k = InterlockedIncrement(&s_misses);
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 64)
        fprintf(stderr, "[wtrack] MISSED WRITE #%ld: %s at guest 0x%08X (+%zu bytes) called clean but its bytes changed\n",
                k, what, (unsigned)((const uint8_t*)p - vm_base), n);
}

#else  /* !_WIN32: no tracking */

int  rsx_wt_mode(void) { return 0; }
int  rsx_wt_query(const void* p, size_t n, uint64_t* stamp) { (void)p; (void)n; (void)stamp; return 0; }
int  rsx_wt_arm(const void* p, size_t n, uint64_t* stamp) { (void)p; (void)n; (void)stamp; return 0; }
void rsx_wt_frame(uint32_t frame) { (void)frame; }
void rsx_wt_report_miss(const char* what, const void* p, size_t n) { (void)what; (void)p; (void)n; }
void rsx_wt_note_verified(void) {}
void rsx_wt_host_io_begin(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void rsx_wt_host_io_end(uint32_t ea, uint32_t len) { (void)ea; (void)len; }

#endif
