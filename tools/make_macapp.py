"""Package the macOS build as a self-contained Joey.app from the user's own game files.

usage: python -I tools/make_macapp.py [--data Data.dat] [--icon icon.ico|png] [--save DIR] [--out build/Joey.app]

  --data   Data.dat to ship (e.g. build/Data_anime.dat from tools/apply_card_addon.py); default game/Data.dat
  --icon   app icon (largest size is used); default: the exe's own 32x32 icon
  --save   starting save copied on first launch (Region.dat, registry.txt, deck.ydc,
           "Yu-Gi-Oh! Power of Chaos Common/"); default: game/

Layout:
  Contents/MacOS/joey                    recompiled game + Win32 layer (build/all/joey)
  Contents/Frameworks/*.dylib            SDL2 (sdl2-compat) + SDL3 + SDL2_ttf + freetype/harfbuzz/... (relinked)
  Contents/Resources/fonts/Arimo.ttf     bundled font
  Contents/Resources/game/               Joey_Pc.exe, Data.dat, Voice.dat, Voice/   (read-only)
  Contents/Resources/game/save/          starting save
At launch (host/main.c) the game runs from ~/Library/Application Support/Joey the Passion, where the save lives.
The result contains Konami data: it is for your own use, never commit or distribute it."""
import io, os, plistlib, shutil, struct, subprocess, sys
from PIL import Image

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
data = opt("--data", "game/Data.dat")
icon = opt("--icon", None)
save = opt("--save", "game")
app = opt("--out", "build/Joey.app")
exe = "build/all/joey"

def run(*cmd): subprocess.run(cmd, check=True)
def clone(src, dst):
    """APFS clone (no extra disk space) when possible."""
    if subprocess.run(["cp", "-cR", src, dst]).returncode: run("cp", "-R", src, dst)

shutil.rmtree(app, ignore_errors=True)
C = os.path.join(app, "Contents")
for d in ("MacOS", "Frameworks", "Resources/fonts", "Resources/game/save"):
    os.makedirs(os.path.join(C, d), exist_ok=True)
shutil.copy2(exe, os.path.join(C, "MacOS/joey"))
shutil.copy2("assets/fonts/Arimo.ttf", os.path.join(C, "Resources/fonts/Arimo.ttf"))
shutil.copy2("assets/fonts/OFL-Arimo.txt", os.path.join(C, "Resources/fonts/OFL-Arimo.txt"))

# ---- game data (read-only) and starting save
G = os.path.join(C, "Resources/game")
for n in ("Joey_Pc.exe", "Voice.dat", "Voice"): clone(os.path.join("game", n), os.path.join(G, n))
clone(data, os.path.join(G, "Data.dat"))
for n in ("Region.dat", "registry.txt", "deck.ydc", "Deck.ydc", "Yu-Gi-Oh! Power of Chaos Common"):
    p = os.path.join(save, n)
    if os.path.exists(p): clone(p, os.path.join(G, "save", n))

# ---- dylibs: copy every non-system dependency into Frameworks and point the references there
F = os.path.join(C, "Frameworks")
def deps(path):
    out = subprocess.run(["otool", "-L", path], capture_output=True, text=True, check=True).stdout.splitlines()[1:]
    return [l.split(" (")[0].strip() for l in out]
def system(p): return p.startswith(("/usr/lib/", "/System/"))
seen = set()
def bundle(path, origdir):
    """Copy every non-system dependency of `path` into Frameworks (recursively) and point it at @rpath."""
    for d in deps(path):
        if system(d): continue
        name = os.path.basename(d)
        if d.startswith("@"):                     # @rpath/@loader_path: next to the original library
            if name == os.path.basename(path) or name in seen: continue
            src = os.path.join(origdir, name)
            if not os.path.exists(src): print("warning: cannot resolve", d, "for", path); continue
        else:
            src = d
        if name not in seen:
            seen.add(name)
            real = os.path.realpath(src); dst = os.path.join(F, name)
            shutil.copy2(real, dst); os.chmod(dst, 0o755)
            run("install_name_tool", "-id", "@rpath/" + name, dst)
            bundle(dst, os.path.dirname(real))
        if not d.startswith("@rpath/"): run("install_name_tool", "-change", d, "@rpath/" + name, path)
bundle(os.path.join(C, "MacOS/joey"), "")
# sdl2-compat loads SDL3 at runtime from @loader_path/libSDL3.dylib
if "libSDL2-2.0.0.dylib" in seen:
    sdl3 = subprocess.run(["brew", "--prefix", "sdl3"], capture_output=True, text=True).stdout.strip() or "/opt/homebrew/opt/sdl3"
    real = os.path.realpath(os.path.join(sdl3, "lib/libSDL3.0.dylib")); dst = os.path.join(F, "libSDL3.dylib")
    shutil.copy2(real, dst); os.chmod(dst, 0o755); seen.add("libSDL3.dylib")
    run("install_name_tool", "-id", "@rpath/libSDL3.dylib", dst)
    bundle(dst, os.path.dirname(real))
run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", os.path.join(C, "MacOS/joey"))

# ---- icon
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
iconset = os.path.join(os.path.dirname(app), "joey.iconset")
shutil.rmtree(iconset, ignore_errors=True); os.makedirs(iconset)
for s in (16, 32, 128, 256, 512):
    im.resize((s, s), resample).save(os.path.join(iconset, f"icon_{s}x{s}.png"))
    im.resize((2 * s, 2 * s), resample).save(os.path.join(iconset, f"icon_{s}x{s}@2x.png"))
run("iconutil", "-c", "icns", iconset, "-o", os.path.join(C, "Resources/joey.icns"))
shutil.rmtree(iconset)

plistlib.dump({
    "CFBundleName": "Joey the Passion", "CFBundleDisplayName": "Joey the Passion",
    "CFBundleIdentifier": "io.github.joey-the-passion-port", "CFBundleExecutable": "joey",
    "CFBundleIconFile": "joey", "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1", "LSMinimumSystemVersion": "12.0", "NSHighResolutionCapable": True,
    "LSApplicationCategoryType": "public.app-category.card-games",
}, open(os.path.join(C, "Info.plist"), "wb"))

run("codesign", "--force", "--deep", "--sign", "-", app)
print("built", app)
