/* ps3_limits.h -- runtime limits telemetry (limits agent, 2026-11-09).
 *
 * A PS3 title is tuned to the console's limits, and the runtime under it has
 * fixed tables of its own. When one of ours fills where the console's would
 * not, the game slows down or breaks with nothing said (the 1024-entry texture
 * cache that turned the city seen from above into a 4-10 fps slideshow). Each
 * fixed table, pool and queue that can fill in play has a ps3_lim gauge:
 *
 *   static ps3_lim L = PS3_LIM_INIT("name", capacity);
 *   ps3_lim_set(&L, in_use);         current use (peak kept)
 *   ps3_lim_add(&L, +1 / -1);        the same as a running count
 *   ps3_lim_full(&L, "fmt", ...);    the table was full / a request failed
 *
 * Every ps3_lim_full is counted, logged to stderr (the first few times and
 * then every 1000th) and handed to the play log (PS3RECOMP_PLAYLOG): the next
 * HITCH line gets rtlim=<name>[,<name>...], or an RTLIMIT line if no
 * frame was slow. PS3_LIMITS=1: one summary line every PS3_LIMITS_S seconds
 * (default 5) with the process's private memory, handles, threads and every
 * gauge in use. Always: one summary line at a normal exit, and one every
 * 10 minutes, so a killed session still has a record.
 *
 * Cost: an uncontended atomic add per create/destroy, nothing on hot paths. */
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
    long             full_seen;  /* the play log's copy (its thread only) */
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

/* Starts the PS3_LIMITS reporter and the exit summary (lv2_init_syscalls). */
void ps3_limits_init(void);
/* One summary line to stderr now ("why" names the occasion). */
void ps3_limits_report(const char* why);
/* The play log's hook: the names of the limits hit since the last call,
 * comma separated, into buf; returns the length (0 = none). One caller. */
int  ps3_limits_take_events(char* buf, size_t size);
/* Cheap check for the play log: anything hit since the last take? */
int  ps3_limits_pending(void);

#ifdef __cplusplus
}
#endif

#endif
