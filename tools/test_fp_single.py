"""test_fp_single.py <out.c> [--verify]: writes a C test of tools/ppu_vmx_flow.py's scalar FP
forms against the lift's own expressions, bit for bit (build it like tools/test_vmx_sse.c:
clang-cl /O2 /arch:AVX2, and run it: exit 0 = equal).

  PPU_FP_SINGLE: ppu_f{add,sub,mul,div}s_sh(x, y, a, b, &o) == ppu_fp_single(ppu_f{add,..}(a, b))
                 (and o its shadow), ppu_fsqrts_sh == ppu_frsp(ppu_fsqrt(b)), ppu_stfs_bits_sh == ppu_stfs_bits
                 for SINGLE-FORMAT operands with their shadows (what the pass promises): an lfs of any word
                 (signalling NaNs keep their bits), a single-precision result, an frsp.
  PPU_FP_CVT:    the new ppu_f2i32 / ppu_f2i64 == the old ones on any double, both roundings.
Both sides are the lifter's preamble text (PPU_FP_ONE_NAN on, as the build has it), so the
test follows the helpers if they change. --verify: the PPU_FP_SINGLE_VERIFY build, which
also checks that a non-single operand is reported and gets the old expression."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
for k in ("PPU_FP_SINGLE", "PPU_FP_CVT", "PPU_FP_SINGLE_VERIFY"):
    os.environ.pop(k, None)
os.environ["PPU_FP_ONE_NAN"] = "1"
import ppu_lifter as L      # noqa: E402
import ppu_fastpath as F    # noqa: E402
import ppu_vmx_flow as W    # noqa: E402

verify = "--verify" in sys.argv
pre = F.source_preamble(L.SOURCE_PREAMBLE)
a, b = pre.index("static inline uint32_t ppu_f2i32"), pre.index("/* FPSCR (Book I")
helpers = pre[a:b]                      # f2i, the NaN helpers, fp_single, frsp, stfs, lfs, fsqrt
old_f2i = W._F2I_OLD
new_f2i = W._F2I_NEW.replace("ppu_f2i32", "ppu_f2i32_n").replace("ppu_f2i64", "ppu_f2i64_n")
if old_f2i not in helpers:
    raise SystemExit("the lifter's ppu_f2i32/ppu_f2i64 text changed: update ppu_vmx_flow.py")
single = ("#define PPU_FP_SINGLE_VERIFY 1\n" if verify else "") + W._SINGLE_C

src = r"""#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define PPU_FP_AUDIT_INC(i) ((void)0)
""" + helpers + new_f2i + single + r"""
static uint64_t s = 0x13198A2E03707344ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static const uint32_t SPW[] = { 0, 0x80000000u, 1, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00800000u, 0x80800000u,
  0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
  0x7F800001u, 0xFF800001u, 0x7FBFFFFFu, 0x7FC12345u, 0xFF812345u, 0x3F7FFFFFu, 0x4B800000u, 0x34000000u,
  0x0C000000u, 0x1F800000u, 0x5F800000u, 0x3F000001u, 0x00000002u, 0x7E800000u, 0x20000000u };
#define NSPW (sizeof SPW / sizeof SPW[0])
static uint32_t word(void) {
  const uint32_t k = (uint32_t)(rnd() % 10);
  if (k < 3) return SPW[rnd() % NSPW];
  if (k == 3) return SPW[rnd() % NSPW] ^ (uint32_t)(rnd() & 3);
  if (k == 4) return (uint32_t)rnd() & 0x807FFFFFu;                                     /* denormals */
  if (k == 5) return ((uint32_t)rnd() & 0x80000000u) | ((uint32_t)(rnd() % 30 + 112) << 23) | ((uint32_t)rnd() & 0x7FFFFFu);
  if (k == 6) return ((uint32_t)rnd() & 0x80000000u) | ((uint32_t)(rnd() % 20 + 1) << 23) | ((uint32_t)rnd() & 0x7FFFFFu);   /* tiny */
  if (k == 7) return ((uint32_t)rnd() & 0x80000000u) | ((uint32_t)(rnd() % 20 + 234) << 23) | ((uint32_t)rnd() & 0x7FFFFFu); /* huge */
  return (uint32_t)rnd();
}
/* a single-format value with its float shadow, as the lift has them: an lfs (the word's float),
 * a single result, an frsp ((float) of it; a NaN's shadow may be any NaN) */
