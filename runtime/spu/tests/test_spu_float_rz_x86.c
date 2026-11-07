/* SPU_FLOAT_RZ (x86): the native single-precision forms, run under MXCSR round-toward-zero +
 * FTZ + DAZ, against the exact forms in the default MXCSR -- bit for bit -- and every other
 * floating-point helper the lifted code calls, which must give the same bits in either mode
 * (double precision switches back to the default around itself).
 *
 *   clang-cl /O2 /arch:AVX2 /clang:-ffp-contract=off -I runtime/spu -I include test_spu_float_rz_x86.c
 *   test_spu_float_rz_x86 [vectors]      (default 3,000,000; exit 0 = no difference)
 *
 * Inputs mix uniformly random words with the cases that matter: zeros of both signs,
 * denormals (exponent 0: zero on the SPU), the extended range (exponent 255: ordinary large
 * numbers on the SPU, Inf/NaN to the host), FLT_MAX and its neighbours, results that overflow
 * past 2^128 or fall below 2^-126, close exponents (cancellation), and fma addends chosen to
 * cancel the product almost exactly. */
#include <stdio.h>
#include "spu_helpers.h"
#include <stdlib.h>
#include <string.h>


#if !SPU_X86_SIMD
int main(void) { puts("test_spu_float_rz_x86: needs the AVX2 x86 forms (build with /arch:AVX2)"); return 0; }
#else

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }
static uint64_t rnd64(void) { return ((uint64_t)rnd32() << 32) | rnd32(); }

static const uint32_t k_special[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00400000,
    0x00800000, 0x80800000, 0x00800001, 0x00FFFFFF, 0x01000000, 0x0B000000, 0x34000000,
    0x3F800000, 0xBF800000, 0x3F800001, 0x3F7FFFFF, 0x40000000, 0x4B000000, 0x4B7FFFFF,
    0x4F000000, 0x4F800000, 0xCF000000, 0x5F000000, 0x7E800000, 0x7F000000, 0x7F7FFFFE,
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7F800001, 0x7FC00000, 0x7FFFFFFF,
    0xFFFFFFFF, 0x3E800000, 0x20000000, 0x1F800000, 0x5F800000, 0x60000000, 0x61000000,
};
#define NSPECIAL (sizeof k_special / sizeof k_special[0])

static uint32_t near_exp(uint32_t base_e) {          /* a normal near 2^(base_e-127), random sign */
    int e = (int)base_e + (int)(rnd32() % 7) - 3;
    if (e < 1) e = 1;
    if (e > 254) e = 254;
    return (rnd32() & 0x80000000u) | ((uint32_t)e << 23) | (rnd32() & 0x7FFFFFu);
}
static uint32_t pick(int mode, uint32_t base_e) {
    switch (mode) {
    case 0: return rnd32();
    case 1: return k_special[rnd32() % NSPECIAL];
    case 2: return near_exp(base_e);
    default: return (rnd32() & 1) ? k_special[rnd32() % NSPECIAL] : near_exp(base_e);
    }
}
static void gen(u128* a, u128* b, u128* c) {
    const int mode = (int)(rnd32() % 5);
    for (int i = 0; i < 4; i++) {
        /* base exponents: anywhere, or pairs whose product lands at the top or bottom of the range */
        uint32_t ea = 1 + rnd32() % 254, eb = ea;
        const int region = (int)(rnd32() % 4);
        if (region == 1) { ea = 190 + rnd32() % 64; eb = 254 + 127 - ea + (rnd32() % 5) - 2; }   /* product ~2^127..2^128 */
        if (region == 2) { ea = 1 + rnd32() % 63;   eb = 127 + 1 - ea + (rnd32() % 5) - 2; }    /* product ~2^-126 */
        if (eb < 1) eb = 1;
        if (eb > 254) eb = 254;
        a->_u32[i] = pick(mode == 4 ? 2 : mode, ea);
        b->_u32[i] = pick(mode == 4 ? 2 : mode, eb);
        c->_u32[i] = pick(mode, (ea + eb > 127) ? ea + eb - 127 : 1);
        if (mode == 4) {                                 /* c cancels a*b to within a few ulps */
            float fa, fb; memcpy(&fa, &a->_u32[i], 4); memcpy(&fb, &b->_u32[i], 4);
            const float p = -(fa * fb);
            uint32_t w; memcpy(&w, &p, 4);
            w += (uint32_t)((int)(rnd32() % 9) - 4);
            c->_u32[i] = w;
        }
    }
}

