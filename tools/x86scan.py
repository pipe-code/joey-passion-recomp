"""Recursive-descent scan of Joey_Pc.exe to measure static-recompilation feasibility.

Usage: python -I tools/x86scan.py game/Joey_Pc.exe [out.json]
"""
import sys, json, struct, collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
from capstone import x86 as X

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
T0 = base + text.VirtualAddress
T1 = T0 + text.Misc_VirtualSize
in_text = lambda a: T0 <= a < T1
def rd32(a): return struct.unpack_from("<I", img, a - base)[0]

imports = {}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports:
        imports[i.address] = f"{e.dll.decode()}!{i.name.decode() if i.name else i.ordinal}"

md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True

insn_at = {}          # addr -> (size, mnemonic, op_str)
func_starts = set()
jumptables = {}       # jmp addr -> list of targets
indirect_jmp_unres = []
indirect_calls = 0
import_calls = collections.Counter()

def decode(a):
    off = a - base
    for ins in md.disasm(img[off:off + 16], a, 1):
        return ins
    return None

def try_jumptable(ins):
    # jmp dword ptr [reg*4 + table]
    if len(ins.operands) != 1: return None
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0 or not in_text(op.mem.disp) and not (base <= op.mem.disp < base + len(img)):
        return None
    tbl, out = op.mem.disp, []
    while len(out) < 1024:
        try: t = rd32(tbl + 4 * len(out))
        except struct.error: break
        if not in_text(t): break
        if out and (tbl + 4 * len(out)) in insn_at: break
        out.append(t)
    return out or None

def walk(start, work_funcs):
    stack = [start]
    while stack:
        a = stack.pop()
        while in_text(a) and a not in insn_at:
            ins = decode(a)
            if ins is None:
                break
            insn_at[a] = (ins.size, ins.mnemonic, ins.op_str)
            nxt = a + ins.size
            g = ins.groups
            if CS_GRP_CALL in g:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    if in_text(op.imm): work_funcs.append(op.imm)
                elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp in imports:
                    import_calls[imports[op.mem.disp]] += 1
                    if imports[op.mem.disp].endswith(("ExitProcess", "ExitThread", "_exit")): break
                else:
                    global indirect_calls; indirect_calls += 1
            elif CS_GRP_RET in g or ins.mnemonic in ("hlt", "int3", "ud2"):
                break
            elif CS_GRP_JUMP in g:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    stack.append(op.imm)
                    if ins.mnemonic == "jmp": break
                else:
                    if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp in imports:
                        import_calls[imports[op.mem.disp]] += 1  # thunk
                        break
                    tg = try_jumptable(ins)
                    if tg:
                        jumptables[a] = tg; stack.extend(tg)
                    else:
                        indirect_jmp_unres.append((a, ins.op_str))
                    break
            a = nxt

# Seeds: entry point + pointers into .text found in data sections (vtables, callbacks, func ptr tables)
seeds = [base + pe.OPTIONAL_HEADER.AddressOfEntryPoint]
ptr_seeds = set()
for s in pe.sections:
    if s.Name.startswith(b".text"): continue
    lo, hi = s.VirtualAddress, s.VirtualAddress + min(s.SizeOfRawData, s.Misc_VirtualSize)
    for off in range(lo, hi - 3, 4):
        v = struct.unpack_from("<I", img, off)[0]
        if in_text(v): ptr_seeds.add(v)
# push imm32 / mov reg, imm32 of text addresses (callbacks e.g. WndProc, CreateThread)
code_ptr_imm = set()

work = list(seeds) + sorted(ptr_seeds)
while work:
    f = work.pop()
    if f in func_starts: continue
    func_starts.add(f)
    walk(f, work)
    # scan new instructions for immediate code pointers
for a, (sz, mn, ops) in list(insn_at.items()):
    pass

# second pass: immediates referencing .text (callbacks) that weren't reached
for a in list(insn_at):
    ins = decode(a)
    for op in ins.operands:
        if op.type == X.X86_OP_IMM and in_text(op.imm) and ins.mnemonic in ("push", "mov") and op.imm not in insn_at:
            code_ptr_imm.add(op.imm)
for f in code_ptr_imm:
    work.append(f)
while work:
    f = work.pop()
    if f in func_starts: continue
    func_starts.add(f); walk(f, work)

covered = sum(v[0] for v in insn_at.values())
mn = collections.Counter(v[1] for v in insn_at.values())
regs_used = collections.Counter()
for v in insn_at.values():
    for r in ("mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7", "xmm"):
        if r in v[2]: regs_used[r[:3] if r != "xmm" else "xmm"] += 1
x87 = sum(c for m, c in mn.items() if m.startswith("f") and m not in ("fwait",))
mmx = regs_used["mm0"] + sum(regs_used[k] for k in regs_used if k.startswith("mm"))
sse = regs_used["xmm"]
weird = {m: mn[m] for m in ("in", "out", "cli", "sti", "int", "into", "lock", "cpuid", "rdtsc", "les", "lds", "pushal", "popal", "pushfd", "popfd", "sahf", "lahf", "enter", "leave", "rep movsd", "rep stosd", "xlatb", "aam", "aad", "daa", "das", "bound", "lsl", "sgdt") if mn.get(m)}
fs_access = sum(1 for v in insn_at.values() if "fs:" in v[2])
# code bytes inside .text not reached
gaps = text.Misc_VirtualSize - covered

report = {
    "text_size": text.Misc_VirtualSize,
    "covered_bytes": covered, "coverage_pct": round(100 * covered / text.Misc_VirtualSize, 1),
    "functions": len(func_starts), "instructions": len(insn_at),
    "data_ptr_seeds": len(ptr_seeds), "imm_code_ptrs": len(code_ptr_imm),
    "jumptables": len(jumptables), "jumptable_targets": sum(len(v) for v in jumptables.values()),
    "indirect_calls": indirect_calls, "unresolved_indirect_jmps": len(indirect_jmp_unres),
    "unresolved_samples": [(hex(a), o) for a, o in indirect_jmp_unres[:15]],
    "x87_insns": x87, "mmx_insns": sum(v for k, v in regs_used.items() if k.startswith("mm")), "sse_insns": sse,
    "fs_segment_accesses": fs_access, "unusual": weird,
    "top_mnemonics": mn.most_common(45),
    "import_calls_top": import_calls.most_common(40),
}
json.dump(report, open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout, indent=1)
if len(sys.argv) > 2:
    json.dump({"functions": sorted(func_starts), "jumptables": {hex(k): v for k, v in jumptables.items()}}, open(sys.argv[2] + ".funcs.json", "w"))
