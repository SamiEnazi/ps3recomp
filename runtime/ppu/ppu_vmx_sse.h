/* ppu_vmx_sse.h -- SSE4.1/AVX2 forms of the hottest ppu_vmx.h helpers (ppu2 agent, 2026-11-08).
 *
 * A lift with PPU_VMX_SSE=1 includes this right after ppu_vmx.h; the macros at
 * the end send the lifted code's calls here. Each function computes EXACTLY
 * what its ppu_vmx.h namesake computes, bit for bit, on every input (NaNs,
 * denormals, infinities, signed zeros, with VSCR[NJ] on or off): the same
 * flushes, the same NaN rules, the same IEEE arithmetic, done on four lanes at
 * once instead of byte by byte. NaN results are always built explicitly (the
 * compiler may swap the operands of a commutative intrinsic, so the x86 NaN
 * propagation rule is never relied on). tools/test_vmx_sse.c compares every
 * one with the scalar helper on random and special inputs.
 *
 * Registers stay big-endian in memory (ppu_vmx.h's layout): a float op swaps
 * the bytes of each lane in and out with one pshufb each, which the compiler
 * folds away between two ops of a chain.
 */
#ifndef PPU_VMX_SSE_H
#define PPU_VMX_SSE_H

#include <immintrin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMXS_INL static inline __attribute__((always_inline))

VMXS_INL __m128i vmxs_bswap(__m128i x) {
    return _mm_shuffle_epi8(x, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
}
VMXS_INL __m128i vmxs_ld(const void* p) { return vmxs_bswap(_mm_loadu_si128((const __m128i*)p)); }
VMXS_INL void vmxs_st(void* p, __m128i x) { _mm_storeu_si128((__m128i*)p, vmxs_bswap(x)); }
/* vmx_flush on 4 lanes: with NJ, a lane whose exponent is 0 keeps only its sign. */
VMXS_INL __m128i vmxs_flush(__m128i x, __m128i nj) {
    const __m128i z = _mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    return _mm_andnot_si128(_mm_and_si128(_mm_and_si128(z, nj), _mm_set1_epi32(0x7FFFFFFF)), x);
}
VMXS_INL __m128i vmxs_nj(uint32_t vscr) { return _mm_set1_epi32((vscr & VMX_NJ) ? -1 : 0); }
/* vmx_isnan on 4 lanes, as an all-ones mask */
VMXS_INL __m128i vmxs_isnan(__m128i x) {
    return _mm_cmpgt_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F800000));
}
/* vmx_fix: any NaN lane becomes the default QNaN 0x7FC00000 */
VMXS_INL __m128i vmxs_fix(__m128i r) {
    return _mm_blendv_epi8(r, _mm_set1_epi32(0x7FC00000), vmxs_isnan(r));
}
/* vmx_vnan2(r, a, b): a NaN A (quieted), else a NaN B (quieted), else vmx_fix(r) */
VMXS_INL __m128i vmxs_nan2(__m128i r, __m128i a, __m128i b) {
    const __m128i q = _mm_set1_epi32(0x7FC00000);
    r = vmxs_fix(r);
    r = _mm_blendv_epi8(r, _mm_or_si128(b, q), vmxs_isnan(b));
    return _mm_blendv_epi8(r, _mm_or_si128(a, q), vmxs_isnan(a));
}
VMXS_INL __m128i vmxs_f2i(__m128 f) { return _mm_castps_si128(f); }
VMXS_INL __m128 vmxs_i2f(__m128i i) { return _mm_castsi128_ps(i); }

/* VMX_FBIN with flush_in = 1: vaddfp / vsubfp */
VMXS_INL void vmx_vaddfp_sse(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr);
    const __m128i x = vmxs_flush(vmxs_ld(a_), nj), y = vmxs_flush(vmxs_ld(b_), nj);
    const __m128i z = vmxs_f2i(_mm_add_ps(vmxs_i2f(x), vmxs_i2f(y)));
    vmxs_st(d_, vmxs_flush(vmxs_nan2(z, x, y), nj));
}
VMXS_INL void vmx_vsubfp_sse(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr);
    const __m128i x = vmxs_flush(vmxs_ld(a_), nj), y = vmxs_flush(vmxs_ld(b_), nj);
    const __m128i z = vmxs_f2i(_mm_sub_ps(vmxs_i2f(x), vmxs_i2f(y)));
    vmxs_st(d_, vmxs_flush(vmxs_nan2(z, x, y), nj));
}
/* vmaddfp: flush(fix(fmaf(a, c, b))) with a, b, c flushed */
VMXS_INL void vmx_vmaddfp_sse(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr);
    const __m128 x = vmxs_i2f(vmxs_flush(vmxs_ld(a_), nj)), y = vmxs_i2f(vmxs_flush(vmxs_ld(b_), nj)),
                 z = vmxs_i2f(vmxs_flush(vmxs_ld(c_), nj));
    vmxs_st(d_, vmxs_flush(vmxs_fix(vmxs_f2i(_mm_fmadd_ps(x, z, y))), nj));
}
/* vnmsubfp: flush(fix(-fmaf(a, c, -b))): the fused a*c - b, then its sign flipped
 * (not fnmadd: -(+0) is -0 where -a*c + b would give +0) */
VMXS_INL void vmx_vnmsubfp_sse(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr);
    const __m128 x = vmxs_i2f(vmxs_flush(vmxs_ld(a_), nj)), y = vmxs_i2f(vmxs_flush(vmxs_ld(b_), nj)),
                 z = vmxs_i2f(vmxs_flush(vmxs_ld(c_), nj));
    const __m128i m = _mm_xor_si128(vmxs_f2i(_mm_fmsub_ps(x, z, y)), _mm_set1_epi32((int)0x80000000u));
    vmxs_st(d_, vmxs_flush(vmxs_fix(m), nj));
}

/* VMX_FUN with flush_in = 1, nanmode 2: vrefp / vrsqrtefp (IEEE division and square
 * root, correctly rounded on both sides) */
