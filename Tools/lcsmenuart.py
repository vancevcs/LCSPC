"""Make the LCS front end's backdrop from the game's own pause-menu texture.

LCS's pause menu draws one 1024x1024 texture stretched over the whole PSP screen: the Brooklyn
bridge in silhouette against a dark blue sky, and below row 848 a plain strip that the game writes
its button hints on. The menu this fork puts up keeps the art and draws its own hint bar, so only
the art is kept:

    python Tools/lcsmenuart.py --source ../texturereplacements/Fronted/Background.png

The source is the upscaled copy of that texture in the HD UI pack (the PSP original is the same
picture at a lower resolution, and works too). Writes assets/lcs/background.png, which ships with
the build and is read through g_VFS at runtime, beside the menu's sounds and font.

The strip's colours are printed rather than shipped, because the menu draws its bar in them:
kBarColor and kBarEdgeColor in UI/LCSMenuScreen.cpp.
"""

import argparse
import os
import sys
from collections import Counter

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT_DIR = os.path.join(REPO, "assets", "lcs")
DEFAULT_SOURCE = os.path.join(os.path.dirname(REPO), "texturereplacements", "Fronted", "Background.png")

# Where the hint strip starts, as a fraction of the texture's height: row 848 of 1024, a lighter
# edge two rows deep and then a flat near-black. A fraction so a smaller original crops the same.
STRIP_TOP = 848.0 / 1024.0


def common(img, box):
    return Counter(img.crop(box).getdata()).most_common(1)[0][0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", default=DEFAULT_SOURCE, help="the pause menu's Background texture")
    ap.add_argument("--out", default=OUT_DIR, help="where to write background.png")
    args = ap.parse_args()

    if not os.path.exists(args.source):
        sys.exit("no texture at %s - point --source at the pause menu's background" % args.source)

    img = Image.open(args.source).convert("RGB")
    w, h = img.size
    top = int(round(h * STRIP_TOP))
    edge = common(img, (0, top, w, top + max(1, h // 512)))
    strip = common(img, (0, top + h // 64, w, h))
    print("strip edge #%02X%02X%02X, strip #%02X%02X%02X" % (edge + strip))

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "background.png")
    img.crop((0, 0, w, top)).save(path, optimize=True)
    print("%s -> %s (%dx%d)" % (os.path.basename(args.source), path, w, top))


if __name__ == "__main__":
    main()
