/* ps3_limits.h -- the runtime's side of PS3RECOMP_LIMITS (limits agent, 2026-11-09).
 *
 * A PS3 title is tuned to the console's limits, and the runtime under it has
 * fixed tables of its own. When one of ours fills where the console's would
 * not, the game slows down or breaks with nothing said (the 1024-entry texture
 * cache that turned the city seen from above into a 4-10 fps slideshow). The
 * renderer's structures are in rsx_draw_engine.h (rsx_lim, rsx5); every fixed
 * table, pool and queue of the runtime proper (threads, sync objects, guest
 * memory, SPU tables, file descriptors, ...) has a ps3_lim gauge here:
 *
 *   static ps3_lim L = PS3_LIM_INIT("name", capacity);
 *   ps3_lim_set(&L, in_use);         current use (peak kept)
 *   ps3_lim_add(&L, +1 / -1);        the same as a running count
 *   ps3_lim_full(&L, "fmt", ...);    the table was full / a request failed
 *
 * Both halves report through the renderer's mechanism: every ps3_lim_full is
 * counted and logged ("[limits] <name> FULL #n: ..." -- the first 3, then each
 * power of two), names its structure on the next HITCH line of the play log
 * (lim=name:count, rsx_lim_take_frame) and puts out the second's LIMIT line;
 * the PS3RECOMP_LIMITS=1 summary (every PS3RECOMP_LIMITS_S s, default 5) and
 * the one at exit carry the gauges (name=peak/cap, !n = times full) and the
 * process's private MB, handles and threads (sampled on a thread of its own).
 *
 * Cost: an atomic add or a store per create/destroy, nothing on hot paths. */
#ifndef PS3EMU_PS3_LIMITS_H
#define PS3EMU_PS3_LIMITS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_lim {
    const char*      name;
    long             cap;        /* 0: no fixed capacity (a counter) */
    volatile long    cur;
    volatile long    peak;
    volatile long    full;       /* exhaustion events */
    long             full_seen;  /* the play log's copy (one reader thread) */
    volatile long    reg;        /* 0 new, 1 being linked, 2 linked */
    struct ps3_lim*  next;
} ps3_lim;

#define PS3_LIM_INIT(nm, capv) { (nm), (long)(capv), 0, 0, 0, 0, 0, 0 }

void ps3_lim_register(ps3_lim* l);
void ps3_lim_set(ps3_lim* l, long cur);
void ps3_lim_add(ps3_lim* l, long delta);
void ps3_lim_full(ps3_lim* l, const char* fmt, ...);

/* In-use entries of a fixed table (a create path, under the table's lock). */
#define PS3_LIM_COUNT(lim, arr, n, field) do {                               \
        long lim_c_ = 0;                                                      \
        for (long lim_i_ = 0; lim_i_ < (long)(n); lim_i_++)                   \
            lim_c_ += (arr)[lim_i_].field != 0;                               \
        ps3_lim_set(&(lim), lim_c_);                                          \
    } while (0)

/* lv2_init_syscalls: starts the process sampler under PS3RECOMP_LIMITS=1. */
void ps3_limits_init(void);
/* For rsx_lim_summary: " proc=<MB>MB/<handles>h/<threads>t name=peak/cap!full ..."
 * appended to buf at buf[len]; returns the new length. */
int  ps3_limits_format(char* buf, int len, int size);
/* For rsx_lim_take_frame: ",name:count" per gauge hit since the last call
 * appended at buf[len] (no leading comma when len == 0); returns the new length. */
int  ps3_limits_take_frame(char* buf, int len, int size);
/* Anything hit since the last take (cheap). */
int  ps3_limits_pending(void);

#ifdef __cplusplus
}
#endif

#endif
