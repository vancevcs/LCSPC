"""Rewrite LCS's tutorial messages to name keyboard keys instead of PSP buttons.

The VCS fork's Tools/vcsgxtkeys.py, on LCS's text. The help lines - "Press ~k~ ~PDSPR~ while running
to start sprinting" - name no button: they carry a TOKEN, which the game resolves through a second
GXT entry, `C<configuration><token>` ("C2PDSPR" reads "~X~"). So the control vocabulary of every
tutorial message is 36 strings per configuration in MAIN, and pointing them at this fork's keys is a
data change.

    python Tools/lcsgxtkeys.py --verify          # rebuild unchanged, prove the writer is faithful
    python Tools/lcsgxtkeys.py --report          # what maps to what, and what is still unmapped
    python Tools/lcsgxtkeys.py                   # write it where the fork's disc patch reads it
    python Tools/lcsgxtkeys.py --iso IN --iso-out OUT     # or bake a patched copy of the disc

The default output is memstick/PSP/LCS/ENGLISH.GXT, which `LCS::PatchDiscRead` stands in for the
disc's own as it is read (LCS opens its files by sector, `disc0:/sce_lbn...`, so there is no name to
redirect). The retail ISO is never touched, and deleting that file undoes everything. The file is
generated, not kept in git: it is the game's own text.

All four configurations (C0..C3) get the same values: the input layer holds the game in C2, and a
player on another would otherwise see PSP buttons again. The replacement is padded back to the
original's length, since the game seeks by sector; trailing zeroes are inert.

THE MAPPING IS HAND-WRITTEN AND MIRRORS kKeyMappings in Core/LCS/LCSInput.cpp. Change a binding
there and change it here; `--report` lists every token the text uses with no mapping.
"""

import argparse
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_IN = os.path.join(os.path.dirname(REPO), "disc", "PSP_GAME", "USRDIR", "TEXT", "ENGLISH.GXT")
DEFAULT_OUT = os.path.join(REPO, "memstick", "PSP", "LCS", "ENGLISH.GXT")

# Token -> what this fork's keyboard does instead, row by row from kKeyMappings. Keys in capitals as
# the controls card names them; a device as a lowercase phrase ("the mouse"), because the sentences
# read "use ... to look around". None leaves a token alone: a line naming a PSP button is wrong, one
# naming the wrong key is worse. Every line that uses each token was read before choosing it.
KEYS = {
    # On foot
    "PDSPR": "LEFT SHIFT",      # sprint
    # Lock on, the R button. Named as right mouse, the aim: with a gun it is free aim from the start,
    # which is how a PC game aims; the middle button is the game's lock-on. Also the sniper's scope
    # and the camera (HELP7, PHOTO1), which right mouse brings up too.
    "PDLT":  "RIGHT MOUSE",
    "PDFW":  "LEFT MOUSE",      # fire, throw, prime a bomb
    "PDCTL": "Q and E",         # change target while locked on
    "PDCWE": "Q and E",         # change weapon (the wheel too)
    "PDLO1": None,              # look around's L - the mouse needs none, see SENTENCES
    "PDLO2": "the mouse",       # look around
    "PDBAK": "C",               # look behind (no line uses it)
    "FREE1": "X",               # free aim, tapped while locked on (HELP42)
    "FREE2": "the mouse",       # aim within free aim
    "TGSUB": "G",               # side missions on / off, a ferry ticket, cancel - 14 lines
    "CVEIW": "V",               # camera mode
    "ANS":   "TAB",             # answer the phone
    "PUCF":  "TAB",             # take a weapon off the ground (the same L button)
    # The sniper's zoom (HELP8) and the photo camera's (PHOTO2), Square and Cross in the scope, where
    # Space and Left Shift do it - and the wheel, which is what a PC player reaches for. Both lines
    # are rewritten to name the wheel once, see SENTENCES.
    "SNZI":  "the mouse wheel",
    "SNZO":  "the mouse wheel",
    "SNSLO": "the mouse",       # fine aim in the scope: the mouse is fine already, see SENTENCES
    # Ammu-Nation, the clothes shop, the races' start: the game's Cross, Circle and d-pad on foot.
    "AMBUY": "LEFT SHIFT",
    "AMEXI": "LEFT MOUSE",
    "AMLEF": "Q",
    "AMRIG": "E",
    "TRSK":  "LEFT SHIFT",      # skip the ferry trip, Cross on foot

    # In a vehicle (configuration 2: R the handbrake, the looks L and the stick)
    "VEACC": "W",
    "VEBRK": "S",
    "VEHB":  "SPACE",
    "VEEE":  "F",               # get in and out, hang up, the train, a boat
    "VEHN":  "H",               # horn, the sirens
    "VECRS": "R and T",         # radio
    "VELL":  "Q",               # look left, and the drive-by to that side
    "VELR":  "E",
    "VELB":  "C",               # look behind (Q + E too)
    "VESTR": "A and D",         # steer; the clothes shop's outfits, where A and D are the stick
    # Lean a bike, the fire truck's cannon up and down: the stick's up and down, the arrows.
    "VEWEI": "the UP and DOWN arrows",
    "VEWEU": "the UP and DOWN arrows",
    # The Rhino's turret and the fire truck's cannon across: the d-pad's left and right, R and T.
    "VEWEL": "R and T",
    "VEWEP": "LEFT MOUSE",      # vehicle fire: drive-bys, the water cannon, an RC car's detonation
}