VMXS_INL __m128i vmxs_nan1(__m128i r, __m128i b) {
    r = vmxs_fix(r);
    return _mm_blendv_epi8(r, _mm_or_si128(b, _mm_set1_epi32(0x7FC00000)), vmxs_isnan(b));
}
VMXS_INL void vmx_vrefp_sse(VMX_D, VMX_B, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr), y = vmxs_flush(vmxs_ld(b_), nj);
    const __m128i z = vmxs_f2i(_mm_div_ps(_mm_set1_ps(1.0f), vmxs_i2f(y)));
    vmxs_st(d_, vmxs_flush(vmxs_nan1(z, y), nj));
}
VMXS_INL void vmx_vrsqrtefp_sse(VMX_D, VMX_B, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr), y = vmxs_flush(vmxs_ld(b_), nj);
    const __m128i z = vmxs_f2i(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(vmxs_i2f(y))));
    vmxs_st(d_, vmxs_flush(vmxs_nan1(z, y), nj));
}
/* vmaxfp / vminfp (VMX_FBIN, flush_in = 0): the larger / smaller, max(-0,+0) = +0 and
 * min(-0,+0) = -0 (AND / OR of two zeros), NaNs by vmx_vnan2, the result flushed */
VMXS_INL void vmx_vmaxfp_sse(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr), x = vmxs_ld(a_), y = vmxs_ld(b_);
    const __m128 xf = vmxs_i2f(x), yf = vmxs_i2f(y);
    __m128i z = _mm_and_si128(x, y);
    z = _mm_blendv_epi8(z, y, vmxs_f2i(_mm_cmp_ps(yf, xf, _CMP_GT_OQ)));
    z = _mm_blendv_epi8(z, x, vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_GT_OQ)));
    vmxs_st(d_, vmxs_flush(vmxs_nan2(z, x, y), nj));
}
VMXS_INL void vmx_vminfp_sse(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i nj = vmxs_nj(vscr), x = vmxs_ld(a_), y = vmxs_ld(b_);
    const __m128 xf = vmxs_i2f(x), yf = vmxs_i2f(y);
    __m128i z = _mm_or_si128(x, y);
    z = _mm_blendv_epi8(z, y, vmxs_f2i(_mm_cmp_ps(yf, xf, _CMP_LT_OQ)));
    z = _mm_blendv_epi8(z, x, vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_LT_OQ)));
    vmxs_st(d_, vmxs_flush(vmxs_nan2(z, x, y), nj));
}
/* VMX_CMPF: operands flushed, an ordered compare (a NaN lane is false), lanes all-ones
 * or zero, and the CR6 nibble [all, 0, none, 0] */
#define VMXS_CMPF(name, pred) \
VMXS_INL uint32_t name(VMX_D, VMX_A, VMX_B, uint32_t vscr) { \
    const __m128i nj = vmxs_nj(vscr); \
    const __m128 c = _mm_cmp_ps(vmxs_i2f(vmxs_flush(vmxs_ld(a_), nj)), vmxs_i2f(vmxs_flush(vmxs_ld(b_), nj)), pred); \
    _mm_storeu_si128((__m128i*)d_, vmxs_f2i(c));   /* all-ones / zero lanes: no swap needed */ \
    const int m = _mm_movemask_ps(c); \
    return (uint32_t)((m == 15) << 3 | (m == 0) << 1); }
VMXS_CMPF(vmx_vcmpeqfp_sse, _CMP_EQ_OQ)
VMXS_CMPF(vmx_vcmpgefp_sse, _CMP_GE_OQ)
VMXS_CMPF(vmx_vcmpgtfp_sse, _CMP_GT_OQ)
/* vcfsx: (float)(int32) lane, times 2^-uimm (exact: |v| >= 1 or 0, uimm <= 31) */
VMXS_INL void vmx_vcfsx_sse(VMX_D, VMX_B, unsigned uimm) {
    const __m128 f = _mm_cvtepi32_ps(vmxs_ld(b_));
    vmxs_st(d_, vmxs_f2i(_mm_mul_ps(f, vmxs_i2f(_mm_set1_epi32((int)((127u - (uimm & 31u)) << 23))))));
}

/* lvx / stvx (vm_raw_ld16 / vm_raw_st16): one 16-byte access instead of two 8-byte
 * ones, so a vector op reading the register right after does not stall on a split
 * store-to-load forward. Same bytes; each aligned 8-byte half is still single-copy
 * atomic. (The old form computes the second half's address modulo 2^32: the same
 * address for every ea below 0xFFFFFFF0, i.e. every mapped one.) */
VMXS_INL void ppu_vld16_sse(void* dst, uint64_t ea) {
    _mm_storeu_si128((__m128i*)dst, _mm_loadu_si128((const __m128i*)(vm_base + (uint32_t)ea)));
}
VMXS_INL void ppu_vst16_sse(uint64_t ea, const void* src) {
    _mm_storeu_si128((__m128i*)(vm_base + (uint32_t)ea), _mm_loadu_si128((const __m128i*)src));
}
/* vsldoi: (a || b)[n .. n+15]; vspltw: word u of b in every word (n, u constants) */
#define PPU_VSLDOI_SSE(d, a, b, n) _mm_storeu_si128((__m128i*)(d), \
    _mm_alignr_epi8(_mm_loadu_si128((const __m128i*)(b)), _mm_loadu_si128((const __m128i*)(a)), (n)))
#define PPU_VSPLTW_SSE(d, b, u) _mm_storeu_si128((__m128i*)(d), \
    _mm_shuffle_epi32(_mm_loadu_si128((const __m128i*)(b)), (u) * 0x55))

/* vmrgh{w,h} / vmrgl{w,h}: interleave the high / low halves (element order = memory order) */
#define PPU_VMRG_SSE(d, a, b, op) _mm_storeu_si128((__m128i*)(d), \
    op(_mm_loadu_si128((const __m128i*)(a)), _mm_loadu_si128((const __m128i*)(b))))
/* lvsl / lvsr: byte i = b + i (b <= 16, so no byte wraps) */
#define PPU_LVSL_SSE(d, b) _mm_storeu_si128((__m128i*)(d), _mm_add_epi8(_mm_set1_epi8((char)(b)), \
    _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)))
