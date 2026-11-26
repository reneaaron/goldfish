"""Draw the paper word cards used in scenes.py: the real wallet word card from
cards/index.html (Wallet card, "For a print shop"), screenshotted in headless
Chrome, with the words and the fingerprint written in by hand.

word_card.png: SeedSigner's public height demise test seed (fingerprint
8b218e81), the seed the device screens show after the scan clip.
word_card_partial.png: the first four words of the New seed screenshot's
seed, mid-way through writing it down; no fingerprint yet.

    uv run --with playwright --with pillow --with numpy python make_word_card.py <handwriting .ttf>

The handwriting font is one of the OFL fonts handwriting/ trains on (e.g.
Kalam-Regular.ttf). Uses the system Chrome (/usr/bin/google-chrome), no
Playwright browser download.
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
HAND = sys.argv[1]
CARDS = os.path.abspath(os.path.join(ROOT, "cards", "index.html"))

CARDS_OUT = {
    "word_card.png": ("height demise useless trap grow lion found off key clown transfer enroll".split(), "8b218e81"),
    "word_card_partial.png": ("unveil nice picture region".split(), ""),
}

# Screenshot the blank word card and measure its word lines and fingerprint boxes.
SCALE = 8
with sync_playwright() as p:
    b = p.chromium.launch(executable_path="/usr/bin/google-chrome")
    pg = b.new_page(device_scale_factor=SCALE)
    pg.goto("file://" + CARDS)
    pg.check("input[value=card]")
    pg.check("#plain")
    card = pg.locator(".card.wc").first
    geo = pg.evaluate("""() => {
      const c = document.querySelector('.card.wc').getBoundingClientRect();
      const rel = r => [r.left - c.left, r.top - c.top, r.width, r.height];
      return {card: rel(c),
              words: [...document.querySelectorAll('.card.wc .word')].map(w => rel(w.getBoundingClientRect())),
              nums: [...document.querySelectorAll('.card.wc .word .n')].map(n => rel(n.getBoundingClientRect())),
              boxes: [...document.querySelectorAll('.card.wc .boxes b')].map(b => rel(b.getBoundingClientRect()))};
    }""")
    shot = card.screenshot()
    b.close()

ink = (28, 34, 52)
blank = Image.open(io.BytesIO(shot)).convert("RGB")
k = blank.width / geo["card"][2]                  # image px per CSS px


def glyphs(im, text, font, cx, cy, rnd, anchor="lm"):
    """`text` at (cx, cy), slanted and a little uneven, like a pen."""
    g = Image.new("RGBA", (int(font.size * (len(text) + 2)), font.size * 3), (0, 0, 0, 0))
    ImageDraw.Draw(g).text((font.size, g.height // 2), text, font=font, fill=ink + (240,), anchor=anchor)
    g = g.transform(g.size, Image.AFFINE, (1, 0.12, -0.12 * g.height / 2, 0, 1, 0), resample=Image.BICUBIC)
    g = g.rotate(rnd.uniform(-2.5, 2.5), resample=Image.BICUBIC, center=(font.size, g.height // 2))
    im.paste(g, (int(cx - font.size), int(cy - g.height // 2)), g)


for name, (words, fp) in CARDS_OUT.items():
    im = blank.copy()
    rnd = random.Random(name)
    for i, w in enumerate(words):
        wx, wy, ww, wh = geo["words"][i]
        nx, _, nw, _ = geo["nums"][i]
        size = int(wh * k * 0.62 * rnd.uniform(0.95, 1.05))
        font = ImageFont.truetype(HAND, size)
        x = (nx + nw) * k + 2.2 * k * 3.78 + rnd.uniform(0, 1.5) * k   # 2 mm after the number
        glyphs(im, w.upper(), font, x, (wy + wh) * k - size * 0.55, rnd)
    for ch, (bx, by, bw, bh) in zip(fp, geo["boxes"]):
        font = ImageFont.truetype(HAND, int(bh * k * 0.7))
        glyphs(im, ch, font, (bx + bw / 2) * k + rnd.uniform(-1, 1) * k * 0.3,   # on the box's baseline, centred
               (by + bh * 0.8) * k + rnd.uniform(-1, 1) * k * 0.3, rnd, anchor="ms")

    # 85.6 x 54 mm card at 200 px/cm, on slightly warm paper
    im = im.resize((1712, 1080), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.4))
    arr = np.array(im).astype(np.float32) * np.array([246, 243, 236]) / 255
    arr += np.random.default_rng(1).integers(-4, 5, arr.shape[:2])[..., None]   # paper fibre
    Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).save(os.path.join(HERE, name))
    print("ok", name)
