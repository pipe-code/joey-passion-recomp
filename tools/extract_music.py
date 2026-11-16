"""Extract the music tracks (stream\\m_*.wav) from an original Voice.dat as loose files the game finds first.
The Spanish repack's Voice.dat lacks them; the English original ("Voz Original/Voice.dat") has all 19.
Usage: python -I tools/extract_music.py <Voice.dat with music> game/Voice/Joey/Spanish/Stream"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcej import entries
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
n = 0
with open(src, "rb") as f:
    for name, off, usize, csize in entries(src):
        if "\\stream\\" in name.lower() and not name.lower().endswith("_dummy.wav"):
            f.seek(off)
            open(os.path.join(out, name.split("\\")[-1]), "wb").write(f.read(csize))
            n += 1
print(n, "tracks ->", out)