/* lvewx: the word at ea (4-aligned) in its own lane, zeros elsewhere */
VMXS_INL void ppu_lvewx_sse(void* d, uint64_t ea) {
    const uint32_t e = VM_RAW_LD(uint32_t, ea);
    const __m128i lane = _mm_cmpeq_epi32(_mm_setr_epi32(0, 4, 8, 12), _mm_set1_epi32((int)(ea & 12u)));
    _mm_storeu_si128((__m128i*)d, _mm_and_si128(_mm_set1_epi32((int)e), lane));
}
/* lvlx: bytes ea .. end of its aligned block, then zeros; lvrx: zeros, then the bytes of
 * the block before ea (pshufb zeroes a lane whose index has bit 7 set). The aligned block
 * is read whole: the same 16 bytes, so the same page. */
VMXS_INL void ppu_lvlx_sse(void* d, uint64_t ea) {
    const __m128i q = _mm_loadu_si128((const __m128i*)(vm_base + (uint32_t)(ea & ~0xFULL)));
    const __m128i ix = _mm_add_epi8(_mm_set1_epi8((char)(ea & 0xF)), _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    _mm_storeu_si128((__m128i*)d, _mm_shuffle_epi8(q, _mm_or_si128(ix, _mm_cmpgt_epi8(ix, _mm_set1_epi8(15)))));
}
VMXS_INL void ppu_lvrx_sse(void* d, uint64_t ea) {
    const __m128i q = _mm_loadu_si128((const __m128i*)(vm_base + (uint32_t)(ea & ~0xFULL)));
    const __m128i ix = _mm_add_epi8(_mm_set1_epi8((char)((int)(ea & 0xF) - 16)), _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    _mm_storeu_si128((__m128i*)d, _mm_shuffle_epi8(q, ix));
}
/* vcmpequw: equal words (no swap needed for equality), CR6 [all, 0, none, 0] */
VMXS_INL uint32_t vmx_vcmpequw_sse(VMX_D, VMX_A, VMX_B) {
    const __m128i c = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)a_), _mm_loadu_si128((const __m128i*)b_));
    _mm_storeu_si128((__m128i*)d_, c);
    const int m = _mm_movemask_ps(_mm_castsi128_ps(c));
    return (uint32_t)((m == 15) << 3 | (m == 0) << 1);
}
/* vslw: each word shifted left by the low 5 bits of B's word */
VMXS_INL void vmx_vslw_sse(VMX_D, VMX_A, VMX_B) {
    vmxs_st(d_, _mm_sllv_epi32(vmxs_ld(a_), _mm_and_si128(vmxs_ld(b_), _mm_set1_epi32(31))));
}

/* vxor / vor / vand / vandc / vsel on the whole register at once */
VMXS_INL __m128i ppu_vandc_sse(__m128i a, __m128i b) { return _mm_andnot_si128(b, a); }
#define PPU_VLOGIC_SSE(d, op, a, b) _mm_storeu_si128((__m128i*)(d), \
    op(_mm_loadu_si128((const __m128i*)(a)), _mm_loadu_si128((const __m128i*)(b))))
#define PPU_VSEL_SSE(d, a, b, c) do { const __m128i _c = _mm_loadu_si128((const __m128i*)(c)); \
    _mm_storeu_si128((__m128i*)(d), _mm_or_si128(_mm_andnot_si128(_c, _mm_loadu_si128((const __m128i*)(a))), \
                                                 _mm_and_si128(_c, _mm_loadu_si128((const __m128i*)(b))))); } while (0)

/* vperm: d[i] = (a || b)[c[i] & 31], bytes in register (memory) order */
VMXS_INL void ppu_vperm_sse(void* d_, const void* a_, const void* b_, const void* c_) {
    const __m128i a = _mm_loadu_si128((const __m128i*)a_), b = _mm_loadu_si128((const __m128i*)b_);
    const __m128i c = _mm_and_si128(_mm_loadu_si128((const __m128i*)c_), _mm_set1_epi8(0x1F));
    const __m128i ra = _mm_shuffle_epi8(a, c), rb = _mm_shuffle_epi8(b, c);   /* low 4 bits index, bit 7 clear */
    _mm_storeu_si128((__m128i*)d_, _mm_blendv_epi8(ra, rb, _mm_slli_epi16(c, 3)));   /* bit 4 -> bit 7 */
}

/* PPU_VMX_FAST (ppu3 agent, 2026-11-08; a lift with PPU_VMX_FAST=1 defines it): the
 * float ops above with the plain SSE result when no lane needs the special rules.
 * The flushes only change a denormal (with NJ; a zero keeps its sign either way),
 * and the NaN rules only change a NaN result -- and a NaN operand always gives a NaN
 * result (IEEE add / sub / fma / div / sqrt). So when no operand and no result lane
 * is a denormal and no result lane is a NaN, the exact form returns exactly the plain
 * result (same IEEE operation, round to nearest, no flush); otherwise the exact form
 * runs from the start (nothing has been stored yet, so d may alias any operand).
 * The test is off the result's dependency chain: a chain of float ops runs at the
 * latency of the SSE ops instead of ~4x it. tools/test_vmx_sse.c checks every one
 * against ppu_vmx.h. */
#ifdef PPU_VMX_FAST
/* |v| - 1 per lane: a denormal is exactly a value below 0x7FFFFF (unsigned); zero wraps high */
VMXS_INL __m128i vmxs_dm1(__m128i v) {
    return _mm_sub_epi32(_mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(1));
}
/* nonzero if any lane of the minimum m of some dm1 values is a denormal, or r1 = dm1(r) is a NaN
 * (as signed: zero is -1, a NaN is above 0x7F7FFFFF = dm1(inf)) */
