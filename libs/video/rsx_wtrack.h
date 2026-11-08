/* rsx_wtrack -- page-protection write tracking of guest memory for the draw
 * engine's content caches (textures, resident vertex and index buffers).
 *
 * Each cache entry used to re-hash its guest bytes every frame to learn that
 * they had not changed: ~2 ms of the RSX thread a frame in New Marais, almost
 * all of it cache misses on bytes nobody wrote. Instead the pages under an
 * entry are write-protected when it is hashed; a write to one of them faults
 * once, the handler makes the page writable again and bumps its version, and
 * an entry whose pages are all still protected with the versions it saw is
 * unchanged without reading a byte.
 *
 * The protocol (any thread may write, only the RSX thread arms and queries):
 *   arm:     protect the pages (under s_lock), THEN read their versions, THEN
 *            hash -- a write after the protect faults and bumps a version.
 *   fault:   make the page writable, THEN bump its version, THEN clear its
 *            protected flag -- every unprotect is followed by a bump.
 *   query:   clean = every page still flagged protected and the version sum
 *            equal to the one stored at arm time.
 * Writes the kernel makes (ReadFile / recv into guest memory) do not fault --
 * they fail with ERROR_NOACCESS -- so those paths call rsx_wt_host_io_begin /
 * _end around the call (through g_vm_host_io_hook, set here).
 * Pages written in three frames running rest for 240 frames (not protected):
 * an entry over them is hashed every frame, as before.
 *
 * PS3RECOMP_WTRACK: 0 = off (hash as before), 1 = on (default), 2 = shadow:
 * track AND hash, and report every entry the tracker called clean whose bytes
 * had changed (a missed write) -- the gate for 1. */
#ifndef RSX_WTRACK_H
#define RSX_WTRACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 off, 1 on, 2 shadow. Installs the fault handler on first use. */
int  rsx_wt_mode(void);
/* Every page of [p, p+n) protected and unwritten: adds their version sum to
 * *stamp and returns 1. Otherwise returns 0. */
int  rsx_wt_query(const void* p, size_t n, uint64_t* stamp);
/* Protect every page of [p, p+n) that may be; adds the version sum read after
 * protecting to *stamp. 1 = every page is protected now. */
int  rsx_wt_arm(const void* p, size_t n, uint64_t* stamp);
/* The presented frame (heuristics: pages that keep faulting rest). */
void rsx_wt_frame(uint32_t frame);
/* Shadow mode found a changed entry the tracker called clean. */
void rsx_wt_report_miss(const char* what, const void* p, size_t n);
/* Shadow mode: an entry called clean whose bytes were indeed unchanged. */
void rsx_wt_note_verified(void);
/* Kernel-side writes into guest memory [ea, ea+len): unprotect and pin the
 * pages before, bump their versions and unpin after. */
void rsx_wt_host_io_begin(uint32_t ea, uint32_t len);
void rsx_wt_host_io_end(uint32_t ea, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
