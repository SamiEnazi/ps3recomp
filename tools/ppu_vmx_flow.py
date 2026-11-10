"""Lift-time dataflow fast paths for VMX floats and scalar single FP (ppu-vmx agent, 2026-11-10).

Each switch is read in every lifting worker (they inherit the environment); all of them off
= the lift as before, byte for byte. ppu_fastpath.rewrite_body / source_preamble call in
here after their own rewrites.

  PPU_VMX_CLEAN=1  (with PPU_VMX_SSE and PPU_VMX_FAST, else off) a forward pass over each function's
                   lines tracks which vector locals hold a CLEAN value (no denormal lane, or
                   NJ = 0 -- see runtime/ppu/ppu_vmx_sse.h) and gives every float op the
                   mask of operands that still need their denormal test (vmx_*_k). Chains
                   of float ops then test only their result, in one float compare.
                   PPU_VMX_CLEAN_VERIFY=1: the check build (every untested operand is tested
                   anyway and a wrong claim reported on stderr, the exact form then runs).
  PPU_VMX_INT=1    (with PPU_VMX_SSE, else off) the integer AltiVec ops still done element by element
                   on SSE/AVX2: vslb vsrw vsraw vrlw vcmpgtsw vcmpgtuw vctsxs vctuxs
                   (ppu_vmx_sse.h) and the lifter's inline vadd/vsub u{b,h,w}m and
                   vmax/vmin {s,u}{h,w} loops.
  PPU_FP_SINGLE=1  the same pass tracks which FPR locals hold a single-format value (an lfs,
                   a single-precision op, frsp: exactly a float, or a NaN). fadds / fsubs /
                   fmuls / fdivs / fsqrts of two such values compute in single precision
                   (the double op then the rounding to single gives the same value: double
                   has 53 >= 2*24+2 bits, so the double rounding is innocuous for + - * /
                   sqrt, the exponent range is wide enough and no product or quotient of
                   floats is a double denormal); a NaN result takes the old expression. stfs
                   of such a value stores the float's bits. Each such local keeps a float
                   SHADOW _fsN (the lfs word's float, the single op's float result,
                   (float) of an fmadds' double): chains stay in float registers, no
                   double <-> float round trip per op. PPU_FP_SINGLE_VERIFY=1: the check
                   build (each claim tested, a wrong one reported, old expression).
                   fmadds keeps its double fma + rounding (a single fma would round once
                   where the lift rounds twice). Assumes PPU threads run with the default
                   MXCSR (no DAZ / FTZ), as the existing forms do.
  PPU_FP_CVT=1     fctiw[z] / fctid[z]: one cvt(t)sd2si and a fix for positive overflow
                   instead of the NaN test, the rounding call and two range tests.

The pass is conservative by construction: a local's state is only ever kept or set by a
line whose whole text it recognises; any other line that assigns a local or takes its
address forgets it; labels, function heads and lines with a call forget everything (calls
reload the registers, and nothing but a call or an mtvscr line could change VSCR[NJ]).
Tests: tools/test_vmx_sse.c (the _k forms against ppu_vmx.h for every mask), tools/
test_fp_single.py (the single forms against the lift's expressions), tools/test_ppu_vmx_flow.py
(the pass on sample lines).
"""
import os
import re


def _on(name: str) -> bool:
    return os.environ.get(name, "0") not in ("0", "")


STATS: dict = {}


def _st(key, n=1):
    STATS[key] = STATS.get(key, 0) + n


# The vector parts build on the SSE forms: without PPU_VMX_SSE (PPU_VMX_FAST for CLEAN) they
# stay off, so a lift that does not use those (the firmware modules' lift) is unchanged.
def _clean_on() -> bool:
    return _on("PPU_VMX_CLEAN") and _on("PPU_VMX_SSE") and _on("PPU_VMX_FAST")


def _vint_on() -> bool:
    return _on("PPU_VMX_INT") and _on("PPU_VMX_SSE")


# ---------------------------------------------------------------- source preamble
_SSE_INC = '#include "ppu_vmx_sse.h"'