VMXS_INL int vmxs_special(__m128i m, __m128i r1) {
    const __m128i d = _mm_cmpeq_epi32(_mm_min_epu32(m, _mm_set1_epi32(0x007FFFFE)), m);
    const __m128i n = _mm_cmpgt_epi32(r1, _mm_set1_epi32(0x7F7FFFFF));
    const __m128i s = _mm_or_si128(d, n);
    return !_mm_testz_si128(s, s);
}
VMXS_INL void vmx_vaddfp_fast(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    const __m128i z = vmxs_f2i(_mm_add_ps(vmxs_i2f(x), vmxs_i2f(y))), z1 = vmxs_dm1(z);
    if (__builtin_expect(vmxs_special(_mm_min_epu32(_mm_min_epu32(vmxs_dm1(x), vmxs_dm1(y)), z1), z1), 0)) {
        vmx_vaddfp_sse(d_, a_, b_, vscr); return; }
    vmxs_st(d_, z);
}
VMXS_INL void vmx_vsubfp_fast(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    const __m128i z = vmxs_f2i(_mm_sub_ps(vmxs_i2f(x), vmxs_i2f(y))), z1 = vmxs_dm1(z);
    if (__builtin_expect(vmxs_special(_mm_min_epu32(_mm_min_epu32(vmxs_dm1(x), vmxs_dm1(y)), z1), z1), 0)) {
        vmx_vsubfp_sse(d_, a_, b_, vscr); return; }
    vmxs_st(d_, z);
}
VMXS_INL void vmx_vmaddfp_fast(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_), c = vmxs_ld(c_);
    const __m128i z = vmxs_f2i(_mm_fmadd_ps(vmxs_i2f(x), vmxs_i2f(c), vmxs_i2f(y))), z1 = vmxs_dm1(z);
    const __m128i m = _mm_min_epu32(_mm_min_epu32(vmxs_dm1(x), vmxs_dm1(y)), _mm_min_epu32(vmxs_dm1(c), z1));
    if (__builtin_expect(vmxs_special(m, z1), 0)) { vmx_vmaddfp_sse(d_, a_, b_, c_, vscr); return; }
    vmxs_st(d_, z);
}
/* -(a*c - b): flipping the sign of a non-NaN, non-denormal value keeps it so */
VMXS_INL void vmx_vnmsubfp_fast(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_), c = vmxs_ld(c_);
    const __m128i z = vmxs_f2i(_mm_fmsub_ps(vmxs_i2f(x), vmxs_i2f(c), vmxs_i2f(y))), z1 = vmxs_dm1(z);
    const __m128i m = _mm_min_epu32(_mm_min_epu32(vmxs_dm1(x), vmxs_dm1(y)), _mm_min_epu32(vmxs_dm1(c), z1));
    if (__builtin_expect(vmxs_special(m, z1), 0)) { vmx_vnmsubfp_sse(d_, a_, b_, c_, vscr); return; }
    vmxs_st(d_, _mm_xor_si128(z, _mm_set1_epi32((int)0x80000000u)));
}
/* vmaxfp / vminfp take no input flush: the plain result needs only no NaN operand
 * (then the exact form's NaN rules do nothing) and no denormal result. The selection
 * (AND / OR of two zeros, then the ordered compares) is the exact form's own. */
VMXS_INL void vmx_vmaxfp_fast(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    const __m128 xf = vmxs_i2f(x), yf = vmxs_i2f(y);
    __m128i z = _mm_and_si128(x, y);
    z = _mm_blendv_epi8(z, y, vmxs_f2i(_mm_cmp_ps(yf, xf, _CMP_GT_OQ)));
    z = _mm_blendv_epi8(z, x, vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_GT_OQ)));
    const __m128i n = vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_UNORD_Q)), z1 = vmxs_dm1(z);
    const __m128i s = _mm_or_si128(n, _mm_cmpeq_epi32(_mm_min_epu32(z1, _mm_set1_epi32(0x007FFFFE)), z1));
    if (__builtin_expect(!_mm_testz_si128(s, s), 0)) { vmx_vmaxfp_sse(d_, a_, b_, vscr); return; }
    vmxs_st(d_, z);
}
VMXS_INL void vmx_vminfp_fast(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    const __m128 xf = vmxs_i2f(x), yf = vmxs_i2f(y);
    __m128i z = _mm_or_si128(x, y);
    z = _mm_blendv_epi8(z, y, vmxs_f2i(_mm_cmp_ps(yf, xf, _CMP_LT_OQ)));
    z = _mm_blendv_epi8(z, x, vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_LT_OQ)));
    const __m128i n = vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_UNORD_Q)), z1 = vmxs_dm1(z);
    const __m128i s = _mm_or_si128(n, _mm_cmpeq_epi32(_mm_min_epu32(z1, _mm_set1_epi32(0x007FFFFE)), z1));
    if (__builtin_expect(!_mm_testz_si128(s, s), 0)) { vmx_vminfp_sse(d_, a_, b_, vscr); return; }
    vmxs_st(d_, z);
}
/* compares: the flush changes only denormal operands; an ordered compare of the rest is the same */
#define VMXS_CMPF_FAST(name, sse, pred) \
VMXS_INL uint32_t name(VMX_D, VMX_A, VMX_B, uint32_t vscr) { \
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_); \
    const __m128i m = _mm_min_epu32(vmxs_dm1(x), vmxs_dm1(y)); \
    const __m128i s = _mm_cmpeq_epi32(_mm_min_epu32(m, _mm_set1_epi32(0x007FFFFE)), m); \
    if (__builtin_expect(!_mm_testz_si128(s, s), 0)) return sse(d_, a_, b_, vscr); \
    const __m128 c = _mm_cmp_ps(vmxs_i2f(x), vmxs_i2f(y), pred); \
    _mm_storeu_si128((__m128i*)d_, vmxs_f2i(c)); \
    const int k = _mm_movemask_ps(c); \
    return (uint32_t)((k == 15) << 3 | (k == 0) << 1); }
VMXS_CMPF_FAST(vmx_vcmpeqfp_fast, vmx_vcmpeqfp_sse, _CMP_EQ_OQ)
VMXS_CMPF_FAST(vmx_vcmpgefp_fast, vmx_vcmpgefp_sse, _CMP_GE_OQ)
VMXS_CMPF_FAST(vmx_vcmpgtfp_fast, vmx_vcmpgtfp_sse, _CMP_GT_OQ)
#endif /* PPU_VMX_FAST */

