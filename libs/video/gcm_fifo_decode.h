/*
 * gcm_fifo_decode.h -- control words of the RSX command FIFO.
 *
 * A FIFO word at a command position is one of:
 *   method header   bits 31-29 = 000 (incrementing) or 010 (non-incrementing),
 *                   count in 28-18, method in 12-2, bits 1-0 = 00
 *   old JUMP        0x20000000 | offset    (bits 31-29 = 001)
 *   new JUMP        offset | 1             (bits 31-29 = 000, bits 1-0 = 01)
 *   CALL            offset | 2             (bits 1-0 = 10)
 *   RETURN          0x00020000
 *
 * The walker in cellGcmSys.c decoded only the old JUMP. A new one, read as a
 * method header, takes its count from bits 28-18 of the offset (0 below
 * 256 KB, up to 2047 above), so the walker either steps into the words after
 * the jump or skips that many words as method data and goes on decoding from
 * the middle of whatever lies there.
 *
 * Header-only, no runtime state: tests/gcm_fifo_decode includes it on its own.
 */
#ifndef GCM_FIFO_DECODE_H
#define GCM_FIFO_DECODE_H

#include <stdint.h>

/* Either JUMP form: 1 and the target IO offset, else 0. `new_form` = 0 decodes
 * the old form only (the walker before this header). */
static inline int gcm_fifo_jump_target(uint32_t w, uint32_t* tgt, int new_form)
{
    if ((w >> 29) == 1u) { *tgt = w & 0x1FFFFFFCu; return 1; }
    if (new_form && (w & 0xE0000003u) == 1u) { *tgt = w & 0x1FFFFFFCu; return 1; }
    return 0;
}

/* A new-form JUMP, whether or not the walker decodes it. */
static inline int gcm_fifo_is_new_jump(uint32_t w) { return (w & 0xE0000003u) == 1u; }

#endif
