/*
 * ps3recomp - (rsx-par) the RSX thread's frame on two threads
 * (PS3RECOMP_RSX_SPLIT, default 1; 0 = the walker runs the draw engine itself,
 * as before). See rsx_split.c.
 */
#ifndef PS3RECOMP_RSX_SPLIT_H
#define PS3RECOMP_RSX_SPLIT_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Walker actions the exec thread runs in FIFO order (gcm_split_run). */
enum {
    RSX_SPLIT_ACT_WORD = 1,   /* a = (subch << 13) | method, b = data, c = the header's get offset */
    RSX_SPLIT_ACT_SEMA = 2,   /* a = label address, b = value (NV406E semaphore release) */
    RSX_SPLIT_ACT_FLIP = 3    /* a = the FIFO flip word (0xFEADxxxx) */
};

/* At the start of a drain whose headers may take the split path (the walker's
 * fast conditions hold): 1 = this drain runs split (the walker's own
 * dispatcher holds the register state, the exec thread runs the engine), 0 =
 * the old path. */
int  rsx_split_drain_begin(void);
/* A drain that runs the old path: the engine's own dispatcher takes the state
 * back first (the exec thread is idle between drains). */
void rsx_split_release(void);
/* The walker's rsx_draw_engine_burst / rsx_draw_engine_method while split. */
void rsx_split_burst(u32 method, int incr, const u32* be_words, u32 count);
void rsx_split_method(u32 method, u32 arg);
/* A walker action (RSX_SPLIT_ACT_*), queued behind everything before it.
 * Every action may write a label the walker reads. */
void rsx_split_act(u32 kind, u32 a, u32 b, u32 c);
/* Before the walker reads a label (semaphore acquire): every queued action
 * that may write one has run. */
void rsx_split_wait_labels(void);
/* Everything queued has run (the end of a drain). */
void rsx_split_sync(void);

/* cellGcmSys.c: one action, on the exec thread. */
void gcm_split_run(u32 kind, u32 a, u32 b, u32 c);

/* (rsx-par) called at the end of every FIFO drain, after everything the drain
 * queued has run (both paths): the Vulkan backend's PS3RECOMP_VK_BT sets it to
 * drain its recording worker, so whatever the frame loop does with the backend
 * between drains finds it idle. NULL = nothing. */
extern void (*g_rsx_drain_end_hook)(void);

/* (rsx-par2) PS3_STALL_MS (runtime/diag/ps3_limits.c): the RSX side's threads in
 * the [stall-rsx] line -- what each one says it waits on (and for how long),
 * its host RIP and the first return addresses on its stack (exe+offset: map
 * them with the exe's PDB), plus the split ring and the recording ring. */
enum { RSX_DIAG_WALKER = 0, RSX_DIAG_EXEC, RSX_DIAG_WORKER, RSX_DIAG_N };
void rsx_diag_thread_here(int who);        /* the calling thread is `who` */
void rsx_diag_wait(const char* what);      /* the calling thread waits on `what` from now; NULL = no longer */
int  rsx_stall_snapshot(char* buf, int size);
/* The Vulkan backend's recording ring, for the line (set by vk_tq_install). */
extern int (*g_rsx_diag_backend)(char* buf, int size);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_SPLIT_H */
