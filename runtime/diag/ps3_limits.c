/* ps3_limits.c -- the runtime's side of PS3RECOMP_LIMITS; see include/ps3emu/ps3_limits.h. */
#include "ps3emu/ps3_limits.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#  include <psapi.h>
#  define LIM_XADD(p, v)        InterlockedExchangeAdd((volatile LONG*)(p), (LONG)(v))
#  define LIM_CAS(p, nv, ov)    InterlockedCompareExchange((volatile LONG*)(p), (LONG)(nv), (LONG)(ov))
#  define LIM_CASP(p, nv, ov)   InterlockedCompareExchangePointer((PVOID volatile*)(p), (PVOID)(nv), (PVOID)(ov))
#else
#  include <pthread.h>
#  include <unistd.h>
#  define LIM_XADD(p, v)        __sync_fetch_and_add((p), (v))
#  define LIM_CAS(p, nv, ov)    __sync_val_compare_and_swap((p), (ov), (nv))
#  define LIM_CASP(p, nv, ov)   __sync_val_compare_and_swap((p), (ov), (nv))
#endif

static ps3_lim* volatile s_head;          /* every gauge touched so far */
static volatile long     s_events;        /* ps3_lim_full calls, all gauges */
static long              s_events_taken;  /* the play log's copy */

void ps3_lim_register(ps3_lim* l)
{
    if (l->reg == 2) return;
    if (LIM_CAS(&l->reg, 1, 0) != 0) {        /* another thread is linking it */
        while (l->reg != 2) { }
        return;
    }
    ps3_lim* h;
    do { h = s_head; l->next = h; } while (LIM_CASP(&s_head, l, h) != h);
    l->reg = 2;
}

static void lim_peak(ps3_lim* l, long v)
{
    long p = l->peak;
    while (v > p) {
        const long o = LIM_CAS(&l->peak, v, p);
        if (o == p) break;
        p = o;
    }
}

void ps3_lim_set(ps3_lim* l, long cur)
{
    if (l->reg != 2) ps3_lim_register(l);
    l->cur = cur;
    lim_peak(l, cur);
}

void ps3_lim_add(ps3_lim* l, long delta)
{
    if (l->reg != 2) ps3_lim_register(l);
    const long v = LIM_XADD(&l->cur, delta) + delta;
    if (delta > 0) lim_peak(l, v);
}

void ps3_lim_full(ps3_lim* l, const char* fmt, ...)
{
    if (l->reg != 2) ps3_lim_register(l);
    const unsigned long n = (unsigned long)(LIM_XADD(&l->full, 1) + 1);
    LIM_XADD(&s_events, 1);
    /* the renderer's rule (rsx_lim_event): the 1st, 2nd, 3rd, then every power of two */
    if (n <= 3u || (n & (n - 1u)) == 0u) {
        char d[256] = "";
        if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap); }
        fprintf(stderr, "[limits] %s FULL #%lu%s%s (use %ld, peak %ld, cap %ld)\n",
                l->name, n, d[0] ? ": " : "", d, l->cur, l->peak, l->cap);
        fflush(stderr);
    }
}

int ps3_limits_pending(void) { return s_events != s_events_taken; }

int ps3_limits_take_frame(char* buf, int len, int size)
{
    if (len < 0 || len >= size) return len;
    const long ev = s_events;
    if (ev == s_events_taken) return len;
    s_events_taken = ev;
    for (ps3_lim* l = s_head; l; l = l->next) {
        const long f = l->full;
        if (f == l->full_seen) continue;
        const int w = snprintf(buf + len, (size_t)(size - len), "%s%s:%ld", len ? "," : "", l->name, f - l->full_seen);
        l->full_seen = f;
        if (w < 0 || w >= size - len) { buf[len] = 0; break; }
        len += w;
    }
    return len;
}

/* ---- the process: private MB, handles, threads ---------------------------
 * Sampled every few seconds on a low-priority thread of its own (a toolhelp
 * walk costs ~1 ms, not something for the RSX thread that prints the line). */
static volatile long s_priv_mb = -1, s_handles, s_threads;
static int s_sampler;                     /* the sampler thread runs (PS3RECOMP_LIMITS=1) */

static void lim_sample(void)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
        s_priv_mb = (long)(pm.PrivateUsage >> 20);
    DWORD hc = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &hc)) s_handles = (long)hc;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s != INVALID_HANDLE_VALUE) {
        long n = 0;
        THREADENTRY32 te; te.dwSize = sizeof te;
        const DWORD me = GetCurrentProcessId();
        if (Thread32First(s, &te))
            do { if (te.th32OwnerProcessID == me) n++; te.dwSize = sizeof te; } while (Thread32Next(s, &te));
        CloseHandle(s);
        s_threads = n;
    }
#endif
}

