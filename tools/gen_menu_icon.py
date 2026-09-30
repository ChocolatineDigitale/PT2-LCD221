#!/usr/bin/env python3
"""Draws the 25x25 menu icon (resources/images/menu_icon.png): the watch face's
black case and white LCD showing "10:58", built from the same segment outlines
(src/c/segments.h) as the watch face itself. Needs Pillow (pip install pillow).

    python3 tools/gen_menu_icon.py
"""
import re
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SCALE = 16  # supersampling for smooth edges
SIZE = 25

# segment letter -> polygon in 0..1000 shape units (A..G = bits 0..6)
header = (ROOT / "src/c/segments.h").read_text()
POLYS = {}
for seg in "ABCDEFG":
    nums = re.search(r"SEG_POLY_%s\[\] = \{([^}]*)\}" % seg, header).group(1)
    v = [int(n) for n in nums.split(",")]
    POLYS[seg] = list(zip(v[0::2], v[1::2]))
DIGITS = {"0": "ABCDEF", "1": "BC", "5": "AFGCD", "8": "ABCDEFG"}


def draw_digit(d, x, y, w, h, ch):
    for seg in DIGITS[ch]:
        d.polygon([((x + px / 1000 * w) * SCALE, (y + py / 1000 * h) * SCALE) for px, py in POLYS[seg]],
                  fill=(0, 0, 0, 255))


big = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
d = ImageDraw.Draw(big)
d.rounded_rectangle([0, 0, SIZE * SCALE - 1, SIZE * SCALE - 1], radius=3 * SCALE, fill=(0, 0, 0, 255))
d.rectangle([1 * SCALE, 4 * SCALE, 24 * SCALE - 1, 21 * SCALE - 1], fill=(255, 255, 255, 255))
for x, ch in ((2, "1"), (7, "0"), (14, "5"), (19, "8")):
    draw_digit(d, x, 8, 4, 9, ch)
for cy in (10, 15):  # colon
    d.rectangle([12 * SCALE, cy * SCALE, 13 * SCALE - 1, (cy + 1) * SCALE - 1], fill=(0, 0, 0, 255))

out = ROOT / "resources/images/menu_icon.png"
big.resize((SIZE, SIZE), Image.LANCZOS).save(out)
print("wrote", out)
