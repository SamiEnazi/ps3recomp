/* x86 (AVX2) forms of the SPU integer ops that spu_helpers.h defines as scalar loops on x86.
 *
 * Included at the END of spu_helpers.h when SPU_X86_SIMD is on (an AVX2 TU) and SPU_X86_INT is
 * not 0: each op below is a vector form of the scalar definition with the same bits, and a
 * macro points spu_X at it for the code that follows (the lifted SPU code). The scalar
 * definitions stay as they are -- they are the reference that tests/test_spu_x86int.c compares
 * every form with (random and edge operands, all shift counts and immediates). The macros are
 * function-like, so (spu_X)(...) still names the scalar definition.
 *
 * Why: in the per-instruction memory form (ctx->gpr[n]) clang's SLP vectorizer turns most of the
 * scalar loops into vector code, but not all (rotm/rotma/shl variable shifts, csflt/cuflt, the
 * u64-pair forms of and/or/selb), and when registers live in C locals (SPU_LIFT_REG_LOCALS)
 * the u64/u32 lane loops make SROA split each register into scalars -- the R form spent its time
 * spilling 64-bit halves (2026-11-08, the Edge geometry job). */
#ifndef SPU_HELPERS_X86INT_H
#define SPU_HELPERS_X86INT_H

#define SPU__XI static inline __attribute__((always_inline))

