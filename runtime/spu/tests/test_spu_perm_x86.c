/* The x86 (SPU_X86_SIMD) byte rotates/shifts and cwd must be bit-identical to their scalar
 * references (spu_*_ref) for random operands and every shift amount -- test_spu_int_neon.c's
 * permute half, for the ops that have an x86 form. Build with clang-cl /O2 /arch:AVX2. */
#include "../spu_helpers.h"
#include <stdio.h>

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static u128 rnd128(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rnd32(); return r; }
static long s_bad;
static void check(const char* op, u128 got, u128 ref, u128 a, u128 b, int imm)
{
    if (!memcmp(&got, &ref, 16)) return;
    if (s_bad++ < 12)
        printf("MISMATCH %s a=%08X%08X%08X%08X b=%08X%08X%08X%08X imm=%d got=%08X%08X%08X%08X ref=%08X%08X%08X%08X\n",
               op, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], imm,
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
}
int main(void)
{
#if !SPU_X86_SIMD
    puts("test_spu_perm_x86: not an AVX2 build, nothing to compare");
    return 0;
#else
    const long N = 2000000;
    for (long it = 0; it < N; it++) {
        u128 a = rnd128(), b = rnd128();
        if (it & 1) b._u32[0] = rnd32() & 0xFF;          /* small shift counts */
        if (it % 3 == 0) b._u32[0] = (uint32_t)(int32_t)((int)(it % 600) - 300);
        int sh = (int)(it % 300) - 150;
#define C2(op) check(#op, spu_##op(a, b), spu_##op##_ref(a, b), a, b, 0)
#define CI(op, v) check(#op, spu_##op(a, v), spu_##op##_ref(a, v), a, b, v)
        CI(shlqbyi, sh); CI(rotqbyi, sh); CI(rotqmbyi, sh); CI(cwd, sh);
        C2(shlqby); C2(rotqby); C2(shlqbybi); C2(rotqbybi); C2(rotqmby); C2(rotqmbybi);
        check("cntb", spu_cntb(a), spu_cntb_ref(a), a, b, 0);
        { u128 c = rnd128();                      /* shufb: random controls, and SPU-typical ones */
          if (it & 1) for (int i = 0; i < 16; i++) c._u8[i] = (uint8_t)(rnd32() % 0x20);
          if (it % 5 == 0) for (int i = 0; i < 16; i++) c._u8[i] = (uint8_t)(0x80 | (rnd32() & 0x7F));
          check("shufb", spu_shufb(a, b, c), spu_shufb_ref(a, b, c), a, b, 0); }
    }
    if (s_bad) { printf("test_spu_perm_x86: %ld mismatches\n", s_bad); return 1; }
    printf("test_spu_perm_x86: ok (%ld random cases x 10 ops)\n", N);
    return 0;
#endif
}