static double sval(float* x) {
  const uint32_t k = (uint32_t)(rnd() % 4);
  if (k < 2) { const uint32_t w = word(); *x = ppu_w2f(w); return ppu_lfs_val(w); }
  double d; uint64_t u = rnd(); memcpy(&d, &u, 8);
  d = k == 2 ? ppu_frsp(d) : ppu_fp_single(d * 1.0);
  *x = (float)d;
  if (d != d && (rnd() & 1)) *x = ppu_w2f(0x7F800001u | (uint32_t)rnd());
  return d;
}
static double anyd(void) {
  const uint32_t k = (uint32_t)(rnd() % 6); double d; uint64_t u;
  if (k < 2) { float x; return sval(&x); }
  if (k == 2) { static const uint64_t SP[] = { 0x41DFFFFFFFC00000ull, 0x41E0000000000000ull, 0x41DFFFFFFFE00000ull,
      0xC1E0000000000000ull, 0xC1E0000000100000ull, 0xC1E0000000200000ull, 0x43E0000000000000ull, 0x43DFFFFFFFFFFFFFull,
      0xC3E0000000000000ull, 0xC3E0000000000001ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull,
      0x7FF0000000000001ull, 0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x3FF8000000000000ull, 0x4004000000000000ull, 0 };
    u = SP[rnd() % (sizeof SP / 8)] + (rnd() % 5) - 2; memcpy(&d, &u, 8); return d; }
  if (k == 3) { d = (double)(int64_t)rnd() / (double)(1u << (rnd() % 32)); return d; }
  if (k == 4) { d = ((double)(int32_t)(uint32_t)rnd()) + 0.5 * (double)(rnd() % 3); return d; }
  u = rnd(); memcpy(&d, &u, 8); return d;
}
static long fails;
static float o;
static void eq(const char* op, double x, double y, double a, double b);
/* the double result must be the old one, and the shadow it leaves a NaN exactly when it is one, else its float */
static void shadow_eq(const char* op, double want, double got, float sh, double a, double b) {
  eq(op, want, got, a, b);
  const double d = sh;
  if (((got != got) != (sh != sh) || (got == got && memcmp(&d, &got, 8))) && fails++ < 12)
    printf("FAIL %s shadow a=%a b=%a: result %a shadow %a\n", op, a, b, got, (double)sh); }
static void eq(const char* op, double x, double y, double a, double b) {
  uint64_t u, v; memcpy(&u, &x, 8); memcpy(&v, &y, 8);
  if (u != v && fails++ < 12) printf("FAIL %s a=%a b=%a: want %016llx got %016llx\n", op, a, b,
                                     (unsigned long long)u, (unsigned long long)v); }
int main(int argc, char** argv) {
  const long n = argc > 1 ? atol(argv[1]) : 4000000;
  long fast = 0;
  for (long i = 0; i < n; i++) {
    float x, y; double a = sval(&x), b = sval(&y);
    if ((i & 7) == 3) { b = a; y = x; } else if ((i & 7) == 5) { b = -a; y = -x; }
    { const double g_ = ppu_fadds_sh(x, y, a, b, &o); shadow_eq("fadds", ppu_fp_single(ppu_fadd(a, b)), g_, o, a, b); }
    { const double g_ = ppu_fsubs_sh(x, y, a, b, &o); shadow_eq("fsubs", ppu_fp_single(ppu_fsub(a, b)), g_, o, a, b); }
    { const double g_ = ppu_fmuls_sh(x, y, a, b, &o); shadow_eq("fmuls", ppu_fp_single(ppu_fmul(a, b)), g_, o, a, b); }
    { const double g_ = ppu_fdivs_sh(x, y, a, b, &o); shadow_eq("fdivs", ppu_fp_single(ppu_fdiv(a, b)), g_, o, a, b); }
    { const double g_ = ppu_fsqrts_sh(y, b, &o); shadow_eq("fsqrts", ppu_frsp(ppu_fsqrt(b)), g_, o, b, b); }
    { const uint32_t p = ppu_stfs_bits_full(a), q = ppu_stfs_bits_sh(x, a);
      if (p != q && fails++ < 12) printf("FAIL stfs %a: %08x vs %08x\n", a, p, q); }
    /* the generic shadow of a single result: (double)(float)r is ppu_fp_single(r) */
    { double r; uint64_t u = rnd(); memcpy(&r, &u, 8); const float t = (float)r;
      eq("fp_single shadow", ppu_fp_single(r), (double)t, r, r); }
    fast += (x + y) == (x + y);
    const double d = anyd();
    for (int rn = 0; rn < 2; rn++) {
      const uint32_t x = ppu_f2i32(d, rn), y = ppu_f2i32_n(d, rn);
      if (x != y && fails++ < 12) printf("FAIL f2i32 rn=%d %a: %08x vs %08x\n", rn, d, x, y);
      const uint64_t p = ppu_f2i64(d, rn), q = ppu_f2i64_n(d, rn);
      if (p != q && fails++ < 12) printf("FAIL f2i64 rn=%d %a: %016llx vs %016llx\n", rn, d,
                                         (unsigned long long)p, (unsigned long long)q);
    }
  }
#ifdef PPU_FP_SINGLE_VERIFY
  { const double a = 1.0 + 0x1p-40, b = 3.0; const long before = ppu_fps_bad_n;   /* a is no float */
    eq("fadds verify", ppu_fp_single(ppu_fadd(a, b)), ppu_fadds_sh((float)a, 3.0f, a, b, &o), a, b);
    eq("fmuls verify", ppu_fp_single(ppu_fmul(b, 2.0)), ppu_fmuls_sh(3.0f, 5.0f, b, 2.0, &o), b, 2.0);   /* a wrong shadow */
    { const uint32_t p = ppu_stfs_bits(a), q = ppu_stfs_bits_sh((float)a, a);
      if (p != q && fails++ < 12) printf("FAIL stfs verify\n"); }
    if (ppu_fps_bad_n - before != 3) { fails++; printf("FAIL verify: %ld reports, want 3\n", ppu_fps_bad_n - before); }
    else printf("verify build: wrong claims reported and handled\n"); }
#endif
  printf("fp single/cvt: %ld cases (%.0f%% non-NaN sums), %ld failures\n", n, 100.0 * fast / (n ? n : 1), fails);
  return fails != 0;
}
"""
open(sys.argv[1], "w").write(src)