_F2I_OLD = """\
static inline uint32_t ppu_f2i32(double v, int rn)
{
    if (v != v) return 0x80000000u;
    double r = rn ? nearbyint(v) : trunc(v);
    if (r >= 2147483647.0)  return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static inline uint64_t ppu_f2i64(double v, int rn)
{
    if (v != v) return 0x8000000000000000ULL;
    double r = rn ? nearbyint(v) : trunc(v);
    if (r >= 9223372036854775807.0)  return 0x7FFFFFFFFFFFFFFFULL;
    if (r <= -9223372036854775808.0) return 0x8000000000000000ULL;
    return (uint64_t)(int64_t)r;
}
"""
# cvt(t)sd2si gives the "integer indefinite" (the minimum) for a NaN and for anything out of
# range; the PPC saturates positive overflow to the maximum: the one case to fix. cvtsd2si
# rounds by MXCSR, the mode nearbyint uses.
_F2I_NEW = """\
#include <immintrin.h>
/* PPU_FP_CVT (ppu-vmx): one conversion; the indefinite (minimum) it gives a NaN or an
 * out-of-range value is the PPC result except for positive overflow (the maximum). */
static inline uint32_t ppu_f2i32(double v, int rn)
{
    const __m128d x = _mm_set_sd(v);
    const int32_t i = rn ? _mm_cvtsd_si32(x) : _mm_cvttsd_si32(x);
    return (i == INT32_MIN && v > 0.0) ? 0x7FFFFFFFu : (uint32_t)i;
}
static inline uint64_t ppu_f2i64(double v, int rn)
{
    const __m128d x = _mm_set_sd(v);
    const int64_t i = rn ? _mm_cvtsd_si64(x) : _mm_cvttsd_si64(x);
    return (i == INT64_MIN && v > 0.0) ? 0x7FFFFFFFFFFFFFFFULL : (uint64_t)i;
}
"""

_SINGLE_C = r"""
/* PPU_FP_SINGLE (ppu-vmx, tools/ppu_vmx_flow.py): single-precision ops on values the lift
 * knows are single-format (exactly a float, or a NaN). Each such local fN may carry a float
 * SHADOW _fsN, set where the value is made: _fsN is a NaN exactly when fN is, else
 * (double)_fsN == fN. A non-NaN float result is the old expression's value (innocuous double
 * rounding: 53 >= 2*24+2); a NaN result takes the old expression itself. Both outputs (the
 * double and the new shadow) come back, so a chain of single ops stays in float registers. */
#include <immintrin.h>
#define PPU_FPS_INL static inline __attribute__((always_inline))
PPU_FPS_INL float ppu_w2f(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }   /* an lfs word's shadow */
#ifdef PPU_FP_SINGLE_VERIFY
static long ppu_fps_bad_n;
static int ppu_fps_bad1(float x, double a) {   /* x is not a's shadow, or a not single-format */
    if (a != a) return x == x;
    const double d = x; return memcmp(&d, &a, 8) != 0;
}
static int ppu_fps_bad(float x, float y, double a, double b, const char* op) {
    if (ppu_fps_bad1(x, a) || ppu_fps_bad1(y, b)) {
        const long c = __atomic_add_fetch(&ppu_fps_bad_n, 1, __ATOMIC_RELAXED);
        if (c <= 16 || (c & (c - 1)) == 0)
            fprintf(stderr, "[fp-single-verify] %s: an operand taken as single (or its shadow) is not (#%ld in this file)\n", op, c);
        return 1;
    }
    return 0;
}
#define PPU_FPS_CHECK(x, y, a, b, op, old) \
    if (ppu_fps_bad((x), (y), (a), (b), op)) { const double _r = (old); *o = (float)_r; return _r; }
#else
#define PPU_FPS_CHECK(x, y, a, b, op, old) (void)0
#endif
#define PPU_FPS_BIN(name, OP, old) \
PPU_FPS_INL double name(float x, float y, double a, double b, float* o) { \
    PPU_FPS_CHECK(x, y, a, b, #name, old); \
    const float r = x OP y; *o = r; \
    if (__builtin_expect(r != r, 0)) return old; \
    return (double)r; }
PPU_FPS_BIN(ppu_fadds_sh, +, ppu_fp_single(ppu_fadd(a, b)))
PPU_FPS_BIN(ppu_fsubs_sh, -, ppu_fp_single(ppu_fsub(a, b)))
PPU_FPS_BIN(ppu_fmuls_sh, *, ppu_fp_single(ppu_fmul(a, b)))
PPU_FPS_BIN(ppu_fdivs_sh, /, ppu_fp_single(ppu_fdiv(a, b)))
PPU_FPS_INL double ppu_fsqrts_sh(float y, double b, float* o) {
    PPU_FPS_CHECK(y, y, b, b, "fsqrts", ppu_frsp(ppu_fsqrt(b)));
    const float r = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(y))); *o = r;
    if (__builtin_expect(r != r, 0)) return ppu_frsp(ppu_fsqrt(b));
    return (double)r;
}
/* stfs of a single-format value: a non-NaN one is exactly its shadow, whose bits the store
 * selects; a NaN (it may be signalling) takes the full selection */
PPU_FPS_INL uint32_t ppu_stfs_bits_sh(float x, double v) {
#ifdef PPU_FP_SINGLE_VERIFY
    if (ppu_fps_bad(x, x, v, v, "stfs")) return ppu_stfs_bits(v);
#endif
    if (__builtin_expect(x == x, 1)) { uint32_t w; memcpy(&w, &x, 4); return w; }
    return ppu_stfs_bits(v);
}
"""


