"""Build custom LiveArea art from the user's own files into build/livearea/sce_sys (gitignored).

usage: python -I tools/make_livearea.py game/Joey_Pc.exe BACKGROUND.jpg [build/livearea] [--icon ICON.ico|png]

- icon0.png (128x128): --icon if given (largest size, downscaled), else the game's own MAIN icon (32x32) scaled 4x
  nearest-neighbour.
- bg.png (840x500): BACKGROUND center-cropped to the LiveArea aspect.
- startup.png (280x158): the same picture with the title.
All saved as 8-bit palette PNGs, which is what the LiveArea expects. The Makefile packs these instead of the
generic vita/sce_sys art when build/livearea exists."""
import io, os, shutil, struct, sys
import pefile
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

args = sys.argv[1:]
icon_path = None
if "--icon" in args:
    i = args.index("--icon"); icon_path = args[i + 1]; del args[i:i + 2]
exe, bgpath = args[0], args[1]
out = os.path.join(args[2] if len(args) > 2 else "build/livearea", "sce_sys")
os.makedirs(os.path.join(out, "livearea/contents"), exist_ok=True)

def main_icon(path):
    pe = pefile.PE(path)
    icons, groups = {}, {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for n in t.directory.entries:
            for l in n.directory.entries:
                d = pe.get_data(l.data.struct.OffsetToData, l.data.struct.Size)
                if t.id == 3: icons[n.id] = d
                if t.id == 14: groups[str(n.name) if n.name else n.id] = d
    g = groups.get("MAIN") or next(iter(groups.values()))
    ents = [struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i) for i in range(struct.unpack_from("<H", g, 4)[0])]
    w, h, c, r, pl, bc, sz, iid = max(ents, key=lambda e: (e[0] or 256, e[5]))
    d = icons[iid]
    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w, h, c, r, pl, bc, len(d), 22) + d
    return Image.open(io.BytesIO(ico)).convert("RGBA")

def cover(im, w, h):
    sw, sh = im.size
    scale = max(w / sw, h / sh)
    im = im.resize((round(sw * scale), round(sh * scale)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))

def save_pal(im, path):
    im.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)

def font(size, names=("Impact.ttf", "Arial Black.ttf", "Arial Bold.ttf")):
    for n in names:
        for d in ("/System/Library/Fonts/Supplemental", "/Library/Fonts", "/usr/share/fonts/truetype/msttcorefonts"):
            p = os.path.join(d, n)
            if os.path.exists(p): return ImageFont.truetype(p, size)
    return ImageFont.load_default()

def title(im, lines):
    """Gold title with a dark outline + soft shadow, centered."""
    d = ImageDraw.Draw(im)
    total = sum(f.size + 4 for _, f in lines)
    y = (im.height - total) // 2
    shadow = Image.new("RGBA", im.size, (0, 0, 0, 0)); sd = ImageDraw.Draw(shadow)
    pos = []
    for text, f in lines:
        w = d.textlength(text, font=f)
        pos.append(((im.width - w) / 2, y, text, f)); y += f.size + 4
    for x, y, t, f in pos: sd.text((x + 3, y + 4), t, font=f, fill=(0, 0, 0, 200))
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(4)))
    for x, y, t, f in pos:
        d.text((x, y), t, font=f, fill=(255, 205, 60, 255), stroke_width=max(2, f.size // 14), stroke_fill=(60, 20, 0, 255))

if icon_path:
    icon = Image.open(icon_path)
    if icon.format == "ICO": icon.size = max(icon.info.get("sizes", {icon.size}))
    icon = icon.convert("RGBA").resize((128, 128), Image.LANCZOS)
else:
    icon = main_icon(exe).resize((128, 128), Image.NEAREST)
save_pal(Image.alpha_composite(Image.new("RGBA", icon.size, (0, 0, 0, 255)), icon), os.path.join(out, "icon0.png"))

bg = Image.open(bgpath).convert("RGB")
save_pal(cover(bg, 840, 500), os.path.join(out, "livearea/contents/bg.png"))

gate = ImageEnhance.Brightness(cover(bg, 280, 158)).enhance(0.6).convert("RGBA")
title(gate, [("JOEY THE PASSION", font(30)), ("Yu-Gi-Oh! Power of Chaos", font(15, ("Arial Bold.ttf",)))])
save_pal(gate, os.path.join(out, "livearea/contents/startup.png"))

shutil.copyfile("vita/sce_sys/livearea/contents/template.xml", os.path.join(out, "livearea/contents/template.xml"))
print("wrote", out)