enum { OP_FA, OP_FS, OP_FM, OP_FMA, OP_FMS, OP_FNMS, OP_CFLTS, OP_CFLTU, OP_CSFLT, OP_CUFLT,
       OP_FI, OP_FREST, OP_FRSQEST, OP_FCGT, OP_FCEQ, OP_FCMGT, OP_FCMEQ,
       OP_EFA, OP_EFM, OP_EFMA,
       OP_DFA, OP_DFS, OP_DFM, OP_DFMA, OP_DFMS, OP_DFNMS, OP_DFNMA, OP_FRDS, OP_FESD,
       OP_DFCEQ, OP_DFCMEQ, OP_DFCGT, OP_DFCMGT, OP_COUNT };
static const char* const k_name[OP_COUNT] = { "fa", "fs", "fm", "fma", "fms", "fnms", "cflts", "cfltu",
    "csflt", "cuflt", "fi", "frest", "frsqest", "fcgt", "fceq", "fcmgt", "fcmeq", "fa(exact)", "fm(exact)",
    "fma(exact)", "dfa", "dfs", "dfm", "dfma", "dfms", "dfnms", "dfnma", "frds", "fesd",
    "dfceq", "dfcmeq", "dfcgt", "dfcmgt" };

/* every op on one input triple; opaque, so the caller's MXCSR writes bracket all of it */
static __attribute__((noinline)) void eval_all(const u128* a, const u128* b, const u128* c,
                                               const u128* da, const u128* db, const u128* dc,
                                               int i8, u128* out)
{
    out[OP_FA] = spu_fa_lean(*a, *b);
    out[OP_FS] = spu_fs_lean(*a, *b);
    out[OP_FM] = spu_fm_lean(*a, *b);
    out[OP_FMA] = spu_fma_lean(*a, *b, *c);
    out[OP_FMS] = spu_fms_lean(*a, *b, *c);
    out[OP_FNMS] = spu_fnms_lean(*a, *b, *c);
    out[OP_CFLTS] = spu_cflts(*a, i8);
    out[OP_CFLTU] = spu_cfltu(*a, i8);
    out[OP_CSFLT] = spu_csflt(*a, i8);
    out[OP_CUFLT] = spu_cuflt(*a, i8);
    out[OP_FI] = spu_fi(*a, spu_frest(*b));
    out[OP_FREST] = spu_frest(*a);
    out[OP_FRSQEST] = spu_frsqest(*a);
    out[OP_FCGT] = spu_fcgt_lean(*a, *b);
    out[OP_FCEQ] = spu_fceq_lean(*a, *b);
    out[OP_FCMGT] = spu_fcmgt_lean(*a, *b);
    out[OP_FCMEQ] = spu_fcmeq_lean(*a, *b);
    out[OP_EFA] = spu_fa(*a, *b);
    out[OP_EFM] = spu_fm(*a, *b);
    out[OP_EFMA] = spu_fma(*a, *b, *c);
    out[OP_DFA] = spu_dfa(*da, *db);
    out[OP_DFS] = spu_dfs(*da, *db);
    out[OP_DFM] = spu_dfm(*da, *db);
    out[OP_DFMA] = spu_dfma(*da, *db, *dc);
    out[OP_DFMS] = spu_dfms(*da, *db, *dc);
    out[OP_DFNMS] = spu_dfnms(*da, *db, *dc);
    out[OP_DFNMA] = spu_dfnma(*da, *db, *dc);
    out[OP_FRDS] = spu_frds(*da);
    out[OP_FESD] = spu_fesd(*a);
    out[OP_DFCEQ] = spu_dfceq(*da, *db);
    out[OP_DFCMEQ] = spu_dfcmeq(*da, *db);
    out[OP_DFCGT] = spu_dfcgt(*da, *db);
    out[OP_DFCMGT] = spu_dfcmgt(*da, *db);
}

static uint64_t gen_d(void) {
    static const uint64_t sp[] = { 0, 0x8000000000000000ull, 1, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
        0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
        0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x3FF0000010000000ull, 0x3FF0000030000000ull,
        0x3FF00000F0000000ull, 0x47EFFFFFF0000000ull, 0x380FFFFFF0000000ull, 0x36A0000000000000ull };
    const uint32_t m = rnd32() % 4;
    if (m == 0) return sp[rnd32() % (sizeof sp / sizeof sp[0])];
    if (m == 1) return rnd64();
    /* near 1, with halfway / near-halfway low bits for frds rounding */
    uint64_t u = 0x3FF0000000000000ull | (rnd64() & 0x000FFFFFFFFFFFFFull);
    if (m == 3) u = (u & ~0x1FFFFFFFull) | (rnd32() % 3 == 0 ? 0x10000000ull : (rnd32() & 0x1FFFFFFFull));
    return u ^ ((uint64_t)(rnd32() & 1) << 63) ^ ((uint64_t)(rnd32() % 64) << 52);
}
static void put_d(u128* r, int i, uint64_t u) { r->_u32[2 * i] = (uint32_t)(u >> 32); r->_u32[2 * i + 1] = (uint32_t)u; }

