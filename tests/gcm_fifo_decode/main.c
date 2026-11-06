/*
 * ps3recomp - tests/gcm_fifo_decode
 *
 * The RSX FIFO's JUMP forms (libs/video/gcm_fifo_decode.h): the old
 * 0x20000000 | offset and the new offset | 1, against the words that must not
 * read as a jump (method headers, CALL, RETURN, the 0xFEAD flip word). Then a
 * small walk over a ring laid out the way the walker in cellGcmSys.c reads it:
 * each segment ends in a JUMP over a stretch of "trap" headers to a landing
 * header; a walker that follows the jump never meets a trap.
 *
 *   cc -I libs/video tests/gcm_fifo_decode/main.c -o /tmp/t && /tmp/t
 *
 * Exit code 0 when every check passes.
 */
#include "gcm_fifo_decode.h"
#include <stdio.h>

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); \
                           else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

#define HDR(method, count) (((uint32_t)(count) << 18) | ((method) & 0x1FFCu))
#define NI(method, count)  (0x40000000u | HDR(method, count))
#define LAND  0x1800u   /* the method a landing header writes */
#define TRAP  0x1804u   /* ...and the one inside a skipped stretch */

/* The walker's dispatch order: JUMP, CALL, RETURN, method header. Returns how
 * many landing and trap methods it executed. */
static void walk(const uint32_t* ring, uint32_t words, int new_form, int* land, int* trap)
{
    uint32_t get = 0, ret = 0; int steps = 0;
    *land = *trap = 0;
    while (get / 4 < words && steps++ < 1000) {
        const uint32_t w = ring[get / 4];
        uint32_t tgt;
        if (gcm_fifo_jump_target(w, &tgt, new_form)) { get = tgt; continue; }
        if ((w & 3u) == 2u) { ret = get + 4; get = w & 0x1FFFFFFCu; continue; }
        if (w == 0x00020000u) { get = ret; continue; }
        const uint32_t type = w >> 29;
        if (type == 0 || type == 2) {
            const uint32_t count = (w >> 18) & 0x7FFu, m = w & 0x1FFCu;
            if (m == LAND) (*land)++;
            if (m == TRAP) (*trap)++;
            get += 4 + count * 4;
            continue;
        }
        get += 4;
    }
}

int main(void)
{
    uint32_t t = 0;
    CHECK(gcm_fifo_jump_target(0x20001234u & ~3u, &t, 1) && t == 0x1234u, "old JUMP 0x20001234 -> 0x%X", t);
    CHECK(gcm_fifo_jump_target(0x00563589u, &t, 1) && t == 0x00563588u, "new JUMP 0x00563589 -> 0x%X", t);
    CHECK(gcm_fifo_jump_target(0x00000101u, &t, 1) && t == 0x100u, "new JUMP below 256 KB -> 0x%X", t);
    CHECK(gcm_fifo_jump_target(0x1FFFFFFDu, &t, 1) && t == 0x1FFFFFFCu, "new JUMP, top of the IO range -> 0x%X", t);
    CHECK(!gcm_fifo_jump_target(0x00563589u, &t, 0), "new JUMP not decoded with new_form = 0");
    CHECK(gcm_fifo_is_new_jump(0x00563589u) && !gcm_fifo_is_new_jump(0x20563588u), "gcm_fifo_is_new_jump");
    CHECK(!gcm_fifo_jump_target(HDR(0x1808, 1), &t, 1), "method header is not a jump");
    CHECK(!gcm_fifo_jump_target(NI(0x1808, 4), &t, 1), "non-incrementing header is not a jump");
    CHECK(!gcm_fifo_jump_target(0x00563588u | 2u, &t, 1), "CALL offset|2 is not a jump");
    CHECK(!gcm_fifo_jump_target(0x00020000u, &t, 1), "RETURN is not a jump");
    CHECK(!gcm_fifo_jump_target(0xFEAD0100u, &t, 1), "flip word 0xFEAD0100 is not a jump");
    CHECK(!gcm_fifo_jump_target(0x40000001u, &t, 1), "bits 31-29 = 010 with low bits 01 is not a jump");

    /* Ring: [0] land, [2] JUMP -> 0x100 over traps at [3..63], [64] land,
     * [66] JUMP -> 0x400000 (> 256 KB: as a header its count is 16) over traps
     * at [67..], [0x100000] land, then a CALL to a landing + RETURN, then a
     * JUMP to the ring's end (the walk stops). */
    static uint32_t ring[0x120000];
    for (uint32_t i = 0; i < sizeof ring / 4; i++) ring[i] = HDR(TRAP, 1);
    for (int form = 0; form < 2; form++) {
        uint32_t* r = ring;
        r[0] = HDR(LAND, 1); r[1] = 0;
        r[2] = form ? (0x100u | 1u) : (0x20000000u | 0x100u);
        r[64] = HDR(LAND, 1); r[65] = 0;
        r[66] = form ? (0x400000u | 1u) : (0x20000000u | 0x400000u);
        r[0x100000] = HDR(LAND, 1); r[0x100001] = 0;
        r[0x100002] = 0x00450000u | 2u;              /* CALL 0x450000 */
        r[0x450000 / 4] = HDR(LAND, 1); r[0x450000 / 4 + 1] = 0;
        r[0x450000 / 4 + 2] = 0x00020000u;           /* RETURN */
        r[0x100003] = 0x20000000u | (uint32_t)sizeof ring;   /* JUMP to the end: stop */
        int land, trap;
        walk(r, sizeof ring / 4, 1, &land, &trap);
        CHECK(land == 4 && trap == 0, "%s JUMPs, walker decoding both: %d landings, %d traps",
              form ? "new-form" : "old-form", land, trap);
        walk(r, sizeof ring / 4, 0, &land, &trap);
        if (form)
            CHECK(trap > 0, "new-form JUMPs, old-only walker: runs into the skipped words (%d traps)", trap);
        else
            CHECK(land == 4 && trap == 0, "old-form JUMPs, old-only walker: unchanged");
    }
    printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
    return g_fail != 0;
}
