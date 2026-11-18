"""Synthetic training strips for the whole-word reader: one handwritten BIP-39
word per strip, as it looks once a photo of a backup is straightened and cut
into rows.

Each strip: an optional row number in front ("7", "7.", "7)"), the word
(sometimes only its first 4 letters) in capitals, lowercase or mixed case
(the label is always capitals: the reader is case-blind), drawn either with EMNIST
handwritten letters or a handwriting font, with random spacing (letters
touching or overlapping), slant, stroke width, baseline wobble; sometimes a
ruled line under it and parts of the rows above and below; rotation, paper
shading and shadows, noise, blur and low resolution. Output is STRIP_H x STRIP_W grayscale, see normalise().
"""
import os
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

STRIP_H, STRIP_W = 32, 192
# CTC classes: 0 blank, 1-26 A-Z, 27-36 0-9, 37 '.', 38 ')'
ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.)"
NUM_CLASSES = len(ALPHABET) + 1
PREFIX_CHARS = "0123456789.)"
LOWER_X = "acemnorsuvwxz"
LOWER_DESC = "gjpqy"

HERE = os.path.dirname(os.path.abspath(__file__))


def bip39_words():
    src = open(os.path.join(HERE, "..", "src", "app", "bip39_english.cpp")).read()
    words = re.findall(r'"([a-z]+)"', src)
    assert len(words) == 2048
    return words


def encode(text):
    return [ALPHABET.index(c) + 1 for c in text]


def normalise(gray):
    """uint8 strip -> float32 network input: ink high, paper 0, ink ~1.
    The firmware does the same (median as paper level, 95th percentile of
    ink darkness as scale)."""
    a = 255.0 - gray.astype(np.float32)
    paper = np.median(a)
    a = a - paper
    top = np.percentile(a, 99)
    return np.clip(a / max(top, 16.0), -0.5, 1.5).astype(np.float32)


class Glyphs:
    def __init__(self, emnist_x, emnist_y, font_paths, size=64):
        # EMNIST byclass: 0-9 digits, 10-35 A-Z, 36-61 a-z
        self.ex, self.ey = emnist_x, emnist_y
        self.by_class = {c: np.nonzero(emnist_y == c)[0] for c in range(62)}
        self.fonts = [ImageFont.truetype(p, size) for p in font_paths]

    def emnist(self, ch, rng):
        if ch.isdigit():
            c = int(ch)
        elif ch.isupper():
            c = 10 + ord(ch) - 65
        elif ch.islower():
            c = 36 + ord(ch) - 97
        else:
            return None
        a = self.ex[rng.choice(self.by_class[c])].astype(np.float32) / 255   # EMNIST stays uint8 in memory
        ys, xs = np.nonzero(a > 0.25)
        return a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def ink_layer(text, g, rng, h):
    """Draw text as an ink alpha image (float, 0..1) with letter height ~h.
    Returns (alpha, baseline_y)."""
    use_font = rng.random() < 0.5
    font = g.fonts[rng.integers(len(g.fonts))] if use_font else None
    spacing = rng.uniform(-0.18, 0.45)          # gap between letters, in letter heights
    W = int(h * (len(text) * 1.6 + 2))
    H = int(h * 2.2)
    canvas = Image.new("L", (W, H), 0)
    base = int(h * 1.5)
    x = int(h * 0.3)
    for ch in text:
        jitter = int(rng.normal(0, h * 0.04))
        scale = rng.uniform(0.88, 1.12)
        if ch in ".)" or use_font:
            f = font or g.fonts[rng.integers(len(g.fonts))]
            fs = ImageFont.truetype(f.path, max(8, int(h * 1.35 * scale)))
            l, t, r, b = fs.getbbox(ch)
            if r - l <= 0:
                continue
            tmp = Image.new("L", (r - l + 4, b - t + 4), 0)
            ImageDraw.Draw(tmp).text((2 - l, 2 - t), ch, font=fs, fill=255)
            a = np.asarray(tmp, np.float32) / 255
            # glyph top relative to baseline from the font's metrics
            asc = fs.getmetrics()[0]
            top = base - asc + t - 2 + jitter
            img = Image.fromarray((a * 255).astype(np.uint8))
        else:
            a = g.emnist(ch, rng)
            drop = 0
            if ch in LOWER_X:                   # lowercase: x-height, ascenders, descenders
                gh = int(h * 0.62 * scale)
            elif ch in LOWER_DESC:
                gh, drop = int(h * 0.9 * scale), int(h * 0.3)
            else:
                gh = int(h * scale)
            gw = max(2, min(int(a.shape[1] * gh / a.shape[0]), int(gh * 1.3)))
            img = Image.fromarray((a * 255).astype(np.uint8)).resize((gw, gh), Image.BILINEAR)
            top = base - gh + drop + jitter
        if x + img.width >= W or top < 0 or top + img.height > H:
            continue
        region = np.asarray(canvas, np.float32)
        patch = np.zeros_like(region)
        patch[top:top + img.height, x:x + img.width] = np.asarray(img, np.float32)
        canvas = Image.fromarray(np.maximum(region, patch).astype(np.uint8))
        adv = img.width + spacing * h if ch not in ".)" else img.width + h * 0.25
        x += max(1, int(adv))
    alpha = np.asarray(canvas, np.float32) / 255
    # stroke width
    k = rng.random()
    if k < 0.25:
        alpha = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3)), np.float32) / 255
    elif k < 0.35 and h > 30:
        alpha = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(3)), np.float32) / 255
    # slant
    sh = rng.uniform(-0.25, 0.25)
    im = Image.fromarray((alpha * 255).astype(np.uint8))
    im = im.transform(im.size, Image.AFFINE, (1, sh, -sh * base, 0, 1, 0), Image.BILINEAR)
    alpha = np.asarray(im, np.float32) / 255
    cols = np.nonzero(alpha.max(0) > 0.2)[0]
    if len(cols) == 0:
        return None, base
    return alpha[:, max(0, cols.min() - 1):cols.max() + 2], base