/* ---- word / halfword arithmetic ---- */
SPU__XI u128 spu__xi_a(u128 a, u128 b)   { return spu__xs(_mm_add_epi32(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_sf(u128 a, u128 b)  { return spu__xs(_mm_sub_epi32(spu__xl(b), spu__xl(a))); }
SPU__XI u128 spu__xi_ai(u128 a, int32_t imm)  { return spu__xs(_mm_add_epi32(spu__xl(a), _mm_set1_epi32(imm))); }
SPU__XI u128 spu__xi_sfi(u128 a, int32_t imm) { return spu__xs(_mm_sub_epi32(_mm_set1_epi32(imm), spu__xl(a))); }
SPU__XI u128 spu__xi_ah(u128 a, u128 b)  { return spu__xs(_mm_add_epi16(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_sfh(u128 a, u128 b) { return spu__xs(_mm_sub_epi16(spu__xl(b), spu__xl(a))); }
SPU__XI u128 spu__xi_ahi(u128 a, int32_t imm)  { return spu__xs(_mm_add_epi16(spu__xl(a), _mm_set1_epi16((short)imm))); }
SPU__XI u128 spu__xi_sfhi(u128 a, int32_t imm) { return spu__xs(_mm_sub_epi16(_mm_set1_epi16((short)imm), spu__xl(a))); }

/* ---- logical ---- */
SPU__XI u128 spu__xi_and(u128 a, u128 b)  { return spu__xs(_mm_and_si128(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_or(u128 a, u128 b)   { return spu__xs(_mm_or_si128(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_xor(u128 a, u128 b)  { return spu__xs(_mm_xor_si128(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_andc(u128 a, u128 b) { return spu__xs(_mm_andnot_si128(spu__xl(b), spu__xl(a))); }
SPU__XI u128 spu__xi_orc(u128 a, u128 b)  { return spu__xs(_mm_or_si128(spu__xl(a), _mm_xor_si128(spu__xl(b), _mm_set1_epi32(-1)))); }
SPU__XI u128 spu__xi_nand(u128 a, u128 b) { return spu__xs(_mm_xor_si128(_mm_and_si128(spu__xl(a), spu__xl(b)), _mm_set1_epi32(-1))); }
SPU__XI u128 spu__xi_nor(u128 a, u128 b)  { return spu__xs(_mm_xor_si128(_mm_or_si128(spu__xl(a), spu__xl(b)), _mm_set1_epi32(-1))); }
SPU__XI u128 spu__xi_eqv(u128 a, u128 b)  { return spu__xs(_mm_xor_si128(_mm_xor_si128(spu__xl(a), spu__xl(b)), _mm_set1_epi32(-1))); }
SPU__XI u128 spu__xi_andi(u128 a, int32_t imm) { return spu__xs(_mm_and_si128(spu__xl(a), _mm_set1_epi32(imm))); }
SPU__XI u128 spu__xi_ori(u128 a, int32_t imm)  { return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi32(imm))); }
SPU__XI u128 spu__xi_xori(u128 a, int32_t imm) { return spu__xs(_mm_xor_si128(spu__xl(a), _mm_set1_epi32(imm))); }
SPU__XI u128 spu__xi_iohl(u128 a, uint16_t imm) { return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi32((int)(uint32_t)imm))); }
SPU__XI u128 spu__xi_andhi(u128 a, int32_t imm) { return spu__xs(_mm_and_si128(spu__xl(a), _mm_set1_epi16((short)imm))); }
SPU__XI u128 spu__xi_orhi(u128 a, int32_t imm)  { return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi16((short)imm))); }
SPU__XI u128 spu__xi_andbi(u128 a, int32_t imm) { return spu__xs(_mm_and_si128(spu__xl(a), _mm_set1_epi8((char)imm))); }
SPU__XI u128 spu__xi_orbi(u128 a, int32_t imm)  { return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi8((char)imm))); }
SPU__XI u128 spu__xi_xorbi(u128 a, int32_t imm) { return spu__xs(_mm_xor_si128(spu__xl(a), _mm_set1_epi8((char)imm))); }
SPU__XI u128 spu__xi_selb(u128 a, u128 b, u128 c) {
    const __m128i C = spu__xl(c);
    return spu__xs(_mm_or_si128(_mm_andnot_si128(C, spu__xl(a)), _mm_and_si128(C, spu__xl(b))));
}

/* ---- compares (an all-ones lane is true) ---- */
#define SPU__XI_U32BIAS _mm_set1_epi32((int)0x80000000u)
#define SPU__XI_U16BIAS _mm_set1_epi16((short)0x8000)
#define SPU__XI_U8BIAS  _mm_set1_epi8((char)0x80)
SPU__XI u128 spu__xi_ceq(u128 a, u128 b)  { return spu__xs(_mm_cmpeq_epi32(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_cgt(u128 a, u128 b)  { return spu__xs(_mm_cmpgt_epi32(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_clgt(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi32(_mm_xor_si128(spu__xl(a), SPU__XI_U32BIAS), _mm_xor_si128(spu__xl(b), SPU__XI_U32BIAS))); }
SPU__XI u128 spu__xi_ceqi(u128 a, int32_t imm)  { return spu__xs(_mm_cmpeq_epi32(spu__xl(a), _mm_set1_epi32(imm))); }
SPU__XI u128 spu__xi_cgti(u128 a, int32_t imm)  { return spu__xs(_mm_cmpgt_epi32(spu__xl(a), _mm_set1_epi32(imm))); }
SPU__XI u128 spu__xi_clgti(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi32(_mm_xor_si128(spu__xl(a), SPU__XI_U32BIAS), _mm_xor_si128(_mm_set1_epi32(imm), SPU__XI_U32BIAS))); }
SPU__XI u128 spu__xi_ceqh(u128 a, u128 b)  { return spu__xs(_mm_cmpeq_epi16(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_cgth(u128 a, u128 b)  { return spu__xs(_mm_cmpgt_epi16(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_clgth(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi16(_mm_xor_si128(spu__xl(a), SPU__XI_U16BIAS), _mm_xor_si128(spu__xl(b), SPU__XI_U16BIAS))); }
SPU__XI u128 spu__xi_ceqhi(u128 a, int32_t imm)  { return spu__xs(_mm_cmpeq_epi16(spu__xl(a), _mm_set1_epi16((short)imm))); }
SPU__XI u128 spu__xi_cgthi(u128 a, int32_t imm)  { return spu__xs(_mm_cmpgt_epi16(spu__xl(a), _mm_set1_epi16((short)imm))); }
SPU__XI u128 spu__xi_clgthi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi16(_mm_xor_si128(spu__xl(a), SPU__XI_U16BIAS), _mm_xor_si128(_mm_set1_epi16((short)imm), SPU__XI_U16BIAS))); }
SPU__XI u128 spu__xi_ceqb(u128 a, u128 b)  { return spu__xs(_mm_cmpeq_epi8(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_cgtb(u128 a, u128 b)  { return spu__xs(_mm_cmpgt_epi8(spu__xl(a), spu__xl(b))); }
SPU__XI u128 spu__xi_clgtb(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi8(_mm_xor_si128(spu__xl(a), SPU__XI_U8BIAS), _mm_xor_si128(spu__xl(b), SPU__XI_U8BIAS))); }
SPU__XI u128 spu__xi_ceqbi(u128 a, int32_t imm)  { return spu__xs(_mm_cmpeq_epi8(spu__xl(a), _mm_set1_epi8((char)imm))); }
SPU__XI u128 spu__xi_cgtbi(u128 a, int32_t imm)  { return spu__xs(_mm_cmpgt_epi8(spu__xl(a), _mm_set1_epi8((char)imm))); }
SPU__XI u128 spu__xi_clgtbi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi8(_mm_xor_si128(spu__xl(a), SPU__XI_U8BIAS), _mm_xor_si128(_mm_set1_epi8((char)imm), SPU__XI_U8BIAS))); }

/* ---- word shifts and rotates. SSE/AVX2 shifts by 32 or more give 0 (logical) or the sign fill
 * (arithmetic), which is the SPU's rule for counts 32..63. ---- */
SPU__XI u128 spu__xi_shli(u128 a, int sh)  { return spu__xs(_mm_sll_epi32(spu__xl(a), _mm_cvtsi32_si128(sh & 0x3F))); }
SPU__XI u128 spu__xi_rotmi(u128 a, int i7) { return spu__xs(_mm_srl_epi32(spu__xl(a), _mm_cvtsi32_si128((0 - i7) & 0x3F))); }
SPU__XI u128 spu__xi_rotmai(u128 a, int i7){ return spu__xs(_mm_sra_epi32(spu__xl(a), _mm_cvtsi32_si128((0 - i7) & 0x3F))); }
SPU__XI u128 spu__xi_roti(u128 a, int sh) {
    const __m128i x = spu__xl(a); sh &= 31;   /* sh = 0: the right shift by 32 is 0 */
    return spu__xs(_mm_or_si128(_mm_sll_epi32(x, _mm_cvtsi32_si128(sh)), _mm_srl_epi32(x, _mm_cvtsi32_si128(32 - sh))));
}
SPU__XI u128 spu__xi_shl(u128 a, u128 b)  { return spu__xs(_mm_sllv_epi32(spu__xl(a), _mm_and_si128(spu__xl(b), _mm_set1_epi32(0x3F)))); }
SPU__XI u128 spu__xi_rotm(u128 a, u128 b) {
    return spu__xs(_mm_srlv_epi32(spu__xl(a), _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), spu__xl(b)), _mm_set1_epi32(0x3F)))); }
SPU__XI u128 spu__xi_rotma(u128 a, u128 b) {
    return spu__xs(_mm_srav_epi32(spu__xl(a), _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), spu__xl(b)), _mm_set1_epi32(0x3F)))); }
SPU__XI u128 spu__xi_rot(u128 a, u128 b) {
    const __m128i x = spu__xl(a), n = _mm_and_si128(spu__xl(b), _mm_set1_epi32(31));
    return spu__xs(_mm_or_si128(_mm_sllv_epi32(x, n), _mm_srlv_epi32(x, _mm_sub_epi32(_mm_set1_epi32(32), n))));
}
/* halfword immediate shifts (counts 16..31 give 0 / the sign fill, as the SPU) */
SPU__XI u128 spu__xi_shlhi(u128 a, int sh)   { return spu__xs(_mm_sll_epi16(spu__xl(a), _mm_cvtsi32_si128(sh & 0x1F))); }
SPU__XI u128 spu__xi_rothmi(u128 a, int i7)  { return spu__xs(_mm_srl_epi16(spu__xl(a), _mm_cvtsi32_si128((0 - i7) & 0x1F))); }

/* ---- quadword shifts / rotates by 0..7 bits. Lane i is SPU word i (word 0 the most
 * significant), so the bits that cross into lane i come from lane i+1 (left) or i-1 (right);
 * a count of 0 shifts the neighbour by 32, which is 0. ---- */
SPU__XI __m128i spu__xi_qbl(__m128i x, __m128i next, int sh) {
    return _mm_or_si128(_mm_sll_epi32(x, _mm_cvtsi32_si128(sh)), _mm_srl_epi32(next, _mm_cvtsi32_si128(32 - sh))); }
SPU__XI __m128i spu__xi_qbr(__m128i x, int sh) {
    return _mm_or_si128(_mm_srl_epi32(x, _mm_cvtsi32_si128(sh)), _mm_sll_epi32(_mm_slli_si128(x, 4), _mm_cvtsi32_si128(32 - sh))); }
SPU__XI u128 spu__xi_shlqbi(u128 a, u128 b)  { const __m128i x = spu__xl(a); return spu__xs(spu__xi_qbl(x, _mm_srli_si128(x, 4), (int)(b._u32[0] & 7))); }
SPU__XI u128 spu__xi_shlqbii(u128 a, int sh) { const __m128i x = spu__xl(a); return spu__xs(spu__xi_qbl(x, _mm_srli_si128(x, 4), sh & 7)); }
SPU__XI u128 spu__xi_rotqbi(u128 a, u128 b)  {
    const __m128i x = spu__xl(a); return spu__xs(spu__xi_qbl(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(0, 3, 2, 1)), (int)(b._u32[0] & 7))); }
SPU__XI u128 spu__xi_rotqbii(u128 a, int sh) {
    const __m128i x = spu__xl(a); return spu__xs(spu__xi_qbl(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(0, 3, 2, 1)), sh & 7)); }
SPU__XI u128 spu__xi_rotqmbi(u128 a, u128 b) { return spu__xs(spu__xi_qbr(spu__xl(a), (0 - (int)b._u32[0]) & 7)); }
SPU__XI u128 spu__xi_rotqmbii(u128 a, int i7) { return spu__xs(spu__xi_qbr(spu__xl(a), (0 - i7) & 7)); }

/* ---- reciprocal (square root) estimates: the same tables, gathered ---- */
SPU__XI u128 spu__xi_frest(u128 a) {
    const __m128i x = spu__xl(a);
    const __m128i e = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    const __m128i fr = _mm_i32gather_epi32((const int*)spu_frest_fraction_lut, _mm_and_si128(_mm_srli_epi32(x, 18), _mm_set1_epi32(0x1F)), 4);
    __m128i re = _mm_sub_epi32(_mm_set1_epi32(253), e);
    re = _mm_andnot_si128(_mm_cmpgt_epi32(e, _mm_set1_epi32(252)), re);              /* exp >= 253: 0 */
    re = _mm_blendv_epi8(re, _mm_set1_epi32(255), _mm_cmpeq_epi32(e, _mm_setzero_si128()));
    return spu__xs(_mm_or_si128(_mm_or_si128(_mm_and_si128(x, _mm_set1_epi32((int)0x80000000u)), _mm_slli_epi32(re, 23)),
                                _mm_and_si128(fr, _mm_set1_epi32(0x7FFFFF))));
}
SPU__XI u128 spu__xi_frsqest(u128 a) {
    const __m128i x = spu__xl(a);
    const __m128i e = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    const __m128i fr = _mm_i32gather_epi32((const int*)spu_frsqest_fraction_lut, _mm_and_si128(_mm_srli_epi32(x, 18), _mm_set1_epi32(0x3F)), 4);
    __m128i re = _mm_sub_epi32(_mm_set1_epi32(190), _mm_srli_epi32(_mm_add_epi32(e, _mm_set1_epi32(1)), 1));
    re = _mm_blendv_epi8(re, _mm_set1_epi32(0xFF), _mm_cmpeq_epi32(e, _mm_setzero_si128()));
    return spu__xs(_mm_or_si128(_mm_slli_epi32(re, 23), _mm_and_si128(fr, _mm_set1_epi32(0x7FFFFF))));
}

/* ---- sign extension ---- */
SPU__XI u128 spu__xi_xsbh(u128 a) { return spu__xs(_mm_srai_epi16(_mm_slli_epi16(spu__xl(a), 8), 8)); }
SPU__XI u128 spu__xi_xshw(u128 a) { return spu__xs(_mm_srai_epi32(_mm_slli_epi32(spu__xl(a), 16), 16)); }

/* ---- 16 x 16 multiplies (the low/high halfword of each word) ---- */
SPU__XI u128 spu__xi_mpy(u128 a, u128 b) {
    return spu__xs(_mm_mullo_epi32(_mm_srai_epi32(_mm_slli_epi32(spu__xl(a), 16), 16), _mm_srai_epi32(_mm_slli_epi32(spu__xl(b), 16), 16))); }
SPU__XI u128 spu__xi_mpyu(u128 a, u128 b) {
    const __m128i m = _mm_set1_epi32(0xFFFF);
    return spu__xs(_mm_mullo_epi32(_mm_and_si128(spu__xl(a), m), _mm_and_si128(spu__xl(b), m))); }
SPU__XI u128 spu__xi_mpya(u128 a, u128 b, u128 c) { return spu__xs(_mm_add_epi32(spu__xl(spu__xi_mpy(a, b)), spu__xl(c))); }
SPU__XI u128 spu__xi_mpyh(u128 a, u128 b) {   /* (a.hi * b.lo) << 16 */
    return spu__xs(_mm_slli_epi32(_mm_mullo_epi32(_mm_srai_epi32(spu__xl(a), 16), spu__xl(b)), 16)); }
SPU__XI u128 spu__xi_mpyhh(u128 a, u128 b) { return spu__xs(_mm_mullo_epi32(_mm_srai_epi32(spu__xl(a), 16), _mm_srai_epi32(spu__xl(b), 16))); }

/* ---- integer -> single (csflt / cuflt), with the SPU_FLOAT_RZ MXCSR (round toward zero): the
 * conversion truncates to 24 bits as the scalar form does, and the 2^-scale is applied to the
 * exponent field (a result below 2^-126 is +0, as the SPU's). Outside that mode: the scalar
 * definition. ---- */
#if defined(SPU_X64_MXCSR) && SPU_X64_MXCSR
SPU__XI __m128i spu__xi_scale(__m128 f, int i8) {
    const int sc = 155 - (i8 & 0xFF);                       /* the result is f * 2^-sc */
    const __m128i fb = _mm_castps_si128(f);
    const __m128i e = _mm_srli_epi32(_mm_slli_epi32(fb, 1), 24);   /* biased exponent; 0 for 0 */
    const __m128i keep = _mm_cmpgt_epi32(e, _mm_set1_epi32(sc));   /* the scaled exponent >= 1 */
    return _mm_and_si128(_mm_sub_epi32(fb, _mm_set1_epi32(sc << 23)), keep);
}
SPU__XI u128 spu__xi_csflt(u128 a, int i8) {
    if (!SPU__RZ_ON || (i8 & 0xFF) > 155) return spu_csflt(a, i8);   /* a negative scale can overflow */
    __m128i x = spu__xl(a);
    /* (critic) opaque to the compiler: a constant input would be converted at compile time
     * with round-to-nearest, not the RZ MXCSR (0x7FFFFFFD, 155 -> 0x4F000000, not 0x4EFFFFFF) */
    __asm__("" : "+x"(x));
    return spu__xs(spu__xi_scale(_mm_cvtepi32_ps(x), i8));
}
SPU__XI u128 spu__xi_cuflt(u128 a, int i8) {
    if (!SPU__RZ_ON || (i8 & 0xFF) > 155) return spu_cuflt(a, i8);
    const __m128i x = spu__xl(a);
    const __m128i big = _mm_srai_epi32(x, 31);                     /* >= 2^31: convert x/2, then *2 */
    __m128i v = _mm_blendv_epi8(x, _mm_srli_epi32(x, 1), big);
    __asm__("" : "+x"(v));   /* (critic) no compile-time (round-to-nearest) conversion of a constant */
    const __m128i f = _mm_add_epi32(_mm_castps_si128(_mm_cvtepi32_ps(v)), _mm_and_si128(big, _mm_set1_epi32(1 << 23)));
    return spu__xs(spu__xi_scale(_mm_castsi128_ps(f), i8));
}
#define spu_csflt(...) spu__xi_csflt(__VA_ARGS__)
#define spu_cuflt(...) spu__xi_cuflt(__VA_ARGS__)
#endif

#define spu_a(...) spu__xi_a(__VA_ARGS__)
#define spu_sf(...) spu__xi_sf(__VA_ARGS__)
#define spu_ai(...) spu__xi_ai(__VA_ARGS__)
#define spu_sfi(...) spu__xi_sfi(__VA_ARGS__)
#define spu_ah(...) spu__xi_ah(__VA_ARGS__)
#define spu_sfh(...) spu__xi_sfh(__VA_ARGS__)
#define spu_ahi(...) spu__xi_ahi(__VA_ARGS__)
#define spu_sfhi(...) spu__xi_sfhi(__VA_ARGS__)
#define spu_and(...) spu__xi_and(__VA_ARGS__)
#define spu_or(...) spu__xi_or(__VA_ARGS__)
#define spu_xor(...) spu__xi_xor(__VA_ARGS__)
#define spu_andc(...) spu__xi_andc(__VA_ARGS__)
#define spu_orc(...) spu__xi_orc(__VA_ARGS__)
#define spu_nand(...) spu__xi_nand(__VA_ARGS__)
#define spu_nor(...) spu__xi_nor(__VA_ARGS__)
#define spu_eqv(...) spu__xi_eqv(__VA_ARGS__)
#define spu_andi(...) spu__xi_andi(__VA_ARGS__)
#define spu_ori(...) spu__xi_ori(__VA_ARGS__)
#define spu_xori(...) spu__xi_xori(__VA_ARGS__)
#define spu_iohl(...) spu__xi_iohl(__VA_ARGS__)
#define spu_andhi(...) spu__xi_andhi(__VA_ARGS__)
#define spu_orhi(...) spu__xi_orhi(__VA_ARGS__)
#define spu_andbi(...) spu__xi_andbi(__VA_ARGS__)
#define spu_orbi(...) spu__xi_orbi(__VA_ARGS__)
#define spu_xorbi(...) spu__xi_xorbi(__VA_ARGS__)
#define spu_selb(...) spu__xi_selb(__VA_ARGS__)
#define spu_ceq(...) spu__xi_ceq(__VA_ARGS__)
#define spu_cgt(...) spu__xi_cgt(__VA_ARGS__)
#define spu_clgt(...) spu__xi_clgt(__VA_ARGS__)
#define spu_ceqi(...) spu__xi_ceqi(__VA_ARGS__)
#define spu_cgti(...) spu__xi_cgti(__VA_ARGS__)
#define spu_clgti(...) spu__xi_clgti(__VA_ARGS__)
#define spu_ceqh(...) spu__xi_ceqh(__VA_ARGS__)
#define spu_cgth(...) spu__xi_cgth(__VA_ARGS__)
#define spu_clgth(...) spu__xi_clgth(__VA_ARGS__)
#define spu_ceqhi(...) spu__xi_ceqhi(__VA_ARGS__)
#define spu_cgthi(...) spu__xi_cgthi(__VA_ARGS__)
#define spu_clgthi(...) spu__xi_clgthi(__VA_ARGS__)
#define spu_ceqb(...) spu__xi_ceqb(__VA_ARGS__)
#define spu_cgtb(...) spu__xi_cgtb(__VA_ARGS__)
#define spu_clgtb(...) spu__xi_clgtb(__VA_ARGS__)
#define spu_ceqbi(...) spu__xi_ceqbi(__VA_ARGS__)
#define spu_cgtbi(...) spu__xi_cgtbi(__VA_ARGS__)
#define spu_clgtbi(...) spu__xi_clgtbi(__VA_ARGS__)
#define spu_shli(...) spu__xi_shli(__VA_ARGS__)
#define spu_rotmi(...) spu__xi_rotmi(__VA_ARGS__)
#define spu_rotmai(...) spu__xi_rotmai(__VA_ARGS__)
#define spu_roti(...) spu__xi_roti(__VA_ARGS__)
#define spu_shl(...) spu__xi_shl(__VA_ARGS__)
#define spu_rotm(...) spu__xi_rotm(__VA_ARGS__)
#define spu_rotma(...) spu__xi_rotma(__VA_ARGS__)
#define spu_rot(...) spu__xi_rot(__VA_ARGS__)
#define spu_shlhi(...) spu__xi_shlhi(__VA_ARGS__)
#define spu_rothmi(...) spu__xi_rothmi(__VA_ARGS__)
#define spu_shlqbi(...) spu__xi_shlqbi(__VA_ARGS__)
#define spu_shlqbii(...) spu__xi_shlqbii(__VA_ARGS__)
#define spu_rotqbi(...) spu__xi_rotqbi(__VA_ARGS__)
#define spu_rotqbii(...) spu__xi_rotqbii(__VA_ARGS__)
#define spu_rotqmbi(...) spu__xi_rotqmbi(__VA_ARGS__)
#define spu_rotqmbii(...) spu__xi_rotqmbii(__VA_ARGS__)
#define spu_frest(...) spu__xi_frest(__VA_ARGS__)
#define spu_frsqest(...) spu__xi_frsqest(__VA_ARGS__)
#define spu_xsbh(...) spu__xi_xsbh(__VA_ARGS__)
#define spu_xshw(...) spu__xi_xshw(__VA_ARGS__)
#define spu_mpy(...) spu__xi_mpy(__VA_ARGS__)
#define spu_mpyu(...) spu__xi_mpyu(__VA_ARGS__)
#define spu_mpya(...) spu__xi_mpya(__VA_ARGS__)
#define spu_mpyh(...) spu__xi_mpyh(__VA_ARGS__)
#define spu_mpyhh(...) spu__xi_mpyhh(__VA_ARGS__)
#endif /* SPU_HELPERS_X86INT_H */
