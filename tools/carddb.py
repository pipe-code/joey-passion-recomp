"""Card database / dialog text decoder for Yu-Gi-Oh! Power of Chaos (Joey the Passion).

Input: the bin#/ directory unpacked from Data.dat (tools/kcej.py ... unpack), plus card/list_card.txt.
usage: carddb.py <unpacked root (contains bin#/ and card/)> <outdir>
writes <outdir>/cards.json and <outdir>/dialog.json

File layouts (all little-endian; N = 1115 card slots, slot 0 = dummy/card back):
  CARD_ID.bin      u16[N]     Konami internal card id (the id used in .ydc decks and list_card.txt "[nnnn]")
  CARD_IntID.bin   u16[2048]  reverse map: internal id -> slot (0 = unused; ids >= 2048 (tokens etc.) absent)
  CARD_Prop.bin    u32[N]     packed properties, see decode_prop()
  CARD_Pack.bin    u16[N]     bitmask of which PoC title contains the card (1=Yugi,2=Kaiba,4=Joey; inferred)
  CARD_Pass.bin    u32[N]     8-digit password as BCD, stored so that bytes read in file order = digits
  CARD_Name<lang>  char[N][64] NUL-padded names (cp1252, jpn = cp932)
  CARD_Indx<lang>  u32[2048]  byte offset of slot's description in CARD_Desc<lang>
  CARD_Desc<lang>  NUL-terminated strings, concatenated
  CARD_Sort<lang>  u16[2048]  slot numbers in alphabetical (per-language) order
  DLG_Indx<lang>   u32[M]     offsets into DLG_Text<lang>; DLG_Text = NUL-terminated strings, "@n" = colour code
"""
import json, os, re, struct, sys

LANGS = {"eng": "cp1252", "fra": "cp1252", "ger": "cp1252", "ita": "cp1252", "spa": "cp1252", "jpn": "cp932"}
N = 1115
ATTRIBUTES = {0: None, 1: "LIGHT", 2: "DARK", 3: "WATER", 4: "FIRE", 5: "EARTH", 6: "WIND"}
TYPES = ["?", "Dragon", "Zombie", "Fiend", "Pyro", "Sea Serpent", "Rock", "Machine", "Fish", "Dinosaur",
         "Insect", "Beast", "Beast-Warrior", "Plant", "Aqua", "Warrior", "Winged Beast", "Fairy",
         "Spellcaster", "Thunder", "Reptile", "Trap", "Spell", "?23", "Divine-Beast"]
MONSTER_KIND = ["Normal", "Effect", "Fusion", "Ritual"]

def decode_prop(p):
    """bits 0-8 DEF/10, 9-17 ATK/10, 18-19 subkind, 20-24 type, 25-28 level, 29-31 attribute.
    For spells/traps bit 17 (ATK high bit) is a subtype flag instead of ATK."""
    deff, atk = p & 0x1FF, (p >> 9) & 0x1FF
    sub, typ, lvl, attr = (p >> 18) & 3, (p >> 20) & 0x1F, (p >> 25) & 0xF, p >> 29
    flag = (atk >> 8) & 1
    d = {"prop_raw": f"0x{p:08x}"}
    if typ == 21:
        d.update(card_type="Trap", icon={(0, 0): "Normal", (0, 1): "Counter", (2, 0): "Continuous"}.get((sub, flag), f"?{sub}/{flag}"))
    elif typ == 23:  # only "Glory of the King's Hand" (JP-only, unused); meaning unknown
        d.update(card_type="Spell?", icon="?type23")
    elif typ == 22:
        d.update(card_type="Spell", icon={(0, 0): "Normal", (1, 0): "Field", (1, 1): "Equip", (2, 0): "Continuous",
                                         (2, 1): "Quick-Play", (3, 0): "Ritual"}.get((sub, flag), f"?{typ}/{sub}/{flag}"))
    else:
        d.update(card_type="Monster", kind=MONSTER_KIND[sub], type=TYPES[typ] if typ < len(TYPES) else f"?{typ}",
                 attribute=ATTRIBUTES.get(attr, f"?{attr}"), level=lvl, atk=atk * 10, defense=deff * 10)
    return d

def cstr(buf, off, enc):
    return buf[off:buf.index(b"\0", off)].decode(enc, "replace")

def load(root):
    b = lambda n: open(os.path.join(root, "bin#", n + ".bin"), "rb").read()
    cid = struct.unpack(f"<{N}H", b("card_id"))
    prop = struct.unpack(f"<{N}I", b("card_prop"))
    pack = struct.unpack(f"<{N}H", b("card_pack"))
    pas = b("card_pass")
    names, descs = {}, {}
    for lg, enc in LANGS.items():
        nm = b("card_name" + lg); ds = b("card_desc" + lg); ix = struct.unpack("<2048I", b("card_indx" + lg))
        names[lg] = [nm[i * 64:i * 64 + 64].split(b"\0")[0].decode(enc, "replace") for i in range(N)]
        descs[lg] = [cstr(ds, ix[i], enc) for i in range(N)]
    images = {}
    lc = os.path.join(root, "card", "list_card.txt")
    if os.path.exists(lc):
        lines = open(lc, encoding="cp932").read().splitlines()
        for k, l in enumerate(lines):
            m = re.match(r"//\t(\d{4}):\[(\d{4})\]", l)
            if m: images[int(m.group(1))] = lines[k + 1].strip()
    cards = []
    for i in range(N):
        c = {"slot": i, "card_id": cid[i], "password": pas[i * 4:i * 4 + 4].hex() if pas[i * 4:i * 4 + 4] != b"\xff" * 4 else None,
             "name": names["eng"][i], "desc": descs["eng"][i]}
        c.update(decode_prop(prop[i]))
        c["pack_mask"] = pack[i]
        if i in images: c["image"] = images[i]
        c["i18n"] = {lg: {"name": names[lg][i], "desc": descs[lg][i]} for lg in LANGS if lg != "eng"}
        cards.append(c)
    dialog = {}
    for lg, enc in LANGS.items():
        t = b("dlg_text" + lg); ix = b("dlg_indx" + lg)
        dialog[lg] = [cstr(t, o, enc) for o in struct.unpack(f"<{len(ix) // 4}I", ix)]
    return cards, dialog

if __name__ == "__main__":
    root, out = sys.argv[1], sys.argv[2]
    cards, dialog = load(root)
    os.makedirs(out, exist_ok=True)
    json.dump(cards, open(os.path.join(out, "cards.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    json.dump(dialog, open(os.path.join(out, "dialog.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(len(cards), "cards,", len(dialog["eng"]), "dialog strings per language")
