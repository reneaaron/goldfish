"""A synthetic eval set of filled-in seed backups, one layout type per
template people really write on (the recovery sheets shipped with hardware
wallets, printable and sold seed cards, our own word card). Each layout is
drawn generically from its shape (size, columns, numbering, word slots), with
no maker's artwork, and filled in with a random valid test seed in one hand,
then photographed: tilted, on a table, with uneven light, blur and noise.
Never real funds.

The layouts and where each comes from: LAYOUTS below and layouts.md.

Writes <out>/NNNN.png and <out>/labels.json in eval_pages.py's format, with
"layout" on each page, so eval_pages.py scores each layout on its own:
  uv run --with-requirements requirements.txt python make_layouts.py --emnist <byclass npz> --fonts <dir> \
      --out data/layouts --per-layout 20
  (from firmware/) python3 handwriting/eval_pages.py handwriting/data/layouts --side 1000
"""
import argparse
import glob
import hashlib
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

from make_card import hand_layer
from strips import Glyphs, bip39_words, ink_layer

S = 12                      # pixels per mm
PRINT = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
PRINT_REG = "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
LINE, INK, GREY = 200, 34, 110   # printed rules and boxes, printed text, labels


def mm(v):
    return int(round(v * S))


# A layout: the sheet (w x h mm, corner radius), a header of printed lines,
# then a grid of word slots.
#   n           words (12 or 24)
#   cols, rows  the grid; order "down" (1..rows in the first column) or "across"
#   area        (x0, y0, x1, y1) mm the grid fills
#   num         how slots are numbered: "1", "1.", "01", "#1" or None
#   slot        "line" (a rule to write on), "box" (a box per word), or
#               "letters4" / "letters8" (a box per letter)
#   extras      printed fields beside the words: ("label", x, y, text) and
#               ("boxes", x, y, count, each mm) for blank boxes
WARN = "Never share these words. Anyone who has them can take your bitcoin."
LAYOUTS = {
    # our own word card (cards/): ID-1, fingerprint boxes, 2 columns of 6 lines
    "goldfish_card": dict(w=85.6, h=54, r=3.18, n=12, cols=2, rows=6, order="down", area=(5, 13.5, 80.6, 50),
                          num="1", slot="line", extras=[("label", 5, 7.75, "FINGERPRINT"), ("boxes", 36, 4.5, 8, 5)]),
    # 2 columns of 12 numbered lines, "1.": Ledger's recovery sheet, SeedSigner's 24-word sheet
    "lines2_24_dot": dict(w=100, h=150, r=3, n=24, cols=2, rows=12, order="down", area=(8, 30, 94, 132),
                          num="1.", slot="line", extras=[("label", 8, 12, "MY RECOVERY PHRASE"), ("text", 8, 19, WARN, 2.2),
                                                         ("text", 8, 142, "Confidential - do not disclose", 2.2)]),
    # the same with bare numbers, landscape, a PIN line: Trezor One's card
    "lines2_24_bare": dict(w=105, h=93, r=0, n=24, cols=2, rows=12, order="down", area=(6, 16, 100, 80),
                           num="1", slot="line", extras=[("label", 6, 8, "RECOVERY SEED"), ("field", 6, 88, 60, "Device PIN (optional)")]),
    # zero-padded numbers, a wallet name and passphrase field: BitBox02's card
    "lines2_24_zero": dict(w=95, h=120, r=0, n=24, cols=2, rows=12, order="down", area=(7, 26, 89, 104),
                           num="01", slot="line", extras=[("field", 7, 12, 50, "Wallet name"), ("field", 7, 114, 80, "Optional passphrase")]),
    # 1 column of 12 numbered lines: SeedSigner's 12-word sheet, and most plain-paper backups
    "lines1_12": dict(w=90, h=130, r=0, n=12, cols=1, rows=12, order="down", area=(10, 24, 80, 116),
                      num="1.", slot="line", extras=[("label", 10, 10, "SEED WORDS"), ("field", 50, 16, 30, "Fingerprint"),
                                                     ("text", 10, 124, WARN, 2.0)]),
    # 4 columns of 6 numbered lines: Foundation Passport's security card
    "lines4_24": dict(w=150, h=90, r=3, n=24, cols=4, rows=6, order="down", area=(6, 14, 146, 60),
                      num="1.", slot="line", extras=[("label", 6, 8, "SEED WORDS"), ("rule", 6, 66, 146),
                                                     ("label", 6, 72, "BACKUP CODE"), ("boxes", 6, 76, 4, 6), ("boxes", 40, 76, 4, 6)]),
    # 2 columns of 12, 8 letter boxes per word: COLDCARD's backup card (2 x 3.5 in)
    "letters8_24": dict(w=51, h=89, r=2, n=24, cols=2, rows=12, order="down", area=(3, 12, 49, 80),
                        num="1", slot="letters8", extras=[("label", 3, 6, "SEED WORDS"), ("text", 3, 85, "Device PIN", 1.8),
                                                          ("boxes", 20, 82.5, 4, 2.6)]),
    # 1 column of 12, 8 letter boxes per word: Trezor's legacy 12-word card (Model T, Safe 3), turned upright
    "letters8_12": dict(w=80, h=141, r=3, n=12, cols=1, rows=12, order="down", area=(8, 20, 74, 130),
                        num="1", slot="letters8", extras=[("label", 8, 11, "RECOVERY SEED")]),
    # 2 columns of 12, 4 letter boxes per word (the first four letters): COLDCARD's printable recovery card
    "letters4_24": dict(w=127, h=76, r=0, n=24, cols=2, rows=12, order="down", area=(6, 12, 70, 72),
                        num="1", slot="letters4", extras=[("label", 6, 6, "SEED WORDS"), ("field", 78, 16, 42, "xfp"),
                                                          ("field", 78, 28, 42, "Derivation"), ("field", 78, 40, 42, "Notes")]),
    # 1 column of 12 lines beside a blank SeedQR grid: SeedSigner's trading card, Jade's and Krux's templates
    "seedqr_list_12": dict(w=150, h=95, r=3, n=12, cols=1, rows=12, order="down", area=(6, 12, 62, 90),
                           num="1.", slot="line", extras=[("label", 6, 6, "SEED WORDS"), ("grid", 70, 10, 21, 3.6)]),
}