/* PPU_VMX_CLEAN (ppu-vmx agent, 2026-11-10; a lift with PPU_VMX_CLEAN=1 defines it, and
 * needs PPU_VMX_FAST): the fast float forms with a lift-time mask k of the operands that
 * still need the denormal test (bit 0 = a_, bit 1 = b_, bit 2 = c_, argument order).
 * tools/ppu_vmx_flow.py clears an operand's bit only when its value is CLEAN: written,
 * with no label, call or VSCR write in between, by one of the float ops below (their
 * result is never a denormal with NJ = 1: the exact form flushes it, the fast form sends
 * a denormal result to the exact form), by a word compare (all-ones / zero lanes), a
 * word splat / shift / merge of clean values, a splat immediate that makes no denormal
 * lane, or a logic op that keeps whole clean lanes. So a clean operand either holds no
 * denormal lane (NJ = 1) or the exact form takes it as it is too (NJ = 0: no flush):
 * the plain operation treats it exactly as the exact form does, and its test can go.
 * The result test is PPU_VMX_FAST's rule (no result lane a denormal or a NaN), in one
 * float compare: |z| < 2^-126 or unordered, ANDed with |z| (a zero lane drops out); a
 * tested operand x adds (|x| < 2^-126, ordered) AND |x| (a NaN operand needs no test of
 * its own: it makes a NaN result).
 * The vscr argument is a pointer, read only on the exact path.
 * tools/test_vmx_sse.c checks every form and every k against ppu_vmx.h, with clean
 * operands (NJ = 1: no denormal lane; NJ = 0: anything) where a bit is clear.
 * PPU_VMX_CLEAN_VERIFY (a check build, PPU_VMX_CLEAN_VERIFY=1 at lift time) tests the
 * untested operands too and reports a denormal one under NJ = 1 on stderr. */
#ifdef PPU_VMX_CLEAN
#ifndef PPU_VMX_FAST
#error "PPU_VMX_CLEAN needs PPU_VMX_FAST"
#endif
/* all-ones lanes where an operand named in k is a denormal */
VMXS_INL __m128i vmxs_den_k(__m128i x, __m128i y, __m128i c, unsigned k) {
    __m128i m = _mm_set1_epi32(-1);
    if (k & 1) m = _mm_min_epu32(m, vmxs_dm1(x));
    if (k & 2) m = _mm_min_epu32(m, vmxs_dm1(y));
    if (k & 4) m = _mm_min_epu32(m, vmxs_dm1(c));
    return _mm_cmpeq_epi32(_mm_min_epu32(m, _mm_set1_epi32(0x007FFFFE)), m);
}
VMXS_INL __m128 vmxs_abs(__m128i v) { return _mm_and_ps(vmxs_i2f(v), vmxs_i2f(_mm_set1_epi32(0x7FFFFFFF))); }
/* nonzero bits in the lanes of v that are a denormal (|v| < 2^-126 and not zero; NaN: no) */
VMXS_INL __m128i vmxs_den(__m128i v) {
    const __m128 av = vmxs_abs(v);
    return _mm_and_si128(vmxs_f2i(_mm_cmp_ps(av, vmxs_i2f(_mm_set1_epi32(0x00800000)), _CMP_LT_OQ)), vmxs_f2i(av));
}
/* nonzero if a result lane of z is a denormal or a NaN, or an operand named in k a denormal */
VMXS_INL int vmxs_special_k(__m128i z, __m128i x, __m128i y, __m128i c, unsigned k) {
    const __m128 az = vmxs_abs(z);
    const __m128i lo = vmxs_f2i(_mm_cmp_ps(az, vmxs_i2f(_mm_set1_epi32(0x00800000)), _CMP_NGE_UQ));
    if (!(k & 7)) return !_mm_testz_si128(lo, vmxs_f2i(az));
    __m128i s = _mm_and_si128(lo, vmxs_f2i(az));
    if (k & 1) s = _mm_or_si128(s, vmxs_den(x));
    if (k & 2) s = _mm_or_si128(s, vmxs_den(y));
    if (k & 4) s = _mm_or_si128(s, vmxs_den(c));
    return !_mm_testz_si128(s, s);
}
#ifdef PPU_VMX_CLEAN_VERIFY
#include <stdio.h>
static long vmxs_clean_bad_n;
static void vmxs_clean_bad(const char* op, unsigned k) {
    const long c = __atomic_add_fetch(&vmxs_clean_bad_n, 1, __ATOMIC_RELAXED);
    if (c <= 16 || (c & (c - 1)) == 0)
        fprintf(stderr, "[vmx-clean-verify] %s k=%u: an operand taken as clean holds a denormal (#%ld in this file)\n", op, k, c);
}
/* an operand left out of k (of those in all) is a denormal while NJ = 1: report, run `exact` (it returns) */
#define VMXS_CLEAN_CHECK(op, x, y, c, all, k, vscr, exact) do { \
    const __m128i _u = vmxs_den_k((x), (y), (c), (all) & ~(k)); \
    if ((*(vscr) & VMX_NJ) && !_mm_testz_si128(_u, _u)) { vmxs_clean_bad(op, k); exact; } } while (0)
