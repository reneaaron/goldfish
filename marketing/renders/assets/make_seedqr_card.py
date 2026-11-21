"""Draw the paper SeedQR card used in scenes.py: the real wallet card from
cards/index.html (Wallet card, "For a print shop"), screenshotted in headless
Chrome, with the 21x21 Compact SeedQR squares and the fingerprint filled in
by hand. It encodes SeedSigner's public 12-word test vector (fingerprint
0948d268, the seed the device shows in those scenes).

    uv run --with playwright --with pillow --with numpy python make_seedqr_card.py <dir with Inter-Regular.ttf>

Uses the system Chrome (/usr/bin/google-chrome), no Playwright browser download.
"""
import io
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..", "..")
F = sys.argv[1]
SRC = os.path.join(ROOT, "firmware", "sim", "samples", "vector4_compact_12word.png")
CARDS = os.path.abspath(os.path.join(ROOT, "cards", "index.html"))

# Read the 21x21 module grid from the test vector image.
src = np.array(Image.open(SRC).convert("L"))
dark = src < 128
ys, xs = np.where(dark)
x0, x1, y0 = xs.min(), xs.max(), ys.min()
N = 21
cell = (x1 - x0 + 1) / N
mods = np.array([[dark[int(y0 + (r + .5) * cell), int(x0 + (c + .5) * cell)] for c in range(N)] for r in range(N)])
assert mods[:7, :7].sum() > 20, "finder pattern not found"

# Screenshot the blank card and measure where its grid and fingerprint boxes are.
SCALE = 8
with sync_playwright() as p:
    b = p.chromium.launch(executable_path="/usr/bin/google-chrome")
    pg = b.new_page(device_scale_factor=SCALE)
    pg.goto("file://" + CARDS)
    pg.check("input[value=card]")
    pg.check("#plain")
    card = pg.locator(".card").first
    geo = pg.evaluate("""() => {
      const c = document.querySelector('.card').getBoundingClientRect();
      const rel = r => [r.left - c.left, r.top - c.top, r.width, r.height];
      return {card: rel(c), svg: rel(document.querySelector('.card svg').getBoundingClientRect()),
              boxes: [...document.querySelectorAll('.card .boxes b')].map(b => rel(b.getBoundingClientRect()))};
    }""")
    shot = card.screenshot()
    b.close()

im = Image.open(io.BytesIO(shot)).convert("RGB")
k = im.width / geo["card"][2]                     # image px per CSS px
d = ImageDraw.Draw(im)
ink = (28, 34, 52)

# The grid sits inside the svg's quiet zone of 3 cells: svg = (N + 6) cells.
sx, sy, sw, _ = geo["svg"]
g = sw / (N + 6) * k
gx, gy = sx * k + 3 * g, sy * k + 3 * g
blank = np.array(im.convert("L"))
rnd = random.Random(42)
for r in range(N):
    for c in range(N):
        if not mods[r, c]:
            continue
        x, y = gx + c * g, gy + r * g
        if blank[int(y + g / 2), int(x + g / 2)] < 80:   # pre-printed (finders, timing)
            continue
        j = lambda: rnd.uniform(0, 0.07) * g
        poly = [(x + j(), y + j()), (x + g - j(), y + j()), (x + g - j(), y + g - j()), (x + j(), y + g - j())]
        shade = rnd.randint(0, 18)
        d.polygon(poly, fill=(ink[0] + shade, ink[1] + shade, ink[2] + shade))

# fingerprint written into its eight boxes, one digit each, a little uneven
for ch, (bx, by, bw, bh) in zip("0948d268", geo["boxes"]):
    size = int(bh * k * 0.78)
    glyph = Image.new("RGBA", (size * 2, size * 2), (0, 0, 0, 0))
    ImageDraw.Draw(glyph).text((size, size), ch, font=ImageFont.truetype(os.path.join(F, "Inter-Regular.ttf"), size),
                               fill=ink + (235,), anchor="mm")
    glyph = glyph.transform(glyph.size, Image.AFFINE, (1, 0.18, -0.18 * size, 0, 1, 0), resample=Image.BICUBIC)
    glyph = glyph.rotate(rnd.uniform(-4, 4), resample=Image.BICUBIC)
    cx, cy = (bx + bw / 2) * k + rnd.uniform(-1, 1) * k * 0.3, (by + bh / 2) * k + rnd.uniform(-1, 1) * k * 0.3
    im.paste(glyph, (int(cx - size), int(cy - size)), glyph)

# 85.6 x 54 mm card at 200 px/cm, on slightly warm paper
im = im.resize((1712, 1080), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.4))
arr = np.array(im).astype(np.float32) * np.array([246, 243, 236]) / 255
arr += np.random.default_rng(1).integers(-4, 5, arr.shape[:2])[..., None]   # paper fibre
Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).save(os.path.join(HERE, "seedqr_card.png"))
print("ok", int(mods.sum()), "modules")
