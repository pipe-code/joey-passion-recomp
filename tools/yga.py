"""Decoder for Power of Chaos .yga sprites -> PNG (RGBA).

Layout (little-endian):
  0x00 char[4]  "yga\\0"
  0x04 u32      width
  0x08 u32      height
  0x0C u32      format/version (always 1)
  0x10 u32      decoded size = width*height*4
  0x14 u32      payload size = filesize - 0x18
  0x18 ...      LZSS payload (same Okumura LZSS as the KCEJYUGI archive, but window pre-filled with 0x00)
Decoded pixels: 32bpp BGRA, top-down rows, alpha straight (not premultiplied).

usage: yga.py <in.yga|dir> [out.png|outdir]   (dir mode converts recursively)
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss import lzss

def decode(data):
    magic, w, h, fmt, usize, csize = struct.unpack_from("<4s5I", data)
    if magic != b"yga\0": raise ValueError("not a yga file")
    if fmt != 1: raise ValueError(f"unknown yga format {fmt}")
    px = lzss(data[0x18:0x18 + csize], fill=0)
    if len(px) != usize or usize != w * h * 4:
        raise ValueError(f"size mismatch {len(px)} vs {usize} ({w}x{h})")
    return w, h, px  # BGRA

def to_image(data):
    from PIL import Image
    w, h, px = decode(data)
    return Image.frombuffer("RGBA", (w, h), px, "raw", "BGRA", 0, 1)

if __name__ == "__main__":
    src = sys.argv[1]; dst = sys.argv[2] if len(sys.argv) > 2 else None
    if os.path.isdir(src):
        dst = dst or src; n = 0
        for root, _, files in os.walk(src):
            for f in files:
                if not f.lower().endswith(".yga"): continue
                p = os.path.join(root, f); o = os.path.join(dst, os.path.relpath(p, src))[:-4] + ".png"
                os.makedirs(os.path.dirname(o), exist_ok=True)
                to_image(open(p, "rb").read()).save(o); n += 1
        print(n, "converted")
    else:
        to_image(open(src, "rb").read()).save(dst or os.path.splitext(src)[0] + ".png")
