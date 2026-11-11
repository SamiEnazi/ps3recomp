"""(rnd1) Build and run test_rsx_draw_delta on Windows (no game, no GPU).

    python -I test_rsx_draw_delta_win.py [--seeds 1,2,3] [--base <toolkit dir>] [--out <dir>]

Builds the test with clang-cl against this toolkit's draw engine, runs every
seed three times -- PS3RECOMP_ENG_DELTA=0, =1, and =1 with
PS3RECOMP_ENG_DELTA_VERIFY=1 -- and checks that the three backend logs are
byte-identical and that verify mode reported no difference. (fastdraw) Three
more: =1 with PS3RECOMP_ENG_FASTDRAW=0 (the whole tail), with
PS3RECOMP_FASTDRAW2=0 (one pair-memo way) and with
PS3RECOMP_ENG_FASTDRAW_VERIFY=2 (no [fastdraw-verify] line allowed). --base also builds
the test against another toolkit tree (e.g. the commit before the delta
engine) and checks that its log equals the delta engine's. Needs the port's
tools/vsenv.py (found by walking up from this file, or PORT_ROOT).
Exit 0 = every check passed.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TK = os.path.normpath(os.path.join(HERE, "..", "..", ".."))


def find_root():
    r = os.environ.get("PORT_ROOT")
    if r: return r
    d = TK
    for _ in range(6):
        d = os.path.dirname(d)
        if os.path.exists(os.path.join(d, "tools", "vsenv.py")): return d
    sys.exit("tools/vsenv.py not found: set PORT_ROOT to the port project folder")


ROOT = find_root()
sys.path.insert(0, os.path.join(ROOT, "tools"))
from vsenv import vs_env  # noqa: E402
ENV = vs_env()
CLANG = r"C:\Program Files\LLVM\bin\clang-cl.exe"
LLD = r"C:\Program Files\LLVM\bin\lld-link.exe"
INC = ["include", r"libs\system", r"libs\filesystem", r"libs\input", r"libs\audio", r"libs\video", r"libs\network",
       r"libs\spurs", r"libs\sync", r"libs\codec", r"libs\font", r"libs\misc", r"runtime\spu", r"runtime\platform\win32_include"]
ENGINE = [r"libs\video\rsx_draw_engine.c", r"libs\video\rsx_dispatch.c", r"libs\video\rsx_vertex_compact.c",
          r"libs\video\rsx_vertex_fetch.c", r"libs\video\rsx_texture_layout.c", r"libs\video\rsx_fp_decompiler.c",
          r"libs\video\rsx_vp_decompiler.c", r"libs\video\rsx_button_icons.c", r"libs\video\rsx_wtrack.c",
          r"libs\video\rsx_split.c", r"runtime\platform\win32_posix_min.c", r"runtime\platform\win32_compat.c"]


def build(tk, out_dir, name):
    obj = os.path.join(out_dir, name + "_obj")
    os.makedirs(obj, exist_ok=True)
    flags = ["/nologo", "-DPS3RECOMP_RSX_VULKAN"] + ["-I" + os.path.join(tk, i) for i in INC] + [
        "/DWIN32", "/D_WINDOWS", "/O2", "/DNDEBUG", "/std:c17", "-MD", "/Z7", "-w", "-fno-strict-aliasing",
        "/clang:-ffp-contract=off", "-msse4.1", "/FI" + os.path.join(tk, r"runtime\platform\win32_posix_min.h")]
    srcs = [os.path.join(HERE, "test_rsx_draw_delta.c"), os.path.join(HERE, "delta_test_stubs.c")] + \
           [os.path.join(tk, s) for s in ENGINE]
    objs = []
    for s in srcs:
        o = os.path.join(obj, os.path.basename(s) + ".obj")
        p = subprocess.run([CLANG] + flags + ["/Fo" + o, "-c", "--", s], env=ENV, capture_output=True, text=True)
        if p.returncode:
            sys.exit("compile failed: %s\n%s" % (s, (p.stdout + p.stderr)[-3000:]))
        objs.append(o)
    exe = os.path.join(out_dir, name + ".exe")
    p = subprocess.run([LLD, "/nologo", "/DEBUG", "/OUT:" + exe] + objs + ["kernel32.lib", "user32.lib", "synchronization.lib", "advapi32.lib"],
                       env=ENV, capture_output=True, text=True)
    if p.returncode:
        sys.exit("link failed:\n" + (p.stdout + p.stderr)[-3000:])
    return exe


def run(exe, log, seed, extra):
    env = dict(os.environ)
    for k in ("PS3RECOMP_ENG_DELTA", "PS3RECOMP_ENG_DELTA_VERIFY", "PS3RECOMP_ENG_FASTDRAW", "PS3RECOMP_FASTDRAW2",
              "PS3RECOMP_ENG_FASTDRAW_VERIFY"): env.pop(k, None)
    env.update(extra)
    env["PS3RECOMP_ENG_DELTA_STATS"] = "1"
    p = subprocess.run([exe, log, str(seed)], env=env, capture_output=True, text=True, timeout=900)
    if p.returncode:
        sys.exit("%s seed %s exited %d:\n%s" % (exe, seed, p.returncode, (p.stdout + p.stderr)[-2000:]))
    with open(log, "rb") as f:
        return f.read(), p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", default="1,2,3,4,5,6")
    ap.add_argument("--base", default="")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "rsx_delta_test"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    exe = build(TK, a.out, "test_rsx_draw_delta")
    base = build(a.base, a.out, "test_rsx_draw_delta_base") if a.base else None
    bad = 0
    for seed in [s for s in a.seeds.split(",") if s]:
        off, _ = run(exe, os.path.join(a.out, "off_%s.log" % seed), seed, {"PS3RECOMP_ENG_DELTA": "0"})
        on, err_on = run(exe, os.path.join(a.out, "on_%s.log" % seed), seed, {"PS3RECOMP_ENG_DELTA": "1"})
        ver, err_v = run(exe, os.path.join(a.out, "verify_%s.log" % seed), seed,
                         {"PS3RECOMP_ENG_DELTA": "1", "PS3RECOMP_ENG_DELTA_VERIFY": "1"})
        diffs = [l for l in err_v.splitlines() if l.startswith("[delta-verify]")]
        stats = [l for l in err_on.splitlines() if l.startswith("[delta]")]
        # (fastdraw) the short tail off, round 2 off, the fast path verified
        f0, _ = run(exe, os.path.join(a.out, "fast0_%s.log" % seed), seed, {"PS3RECOMP_ENG_DELTA": "1", "PS3RECOMP_ENG_FASTDRAW": "0"})
        f2, _ = run(exe, os.path.join(a.out, "fast2off_%s.log" % seed), seed, {"PS3RECOMP_ENG_DELTA": "1", "PS3RECOMP_FASTDRAW2": "0"})
        fv, err_fv = run(exe, os.path.join(a.out, "fastverify_%s.log" % seed), seed,
                         {"PS3RECOMP_ENG_DELTA": "1", "PS3RECOMP_ENG_FASTDRAW_VERIFY": "2"})
        fdiffs = [l for l in err_fv.splitlines() if l.startswith("[fastdraw-verify]")]
        fstats = [l for l in err_on.splitlines() if l.startswith("[fastdraw]")]
        diffs += fdiffs
        ok = on == off and ver == off and f0 == off and f2 == off and fv == off and not diffs
        if base:
            b, _ = run(base, os.path.join(a.out, "base_%s.log" % seed), seed, {})
            ok = ok and b == on
        print("seed %s: %s (%d backend calls)%s" % (seed, "PASS" if ok else "FAIL", off.count(b"\n"),
                                                     ("\n  " + stats[-1]) if stats else ""))
        for l in diffs[:8]: print("  " + l)
        if fstats: print("  " + fstats[-1][:300])
        if on != off: print("  delta on differs from off")
        if ver != off: print("  verify differs from off")
        if f0 != off: print("  PS3RECOMP_ENG_FASTDRAW=0 differs from off")
        if f2 != off: print("  PS3RECOMP_FASTDRAW2=0 differs from off")
        if fv != off: print("  PS3RECOMP_ENG_FASTDRAW_VERIFY=2 differs from off")
        if base and b != on: print("  the base toolkit's log differs from the delta engine's")
        bad += not ok
    print("all seeds passed" if not bad else "%d seed(s) FAILED" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
