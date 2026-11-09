/*
 * ppu_hooks -- a title's own code changing guest behaviour while the game runs
 * (cheats, fixes), on a static lift. Patching the guest's instruction bytes does
 * nothing there (the code runs from the translation), so the lift itself leaves
 * the places to change, from two files of title knowledge kept with the title:
 *
 *   PPU_HOOK_FILE=<file>   one "<hex address>" per line: that function's entry gets
 *                          a hook slot (tools/ppu_fastpath.py). An empty slot costs a
 *                          load and a never-taken branch per call.
 *   PPU_PATCH_FILE=<file>  "<hex address> <slot> <hex original> <hex alt>[,<hex alt>...]":
 *                          that instruction is lifted with its alternatives, picked by
 *                          g_ppu_patch[slot] (0 = the original; 1.. = an alternative).
 *                          Straight-line instructions only.
 *
 * The title's code (not the toolkit) then sets a hook or picks an alternative at
 * any time, from any thread:
 *
 *     static void my_hook(ppu_context* ctx, ppu_hook_body body)
 *     {
 *         ... read the arguments the function reads (ctx->gpr[3], ctx->fpr[1]) ...
 *         ppu_hook_call(ctx, body);     // the function itself (and its tail jumps)
 *         ... its results (ctx->gpr[3], ctx->fpr[1]) ...
 *     }
 *     ppu_hook_set(0x00123456, my_hook);   // NULL = off again
 *     ppu_patch_set(3, 1);                 // instruction slot 3: alternative 1
 *
 * A hook runs on the guest thread that called the function, inside the guest's
 * call: guest memory through vm_read32/vm_write32 (ppu_recomp.h), no blocking.
 * ctx holds, at entry, the registers the lifted body reads (its callers write those
 * back to the file); registers it does not read can be stale there.
 *
 * ppu_hook_exists / ppu_patch_lifted tell whether this executable's lift has the
 * slot (a lift without the files has none, and the setters then do nothing).
 */
#ifndef PS3EMU_PPU_HOOKS_H
#define PS3EMU_PPU_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;
typedef void (*ppu_hook_body)(struct ppu_context*);
typedef void (*ppu_hook_fn)(struct ppu_context*, ppu_hook_body);

/* runtime/ppu/ppu_loader.cpp */
extern volatile uint8_t g_ppu_patch[256];
int  ppu_hook_register(uint32_t addr, ppu_hook_fn volatile* slot);   /* the lift's entries, before main */
int  ppu_patch_register(int slot);                                   /* the lift's instruction slots, before main */
int  ppu_hook_exists(uint32_t addr);
int  ppu_hook_set(uint32_t addr, ppu_hook_fn fn);                    /* 0: no such slot (yet: a call made
                                                                        before main is kept for the lift's
                                                                        registration, which runs in linker order) */
int  ppu_patch_lifted(int slot);
void ppu_patch_set(int slot, int alternative);
void ppu_hook_call(struct ppu_context* ctx, ppu_hook_body body);    /* the body and its trampoline drain */

#ifdef __cplusplus
}
#endif

#endif
