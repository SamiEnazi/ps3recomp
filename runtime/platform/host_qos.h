/*
 * Host thread QoS for the threads that set the frame rate.
 *
 * On Apple silicon the scheduler picks P-cores vs E-cores, and how readily it
 * preempts, from a thread's QoS class -- and a pthread does NOT inherit its
 * creator's class: it starts at QOS_CLASS_DEFAULT whatever the parent runs at
 * (measured on macOS 27: main 0x21 USER_INTERACTIVE, every child 0x15
 * DEFAULT, also after the parent raised itself). So each guest PPU thread and
 * each SPU host thread asks for its class itself, at its own entry, through
 * ps3_host_qos_self().
 *
 *   interactive != 0  QOS_CLASS_USER_INTERACTIVE  frame-critical work
 *   interactive == 0  QOS_CLASS_USER_INITIATED    the title's background work
 *
 * Opt-in, PS3_QOS=1: without it every thread keeps the class it started
 * with. In a headless New Marais A/B (2026-10-18, 3 runs per side) it did not
 * move the frame time (31.0 vs 31.4 ms at 270 s), and two of the three runs
 * with it hung on the frame fence with get == put (the flip-race signature),
 * which none of the runs without it did. PS3_HOT_QOS=1 (SetThreadPriority
 * ABOVE_NORMAL on the SPU threads and the frame clock) is separate.
 *
 * A no-op off Apple: Windows has thread priorities (SetThreadPriority, used
 * where it matters) and Linux has nothing QoS-like to ask for.
 */
#ifndef PS3RECOMP_HOST_QOS_H
#define PS3RECOMP_HOST_QOS_H

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>

static inline int ps3_host_qos_enabled(void)
{
    static int on = -1;          /* per TU; every copy reads the same variable */
    if (on < 0) {
        const char* e = getenv("PS3_QOS");
        on = e && e[0] == '1';
    }
    return on;
}

static inline void ps3_host_qos_self(int interactive)
{
    if (ps3_host_qos_enabled())
        pthread_set_qos_class_self_np(interactive ? QOS_CLASS_USER_INTERACTIVE
                                                  : QOS_CLASS_USER_INITIATED, 0);
}
#else
static inline int  ps3_host_qos_enabled(void) { return 0; }
static inline void ps3_host_qos_self(int interactive) { (void)interactive; }
#endif

/* Guest PPU priorities run 0 (highest) .. 3071. inFAMOUS 2's frame, SPURS
 * handler, streaming (fios), audio-event and kicker threads sit at 99..1001;
 * what is above that (sound-bank loading 1600, motion controller 2001, an
 * install copy 3001) can wait. */
#define PS3_HOST_QOS_PPU_INTERACTIVE_BELOW 1500

#endif /* PS3RECOMP_HOST_QOS_H */