#else
#define VMXS_CLEAN_CHECK(op, x, y, c, all, k, vscr, exact) do { } while (0)
#endif
VMXS_INL void vmx_vaddfp_k(VMX_D, VMX_A, VMX_B, const uint32_t* vscr, unsigned k) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    VMXS_CLEAN_CHECK("vaddfp", x, y, y, 3u, k, vscr, vmx_vaddfp_sse(d_, a_, b_, *vscr); return);
    const __m128i z = vmxs_f2i(_mm_add_ps(vmxs_i2f(x), vmxs_i2f(y)));
    if (__builtin_expect(vmxs_special_k(z, x, y, y, k & 3u), 0)) { vmx_vaddfp_sse(d_, a_, b_, *vscr); return; }
    vmxs_st(d_, z);
}
VMXS_INL void vmx_vsubfp_k(VMX_D, VMX_A, VMX_B, const uint32_t* vscr, unsigned k) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    VMXS_CLEAN_CHECK("vsubfp", x, y, y, 3u, k, vscr, vmx_vsubfp_sse(d_, a_, b_, *vscr); return);
    const __m128i z = vmxs_f2i(_mm_sub_ps(vmxs_i2f(x), vmxs_i2f(y)));
    if (__builtin_expect(vmxs_special_k(z, x, y, y, k & 3u), 0)) { vmx_vsubfp_sse(d_, a_, b_, *vscr); return; }
    vmxs_st(d_, z);
}
/* vmaddfp: a * c + b, fused (the exact form's fmaf) */
VMXS_INL void vmx_vmaddfp_k(VMX_D, VMX_A, VMX_B, VMX_C, const uint32_t* vscr, unsigned k) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_), c = vmxs_ld(c_);
    VMXS_CLEAN_CHECK("vmaddfp", x, y, c, 7u, k, vscr, vmx_vmaddfp_sse(d_, a_, b_, c_, *vscr); return);
    const __m128i z = vmxs_f2i(_mm_fmadd_ps(vmxs_i2f(x), vmxs_i2f(c), vmxs_i2f(y)));
    if (__builtin_expect(vmxs_special_k(z, x, y, c, k & 7u), 0)) { vmx_vmaddfp_sse(d_, a_, b_, c_, *vscr); return; }
    vmxs_st(d_, z);
}
/* vnmsubfp: -(a * c - b), the fused a*c - b tested, then its sign flipped */
VMXS_INL void vmx_vnmsubfp_k(VMX_D, VMX_A, VMX_B, VMX_C, const uint32_t* vscr, unsigned k) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_), c = vmxs_ld(c_);
    VMXS_CLEAN_CHECK("vnmsubfp", x, y, c, 7u, k, vscr, vmx_vnmsubfp_sse(d_, a_, b_, c_, *vscr); return);
    const __m128i z = vmxs_f2i(_mm_fmsub_ps(vmxs_i2f(x), vmxs_i2f(c), vmxs_i2f(y)));
    if (__builtin_expect(vmxs_special_k(z, x, y, c, k & 7u), 0)) { vmx_vnmsubfp_sse(d_, a_, b_, c_, *vscr); return; }
    vmxs_st(d_, _mm_xor_si128(z, _mm_set1_epi32((int)0x80000000u)));
}
/* vrefp / vrsqrtefp: IEEE 1/b, 1/sqrt(b) (the exact forms' arithmetic); the plain result is
 * the exact one when b is no denormal (or NJ = 0) and the result no denormal and no NaN */
VMXS_INL void vmx_vrefp_k(VMX_D, VMX_B, const uint32_t* vscr, unsigned k) {
    const __m128i y = vmxs_ld(b_);
    VMXS_CLEAN_CHECK("vrefp", y, y, y, 1u, k, vscr, vmx_vrefp_sse(d_, b_, *vscr); return);
    const __m128i z = vmxs_f2i(_mm_div_ps(_mm_set1_ps(1.0f), vmxs_i2f(y)));
    if (__builtin_expect(vmxs_special_k(z, y, y, y, k & 1u), 0)) { vmx_vrefp_sse(d_, b_, *vscr); return; }
    vmxs_st(d_, z);
}
VMXS_INL void vmx_vrsqrtefp_k(VMX_D, VMX_B, const uint32_t* vscr, unsigned k) {
    const __m128i y = vmxs_ld(b_);
    VMXS_CLEAN_CHECK("vrsqrtefp", y, y, y, 1u, k, vscr, vmx_vrsqrtefp_sse(d_, b_, *vscr); return);
    const __m128i z = vmxs_f2i(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(vmxs_i2f(y))));
    if (__builtin_expect(vmxs_special_k(z, y, y, y, k & 1u), 0)) { vmx_vrsqrtefp_sse(d_, b_, *vscr); return; }
    vmxs_st(d_, z);
}
/* vmaxfp / vminfp: with k != 0 the PPU_VMX_FAST form (no NaN operand, no denormal result).
 * With k == 0 both operands are clean: the result is one of them (or the AND / OR of two
 * equal values: the same bits, or two zeros), so with NJ = 1 it is no denormal, and with
 * NJ = 0 the exact form does not flush it: only the NaN test is left. */
