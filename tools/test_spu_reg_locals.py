#!/usr/bin/env python3
"""spu_lifter.reg_localize: SPU registers in C locals must behave exactly like ctx->gpr[].

Structural checks on the rewritten text (write-backs only before exits and runtime calls,
re-reads after them, functions with an unknown transfer left alone, the environment
switch), then a semantic check: a synthetic lifted function with a loop, LS loads/stores,
conditional exits, a runtime call and a nested call is compiled in both forms against a
small stub runtime and run on random register files / local stores; the log of every
runtime call (with the register file at that moment), the exit and the final state must
match. The real lifted tree is compared the same way by the port's equivalence harness.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spu_lifter  # noqa: E402
from spu_lifter import reg_localize  # noqa: E402

BODY = [
    "loc_00000100:",
    "ctx->gpr[5] = spu_ls_read128(ctx, ctx->gpr[3]._u32[0] + 0x0);",
    "/* nop */;",
    "ctx->gpr[6] = spu_a(ctx->gpr[5], ctx->gpr[6]);",
    "spu_ls_write128(ctx, ctx->gpr[3]._u32[0] + 0x10, ctx->gpr[6]);",
    "ctx->gpr[3] = spu_ai(ctx->gpr[3], 32);",
    "if (ctx->gpr[6]._u32[0] == 7) { SPU_DIRECT(ctx, 0x200, f_200); }",
    "ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);",
    "if ((int32_t)ctx->gpr[4]._u32[0] == (int32_t)(-9)) { ctx->status = 9; return; }",
    "if (ctx->gpr[4]._u32[1] & 1) goto loc_00000140;",
    "spu_wrch(ctx, 7, ctx->gpr[4]);",
    "ctx->gpr[7] = spu_rdch(ctx, 3);",
    "loc_00000140:",
    "ctx->gpr[8] = spu_a(ctx->gpr[7], ctx->gpr[5]);",
    "ctx->gpr[0] = spu_link(0x150); { ctx->host_depth++; f_callee(ctx); ctx->host_depth--; }",
    "if (ctx->gpr[4]._u32[0] != 0) goto loc_00000100;",
    "ctx->gpr[9] = spu_ai(ctx->gpr[8], 1);",
    "ctx->gpr[0] = spu_link(0x160); goto loc_00000164;",
    "loc_00000164:",
    "ctx->gpr[10] = spu_a(ctx->gpr[9], ctx->gpr[0]);",
]

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef union { uint32_t _u32[4]; uint64_t _u64[2]; } u128;
typedef struct { u128 gpr[128]; uint8_t ls[65536]; uint32_t pc, status, host_depth; } spu_context;
static uint64_t H;
static void mix(uint64_t v) { H = (H ^ v) * 0x100000001B3ull; }
static void mixctx(spu_context* c) { for (int i = 0; i < 128; i++) { mix(c->gpr[i]._u64[0]); mix(c->gpr[i]._u64[1]); } }
static u128 spu_ls_read128(spu_context* c, uint32_t a) { u128 r; memcpy(&r, c->ls + (a & 0xFFF0u), 16); return r; }
static void spu_ls_write128(spu_context* c, uint32_t a, u128 v) { memcpy(c->ls + (a & 0xFFF0u), &v, 16); }
static u128 spu_a(u128 a, u128 b) { for (int i = 0; i < 4; i++) a._u32[i] += b._u32[i]; return a; }
static u128 spu_ai(u128 a, int32_t k) { for (int i = 0; i < 4; i++) a._u32[i] += (uint32_t)k; return a; }
static u128 spu_link(uint32_t a) { u128 r = { { a, 0, 0, 0 } }; return r; }
static void spu_wrch(spu_context* c, int ch, u128 v) { mix(ch); mix(v._u64[0]); mixctx(c); }
static u128 spu_rdch(spu_context* c, int ch) { mix(ch); mixctx(c); u128 r = { { (uint32_t)H, 1, 2, 3 } }; return r; }
static void f_callee(spu_context* c) { mixctx(c); c->gpr[7]._u32[0] ^= 0x55u; c->gpr[3]._u32[2] += 1; }
#define SPU_DIRECT(ctx, PC, FN) do { (ctx)->pc = (PC); mixctx(ctx); return; } while (0)
"""

