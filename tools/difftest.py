"""Differential test: original x86 (Unicorn) vs. lifted C (libdifftest.dylib), function by function.

Usage:
  python -I tools/difftest.py [--jobs N] [--seeds K] [--funcs 0x..,0x..] [--limit N] OUT.json

Each function runs from identical random-but-coherent initial state (registers and stack args
point into a scratch area whose dwords are themselves pointers or small ints). Functions that the
reference emulator cannot complete (they reach imports, unmapped memory, etc.) are "skip".
Otherwise the lifted result must match: 8 GPRs and all writable guest memory.
"""
import sys, os, json, time, random, struct, ctypes, subprocess, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "all")
EXE = os.path.join(ROOT, "game", "Joey_Pc.exe")

TEB, TEB_SZ = 0x01E00000, 0x1000
STK_LO, STK_HI = 0x01E10000, 0x01F00000
SCR, SCR_SZ = 0x02000000, 0x10000
SENTINEL = 0x03000000
ARENA = 0x03100000
ICOUNT = 300000


class CPU(ctypes.Structure):
    _fields_ = [(r, ctypes.c_uint32) for r in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                                "fs_base", "cf", "zf", "sf", "of", "pf", "eflags_hi")] + \
               [("st", ctypes.c_double * 8), ("ftop", ctypes.c_uint32), ("fcw", ctypes.c_uint16),
                ("fsw", ctypes.c_uint16), ("mm", ctypes.c_uint64 * 8)]

REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


def image_layout():
    import pefile
    pe = pefile.PE(EXE)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = bytes(pe.get_memory_mapped_image())
    size = (len(img) + 0xFFF) & ~0xFFF
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    t0 = base + text.VirtualAddress
    t1 = t0 + ((text.Misc_VirtualSize + 0xFFF) & ~0xFFF)
    return base, img.ljust(size, b"\0"), t0, t1


