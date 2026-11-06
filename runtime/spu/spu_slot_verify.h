/* Resident code slot check on the dispatch path (see spu_resident_slot_stale
 * in spu_channels.c): the word at pc is compared with the slot module's own
 * byte image inline, and only a mismatch takes the out-of-line call that
 * decides (PS3_SPU_SLOT_VERIFY) and reports. */
#ifndef SPU_SLOT_VERIFY_H
#define SPU_SLOT_VERIFY_H
#include <stdint.h>
#include <string.h>
#include "spu_context.h"

extern uint8_t* vm_base;
extern int g_spu_slot_verify;   /* PS3_SPU_SLOT_VERIFY, default 1 */
int spu_resident_slot_stale(spu_context* ctx, unsigned slot, uint32_t pc);

/* Caller has checked pc - lsa < size for this slot. */
static inline int spu_slot_word_stale(spu_context* ctx, unsigned slot, uint32_t pc)
{
    if (!g_spu_slot_verify) return 0;
    const uint32_t at = pc & 0x3FFFCu;
    const uint32_t off = at - ctx->resident_code[slot].lsa;
    uint32_t have, want;
    memcpy(&have, ctx->ls + at, 4);
    memcpy(&want, vm_base + ctx->resident_code[slot].source_ea + off, 4);
    return have != want && spu_resident_slot_stale(ctx, slot, pc);
}
#endif
