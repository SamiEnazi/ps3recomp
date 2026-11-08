/*
 * Host thread placement on hybrid CPUs (Windows CPU Sets). Timing only: the
 * same guest work runs, on chosen cores.
 *
 * Measured 2026-11-08 (inFAMOUS 2, i5-13600K = 6 P-cores with 2 hyperthreads
 * + 8 E-cores, ETW sampling): left to the OS, the five SPURS worker threads ran
 * ~40% of their time on the E-cores, the RSX thread 21% and the main PPU thread
 * 14%, while two guest poller threads that spin in sub-millisecond usleep (90%
 * of a core between them) sat on the P-cores, next to the frame-critical
 * threads on the same physical cores.
 *
 * PS3_CPU_PLACE (read once):
 *   unset/0  the OS decides (default)
 *   p        main, RSX and SPU threads on the performance cores; others free
 *   pe       as p, and every other thread (guest PPU threads, audio, I/O,
 *            driver threads: the process default) on the efficiency cores
 *   ded      the main PPU thread and the RSX thread each get a performance
 *            core of their own (both hyperthreads; nothing else is placed
 *            there), the SPU threads the other performance cores, everything
 *            else the efficiency cores
 *   dedx     as ded, the SPU threads also on the efficiency cores
 *   own      main and RSX own a performance core each (both hyperthreads);
 *            every other thread may run anywhere else (nothing confined to
 *            the E-cores: the guest's task kickers feed the SPUs)
 * On a CPU with one efficiency class only (no E-cores) p/pe do nothing.
 *
 * PS3_HIGHQOS=1: process power throttling off (Windows EcoQoS), so the
 * scheduler treats every thread as high QoS whether or not the window has
 * focus, and the same per thread for main, RSX and SPU threads
 * (ThreadPowerThrottling). Independent of PS3_CPU_PLACE.
 *
 * PS3_PLACE_PRIO=1: main and RSX threads at THREAD_PRIORITY_ABOVE_NORMAL
 * (=2: HIGHEST). Independent of PS3_CPU_PLACE.
 *
 * Call ps3_host_place_self(role) at a thread's entry, on that thread. A thread
 * placed once keeps its first role (ps3_host_place_self_once for call sites a
 * placed thread may also reach).
 */
#ifndef PS3RECOMP_HOST_PLACE_H
#define PS3RECOMP_HOST_PLACE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_PLACE_MAIN = 0,   /* the guest's main PPU thread (the frame's game logic) */
    PS3_PLACE_RSX  = 1,   /* the thread that drains the GCM FIFO and encodes draws */
    PS3_PLACE_SPU  = 2,   /* SPU host threads (SPURS workers, raw SPUs) */
    PS3_PLACE_PPU  = 3,   /* other guest PPU threads */
    PS3_PLACE_OTHER = 4
};

void ps3_host_place_self(int role);
void ps3_host_place_self_once(int role);   /* no-op if this thread was placed */

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_HOST_PLACE_H */
