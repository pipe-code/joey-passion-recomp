"""Write a copy of Data.dat with card art replaced from an addon folder (e.g. the "AIO anime style cards" pack).

usage: python -I tools/apply_card_addon.py game/Data.dat ADDON_DIR OUT.dat

ADDON_DIR holds card/*.bmp (200x290) and mini/*.bmp (50x72), named like the archive entries (case-insensitive).
Card images are stored uncompressed in Data.dat, so each replacement is written in place at the entry's offset
and must have the original size (zero padding at the end is trimmed); anything else is skipped and reported. Extra addon files (cards this
game does not have) are ignored."""
import os, shutil, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcej import entries

src, addon, out = sys.argv[1:4]
files = {}
for sub in ("card", "mini"):
    d = os.path.join(addon, sub)
    for n in os.listdir(d) if os.path.isdir(d) else []:
        if n.lower().endswith(".bmp"): files[(sub + "\\" + n).lower()] = os.path.join(d, n)

shutil.copyfile(src, out)
done = skipped = 0
with open(out, "r+b") as f:
    for name, off, usize, csize in entries(src):
        p = files.get(name.lower())
        if not p: continue
        data = open(p, "rb").read()
        extra = len(data) - csize
        if usize == csize and 0 < extra <= 4 and not data[csize:].strip(b"\0"):
            # some originals have no trailing row padding: drop the addon's zero padding, fix bfSize/biSizeImage
            data = bytearray(data[:csize])
            struct.pack_into("<I", data, 2, csize)
            struct.pack_into("<I", data, 34, csize - struct.unpack_from("<I", data, 10)[0])
            data = bytes(data)
        if usize != csize or len(data) != csize or data[:2] != b"BM":
            print("skip", name, len(data), usize, csize); skipped += 1; continue
        f.seek(off); f.write(data); done += 1
print(f"replaced {done} images, skipped {skipped}, addon files unused: {len(files) - done - skipped}")
