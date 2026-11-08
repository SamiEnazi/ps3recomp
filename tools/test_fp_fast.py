"""test_fp_fast.py <out.c>: writes a C test comparing the lifter's scalar FP helpers with
their PPU_FP_ONE_NAN forms (tools/ppu_fastpath.py) bit for bit on special and random
doubles. Build it like tools/test_vmx_sse.c (clang-cl /O2 /arch:AVX2) and run it: exit 0 = equal."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["PPU_FP_ONE_NAN"] = "0"
import ppu_lifter as L      # noqa: E402
import ppu_fastpath as F    # noqa: E402

pre = L.SOURCE_PREAMBLE
a, b = pre.index("static inline double ppu_fp_quiet"), pre.index("/* fsqrt/frsqrte")
old = pre[a:b]
os.environ["PPU_FP_ONE_NAN"] = "1"
new = F.source_preamble(pre)
a2, b2 = new.index("static inline double ppu_fp_quiet"), new.index("/* fsqrt/frsqrte")
new = new[a2:b2]


def rename(text, suf):
    for n in sorted(set(re.findall(r"\b(ppu_[a-z0-9_]+)\s*\(", text)), key=len, reverse=True):
        text = re.sub(r"\b%s\b" % n, n + suf, text)
    return text


src = r"""#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define PPU_FP_AUDIT_INC(i) ((void)0)
""" + rename(old, "_o") + rename(new, "_n") + r"""
static uint64_t s = 0x243F6A8885A308D3ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static const uint64_t SP[] = { 0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000FFFFFFFFFFFFFull,
  0x0010000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
  0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull, 0xFFF4000000000123ull,
  0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x3810000000000000ull, 0x3800000000000000ull, 0x36A0000000000000ull,
  0x3690000000000000ull, 0x380FFFFFE0000000ull, 0x3FF0000010000000ull, 0x4700000000000000ull };
static double dv(void) { uint64_t k = rnd() % 10, u; double d;
  if (k < 3) u = SP[rnd() % (sizeof SP / 8)];
  else if (k < 5) { float f; uint32_t w = (uint32_t)rnd(); memcpy(&f, &w, 4); d = f; return d; }  /* single values */
  else if (k == 5) u = SP[rnd() % (sizeof SP / 8)] ^ (rnd() & 3);
  else u = rnd();
  memcpy(&d, &u, 8); return d; }
static long fails;
static void eq(const char* op, double x, double y, double a, double b, double c) {
  uint64_t u, v; memcpy(&u, &x, 8); memcpy(&v, &y, 8);
  if (u != v && fails++ < 10) printf("FAIL %s %a %a %a: %016llx vs %016llx\n", op, a, b, c, (unsigned long long)u, (unsigned long long)v); }
int main(void) {
  for (long i = 0; i < 4000000; i++) {
    double a = dv(), b = dv(), c = dv();
    eq("fadd", ppu_fadd_o(a, b), ppu_fadd_n(a, b), a, b, 0); eq("fsub", ppu_fsub_o(a, b), ppu_fsub_n(a, b), a, b, 0);
    eq("fmul", ppu_fmul_o(a, b), ppu_fmul_n(a, b), a, b, 0); eq("fdiv", ppu_fdiv_o(a, b), ppu_fdiv_n(a, b), a, b, 0);
    for (int nb = 0; nb < 2; nb++) for (int nr = 0; nr < 2; nr++)
      eq("fmadd", ppu_fmadd_core_o(a, c, b, nb, nr), ppu_fmadd_core_n(a, c, b, nb, nr), a, b, c);
    { uint32_t x = ppu_stfs_bits_o(a), y = ppu_stfs_bits_n(a);
      if (x != y && fails++ < 10) printf("FAIL stfs %a: %08x vs %08x\n", a, x, y); }
  }
  printf("fp: %ld failures\n", fails);
  return fails != 0;
}
"""
open(sys.argv[1], "w").write(src)
