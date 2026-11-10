/* spu-cg helper forms (x86, SPU_FLOAT_RZ): the vector csflt / cuflt (SPU_CG_CVT) against the scalar
 * lanes, and the speculative float ops (spu_*_spec, SPU_LIFT_SPEC_FLOAT) against the per-op forms --
 * bit for bit, under the SPU threads' MXCSR (round toward zero + FTZ + DAZ).
 *
 *   clang-cl /O2 /arch:AVX2 /clang:-ffp-contract=off -I runtime/spu -I include runtime/spu/tests/test_spu_cg_x86.c
 *   test_spu_cg_x86 [vectors]      (default 2,000,000; exit 0 = no difference)
 *
 * csflt/cuflt: every scale 0..155 with random and edge integers (0, +-1, powers of two and their
 * neighbours, INT_MIN/MAX, 2^24 +- 1, 0xFFFFFFFF), and the out-of-range scales (the scalar form).
 * Speculative ops: an op whose accumulator stays clear must give the per-op form's bits; the
 * accumulator must be raised exactly when the per-op form takes its exact path (RZ ops: the raw
 * result's magnitude at or above 0x7F7FFFFF; cflts/cfltu: an exponent-255 lane) -- checked against
 * that condition computed here independently. */
#include <stdio.h>
#include "spu_helpers.h"
#include <stdlib.h>
#include <string.h>

#if !SPU_X86_SIMD || !SPU_X64_MXCSR
int main(void) { puts("test_spu_cg_x86: needs the AVX2 x86 forms (build with /arch:AVX2)"); return 0; }
#else

static uint64_t s_rng = 0xD1B54A32D192ED03ull;
static uint32_t rnd32(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }

static const uint32_t k_int[] = {
    0, 1, 0xFFFFFFFFu, 2, 3, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0x00FFFFFFu, 0x01000000u,
    0x01000001u, 0x00FFFFFEu, 0xFF000000u, 0xFEFFFFFFu, 0x7FFFFF80u, 0x7FFFFFC0u, 0x40000000u,
    0xC0000000u, 0x0000FFFFu, 0xFFFF0000u, 0x00800000u, 0x007FFFFFu, 0x12345678u, 0x87654321u,
};
static const uint32_t k_flt[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x80800000,
    0x3F800000, 0xBF800000, 0x3F7FFFFF, 0x40000000, 0x4B000000, 0x4F000000, 0xCF000000,
    0x7E800000, 0x7F000000, 0x7F7FFFFE, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000,
    0x7F800001, 0x7FC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x20000000, 0x1F800000, 0x5F800000,
};
#define N(a) (sizeof a / sizeof a[0])
static uint32_t pick_int(void) {
    switch (rnd32() % 4) {
    case 0: return k_int[rnd32() % N(k_int)];
    case 1: { const uint32_t p = 1u << (rnd32() % 32); return p + (rnd32() % 3) - 1; }
    case 2: return rnd32() >> (rnd32() % 32);
    default: return rnd32();
    }
}
static uint32_t pick_flt(void) {
    switch (rnd32() % 4) {
    case 0: return k_flt[rnd32() % N(k_flt)];
    case 1: { const uint32_t e = 1 + rnd32() % 254; return (rnd32() & 0x80000000u) | (e << 23) | (rnd32() & 0x7FFFFFu); }
    case 2: { const uint32_t e = (rnd32() & 1) ? 200 + rnd32() % 56 : rnd32() % 60; return (rnd32() & 0x80000000u) | (e << 23) | (rnd32() & 0x7FFFFFu); }
    default: return rnd32();
    }
}

static long long s_bad, s_checked, s_flagged;
static void diff(const char* what, int i8, const u128* in, const u128* got, const u128* want) {
    if (++s_bad <= 40)
        printf("MISMATCH %s i8=%d in=%08X %08X %08X %08X got=%08X %08X %08X %08X want=%08X %08X %08X %08X\n", what, i8,
               in->_u32[0], in->_u32[1], in->_u32[2], in->_u32[3], got->_u32[0], got->_u32[1], got->_u32[2], got->_u32[3],
               want->_u32[0], want->_u32[1], want->_u32[2], want->_u32[3]);
}
static int same(const u128* a, const u128* b) { return memcmp(a, b, 16) == 0; }
static int acc_ok(spu_spec_acc a) { return SPU_SPEC_OK(a); }
/* the per-op RZ forms' exact-path condition, computed from the raw native result */
static int rz_slow(__m128 raw) {
    const __m128i m = _mm_and_si128(_mm_castps_si128(raw), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF), m))) != 0xF;
}
static int cfl_slow(const u128* a) {
    for (int i = 0; i < 4; i++) if (((a->_u32[i] >> 23) & 0xFF) == 0xFF) return 1;
    return 0;
}

