"""Stage the generated Android resources in build/android (called by `make android`).

usage: python -I tools/make_android_res.py [--icon icon.ico|png]

  build/android/assets/fonts/Arimo.ttf      bundled font (read through SDL_RWFromFile from the APK)
  build/android/res/mipmap-*/ic_launcher.png launcher icon: --icon (largest size is used), else the exe's own icon
The icon comes from your own files: build/ is never committed."""
import io, os, shutil, struct, sys
from PIL import Image

args = sys.argv[1:]
icon = args[args.index("--icon") + 1] if "--icon" in args else None
A = "build/android"

os.makedirs(f"{A}/assets/fonts", exist_ok=True)
shutil.copy2("assets/fonts/Arimo.ttf", f"{A}/assets/fonts/Arimo.ttf")

def load_icon():
    if icon:
        im = Image.open(icon)
        if im.format == "ICO": im.size = max(im.info.get("sizes", {im.size}))
        return im.convert("RGBA"), Image.LANCZOS
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pefile
    pe = pefile.PE("game/Joey_Pc.exe"); icons, groups = {}, {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for n in t.directory.entries:
            for l in n.directory.entries:
                d = pe.get_data(l.data.struct.OffsetToData, l.data.struct.Size)
                if t.id == 3: icons[n.id] = d
                if t.id == 14: groups[str(n.name) if n.name else n.id] = d
    g = groups.get("MAIN") or next(iter(groups.values()))
    e = max((struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i) for i in range(struct.unpack_from("<H", g, 4)[0])),
            key=lambda e: (e[0] or 256, e[5]))
    d = icons[e[7]]
    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", *e[:6], len(d), 22) + d
    return Image.open(io.BytesIO(ico)).convert("RGBA"), Image.NEAREST

im, resample = load_icon()
for dpi, s in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
    d = f"{A}/res/mipmap-{dpi}"; os.makedirs(d, exist_ok=True)
    im.resize((s, s), resample).save(f"{d}/ic_launcher.png")
print("staged", A)
