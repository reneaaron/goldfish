"""Draw the paper SeedQR card used in scenes.py: a printed grid with the QR
squares filled in by hand, encoding SeedSigner's public 12-word test vector
(fingerprint 0948d268, the seed the device shows in those scenes).

    uvx --with pillow --with numpy python make_seedqr_card.py <dir with Inter-*.ttf>
"""
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
F = sys.argv[1]
SRC = os.path.join(HERE, "..", "..", "..", "firmware", "sim", "samples", "vector4_standard_12word.png")

# Read the 25x25 module grid from the test vector image.
src = np.array(Image.open(SRC).convert("L"))
dark = src < 128
ys, xs = np.where(dark)
x0, x1, y0 = xs.min(), xs.max(), ys.min()
N = 25
cell = (x1 - x0 + 1) / N
mods = np.array([[dark[int(y0 + (r + .5) * cell), int(x0 + (c + .5) * cell)] for c in range(N)] for r in range(N)])
assert mods[:7, :7].sum() > 20, "finder pattern not found"

S = 4                                   # supersample for crisp lines
W, H = 1712 * S, 1080 * S               # 85.6 x 54 mm card at 200 px/cm
im = Image.new("RGB", (W, H), (246, 243, 236))
d = ImageDraw.Draw(im)
font = lambda name, s: ImageFont.truetype(os.path.join(F, name), s * S)
gray, faint, ink = (150, 148, 142), (205, 202, 195), (28, 34, 52)

# left: printed fields
d.text((70 * S, 70 * S), "SEED QR", font=font("Inter-SemiBold.ttf", 46), fill=(60, 60, 60))
d.text((70 * S, 132 * S), "12 words", font=font("Inter-Medium.ttf", 30), fill=gray)
for i, label in enumerate(["Fingerprint", "Date", "Notes"]):
    y = (330 + i * 190) * S
    d.text((70 * S, y), label, font=font("Inter-Medium.ttf", 26), fill=gray)
    d.line([(70 * S, y + 110 * S), (700 * S, y + 110 * S)], fill=faint, width=3 * S)

# right: grid with labels, squares filled in by hand
g = 34 * S
gx, gy = W - 70 * S - N * g, (H - N * g) // 2 + 20 * S
lab = font("Inter-Medium.ttf", 17)
for i in range(N):
    d.text((gx + i * g + g // 2, gy - 22 * S), chr(65 + i), font=lab, fill=gray, anchor="mm")
    d.text((gx - 24 * S, gy + i * g + g // 2), str(i + 1), font=lab, fill=gray, anchor="mm")
for i in range(N + 1):
    w = 2 * S if i % 5 else 3 * S
    d.line([(gx + i * g, gy), (gx + i * g, gy + N * g)], fill=faint, width=w)
    d.line([(gx, gy + i * g), (gx + N * g, gy + i * g)], fill=faint, width=w)

rnd = random.Random(42)
for r in range(N):
    for c in range(N):
        if not mods[r, c]:
            continue
        x, y = gx + c * g, gy + r * g
        j = lambda: rnd.randint(0, 2) * S
        poly = [(x + j(), y + j()), (x + g - j(), y + j()), (x + g - j(), y + g - j()), (x + j(), y + g - j())]
        shade = rnd.randint(0, 18)
        d.polygon(poly, fill=(ink[0] + shade, ink[1] + shade, ink[2] + shade))

# fingerprint written in, slanted like handwriting
hw = Image.new("RGBA", (900 * S, 160 * S), (0, 0, 0, 0))
ImageDraw.Draw(hw).text((0, 0), "0948 d268", font=font("Inter-Regular.ttf", 64), fill=ink + (235,))
hw = hw.transform(hw.size, Image.AFFINE, (1, 0.18, 0, 0, 1, 0), resample=Image.BICUBIC).rotate(2, resample=Image.BICUBIC)
im.paste(hw, (90 * S, 368 * S), hw)

im = im.resize((W // S, H // S), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.4))
arr = np.array(im).astype(np.int16)
arr += np.random.default_rng(1).integers(-4, 5, arr.shape[:2])[..., None]   # paper fibre
Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).save(os.path.join(HERE, "seedqr_card.png"))
print("ok", int(mods.sum()), "modules")
