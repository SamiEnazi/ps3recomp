#!/usr/bin/env python3
"""Summarise SPU job recordings (runtime/spu/spu_jobrec.c, PS3_JOBREC=<dir>).

    python -I tools/jobrec_report.py <dir> [--names jobmods_register.c] [--minutes M]
                                           [--dump <file.jrec>]

Per job module (resident job overlay id): recordings, recorded host time per job, the DMA
the job moves (GET/PUT count and bytes, distinct EA regions), local store lines it changes,
interrupts taken inside it, atomics, stops, and the per-stream operation mix. With
<dir>/jobstat.tsv (written by the same run) it adds calls and host time per module and the
calls per minute of play. --dump prints one recording's operation stream.
"""
import collections
import glob
import os
import re
import struct
import sys

HDR = struct.Struct("<8s4Ii4I3IQQ6IQQ8I")
EV = struct.Struct("<BBH6I")
EV_NAMES = {1: "rdch", 2: "rchcnt", 3: "wrch", 4: "dma", 5: "atomic", 6: "stop", 7: "irq"}
CH_NAMES = {0: "RdEventStat", 1: "WrEventMask", 2: "WrEventAck", 3: "RdSigNotify1", 4: "RdSigNotify2",
            7: "WrDec", 8: "RdDec", 9: "WrMSSyncReq", 11: "RdEventMask", 12: "RdTagMask", 13: "RdMachStat",
            14: "WrSRR0", 15: "RdSRR0", 16: "MFC_LSA", 17: "MFC_EAH", 18: "MFC_EAL", 19: "MFC_Size",
            20: "MFC_TagID", 21: "MFC_Cmd", 22: "WrTagMask", 23: "WrTagUpdate", 24: "RdTagStat",
            25: "RdListStallStat", 26: "WrListStallAck", 27: "RdAtomicStat", 28: "WrOutMbox",
            29: "RdInMbox", 30: "WrOutIntrMbox"}
LS = 256 * 1024


def load(path):
    b = open(path, "rb").read()
    h = HDR.unpack_from(b, 0)
    (magic, ver, hdr_size, state_size, ev_size, ovl, entry, ret, seq, spu, n_ev, data_len, n_lines,
     t_ms, dur_ns, n_irq, n_irq_lr, n_get, n_put, n_atom, n_stop, get_b, put_b) = h[:23]
    assert magic == b"PS3JREC1", path
    o = hdr_size + state_size + 128 * 16 + LS
    evs = [EV.unpack_from(b, o + i * ev_size) for i in range(n_ev)]
    return dict(path=path, ovl=ovl, entry=entry, seq=seq, spu=spu, n_ev=n_ev, data_len=data_len,
                n_lines=n_lines, t_ms=t_ms, dur_us=dur_ns / 1e3, n_irq=n_irq, n_irq_lr=n_irq_lr,
                n_get=n_get, n_put=n_put, n_atom=n_atom, n_stop=n_stop, get_b=get_b, put_b=put_b,
                ev=evs, size=len(b))


def names_from(register_c):
    out = {}
    if register_c and os.path.isfile(register_c):
        for m in re.finditer(r"spu_begin_image\((\d+)\); (\w+?)_spu_recomp_register", open(register_c).read()):
            out[int(m.group(1))] = m.group(2)
    return out


