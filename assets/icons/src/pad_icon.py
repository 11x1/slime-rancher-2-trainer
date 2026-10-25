"""Turns a hand-drawn icon into an embeddable one (offline tool, Pillow).

python pad_icon.py <drawing.png> <name> [size]
Writes assets/icons/<name>.png: the drawing cropped to its visible pixels, centred on a
transparent square canvas and reduced by a whole factor (2x, 3x, ... box filter, never a
fractional resize) so it fits the 256 x 256 the other icons use (or `size`, e.g. 512 for a
`<name>_large` copy: the Fabricator details picture of a buildable). A drawing that already
fits is padded unscaled. The input file is not changed.
"""

import math
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.normpath(os.path.join(HERE, ".."))
SIZE = 256


def main():
    global SIZE
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    src, name = sys.argv[1], sys.argv[2]
    if len(sys.argv) == 4:
        SIZE = int(sys.argv[3])
    im = Image.open(src).convert("RGBA")
    box = im.getchannel("A").getbbox()
    if not box:
        sys.exit(f"{src}: fully transparent")
    art = im.crop(box)
    n = max(1, math.ceil(max(art.size) / SIZE))  # whole reduction factor
    side = SIZE * n
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    # Offsets on multiples of n keep the art's pixel grid aligned with the reduction.
    x = (side - art.width) // 2 // n * n
    y = (side - art.height) // 2 // n * n
    canvas.alpha_composite(art, (x, y))
    if n > 1:
        # Box filter on premultiplied colour, so transparent pixels do not darken the edges.
        canvas = canvas.convert("RGBa").reduce(n).convert("RGBA")
    out = os.path.join(ICONS, name + ".png")
    canvas.save(out, optimize=True)
    print(f"wrote {out}: art {art.size} from {im.size}, 1/{n} scale, {SIZE}x{SIZE}")


if __name__ == "__main__":
    main()