def source_preamble(text: str) -> str:
    """The per-chunk source preamble with this module's switched-on parts."""
    clean, vint = _clean_on(), _vint_on()
    if clean or vint:
        if _SSE_INC not in text:
            raise RuntimeError("PPU_VMX_CLEAN / PPU_VMX_INT: the lifter's ppu_vmx_sse.h include changed; update ppu_vmx_flow.py")
        defs = ""
        if clean:
            defs += "#define PPU_VMX_CLEAN 1   /* PPU_VMX_CLEAN (ppu-vmx): operand tests only where a value may be a denormal */\n"
            if _on("PPU_VMX_CLEAN_VERIFY"):
                defs += "#define PPU_VMX_CLEAN_VERIFY 1\n"
        if vint:
            defs += "#define PPU_VMX_INT 1   /* PPU_VMX_INT (ppu-vmx): integer AltiVec ops on SSE/AVX2 */\n"
        text = text.replace(_SSE_INC, defs + _SSE_INC, 1)
    if _on("PPU_FP_CVT"):
        if _F2I_OLD not in text:
            raise RuntimeError("PPU_FP_CVT: the lifter's ppu_f2i32/ppu_f2i64 text changed; update ppu_vmx_flow.py")
        text = text.replace(_F2I_OLD, _F2I_NEW)
    if _on("PPU_FP_SINGLE"):
        text += ("\n#define PPU_FP_SINGLE_VERIFY 1\n" if _on("PPU_FP_SINGLE_VERIFY") else "") + _SINGLE_C
    return text


# ---------------------------------------------------------------- PPU_VMX_INT text rewrites
_OPND = r"(&(?:v\d+|ctx->vr\[\d+\]))"


def _esc_loop(s: str) -> str:
    return re.escape(s).replace(r"OPND", _OPND)


