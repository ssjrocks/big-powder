#!/usr/bin/env python3
"""Draws Big Powder's icon (pixel art: a pile of powder under a falling stream, in TPT's element colours) as
assets/big-powder.png (256x256) and assets/big-powder.ico (16-256). Deterministic: run it again to remake them."""
import random
from pathlib import Path

from PIL import Image, ImageDraw

N, SCALE = 32, 8
rng = random.Random(1)
SAND, DUST, STNE, WATR = (255, 208, 144), (255, 224, 160), (160, 160, 160), (32, 48, 208)


def tint(c, spread=22):
    d = rng.randint(-spread, spread)
    return tuple(max(0, min(255, v + d + rng.randint(-6, 6))) for v in c)


grid = {}
# the pile: a hill whose height falls off from the middle
for x in range(2, 30):
    h = int(13 - abs(x - 15.5) * 0.95 + rng.choice((0, 0, 1)))
    for y in range(29 - max(h, 1), 29):
        grid[(x, y)] = tint(SAND if (x * 7 + y * 3) % 5 else DUST)
# a little water pooled at the foot, stone under it all
for x in range(2, 30):
    if (x, 28) not in grid or x < 5 or x > 26:
        grid[(x, 28)] = tint(WATR, 10)
    grid[(x, 29)] = tint(STNE, 12)
# the falling stream, thinning out towards the top
for y in range(3, 17):
    for x in (15, 16):
        if rng.random() < 0.35 + y * 0.04:
            grid[(x + rng.choice((-1, 0, 0, 1)), y)] = tint(DUST)

img = Image.new("RGBA", (N * SCALE, N * SCALE), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((0, 0, N * SCALE - 1, N * SCALE - 1), radius=40, fill=(12, 14, 22, 255),
                    outline=(70, 80, 110, 255), width=4)
for (x, y), c in grid.items():
    d.rectangle((x * SCALE, y * SCALE, x * SCALE + SCALE - 1, y * SCALE + SCALE - 1), fill=c + (255,))
# keep the grains inside the rounded corners
mask = Image.new("L", img.size, 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, N * SCALE - 1, N * SCALE - 1), radius=40, fill=255)
img.putalpha(Image.composite(img.getchannel("A"), mask, mask))

out = Path(__file__).resolve().parent.parent / "assets"
out.mkdir(exist_ok=True)
img.save(out / "big-powder.png")
img.save(out / "big-powder.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print(out / "big-powder.png", out / "big-powder.ico")
