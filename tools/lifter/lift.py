"""x86 (32-bit, MSVC code) -> C static lifter for Joey_Pc.exe.

Usage:
  python -I tools/lifter/lift.py game/Joey_Pc.exe work/exe/scan.json.funcs.json OUT_DIR [addr ...|--all]

Writes OUT_DIR/lifted.c (one C function per guest function + registration table)
and OUT_DIR/image.bin (memory-mapped PE image, loaded at the image base).
Unsupported instructions are emitted as x86_fatal() calls and summarized on stdout.
"""
import sys, os, re, json, struct, collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
from capstone import x86 as X

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SUB = {  # name -> (full reg, shift, bits)
    **{r: (r, 0, 32) for r in REG32},
    "ax": ("eax", 0, 16), "cx": ("ecx", 0, 16), "dx": ("edx", 0, 16), "bx": ("ebx", 0, 16),
    "sp": ("esp", 0, 16), "bp": ("ebp", 0, 16), "si": ("esi", 0, 16), "di": ("edi", 0, 16),
    "al": ("eax", 0, 8), "cl": ("ecx", 0, 8), "dl": ("edx", 0, 8), "bl": ("ebx", 0, 8),
    "ah": ("eax", 8, 8), "ch": ("ecx", 8, 8), "dh": ("edx", 8, 8), "bh": ("ebx", 8, 8),
}
MASK = {8: "0xffu", 16: "0xffffu", 32: "0xffffffffu"}
CTYPE = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t"}
STYPE = {8: "int8_t", 16: "int16_t", 32: "int32_t"}

COND = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf|zf)", "a": "(!cf&&!zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf||sf!=of)", "g": "(!zf&&sf==of)",
}


MMX_OPS = {"movd", "movq", "emms", "pxor", "pand", "pandn", "por",
           "punpcklbw", "punpcklwd", "punpckldq", "punpckhbw", "punpckhwd", "punpckhdq",
           "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd",
           "pmullw", "pmulhw", "pmaddwd", "paddb", "paddw", "paddd", "paddusb", "paddusw", "paddsb", "paddsw",
           "psubb", "psubw", "psubd", "psubusb", "psubusw", "psubsb", "psubsw",
           "psrlw", "psrld", "psrlq", "psraw", "psrad", "psllw", "pslld", "psllq",
           "packuswb", "packsswb", "packssdw"}


class Unsupported(Exception):
    pass


def reg(name):
    if name not in SUB:
        raise Unsupported(f"reg:{name}")
    return SUB[name]


