"""Package the Windows build as a folder (and .zip) from the user's own game files.

usage: python -I tools/make_winpkg.py [--data Data.dat] [--save DIR] [--out "build/Joey the Passion"] [--no-zip]

  --data   Data.dat to ship (e.g. build/Data_anime.dat from tools/apply_card_addon.py); default game/Data.dat
  --save   starting save (Region.dat, registry.txt, deck.ydc, "Yu-Gi-Oh! Power of Chaos Common/"); default game/

Layout (copy the folder anywhere writable, e.g. Documents; not Program Files):
  Joey.exe, SDL2.dll, SDL2_ttf.dll       build/win/Joey.exe (make win WICON=...) + the SDL mingw runtimes
  fonts/Arimo.ttf                        bundled font
  game/                                  Joey_Pc.exe, Data.dat, Voice.dat, Voice/, starting save; the game saves
                                         here and writes game/log.txt
The result contains Konami data: it is for your own use, never commit or distribute it."""
import os, shutil, subprocess, sys

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
data = opt("--data", "game/Data.dat")
save = opt("--save", "game")
out = opt("--out", "build/Joey the Passion")
make_zip = "--no-zip" not in args
wlibs = os.environ.get("WLIBS", "work/winlibs")

def clone(src, dst):
    if subprocess.run(["cp", "-cR", src, dst]).returncode: subprocess.run(["cp", "-R", src, dst], check=True)

if not os.path.exists("build/win/Joey.exe"): sys.exit("build/win/Joey.exe missing: run `make -j10 win WICON=icon.ico` first")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(os.path.join(out, "fonts")); os.makedirs(os.path.join(out, "game"))
shutil.copy2("build/win/Joey.exe", out)
shutil.copy2(os.path.join(wlibs, "SDL2-2.30.9/x86_64-w64-mingw32/bin/SDL2.dll"), out)
shutil.copy2(os.path.join(wlibs, "SDL2_ttf-2.22.0/x86_64-w64-mingw32/bin/SDL2_ttf.dll"), out)
shutil.copy2("assets/fonts/Arimo.ttf", os.path.join(out, "fonts"))
shutil.copy2("assets/fonts/OFL-Arimo.txt", os.path.join(out, "fonts"))
G = os.path.join(out, "game")
for n in ("Joey_Pc.exe", "Voice.dat", "Voice"): clone(os.path.join("game", n), os.path.join(G, n))
clone(data, os.path.join(G, "Data.dat"))
for n in ("Region.dat", "registry.txt", "deck.ydc", "Deck.ydc", "Yu-Gi-Oh! Power of Chaos Common"):
    p = os.path.join(save, n)
    if os.path.exists(p): clone(p, os.path.join(G, n))
print("built", out)
if make_zip:
    z = out + ".zip"
    if os.path.exists(z): os.remove(z)
    subprocess.run(["zip", "-qr", "-0", os.path.basename(z), os.path.basename(out)], cwd=os.path.dirname(out) or ".", check=True)
    print("zipped", z)