def dump(path):
    r = load(path)
    print(f"{os.path.basename(path)}: ovl {r['ovl']} entry 0x{r['entry']:05X} spu {r['spu']:X} t={r['t_ms']/1000:.1f}s "
          f"{r['dur_us']:.1f} us, {r['n_ev']} ops, {r['n_lines']} LS lines changed, irq {r['n_irq']}")
    for i, (t, s, ch, a, b, c, d, ln, off) in enumerate(r["ev"]):
        n = EV_NAMES.get(t, "?")
        if t in (1, 2, 3):
            print(f"  {i:5d} s{s} {n:6s} {CH_NAMES.get(ch, ch)} 0x{a:X}" + (f" (events 0x{b:X})" if ch == 0 else ""))
        elif t == 4:
            print(f"  {i:5d} s{s} {n:6s} cmd 0x{ch:02X} lsa 0x{a:05X} ea 0x{b:08X} size 0x{c:X} rc {d} data {ln}")
        elif t == 5:
            print(f"  {i:5d} s{s} {n:6s} cmd 0x{ch:02X} lsa 0x{a:05X} ea 0x{b:08X} stat {c}")
        elif t == 6:
            print(f"  {i:5d} s{s} {n:6s} code 0x{ch:X} status 0x{a:X}")
        else:
            print(f"  {i:5d} s{s} {n:6s} pc 0x{a:05X} pending 0x{b:X}")


def main():
    a = sys.argv[1:]
    if "--dump" in a:
        dump(a[a.index("--dump") + 1])
        return
    d = a[0]
    names = names_from(a[a.index("--names") + 1] if "--names" in a else "")
    minutes = float(a[a.index("--minutes") + 1]) if "--minutes" in a else None
    recs = [load(p) for p in sorted(glob.glob(os.path.join(d, "*.jrec")))]
    by = collections.defaultdict(list)
    for r in recs:
        by[r["ovl"]].append(r)
    stat = {}
    sp = os.path.join(d, "jobstat.tsv")
    if os.path.isfile(sp):
        for ln in open(sp):
            p = ln.rstrip("\n").split("\t")
            if p[0] == "t_s":
                stat["t_s"] = float(p[1])
            elif p[0].isdigit():
                stat[int(p[0])] = dict(calls=int(p[1]), ms=float(p[2]), share=float(p[3]), avg=float(p[4]))
    tot_files = sum(r["size"] for r in recs)
    print(f"{len(recs)} recordings in {d} ({tot_files/2**20:.1f} MB)")
    if stat:
        print(f"jobstat.tsv: {stat.get('t_s', 0):.0f} s of run")
    print(f"{'ovl':>4} {'module':28s} {'calls':>8} {'host%':>6} {'avg_us':>7} {'rec':>4} {'rec_us':>8} "
          f"{'GET':>5} {'GETkB':>7} {'PUT':>5} {'PUTkB':>7} {'LSkB':>6} {'irq':>5} {'atom':>4} {'stop':>4}  op mix (stream 0 / 1)")
    order = sorted(set(by) | {k for k in stat if isinstance(k, int)},
                   key=lambda o: -(stat.get(o, {}).get("ms", 0) or sum(r["dur_us"] for r in by.get(o, []))))
    for o in order:
        rs = by.get(o, [])
        st = stat.get(o, {})
        n = len(rs) or 1
        mix = [collections.Counter(), collections.Counter()]
        for r in rs:
            for e in r["ev"]:
                if e[1] < 2:
                    mix[e[1]][EV_NAMES.get(e[0], "?")] += 1
        def avg(k):
            return sum(r[k] for r in rs) / n
        m0 = " ".join(f"{k}{v//n}" for k, v in sorted(mix[0].items()))
        m1 = " ".join(f"{k}{v//n}" for k, v in sorted(mix[1].items()))
        print(f"{o:4d} {names.get(o, '?'):28s} {st.get('calls', 0):8d} {st.get('share', 0):6.1f} {st.get('avg', 0):7.1f} "
              f"{len(rs):4d} {avg('dur_us'):8.1f} {avg('n_get'):5.0f} {avg('get_b')/1024:7.1f} {avg('n_put'):5.0f} "
              f"{avg('put_b')/1024:7.1f} {avg('n_lines')*16/1024:6.1f} {avg('n_irq'):5.1f} {avg('n_atom'):4.1f} "
              f"{avg('n_stop'):4.1f}  {m0} / {m1}")
        if minutes and st:
            print(f"      {st['calls']/minutes:.0f} calls/min of play, {len(rs)/minutes:.1f} recorded/min")


if __name__ == "__main__":
    main()