def mnemonic(rng, n):
    """A random valid BIP-39 mnemonic of n words (a test seed)."""
    cs = n // 3
    bits = "".join(str(int(b)) for b in rng.integers(0, 2, n * 11 - cs))
    entropy = int(bits, 2).to_bytes(n * 4 // 3, "big")
    bits += f"{hashlib.sha256(entropy).digest()[0] >> (8 - cs):0{cs}b}"
    return [bip39_words()[int(bits[i * 11:(i + 1) * 11], 2)] for i in range(n)]


def draw_template(L, dr, title_font, num_font):
    """The printed sheet. Returns each word slot as (number, x0, y0, x1, y1) in pixels."""
    for e in L.get("extras", []):
        if e[0] == "label":
            dr.text((mm(e[1]), mm(e[2])), e[3], font=title_font, fill=INK, anchor="lm")
        elif e[0] == "text":
            dr.text((mm(e[1]), mm(e[2])), e[3], font=ImageFont.truetype(PRINT_REG, mm(e[4])), fill=GREY, anchor="lm")
        elif e[0] == "field":
            dr.text((mm(e[1]), mm(e[2]) - mm(1.5)), e[4], font=ImageFont.truetype(PRINT_REG, mm(2)), fill=GREY, anchor="ls")
            dr.line((mm(e[1]), mm(e[2] + 4), mm(e[1] + e[3]), mm(e[2] + 4)), fill=LINE, width=2)
        elif e[0] == "rule":
            dr.line((mm(e[1]), mm(e[2]), mm(e[3]), mm(e[2])), fill=LINE, width=2)
        elif e[0] == "grid":
            x, y, n, c = e[1], e[2], e[3], e[4]
            for i in range(n + 1):
                w = 3 if i % 5 == 0 or i == n else 1
                dr.line((mm(x + i * c), mm(y), mm(x + i * c), mm(y + n * c)), fill=LINE, width=w)
                dr.line((mm(x), mm(y + i * c), mm(x + n * c), mm(y + i * c)), fill=LINE, width=w)
            for fx, fy in ((0, 0), (n - 7, 0), (0, n - 7)):   # the finder patterns, printed
                for k, col in ((7, INK), (5, 255), (3, INK)):
                    o = (7 - k) / 2
                    dr.rectangle((mm(x + (fx + o) * c), mm(y + (fy + o) * c), mm(x + (fx + o + k) * c), mm(y + (fy + o + k) * c)), fill=col)
        elif e[0] == "boxes":
            x, y, k, size = e[1], e[2], e[3], e[4]
            for i in range(k):
                bx = x + i * (size + 0.8) + (2.4 if i >= k // 2 and k == 8 else 0)
                dr.rounded_rectangle((mm(bx), mm(y), mm(bx + size), mm(y + size * 1.3)), mm(0.5), outline=LINE, width=2)
    x0, y0, x1, y1 = L["area"]
    cols, rows = L["cols"], L["rows"]
    cw, rh = (x1 - x0) / cols, (y1 - y0) / rows
    slots = []
    for k in range(L["n"]):
        col, row = (divmod(k, rows) if L["order"] == "down" else divmod(k, cols)[::-1])
        cx, cy = x0 + col * cw, y0 + row * rh
        num = {"1": f"{k + 1}", "1.": f"{k + 1}.", "01": f"{k + 1:02d}", "#1": f"#{k + 1}", None: ""}[L["num"]]
        nw = 0
        if num:
            nw = 5 if len(num) > 2 else 4
            dr.text((mm(cx + nw - 0.6), mm(cy + rh - 1)), num, font=num_font, fill=INK, anchor="rs")
            nw += 1.4
        sx0, sx1, sy0, sy1 = cx + nw, cx + cw - 2.5, cy + 0.8, cy + rh - 0.4
        if L["slot"] == "line":
            dr.line((mm(sx0 - 1), mm(sy1), mm(sx1), mm(sy1)), fill=LINE, width=2)
        elif L["slot"] == "box":
            dr.rounded_rectangle((mm(sx0), mm(sy0), mm(sx1), mm(sy1)), mm(0.6), outline=LINE, width=2)
        else:
            k_letters = int(L["slot"][7:])
            bw = min((sx1 - sx0) / k_letters, (sy1 - sy0) * 0.85)
            for i in range(k_letters):
                dr.rectangle((mm(sx0 + i * bw), mm(sy0), mm(sx0 + (i + 1) * bw), mm(sy1)), outline=LINE, width=2)
        slots.append((k + 1, mm(sx0), mm(sy0), mm(sx1), mm(sy1)))
    return slots


def page(L, rng, g, fonts, words):
    """One photo of layout L filled in with words. Returns (image, [(text, box)])."""
    W, H = mm(L["w"]), mm(L["h"])
    sheet = Image.new("L", (W, H), 255)
    dr = ImageDraw.Draw(sheet)
    title_font = ImageFont.truetype(PRINT, mm(2.2))
    num_font = ImageFont.truetype(PRINT if rng.random() < 0.5 else PRINT_REG, mm(2.6))
    slots = draw_template(L, dr, title_font, num_font)

    ink = np.zeros((H, W), np.float32)
    hand = fonts[rng.integers(len(fonts))] if rng.random() < 0.6 else None   # one font, or mixed EMNIST glyphs
    lower = rng.random() < 0.25

    def write(text, x, base, h):
        h = max(h, 10)
        a = None
        for _ in range(5):   # ink_layer gives up now and then (a glyph that doesn't fit)
            a, b = hand_layer(text, hand, rng, h) if hand else ink_layer(text, g, rng, h)
            if a is not None:
                break
        if a is None:
            return None
        ys, xs = np.nonzero(a > 0.3)
        t = base - b
        region = ink[max(t, 0):t + a.shape[0], max(x, 0):x + a.shape[1]]
        sub = a[max(t, 0) - t:max(t, 0) - t + region.shape[0], max(x, 0) - x:max(x, 0) - x + region.shape[1]]
        region[:] = np.maximum(region, sub)
        if not len(xs):
            return None
        return (x + xs.min(), t + ys.min(), x + xs.max() + 1, t + ys.max() + 1)

    labels = []
    for (k, x0, y0, x1, y1), word in zip(slots, words):
        text = word.lower() if lower else word.upper()
        h = int((y1 - y0) * rng.uniform(0.5, 0.62))
        if L["slot"].startswith("letters"):
            k_letters = int(L["slot"][7:])
            text = text[:k_letters]
            bw = min((x1 - x0) / k_letters, (y1 - y0) * 0.85)
            box = None
            for i, ch in enumerate(text):
                r = write(ch, int(x0 + i * bw + bw * 0.12), int(y1 - (y1 - y0) * 0.2), min(int((y1 - y0) * 0.62), int(bw * 1.1)))
                if r:
                    box = r if box is None else (min(box[0], r[0]), min(box[1], r[1]), max(box[2], r[2]), max(box[3], r[3]))
        else:
            box = write(text, x0 + int(rng.uniform(0, 1.5) * S), int(y1 - (y1 - y0) * rng.uniform(0.12, 0.22)), h)
        if box:
            labels.append((text.lower(), box))

    # the photo: paper with uneven light, lying tilted on a table
    c = np.asarray(sheet, np.float32) / 255 * (1 - ink * rng.uniform(0.75, 0.9))
    yy, xx = np.mgrid[0:H, 0:W]
    gx, gy = rng.uniform(-0.12, 0.12), rng.uniform(-0.08, 0.08)
    c = c * (rng.uniform(0.82, 0.95) + gx * (xx / W - 0.5) + gy * (yy / H - 0.5))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), mm(L.get("r", 0)), fill=255)
    p = int(max(W, H) * rng.uniform(0.06, 0.16))
    table = int(rng.choice([rng.uniform(30, 90), rng.uniform(120, 170)]))   # dark table, or a light desk
    out = Image.new("L", (W + 2 * p, H + 2 * p), table)
    shadow = Image.new("L", out.size, 0)
    shadow.paste(mask, (p + 6, p + 9))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10))
    out = Image.fromarray((np.asarray(out, np.float32) * (1 - 0.4 * np.asarray(shadow, np.float32) / 255)).astype(np.uint8))
    out.paste(Image.fromarray((np.clip(c, 0, 1) * 255).astype(np.uint8)), (p, p), mask)
    angle = rng.uniform(-6, 6)
    out = out.rotate(angle, Image.BILINEAR, fillcolor=table)
    out = out.filter(ImageFilter.GaussianBlur(rng.uniform(0.5, 1.1)))
    a = np.asarray(out, np.float32) + rng.normal(0, rng.uniform(2, 5), (out.size[1], out.size[0]))
    img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))

    # the word boxes, through the same rotation (PIL turns counter-clockwise about the centre)
    cx, cy = out.size[0] / 2, out.size[1] / 2
    ca, sa = np.cos(np.radians(angle)), np.sin(np.radians(angle))
    boxes = []
    for text, (bx0, by0, bx1, by1) in labels:
        pts = [(bx0 + p, by0 + p), (bx1 + p, by0 + p), (bx0 + p, by1 + p), (bx1 + p, by1 + p)]
        rx = [cx + (x - cx) * ca + (y - cy) * sa for x, y in pts]
        ry = [cy - (x - cx) * sa + (y - cy) * ca for x, y in pts]
        boxes.append({"text": text, "box": [int(min(rx)), int(min(ry)), int(max(rx)) + 1, int(max(ry)) + 1]})
    return img, boxes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emnist", required=True)
    ap.add_argument("--fonts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-layout", type=int, default=20)
    ap.add_argument("--only", help="comma-separated layout names")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    d = np.load(args.emnist)
    fonts = sorted(glob.glob(os.path.join(args.fonts, "*.ttf")))
    g = Glyphs(d["x"], d["y"], fonts)
    os.makedirs(args.out, exist_ok=True)
    names = args.only.split(",") if args.only else list(LAYOUTS)
    entries = []
    for li, name in enumerate(names):
        L = LAYOUTS[name]
        for i in range(args.per_layout):
            rng = np.random.default_rng([args.seed, li, i])
            words = mnemonic(rng, L["n"])
            img, boxes = page(L, rng, g, fonts, words)
            f = f"{name}_{i:03d}.png"
            img.save(os.path.join(args.out, f))
            entries.append({"file": f, "layout": name, "seed": words, "words": boxes})
    json.dump(entries, open(os.path.join(args.out, "labels.json"), "w"), indent=0)
    print(f"{len(entries)} pages in {args.out}")


if __name__ == "__main__":
    main()