def _int_loops():
    """(regex, replacement) for the lifter's inline element loops (ppu_lifter.py vmx_int_binop
    and the vmax/vmin family), rebuilt from the same f-strings."""
    rx = []
    for ty, cnt, w, mac in (("uint8_t", 16, 1, "PPU_VINT8_SSE"), ("uint16_t", 8, 2, "PPU_VINT16_SSE"),
                            ("uint32_t", 4, 4, "PPU_VINT32_SSE")):
        for op, fn in (("+", "_mm_add_epi%d" % (8 * w)), ("-", "_mm_sub_epi%d" % (8 * w))):
            s = ("{ const uint8_t* a=(const uint8_t*)OPND; const uint8_t* b=(const uint8_t*)OPND; uint8_t d[16]; "
                 f"for(int i=0;i<{cnt};i++){{ {ty} x=0,y=0; "
                 f"for(int k=0;k<{w};k++){{ x=({ty})((x<<8)|a[i*{w}+k]); y=({ty})((y<<8)|b[i*{w}+k]); }} "
                 f"{ty} r=({ty})(x{op}y); "
                 f"for(int k=0;k<{w};k++) d[i*{w}+k]=(uint8_t)(r>>(8*({w}-1-k))); }} memcpy(OPND, d, 16); }}")
            rx.append((re.compile(_esc_loop(s)), r"%s(\3, %s, \1, \2);" % (mac, fn), "vint_" + ("add" if op == "+" else "sub") + str(8 * w)))
    for prefix, cmp in (("max", ">"), ("min", "<")):
        for ty, cnt, w, mac, fn in (("uint16_t", 8, 2, "PPU_VINT16_SSE", "_mm_%s_epu16"), ("int16_t", 8, 2, "PPU_VINT16_SSE", "_mm_%s_epi16"),
                                    ("uint32_t", 4, 4, "PPU_VINT32_SSE", "_mm_%s_epu32"), ("int32_t", 4, 4, "PPU_VINT32_SSE", "_mm_%s_epi32")):
            s = ("{ const uint8_t* a=(const uint8_t*)OPND; const uint8_t* b=(const uint8_t*)OPND; uint8_t o[16]; "
                 f"for(int i=0;i<{cnt};i++){{ uint32_t ux=0,uy=0; "
                 f"for(int k=0;k<{w};k++){{ ux=(ux<<8)|a[i*{w}+k]; uy=(uy<<8)|b[i*{w}+k]; }} "
                 f"{ty} x=({ty})ux, y=({ty})uy; {ty} r=(x{cmp}y)?x:y; "
                 f"for(int k=0;k<{w};k++) o[i*{w}+k]=(uint8_t)((({ty})r)>>(8*({w}-1-k))); }} memcpy(OPND, o, 16); }}")
            rx.append((re.compile(_esc_loop(s)), r"%s(\3, %s, \1, \2);" % (mac, fn % prefix), "vint_" + prefix))
    return rx


_INT_RX = None


def _int_rewrite(ln: str) -> str:
    global _INT_RX
    if "uint8_t* a=(const uint8_t*)" not in ln:
        return ln
    if _INT_RX is None:
        _INT_RX = _int_loops()
    for rx, rep, key in _INT_RX:
        ln, k = rx.subn(rep, ln)
        if k:
            _st(key, k)
    return ln


# ---------------------------------------------------------------- the dataflow pass
ND, LM = 1, 2     # vector states: no denormal lane / every word lane all-ones or zero (LM implies ND)

# Where the state is forgotten: function heads and ends, any label (any name: a jump may land
# there with other values), a line that opens or closes a multi-line block or has an else (the
# lines after it are a join), and a declaration of a register local (an inner scope's local of
# the same name would shadow the tracked one).
_RE_RESET = re.compile(r"^(?:void |static |extern |}|#)|\bloc_[0-9A-Fa-f]+:(?!:)|^\s*[A-Za-z_]\w*\s*:(?!:)"
                       r"|^\s*\}|\{\s*$|\belse\b|\bppu_vr\s+v\d|\bdouble\s+f\d")
