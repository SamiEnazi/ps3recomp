/* PS3_SPU_FASTJMP (default 1; spu_channels.c): SPU_SETJMP / SPU_LONGJMP without
 * the UCRT's unwind.
 *
 * On Windows x64 the UCRT longjmp is an SEH unwind -- RtlUnwindEx over every
 * host frame between the longjmp and its setjmp, then an NtContinue system call
 * -- and the SPU runtime longjmps on every WWS interrupt return (spu_drain.c,
 * the iret that lands below the frame that took the interrupt) and on every
 * one-way stack reset (spu_check_stack_reset). In a combat profile that was ~2%
 * of all SPU-thread time, and it sits on the job's critical path.
 *
 * Nothing between an SPU_SETJMP and its SPU_LONGJMP has an unwind handler: the
 * frames are the SPU runtime's and the lifted SPU code's, all C, no __try, no
 * destructors. So the transfer the UCRT makes after its unwind -- the
 * callee-saved registers (rbx rbp rsi rdi r12-r15 xmm6-xmm15), rsp, the resume
 * address, MXCSR and the x87 control word -- is the whole transfer, and that is
 * what spu_fast_longjmp restores. (The UCRT also restores MXCSR / FPCW, so the
 * SPU float mode that spu_run_with_halt set before its setjmp comes back the
 * same way.)
 *
 * Both forms fill a jmp_buf (256 bytes; this one uses 0x100), and a buffer is
 * always longjmp'd by the form that set it: the mode is decided once per
 * process from the environment, before any SPU runs, and never changes. A
 * process with hardware shadow stacks (a /CETCOMPAT build) keeps the UCRT form,
 * which fixes the shadow stack up through NtContinue; this one would not.
 * PS3_SPU_FASTJMP=0: the UCRT setjmp / longjmp as before.
 *
 * Include after spu_context.h (it replaces that header's two macros) -- only
 * the runtime files that set or take these jumps include it, so the lifted SPU
 * code, which never uses them, is not rebuilt. */
#ifndef SPU_FASTJMP_H
#define SPU_FASTJMP_H

#include <setjmp.h>

#if defined(_WIN32) && defined(__clang__) && (defined(__x86_64__) || defined(_M_X64))
#define SPU_FASTJMP_AVAILABLE 1
int  spu_fastjmp_on(void);
__attribute__((returns_twice)) int spu_fast_setjmp(void* env);
__attribute__((noreturn)) void spu_fast_longjmp(void* env, int v);
#  undef SPU_SETJMP
#  undef SPU_LONGJMP
#  define SPU_SETJMP(env)     (spu_fastjmp_on() ? spu_fast_setjmp(env) : setjmp(env))
#  define SPU_LONGJMP(env, v) do { if (spu_fastjmp_on()) spu_fast_longjmp((env), (v)); \
                                   longjmp((env), (v)); } while (0)
#endif

#endif /* SPU_FASTJMP_H */
