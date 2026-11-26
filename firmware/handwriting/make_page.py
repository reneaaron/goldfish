"""A synthetic photo of a handwritten backup for the page reader's tests: a
valid 24-word (or, with --words 12, 12-word) test seed, numbered, in two columns whose rows don't line up, on
ruled paper with a margin line, tilted, with uneven light. Drawn with the same glyph sources as
the training strips (EMNIST capitals, handwriting fonts), but a fixed seed.

Run from firmware/handwriting:
  uv run --with-requirements requirements.txt python make_page.py --emnist <byclass npz> --fonts <dir> \
      --out ../sim/samples/handwritten_page.png
Prints the words. The browser simulator (the landing page) uses a 12-word page of the
"height demise" test seed, so it signs the same transaction as the SeedQR; the firmware
reads this layout 12 of 12 (most don't: try other --seed values after a model change):
  ... make_page.py --emnist ... --fonts ... --words 12 --seed 20 --pad 80 \
      --mnemonic "height demise useless trap grow lion found off key clown transfer enroll" \
      --out ../sim/samples/handwritten_page_demo.png
"""
import argparse
import glob
import hashlib
import os

import numpy as np
from PIL import Image, ImageFilter

from strips import Glyphs, bip39_words, ink_layer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emnist", required=True)
    ap.add_argument("--fonts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--pad", type=int, default=0, help="table around the photo, in pixels")
    ap.add_argument("--words", type=int, default=24, choices=(12, 24))
    ap.add_argument("--mnemonic", help="write these words instead of a random seed")
    args = ap.parse_args()
    rng = np.random.default_rng(args.seed)
    d = np.load(args.emnist)
    g = Glyphs(d["x"], d["y"], sorted(glob.glob(os.path.join(args.fonts, "*.ttf"))))
    # a valid mnemonic (a test seed; never use it): n-1 random words, the last one
    # from the remaining entropy bits (3 of 256, 7 of 128) and the checksum (8 or 4 bits)
    n = args.words
    cs = n // 3
    idx = [int(i) for i in rng.choice(2048, n - 1, replace=False)]
    bits = "".join(f"{i:011b}" for i in idx) + "".join(str(int(b)) for b in rng.integers(0, 2, 11 - cs))
    entropy = int(bits, 2).to_bytes(n * 4 // 3, "big")
    idx.append(int(bits[-(11 - cs):] + f"{hashlib.sha256(entropy).digest()[0] >> (8 - cs):0{cs}b}", 2))
    words = [bip39_words()[i] for i in idx]
    if args.mnemonic:   # the random words are still drawn, so --seed gives the same layout
        words = args.mnemonic.split()
        assert len(words) == n and all(w in bip39_words() for w in words)
    rows = n // 2
    W, H, h = 1400, (1800 if n == 24 else 240 + rows * 66 + 160), 44
    ink = np.zeros((H, W), np.float32)
    spacing = 66
    for y in range(240, H - 80, spacing):                   # ruled lines
        ink[y:y + 2, :] = np.maximum(ink[y:y + 2, :], 0.35)
    ink[:, 150:153] = np.maximum(ink[:, 150:153], 0.5)    # margin line
    for k, word in enumerate(words):
        col, row = divmod(k, rows)
        base = 240 + row * spacing + (0 if col == 0 else spacing // 2)   # columns offset by half a row
        x = 200 + col * 620
        for text, gap in ((f"{k + 1}.", 0), (word.upper(), 18)):
            a, b = ink_layer(text, g, rng, h)
            ah, aw = a.shape
            top = base - b - 2
            x += gap
            region = ink[top:top + ah, x:x + aw]
            region[:] = np.maximum(region, a[:region.shape[0], :region.shape[1]])
            x += aw
    paper = 0.88 - ink * 0.75
    yy, xx = np.mgrid[0:H, 0:W]
    paper -= 0.12 * (xx / W) + 0.06 * (yy / H)             # uneven light
    im = Image.fromarray((np.clip(paper, 0, 1) * 255).astype(np.uint8))
    im = im.rotate(4, Image.BILINEAR, fillcolor=60)         # a tilted photo on a dark table
    im = im.filter(ImageFilter.GaussianBlur(0.8))
    a = np.pad(np.asarray(im, np.float32), args.pad, constant_values=60)   # more table in view
    a = a + rng.normal(0, 3, a.shape)
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(args.out)
    print(" ".join(words))


if __name__ == "__main__":
    main()
