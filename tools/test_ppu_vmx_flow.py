"""test_ppu_vmx_flow.py: tools/ppu_vmx_flow.py's pass on sample lifted lines (python -I, exit 0 = ok).

Checks the operand masks it gives the float ops, what keeps / forgets a local's state (labels,
calls, VSCR writes, unknown statements, control flow), the scalar single forms, and that with
every switch off the lines come back unchanged."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_vmx_flow as W   # noqa: E402

SW = ("PPU_VMX_SSE", "PPU_VMX_FAST", "PPU_VMX_CLEAN", "PPU_FP_SINGLE", "PPU_VMX_INT")
fails = 0


def run(lines, on=SW):
    for k in SW + ("PPU_FP_CVT",):
        os.environ[k] = "1" if k in on else "0"
    global DECL
    out = W.rewrite(["void func_00010000(ppu_context* ctx) {"] + [("        " + ln) for ln in lines] + ["}"])[1:-1]
    DECL = [ln.strip() for ln in out if "PPU_FP_SINGLE shadows" in ln]
    return [ln for ln in out if "PPU_FP_SINGLE shadows" not in ln]


DECL = []


def expect(name, lines, want, on=SW):
    global fails
    got = [ln.strip() for ln in run(lines, on)]
    for i, (g, w) in enumerate(zip(got, want)):
        if w is not None and g != w:
            fails += 1
            print(f"FAIL {name} line {i}:\n  got  {g}\n  want {w}")


LD = "{ uint64_t ea = (r3) & ~0xFULL; ppu_vld16_sse(&v1, ea); }"
expect("chain", [LD, "vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", "vmx_vmaddfp(&v3, &v2, &v2, &v2, ctx->vscr);",
                 "vmx_vnmsubfp(&v4, &v3, &v1, &v2, ctx->vscr);", "vmx_vrsqrtefp(&v5, &v4, ctx->vscr);"],
       [None, "vmx_vaddfp_k(&v2, &v1, &v1, &ctx->vscr, 3u);", "vmx_vmaddfp_k(&v3, &v2, &v2, &v2, &ctx->vscr, 0u);",
        "vmx_vnmsubfp_k(&v4, &v3, &v1, &v2, &ctx->vscr, 2u);", "vmx_vrsqrtefp_k(&v5, &v4, &ctx->vscr, 0u);"])
expect("label resets", ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", "loc_00010010:", "vmx_vsubfp(&v3, &v2, &v2, ctx->vscr);"],
       [None, None, "vmx_vsubfp_k(&v3, &v2, &v2, &ctx->vscr, 3u);"])
expect("call resets", ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);",
                       "ctx->lr = 0x10020; ctx->vr[2] = v2; func_00020000(ctx); v2 = ctx->vr[2];",
                       "vmx_vsubfp(&v3, &v2, &v2, ctx->vscr);"],
       [None, None, "vmx_vsubfp_k(&v3, &v2, &v2, &ctx->vscr, 3u);"])
for brk in ("xi3_ret_00010010:", "}", "if (cr6 & 2) {", "{", "ppu_vr v2 = v7;", "if (r3) { r4 = 1; } else { r4 = 2; }"):
    expect("reset at " + brk, ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", brk, "vmx_vsubfp(&v3, &v2, &v2, ctx->vscr);"],
           [None, None, "vmx_vsubfp_k(&v3, &v2, &v2, &ctx->vscr, 3u);"])
    expect("fp reset at " + brk, ["f1 = ppu_lfs_val(vm_read32(r3 + 0x4));", brk,
                                  "f3 = ppu_fp_single(ppu_fadd(f1, f1));"], [None, None, "{ const float _t = (float)(ppu_fadd(f1, f1)); f3 = (double)_t; _fs3 = _t; }"])
expect("vscr write resets", ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);",
                             "{ const uint8_t* _b = (const uint8_t*)&ctx->vr[9]; ctx->vscr = ((uint32_t)_b[12] << 24) & 0x00010001u; }",
                             "vmx_vsubfp(&v3, &v2, &v2, ctx->vscr);"],
       [None, None, "vmx_vsubfp_k(&v3, &v2, &v2, &ctx->vscr, 3u);"])
expect("unknown statement forgets", ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", "vmx_vaddfp(&v6, &v1, &v1, ctx->vscr);",
                                     "{ uint8_t* p = (uint8_t*)&v2; p[3] = 0; }", "ctx->vr[6] = v6;",
                                     "vmx_vsubfp(&v3, &v2, &v6, ctx->vscr);"],
       [None, None, None, None, "vmx_vsubfp_k(&v3, &v2, &v6, &ctx->vscr, 1u);"])
expect("lane masks", ["PPU_VLOGIC_SSE(&v8, _mm_xor_si128, &v8, &v8);",
                      "{ uint32_t v[4]; for(int i=0;i<4;i++) v[i]=(uint32_t)(int32_t)-1; ppu_vstu4(&v9,v); }",
                      "PPU_VSLDOI_SSE(&v3, &v9, &v8, 4);", LD, "vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);",
                      "PPU_VLOGIC_SSE(&v4, _mm_and_si128, &v2, &v3);", "PPU_VSEL_SSE(&v5, &v4, &v2, &v3);",
                      "PPU_VSEL_SSE(&v6, &v4, &v2, &v1);", "PPU_VSPLTW_SSE(&v7, &v5, 2);",
                      "vmx_vmaddfp(&v10, &v5, &v6, &v7, ctx->vscr);", "vmx_vmaddfp(&v11, &v8, &v9, &v3, ctx->vscr);"],
       [None] * 9 + ["vmx_vmaddfp_k(&v10, &v5, &v6, &v7, &ctx->vscr, 2u);",
                     "vmx_vmaddfp_k(&v11, &v8, &v9, &v3, &ctx->vscr, 0u);"])
expect("odd shifts and halfword merges", ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", "PPU_VSLDOI_SSE(&v3, &v2, &v2, 3);",
                                          "PPU_VMRG_SSE(&v4, &v2, &v2, _mm_unpacklo_epi16);", "PPU_VMRG_SSE(&v5, &v2, &v2, _mm_unpackhi_epi32);",
                                          "vmx_vmaddfp(&v6, &v3, &v4, &v5, ctx->vscr);"],
       [None] * 4 + ["vmx_vmaddfp_k(&v6, &v3, &v4, &v5, &ctx->vscr, 3u);"])
expect("splat immediates", ["{ uint32_t v[4]; for(int i=0;i<4;i++) v[i]=(uint32_t)(int32_t)1; ppu_vstu4(&v1,v); }",
                            "{ uint32_t v[4]; for(int i=0;i<4;i++) v[i]=(uint32_t)(int32_t)-5; ppu_vstu4(&v2,v); }",
                            "{ memset(&v3, (uint8_t)1, 16); }", "vmx_vmaddfp(&v4, &v1, &v2, &v3, ctx->vscr);",
                            "{ uint8_t* p=(uint8_t*)&v5; uint16_t hv=(uint16_t)((int16_t)3); for(int i=0;i<8;i++){ p[i*2]=(uint8_t)(hv>>8); p[i*2+1]=(uint8_t)hv; } }",
                            "vmx_vaddfp(&v6, &v5, &v4, ctx->vscr);"],
       [None] * 3 + ["vmx_vmaddfp_k(&v4, &v1, &v2, &v3, &ctx->vscr, 1u);", None, "vmx_vaddfp_k(&v6, &v5, &v4, &ctx->vscr, 1u);"])
expect("compares and stores", ["{ uint32_t _c6 = vmx_vcmpgtfp(&v2, &v1, &v1, ctx->vscr); cr6 = _c6; }",
                               "{ uint64_t ea = (r3) & ~0xFULL; ppu_vst16_sse(ea, &v2); }", "(void)vmx_vcmpequw(&v3, &v1, &v1);",
                               "vmx_vmaxfp(&v4, &v2, &v3, ctx->vscr);", "vmx_vcfsx(&v5, &v1, 3);", "(void)vmx_vcmpeqfp(&v6, &v5, &v4, ctx->vscr);"],
       ["{ uint32_t _c6 = vmx_vcmpgtfp_k(&v2, &v1, &v1, &ctx->vscr, 3u); cr6 = _c6; }", None, None,
        "vmx_vmaxfp_k(&v4, &v2, &v3, &ctx->vscr, 0u);", None, "(void)vmx_vcmpeqfp_k(&v6, &v5, &v4, &ctx->vscr, 0u);"])
expect("dest-first ops write only the dest", ["vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", "ppu_vperm_sse(&v3, &v2, &v2, &v1);",
                                              "vmx_vslb(&v4, &v2, &v2);", "vmx_vsubfp(&v5, &v2, &v3, ctx->vscr);"],
       [None, None, None, "vmx_vsubfp_k(&v5, &v2, &v3, &ctx->vscr, 2u);"])
expect("int loops", ["{ const uint8_t* a=(const uint8_t*)&v1; const uint8_t* b=(const uint8_t*)&v2; uint8_t d[16]; for(int i=0;i<4;i++){ uint32_t x=0,y=0; for(int k=0;k<4;k++){ x=(uint32_t)((x<<8)|a[i*4+k]); y=(uint32_t)((y<<8)|b[i*4+k]); } uint32_t r=(uint32_t)(x+y); for(int k=0;k<4;k++) d[i*4+k]=(uint8_t)(r>>(8*(4-1-k))); } memcpy(&v3, d, 16); }"],
       ["PPU_VINT32_SSE(&v3, _mm_add_epi32, &v1, &v2);"])
# scalar single
LFS1, LFS2 = "f1 = ppu_lfs_val(vm_read32(r3 + 0x4));", "f2 = ppu_lfs_val(vm_read32(r3 + 0x8));"
expect("fp chain", [LFS1, LFS2, "f3 = ppu_fp_single(ppu_fsub(f1, f2));", "f4 = ppu_fp_single(ppu_fmadd_core(f1, f3, f2, 0, 0));",
                    "f5 = ppu_fp_single(ppu_fmul(f4, f3));", "f6 = ppu_frsp(ppu_fsqrt(f5));", "vm_write32(r3 + 0xC, ppu_stfs_bits(f6));",
                    "f7 = -f6;", "f8 = (f7 >= 0.0) ? f6 : f5;", "f9 = ppu_fp_single(ppu_fdiv(f8, f7));"],
       ["{ const uint32_t _lw = vm_read32(r3 + 0x4); f1 = ppu_lfs_val(_lw); _fs1 = ppu_w2f(_lw); }", None,
        "f3 = ppu_fsubs_sh(_fs1, _fs2, f1, f2, &_fs3);",
        "{ const float _t = (float)(ppu_fmadd_core(f1, f3, f2, 0, 0)); f4 = (double)_t; _fs4 = _t; }",
        "f5 = ppu_fmuls_sh(_fs4, _fs3, f4, f3, &_fs5);", "f6 = ppu_fsqrts_sh(_fs5, f5, &_fs6);",
        "vm_write32(r3 + 0xC, ppu_stfs_bits_sh(_fs6, f6));", "f7 = -f6; _fs7 = -_fs6;", None,
        "f9 = ppu_fdivs_sh((float)f8, _fs7, f8, f7, &_fs9);"])
if DECL != ["float _fs1, _fs2, _fs3, _fs4, _fs5, _fs6, _fs7, _fs9;   /* PPU_FP_SINGLE shadows (ppu_vmx_flow.py) */"]:
    fails += 1
    print("FAIL shadow declaration:", DECL)
expect("fp unknowns", [LFS1, "{ int64_t iv; memcpy(&iv, &f1, 8); f1 = (double)iv; }", LFS2,
                       "f3 = ppu_fp_single(ppu_fadd(f1, f2));", "f4 = ctx->fpr[4];", "f5 = ppu_fp_single(ppu_fadd(f2, f4));",
                       "if (cr6 & 2) { f2 = f4; }", "vm_write32(r3, ppu_stfs_bits(f2));", "loc_00010040:", "vm_write32(r3, ppu_stfs_bits(f3));"],
       [None, None, None, "{ const float _t = (float)(ppu_fadd(f1, f2)); f3 = (double)_t; _fs3 = _t; }", None,
        "{ const float _t = (float)(ppu_fadd(f2, f4)); f5 = (double)_t; _fs5 = _t; }",
        None, "vm_write32(r3, ppu_stfs_bits(f2));", None, "vm_write32(r3, ppu_stfs_bits(f3));"])
# everything off: the same lines
sample = [LD, "vmx_vaddfp(&v2, &v1, &v1, ctx->vscr);", LFS1, LFS2, "f3 = ppu_fp_single(ppu_fsub(f1, f2));"]
if run(sample, on=()) != ["        " + ln for ln in sample]:
    fails += 1
    print("FAIL switches off changed the text")
print(f"ppu_vmx_flow: {fails} failures")
sys.exit(1 if fails else 0)