_RE_CALLISH = re.compile(r"\(ctx\)|\(ctx,|\bfunc_[0-9A-Fa-f]{8}\(|\bps3_\w+\(|\blv2_\w+\(|DRAIN_TRAMPOLINE")
_RE_CTRL = re.compile(r"\bif\s*\(|\belse\b|\bswitch\b|\bwhile\b|\bcase\b|\bgoto\b|\breturn\b|\bfor\s*\(")
_VKEY = r"v\d+\b|ctx->vr\[\d+\]"
_RE_VADDR = re.compile(r"&(" + _VKEY + r")")
_RE_VASSIGN = re.compile(r"(?<![\w>.])(" + _VKEY + r")\s*=(?!=)")
_FKEY = r"f\d+\b|ctx->fpr\[\d+\]"
_RE_FADDR = re.compile(r"&(" + _FKEY + r")")
_RE_FASSIGN = re.compile(r"(?<![\w>.])(" + _FKEY + r")\s*=(?!=)")
_RE_VSCR_W = re.compile(r"ctx->vscr\s*=(?!=)|ctx->vscr\s*[|&^]=")

# one-call statement forms (the call's own text is group "call")
_CALL = r"(?P<fn>\w+)\((?P<args>[^;{}]*)\)"
_RE_LINE_FORMS = [
    re.compile(r"^(?P<pre>\s*(?:\(void\))?)" + _CALL + r"(?P<post>;\s*)$"),
    re.compile(r"^(?P<pre>\s*\{ uint32_t _c6 = )" + _CALL + r"(?P<post>; cr6 = _c6; \}\s*)$"),
    re.compile(r"^(?P<pre>\s*\{ uint64_t ea = [^;{}]*;(?: ea &= ~3ULL;)? )" + _CALL + r"(?P<post>; \}\s*)$"),
    re.compile(r"^(?P<pre>\s*\{ uint8_t b = [^;{}]*; )" + _CALL + r"(?P<post>; \}\s*)$"),
]
_RE_SPLTISW = re.compile(r"^\s*\{ uint32_t v\[4\]; for\(int i=0;i<4;i\+\+\) v\[i\]=\(uint32_t\)\(int32_t\)(-?\d+); "
                         r"ppu_vstu4\(&(" + _VKEY + r"),v\); \}\s*$")
_RE_SPLTISH = re.compile(r"^\s*\{ uint8_t\* p=\(uint8_t\*\)&(" + _VKEY + r"); uint16_t hv=\(uint16_t\)\(\(int16_t\)(-?\d+)\); "
                         r"for\(int i=0;i<8;i\+\+\)\{ p\[i\*2\]=\(uint8_t\)\(hv>>8\); p\[i\*2\+1\]=\(uint8_t\)hv; \} \}\s*$")
_RE_SPLTISB = re.compile(r"^\s*\{ memset\(&(" + _VKEY + r"), \(uint8_t\)(-?\d+), 16\); \}\s*$")

# float ops: name -> (number of vector sources, state of the result)
_FLOAT = {"vmx_vaddfp": (2, ND), "vmx_vsubfp": (2, ND), "vmx_vmaxfp": (2, ND), "vmx_vminfp": (2, ND),
          "vmx_vmaddfp": (3, ND), "vmx_vnmsubfp": (3, ND), "vmx_vrefp": (1, ND), "vmx_vrsqrtefp": (1, ND),
          "vmx_vcmpeqfp": (2, LM), "vmx_vcmpgefp": (2, LM), "vmx_vcmpgtfp": (2, LM)}
# ops whose first argument is the only vector they write (anything else is read)
_DEST_FIRST_PREFIX = ("vmx_v", "PPU_V", "PPU_LVSL_SSE", "ppu_vperm_sse", "ppu_vld16_sse", "ppu_lvewx_sse",
                      "ppu_lvlx_sse", "ppu_lvrx_sse")
_LM_OPS = ("vmx_vcmpequw", "vmx_vcmpgtsw", "vmx_vcmpgtuw")   # word compares: all-ones / zero words
_ND_OPS = ("vmx_vcfsx", "vmx_vcfux", "vmx_vcmpbfp")   # |int * 2^-n| >= 2^-31 or 0; bound bits 31/30 only


def _split_args(s: str):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth < 0:
                return None
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if depth:
        return None
    out.append(cur.strip())
    return out


