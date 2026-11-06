/*
 * win32_posix_min.h -- the few POSIX calls the inFAMOUS 2 patch set uses
 * unguarded, provided on Windows (clang-cl).
 *
 * Force-included on WIN32 by CMakeLists.txt (/FI), so community files need no
 * edits. Deliberately does NOT include <windows.h>: every Win32 call lives in
 * win32_posix_min.c, so no Win32 macro (min/max/near/far/ERROR...) leaks into
 * files that never asked for it. The stand-in headers in win32_include/
 * (unistd.h, sched.h, pthread.h) only pull this file in.
 *
 * Covered: clock_gettime(CLOCK_MONOTONIC|CLOCK_REALTIME), nanosleep, usleep,
 * sleep, sched_yield, access/F_OK, getpid, and non-recursive static pthread
 * mutexes (PTHREAD_MUTEX_INITIALIZER + init/lock/trylock/unlock/destroy).
 * Anything else stays a compile error on purpose: it should be ported, not
 * silently stubbed.
 */
#ifndef PS3RECOMP_WIN32_POSIX_MIN_H
#define PS3RECOMP_WIN32_POSIX_MIN_H

#ifdef _WIN32

#include <time.h>      /* struct timespec (UCRT, C11) */
#include <io.h>        /* _access */
#include <process.h>   /* _getpid */
#include <direct.h>    /* _mkdir -- included BEFORE the mkdir(p, m) macro below, so
                        * direct.h's own one-argument mkdir declaration is already
                        * parsed (and include-guarded) when the macro exists */
#include <string.h>    /* _stricmp, _strnicmp */
#include <sys/types.h>
#include <sys/stat.h>  /* _S_IFDIR, _S_IFREG, _S_IFMT */

/* GNU C `typeof` (C23 keyword); clang-cl in its default C mode only knows the
 * spelling __typeof__. */
#if !defined(__cplusplus) && !defined(typeof)
#define typeof __typeof__
#endif

/* POSIX two-argument mkdir; the mode has no meaning on NTFS. */
#define mkdir(p, m)   _mkdir(p)
/* Same text as runtime/syscalls/sys_fs.c's own Windows definition, so its
 * later #define is an identical redefinition, not a conflicting one. */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFDIR) != 0)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#define strcasecmp    _stricmp
#define strncasecmp   _strnicmp

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clocks and sleeping ------------------------------------------------ */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
typedef int ps3w_clockid_t;
typedef unsigned int useconds_t;   /* POSIX: an unsigned integer type */

int  ps3w_clock_gettime(ps3w_clockid_t id, struct timespec* ts);
int  ps3w_nanosleep(const struct timespec* req, struct timespec* rem);
int  ps3w_usleep(unsigned long long usec);
unsigned ps3w_sleep(unsigned sec);
int  ps3w_sched_yield(void);

#define clock_gettime(id, ts)  ps3w_clock_gettime((id), (ts))
#define nanosleep(req, rem)    ps3w_nanosleep((req), (rem))
#define usleep(us)             ps3w_usleep((unsigned long long)(us))
#define sleep(s)               ps3w_sleep((unsigned)(s))
#define sched_yield()          ps3w_sched_yield()

/* ---- files / process ---------------------------------------------------- */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#define access(p, m)  _access((p), (m))
#define getpid()      _getpid()

/* ---- pthread mutex over SRWLOCK ------------------------------------------
 * Layout matches SRWLOCK (one pointer, zero = unlocked), so a static
 * PTHREAD_MUTEX_INITIALIZER needs no run-time init. Non-recursive, like the
 * POSIX default mutex these call sites rely on. */
typedef struct ps3w_mutex { void* srw; } pthread_mutex_t;
typedef struct ps3w_mutexattr { int unused; } pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a);
int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock(pthread_mutex_t* m);
int pthread_mutex_trylock(pthread_mutex_t* m);
int pthread_mutex_unlock(pthread_mutex_t* m);

/* ---- dlopen family over LoadLibrary (the Vulkan backend loads vulkan-1.dll) */
#ifndef RTLD_LAZY
#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x0
#define RTLD_GLOBAL 0x100
#endif
void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int   dlclose(void* handle);
char* dlerror(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* PS3RECOMP_WIN32_POSIX_MIN_H */
