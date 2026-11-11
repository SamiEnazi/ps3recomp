/* SPU job record / replay equivalence checker (spu_jobrec.c).
 *
 * RECORD (PS3_JOBREC=<dir>): every WWS job-module call the policy module makes
 * (spu_indirect_branch into the resident job overlay with the job manager's
 * return address in r0) is run inside a wrapper that captures what the job
 * starts from -- the whole local store, the register file and the runtime
 * context fields dispatch reads -- and every value that reaches it from outside
 * while it runs: each channel read and count, each DMA GET with the bytes it
 * delivered, each atomic with its outcome, each stop. It also captures what the
 * job produces: every channel write, every DMA PUT with its bytes, and the final
 * local store, registers and context. One file per job (<dir>/ovl<id>_<n>.jrec).
 *
 * REPLAY (PS3_JOBREPLAY=<dir>): instead of booting the game, the exe runs every
 * recording through ITS OWN lifted code on a fresh context, serving the
 * recorded external values, and compares everything the job produced bit for
 * bit. A request that differs from the recording (another channel, address,
 * size or order) is itself a mismatch.
 *
 * STATS (PS3_JOBSTAT=1): per job module, number of calls and the host time
 * spent inside them, printed every PS3_JOBSTAT_SEC seconds (default 10).
 *
 * g_spu_jobrec_mode is 0 unless one of the variables is set, and every hook is
 * a load of it and a branch that is not taken. */
#ifndef SPU_JOBREC_H
#define SPU_JOBREC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

#define SPU_JOBREC_RECORD 1
#define SPU_JOBREC_REPLAY 2
#define SPU_JOBREC_STATS  4
extern volatile int g_spu_jobrec_mode;

/* spu_indirect_branch: a job call is run (wrapped) here; 1 = done, the caller returns. */
int spu_jobrec_job_call(struct spu_context* ctx);

/* The per-operation hooks. Each returns 1 when it handled the operation (the
 * caller returns *out), 0 when the normal path should run (no job of this
 * context is being recorded or replayed). Called only while the mode is on. */
int spu_jobrec_rdch(struct spu_context* ctx, uint32_t ch, uint32_t* out);
int spu_jobrec_rchcnt(struct spu_context* ctx, uint32_t ch, uint32_t* out);
int spu_jobrec_wrch(struct spu_context* ctx, uint32_t ch, uint32_t v);
int spu_jobrec_dma(struct spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size,
                   uint32_t cmd, int* rc);
int spu_jobrec_atomic(struct spu_context* ctx, uint32_t cmd, int* handled);
int spu_jobrec_stop(struct spu_context* ctx);
/* spu_lifted_lookup: 1 = a job replayed on the interpreter must not rejoin lifted code. */
int spu_jobrec_no_lifted(const struct spu_context* ctx);
/* spu_take_interrupt: an interrupt taken inside a recorded job (statistics). */
void spu_jobrec_irq(struct spu_context* ctx, uint32_t pending);
/* spu_run_with_halt: a halt / restart unwound past a recorded job's wrapper. */
void spu_jobrec_unwound(struct spu_context* ctx);

/* ppu_run: with PS3_JOBREPLAY set, run the replays and return the process exit
 * code (0 = every replay passed); -1 = not in replay mode, boot the game. */
int spu_jobreplay_main(void);

/* The recording / replay directory ("" = neither): spu_patch.c keeps spu_patches.txt there. */
const char* spu_jobrec_dir(void);

/* The unhooked operations (spu_channels.c), for the recorder's own use. */
uint32_t spu_rdch_raw(struct spu_context* ctx, uint32_t ch);
uint32_t spu_rchcnt_raw(struct spu_context* ctx, uint32_t ch);
void     spu_wrch_raw(struct spu_context* ctx, uint32_t ch, uint32_t v);
int      spu_mfc_atomic_raw(struct spu_context* ctx, uint32_t cmd);
void     spu_stop_raw(struct spu_context* ctx);
void*    spu_jobrec_mfc_engine(struct spu_context* ctx);   /* this context's mfc_engine */

/* Natives of a job's main function (work/rewrites/spu, agent/native204; see spu_jobrec.c). In a replay: */
void spu_native_dead_ls(uint32_t lo, uint32_t hi);        /* LS [lo,hi) is dead after the native: the replay does not compare it */
int  spu_native_replay_begin(struct spu_context* ctx);    /* replay: prime guest memory from the recording; 0 = not a replay */
void spu_native_replay_end(struct spu_context* ctx, int ran);   /* ran: replace the recorded operations by checks of the native's results */
int  spu_native_ea_ok(uint32_t ea, uint32_t size);            /* may a native read guest [ea, ea+size)? (a replay: only what the recording touched) */
uint32_t spu_native_atomic_add32(struct spu_context* ctx, uint32_t ea, uint32_t v);   /* guest word += v atomically as the job does; returns the old value (a replay: the recorded one) */

#ifdef __cplusplus
}
#endif

#endif /* SPU_JOBREC_H */