VMXS_INL void vmx_vmaxfp_k(VMX_D, VMX_A, VMX_B, const uint32_t* vscr, unsigned k) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    VMXS_CLEAN_CHECK("vmaxfp", x, y, y, 3u, k, vscr, vmx_vmaxfp_sse(d_, a_, b_, *vscr); return);
    const __m128 xf = vmxs_i2f(x), yf = vmxs_i2f(y);
    __m128i z = _mm_and_si128(x, y);
    z = _mm_blendv_epi8(z, y, vmxs_f2i(_mm_cmp_ps(yf, xf, _CMP_GT_OQ)));
    z = _mm_blendv_epi8(z, x, vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_GT_OQ)));
    __m128i s = vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_UNORD_Q));
    if (k & 3u) s = _mm_or_si128(s, vmxs_den(z));
    if (__builtin_expect(!_mm_testz_si128(s, s), 0)) { vmx_vmaxfp_sse(d_, a_, b_, *vscr); return; }
    vmxs_st(d_, z);
}
VMXS_INL void vmx_vminfp_k(VMX_D, VMX_A, VMX_B, const uint32_t* vscr, unsigned k) {
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_);
    VMXS_CLEAN_CHECK("vminfp", x, y, y, 3u, k, vscr, vmx_vminfp_sse(d_, a_, b_, *vscr); return);
    const __m128 xf = vmxs_i2f(x), yf = vmxs_i2f(y);
    __m128i z = _mm_or_si128(x, y);
    z = _mm_blendv_epi8(z, y, vmxs_f2i(_mm_cmp_ps(yf, xf, _CMP_LT_OQ)));
    z = _mm_blendv_epi8(z, x, vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_LT_OQ)));
    __m128i s = vmxs_f2i(_mm_cmp_ps(xf, yf, _CMP_UNORD_Q));
    if (k & 3u) s = _mm_or_si128(s, vmxs_den(z));
    if (__builtin_expect(!_mm_testz_si128(s, s), 0)) { vmx_vminfp_sse(d_, a_, b_, *vscr); return; }
    vmxs_st(d_, z);
}
/* compares: only a tested operand's denormal lane needs the exact form */
#define VMXS_CMPF_K(name, sse, pred) \
VMXS_INL uint32_t name(VMX_D, VMX_A, VMX_B, const uint32_t* vscr, unsigned k) { \
    const __m128i x = vmxs_ld(a_), y = vmxs_ld(b_); \
    VMXS_CLEAN_CHECK(#name, x, y, y, 3u, k, vscr, return sse(d_, a_, b_, *vscr)); \
    if (k & 3u) { const __m128i s = _mm_or_si128((k & 1u) ? vmxs_den(x) : _mm_setzero_si128(), \
                                                 (k & 2u) ? vmxs_den(y) : _mm_setzero_si128()); \
        if (__builtin_expect(!_mm_testz_si128(s, s), 0)) return sse(d_, a_, b_, *vscr); } \
    const __m128 c = _mm_cmp_ps(vmxs_i2f(x), vmxs_i2f(y), pred); \
    _mm_storeu_si128((__m128i*)d_, vmxs_f2i(c)); \
    const int m = _mm_movemask_ps(c); \
    return (uint32_t)((m == 15) << 3 | (m == 0) << 1); }
VMXS_CMPF_K(vmx_vcmpeqfp_k, vmx_vcmpeqfp_sse, _CMP_EQ_OQ)
VMXS_CMPF_K(vmx_vcmpgefp_k, vmx_vcmpgefp_sse, _CMP_GE_OQ)
VMXS_CMPF_K(vmx_vcmpgtfp_k, vmx_vcmpgtfp_sse, _CMP_GT_OQ)
#endif /* PPU_VMX_CLEAN */

/* PPU_VMX_INT (ppu-vmx agent, 2026-11-10; a lift with PPU_VMX_INT=1 defines it): the
 * integer AltiVec ops the lift still did element by element, on SSE/AVX2. Same bits as
 * ppu_vmx.h / the lifter's inline loops (tools/test_vmx_sse.c). Word and halfword lanes
 * are big-endian in the register: swapped in and out around the operation. */
#ifdef PPU_VMX_INT
VMXS_INL __m128i vmxs_bswap16(__m128i x) {
    return _mm_shuffle_epi8(x, _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1));
}
/* vslb: each byte shifted left by the low 3 bits of B's byte (by 4, 2, 1 as those bits say;
 * blendv picks by bit 7 of each mask byte, the 16-bit shifts move bit n of every byte there) */
VMXS_INL void vmx_vslb_sse(VMX_D, VMX_A, VMX_B) {
    __m128i x = _mm_loadu_si128((const __m128i*)a_);
    const __m128i s = _mm_loadu_si128((const __m128i*)b_);
    x = _mm_blendv_epi8(x, _mm_and_si128(_mm_slli_epi16(x, 4), _mm_set1_epi8((char)0xF0)), _mm_slli_epi16(s, 5));
    x = _mm_blendv_epi8(x, _mm_and_si128(_mm_slli_epi16(x, 2), _mm_set1_epi8((char)0xFC)), _mm_slli_epi16(s, 6));
    x = _mm_blendv_epi8(x, _mm_add_epi8(x, x), _mm_slli_epi16(s, 7));
    _mm_storeu_si128((__m128i*)d_, x);
}
/* vsrw / vsraw / vrlw: each word by the low 5 bits of B's word */
VMXS_INL void vmx_vsrw_sse(VMX_D, VMX_A, VMX_B) {
    vmxs_st(d_, _mm_srlv_epi32(vmxs_ld(a_), _mm_and_si128(vmxs_ld(b_), _mm_set1_epi32(31))));
}
VMXS_INL void vmx_vsraw_sse(VMX_D, VMX_A, VMX_B) {
    vmxs_st(d_, _mm_srav_epi32(vmxs_ld(a_), _mm_and_si128(vmxs_ld(b_), _mm_set1_epi32(31))));
}
VMXS_INL void vmx_vrlw_sse(VMX_D, VMX_A, VMX_B) {   /* a count of 0: x | (x >> 32 = 0) */
    const __m128i x = vmxs_ld(a_), n = _mm_and_si128(vmxs_ld(b_), _mm_set1_epi32(31));
    vmxs_st(d_, _mm_or_si128(_mm_sllv_epi32(x, n), _mm_srlv_epi32(x, _mm_sub_epi32(_mm_set1_epi32(32), n))));
}
/* vcmpgtsw / vcmpgtuw: all-ones / zero words (no swap needed out), CR6 [all, 0, none, 0] */
VMXS_INL uint32_t vmx_vcmpgtsw_sse(VMX_D, VMX_A, VMX_B) {
    const __m128i c = _mm_cmpgt_epi32(vmxs_ld(a_), vmxs_ld(b_));
    _mm_storeu_si128((__m128i*)d_, c);
    const int m = _mm_movemask_ps(_mm_castsi128_ps(c));
    return (uint32_t)((m == 15) << 3 | (m == 0) << 1);
}
VMXS_INL uint32_t vmx_vcmpgtuw_sse(VMX_D, VMX_A, VMX_B) {
    const __m128i bias = _mm_set1_epi32((int)0x80000000u);
    const __m128i c = _mm_cmpgt_epi32(_mm_xor_si128(vmxs_ld(a_), bias), _mm_xor_si128(vmxs_ld(b_), bias));
    _mm_storeu_si128((__m128i*)d_, c);
    const int m = _mm_movemask_ps(_mm_castsi128_ps(c));
    return (uint32_t)((m == 15) << 3 | (m == 0) << 1);
}
/* vctsxs: trunc(b * 2^uimm) saturated to int32 (SAT on saturation), NaN -> 0. The scale is
 * the exact form's float multiply; cvttps2dq gives 0x80000000 for NaN and out of range. */
