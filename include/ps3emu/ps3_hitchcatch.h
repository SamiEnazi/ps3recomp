/* ps3_hitchcatch.h -- PS3RECOMP_HITCHCATCH: what every guest thread was doing
 * during a frame that froze (runtime/diag/ps3_hitchcatch.c).
 *
 * On whenever PS3RECOMP_PLAYLOG is set (the play log gives the frame clock);
 * PS3RECOMP_HITCHCATCH=0 turns it off. A low-priority watchdog thread looks at
 * how long the current frame (present to present, the play log's clock) has
 * lasted. Normal frames cost the presenting thread one store and the watchdog
 * about one wake-up per 40 ms. Once a frame passes PS3RECOMP_HITCHCATCH_MS's
 * first number (default 40 ms) the watchdog samples, every 1 ms until the
 * frame ends, each registered thread: the guest PPU threads, the SPU workers
 * (with the job module their context has resident) and the presenting thread.
 * A frame that ends over the second number (default 80 ms) has its samples
 * appended to hitches.tsv next to playlog.tsv; shorter ones are discarded.
 * tools/hitch_stacks.py names the functions offline (the exe's PDB).
 *
 * A sample is SuspendThread + GetThreadContext + a copy of the top of the
 * stack + ResumeThread, nothing else while the thread is stopped: no lock, no
 * allocation, no loader or heap call. The stack is unwound afterwards on the
 * copy (RtlLookupFunctionEntry / RtlVirtualUnwind), with every thread running.
 *
 *   PS3RECOMP_HITCHCATCH_MS=start,keep   thresholds in ms (default 40,80)
 *   PS3RECOMP_HITCHCATCH_MAX=<n>         hitches written per session (default 200)
 *   PS3RECOMP_HITCHCATCH_MB=<n>          sample buffer, MB (default 16 = ~6 s of 1 ms samples)
 *   PS3RECOMP_HITCHCATCH_PERIOD_US=<n>   sampling period (default 1000)
 *   PS3RECOMP_HITCHCATCH_DEPTH=<n>       frames per stack (default 24, max 32)
 *   PS3RECOMP_HITCHCATCH_FILE=<path>     the output file (default: hitches.tsv beside the play log)
 *
 * Every function is a no-op when the catcher is off and away from Windows. */
#ifndef PS3EMU_PS3_HITCHCATCH_H
#define PS3EMU_PS3_HITCHCATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_HC_PPU 1   /* a guest PPU thread's host thread (id = guest thread id) */
#define PS3_HC_SPU 2   /* an SPU worker's host thread (id = SPU thread id) */
#define PS3_HC_RSX 3   /* the thread that presents frames */

/* What the sampler reads of an SPU worker while it is stopped: pointers into
 * its spu_context (any may be NULL). Read only while the thread is suspended
 * and still registered, so the context is alive. */
typedef struct ps3_hc_spu_view {
    const volatile int*      ovl;      /* resident_ovl: the job module (>= 200) */
    const volatile int*      img;      /* image_id */
    const volatile uint32_t* pc;       /* pc (the last dispatched LS address) */
    const volatile int*      policy;   /* policy_mode */
} ps3_hc_spu_view;

/* The calling thread joins / leaves the sampled set. name is copied; spu is
 * copied (NULL for a PPU or RSX thread). Cheap and safe to call when off. */
void ps3_hc_thread_begin(int kind, unsigned id, const char* name, const ps3_hc_spu_view* spu);
void ps3_hc_thread_end(void);

/* PPU threads: the runtime's "HLE call in flight" names, indexed by guest
 * thread id (ppu_hle.cpp's g_hle_inflight). Read as a pointer per sample. */
void ps3_hc_set_hle_table(const char* const volatile* table, unsigned n);

/* The play log opened (t0 = its clock origin, QueryPerformanceCounter seconds)
 * and every present after it (now = the same clock). playlog_path picks the
 * output folder. */
void ps3_hc_session(const char* playlog_path, double t0);
void ps3_hc_frame(double now);

/* Counters for tests and the exit line. */
typedef struct ps3_hc_stats {
    uint64_t wakeups;          /* watchdog wake-ups while idle */
    uint64_t frames_sampled;   /* frames that passed the start threshold */
    uint64_t frames_kept;      /* ... and were written */
    uint64_t passes, samples, waits_skipped, failed;
    uint64_t susp_ns_total, susp_ns_max;   /* time a target was stopped */
    uint64_t pass_ns_total, pass_ns_max;
    uint64_t write_ns_total;
    uint64_t sampler_cycles;   /* the watchdog thread's own CPU (QueryThreadCycleTime) */
    uint64_t bytes_written;
    int      threads_live;
} ps3_hc_stats;
int ps3_hc_get_stats(ps3_hc_stats* out);   /* 0 = the catcher is off */

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_PS3_HITCHCATCH_H */
