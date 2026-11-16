"""Function discovery v2 for Joey_Pc.exe with seed confidence levels.

Usage: python -I tools/lifter/funcs.py game/Joey_Pc.exe work/exe/funcs.json

Levels: 1) entry + direct call targets (trusted)
        2) code immediates (push/mov imm32 pointing into .text: callbacks)
        3) data pointers (dwords in .rdata/.data pointing into .text: vtables, tables)
A level-2/3 seed is rejected if it lies inside an accepted function's body, overlaps bytes
of accepted instructions at a different boundary, or decodes to junk.
"""
import sys, json, struct, collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
from capstone import x86 as X

JUNK = {"in", "out", "cli", "sti", "into", "retf", "lcall", "ljmp", "les", "lds", "hlt", "insb", "insd",
        "outsb", "outsd", "arpl", "bound", "iretd", "int1", "salc", "aaa", "aas", "daa", "das", "aam", "aad",
        "int", "sldt", "str", "lgdt", "sgdt", "lidt", "sidt"}
FULL = {**{r: r for r in ("eax","ecx","edx","ebx","esp","ebp","esi","edi")},
        **{k: "e" + k[0] + "x" for k in ("al","ah","ax","cl","ch","cx","dl","dh","dx","bl","bh","bx")},
        "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}
STRING_OPS = ("stos", "movs", "scas", "cmps", "lods")
NORETURN_IMPORTS = ("ExitProcess", "ExitThread", "_exit", "exit", "RaiseException")

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
img = bytes(pe.get_memory_mapped_image())
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
T0, T1 = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
in_text = lambda a: T0 <= a < T1
rd32 = lambda a: struct.unpack_from("<I", img, a - base)[0]
imports = {i.address: (i.name or b"").decode() for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports}

md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
_cache = {}
def insn(a):
    if a not in _cache:
        r = list(md.disasm(img[a - base:a - base + 16], a, 1))
        _cache[a] = r[0] if r else None
    return _cache[a]

def is_junk(ins):
    if ins.mnemonic.split()[0] in JUNK or ins.mnemonic in JUNK:
        return True
    for op in ins.operands:
        if op.type == X.X86_OP_REG and ins.reg_name(op.reg) in ("es", "ss", "cs", "ds"):
            return True
        if op.type == X.X86_OP_MEM and op.mem.segment and ins.reg_name(op.mem.segment) in ("es", "ss", "cs", "ds") \
                and not any(s in ins.mnemonic for s in STRING_OPS):
            return True
    return False

def jumptable(ins, last_cmp):
    if len(ins.operands) != 1: return None
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0 or not op.mem.index: return None
    tbl = op.mem.disp & 0xffffffff
    if not (base <= tbl < base + len(img)): return None
    idx = FULL.get(ins.reg_name(op.mem.index))
    n = last_cmp.get(idx)
    limit = n + 1 if n is not None and n < 1024 else 256
    out = []
    for k in range(limit):
        try: t = rd32(tbl + 4 * k)
        except struct.error: break
        if not in_text(t):
            if n is None: break
            return None  # bounded table with a non-code entry: not a jump table
        out.append(t)
    return (tbl, out) if out else None

def walk(f):
    """Body of f (not following calls). Returns dict with addrs, labels, calls, imms, junk, jumptables, tables."""
    addrs, calls, imms, jts, tables, labels = set(), set(), set(), {}, [], set()
    junk = False
    stack = [(f, {})]
    while stack:
        a, last_cmp = stack.pop()
        last_cmp = dict(last_cmp)
        while True:
            if a in addrs: break
            if not in_text(a): junk = True; break
            ins = insn(a)
            if ins is None: junk = True; break
            addrs.add(a)
            if is_junk(ins): junk = True
            m = ins.mnemonic
            ops = ins.operands
            # track switch bounds: cmp reg, N (invalidate on writes; follow byte-index tables)
            if ops and ops[0].type == X.X86_OP_REG and m not in ("cmp", "test", "push") and not m.startswith("j"):
                full = FULL.get(ins.reg_name(ops[0].reg))
                load = None
                if m in ("mov", "movzx") and len(ops) == 2 and ops[1].type == X.X86_OP_MEM and ops[1].size == 1 \
                        and ops[1].mem.base and not ops[1].mem.index:
                    b = FULL.get(ins.reg_name(ops[1].mem.base))
                    if b in last_cmp and 0 <= last_cmp[b] < 4096:
                        t2 = ops[1].mem.disp & 0xffffffff
                        if base <= t2 < base + len(img):
                            load = max(img[t2 - base:t2 - base + last_cmp[b] + 1])
                if load is not None:
                    last_cmp[full] = load
                else:
                    last_cmp.pop(full, None)
            if m == "cmp" and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_IMM:
                last_cmp[FULL.get(ins.reg_name(ops[0].reg))] = ops[1].imm
            for op in ops:
                if op.type == X.X86_OP_IMM and m in ("push", "mov") and in_text(op.imm & 0xffffffff):
                    imms.add(op.imm & 0xffffffff)
            g = ins.groups
            if CS_GRP_RET in g or m in ("int3", "ud2"):
                break
            if CS_GRP_CALL in g:
                op = ops[0]
                if op.type == X.X86_OP_IMM:
                    if in_text(op.imm): calls.add(op.imm)
                    else: junk = True
                elif op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index and \
                        imports.get(op.mem.disp, "") in NORETURN_IMPORTS:
                    break
            elif CS_GRP_JUMP in g:
                op = ops[0]
                if op.type == X.X86_OP_IMM:
                    if not in_text(op.imm): junk = True; break
                    labels.add(op.imm)
                    if m == "jmp":
                        a = op.imm; continue
                    stack.append((op.imm, last_cmp))
                else:
                    jt = jumptable(ins, last_cmp)
                    if jt:
                        jts[a] = jt[1]; tables.append((jt[0], 4 * len(jt[1])))
                        stack.extend((t, {}) for t in jt[1])
                    break
            a += ins.size
    return dict(addrs=addrs, labels=labels, calls=calls, imms=imms, junk=junk, jts=jts, tables=tables)

# ------------------------------------------------------------------ discovery
funcs, bodies = {}, {}
owner_byte = {}            # byte address -> instruction start owning it
insn_starts = set()
label_starts = set()       # branch targets inside accepted bodies
jt_targets = set()         # switch-table case labels (never function starts)
table_bytes = set()

def accept(f, w):
    funcs[f] = w
    for a in w["addrs"]:
        ins = insn(a); insn_starts.add(a)
        for b in range(a, a + ins.size):
            owner_byte.setdefault(b, a)
    for t, ln in w["tables"]:
        table_bytes.update(range(t, t + ln))
    label_starts.update(w["labels"])
    for ts in w["jts"].values():
        jt_targets.update(ts)

def conflicts(f, w, level=9):
    # interior instruction of an accepted function; a code-immediate seed (level 2) may sit on a
    # branch target, since tail jumps (e.g. EH funclets -> destructor) reach real functions.
    # Seeds from code immediates (2) or vtable-like runs (3) may sit on a branch target: tail jumps
    # (EH funclets -> destructor, constructor -> init method) reach real functions. Switch cases may not.
    if f in insn_starts and f not in funcs and not (level in (2, 3) and f in label_starts and f not in jt_targets):
        return "interior"
    for a in w["addrs"]:
        if a in table_bytes: return "in-table"
        if a in insn_starts:
            continue
        o = owner_byte.get(a)
        if o is not None and o != a: return "overlap"
        ins = insn(a)
        for b in range(a + 1, a + ins.size):
            if b in insn_starts: return "overlap"
    return None

def run_trusted(work):
    while work:
        f = work.pop()
        if f in funcs: continue
        w = walk(f)
        accept(f, w)
        work.extend(w["calls"] - funcs.keys())

entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
run_trusted([entry])
stats = collections.Counter()

def try_seeds(seeds, level):
    changed = True
    while changed:
        changed = False
        for s in sorted(seeds):
            if s in funcs: continue
            w = walk(s)
            why = "junk" if w["junk"] else conflicts(s, w, level)
            if why:
                stats[f"L{level}-reject-{why}"] += 1
                continue
            stats[f"L{level}-accept"] += 1
            accept(s, w)
            run_trusted(list(w["calls"] - funcs.keys()))
            changed = True
        seeds = {s for s in seeds if s not in funcs}
        if changed:
            stats.clear() if False else None

# level 2: code immediates (repeat until fixpoint since new code adds immediates)
while True:
    imms = set().union(*(w["imms"] for w in funcs.values())) - funcs.keys()
    before = len(funcs)
    try_seeds(imms, 2)
    if len(funcs) == before: break

# level 3: data pointers. Prefer runs of >=2 consecutive code pointers (vtables/fn tables).
data_ptrs = collections.Counter()
for s in pe.sections:
    if s.Name.startswith(b".text"): continue
    lo, hi = base + s.VirtualAddress, base + s.VirtualAddress + min(s.SizeOfRawData, s.Misc_VirtualSize)
    run = []
    for a in range(lo, hi - 3, 4):
        v = rd32(a)
        if in_text(v): run.append(v)
        else:
            for v2 in run: data_ptrs[v2] = max(data_ptrs[v2], len(run))
            run = []
strong3 = {v for v, r in data_ptrs.items() if r >= 2}
weak3 = set(data_ptrs) - strong3
try_seeds(strong3, 3)
try_seeds(weak3, 4)
while True:  # new code may expose more immediates
    imms = set().union(*(w["imms"] for w in funcs.values())) - funcs.keys()
    before = len(funcs); try_seeds(imms, 2)
    if len(funcs) == before: break

missing_vt = sorted(v for v in strong3 if v not in funcs)
print("vtable-run entries not accepted as functions:", len(missing_vt), [hex(v) for v in missing_vt[:12]])
covered = sum(insn(a).size for a in insn_starts)
jts = {}
for w in funcs.values(): jts.update(w["jts"])
junk_funcs = [hex(f) for f, w in funcs.items() if w["junk"]]
print(f"functions {len(funcs)} | instructions {len(insn_starts)} | coverage {100*covered/(T1-T0):.1f}% | jumptables {len(jts)}")
print("seeds:", dict(sorted(stats.items())))
print("trusted functions containing junk:", junk_funcs[:20], len(junk_funcs))
json.dump({"functions": sorted(funcs), "jumptables": {hex(k): v for k, v in jts.items()},
           "junk": junk_funcs}, open(sys.argv[2], "w"))
