"""Lister/extractor for Konami KCEJYUGI .dat archives (Yu-Gi-Oh! Power of Chaos)."""
import struct, sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss import lzss

def swap(b): return bytes(((c >> 4) | (c << 4)) & 0xFF for c in b)

def entries(path):
    with open(path, "rb") as f:
        hdr = f.read(12)
        assert hdr[:8] == b"KCEJYUGI", hdr
        n = struct.unpack("<I", hdr[8:])[0]
        raw = f.read(n * 0x10C)
    for i in range(n):
        e = raw[i * 0x10C:(i + 1) * 0x10C]
        name = swap(e[:0x100]).split(b"\0")[0].decode("latin1")
        off, usize, csize = struct.unpack("<III", e[0x100:])
        yield name, off, usize, csize

if __name__ == "__main__":
    path, mode = sys.argv[1], sys.argv[2]
    ents = list(entries(path))
    if mode == "list":
        for e in ents: print(*e, sep="\t")
    elif mode == "stats":
        ext = collections.Counter(); sz = collections.Counter(); comp = 0
        for n, o, u, c in ents:
            x = os.path.splitext(n)[1].lower(); ext[x] += 1; sz[x] += u; comp += (u != c)
        print("entries", len(ents), "compressed", comp)
        for x, k in ext.most_common(): print(x or "(none)", k, f"{sz[x]/1e6:.1f}MB")
        dirs = collections.Counter(n.split("\\")[0] + "\\" + (n.split("\\")[1] if n.count("\\") > 1 else "") for n, *_ in ents)
        for d, k in dirs.most_common(40): print(" ", d, k)
    elif mode in ("extract", "unpack"):
        # extract: raw stored bytes (.packed suffix if compressed); unpack: LZSS-decoded, original name
        out, pat = sys.argv[3], (sys.argv[4] if len(sys.argv) > 4 else "")
        with open(path, "rb") as f:
            for n, o, u, c in ents:
                if pat not in n: continue
                f.seek(o); data = f.read(c)
                packed = u != c
                if mode == "unpack" and packed:
                    data = lzss(data); assert len(data) == u, (n, len(data), u); packed = False
                dst = os.path.join(out, n.replace("\\", "/") + (".packed" if packed else ""))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, "wb").write(data)
