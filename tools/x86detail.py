"""Second-level stats on top of x86scan output: per-function MMX use, odd insns, distinct mnemonics."""
import sys, json, struct, bisect, collections, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe=pefile.PE(sys.argv[1]); base=pe.OPTIONAL_HEADER.ImageBase; img=pe.get_memory_mapped_image()
F=json.load(open(sys.argv[2]))["functions"]; F.sort()
md=Cs(CS_ARCH_X86,CS_MODE_32)
text=next(s for s in pe.sections if s.Name.startswith(b".text")); T1=base+text.VirtualAddress+text.Misc_VirtualSize
mn=collections.Counter(); mmxf=collections.Counter(); odd=[]; x87f=collections.Counter()
for i,f in enumerate(F):
    end=F[i+1] if i+1<len(F) else T1
    for ins in md.disasm(img[f-base:end-base], f):
        mn[ins.mnemonic]+=1
        if "mm" in ins.op_str and "xmm" not in ins.op_str: mmxf[f]+=1
        if ins.mnemonic.startswith("f") and ins.mnemonic!="fwait": x87f[f]+=1
        if ins.mnemonic in ("in","out","cli","sti","into","les","lds","cpuid","pushal","popal","sahf"): odd.append((hex(ins.address),hex(f),ins.mnemonic,ins.op_str))
        if ins.mnemonic in ("ret","jmp") and ins.address+ins.size>=end: break
print("distinct mnemonics", len(mn))
print("MMX: functions", len(mmxf), "top", [(hex(a),c) for a,c in mmxf.most_common(12)])
print("x87: functions", len(x87f))
print("odd", odd[:25])
