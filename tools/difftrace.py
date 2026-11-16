"""Locate the first diverging instruction for difftest failures using per-instruction traces.

Usage: python -I tools/difftrace.py work/difftest/all.json OUT.json [--max N] [--status mismatch,fatal]
Needs build/trace/libdifftest.dylib (make lift-trace).
"""
import sys, os, json, argparse, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import difftest as D
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ap = argparse.ArgumentParser()
ap.add_argument("results"); ap.add_argument("out")
ap.add_argument("--max", type=int, default=100000)
ap.add_argument("--status", default="mismatch,fatal")
ap.add_argument("--funcs")
a = ap.parse_args()

want = set(a.status.split(","))
items = [r for r in json.load(open(a.results)) if r["status"] in want]
if a.funcs:
    fs = {int(x, 16) for x in a.funcs.split(",")}
    items = [r for r in items if r["f"] in fs]
items = items[:a.max]

H = D.Harness(os.path.join(D.ROOT, "build", "trace"))
md = Cs(CS_ARCH_X86, CS_MODE_32)
def dis(addr):
    off = addr - H.base
    i = next(md.disasm(H.img[off:off + 16], addr), None)
    return f"{i.mnemonic} {i.op_str}" if i else "?"

out = []
for r in items:
    f, k = r["f"], r["k"]
    _, _, _, rt = H.run_ref(f, k, trace=True)
    _, _, lt = H.run_lifted(f, k, trace=True)
    rt = [t for t in rt if t[0] not in H.hle]
    n = min(len(rt), len(lt))
    i = next((j for j in range(n) if rt[j] != lt[j]), n)
    rec = {"f": f, "k": k, "status": r["status"], "ref_len": len(rt), "lifted_len": len(lt), "at": i}
    if i == n and len(rt) == len(lt):
        rec["culprit"] = None   # traces identical: divergence is in memory only (or after last instruction)
        if lt:
            last = lt[-1][0]; rec["culprit"] = last; rec["insn"] = dis(last); rec["kind"] = "memory-only"
    elif i == 0:
        rec["culprit"] = f; rec["insn"] = dis(f); rec["kind"] = "entry"
    else:
        c = rt[i - 1][0]
        rec.update(culprit=c, insn=dis(c), kind="regs" if i < n and rt[i][0] == lt[i][0] else "flow")
        if i < n:
            rec["ref_next"] = [hex(rt[i][0])] + [f"{x:#x}" for x in rt[i][1]]
            rec["lif_next"] = [hex(lt[i][0])] + [f"{x:#x}" for x in lt[i][1]]
        else:
            rec["note"] = "one trace ended"
    out.append(rec)

json.dump(out, open(a.out, "w"), indent=0)
by_m = collections.Counter((o.get("insn") or "?").split()[0] + " / " + o.get("kind", "?") for o in out)
print("cases:", len(out))
for m, c in by_m.most_common(30):
    print(f"{c:5d}  {m}")