def make_state(f, k):
    rnd = random.Random(f * 131 + k)
    def ptr():
        return SCR + 4 * rnd.randrange(0, (SCR_SZ - 0x1000) // 4)
    def val():
        r = rnd.random()
        return ptr() if r < 0.7 else rnd.randrange(0, 64) if r < 0.92 else rnd.getrandbits(32)
    scratch = b"".join(struct.pack("<I", val()) for _ in range(SCR_SZ // 4))
    regs = {r: ptr() for r in REGS}
    regs["eax"], regs["edx"] = val(), val()
    esp = STK_HI - 0x1000 - 4 * 16
    args = [val() for _ in range(15)]
    stack = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", a) for a in args)
    regs["esp"] = esp
    return regs, scratch, esp, stack


class TraceRec(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint32), ("r", ctypes.c_uint32 * 8)]


class Harness:
    """Reference (Unicorn) and lifted (dylib) execution of one guest function from identical state."""

    def __init__(self, build_dir=BUILD):
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_PROT_READ, UC_PROT_WRITE
        from unicorn import UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE
        from unicorn import x86_const as U
        self.U = U
        self.base, self.img, self.t0, self.t1 = base, img, t0, t1 = image_layout()
        lib = self.lib = ctypes.CDLL(os.path.join(build_dir, "libdifftest.dylib"))
        lib.difftest_run.argtypes = [ctypes.c_uint32, ctypes.POINTER(CPU)]
        lib.difftest_fatal_what.restype = ctypes.c_char_p
        self.arena = (ctypes.c_uint8 * ARENA)()
        self.A = ctypes.addressof(self.arena)
        lib.difftest_init(self.arena)

        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        RW = UC_PROT_READ | UC_PROT_WRITE
        uc.mem_map(base, t0 - base, RW)
        uc.mem_map(t0, t1 - t0, UC_PROT_ALL)
        uc.mem_map(t1, base + len(img) - t1, RW)
        uc.mem_map(TEB, TEB_SZ, RW)
        uc.mem_map(STK_LO, STK_HI - STK_LO, RW)
        uc.mem_map(SCR, SCR_SZ, RW)
        uc.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
        # fs: via a GDT descriptor whose base is the TEB (Unicorn has no FS_BASE in 32-bit mode);
        # loading a GDT also requires flat 32-bit SS/DS/ES descriptors.
        GDT = 0x03200000
        uc.mem_map(GDT, 0x1000, RW)
        fsd = 0xFFF | (TEB & 0xffffff) << 16 | (0x80 | 0x10 | 0x2) << 40 | (0x4 << 52) | ((TEB >> 24) & 0xff) << 56
        flat = 0xFFFF | (0x80 | 0x10 | 0x2) << 40 | 0xF << 48 | (0xC << 52)
        uc.mem_write(GDT + 8 * 2, struct.pack("<Q", flat))
        uc.mem_write(GDT + 8 * 3, struct.pack("<Q", fsd))
        uc.reg_write(U.UC_X86_REG_GDTR, (0, GDT, 0xFFF, 0))
        for r in ("SS", "DS", "ES"):
            uc.reg_write(getattr(U, f"UC_X86_REG_{r}"), 2 << 3)
        uc.reg_write(U.UC_X86_REG_FS, 3 << 3)

        teb = struct.pack("<III", 0xFFFFFFFF, STK_HI, STK_LO).ljust(0x18, b"\0") + struct.pack("<I", TEB)
        self.teb = teb.ljust(TEB_SZ, b"\0")
        self.compare = [(t1, base + len(img) - t1), (TEB, TEB_SZ), (STK_HI - 0x10000, 0x10000), (SCR, SCR_SZ)]

        self.last_fault = 0
        def on_unmapped(uc_, access, addr, size, value, user):
            self.last_fault = addr
            return False
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

        # HLE functions run natively in the lifted build; give the reference the same semantics.
        def hle_memmove(uc_, addr, size, user):
            esp_ = uc_.reg_read(U.UC_X86_REG_ESP)
            ret, dst, src, n = struct.unpack("<IIII", uc_.mem_read(esp_, 16))
            if n:
                uc_.mem_write(dst, bytes(uc_.mem_read(src, n)))
            uc_.reg_write(U.UC_X86_REG_EAX, dst)
            uc_.reg_write(U.UC_X86_REG_ESP, esp_ + 4)
            uc_.reg_write(U.UC_X86_REG_EIP, ret)
        # CPUID: same identity as runtime/x86rt.c x86_cpuid (Pentium II class, MMX, no SSE)
        from unicorn import UC_HOOK_INSN
        def on_cpuid(uc_, user):
            leaf = uc_.reg_read(U.UC_X86_REG_EAX)
            if leaf == 0: v = (1, 0x756e6547, 0x6c65746e, 0x49656e69)
            elif leaf == 1: v = (0x652, 0, 0, (1 << 0) | (1 << 4) | (1 << 8) | (1 << 15) | (1 << 23))
            else: v = (0, 0, 0, 0)
            for r, x in zip(("EAX", "EBX", "ECX", "EDX"), v):
                uc_.reg_write(getattr(U, f"UC_X86_REG_{r}"), x)
            uc_.reg_write(U.UC_X86_REG_EIP, uc_.reg_read(U.UC_X86_REG_EIP) + 2)
            return True
        uc.hook_add(UC_HOOK_INSN, on_cpuid, None, 1, 0, U.UC_X86_INS_CPUID)
        self.hle = (0x5C38C0, 0x5C6110)
        for h in self.hle:
            uc.hook_add(UC_HOOK_CODE, hle_memmove, begin=h, end=h)

        self.trace = None
        self.regids = [getattr(U, f"UC_X86_REG_{r.upper()}") for r in REGS]
        def on_code(uc_, addr, size, user):
            if self.trace is not None and t0 <= addr < t1 and len(self.trace) < 200000:
                self.trace.append((addr, tuple(uc_.reg_read(r) for r in self.regids)))
        uc.hook_add(UC_HOOK_CODE, on_code, begin=t0, end=t1 - 1)

    def run_ref(self, f, k, trace=False):
        U, uc = self.U, self.uc
        from unicorn import UcError
        regs, scratch, esp, stack = make_state(f, k)
        uc.mem_write(self.base, self.img)
        uc.mem_write(TEB, self.teb)
        uc.mem_write(STK_LO, b"\0" * (STK_HI - STK_LO))
        uc.mem_write(esp, stack)
        uc.mem_write(SCR, scratch)
        for r, v in regs.items():
            uc.reg_write(getattr(U, f"UC_X86_REG_{r.upper()}"), v)
        uc.reg_write(U.UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(U.UC_X86_REG_FPCW, 0x27F)
        self.trace = [] if trace else None
        why = None
        try:
            uc.emu_start(f, SENTINEL, count=ICOUNT)
            if uc.reg_read(U.UC_X86_REG_EIP) != SENTINEL:
                why = "icount"
        except UcError as e:
            why = f"{str(e)[15:20]} @{self.last_fault:#x} eip={uc.reg_read(U.UC_X86_REG_EIP):#x}"
        tr, self.trace = self.trace, None
        if why:
            return why, None, None, tr
        ref_regs = {r: uc.reg_read(getattr(U, f"UC_X86_REG_{r.upper()}")) for r in REGS}
        ref_mem = [bytes(uc.mem_read(a, n)) for a, n in self.compare]
        return None, ref_regs, ref_mem, tr

    def run_lifted(self, f, k, trace=False):
        A = self.A
        regs, scratch, esp, stack = make_state(f, k)
        ctypes.memmove(A + self.base, self.img, len(self.img))
        ctypes.memmove(A + TEB, self.teb, TEB_SZ)
        ctypes.memset(A + STK_LO, 0, STK_HI - STK_LO)
        ctypes.memmove(A + esp, stack, len(stack))
        ctypes.memmove(A + SCR, scratch, SCR_SZ)
        cpu = CPU(**regs)
        cpu.fs_base, cpu.fcw = TEB, 0x27F
        buf = None
        if trace:
            buf = (TraceRec * 200000)()
            self.lib.difftest_trace(buf, 200000)
        fatal = self.lib.difftest_run(f, ctypes.byref(cpu))
        tr = None
        if trace:
            n = self.lib.difftest_trace_len()
            tr = [(buf[i].addr, tuple(buf[i].r)) for i in range(n)]
            self.lib.difftest_trace(None, 0)
        if fatal:
            return f"{self.lib.difftest_fatal_what().decode()} @0x{self.lib.difftest_fatal_addr():x}", cpu, tr
        return None, cpu, tr

    def diff(self, ref_regs, ref_mem, cpu):
        diffs = [f"{r}: ref {ref_regs[r]:#x} lifted {getattr(cpu, r):#x}" for r in REGS if ref_regs[r] != getattr(cpu, r)]
        for (a, n), rm in zip(self.compare, ref_mem):
            lm = ctypes.string_at(self.A + a, n)
            if a == STK_HI - 0x10000:          # only live stack (at/above final esp) matters
                cut = max(0, min(n, ref_regs["esp"] - a))
                lm, rm = b"\0" * cut + lm[cut:], b"\0" * cut + rm[cut:]
            if lm != rm:
                i = next(j for j in range(n) if lm[j] != rm[j])
                diffs.append(f"mem {a + i:#x}: ref {rm[i]:02x} lifted {lm[i]:02x}")
        return diffs


def worker(funcs, seeds, out_path, prog_path):
    H = Harness()
    out = open(out_path, "a")
    for f in funcs:
        for k in range(seeds):
            open(prog_path, "w").write(f"{f} {k}\n")
            res = {"f": f, "k": k}
            why, ref_regs, ref_mem, _ = H.run_ref(f, k)
            if why:
                res.update(status="skip", why=why)
            else:
                fatal, cpu, _ = H.run_lifted(f, k)
                if fatal:
                    res.update(status="fatal", why=fatal)
                else:
                    d = H.diff(ref_regs, ref_mem, cpu)
                    res["status"] = "mismatch" if d else "ok"
                    if d:
                        res["why"] = d[:4]
            out.write(json.dumps(res) + "\n"); out.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--funcs")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--worker", nargs=3)  # internal: funcs_file out prog
    a = ap.parse_args()
    if a.worker:
        funcs = json.load(open(a.worker[0]))
        return worker(funcs, a.seeds, a.worker[1], a.worker[2])

    funcs = [int(x, 16) for x in a.funcs.split(",")] if a.funcs else \
        json.load(open(os.path.join(ROOT, "work", "exe", "funcs.json")))["functions"]
    if a.limit:
        funcs = funcs[:a.limit]
    tmp = a.out + ".d"
    os.makedirs(tmp, exist_ok=True)
    chunks = [funcs[i::a.jobs] for i in range(a.jobs)]
    procs = {}
    def launch(i, fl):
        ff, prog, part = f"{tmp}/funcs{i}.json", f"{tmp}/prog{i}", f"{tmp}/part{i}.jsonl"
        json.dump(fl, open(ff, "w")); open(prog, "w").write("")
        p = subprocess.Popen([sys.executable, "-I", __file__, a.out, "--seeds", str(a.seeds), "--worker", ff, part, prog],
                             stderr=subprocess.DEVNULL)
        procs[i] = [p, fl, prog, part, time.time(), ""]
    for i, c in enumerate(chunks):
        if c: launch(i, c)
    crashes = []
    while procs:
        time.sleep(0.5)
        for i in list(procs):
            p, fl, prog, part, last_t, last_s = procs[i]
            cur = open(prog).read().strip()
            if cur != last_s:
                procs[i][4], procs[i][5] = time.time(), cur
            rc = p.poll()
            stuck = rc is None and time.time() - procs[i][4] > 20
            if rc is None and not stuck:
                continue
            if stuck:
                p.kill(); p.wait()
            if (rc not in (0, None) or stuck) and cur:
                fcur = int(cur.split()[0])
                crashes.append({"f": fcur, "k": int(cur.split()[1]), "status": "hang" if stuck else "crash",
                                "why": f"rc={rc}"})
                rest = fl[fl.index(fcur) + 1:]
                del procs[i]
                if rest:
                    launch(i, rest)
                continue
            del procs[i]
    results = crashes[:]
    for i in range(a.jobs):
        part = f"{tmp}/part{i}.jsonl"
        if os.path.exists(part):
            results += [json.loads(l) for l in open(part) if l.strip()]
            os.remove(part)
    by_f = {}
    for r in results:
        prev = by_f.get(r["f"])
        rank = {"ok": 1, "skip": 0, "fatal": 3, "mismatch": 3, "hang": 4, "crash": 4}
        if prev is None or rank[r["status"]] > rank[prev["status"]]:
            by_f[r["f"]] = r
    import collections
    c = collections.Counter(r["status"] for r in by_f.values())
    print("functions:", len(by_f), dict(c))
    json.dump(sorted(by_f.values(), key=lambda r: r["f"]), open(a.out, "w"), indent=0)


if __name__ == "__main__":
    main()