MAIN = r"""
int main(void) {
    static spu_context c;
    uint64_t s = 88172645463325252ull;
    for (int t = 0; t < 2000; t++) {
        for (int i = 0; i < 128; i++) for (int k = 0; k < 4; k++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; c.gpr[i]._u32[k] = (uint32_t)s % 23u; }
        for (int i = 0; i < 65536; i += 4) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; uint32_t w = (uint32_t)s % 5u; memcpy(c.ls + i, &w, 4); }
        c.gpr[3]._u32[0] = (uint32_t)(s % 4096u) * 16u; c.status = 0; c.pc = 0;
        H = 0xCBF29CE484222325ull;
        f(&c);
        mixctx(&c); mix(c.pc); mix(c.status);
        for (int i = 0; i < 65536; i += 8) { uint64_t w; memcpy(&w, c.ls + i, 8); mix(w); }
        printf("%016llx\n", (unsigned long long)H);
    }
    return 0;
}
"""


def emit(body):
    return "void f(spu_context* ctx) {\n" + "\n".join(
        b if b.endswith(":") else "    " + b for b in body) + "\n}\n"


def structural():
    out = reg_localize(BODY)
    assert out[0].startswith("u128 R_"), out[0]
    text = "\n".join(out)
    # plain statements use locals; the loop has no ctx->gpr store on its fall-through path
    assert "R_6 = spu_a(R_5, R_6);" in text
    assert "spu_ls_write128(ctx, R_3._u32[0] + 0x10, R_6);" in text
    # the conditional exit writes back inside its branch only
    line = next(o for o in out if o.startswith("if (R_6._u32[0] == 7)"))
    assert "ctx->gpr[6] = R_6;" in line and "SPU_DIRECT" in line, line
    # the runtime call runs on the register file, then the locals are re-read
    i = out.index("spu_wrch(ctx, 7, ctx->gpr[4]);")
    assert "ctx->gpr[4] = R_4;" in out[:i], "write-back before the runtime call"
    assert out[i + 1] == "ctx->gpr[7] = spu_rdch(ctx, 3);", "consecutive calls share one write-back"
    assert out[i + 2].startswith("R_"), "re-read after the calls"
    # link + goto on one line is split into a plain def and the goto
    assert "R_0 = spu_link(0x160);" in out and "goto loc_00000164;" in out
    # a goto into nowhere / an unknown transfer: the function stays in the old form
    assert reg_localize(["ctx->gpr[3] = spu_ai(ctx->gpr[3], 1);", "goto loc_00000999;"]) == \
        ["ctx->gpr[3] = spu_ai(ctx->gpr[3], 1);", "goto loc_00000999;"]
    odd = ["ctx->gpr[3] = spu_ai(ctx->gpr[3], 1);", "{ x = 1; goto loc_00000100; }", "loc_00000100:"]
    assert reg_localize(odd) == odd
    # falling off the end writes back
    tail = reg_localize(["ctx->gpr[3] = spu_ai(ctx->gpr[3], 1);"])
    assert tail[-1] == "ctx->gpr[3] = R_3;", tail
    # the switch: an SPULifter with reg_locals off, or with --trace, emits the old form
    os.environ.pop("SPU_LIFT_REG_LOCALS", None)
    assert not spu_lifter.SPULifter().reg_locals          # opt-in
    os.environ["SPU_LIFT_REG_LOCALS"] = "1"
    assert spu_lifter.SPULifter().reg_locals and not spu_lifter.SPULifter(trace=True).reg_locals
    del os.environ["SPU_LIFT_REG_LOCALS"]
    assert spu_lifter.SPULifter(reg_locals=True).reg_locals
    print("PASS: reg_localize structure")


def semantic():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        print("SKIP: no C compiler for the semantic check")
        return
    with tempfile.TemporaryDirectory() as tmp:
        res = []
        for name, body in (("old", BODY), ("new", reg_localize(BODY))):
            src = os.path.join(tmp, name + ".c")
            with open(src, "w") as f:
                f.write(PRELUDE + emit(body) + MAIN)
            exe = os.path.join(tmp, name)
            subprocess.run([cc, "-O2", "-fno-strict-aliasing", "-w", src, "-o", exe], check=True)
            try:   # a lost write-back can leave the loop counter stuck
                res.append(subprocess.run([exe], check=True, capture_output=True, text=True,
                                          timeout=60).stdout)
            except subprocess.TimeoutExpired:
                raise AssertionError(f"{name} form did not finish")
        assert res[0] == res[1], "register-locals form diverged from ctx->gpr form"
        assert len(set(res[0].split())) > 100, "test inputs do not exercise the paths"
    print("PASS: reg_localize semantics (2000 random states)")


if __name__ == "__main__":
    random.seed(1)
    structural()
    semantic()
