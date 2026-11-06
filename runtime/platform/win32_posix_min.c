/*
 * win32_posix_min.c -- Win32 implementations behind win32_posix_min.h.
 * The whole file is empty off Windows.
 */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <stdint.h>

/* The force-included header #defines these names; this file defines the
 * real functions, so drop the macros here. */
#undef clock_gettime
#undef nanosleep
#undef usleep
#undef sleep
#undef sched_yield

#include "win32_posix_min.h"

_Static_assert(sizeof(pthread_mutex_t) == sizeof(SRWLOCK), "pthread_mutex_t must overlay SRWLOCK");

static LARGE_INTEGER s_freq;

static uint64_t qpc_ns(void)
{
    LARGE_INTEGER c;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    /* split so count * 1e9 cannot overflow 64 bits */
    return (uint64_t)(c.QuadPart / s_freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % s_freq.QuadPart) * 1000000000ull / (uint64_t)s_freq.QuadPart;
}

int ps3w_clock_gettime(ps3w_clockid_t id, struct timespec* ts)
{
    if (!ts) { errno = EINVAL; return -1; }
    if (id == CLOCK_MONOTONIC) {
        const uint64_t ns = qpc_ns();
        ts->tv_sec  = (time_t)(ns / 1000000000ull);
        ts->tv_nsec = (long)(ns % 1000000000ull);
        return 0;
    }
    if (id == CLOCK_REALTIME) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
        t -= 116444736000000000ull;                                          /* -> since 1970 */
        ts->tv_sec  = (time_t)(t / 10000000ull);
        ts->tv_nsec = (long)((t % 10000000ull) * 100ull);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* Sleeps below a millisecond spin on QPC with SwitchToThread, like the
 * toolkit's own sys_timer_usleep Windows branch: Sleep() rounds up to the
 * scheduler quantum, which would turn a 3 us wait into ~1 ms. */
static void sleep_ns(uint64_t ns)
{
    if (ns >= 1000000ull) {
        HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) t = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (t) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(ns / 100ull);
            SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
            return;
        }
        Sleep((DWORD)(ns / 1000000ull));
        return;
    }
    const uint64_t end = qpc_ns() + ns;
    while (qpc_ns() < end) SwitchToThread();
}

int ps3w_nanosleep(const struct timespec* req, struct timespec* rem)
{
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    sleep_ns((uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

int ps3w_usleep(unsigned long long usec) { sleep_ns(usec * 1000ull); return 0; }
unsigned ps3w_sleep(unsigned sec) { sleep_ns((uint64_t)sec * 1000000000ull); return 0; }
int ps3w_sched_yield(void) { SwitchToThread(); return 0; }

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a)
{
    (void)a;
    InitializeSRWLock((PSRWLOCK)m);
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t* m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t* m) { AcquireSRWLockExclusive((PSRWLOCK)m); return 0; }
int pthread_mutex_trylock(pthread_mutex_t* m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) ? 0 : EBUSY; }
int pthread_mutex_unlock(pthread_mutex_t* m) { ReleaseSRWLockExclusive((PSRWLOCK)m); return 0; }

static __declspec(thread) char s_dlerr[256];

void* dlopen(const char* file, int mode)
{
    (void)mode;
    HMODULE h = file ? LoadLibraryA(file) : GetModuleHandleA(NULL);
    if (!h)
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(),
                       0, s_dlerr, sizeof s_dlerr, NULL);
    return (void*)h;
}
void* dlsym(void* handle, const char* name)
{
    FARPROC f = GetProcAddress((HMODULE)handle, name);
    if (!f)
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(),
                       0, s_dlerr, sizeof s_dlerr, NULL);
    return (void*)(uintptr_t)f;
}
int dlclose(void* handle) { return FreeLibrary((HMODULE)handle) ? 0 : -1; }
char* dlerror(void) { return s_dlerr[0] ? s_dlerr : (char*)"unknown error"; }

#endif /* _WIN32 */
