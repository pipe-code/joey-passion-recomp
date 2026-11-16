"""Find suspicious (likely data-misdecoded) functions in the scan output."""
import sys, os, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lift import Lifter, Unsupported, X, CS_GRP_JUMP, CS_GRP_CALL
L = Lifter(sys.argv[1], sys.argv[2])
JUNK = {"in","out","cli","sti","into","retf","lcall","ljmp","les","lds","hlt","insb","insd","outsb","outsd","arpl","bound","iretd","int1","salc","aaa","aas","daa","das","aam","aad"}
SEGS = {"es","ss","cs","ds"}
bad = {}
callers = collections.defaultdict(set)
for f in sorted(L.func_starts):
    addrs, labels = L.body(f)
    why = []
    for a in addrs:
        ins = L.insn(a)
        if ins.mnemonic in JUNK: why.append(ins.mnemonic)
        for op in ins.operands:
            if op.type == X.X86_OP_REG and ins.reg_name(op.reg) in SEGS: why.append("segreg")
            if op.type == X.X86_OP_MEM and op.mem.segment and ins.reg_name(op.mem.segment) in ("es","ss","cs","ds") and ins.mnemonic not in ("stosb","stosw","stosd","rep stosb","rep stosw","rep stosd","movsb","movsw","movsd","rep movsb","rep movsw","rep movsd","repne scasb","repe cmpsb","repe cmpsd","scasb","scasd","cmpsb","cmpsd","lodsb","lodsd"): why.append("segprefix")
        if CS_GRP_JUMP in ins.groups and ins.operands and ins.operands[0].type == X.X86_OP_IMM and not (L.t0 <= ins.operands[0].imm < L.t1): why.append("jmp-out")
        if CS_GRP_CALL in ins.groups and ins.operands[0].type == X.X86_OP_IMM:
            t = ins.operands[0].imm
            if not (L.t0 <= t < L.t1): why.append("call-out")
            else: callers[t].add(f)
    # ran off into undecodable bytes
    last = L.insn(addrs[-1]) if addrs else None
    if not addrs: why.append("empty")
    if why: bad[f] = sorted(set(why))
print("suspicious:", len(bad))
for f, w in list(bad.items())[:40]:
    print(hex(f), w, "called by", [hex(c) for c in list(callers[f])[:3]])
json.dump({hex(f): w for f, w in bad.items()}, open(sys.argv[3], "w"), indent=0)
