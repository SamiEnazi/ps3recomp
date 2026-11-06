/* spu_fi (the NEON form on arm64) must be bit-identical to spu_fi_exact for every input.
 * Every 32-bit estimate word is checked against three Y operands (0, all ones, and a
 * pseudo-random one per word), then random pairs, then the borrow edge hi == p +- 1.
 * Build on arm64:
 *   cc -O2 -fno-strict-aliasing -fwrapv -I.. -I../../../include test_spu_fi_neon.c -o t && ./t [N]
 * With -DSPU_SCALAR_HELPERS (or on another host) it compares the reference with itself. */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0xD1B54A32D192ED03ull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static unsigned long long s_checked, s_bad;
static void check(const uint32_t a[4], const uint32_t b[4])
{
    u128 va, vb;
    for (int i = 0; i < 4; i++) { va._u32[i] = a[i]; vb._u32[i] = b[i]; }
    const u128 got = spu_fi(va, vb), want = spu_fi_exact(va, vb);
    for (int i = 0; i < 4; i++) {
        s_checked++;
        if (got._u32[i] != want._u32[i] && s_bad++ < 10)
            printf("MISMATCH a=%08X b=%08X got=%08X want=%08X\n", a[i], b[i], got._u32[i], want._u32[i]);
    }
}

int main(int argc, char** argv)
{
    const unsigned long long n = argc > 1 ? strtoull(argv[1], 0, 0) : 50000000ull;
    uint32_t a[4], b[4];
    /* every estimate word, Y = 0 / all ones / random */
    for (uint64_t w = 0; w < (1ull << 32); w += 4) {
        for (int i = 0; i < 4; i++) b[i] = (uint32_t)(w + (uint64_t)i);
        for (int i = 0; i < 4; i++) a[i] = 0; check(a, b);
        for (int i = 0; i < 4; i++) a[i] = 0xFFFFFFFFu; check(a, b);
        for (int i = 0; i < 4; i++) a[i] = rnd32(); check(a, b);
    }
    /* random pairs */
    for (unsigned long long k = 0; k < n; k++) {
        for (int i = 0; i < 4; i++) { a[i] = rnd32(); b[i] = rnd32(); }
        check(a, b);
    }
    /* the borrow edge: step * y next to base << 19 */
    for (unsigned long long k = 0; k < n / 4; k++) {
        for (int i = 0; i < 4; i++) {
            const uint32_t step = 1u + rnd32() % 0x3FFu, y = rnd32() & 0x7FFFFu;
            const uint64_t p = (uint64_t)step * y;
            uint32_t base = (uint32_t)((p >> 19) & 0x1FFFu);
            base = (base + (rnd32() % 3u) - 1u) & 0x1FFFu;
            const uint32_t e = rnd32() % 4u == 0 ? (rnd32() % 3u) : (rnd32() & 0xFFu);
            b[i] = (rnd32() & 0x80000000u) | (e << 23) | (base << 10) | step;
            a[i] = (rnd32() & ~0x7FFFFu) | y;
        }
        check(a, b);
    }
    printf("spu_fi vs spu_fi_exact: %llu lanes, %llu mismatches\n", s_checked, s_bad);
    return s_bad != 0;
}