VMXS_INL void vmx_vctsxs_sse(VMX_D, VMX_B, unsigned uimm, uint32_t* vscr) {
    const __m128 f = _mm_mul_ps(vmxs_i2f(vmxs_ld(b_)), vmxs_i2f(_mm_set1_epi32((int)((127u + (uimm & 31u)) << 23))));
    const __m128 hi = _mm_cmp_ps(f, _mm_set1_ps(2147483648.0f), _CMP_GE_OQ);
    const __m128 lo = _mm_cmp_ps(f, _mm_set1_ps(-2147483648.0f), _CMP_LT_OQ);
    __m128i r = _mm_blendv_epi8(_mm_cvttps_epi32(f), _mm_set1_epi32(0x7FFFFFFF), vmxs_f2i(hi));
    r = _mm_andnot_si128(vmxs_f2i(_mm_cmp_ps(f, f, _CMP_UNORD_Q)), r);
    if (_mm_movemask_ps(_mm_or_ps(hi, lo))) *vscr |= VMX_SAT;
    vmxs_st(d_, r);
}
/* vctuxs: trunc(b * 2^uimm) saturated to uint32 (below 0 -> 0, SAT), NaN -> 0; [2^31, 2^32)
 * converts as f - 2^31 (exact there) with the top bit set back */
VMXS_INL void vmx_vctuxs_sse(VMX_D, VMX_B, unsigned uimm, uint32_t* vscr) {
    const __m128 f = _mm_mul_ps(vmxs_i2f(vmxs_ld(b_)), vmxs_i2f(_mm_set1_epi32((int)((127u + (uimm & 31u)) << 23))));
    const __m128 two31 = _mm_set1_ps(2147483648.0f);
    const __m128 big = _mm_cmp_ps(f, two31, _CMP_GE_OQ);
    const __m128 hi = _mm_cmp_ps(f, _mm_set1_ps(4294967296.0f), _CMP_GE_OQ);
    const __m128 neg = _mm_cmp_ps(f, _mm_setzero_ps(), _CMP_LT_OQ);
    const __m128i t = _mm_blendv_epi8(_mm_cvttps_epi32(f),
        _mm_xor_si128(_mm_cvttps_epi32(_mm_sub_ps(f, two31)), _mm_set1_epi32((int)0x80000000u)), vmxs_f2i(big));
    __m128i r = _mm_or_si128(t, vmxs_f2i(hi));   /* >= 2^32: all ones */
    r = _mm_andnot_si128(_mm_or_si128(vmxs_f2i(neg), vmxs_f2i(_mm_cmp_ps(f, f, _CMP_UNORD_Q))), r);
    if (_mm_movemask_ps(_mm_or_ps(hi, neg))) *vscr |= VMX_SAT;
    vmxs_st(d_, r);
}
/* the lifter's inline element loops (tools/ppu_vmx_flow.py rewrites them): vadd/vsub u{b,h,w}m
 * (modulo) and vmax/vmin {s,u}{h,w} on big-endian lanes */
#define PPU_VINT32_SSE(d, op, a, b) vmxs_st((d), op(vmxs_ld(a), vmxs_ld(b)))
#define PPU_VINT16_SSE(d, op, a, b) _mm_storeu_si128((__m128i*)(d), vmxs_bswap16(op( \
    vmxs_bswap16(_mm_loadu_si128((const __m128i*)(a))), vmxs_bswap16(_mm_loadu_si128((const __m128i*)(b))))))
#define PPU_VINT8_SSE(d, op, a, b) _mm_storeu_si128((__m128i*)(d), \
    op(_mm_loadu_si128((const __m128i*)(a)), _mm_loadu_si128((const __m128i*)(b))))
#endif /* PPU_VMX_INT */

#ifndef PPU_VMX_SSE_NO_REDIRECT
#ifdef PPU_VMX_FAST
#define vmx_vaddfp    vmx_vaddfp_fast
#define vmx_vsubfp    vmx_vsubfp_fast
#define vmx_vmaddfp   vmx_vmaddfp_fast
#define vmx_vnmsubfp  vmx_vnmsubfp_fast
#define vmx_vmaxfp    vmx_vmaxfp_fast
#define vmx_vminfp    vmx_vminfp_fast
#define vmx_vcmpeqfp  vmx_vcmpeqfp_fast
#define vmx_vcmpgefp  vmx_vcmpgefp_fast
#define vmx_vcmpgtfp  vmx_vcmpgtfp_fast
#else
#define vmx_vaddfp    vmx_vaddfp_sse
#define vmx_vsubfp    vmx_vsubfp_sse
#define vmx_vmaddfp   vmx_vmaddfp_sse
#define vmx_vnmsubfp  vmx_vnmsubfp_sse
#define vmx_vmaxfp    vmx_vmaxfp_sse
#define vmx_vminfp    vmx_vminfp_sse
#endif
#define vmx_vrefp     vmx_vrefp_sse
#define vmx_vrsqrtefp vmx_vrsqrtefp_sse
#ifndef PPU_VMX_FAST
#define vmx_vcmpeqfp  vmx_vcmpeqfp_sse
#define vmx_vcmpgefp  vmx_vcmpgefp_sse
#define vmx_vcmpgtfp  vmx_vcmpgtfp_sse
#endif
#define vmx_vcfsx     vmx_vcfsx_sse
#define vmx_vcmpequw  vmx_vcmpequw_sse
#define vmx_vslw      vmx_vslw_sse
#ifdef PPU_VMX_INT
#define vmx_vslb      vmx_vslb_sse
#define vmx_vsrw      vmx_vsrw_sse
#define vmx_vsraw     vmx_vsraw_sse
#define vmx_vrlw      vmx_vrlw_sse
#define vmx_vcmpgtsw  vmx_vcmpgtsw_sse
#define vmx_vcmpgtuw  vmx_vcmpgtuw_sse
#define vmx_vctsxs    vmx_vctsxs_sse
#define vmx_vctuxs    vmx_vctuxs_sse
#endif
#endif

#ifdef __cplusplus
}
#endif
#endif /* PPU_VMX_SSE_H */
