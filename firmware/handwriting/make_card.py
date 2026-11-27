"""A synthetic photo of the project's own word card (cards/, the wallet-card
size: 85.6 x 54 mm, fingerprint boxes on top, two printed columns of six
numbered lines), filled in by hand with a 12-word test seed and its
fingerprint, lying tilted on a dark table. Same glyph sources as make_page.py;
the card's print uses Liberation Sans Bold (--print-font).

The browser simulator (the landing page) offers this card with the "height
demise" test seed, so it signs the same transaction as the SeedQR. Run from
firmware/handwriting:
  uv run --with-requirements requirements.txt python make_card.py --emnist <byclass npz> --fonts <dir> \
      --mnemonic "height demise useless trap grow lion found off key clown transfer enroll" \
      --fingerprint 8b218e81 --hand Handlee-Regular.ttf --angle -2 --seed 1 \
      --out ../sim/samples/handwritten_page_demo.png
The firmware reads it 12 of 12; after a reader or model change, check the e2e
demo test and try other --seed or --hand values if it doesn't.
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

from strips import Glyphs, bip39_words, ink_layer

S = 12                      # pixels per mm
PRINT = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"


def mm(v):
    return int(round(v * S))


def hand_layer(text, path, rng, h):
    """text in one handwriting font, letter by letter with a little jitter in
    size, slant and baseline. Returns (alpha, baseline_y) like ink_layer."""
    W, H = int(h * (len(text) * 1.6 + 2)), int(h * 2.4)
    canvas = Image.new("L", (W, H), 0)
    base, x = int(h * 1.6), int(h * 0.3)
    for ch in text:
        fs = ImageFont.truetype(path, max(8, int(h * 1.35 * rng.uniform(0.94, 1.06))))
        l, t, r, b = fs.getbbox(ch, anchor="ls")
        tmp = Image.new("L", (r - l + 8, b - t + 8), 0)
        ImageDraw.Draw(tmp).text((4 - l, 4 - t), ch, font=fs, fill=255, anchor="ls")
        tmp = tmp.rotate(rng.normal(0, 2.5), Image.BILINEAR, expand=True)
        canvas.paste(255, (x, base + t - 4 + int(rng.normal(0, h * 0.03))), tmp)
        x += r - l + int(h * rng.uniform(0.08, 0.16))
    return np.asarray(canvas, np.float32) / 255, base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emnist", required=True)
    ap.add_argument("--fonts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--mnemonic", required=True)
    ap.add_argument("--fingerprint", required=True, help="8 hex digits, written in the boxes")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--pad", type=int, default=150, help="table around the card, in pixels")
    ap.add_argument("--angle", type=float, default=-3.5)
    ap.add_argument("--print-font", default=PRINT)
    ap.add_argument("--hand", help="one handwriting font (a .ttf in --fonts) for every word, as one person writes; default: mixed glyphs")
    ap.add_argument("--no-fingerprint", action="store_true")
    args = ap.parse_args()
    words = args.mnemonic.split()
    if len(words) != 12 or any(w not in bip39_words() for w in words):
        raise SystemExit("--mnemonic: need 12 BIP-39 words")
    rng = np.random.default_rng(args.seed)
    d = np.load(args.emnist)
    g = Glyphs(d["x"], d["y"], sorted(glob.glob(os.path.join(args.fonts, "*.ttf"))))

    W, H = mm(85.6), mm(54)
    # the printed card, as in cards/index.html: --line rules and boxes, --ink numbers
    card = Image.new("L", (W, H), 255)
    dr = ImageDraw.Draw(card)
    line, ink_c, grey = 205, 34, 118
    cap = ImageFont.truetype(args.print_font, mm(1.9) + 2)
    x = mm(5)
    for ch in "FINGERPRINT":                    # letter-spaced caps
        dr.text((x, mm(4.5) + mm(3.25)), ch, font=cap, fill=ink_c, anchor="lm")
        x += cap.getlength(ch) + mm(1.9 * 0.12)
    boxes = []
    bx = W - mm(5) - (8 * mm(5) + 7 * mm(0.8) + mm(2.4))
    for i in range(8):
        dr.rounded_rectangle((bx, mm(4.5), bx + mm(5), mm(4.5) + mm(6.5)), mm(0.6), outline=line, width=max(2, mm(0.2)))
        boxes.append(bx)
        bx += mm(5) + mm(0.8) + (mm(2.4) if i == 3 else 0)
    top = mm(4.5 + 6.5 + 2.5)
    rowh = (H - mm(4) - top) / 6
    colw = (W - mm(10) - mm(6)) / 2
    num = ImageFont.truetype(args.print_font, mm(2.4) + 4)
    slots = []
    for k in range(12):
        col, row = divmod(k, 6)
        x0 = mm(5) + col * (colw + mm(6))
        y1 = int(top + (row + 1) * rowh)
        dr.line((x0, y1, x0 + colw, y1), fill=line, width=max(2, mm(0.2)))
        dr.text((x0 + mm(3.6), y1 - mm(0.9)), str(k + 1), font=num, fill=ink_c, anchor="rs")
        slots.append((int(x0 + mm(3.6) + mm(2)), y1))

    # the handwriting, as an ink layer over the card
    ink = np.zeros((H, W), np.float32)

    hand = os.path.join(args.fonts, args.hand) if args.hand else None

    def write(text, x, base, h):
        a, b = hand_layer(text, hand, rng, h) if hand else ink_layer(text, g, rng, h)
        ah, aw = a.shape
        t = base - b
        y0, x0 = max(t, 0), max(x, 0)
        region = ink[y0:t + ah, x0:x + aw]
        region[:] = np.maximum(region, a[y0 - t:y0 - t + region.shape[0], x0 - x:x0 - x + region.shape[1]])

    for (x, y1), word in zip(slots, words):
        write(word.upper(), x - mm(1) + int(rng.integers(0, mm(0.5))), y1 - mm(0.7), mm(3.3))
    for bx, ch in zip(boxes, "" if args.no_fingerprint else args.fingerprint.upper()):
        write(ch, bx + mm(1.1), mm(4.5) + mm(5.2), mm(3.2))

    c = np.asarray(card, np.float32) / 255 * (1 - ink * 0.85)
    yy, xx = np.mgrid[0:H, 0:W]
    c = c * (0.93 - 0.10 * (xx / W) - 0.05 * (yy / H))   # uneven light
    # round corners (3.18 mm): an alpha mask
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), mm(3.18), fill=255)
    p = args.pad
    table = 60
    out = Image.new("L", (W + 2 * p, H + 2 * p), table)
    shadow = Image.new("L", out.size, 0)
    shadow.paste(mask, (p + 6, p + 9))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10))
    out = Image.fromarray((np.asarray(out, np.float32) * (1 - 0.45 * np.asarray(shadow, np.float32) / 255)).astype(np.uint8))
    out.paste(Image.fromarray((np.clip(c, 0, 1) * 255).astype(np.uint8)), (p, p), mask)
    out = out.rotate(args.angle, Image.BILINEAR, fillcolor=table)
    out = out.filter(ImageFilter.GaussianBlur(0.8))
    a = np.asarray(out, np.float32) + rng.normal(0, 3, (out.size[1], out.size[0]))
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(args.out)


if __name__ == "__main__":
    main()