class Lifter:
    def __init__(self, exe, funcs_json):
        self.pe = pefile.PE(exe)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = bytes(self.pe.get_memory_mapped_image())
        t = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.t0, self.t1 = self.base + t.VirtualAddress, self.base + t.VirtualAddress + t.Misc_VirtualSize
        fj = json.load(open(funcs_json))
        self.func_starts = set(fj["functions"])
        self.jumptables = {int(k, 16): v for k, v in fj["jumptables"].items()}
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.cache = {}
        self.unsupported = collections.Counter()
        self.trace = os.environ.get("LIFT_TRACE") == "1"
        hle = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hle.txt")
        self.hle = set()
        if os.path.exists(hle):
            for line in open(hle):
                line = line.split("#")[0].strip()
                if line:
                    self.hle.add(int(line.split()[0], 16))

    def insn(self, a):
        if a not in self.cache:
            off = a - self.base
            r = list(self.md.disasm(self.img[off:off + 16], a, 1))
            self.cache[a] = r[0] if r else None
        return self.cache[a]

    # ---------------------------------------------------------------- CFG
    def body(self, f):
        """Instructions reachable from f without following calls. Returns sorted addrs, labels, tailcalls."""
        seen, labels, stack = set(), {f}, [f]
        while stack:
            a = stack.pop()
            while True:
                if a in seen:
                    break
                if a != f and a in self.func_starts and a not in labels:
                    break  # fell into another function: treat as tail call (handled at emit)
                ins = self.insn(a)
                if ins is None:
                    break
                seen.add(a)
                g = ins.groups
                if CS_GRP_RET in g or ins.mnemonic in ("hlt", "int3", "ud2"):
                    break
                if CS_GRP_JUMP in g:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm
                        if not (self.t0 <= t < self.t1):
                            if ins.mnemonic == "jmp":
                                break
                        elif not (t in self.func_starts and t != f):
                            labels.add(t); stack.append(t)
                        if ins.mnemonic == "jmp":
                            break
                    else:
                        for t in self.jumptables.get(a, []):
                            labels.add(t); stack.append(t)
                        break
                if CS_GRP_CALL in g and self.is_noreturn_call(ins):
                    break
                a += ins.size
        return sorted(seen), labels

    def is_noreturn_call(self, ins):
        return False

    # ---------------------------------------------------------------- operands
    def addr_expr(self, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(reg(self.cur.reg_name(m.base))[0])
        if m.index:
            r = reg(self.cur.reg_name(m.index))[0]
            parts.append(f"{r}*{m.scale}u" if m.scale != 1 else r)
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xffffffff:x}u")
        e = " + ".join(parts)
        if m.segment and self.cur.reg_name(m.segment) == "fs":
            e = f"ctx->fs_base + {e}"
        return f"(uint32_t)({e})"

    def rd(self, op, bits=None):
        bits = bits or op.size * 8
        if op.type == X.X86_OP_REG:
            full, sh, b = reg(self.cur.reg_name(op.reg))
            if b == 32:
                return full
            return f"(({full} >> {sh}) & {MASK[b]})" if sh else f"({full} & {MASK[b]})"
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & ((1 << bits) - 1):x}u"
        if op.type == X.X86_OP_MEM:
            return f"RD{op.size * 8}({self.addr_expr(op)})"
        raise Unsupported("operand")

    def wr(self, op, val):
        if op.type == X.X86_OP_REG:
            full, sh, b = reg(self.cur.reg_name(op.reg))
            if b == 32:
                return f"{full} = (uint32_t)({val});"
            m = (1 << b) - 1
            return f"{full} = ({full} & 0x{~(m << sh) & 0xffffffff:x}u) | ((uint32_t)(({val}) & 0x{m:x}u) << {sh});"
        if op.type == X.X86_OP_MEM:
            return f"WR{op.size * 8}({self.addr_expr(op)}, ({CTYPE[op.size * 8]})({val}));"
        raise Unsupported("write operand")

    # ---------------------------------------------------------------- flags
    @staticmethod
    def szp(r, bits):
        return f"zf = ({r}) == 0; sf = (({r}) >> {bits - 1}) & 1; pf = PARITY({r});"

    # ---------------------------------------------------------------- instruction semantics
    def lift_insn(self, ins, f):
        self.cur = ins
        mn, ops = ins.mnemonic, ins.operands
        nxt = ins.address + ins.size
        out = []
        o = out.append
        bits = ops[0].size * 8 if ops else 32
        M = MASK.get(bits)

        op0 = list(ins.bytes)
        while op0 and op0[0] in (0x66, 0xf2, 0xf3, 0x2e, 0x3e, 0x26, 0x64, 0x65, 0x36, 0x9b):
            op0.pop(0)
        if len(op0) >= 2 and 0xd8 <= op0[0] <= 0xdf:
            return self.lift_x87(ins, op0), True
        if mn in MMX_OPS:
            return self.lift_mmx(ins), True

        def binop_flags(kind):
            a, b = self.rd(ops[0]), self.rd(ops[1], bits)
            if ops[1].type == X.X86_OP_IMM and ops[1].size * 8 < bits:
                b = f"0x{ops[1].imm & ((1 << bits) - 1):x}u"
            o(f"{{ uint32_t a = {a}, b = {b};")
            if kind == "add":
                o(f"  uint32_t r = (a + b) & {M}; cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> {bits - 1}) & 1;")
            elif kind == "adc":
                o(f"  uint64_t w = (uint64_t)a + b + cf; uint32_t r = (uint32_t)w & {M}; cf = (w >> {bits}) & 1; of = ((~(a ^ b) & (a ^ r)) >> {bits - 1}) & 1;")
            elif kind in ("sub", "cmp"):
                o(f"  uint32_t r = (a - b) & {M}; cf = a < b; of = (((a ^ b) & (a ^ r)) >> {bits - 1}) & 1;")
            elif kind == "sbb":
                o(f"  uint32_t c = cf; uint32_t r = (a - b - c) & {M}; cf = (uint64_t)a < (uint64_t)b + c; of = (((a ^ b) & (a ^ r)) >> {bits - 1}) & 1;")
            elif kind in ("and", "test"):
                o(f"  uint32_t r = a & b; cf = 0; of = 0;")
            elif kind == "or":
                o(f"  uint32_t r = a | b; cf = 0; of = 0;")
            elif kind == "xor":
                o(f"  uint32_t r = a ^ b; cf = 0; of = 0;")
            o("  " + self.szp("r", bits))
            if kind not in ("cmp", "test"):
                o("  " + self.wr(ops[0], "r"))
            o("}")

        if mn in ("add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test"):
            if mn == "xor" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                o(self.wr(ops[0], "0") + " cf = 0; of = 0; zf = 1; sf = 0; pf = 1;")
            else:
                binop_flags(mn)
        elif mn == "mov":
            o(self.wr(ops[0], self.rd(ops[1], bits)))
        elif mn == "movzx":
            o(self.wr(ops[0], self.rd(ops[1])))
        elif mn == "movsx":
            sb = ops[1].size * 8
            o(self.wr(ops[0], f"(uint32_t)(int32_t)({STYPE[sb]})({self.rd(ops[1])})"))
        elif mn == "lea":
            o(self.wr(ops[0], self.addr_expr(ops[1])))
        elif mn in ("inc", "dec"):
            d = "+" if mn == "inc" else "-"
            o(f"{{ uint32_t a = {self.rd(ops[0])}; uint32_t r = (a {d} 1) & {M};")
            o(f"  of = r == {'0x%xu' % (1 << (bits - 1)) if mn == 'inc' else '0x%xu' % ((1 << (bits - 1)) - 1)}; {self.szp('r', bits)} {self.wr(ops[0], 'r')} }}")
        elif mn == "neg":
            o(f"{{ uint32_t a = {self.rd(ops[0])}; uint32_t r = (0u - a) & {M}; cf = a != 0; of = a == 0x{1 << (bits - 1):x}u; {self.szp('r', bits)} {self.wr(ops[0], 'r')} }}")
        elif mn == "not":
            o(self.wr(ops[0], f"~{self.rd(ops[0])}"))
        elif mn in ("shl", "sal", "shr", "sar", "rol", "ror"):
            cnt = self.rd(ops[1]) if len(ops) > 1 else "1u"
            o(f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {self.rd(ops[0])}, r;")
            if mn in ("shl", "sal"):
                o(f"  r = (n >= {bits}) ? 0 : (a << n) & {M}; cf = (n <= {bits}) ? (a >> ({bits} - n)) & 1 : 0; of = ((r >> {bits - 1}) & 1) ^ cf;")
            elif mn == "shr":
                o(f"  r = (n >= {bits}) ? 0 : a >> n; cf = (a >> (n - 1)) & 1; of = (a >> {bits - 1}) & 1;")
            elif mn == "sar":
                o(f"  int32_t s = (int32_t)({STYPE[bits]})a; r = (uint32_t)(s >> (n > {bits - 1} ? {bits - 1} : n)) & {M}; cf = (uint32_t)(s >> (n - 1 > {bits - 1} ? {bits - 1} : n - 1)) & 1; of = 0;")
            elif mn == "rol":
                o(f"  uint32_t k = n % {bits}; r = k ? ((a << k) | (a >> ({bits} - k))) & {M} : a; cf = r & 1; of = ((r >> {bits - 1}) & 1) ^ cf;")
            elif mn == "ror":
                o(f"  uint32_t k = n % {bits}; r = k ? ((a >> k) | (a << ({bits} - k))) & {M} : a; cf = (r >> {bits - 1}) & 1; of = ((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1;")
            if mn not in ("rol", "ror"):
                o("  " + self.szp("r", bits))
            o(f"  {self.wr(ops[0], 'r')} }} }}")
        elif mn in ("shld", "shrd"):
            cnt = self.rd(ops[2])
            a, b = self.rd(ops[0]), self.rd(ops[1])
            if mn == "shld":
                o(f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {a}, b = {b}; uint32_t r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1; {self.szp('r', 32)} {self.wr(ops[0], 'r')} }} }}")
            else:
                o(f"{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {a}, b = {b}; uint32_t r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1; {self.szp('r', 32)} {self.wr(ops[0], 'r')} }} }}")
        elif mn == "push":
            v = self.rd(ops[0], 32)
            if ops[0].type == X.X86_OP_IMM:
                v = f"0x{ops[0].imm & 0xffffffff:x}u"
            o(f"{{ uint32_t v = {v}; esp -= 4; WR32(esp, v); }}")
        elif mn == "pop":
            o(f"{{ uint32_t v = RD32(esp); esp += 4; {self.wr(ops[0], 'v')} }}")
        elif mn == "pushal":
            o("{ uint32_t t = esp; esp -= 32; WR32(esp+28,eax); WR32(esp+24,ecx); WR32(esp+20,edx); WR32(esp+16,ebx); WR32(esp+12,t); WR32(esp+8,ebp); WR32(esp+4,esi); WR32(esp,edi); }")
        elif mn == "popal":
            o("{ edi=RD32(esp); esi=RD32(esp+4); ebp=RD32(esp+8); ebx=RD32(esp+16); edx=RD32(esp+20); ecx=RD32(esp+24); eax=RD32(esp+28); esp += 32; }")
        elif mn == "leave":
            o("esp = ebp; ebp = RD32(esp); esp += 4;")
        elif mn == "xchg":
            o(f"{{ uint32_t t0 = {self.rd(ops[0])}, t1 = {self.rd(ops[1])}; {self.wr(ops[0], 't1')} {self.wr(ops[1], 't0')} }}")
        elif mn == "cdq":
            o("edx = (uint32_t)((int32_t)eax >> 31);")
        elif mn == "cwde":
            o("eax = (uint32_t)(int32_t)(int16_t)eax;")
        elif mn == "cbw":
            o("eax = (eax & 0xffff0000u) | ((uint16_t)(int16_t)(int8_t)eax);")
        elif mn == "imul":
            if len(ops) == 1:
                if bits == 32:
                    o(f"{{ int64_t r = (int64_t)(int32_t)eax * (int32_t){self.rd(ops[0])}; eax = (uint32_t)r; edx = (uint32_t)(r >> 32); cf = of = r != (int32_t)r; }}")
                elif bits == 16:
                    o(f"{{ int32_t r = (int32_t)(int16_t)eax * (int16_t){self.rd(ops[0])}; eax = (eax & 0xffff0000u) | ((uint32_t)r & 0xffffu); edx = (edx & 0xffff0000u) | (((uint32_t)r >> 16) & 0xffffu); cf = of = r != (int16_t)r; }}")
                else:
                    o(f"{{ int32_t r = (int32_t)(int8_t)eax * (int8_t){self.rd(ops[0])}; eax = (eax & 0xffff0000u) | ((uint32_t)r & 0xffffu); cf = of = r != (int8_t)r; }}")
            else:
                a = self.rd(ops[1]) if len(ops) == 3 else self.rd(ops[0])
                b = self.rd(ops[2], bits) if len(ops) == 3 else self.rd(ops[1])
                if len(ops) == 3 and ops[2].type == X.X86_OP_IMM:
                    b = f"0x{ops[2].imm & 0xffffffff:x}u"
                o(f"{{ int64_t r = (int64_t)({STYPE[bits]}){a} * ({STYPE[bits]}){b}; cf = of = r != ({STYPE[bits]})r; {self.wr(ops[0], '(uint32_t)r')} }}")
        elif mn == "mul":
            if bits == 32:
                o(f"{{ uint64_t r = (uint64_t)eax * {self.rd(ops[0])}; eax = (uint32_t)r; edx = (uint32_t)(r >> 32); cf = of = edx != 0; }}")
            elif bits == 16:
                o(f"{{ uint32_t r = (eax & 0xffffu) * {self.rd(ops[0])}; eax = (eax & 0xffff0000u) | (r & 0xffffu); edx = (edx & 0xffff0000u) | (r >> 16); cf = of = (r >> 16) != 0; }}")
            else:
                o(f"{{ uint32_t r = (eax & 0xffu) * {self.rd(ops[0])}; eax = (eax & 0xffff0000u) | (r & 0xffffu); cf = of = (r >> 8) != 0; }}")
        elif mn in ("div", "idiv"):
            if bits != 32: raise Unsupported(f"{mn}{bits}")
            if mn == "div":
                o(f"{{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = {self.rd(ops[0])}; if (!d) x86_fatal(\"div by zero\", 0x{ins.address:x}u); eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }}")
            else:
                o(f"{{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t){self.rd(ops[0])}; if (!d) x86_fatal(\"div by zero\", 0x{ins.address:x}u); eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d); }}")
        elif mn.startswith("set") and mn[3:] in COND:
            o(self.wr(ops[0], f"({COND[mn[3:]]}) ? 1 : 0"))
        elif mn.startswith("cmov") and mn[4:] in COND:
            o(f"if ({COND[mn[4:]]}) {self.wr(ops[0], self.rd(ops[1]))}")
        elif mn in ("nop", "fwait", "wait"):
            pass
        elif mn == "cld":
            o("df = 0;")
        elif mn in ("clc", "stc", "cmc"):
            o({"clc": "cf = 0;", "stc": "cf = 1;", "cmc": "cf = !cf;"}[mn])
        elif mn in ("stosb", "stosw", "stosd", "rep stosb", "rep stosw", "rep stosd",
                    "movsb", "movsw", "movsd", "rep movsb", "rep movsw", "rep movsd") and (mn != "movsd" or not ops or ops[0].type == X.X86_OP_MEM):
            rep = mn.startswith("rep ")
            base = mn.split()[-1]
            sz = {"b": 1, "w": 2, "d": 4}[base[-1]]
            if base.startswith("stos"):
                body = f"WR{sz * 8}(edi, ({CTYPE[sz * 8]})eax); edi += df ? -{sz} : {sz};"
            else:
                body = f"WR{sz * 8}(edi, RD{sz * 8}(esi)); esi += df ? -{sz} : {sz}; edi += df ? -{sz} : {sz};"
            o(f"while (ecx) {{ {body} ecx--; }}" if rep else body)
        elif mn in ("repne scasb", "repe cmpsb", "repe cmpsd", "repne scasw"):
            if mn == "repne scasb":
                o("while (ecx) { uint32_t a = eax & 0xff, b = RD8(edi); edi++; ecx--; uint32_t r = (a - b) & 0xff; cf = a < b; zf = r == 0; sf = r >> 7; of = (((a ^ b) & (a ^ r)) >> 7) & 1; if (zf) break; }")
            elif mn in ("repe cmpsb", "repe cmpsd"):
                sz = 1 if mn.endswith("b") else 4
                o(f"while (ecx) {{ uint32_t a = RD{sz*8}(esi), b = RD{sz*8}(edi); esi += {sz}; edi += {sz}; ecx--; uint32_t r = (a - b) & {MASK[sz*8]}; cf = a < b; zf = r == 0; sf = r >> {sz*8-1}; of = (((a ^ b) & (a ^ r)) >> {sz*8-1}) & 1; if (!zf) break; }}")
            else:
                raise Unsupported(mn)
        elif mn == "lodsb":
            o("eax = (eax & 0xffffff00u) | RD8(esi); esi++;")
        elif mn == "lodsd":
            o("eax = RD32(esi); esi += 4;")
        elif mn == "sahf":
            o("{ uint32_t h = (eax >> 8) & 0xff; cf = h & 1; pf = (h >> 2) & 1; zf = (h >> 6) & 1; sf = (h >> 7) & 1; }")
        elif mn == "lahf":
            o("eax = (eax & 0xffff00ffu) | ((uint32_t)(cf | 2 | (pf << 2) | (zf << 6) | (sf << 7)) << 8);")
        elif mn == "bswap":
            o(self.wr(ops[0], f"__builtin_bswap32({self.rd(ops[0])})"))
        elif mn == "bt" and not (ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG):
            o(f"cf = ({self.rd(ops[0])} >> ({self.rd(ops[1])} & {bits - 1})) & 1;")
        elif mn in ("lock inc", "lock dec", "lock add", "lock sub", "lock xadd", "lock cmpxchg", "lock or", "lock and"):
            # single-threaded guest semantics are enough: drop the lock prefix
            ins_m = mn.split()[1]
            if ins_m in ("inc", "dec"):
                d = "+" if ins_m == "inc" else "-"
                o(f"{{ uint32_t a = {self.rd(ops[0])}; uint32_t r = (a {d} 1) & {M}; of = r == {'0x%xu' % (1 << (bits - 1)) if ins_m == 'inc' else '0x%xu' % ((1 << (bits - 1)) - 1)}; {self.szp('r', bits)} {self.wr(ops[0], 'r')} }}")
            elif ins_m == "xadd":
                o(f"{{ uint32_t a = {self.rd(ops[0])}, b = {self.rd(ops[1])}; uint32_t r = (a + b) & {M}; cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> {bits - 1}) & 1; {self.szp('r', bits)} {self.wr(ops[1], 'a')} {self.wr(ops[0], 'r')} }}")
            elif ins_m == "cmpxchg":
                acc = {8: "(eax & 0xffu)", 16: "(eax & 0xffffu)", 32: "eax"}[bits]
                o(f"{{ uint32_t a = {acc}, d = {self.rd(ops[0])}; uint32_t r = (a - d) & {M}; cf = a < d; of = (((a ^ d) & (a ^ r)) >> {bits - 1}) & 1; {self.szp('r', bits)}")
                o(f"  if (zf) {{ {self.wr(ops[0], self.rd(ops[1]))} }} else {{ eax = {'d' if bits == 32 else f'(eax & ~{M}) | d'}; }} }}")
            else:
                raise Unsupported(mn)
        elif mn in ("rcl", "rcr"):
            cnt = self.rd(ops[1]) if len(ops) > 1 else "1u"
            o(f"{{ uint32_t n = (({cnt}) & 31) % {bits + 1}; uint32_t a = {self.rd(ops[0])}; uint64_t c = cf;")
            if mn == "rcl":
                o(f"  for (uint32_t k = 0; k < n; k++) {{ uint32_t hi = (a >> {bits - 1}) & 1; a = ((a << 1) | (uint32_t)c) & {M}; c = hi; }}")
                o(f"  if (n) {{ cf = (uint32_t)c; of = ((a >> {bits - 1}) & 1) ^ cf; }}")
            else:
                o(f"  for (uint32_t k = 0; k < n; k++) {{ uint32_t lo = a & 1; a = (a >> 1) | ((uint32_t)c << {bits - 1}); c = lo; }}")
                o(f"  if (n) {{ cf = (uint32_t)c; of = ((a >> {bits - 1}) ^ (a >> {bits - 2})) & 1; }}")
            o(f"  {self.wr(ops[0], 'a')} }}")
        elif mn in ("bt", "bts", "btr", "btc") and ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
            # register bit offset is not masked for memory operands: it indexes a bit string
            A = self.addr_expr(ops[0])
            o(f"{{ int32_t off = (int32_t){self.rd(ops[1])}; uint32_t ea = (uint32_t)({A} + (uint32_t)((off >> 5) * 4)), n = off & 31, a = RD32(ea); cf = (a >> n) & 1;")
            if mn != "bt":
                opn = {"bts": "|", "btr": "& ~", "btc": "^"}[mn]
                o(f"  WR32(ea, a {opn} (1u << n));")
            o("}")
        elif mn in ("bts", "btr", "btc"):
            opn = {"bts": "|", "btr": "& ~", "btc": "^"}[mn]
            o(f"{{ uint32_t a = {self.rd(ops[0])}, n = {self.rd(ops[1])} & {bits - 1}; cf = (a >> n) & 1; {self.wr(ops[0], f'a {opn} (1u << n)')} }}")
        elif mn == "pushfd":
            o("{ uint32_t v = cf | 2 | 0x200 | (pf << 2) | (zf << 6) | (sf << 7) | (df << 10) | (of << 11) | ctx->eflags_hi; esp -= 4; WR32(esp, v); }")
        elif mn == "popfd":
            o("{ uint32_t v = RD32(esp); esp += 4; cf = v & 1; pf = (v >> 2) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; df = (v >> 10) & 1; of = (v >> 11) & 1; ctx->eflags_hi = v & 0x00200000u; }")
        elif mn == "std":
            o("df = 1;")
        elif mn == "cpuid":
            o("x86_cpuid(ctx, &eax, &ebx, &ecx, &edx);")
        elif mn in ("cmpsb", "cmpsd", "scasb", "scasd") and not (mn == "cmpsd" and ops and ops[0].type != X.X86_OP_MEM):
            sz = 1 if mn.endswith("b") else 4
            if mn.startswith("cmps"):
                o(f"{{ uint32_t a = RD{sz*8}(esi), b = RD{sz*8}(edi); esi += df ? -{sz} : {sz}; edi += df ? -{sz} : {sz};")
            else:
                o(f"{{ uint32_t a = eax & {MASK[sz*8]}, b = RD{sz*8}(edi); edi += df ? -{sz} : {sz};")
            o(f"  uint32_t r = (a - b) & {MASK[sz*8]}; cf = a < b; of = (((a ^ b) & (a ^ r)) >> {sz*8-1}) & 1; {self.szp('r', sz*8)} }}")
        # ---- control flow
        elif CS_GRP_JUMP in ins.groups and ops and ops[0].type == X.X86_OP_IMM and not (self.t0 <= ops[0].imm < self.t1):
            o(f"x86_fatal(\"jump outside .text (bad decode?)\", 0x{ins.address:x}u);")
            return out, mn != "jmp"
        elif mn == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm
                if t in self.func_starts and t != f:
                    o(f"SPILL; SPILLF; f_{t:x}_or_call(ctx, 0x{t:x}u); return;")
                else:
                    o(f"goto L_{t:x};")
            elif ins.address in self.jumptables:
                o(f"switch ({self.rd(op)}) {{")
                for t in sorted(set(self.jumptables[ins.address])):
                    o(f"  case 0x{t:x}u: goto L_{t:x};")
                o(f"  default: x86_fatal(\"bad jumptable target\", 0x{ins.address:x}u); }}")
            else:
                # indirect tail jump (e.g. import thunk or COM thunk)
                o(f"{{ uint32_t t = {self.rd(op)}; SPILL; SPILLF; x86_call(ctx, t); return; }}")
            return out, False
        elif mn == "jecxz":
            o(f"if (ecx == 0) goto L_{ops[0].imm:x};")
        elif mn.startswith("j") and mn[1:] in COND:
            t = ops[0].imm
            if t in self.func_starts and t != f:
                o(f"if ({COND[mn[1:]]}) {{ SPILL; SPILLF; f_{t:x}_or_call(ctx, 0x{t:x}u); return; }}")
            else:
                o(f"if ({COND[mn[1:]]}) goto L_{t:x};")
        elif mn == "call":
            op = ops[0]
            tgt = f"0x{op.imm:x}u" if op.type == X.X86_OP_IMM else self.rd(op)
            o(f"{{ uint32_t t = {tgt}; esp -= 4; WR32(esp, 0x{nxt:x}u); SPILL;")
            if op.type == X.X86_OP_IMM:
                o(f"  f_{op.imm:x}_or_call(ctx, t); RELOAD; PROF_FN(0x{f:x}u); }}")
            else:
                o(f"  x86_call(ctx, t); RELOAD; PROF_FN(0x{f:x}u); }}")
        elif mn == "ret":
            n = ops[0].imm if ops else 0
            o(f"esp += {4 + n}; SPILL; return;")
            return out, False
        elif mn in ("int3", "hlt", "ud2"):
            o(f"x86_fatal(\"{mn}\", 0x{ins.address:x}u);")
            return out, False
        else:
            raise Unsupported(mn)
        return out, True


    # ---------------------------------------------------------------- x87 (decoded from opcode bytes)
    def lift_x87(self, ins, b):
        op, modrm = b[0], b[1]
        reg = (modrm >> 3) & 7
        if modrm < 0xC0:
            mem = next(x for x in ins.operands if x.type == X.X86_OP_MEM)
            A = self.addr_expr(mem)
            ld = {0xd8: "RDF32", 0xdc: "RDF64", 0xda: "(double)(int32_t)RD32", 0xde: "(double)(int16_t)RD16"}
            if op in ld:
                arith = ["FST(0) = FST(0) + v;", "FST(0) = FST(0) * v;", "x87_cmp(ctx, FST(0), v);",
                         "x87_cmp(ctx, FST(0), v); FPOP();", "FST(0) = FST(0) - v;", "FST(0) = v - FST(0);",
                         "FST(0) = FST(0) / v;", "FST(0) = v / FST(0);"]
                return [f"{{ double v = {ld[op]}({A}); {arith[reg]} }}"]
            t = {
                (0xd9, 0): f"FPUSH(RDF32({A}));",
                (0xd9, 2): f"WRF32({A}, FST(0));",
                (0xd9, 3): f"WRF32({A}, FST(0)); FPOP();",
                (0xd9, 5): f"ctx->fcw = RD16({A});",
                (0xd9, 7): f"WR16({A}, ctx->fcw);",
                (0xdb, 0): f"FPUSH((double)(int32_t)RD32({A}));",
                (0xdb, 1): f"WR32({A}, (uint32_t)x87_toint(ctx, FST(0), 1, 32)); FPOP();",
                (0xdb, 2): f"WR32({A}, (uint32_t)x87_toint(ctx, FST(0), 0, 32));",
                (0xdb, 3): f"WR32({A}, (uint32_t)x87_toint(ctx, FST(0), 0, 32)); FPOP();",
                (0xdb, 5): f"FPUSH(RDF80({A}));",
                (0xdb, 7): f"WRF80({A}, FST(0)); FPOP();",
                (0xdd, 0): f"FPUSH(RDF64({A}));",
                (0xdd, 1): f"WR64({A}, (uint64_t)x87_toint(ctx, FST(0), 1, 64)); FPOP();",
                (0xdd, 2): f"WRF64({A}, FST(0));",
                (0xdd, 3): f"WRF64({A}, FST(0)); FPOP();",
                (0xdd, 7): f"WR16({A}, x87_sw(ctx));",
                (0xdf, 0): f"FPUSH((double)(int16_t)RD16({A}));",
                (0xdf, 1): f"WR16({A}, (uint16_t)x87_toint(ctx, FST(0), 1, 16)); FPOP();",
                (0xdf, 2): f"WR16({A}, (uint16_t)x87_toint(ctx, FST(0), 0, 16));",
                (0xdf, 3): f"WR16({A}, (uint16_t)x87_toint(ctx, FST(0), 0, 16)); FPOP();",
                (0xdf, 5): f"FPUSH((double)(int64_t)RD64({A}));",
                (0xdf, 7): f"WR64({A}, (uint64_t)x87_toint(ctx, FST(0), 0, 64)); FPOP();",
            }.get((op, reg))
            if t is None:
                raise Unsupported(f"x87 {op:02x}/{reg}")
            return [t]
        i, hi = modrm & 7, modrm & 0xF8
        S = f"FST({i})"
        if op == 0xd8:
            return [{0xC0: f"FST(0) = FST(0) + {S};", 0xC8: f"FST(0) = FST(0) * {S};",
                     0xD0: f"x87_cmp(ctx, FST(0), {S});", 0xD8: f"x87_cmp(ctx, FST(0), {S}); FPOP();",
                     0xE0: f"FST(0) = FST(0) - {S};", 0xE8: f"FST(0) = {S} - FST(0);",
                     0xF0: f"FST(0) = FST(0) / {S};", 0xF8: f"FST(0) = {S} / FST(0);"}[hi]]
        if op in (0xdc, 0xde):
            if op == 0xde and modrm == 0xD9:
                return ["x87_cmp(ctx, FST(0), FST(1)); FPOP(); FPOP();"]
            r = {0xC0: f"{S} = {S} + FST(0);", 0xC8: f"{S} = {S} * FST(0);",
                 0xE0: f"{S} = FST(0) - {S};", 0xE8: f"{S} = {S} - FST(0);",
                 0xF0: f"{S} = FST(0) / {S};", 0xF8: f"{S} = {S} / FST(0);"}.get(hi)
            if r is None:
                raise Unsupported(f"x87 {op:02x} {modrm:02x}")
            return [r + (" FPOP();" if op == 0xde else "")]
        if op == 0xd9:
            if hi == 0xC0: return [f"{{ double v = {S}; FPUSH(v); }}"]
            if hi == 0xC8: return [f"{{ double v = {S}; {S} = FST(0); FST(0) = v; }}"]
            r = {0xD0: "", 0xE0: "FST(0) = -FST(0);", 0xE1: "FST(0) = fabs(FST(0));",
                 0xE4: "x87_cmp(ctx, FST(0), 0.0);", 0xE8: "FPUSH(1.0);", 0xE9: "FPUSH(3.321928094887362);",
                 0xEA: "FPUSH(1.4426950408889634);", 0xEB: "FPUSH(3.141592653589793);",
                 0xEC: "FPUSH(0.3010299956639812);", 0xED: "FPUSH(0.6931471805599453);", 0xEE: "FPUSH(0.0);",
                 0xF0: "FST(0) = exp2(FST(0)) - 1.0;", 0xF1: "FST(1) = FST(1) * log2(FST(0)); FPOP();",
                 0xF2: "FST(0) = tan(FST(0)); FPUSH(1.0);", 0xF3: "FST(1) = atan2(FST(1), FST(0)); FPOP();",
                 0xFA: "FST(0) = sqrt(FST(0));", 0xFB: "{ double v = FST(0); FST(0) = sin(v); FPUSH(cos(v)); }",
                 0xFC: "FST(0) = x87_rint(ctx, FST(0));", 0xFD: "FST(0) = ldexp(FST(0), (int)trunc(FST(1)));",
                 0xFE: "FST(0) = sin(FST(0));", 0xFF: "FST(0) = cos(FST(0));",
                 0xF8: "FST(0) = fmod(FST(0), FST(1));", 0xF5: "FST(0) = remainder(FST(0), FST(1));"}.get(modrm)
            if r is None: raise Unsupported(f"x87 d9 {modrm:02x}")
            return [r]
        if op == 0xdd:
            r = {0xC0: "", 0xD0: f"{S} = FST(0);", 0xD8: f"{S} = FST(0); FPOP();",
                 0xE0: f"x87_cmp(ctx, FST(0), {S});", 0xE8: f"x87_cmp(ctx, FST(0), {S}); FPOP();"}.get(hi)
            if r is None: raise Unsupported(f"x87 dd {modrm:02x}")
            return [r]
        if op == 0xda and modrm == 0xE9:
            return ["x87_cmp(ctx, FST(0), FST(1)); FPOP(); FPOP();"]
        if op == 0xdb:
            if modrm == 0xE2: return ["ctx->fsw &= 0x7f00u;"]
            if modrm == 0xE3: return ["ctx->ftop = 0; ctx->fcw = 0x37f; ctx->fsw = 0;"]
            if hi in (0xE8, 0xF0):
                return [f"{{ double a = FST(0), c = {S}; int u = a != a || c != c; zf = u || a == c; pf = u; cf = u || a < c; of = sf = 0; }}"]
        if op == 0xdf:
            if modrm == 0xE0: return ["eax = (eax & 0xffff0000u) | x87_sw(ctx);"]
            if hi in (0xE8, 0xF0):
                return [f"{{ double a = FST(0), c = {S}; int u = a != a || c != c; zf = u || a == c; pf = u; cf = u || a < c; of = sf = 0; FPOP(); }}"]
        raise Unsupported(f"x87 {op:02x} {modrm:02x}")

    # ---------------------------------------------------------------- MMX
    def mmx_src(self, op, bits=64):
        if op.type == X.X86_OP_REG:
            n = self.cur.reg_name(op.reg)
            if n.startswith("mm"):
                return f"ctx->mm[{n[2]}]"
            return f"(uint64_t){self.rd(op)}"
        if op.type == X.X86_OP_MEM:
            return f"(uint64_t)RD{op.size * 8}({self.addr_expr(op)})"
        if op.type == X.X86_OP_IMM:
            return f"(uint64_t)0x{op.imm & 0xff:x}u"
        raise Unsupported("mmx operand")

    def lift_mmx(self, ins):
        mn, ops = ins.mnemonic, ins.operands
        if mn == "emms":
            return []
        d = ops[0]
        dmm = d.type == X.X86_OP_REG and self.cur.reg_name(d.reg).startswith("mm")
        if mn in ("movd", "movq"):
            src = self.mmx_src(ops[1])
            if dmm:
                return [f"ctx->mm[{self.cur.reg_name(d.reg)[2]}] = {'(uint32_t)' if mn == 'movd' else ''}({src});"]
            if mn == "movd":
                return [self.wr(d, f"(uint32_t)({src})")]
            return [f"WR64({self.addr_expr(d)}, {src});"]
        if not dmm:
            raise Unsupported(f"mmx dst {mn}")
        n = self.cur.reg_name(d.reg)[2]
        return [f"ctx->mm[{n}] = mmx_{mn}(ctx->mm[{n}], {self.mmx_src(ops[1])});"]

    # ---------------------------------------------------------------- functions
    # ------------------------------------------------------------------ dead flag elimination
    # Every arithmetic instruction is lifted with all of CF/ZF/SF/OF/PF computed. Almost none are ever
    # read, and in big functions (the game's 14 KB blitters) the C compiler cannot prove it, so they cost
    # real instructions (PF is a popcount). A backward liveness pass over the function's instructions
    # removes assignments to flags that are dead. Conservative rules:
    #   uses  = any textual read of a flag in the instruction's C (SPILLF reads all of them)
    #   kills = assignments that execute unconditionally (not under if/while/for/?:/else/case)
    #   an assignment is removed only if its flag is dead after the instruction and the instruction's
    #   C does not read that flag itself.
    FLAGS = ("cf", "zf", "sf", "of", "pf")
    _flag_tok = re.compile(r"\b(cf|zf|sf|of|pf)\b")
    _flag_lhs = re.compile(r"\b(cf|zf|sf|of|pf)\s*=(?!=)")

    def _flag_uses_kills(self, text):
        if "SPILLF" in text:
            return set(self.FLAGS), set()
        uses = set()
        for m in self._flag_tok.finditer(text):
            if not self._flag_lhs.match(text, m.start()):
                uses.add(m.group(1))
        kills = set()
        for m in self._flag_lhs.finditer(text):
            if self._unconditional(text, m.start()):
                kills.add(m.group(1))
        return uses, kills

    @staticmethod
    def _unconditional(text, pos):
        # walk the brace structure up to pos: every enclosing '{' must open a plain block
        stack = []
        stmt_start = 0
        i = 0
        while i < pos:
            c = text[i]
            if c == "{":
                head = text[stmt_start:i]
                stack.append(bool(re.search(r"\b(if|while|for|switch|else|do)\b|\?|\bcase\b", head)))
                stmt_start = i + 1
            elif c == "}":
                if stack:
                    stack.pop()
                stmt_start = i + 1
            elif c == ";":
                stmt_start = i + 1
            i += 1
        if any(stack):
            return False
        cur = text[stmt_start:pos]
        return not re.search(r"\b(if|while|for|switch|else|do|case|goto|return|break)\b|\?|:", cur)

    def prune_flags(self, items, addrs, addr_set, entry):
        n = len(items)
        index = {a: k for k, (a, *_rest) in enumerate(items)}
        texts = ["\n".join(body) + "\n" + (tail or "") for (a, ins, body, falls, tail) in items]
        uk = [self._flag_uses_kills(t) for t in texts]
        succ = []
        allf = set(self.FLAGS)
        for k, (a, ins, body, falls, tail) in enumerate(items):
            ss = set()
            for t in re.findall(r"goto L_([0-9a-f]+);", texts[k]):
                t = int(t, 16)
                if t in index:
                    ss.add(index[t])
            if falls and not tail and k + 1 < n:
                ss.add(k + 1)
            succ.append(ss)
        live_in = [set() for _ in range(n)]
        live_out = [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for k in range(n - 1, -1, -1):
                out = set()
                for s_ in succ[k]:
                    out |= live_in[s_]
                uses, kills = uk[k]
                inn = uses | (out - kills)
                if out != live_out[k] or inn != live_in[k]:
                    live_out[k], live_in[k] = out, inn
                    changed = True
        res = []
        for k, (a, ins, body, falls, tail) in enumerate(items):
            uses, _ = uk[k]
            dead = allf - live_out[k] - uses
            if dead:
                body = [self._drop_flag_assigns(l, dead) for l in body]
            res.append((a, ins, body, falls, tail))
        k0 = index.get(entry)
        entry_live = live_in[k0] if k0 is not None else allf
        return res, entry_live

    @staticmethod
    def _drop_flag_assigns(line, dead):
        prev = None
        while prev != line:
            prev = line
            for fl in dead:
                # inner target of a chain "x = fl = expr;" -> "x = expr;"
                line = re.sub(rf"(?<==)(\s*){fl}\s*=(?!=)", r"\1", line)
                # head of a chain "fl = x = expr;" -> "x = expr;"
                line = re.sub(rf"(^|(?<=[;{{}}\s])){fl}\s*=\s*(?=(cf|zf|sf|of|pf)\s*=(?!=))", "", line)
                # whole statement "fl = expr;" not part of a chain (expressions never contain ';')
                line = re.sub(rf"(^|(?<=[;{{}}]))(\s*){fl}\s*=(?!=)[^;=]*(==[^;=]*|!=[^;=]*|<=[^;=]*|>=[^;=]*)*;", r"\2", line)
        return line

    def lift_function(self, f):
        addrs, labels = self.body(f)
        addr_set = set(addrs)
        # pre-pass: fallthrough into a non-adjacent emitted address needs a label
        for i, a in enumerate(addrs):
            ins = self.insn(a)
            if ins.mnemonic in ("jmp", "ret", "int3", "hlt", "ud2"):
                continue
            nxt = a + ins.size
            if nxt in addr_set and (i + 1 >= len(addrs) or addrs[i + 1] != nxt):
                labels.add(nxt)
        # pass 1: lift every instruction
        lifted_insns = []
        for i, a in enumerate(addrs):
            ins = self.insn(a)
            try:
                body, falls = self.lift_insn(ins, f)
            except Unsupported as e:
                self.unsupported[str(e) if str(e) != "operand" else f"operand:{ins.mnemonic}"] += 1
                body, falls = [f"x86_fatal(\"unsupported: {ins.mnemonic} {ins.op_str}\", 0x{a:x}u);"], True
            tail = None
            if falls:
                nxt = a + ins.size
                following = addrs[i + 1] if i + 1 < len(addrs) else None
                if nxt != following:
                    if nxt in addr_set:
                        tail = f"goto L_{nxt:x};"
                    else:
                        tail = f"SPILL; SPILLF; f_{nxt:x}_or_call(ctx, 0x{nxt:x}u); return; /* falls into next function */"
            lifted_insns.append((a, ins, body, falls, tail))
        # pass 2: drop flag computations nobody reads (see flag_liveness)
        entry_flags = set(self.FLAGS)
        if not self.trace:
            lifted_insns, entry_flags = self.prune_flags(lifted_insns, addrs, addr_set, f)
        # restrict: guest memory (mem_) never overlaps the CPU struct, so the compiler may keep guest
        # registers in host registers across guest stores (and forward them through inlined callees)
        lines = [f"void f_{f:x}(CPU *restrict ctx) {{",
                 "  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;",
                 "  uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, df = 0;",
                 "  uint8_t *const mem_ = MEM;  /* guest memory base, kept in a register (MEM never changes) */",
                 "  RELOAD;" + (" RELOADF;" if entry_flags else " /* no flag is read before being set */") +
                 f" PROF_FN(0x{f:x}u);"]
        if addrs and addrs[0] != f:
            lines.append(f"  goto L_{f:x};  /* entry is not the lowest address of the body */")
        for a, ins, body, falls, tail in lifted_insns:
            if a in labels:
                lines.append(f" L_{a:x}:;")
            lines.append(f"  /* {a:x}: {ins.mnemonic} {ins.op_str} */")
            if self.trace:
                lines.append(f"  TRACE(0x{a:x}u);")
            lines += ["  " + l for l in body]
            if tail:
                lines.append("  " + tail)
        lines.append("}")
        return "\n".join(lines)

    def emit(self, funcs, out_dir):
        os.makedirs(out_dir, exist_ok=True)
        lifted = set(funcs)
        bodies = [self.lift_function(f) for f in sorted(funcs)]
        # callees referenced
        import re
        refs = set()
        for b in bodies:
            refs |= {int(x, 16) for x in re.findall(r"f_([0-9a-f]+)_or_call", b)}
        hdr = ['/* lifted code addresses guest memory through a function-local copy of MEM (see x86rt.h) */',
               '#define X86_MEMBASE mem_',
               '#include "x86rt.h"',
               '#ifdef X86_PROF  /* sampling profiler: innermost guest function currently running */',
               'extern volatile uint32_t x86_prof_cur;',
               '#define PROF_FN(a) (x86_prof_cur = (a))',
               '#else',
               '#define PROF_FN(a) ((void)0)',
               '#endif',
               "#define SPILL do { ctx->eax=eax; ctx->ecx=ecx; ctx->edx=edx; ctx->ebx=ebx; ctx->esp=esp; ctx->ebp=ebp; ctx->esi=esi; ctx->edi=edi; } while (0)",
               "#define RELOAD do { eax=ctx->eax; ecx=ctx->ecx; edx=ctx->edx; ebx=ctx->ebx; esp=ctx->esp; ebp=ctx->ebp; esi=ctx->esi; edi=ctx->edi; } while (0)",
               "#define SPILLF do { ctx->cf=cf; ctx->zf=zf; ctx->sf=sf; ctx->of=of; ctx->pf=pf; } while (0)",
               "#define RELOADF do { cf=ctx->cf; zf=ctx->zf; sf=ctx->sf; of=ctx->of; pf=ctx->pf; } while (0)",
               "#define TRACE(a) do { if (x86_trace_buf) { SPILL; x86_trace_rec(ctx, a); } } while (0)",
               '#pragma clang diagnostic ignored "-Wunused-label"',
               '#pragma clang diagnostic ignored "-Wunused-but-set-variable"', ""]
        hdr += [f"void f_{f:x}(CPU *ctx);" for f in sorted(lifted)]
        for r in sorted(refs):
            hdr.append(f"#define f_{r:x}_or_call(ctx, t) f_{r:x}(ctx)" if r in lifted and r not in self.hle else f"#define f_{r:x}_or_call(ctx, t) x86_call(ctx, t)")
        with open(os.path.join(out_dir, "lifted.h"), "w") as fp:
            fp.write("\n".join(hdr) + "\n")
        for old in os.listdir(out_dir):
            if old.startswith("lifted") and old.endswith(".c"):
                os.remove(os.path.join(out_dir, old))
        CHUNK = 150
        for k in range(0, len(bodies), CHUNK):
            with open(os.path.join(out_dir, f"lifted_{k // CHUNK:03d}.c"), "w") as fp:
                fp.write('#include "lifted.h"\n\n' + "\n\n".join(bodies[k:k + CHUNK]) + "\n")
        with open(os.path.join(out_dir, "lifted_reg.c"), "w") as fp:
            fp.write('#include "lifted.h"\nvoid lifted_register_all(void) {\n')
            for f in sorted(lifted):
                fp.write(f"  x86_register(0x{f:x}u, f_{f:x});\n")
            fp.write("}\n")
        with open(os.path.join(out_dir, "image.bin"), "wb") as fp:
            fp.write(self.img)
        with open(os.path.join(out_dir, "image.h"), "w") as fp:
            fp.write(f"#define IMAGE_BASE 0x{self.base:x}u\n#define IMAGE_SIZE 0x{len(self.img):x}u\n#define IMAGE_ENTRY 0x{self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint:x}u\n")


if __name__ == "__main__":
    exe, fj, out = sys.argv[1:4]
    L = Lifter(exe, fj)
    if sys.argv[4:] == ["--all"]:
        funcs = sorted(L.func_starts)
    else:
        funcs = [int(x, 16) for x in sys.argv[4:]]
    L.emit(funcs, out)
    print(f"lifted {len(funcs)} functions -> {out}/lifted_*.c")
    if L.unsupported:
        print("unsupported:", L.unsupported.most_common(60))