int main(int argc, char** argv)
{
    const long n = argc > 1 ? atol(argv[1]) : 3000000;
    const unsigned csr0 = _mm_getcsr();
    long bad[OP_COUNT] = { 0 }, bad_default[OP_COUNT] = { 0 }, slow_lanes = 0, total_bad = 0;
    for (long it = 0; it < n; it++) {
        u128 a, b, c, da, db, dc;
        gen(&a, &b, &c);
        for (int i = 0; i < 2; i++) { put_d(&da, i, gen_d()); put_d(&db, i, gen_d()); put_d(&dc, i, gen_d()); }
        const int i8 = (int)(rnd32() % 256);
        u128 ref[OP_COUNT], got[OP_COUNT];
        g_spu_float_rz = 0;
        _mm_setcsr(csr0);
        eval_all(&a, &b, &c, &da, &db, &dc, i8, ref);
        g_spu_float_rz = 1;
        _mm_setcsr(csr0 | SPU_MXCSR_RZFZ);
        eval_all(&a, &b, &c, &da, &db, &dc, i8, got);
        _mm_setcsr(csr0);
        g_spu_float_rz = 0;
        /* the six lean ops: both forms against the exact integer definition (spu__*_lane);
         * every other op: the round-toward-zero run against the default-mode run */
        u128 truth[6];
        for (int i = 0; i < 4; i++) {
            const uint32_t x = a._u32[i], y = b._u32[i], z = c._u32[i];
            truth[OP_FA]._u32[i] = spu__fa_lane(x, y);
            truth[OP_FS]._u32[i] = spu__fa_lane(x, y ^ 0x80000000u);
            truth[OP_FM]._u32[i] = spu__fm_lane(x, y);
            truth[OP_FMA]._u32[i] = spu__fma_lane(x, y, z, 0, 0);
            truth[OP_FMS]._u32[i] = spu__fma_lane(x, y, z, 0, 0x80000000u);
            truth[OP_FNMS]._u32[i] = spu__fma_lane(x, y, z, 0x80000000u, 0);
        }
        for (int op = 0; op <= OP_FNMS; op++)
            if (memcmp(&ref[op], &truth[op], 16) && ++bad_default[op] <= 2)
                printf("DEFAULT-MODE %-6s differs from the integer definition: a=%08X %08X %08X %08X b=%08X %08X %08X %08X\n",
                       k_name[op], a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3]);
        for (int op = 0; op <= OP_FNMS; op++) ref[op] = truth[op];
        for (int op = 0; op < OP_COUNT; op++) {
            if (!memcmp(&ref[op], &got[op], 16)) continue;
            bad[op]++; total_bad++;
            if (bad[op] <= 4)
                printf("MISMATCH %-10s a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X %08X %08X %08X i8=%d\n"
                       "           ref=%08X %08X %08X %08X got=%08X %08X %08X %08X\n",
                       k_name[op], a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2],
                       b._u32[3], c._u32[0], c._u32[1], c._u32[2], c._u32[3], i8,
                       ref[op]._u32[0], ref[op]._u32[1], ref[op]._u32[2], ref[op]._u32[3],
                       got[op]._u32[0], got[op]._u32[1], got[op]._u32[2], got[op]._u32[3]);
        }
        /* how often the RZ forms leave the fast path (exponent 255 / overflow lanes) */
        for (int i = 0; i < 4; i++) {
            const uint32_t e = (got[OP_FMA]._u32[i] >> 23) & 0xFF;
            slow_lanes += e == 0xFF;
        }
    }
    _mm_setcsr(csr0);
    printf("test_spu_float_rz_x86: %ld vectors x %d ops, %ld mismatches (extended-range fma results: %ld lanes)\n",
           n, OP_COUNT, total_bad, slow_lanes);
    for (int op = 0; op < OP_COUNT; op++)
        if (bad[op]) printf("  %-10s %ld\n", k_name[op], bad[op]);
    long dbad = 0;
    for (int op = 0; op <= OP_FNMS; op++) dbad += bad_default[op];
    printf("default-mode lean forms vs the integer definition: %ld mismatches\n", dbad);
    return (total_bad || dbad) ? 1 : 0;
}
#endif