def _key(arg: str):
    m = re.fullmatch(r"&(" + _VKEY + r")", arg)
    return m.group(1) if m else None


class _Flow:
    def __init__(self, clean: bool, single: bool):
        self.clean, self.single = clean, single
        self.vs: dict = {}
        self.fs: set = set()     # FPR keys holding a single-format value
        self.sh: set = set()     # f-locals whose float shadow _fsN is that value's (a subset of fs)
        self.used: set = set()   # shadows the current function uses (declared after its head)

    def reset(self):
        self.vs.clear()
        self.fs.clear()
        self.sh.clear()

    # ---- vectors
    def vget(self, k):
        return self.vs.get(k, 0)

    def vset(self, k, s):
        if s:
            self.vs[k] = s
        else:
            self.vs.pop(k, None)

    def vforget_line(self, ln):
        if _RE_VSCR_W.search(ln):
            self.vs.clear()
            return
        for m in _RE_VADDR.finditer(ln):
            self.vs.pop(m.group(1), None)
        for m in _RE_VASSIGN.finditer(ln):
            self.vs.pop(m.group(1), None)

    def vector_line(self, ln: str) -> str:
        m = _RE_SPLTISW.match(ln)
        if m:
            v = int(m.group(1))
            self.vset(m.group(2), LM if v in (0, -1) else ND if v < 0 else 0)
            return ln
        m = _RE_SPLTISH.match(ln)
        if m:
            v = int(m.group(2))
            self.vset(m.group(1), LM if v in (0, -1) else ND if v < 0 else 0)
            return ln
        m = _RE_SPLTISB.match(ln)
        if m:   # every byte b: a word bbbb has exponent field 2*(b & 0x7F) + (b >> 7), zero only for b = 0
            v = int(m.group(2))
            self.vset(m.group(1), LM if v in (0, -1) else ND)
            return ln
        for rx in _RE_LINE_FORMS:
            m = rx.match(ln)
            if m:
                break
        else:
            self.vforget_line(ln)
            return ln
        fn, args = m.group("fn"), _split_args(m.group("args"))
        rest = m.group("pre") + m.group("post")
        if args is None or re.search(r"\bv\d+\b|ctx->vr\[", rest):
            self.vforget_line(ln)
            return ln
        if fn == "ppu_vst16_sse":   # (ea, &src): a store, no vector written
            if _RE_VADDR.search(args[0]) or _RE_VASSIGN.search(args[0]):
                self.vforget_line(ln)
            return ln
        if not fn.startswith(_DEST_FIRST_PREFIX):
            self.vforget_line(ln)
            return ln
        d = _key(args[0])
        srcs = [_key(a) for a in args[1:]]
        if d is None or any(_RE_VADDR.search(a) for a, k in zip(args[1:], srcs) if k is None):
            self.vforget_line(ln)
            return ln
        S = self.vget
        new = 0
        if fn in _FLOAT and self.clean:
            n, res = _FLOAT[fn]
            vsrc = srcs[:n]
            if len(args) == n + 2 and all(vsrc) and args[n + 1] == "ctx->vscr":
                kmask = sum(1 << i for i, k in enumerate(vsrc) if not S(k))
                _st("vmx_ops")
                _st("vmx_tests_kept", bin(kmask).count("1"))
                _st("vmx_tests_dropped", n - bin(kmask).count("1"))
                call = "%s_k(%s, &ctx->vscr, %du)" % (fn, ", ".join(args[:n + 1]), kmask)
                ln = m.group("pre") + call + m.group("post")
                new = res
        elif fn in _LM_OPS:
            new = LM
        elif fn in _ND_OPS:
            new = ND
        elif fn == "PPU_VLOGIC_SSE" and len(args) == 4:
            op, a, b = args[1], srcs[1], srcs[2]
            if a and b:
                sa, sb = S(a), S(b)
                if op == "_mm_xor_si128":
                    new = LM if a == b or (sa == LM and sb == LM) else 0
                elif op in ("_mm_and_si128", "_mm_or_si128"):
                    # a op a = a; with one lane mask, every lane is the other value's lane, 0 or all-ones
                    new = sa if a == b else min(sa, sb) if LM in (sa, sb) else 0
                elif op == "ppu_vandc_sse":   # a & ~b
                    new = LM if a == b else (sa if sb == LM else 0) if sa != LM else (LM if sb == LM else 0)
        elif fn == "PPU_VSEL_SSE" and len(args) == 4 and all(srcs):
            a, b, c = srcs
            # each bit from a or b: with c a lane mask, every lane is a's or b's
            new = S(a) if a == b else min(S(a), S(b)) if S(c) == LM else 0
        elif fn == "PPU_VSPLTW_SSE" and len(args) == 3 and srcs[0]:
            new = S(srcs[0])
        elif fn == "PPU_VSLDOI_SSE" and len(args) == 4 and srcs[0] and srcs[1] and args[3].isdigit():
            sh = int(args[3])
            new = (S(srcs[0]) if sh == 0 else min(S(srcs[0]), S(srcs[1]))) if sh % 4 == 0 else 0
        elif fn == "PPU_VMRG_SSE" and len(args) == 4 and srcs[0] and srcs[1]:
            new = min(S(srcs[0]), S(srcs[1])) if args[3].endswith("_epi32") else 0
        self.vset(d, new)
        return ln

    # ---- scalar FP
    def _shadow(self, k):   # the float of a single-format key: its shadow, or a conversion
        if k in self.sh:
            self.used.add(k)
            return "_fs" + k[1:]
        return "(float)" + k

    def _fset(self, d, single, shadow=False):
        (self.fs.add if single else self.fs.discard)(d)
        if shadow and d.startswith("f"):
            self.sh.add(d)
            self.used.add(d)
        else:
            self.sh.discard(d)

    def fp_line(self, ln: str) -> str:
        F = r"(" + _FKEY + r")"
        L = r"(f\d+)"   # a local: it can have a shadow
        m = re.match(r"^(\s*)" + L + r" = ppu_fp_single\(ppu_(fadd|fsub|fmul|fdiv)\(" + L + r", " + L + r"\)\);\s*$", ln)
        if m and m.group(4) in self.fs and m.group(5) in self.fs:
            ind, d, op, a, b = m.groups()
            ln = "%s%s = ppu_%ss_sh(%s, %s, %s, %s, &_fs%s);" % (ind, d, op, self._shadow(a), self._shadow(b), a, b, d[1:])
            _st("fp_single_ops")
            self._fset(d, True, True)
            return ln
        m = re.match(r"^(\s*)" + L + r" = ppu_frsp\(ppu_fsqrt\(" + L + r"\)\);\s*$", ln)
        if m and m.group(3) in self.fs:
            ind, d, b = m.groups()
            ln = "%s%s = ppu_fsqrts_sh(%s, %s, &_fs%s);" % (ind, d, self._shadow(b), b, d[1:])
            _st("fp_single_ops")
            self._fset(d, True, True)
            return ln
        m = re.match(r"^(\s*)" + L + r" = ppu_fp_single\(([^;]*)\);\s*$", ln)
        if m:
            # any single-precision result (fmadds ...): fp_single is (double)(float)x, spelt out
            # so the float stays as the shadow
            ind, d, expr = m.groups()
            _st("fp_single_kept" if expr.startswith(("ppu_fadd(", "ppu_fsub(", "ppu_fmul(", "ppu_fdiv(")) else "fp_single_shadowed")
            ln = "%s{ const float _t = (float)(%s); %s = (double)_t; _fs%s = _t; }" % (ind, expr, d, d[1:])
            self._fset(d, True, True)
            return ln
        m = re.match(r"^(\s*)" + L + r" = ppu_lfs_val\(([^;]*)\);\s*$", ln)
        if m:   # lfs: the word's float is the shadow (a NaN word: a NaN float, the double keeps its bits)
            ind, d, expr = m.groups()
            ln = "%s{ const uint32_t _lw = %s; %s = ppu_lfs_val(_lw); _fs%s = ppu_w2f(_lw); }" % (ind, expr, d, d[1:])
            _st("fp_lfs_shadowed")
            self._fset(d, True, True)
            return ln
        m = re.match(r"^(\s*)" + F + r" = (-?)(fabs\()?" + F + r"(\)?);\s*$", ln)
        if m and bool(m.group(4)) == bool(m.group(6)):
            # fmr / fneg / fabs / fnabs: a sign change keeps a float a float (and a NaN a NaN)
            ind, d, neg, fab, b = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
            if b in self.fs and b in self.sh and d.startswith("f"):
                ln = ln.rstrip() + " _fs%s = %s%s%s%s;" % (d[1:], neg, "fabsf(" if fab else "", self._shadow(b), ")" if fab else "")
                self._fset(d, True, True)
            else:
                self._fset(d, b in self.fs)
            return ln
        m = re.match(r"^\s*" + F + r" = \(" + F + r" >= 0\.0\) \? " + F + r" : " + F + r";\s*$", ln)
        if m:   # fsel
            self._fset(m.group(1), m.group(3) in self.fs and m.group(4) in self.fs)
            return ln
        ctrl = bool(_RE_CTRL.search(ln))
        assigned = [mm.group(1) for mm in _RE_FASSIGN.finditer(ln)]
        addr = {mm.group(1) for mm in _RE_FADDR.finditer(ln)}
        if not ctrl and "ppu_stfs_bits(" in ln:
            def stfs(mm):
                k = mm.group(1)
                if k in self.fs and k not in assigned and k not in addr:
                    _st("fp_stfs")
                    return "ppu_stfs_bits_sh(%s, %s)" % (self._shadow(k), k)
                return mm.group(0)
            ln = re.sub(r"\bppu_stfs_bits\((" + _FKEY + r")\)", stfs, ln)
        for k in assigned:
            self._fset(k, False)
        for k in addr:
            self._fset(k, False)
        if not ctrl:
            for mm in re.finditer(r"(?<![\w>.])" + F + r" = (?:ppu_lfs_val|ppu_fp_single|ppu_frsp|\(double\)\(float\))\(", ln):
                if assigned.count(mm.group(1)) == 1 and mm.group(1) not in addr:
                    self._fset(mm.group(1), True)
        return ln

    def line(self, ln: str) -> str:
        if _RE_RESET.search(ln):
            self.reset()
            return ln
        if _RE_CALLISH.search(ln):
            self.reset()
            return ln
        if self.clean:
            ln = self.vector_line(ln)
        if self.single:
            ln = self.fp_line(ln)
        return ln