# Whole lines a token swap cannot save, because two PSP controls are one on a mouse.
SENTENCES = {
    ("MAIN", "HELP40"): "~w~When on foot use~h~ ~k~ ~PDLO2~ ~w~to look around.",
    ("MAIN", "HELP9"): "~w~Move~h~ ~k~ ~SNSLO~ ~w~to make fine adjustments to your aim.",
    ("MAIN", "HELP8"): "~w~Use~h~ ~k~ ~SNZI~ ~w~to ~h~zoom in and out~w~ when targeting.",
    ("MAIN", "PHOTO2"): "~w~While targeting with the camera, use~h~ ~k~ ~SNZI~ ~w~to ~h~zoom~w~ in and out.",
}

HEADER = b"TABL"


def parse(data):
    """[(table name, [(key, value)])], in file order."""
    size = struct.unpack_from("<I", data, 4)[0]
    tables = []
    for off in range(8, 8 + size, 12):
        name = data[off:off + 8].split(b"\0")[0].decode("latin-1")
        tables.append((name, struct.unpack_from("<I", data, off + 8)[0]))

    out = []
    for name, pos in tables:
        p = pos if data[pos:pos + 4] == b"TKEY" else pos + 8
        assert data[p:p + 4] == b"TKEY", (name, data[p:p + 8])
        ksize = struct.unpack_from("<I", data, p + 4)[0]
        kstart = p + 8
        dat = kstart + ksize
        assert data[dat:dat + 4] == b"TDAT", (name, data[dat:dat + 8])
        dstart = dat + 8

        entries = []
        for i in range(ksize // 12):
            o = kstart + i * 12
            doff = struct.unpack_from("<I", data, o)[0]
            key = data[o + 4:o + 12].split(b"\0")[0].decode("latin-1")
            s = dstart + doff
            e = s
            while data[e:e + 2] != b"\0\0":
                e += 2
            # The original offset comes along because TKEY is ordered by KEY and TDAT is not:
            # the two orders are independent, and a rebuild has to preserve both.
            entries.append((key, data[s:e].decode("utf-16-le"), doff))
        out.append((name, entries))
    return out


def build(tables):
    """The inverse of parse.

    Two things here were guesses that --verify caught, and both are the kind a naive rebuild gets
    wrong silently:

    Strings are written once each, NOT pooled by value. The original has 2408 entries in MAIN with
    2408 distinct offsets laid out contiguously, so duplicate values really do get duplicate
    copies; pooling them produced a file 8272 bytes short.

    And TKEY is ordered by KEY while TDAT is not. So the pool is packed by walking the entries in
    their ORIGINAL offset order, and the key table is then written back in its own order with the
    new offsets. Writing the strings in key order instead diverged at the very first entry."""
    bodies = []
    for name, entries in tables:
        pool = bytearray()
        placed = {}
        for i in sorted(range(len(entries)), key=lambda n: entries[n][2]):
            placed[i] = len(pool)
            pool += entries[i][1].encode("utf-16-le") + b"\0\0"
        keys = bytearray()
        for i, (key, value, _) in enumerate(entries):
            keys += struct.pack("<I", placed[i]) + key.encode("latin-1").ljust(8, b"\0")
        body = b"TKEY" + struct.pack("<I", len(keys)) + bytes(keys)
        body += b"TDAT" + struct.pack("<I", len(pool)) + bytes(pool)
        bodies.append((name, body))

    tabl = bytearray()
    for name, _ in bodies:
        tabl += name.encode("latin-1").ljust(8, b"\0") + b"\0\0\0\0"  # offset patched below

    out = bytearray(HEADER + struct.pack("<I", len(tabl)) + bytes(tabl))
    for i, (name, body) in enumerate(bodies):
        while len(out) % 4:
            out += b"\0"
        struct.pack_into("<I", out, 8 + i * 12 + 8, len(out))
        if i:  # every table but the first carries its own name again
            out += name.encode("latin-1").ljust(8, b"\0")
        out += body
    return bytes(out)


# "Use the ~VECRS~" has to lose its article, and that is a rule rather than a list of exceptions.
#
# Every PSP name in this table is a noun phrase that wants one - "the up button", "the analog
# stick" - so the help lines were written with "the" in front of the token. Almost nothing on a
# keyboard is: the replacements are either bare key names or already carry their own article, so
# the same sentences come out as "use the the mouse" and "press the LEFT MOUSE".
#
# Seven lines needed it, in five tables including two that only a debug build ever shows, which is
# the argument for doing it by rule: hand-written fixes would have covered the ones somebody
# happened to read.
def strip_article(value, token):
    # The article, then whatever markup sits between it and the token - "the~h~ ~k~ ~FREE2~" is
    # written without a space, so the whitespace after "the" is optional.
    return re.sub(r"(?i)\bthe(\s*(?:~[a-z]~\s*)*)~" + token + r"~", r"\1~" + token + "~", value)


def apply_keys(tables):
    changed = 0
    for name, entries in tables:
        for i, (key, value, off) in enumerate(entries):
            m = re.match(r"^C([0-3])(.+)$", key) if name == "MAIN" else None
            if m and m.group(2) in KEYS and KEYS[m.group(2)] is not None:
                entries[i] = (key, KEYS[m.group(2)], off)
                changed += 1
            elif (name, key) in SENTENCES:
                entries[i] = (key, SENTENCES[(name, key)], off)
                changed += 1
            else:
                # Any other string that names a control: drop the article in front of it. Done
                # after the two cases above rather than beside them, because a C0* value IS a
                # control name and a rewritten sentence was written already correct.
                fixed = value
                for token in KEYS:
                    if ("~%s~" % token) in fixed:
                        fixed = strip_article(fixed, token)
                if fixed != value:
                    entries[i] = (key, fixed, off)
                    changed += 1
    return changed


def report(tables):
    used = {}
    for name, entries in tables:
        for key, value, _ in entries:
            for tok in re.findall(r"~([A-Z0-9]{2,8})~", value):
                used.setdefault(tok, []).append("%s.%s" % (name, key))
    main_table = next(entries for name, entries in tables if name == "MAIN")
    actions = {k[2:] for k, _, _ in main_table if re.match(r"^C0.+", k)}

    print("%-8s %-14s %s" % ("TOKEN", "KEYBOARD", "USED"))
    for tok in sorted(a for a in actions if a in used):
        mapped = KEYS.get(tok, "-- no entry --")
        print("%-8s %-14s %d place(s)" % (tok, mapped if mapped else "** UNMAPPED **", len(used[tok])))
    missing = sorted(a for a in actions if a in used and not KEYS.get(a))
    print("\n%d of %d used tokens have no keyboard name%s" % (
        len(missing), len([a for a in actions if a in used]),
        (": " + ", ".join(missing)) if missing else ""))


def patch_iso(src, dst, original, patched):
    """Copy the disc and overwrite the GXT where it lies.

    The file is found by searching for its own bytes rather than by parsing ISO9660, because the
    thing that has to be right is the OFFSET the game will read from, and matching the whole file
    proves that directly. Padded back to the original length so nothing after it moves - the disc
    is read by sector, and a shorter file would slide every later sector under the game's feet."""
    needle = original[:64]
    offset = -1
    chunk = 1 << 24
    with open(src, "rb") as f:
        pos, prev = 0, b""
        while offset < 0:
            buf = f.read(chunk)
            if not buf:
                break
            hay = prev + buf
            i = hay.find(needle)
            if i >= 0:
                offset = pos - len(prev) + i
            prev = hay[-len(needle):]
            pos += len(buf)
    if offset < 0:
        sys.exit("could not find ENGLISH.GXT inside %s - is it the same build?" % src)

    with open(src, "rb") as f:
        f.seek(offset)
        if f.read(len(original)) != original:
            sys.exit("found the header at 0x%x but the file after it differs - refusing to write" % offset)

    print("ENGLISH.GXT at 0x%x (sector %d); copying the disc..." % (offset, offset // 2048))
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        f.seek(offset)
        f.write(patched + b"\0" * (len(original) - len(patched)))
    print("wrote %d bytes (+%d of padding) -> %s" % (len(patched), len(original) - len(patched), dst))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", default=DEFAULT_IN, help="the game's ENGLISH.GXT")
    ap.add_argument("--out", default=DEFAULT_OUT, help="where the patched copy goes")
    ap.add_argument("--verify", action="store_true", help="rebuild unchanged and compare, byte for byte")
    ap.add_argument("--report", action="store_true", help="print the mapping and what is missing from it")
    ap.add_argument("--iso", help="disc image to take a patched copy of")
    ap.add_argument("--iso-out", dest="iso_out", help="where the patched copy goes")
    args = ap.parse_args()

    if not os.path.exists(args.src):
        sys.exit("no GXT at %s - point --in at a dump of the disc" % args.src)
    data = open(args.src, "rb").read()
    tables = parse(data)

    if args.verify:
        rebuilt = build(tables)
        if rebuilt == data:
            print("round trip is byte-identical (%d bytes, %d tables)" % (len(data), len(tables)))
        else:
            print("round trip DIFFERS: %d in, %d out" % (len(data), len(rebuilt)), file=sys.stderr)
            for i in range(min(len(data), len(rebuilt))):
                if data[i] != rebuilt[i]:
                    print("first difference at 0x%x" % i, file=sys.stderr)
                    break
            sys.exit(1)
        return

    if args.report:
        report(tables)
        return

    changed = apply_keys(tables)
    out = build(tables)
    if len(out) > len(data):
        sys.exit("patched GXT is %d bytes, larger than the original's %d - it cannot be written "
                 "back into the disc's own space. Shorten the longest replacements in KEYS."
                 % (len(out), len(data)))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(out)
    print("%d strings rewritten -> %s (%d bytes, was %d)" % (changed, args.out, len(out), len(data)))

    if args.iso:
        if not args.iso_out:
            sys.exit("--iso needs --iso-out; this does not patch a disc image in place")
        patch_iso(args.iso, args.iso_out, data, out)


if __name__ == "__main__":
    main()
