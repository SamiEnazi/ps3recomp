/* The x86 vector forms of the integer ops (spu_helpers_x86int.h) against the scalar
 * definitions in spu_helpers.h, bit for bit: random and edge operands, every shift count /
 * immediate the ops accept, and for csflt/cuflt every scale under the SPU_FLOAT_RZ MXCSR.
 *
 *   clang-cl /O2 /arch:AVX2 /clang:-ffp-contract=off -I runtime/spu -I include test_spu_x86int.c
 *   test_spu_x86int [vectors]      (default 2,000,000; exit 0 = no difference)
 *
 * (spu_X)(...) is the scalar definition: the forms' macros are function-like. */
#include <stdio.h>
#include "spu_helpers.h"
#include <stdlib.h>
#include <string.h>

#if !SPU_X86_SIMD
int main(void) { puts("test_spu_x86int: needs the AVX2 x86 forms (build with /arch:AVX2)"); return 0; }
#else

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd32(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }
static const uint32_t k_edge[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
                                   0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x00FFFFFF, 0x01000000, 0xFFFFFF80u,
                                   31, 32, 33, 63, 64, 0xFFFFFFE0u, 0xFFFFFFC1u, 0x1F, 0x3F, 0x40, 0xC0, 0x8080, 0x80808080u };
#define NEDGE (sizeof k_edge / sizeof k_edge[0])
static uint32_t word(void) {
    const uint32_t k = rnd32() % 8;
    if (k == 0) return k_edge[rnd32() % NEDGE];
    if (k == 1) return rnd32() & 0xFF;           /* small counts */
    if (k == 2) return (uint32_t)-(int32_t)(rnd32() & 0x7F);
    if (k == 3) return rnd32() >> (rnd32() % 32);   /* every magnitude (csflt/cuflt) */
    if (k == 4) return 0u - (rnd32() >> (rnd32() % 32));
    return rnd32();
}
static u128 vec(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = word(); return r; }

static long s_bad, s_checked;
static void cmp(const char* op, int imm, u128 got, u128 ref, u128 a, u128 b) {
    s_checked++;
    if (!memcmp(&got, &ref, 16)) return;
    if (++s_bad <= 40)
        printf("MISMATCH %-7s imm=%d a=%08X %08X %08X %08X b=%08X %08X %08X %08X got=%08X %08X %08X %08X ref=%08X %08X %08X %08X\n",
               op, imm, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3],
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
}
#define T2(op) cmp(#op, 0, spu_##op(a, b), (spu_##op)(a, b), a, b)
#define T3(op) cmp(#op, 0, spu_##op(a, b, c), (spu_##op)(a, b, c), a, b)
#define TI(op, imm) cmp(#op, (int)(imm), spu_##op(a, imm), (spu_##op)(a, imm), a, b)

int main(int argc, char** argv) {
    const long n = argc > 1 ? atol(argv[1]) : 2000000;
    for (long it = 0; it < n; it++) {
        const u128 a = vec(), b = vec(), c = vec();
        const int32_t imm = (int32_t)word();
        const int i10 = (int)(rnd32() & 0x3FF) - 512, i7 = (int)(rnd32() & 0x7F) - 64;
        T2(a); T2(sf); T2(ah); T2(sfh); T2(and); T2(or); T2(xor); T2(andc); T2(orc); T2(nand); T2(nor); T2(eqv);
        T2(ceq); T2(cgt); T2(clgt); T2(ceqh); T2(cgth); T2(clgth); T2(ceqb); T2(cgtb); T2(clgtb);
        T2(shl); T2(rotm); T2(rotma); T2(rot); T2(mpy); T2(mpyu); T2(mpyh); T2(mpyhh);
        T3(selb); T3(mpya);
        TI(ai, i10); TI(sfi, i10); TI(ahi, i10); TI(sfhi, i10); TI(andi, i10); TI(ori, i10); TI(xori, i10);
        TI(ai, imm); TI(andi, imm); TI(ori, imm);
        TI(iohl, (uint16_t)imm); TI(andhi, i10); TI(orhi, i10); TI(andbi, imm & 0xFF); TI(orbi, imm & 0xFF); TI(xorbi, imm & 0xFF);
        TI(ceqi, i10); TI(cgti, i10); TI(clgti, i10); TI(ceqhi, i10); TI(cgthi, i10); TI(clgthi, i10);
        TI(ceqbi, imm & 0xFF); TI(cgtbi, imm & 0xFF); TI(clgtbi, imm & 0xFF);
        TI(shli, i7); TI(rotmi, i7); TI(rotmai, i7); TI(roti, i7); TI(shlhi, i7); TI(rothmi, i7);
        TI(shli, it & 0x7F); TI(rotmi, -(int)(it & 0x7F)); TI(rotmai, -(int)(it & 0x7F)); TI(roti, it & 0x7F);
        T2(shlqbi); T2(rotqbi); T2(rotqmbi); TI(shlqbii, i7); TI(rotqbii, i7); TI(rotqmbii, i7);
        cmp("frest", 0, spu_frest(a), (spu_frest)(a), a, a); cmp("frsqest", 0, spu_frsqest(a), (spu_frsqest)(a), a, a);
        cmp("xsbh", 0, spu_xsbh(a), (spu_xsbh)(a), a, a); cmp("xshw", 0, spu_xshw(a), (spu_xshw)(a), a, a);
    }
    /* csflt / cuflt under the SPU's MXCSR (round toward zero + FTZ + DAZ), every scale */
    long nf = n / 8;
    const unsigned old = _mm_getcsr();
    g_spu_float_rz = 1;
    _mm_setcsr((old & ~0x6000u) | SPU_MXCSR_RZFZ);
    for (long it = 0; it < nf; it++) {
        const u128 a = vec(), b = a;
        for (int sc = -100; sc <= 155; sc += (it & 1) ? 1 : 9) {
            const int i8 = (155 - sc) | (int)(rnd32() & ~0xFFu);   /* the op reads the low 8 bits */
            TI(csflt, i8); TI(cuflt, i8);
        }
    }
    _mm_setcsr(old);
    printf("test_spu_x86int: %ld comparisons, %ld differ\n", s_checked, s_bad);
    return s_bad ? 1 : 0;
}
#endif
