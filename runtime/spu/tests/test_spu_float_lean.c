/* The lean single-precision forms (spu_*_lean, what the lifter emits) must be bit-identical to
 * the exact lanes (spu_*_ref, spu_fcgt...) for every input: random words, ordinary floats, the
 * normal/extended-range and underflow boundaries, zeros and denormals, cancellations, exponent
 * gaps around 29, fma sums on and next to 24-bit boundaries, mixed per lane. Build on arm64:
 *   cc -O2 -fno-strict-aliasing -fwrapv -I.. -I../../../include test_spu_float_lean.c -o t && ./t [N] */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static uint32_t rnd_float(void)
{
    uint32_t sign = (rnd32() & 1u) << 31, man = rnd32() & 0x7FFFFFu, e;
    switch (rnd32() % 14) {
    case 0: e = 0; break;                                            /* zero / denormal */
    case 1: e = 1 + rnd32() % 4; break;                              /* bottom of the normal range */
    case 2: e = 248 + rnd32() % 8; break;                            /* top, into extended range */
    case 3: return rnd32();                                          /* anything */
    case 4: return sign;                                             /* +-0 */
    case 5: man &= ~((1u << (rnd32() % 24)) - 1); e = 110 + rnd32() % 36; break;   /* short significands */
    case 6: man = (rnd32() & 1) ? 0x7FFFFF : 0; e = 120 + rnd32() % 16; break;
    case 7: e = 1 + rnd32() % 40; break;                             /* underflow territory */
    case 8: e = 200 + rnd32() % 55; break;                           /* overflow territory */
    default: e = 110 + rnd32() % 36; break;                          /* ordinary */
    }
    return sign | (e << 23) | man;
}
static uint32_t near_word(uint32_t o)
{
    switch (rnd32() % 6) {
    case 0: return o;
    case 1: return o ^ 0x80000000u;
    case 2: return o + (rnd32() % 3) - 1;
    case 3: return o & 0x807FFFFFu;                                  /* same significand, exponent 0 */
    default: return rnd_float();
    }
}
static long s_bad;
static void check(const char* op, u128 got, u128 ref, u128 a, u128 b, u128 c)
{
    if (!memcmp(&got, &ref, 16)) return;
    if (s_bad++ < 12)
        printf("MISMATCH %s a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X %08X %08X %08X\n"
               "  got=%08X %08X %08X %08X ref=%08X %08X %08X %08X\n", op,
               a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3],
               c._u32[0], c._u32[1], c._u32[2], c._u32[3], got._u32[0], got._u32[1], got._u32[2], got._u32[3],
               ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
}
static void all_ops(u128 a, u128 b, u128 c)
{
    check("fa", spu_fa_lean(a, b), spu_fa_ref(a, b), a, b, c);
    check("fs", spu_fs_lean(a, b), spu_fs_ref(a, b), a, b, c);
    check("fm", spu_fm_lean(a, b), spu_fm_ref(a, b), a, b, c);
    check("fma", spu_fma_lean(a, b, c), spu_fma_ref(a, b, c), a, b, c);
    check("fms", spu_fms_lean(a, b, c), spu_fms_ref(a, b, c), a, b, c);
    check("fnms", spu_fnms_lean(a, b, c), spu_fnms_ref(a, b, c), a, b, c);
    check("fceq", spu_fceq_lean(a, b), spu_fceq(a, b), a, b, c);
    check("fcgt", spu_fcgt_lean(a, b), spu_fcgt(a, b), a, b, c);
    check("fcmeq", spu_fcmeq_lean(a, b), spu_fcmeq(a, b), a, b, c);
    check("fcmgt", spu_fcmgt_lean(a, b), spu_fcmgt(a, b), a, b, c);
}
static u128 w4(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { u128 r; r._u32[0] = x; r._u32[1] = y; r._u32[2] = z; r._u32[3] = w; return r; }

int main(int argc, char** argv)
{
#if !defined(__ARM_NEON) && !SPU_X86_SIMD
    puts("test_spu_float_lean: no NEON, the lean forms are the plain ones");
    return 0;
#else
    /* directed: (1 - 2^-24) * 2^-126 = 2^-126 - 2^-150 truncates below Smin (+0); round-to-nearest
     * narrowing would give 2^-126. Overflow into and past the extended range, x + tiny, x - tiny. */
    all_ops(w4(0x3F7FFFFFu, 0xBF7FFFFFu, 0x7F7FFFFFu, 0x7FFFFFFFu), w4(0x00800000u, 0x00800000u, 0x40000000u, 0x3F800000u),
            w4(0x00000001u, 0x80800000u, 0x7F7FFFFFu, 0x80000001u));
    all_ops(w4(0x3F800000u, 0x3F800000u, 0x00800000u, 0x7F800000u), w4(0x2F800000u, 0xAF800000u, 0x80000000u, 0x7F800000u),
            w4(0xBF800000u, 0x3F800000u, 0x00800000u, 0xFF800000u));
    const long N = argc > 1 ? atol(argv[1]) : 4000000;
    for (long it = 0; it < N; it++) {
        u128 a, b, c;
        for (int i = 0; i < 4; i++) { a._u32[i] = rnd_float(); b._u32[i] = near_word(rnd_float()); c._u32[i] = rnd_float(); }
        switch (rnd32() % 6) {
        case 0: for (int i = 0; i < 4; i++) b._u32[i] = (a._u32[i] ^ 0x80000000u) + (rnd32() & 0xFF) - 0x80; break; /* cancellation */
        case 1: for (int i = 0; i < 4; i++)                                                                        /* gaps 12..35 */
                    b._u32[i] = (a._u32[i] & 0x80000000u) | (((a._u32[i] >> 23 & 0xFF) - 12 - rnd32() % 24) & 0xFF) << 23 | (rnd32() & 0x7FFFFF);
                break;
        case 2: for (int i = 0; i < 4; i++) {                                                                      /* c ~ -a*b */
                    float x, y; memcpy(&x, &a._u32[i], 4); memcpy(&y, &b._u32[i], 4);
                    const float p = -(x * y); uint32_t u; memcpy(&u, &p, 4); c._u32[i] = u + (rnd32() % 5) - 2; }
                break;
        case 3: for (int i = 0; i < 4; i++) c._u32[i] = (c._u32[i] & 0xFF800000u) | (rnd32() & 1 ? 0 : 0x400000); break;
        case 4: for (int i = 0; i < 4; i++) b._u32[i] = near_word(a._u32[i]); break;                              /* compares */
        default: break;
        }
        all_ops(a, b, c);
    }
    printf("test_spu_float_lean: %ld vectors x 10 ops, %ld mismatches\n", N, s_bad);
    return s_bad != 0;
#endif
}
