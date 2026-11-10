#!/usr/bin/env python3
"""spu_lifter SPU_PATCH_FILE: run-time instruction alternatives (load_spu_patches, _patch_line).

Lifts a small synthetic image with and without a patch file and checks the text: only the
listed instructions change, each into `if (g_spu_patch[slot] == k) { alt k } ... else { orig }`
with the lifter's own translations; the image registers its switches from a constructor; a
patch for another image changes nothing; a wrong original word, a branch as an alternative, an
alternative writing r0, an unconditional original, an unknown switch and an address outside the
lift each stop the lift.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from spu_disasm import spu_decode  # noqa: E402

LIFTER = os.path.join(HERE, "spu_lifter.py")


def il(rt, imm): return (0x081 << 23) | ((imm & 0xFFFF) << 7) | rt
def ai(rt, ra, imm): return (0x1C << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt
def brnz(rt, words): return (0x042 << 23) | ((words & 0xFFFF) << 7) | rt
def bi(ra): return (0x1A8 << 21) | (ra << 7)


NOP = 0x4020007F
CODE = [ai(3, 3, 1),        # 0x00
        brnz(4, 3),         # 0x04 -> 0x10
        il(5, 7),           # 0x08
        bi(0),              # 0x0C
        il(6, 1),           # 0x10
        bi(0)]              # 0x14
WANT = ["ai", "brnz", "il", "bi", "il", "bi"]


def lift(tmp, patch_text, tag):
    img = os.path.join(tmp, "img.bin")
    with open(img, "wb") as f:
        f.write(b"".join(struct.pack(">I", w) for w in CODE))
    fj = os.path.join(tmp, "funcs.json")
    with open(fj, "w") as f:
        json.dump([{"start": "0x0", "end": "0x18"}], f)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SPU_")}
    if patch_text is not None:
        pf = os.path.join(tmp, f"patches_{tag}.txt")
        with open(pf, "w", encoding="utf-8") as f:
            f.write(patch_text)
        env["SPU_PATCH_FILE"] = pf
    out = os.path.join(tmp, "out_" + tag)
    r = subprocess.run([sys.executable, "-I", LIFTER, img, "--base", "0", "--functions", fj,
                        "--symbol-prefix", "testimg_", "-o", out], capture_output=True, text=True, env=env)
    src = os.path.join(out, "spu_recomp.c")
    return r, (open(src, encoding="utf-8").read() if r.returncode == 0 else "")


def main():
    for w, m in zip(CODE, WANT):
        assert spu_decode(w, 0).mnemonic == m, (hex(w), spu_decode(w, 0).mnemonic, m)
    assert spu_decode(NOP, 0).mnemonic == "nop"
    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("ok    " if cond else "FAIL  ") + what)
        fails += 0 if cond else 1

    with tempfile.TemporaryDirectory() as tmp:
        r0, base = lift(tmp, None, "none")
        check(r0.returncode == 0 and "g_spu_patch" not in base, "no SPU_PATCH_FILE: no switch in the lift")
        good = ("switch trim PS3X_TRIM 0 0=1 1=0\n"
                "switch konst PS3X_K off off=0 a=1 b=2   # two alternatives\n"
                f"testimg 0x4 {brnz(4, 3):08X} {NOP:08X} trim   # brnz -> nop\n"
                f"testimg 0x8 {il(5, 7):08X} {il(5, 9):08X},{il(5, 11):08X} konst\n"
                f"otherimg 0x8 {il(5, 7):08X} {il(5, 9):08X} trim\n")
        r1, pat = lift(tmp, good, "good")
        check(r1.returncode == 0, "a good file lifts" + ("" if r1.returncode == 0 else ": " + r1.stderr[-300:]))
        bl, pl = base.splitlines(), pat.splitlines()
        extra = [x for x in pl if x not in bl]
        gone = [x for x in bl if x not in pl]
        check(len(gone) == 2 and any("goto loc_00000010" in g for g in gone) and any("spu_il(7)" in g for g in gone),
              "exactly the two listed instructions changed")
        check(any("if (g_spu_patch[0] == 1) { /* nop */; } else { if (ctx->gpr[4]._u32[0] != 0) goto loc_00000010; }" in x
                  for x in extra), "brnz -> nop: alternative 1 of slot 0, the original in the else")
        check(any("if (g_spu_patch[1] == 1) { ctx->gpr[5] = spu_il(9); } else if (g_spu_patch[1] == 2) "
                  "{ ctx->gpr[5] = spu_il(11); } else { ctx->gpr[5] = spu_il(7); }" in x for x in extra),
              "two alternatives of slot 1 in order")
        check(any('spu_patch_register(0, "trim", "PS3X_TRIM", "0", "0=1 1=0", 1);' in x for x in extra) and
              any('spu_patch_register(1, "konst", "PS3X_K", "off", "off=0 a=1 b=2", 1);' in x for x in extra) and
              any("__attribute__((constructor))" in x for x in extra), "the switches register from a constructor")
        r2, oth = lift(tmp, good.replace("testimg 0x", "zzimg 0x"), "other")
        check(r2.returncode == 0 and oth == base, "lines for other images only: the old lift, byte for byte")
        bad = {
            "wrong original": f"switch t E 0 0=0 1=1\ntestimg 0x8 {il(5, 8):08X} {NOP:08X} t\n",
            "a branch as alternative": f"switch t E 0 0=0 1=1\ntestimg 0x8 {il(5, 7):08X} {brnz(4, 2):08X} t\n",
            "an alternative writing r0": f"switch t E 0 0=0 1=1\ntestimg 0x8 {il(5, 7):08X} {il(0, 1):08X} t\n",
            "an unconditional original": f"switch t E 0 0=0 1=1\ntestimg 0xC {bi(0):08X} {NOP:08X} t\n",
            "an unknown switch": f"testimg 0x8 {il(5, 7):08X} {NOP:08X} nosuch\n",
            "too few alternatives": f"switch t E 0 0=0 1=2\ntestimg 0x8 {il(5, 7):08X} {NOP:08X} t\n",
            "a default without alternative": f"switch t E 5 0=0 1=1\ntestimg 0x8 {il(5, 7):08X} {NOP:08X} t\n",
            "an address outside the lift": f"switch t E 0 0=0 1=1\ntestimg 0x40 {il(5, 7):08X} {NOP:08X} t\n",
        }
        for i, (what, text) in enumerate(bad.items()):
            r, _ = lift(tmp, text, f"bad{i}")
            check(r.returncode != 0 and "SPU_PATCH_FILE" in (r.stderr + r.stdout), f"{what}: the lift stops")
    print("PASS" if not fails else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