int main(int argc, char** argv) {
    const long n = argc > 1 ? atol(argv[1]) : 2000000;
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | SPU_MXCSR_RZFZ);
    g_spu_float_rz = 1;
    for (long it = 0; it < n; it++) {
        u128 a, b, c, got, want;
        /* ---- csflt / cuflt ---- */
        for (int i = 0; i < 4; i++) a._u32[i] = pick_int();
        const int i8 = (int)(rnd32() % 160);      /* 156..159: outside the vector form's range */
        got = spu_csflt(a, i8); want = (spu_csflt)(a, i8); s_checked++;
        if (!same(&got, &want)) diff("csflt", i8, &a, &got, &want);
        got = spu_cuflt(a, i8); want = (spu_cuflt)(a, i8); s_checked++;
        if (!same(&got, &want)) diff("cuflt", i8, &a, &got, &want);
        /* ---- speculative ops ---- */
        for (int i = 0; i < 4; i++) { a._u32[i] = pick_flt(); b._u32[i] = pick_flt(); c._u32[i] = pick_flt(); }
        const __m128 x = _mm_castsi128_ps(spu__xl(a)), y = _mm_castsi128_ps(spu__xl(b)), z = _mm_castsi128_ps(spu__xl(c));
        const __m128 nz = _mm_castsi128_ps(_mm_xor_si128(spu__xl(c), _mm_set1_epi32((int)0x80000000u)));
        const __m128 ny = _mm_castsi128_ps(_mm_xor_si128(spu__xl(b), _mm_set1_epi32((int)0x80000000u)));
        const __m128 nx = _mm_castsi128_ps(_mm_xor_si128(spu__xl(a), _mm_set1_epi32((int)0x80000000u)));
#define SPEC2(NAME, RAW) do { spu_spec_acc acc = SPU_SPEC_INIT(); got = spu_##NAME##_spec(a, b, &acc); \
            want = spu_##NAME##_lean(a, b); const int fl = !acc_ok(acc), sl = rz_slow(RAW); s_checked++; s_flagged += fl; \
            if (fl != sl) { if (++s_bad <= 40) printf("FLAG %s a=%08X b=%08X flag=%d slow=%d\n", #NAME, a._u32[0], b._u32[0], fl, sl); } \
            else if (!fl && !same(&got, &want)) diff(#NAME, 0, &a, &got, &want); } while (0)
#define SPEC3(NAME, RAW) do { spu_spec_acc acc = SPU_SPEC_INIT(); got = spu_##NAME##_spec(a, b, c, &acc); \
            want = spu_##NAME##_lean(a, b, c); const int fl = !acc_ok(acc), sl = rz_slow(RAW); s_checked++; s_flagged += fl; \
            if (fl != sl) { if (++s_bad <= 40) printf("FLAG %s a=%08X b=%08X c=%08X flag=%d slow=%d\n", #NAME, a._u32[0], b._u32[0], c._u32[0], fl, sl); } \
            else if (!fl && !same(&got, &want)) diff(#NAME, 0, &a, &got, &want); } while (0)
        SPEC2(fa, _mm_add_ps(x, y));
        SPEC2(fs, _mm_add_ps(x, ny));
        SPEC2(fm, _mm_mul_ps(x, y));
        SPEC3(fma, _mm_fmadd_ps(x, y, z));
        SPEC3(fms, _mm_fmadd_ps(x, y, nz));
        SPEC3(fnms, _mm_fmadd_ps(nx, y, z));
        /* cflts / cfltu: the per-op form's exact path is taken for an exponent-255 lane (or an
         * out-of-range scale, where the _spec form is the per-op form itself) */
        {
            const int k = (int)(rnd32() % 180);
            spu_spec_acc acc = SPU_SPEC_INIT();
            got = spu_cflts_spec(a, k, &acc); want = spu_cflts(a, k);
            const int inr = (173 - k) >= 0 && (173 - k) <= 127, fl = !acc_ok(acc), sl = inr && cfl_slow(&a);
            s_checked++; s_flagged += fl;
            if (fl != sl) { if (++s_bad <= 40) printf("FLAG cflts k=%d a=%08X flag=%d slow=%d\n", k, a._u32[0], fl, sl); }
            else if (!fl && !same(&got, &want)) diff("cflts", k, &a, &got, &want);
            acc = SPU_SPEC_INIT();
            got = spu_cfltu_spec(a, k, &acc); want = spu_cfltu(a, k);
            s_checked++;
            if (!acc_ok(acc) != sl) { if (++s_bad <= 40) printf("FLAG cfltu k=%d a=%08X\n", k, a._u32[0]); }
            else if (acc_ok(acc) && !same(&got, &want)) diff("cfltu", k, &a, &got, &want);
        }
    }
    /* a flag raised once stays raised over a run of ops (the accumulator) */
    {
        spu_spec_acc acc = SPU_SPEC_INIT(); u128 big, one;
        for (int i = 0; i < 4; i++) { big._u32[i] = 0x7F7FFFFF; one._u32[i] = 0x3F800000; }
        (void)spu_fm_spec(big, one, &acc);                 /* FLT_MAX: flagged (as the per-op form) */
        for (int k = 0; k < 8; k++) (void)spu_fa_spec(one, one, &acc);
        if (acc_ok(acc)) { s_bad++; puts("FLAG accumulator lost a raised verdict"); }
    }
    _mm_setcsr(csr);
    printf("test_spu_cg_x86: %lld checks, %lld flagged (exact path), %lld differ\n", s_checked, s_flagged, s_bad);
    return s_bad != 0;
}
#endif