def rewrite(lines: list) -> list:
    """A chunk's function lines with this module's switched-on statement rewrites."""
    clean, single, vint = _clean_on(), _on("PPU_FP_SINGLE"), _vint_on()
    if not (clean or single or vint):
        return lines
    flow = _Flow(clean, single)
    before = dict(STATS)
    out = []
    head = -1   # the open function's head line in out

    def declare():   # the shadows the function used, declared at its top (no initialiser: a goto may pass them)
        if head >= 0 and flow.used:
            out.insert(head + 1, "        float %s;   /* PPU_FP_SINGLE shadows (ppu_vmx_flow.py) */"
                       % ", ".join("_fs%s" % k[1:] for k in sorted(flow.used, key=lambda k: int(k[1:]))))
        flow.used.clear()

    for ln in lines:
        if ln.startswith(("void ", "}")):
            declare()
            head = len(out) if re.match(r"^void \w+\(ppu_context\* ctx\) \{\s*$", ln) else -1
        if vint:
            ln = _int_rewrite(ln)
        if clean or single:
            ln = flow.line(ln)
        out.append(ln)
    declare()
    delta = {k: v - before.get(k, 0) for k, v in STATS.items() if v != before.get(k, 0)}
    print("  ppu_vmx_flow: " + (", ".join("%s %d" % kv for kv in sorted(delta.items())) or "nothing"), flush=True)
    return out