def make_strip(word, g, words, rng):
    """One training strip and its label text."""
    text = word.upper()
    if len(word) > 4 and rng.random() < 0.15:
        text = text[:4]
    case = rng.random()
    if case < 0.5:
        shown = text
    elif case < 0.75:
        shown = text.lower()
    elif case < 0.9:
        shown = text[0] + text[1:].lower()
    else:
        shown = "".join(c.lower() if rng.random() < 0.4 else c for c in text)
    prefix = ""
    r = rng.random()
    if r < 0.55:
        n = str(rng.integers(1, 25))
        prefix = n + ["", ".", ")", "."][rng.integers(4)]
    h = int(rng.uniform(28, 48))
    label = prefix + text

    word_alpha, base = ink_layer(shown, g, rng, h)
    if word_alpha is None:
        return None
    pre_alpha = None
    if prefix:
        pre_alpha, _ = ink_layer(prefix, g, rng, int(h * rng.uniform(0.7, 1.0)))
    gap = int(h * rng.uniform(0.3, 1.2))
    W = word_alpha.shape[1] + (pre_alpha.shape[1] + gap if pre_alpha is not None else 0) + int(h * 1.5)
    H = int(h * 4.4)
    ink = np.zeros((H, W), np.float32)
    y0 = int(h * 1.1)                           # word canvas top: baseline at y0 + base
    x = int(h * rng.uniform(0.1, 0.6))
    if pre_alpha is not None:
        ph, pw = pre_alpha.shape
        ink[y0:y0 + ph, x:x + pw] = np.maximum(ink[y0:y0 + ph, x:x + pw], pre_alpha)
        x += pw + gap
    wh, ww = word_alpha.shape
    ink[y0:y0 + wh, x:x + ww] = np.maximum(ink[y0:y0 + wh, x:x + ww], word_alpha)
    baseline = y0 + base
    right = x + ww

    # rows above and below, partly in view
    spacing = int(h * rng.uniform(1.35, 1.9))
    if rng.random() < 0.5:
        for dy in (-spacing, spacing):
            if rng.random() < 0.7:
                other_word = words[rng.integers(len(words))]
                other, ob = ink_layer(other_word.upper() if case < 0.5 else other_word, g, rng, h)
                if other is None:
                    continue
                oy = baseline + dy - ob
                ox = int(rng.uniform(0, h))
                oh, ow = other.shape
                ys, ye = max(0, oy), min(H, oy + oh)
                xe = min(W, ox + ow)
                if ye > ys and xe > ox:
                    ink[ys:ye, ox:xe] = np.maximum(ink[ys:ye, ox:xe], other[ys - oy:ye - oy, :xe - ox])
    # ruled lines
    if rng.random() < 0.5:
        lt = rng.integers(1, 4)
        dark = rng.uniform(0.3, 0.9)
        off = int(rng.uniform(-0.05, 0.2) * h)
        for dy in (0, -spacing, spacing):
            y = baseline + off + dy
            if 0 <= y < H - lt:
                ink[y:y + lt, :] = np.maximum(ink[y:y + lt, :], dark)

    # paper and ink
    paper = rng.uniform(0.7, 0.97)
    ink_level = rng.uniform(0.02, 0.45)
    img = paper - ink * (paper - ink_level)
    img += np.linspace(rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1), W)[None, :]
    img += np.linspace(rng.uniform(-0.08, 0.08), rng.uniform(-0.08, 0.08), H)[:, None]
    if rng.random() < 0.3:                      # a soft shadow across part of the row
        yy, xx = np.mgrid[0:H, 0:W]
        cx, cy, r = rng.uniform(0, W), rng.uniform(0, H), rng.uniform(0.5, 2) * h
        img -= rng.uniform(0.1, 0.35) * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * r * r))
    # tilt: mostly slight, sometimes a lot (a photo taken at an angle)
    angle = rng.uniform(-10, 10) if rng.random() < 0.35 else rng.uniform(-3, 3)
    pivot = (x + ww / 2, baseline - h / 2)
    img = np.asarray(Image.fromarray(img.astype(np.float32), mode="F").rotate(
        angle, Image.BILINEAR, center=pivot, fillcolor=float(paper)), np.float32)

    # crop: the word's line with the word filling 55-80% of the height
    frac = rng.uniform(0.55, 0.8)
    ch = int(h / frac + abs(np.sin(np.radians(angle))) * ww)   # a tilted word needs a taller crop
    cy = baseline - h // 2 + int(rng.normal(0, h * 0.06))
    top = max(0, cy - ch // 2)
    left = max(0, int(rng.uniform(0, h * 0.4)))
    rgt = min(W, right + int(rng.uniform(0.1, 0.6) * h))
    crop = img[top:top + ch, left:rgt]
    im = Image.fromarray((np.clip(crop, 0, 1) * 255).astype(np.uint8))
    nw = max(8, int(round(im.width * STRIP_H / im.height)))
    im = im.resize((min(nw, STRIP_W), STRIP_H), Image.BILINEAR)
    if rng.random() < 0.3:                      # a low-resolution photo, scaled up
        lh = int(rng.uniform(12, 22))
        im = im.resize((max(4, im.width * lh // STRIP_H), lh), Image.BILINEAR).resize(im.size, Image.BILINEAR)
    if rng.random() < 0.7:
        im = im.filter(ImageFilter.GaussianBlur(rng.uniform(0.2, 1.5)))
    a = np.asarray(im, np.float32)
    a += rng.normal(0, rng.uniform(1, 8), a.shape)
    out = np.full((STRIP_H, STRIP_W), np.median(a), np.float32)
    out[:, :a.shape[1]] = a
    return np.clip(out, 0, 255).astype(np.uint8), label


def real_strip(img, box):
    """A labelled word box from a photo -> strip, the way the firmware will cut
    a row: the word's height ~65% of the strip."""
    x0, y0, x1, y1 = box
    h = y1 - y0
    cy = (y0 + y1) / 2
    ch = h / 0.65
    crop = img.crop((int(x0 - 0.3 * h), int(cy - ch / 2), int(x1 + 0.3 * h), int(cy + ch / 2))).convert("L")
    nw = max(8, int(round(crop.width * STRIP_H / crop.height)))
    crop = crop.resize((min(nw, STRIP_W), STRIP_H), Image.BILINEAR)
    a = np.asarray(crop, np.float32)
    out = np.full((STRIP_H, STRIP_W), np.median(a), np.float32)
    out[:, :a.shape[1]] = a
    return out.astype(np.uint8)