int ps3_limits_format(char* buf, int len, int size)
{
    if (len < 0 || len >= size) return len;
    if (!s_sampler) lim_sample();         /* no sampler: the exit line and event seconds only */
    int w = 0;
    if (s_priv_mb >= 0)
        w = snprintf(buf + len, (size_t)(size - len), " proc=%ldMB/%ldh/%ldt", s_priv_mb, s_handles, s_threads);
    if (w > 0 && w < size - len) len += w;
    for (ps3_lim* l = s_head; l && len < size - 64; l = l->next) {
        if (!l->peak && !l->full && !l->cur) continue;
        w = l->cap ? snprintf(buf + len, (size_t)(size - len), " %s=%ld/%ld", l->name, l->peak, l->cap)
                   : snprintf(buf + len, (size_t)(size - len), " %s=%ld/-", l->name, l->peak);
        if (w < 0 || w >= size - len) break;
        len += w;
        if (l->full) {
            w = snprintf(buf + len, (size_t)(size - len), "!%ld", l->full);
            if (w < 0 || w >= size - len) break;
            len += w;
        }
    }
    return len;
}

/* ---- PS3_STALL_MS=<ms>: the game made no frame for that long -----------------
 * A thread polls the guest's flip count every 5 ms; when it has not moved for
 * <ms>, one [stall] line says where every guest PPU thread is (syscall, lr,
 * args: ppu_stall_snapshot) and where every SPU is (image, pc), and again at
 * 2.5x, 5x and every further second of the same stall: what a 1-second gap is
 * waiting on, caught while it waits. At most 60 lines a run. Default: off
 * (PS3RECOMP_LIMITS=1 turns it on at 150 ms). */
unsigned cellGcm_flip_request_count(void);
int ppu_stall_snapshot(char* buf, int size);
int spu_stall_snapshot(char* buf, int size);
int gcm_stall_snapshot(char* buf, int size);
int ppu_stall_host_where(char* buf, int size);
int rsx_stall_snapshot(char* buf, int size);   /* (rsx-par2) libs/video/rsx_split.c */

static double lim_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    return 0.0;
#endif
}

#ifdef _WIN32
static DWORD WINAPI stall_thread(LPVOID p)
#else
static void* stall_thread(void* p)
#endif
{
    const double thr = (double)(long)(size_t)p;
    const double t0 = lim_ms();
    unsigned last = cellGcm_flip_request_count();
    double since = t0, next = thr;
    int lines = 0;
    static char buf[4096];
    while (lines < 60) {
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
        const unsigned f = cellGcm_flip_request_count();
        const double now = lim_ms();
        if (f != last || !f) { last = f; since = now; next = thr; continue; }
        const double gap = now - since;
        if (gap < next) continue;
        int k = snprintf(buf, sizeof buf, "[stall] t=%.2fs no guest flip for %.0f ms (flips %u) PPU:",
                         (now - t0) / 1000.0, gap, f);
        k += ppu_stall_snapshot(buf + k, (int)sizeof buf - k - 512);
        if (k < (int)sizeof buf - 400) { k += snprintf(buf + k, sizeof buf - (size_t)k, " SPU:");
                                         k += spu_stall_snapshot(buf + k, (int)sizeof buf - k - 300); }
        if (k < (int)sizeof buf - 260) { k += snprintf(buf + k, sizeof buf - (size_t)k, " RSX:");
                                         k += gcm_stall_snapshot(buf + k, (int)sizeof buf - k - 200); }
        if (k < (int)sizeof buf - 200) k += ppu_stall_host_where(buf + k, (int)sizeof buf - k - 2);
        fprintf(stderr, "%s\n", buf);
        /* (rsx-par2) the walker, rsx_exec and the recording worker: waits, RIPs, stacks, rings */
        { static char rb[3072];
          if (rsx_stall_snapshot(rb, (int)sizeof rb) > 0) fprintf(stderr, "[stall-rsx] t=%.2fs %s\n", (now - t0) / 1000.0, rb); }
        fflush(stderr);
        lines++;
        next = next < 2.5 * thr ? 2.5 * thr : next < 5 * thr ? 5 * thr : next + 1000.0;
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static double s_period = 5.0;

#ifdef _WIN32
static DWORD WINAPI lim_thread(LPVOID p)
#else
static void* lim_thread(void* p)
#endif
{
    (void)p;
    for (;;) {
        lim_sample();
#ifdef _WIN32
        Sleep((DWORD)(s_period * 1000.0));
#else
        usleep((useconds_t)(s_period * 1e6));
#endif
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void stall_start(int lim_on)
{
    const char* e = getenv("PS3_STALL_MS");
    const long ms = e ? atol(e) : (lim_on ? 150 : 0);
    if (ms <= 0) return;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 64u * 1024u, stall_thread, (LPVOID)(size_t)ms, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, stall_thread, (void*)(size_t)ms) == 0) pthread_detach(th);
#endif
}

void ps3_limits_init(void)
{
    static volatile long once;
    if (LIM_CAS(&once, 1, 0) != 0) return;
    const char* e = getenv("PS3RECOMP_LIMITS");
    stall_start(e && *e && *e != '0');
    if (!(e && *e && *e != '0')) return;
    const char* s = getenv("PS3RECOMP_LIMITS_S");
    const double v = s ? atof(s) : 0.0;
    if (v >= 1.0) s_period = v;
    lim_sample();
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 64u * 1024u, lim_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); s_sampler = 1; }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, lim_thread, NULL) == 0) { pthread_detach(th); s_sampler = 1; }
#endif
}
