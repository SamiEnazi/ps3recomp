/* ps3_limits.c -- runtime limits telemetry; see include/ps3emu/ps3_limits.h. */
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
#  include <time.h>
#  include <unistd.h>
#  define LIM_XADD(p, v)        __sync_fetch_and_add((p), (v))
#  define LIM_CAS(p, nv, ov)    __sync_val_compare_and_swap((p), (ov), (nv))
#  define LIM_CASP(p, nv, ov)   __sync_val_compare_and_swap((p), (ov), (nv))
#endif

static ps3_lim* volatile s_head;          /* every gauge touched so far */
static volatile long     s_events;        /* ps3_lim_full calls, all gauges */
static long              s_events_taken;  /* the play log's copy */
static double            s_t0;

static double lim_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

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
    const long n = LIM_XADD(&l->full, 1) + 1;
    LIM_XADD(&s_events, 1);
    if (n <= 4 || n % 1000 == 0) {
        char d[256] = "";
        if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap); }
        fprintf(stderr, "[limits] FULL %s (cap %ld, in use %ld, event %ld%s) at %.1f s%s%s\n",
                l->name, l->cap, l->cur, n, n == 4 ? ", later ones every 1000th" : "",
                s_t0 ? lim_now() - s_t0 : 0.0, d[0] ? ": " : "", d);
        fflush(stderr);
    }
}

int ps3_limits_pending(void) { return s_events != s_events_taken; }

int ps3_limits_take_events(char* buf, size_t size)
{
    if (!size) return 0;
    buf[0] = 0;
    const long ev = s_events;
    if (ev == s_events_taken) return 0;
    s_events_taken = ev;
    size_t k = 0;
    for (ps3_lim* l = s_head; l; l = l->next) {
        const long f = l->full;
        if (f == l->full_seen) continue;
        l->full_seen = f;
        const int w = snprintf(buf + k, size - k, "%s%s", k ? "," : "", l->name);
        if (w < 0 || (size_t)w >= size - k) break;
        k += (size_t)w;
    }
    return (int)k;
}

/* Process-wide numbers: private bytes (MB), handles, threads. */
static void lim_process(double* priv_mb, long* handles, long* threads)
{
    *priv_mb = 0; *handles = 0; *threads = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
        *priv_mb = (double)pm.PrivateUsage / 1048576.0;
    DWORD hc = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &hc)) *handles = (long)hc;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof te;
        const DWORD me = GetCurrentProcessId();
        if (Thread32First(s, &te))
            do { if (te.th32OwnerProcessID == me) (*threads)++; te.dwSize = sizeof te; }
            while (Thread32Next(s, &te));
        CloseHandle(s);
    }
#endif
}

void ps3_limits_report(const char* why)
{
    double mb; long hc, tc;
    lim_process(&mb, &hc, &tc);
    char line[4096];
    int k = snprintf(line, sizeof line, "[limits] %s t=%.0fs priv=%.0fMB handles=%ld threads=%ld |",
                     why, s_t0 ? lim_now() - s_t0 : 0.0, mb, hc, tc);
    long fulls = 0;
    for (ps3_lim* l = s_head; l && k < (int)sizeof line - 96; l = l->next) {
        if (!l->peak && !l->full && !l->cur) continue;
        if (l->cap) k += snprintf(line + k, sizeof line - k, " %s=%ld/%ld/%ld", l->name, l->cur, l->peak, l->cap);
        else        k += snprintf(line + k, sizeof line - k, " %s=%ld/%ld", l->name, l->cur, l->peak);
        if (l->full) { k += snprintf(line + k, sizeof line - k, "!%ld", l->full); fulls += l->full; }
    }
    snprintf(line + k, sizeof line - k, " | full=%ld (name=cur/peak/cap, !n = times full)\n", fulls);
    fputs(line, stderr);
    fflush(stderr);
}

static double s_period = 600.0;

#ifdef _WIN32
static DWORD WINAPI lim_thread(LPVOID p)
#else
static void* lim_thread(void* p)
#endif
{
    (void)p;
    for (;;) {
#ifdef _WIN32
        Sleep((DWORD)(s_period * 1000.0));
#else
        usleep((useconds_t)(s_period * 1e6));
#endif
        ps3_limits_report("now");
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void lim_atexit(void) { ps3_limits_report("exit"); }

void ps3_limits_init(void)
{
    static volatile long once;
    if (LIM_CAS(&once, 1, 0) != 0) return;
    s_t0 = lim_now();
    const char* e = getenv("PS3_LIMITS");
    if (e && e[0] == '0') return;                 /* PS3_LIMITS=0: no reports at all */
    if (e && e[0] == '1') {
        const char* s = getenv("PS3_LIMITS_S");
        const double v = s ? atof(s) : 0.0;
        s_period = v >= 0.5 ? v : 5.0;
    }
    atexit(lim_atexit);
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 64u * 1024u, lim_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, lim_thread, NULL) == 0) pthread_detach(th);
#endif
}
