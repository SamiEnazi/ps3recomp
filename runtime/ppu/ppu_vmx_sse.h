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

#ifndef PPU_VMX_SSE_NO_REDIRECT
#define vmx_vaddfp    vmx_vaddfp_sse
#define vmx_vsubfp    vmx_vsubfp_sse
#define vmx_vmaddfp   vmx_vmaddfp_sse
#define vmx_vnmsubfp  vmx_vnmsubfp_sse
#define vmx_vrefp     vmx_vrefp_sse
#define vmx_vrsqrtefp vmx_vrsqrtefp_sse
#define vmx_vmaxfp    vmx_vmaxfp_sse
#define vmx_vminfp    vmx_vminfp_sse
#define vmx_vcmpeqfp  vmx_vcmpeqfp_sse
#define vmx_vcmpgefp  vmx_vcmpgefp_sse
#define vmx_vcmpgtfp  vmx_vcmpgtfp_sse
#define vmx_vcfsx     vmx_vcfsx_sse
#endif

#ifdef __cplusplus
}
#endif
#endif /* PPU_VMX_SSE_H */
