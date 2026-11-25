"""Packs the turntable frames (out/turntable/<colour>/NN.png, from turntable.py)
into one WebP sprite sheet per colour for the landing page's hero, plus the
front frame on its own for the first paint.

    uv run --with pillow python marketing/renders/turntable_pack.py

All frames are cropped to the same box (the union of what's drawn in any of
them), so the device doesn't jump when the page switches frame or colour.
Writes landing/img/turn/<colour>.webp (COLS frames per row) and <colour>-0.webp.
"""
import os, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "out", "turntable")
DST = os.path.join(HERE, "..", "..", "landing", "img", "turn")
COLS, SCALE = 6, 1.0
colours = sorted(d for d in os.listdir(SRC) if os.path.isdir(os.path.join(SRC, d)))
frames = {c: [Image.open(os.path.join(SRC, c, f)).convert("RGBA")
              for f in sorted(os.listdir(os.path.join(SRC, c))) if f.endswith(".png")] for c in colours}
n = {len(v) for v in frames.values()}
if len(n) != 1:
    sys.exit(f"colours have different frame counts: { {c: len(v) for c, v in frames.items()} }")
n = n.pop()

box = None
for fr in frames.values():
    for im in fr:
        b = im.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
        box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
pad = 4
w0, h0 = frames[colours[0]][0].size
box = (max(box[0] - pad, 0), max(box[1] - pad, 0), min(box[2] + pad, w0), min(box[3] + pad, h0))
fw, fh = round((box[2] - box[0]) * SCALE), round((box[3] - box[1]) * SCALE)

os.makedirs(DST, exist_ok=True)
rows = -(-n // COLS)
for c, fr in frames.items():
    sheet = Image.new("RGBA", (fw * COLS, fh * rows), (0, 0, 0, 0))
    for i, im in enumerate(fr):
        cell = im.crop(box).resize((fw, fh), Image.LANCZOS)
        sheet.paste(cell, ((i % COLS) * fw, (i // COLS) * fh))
        if i == 0:
            cell.save(os.path.join(DST, f"{c}-0.webp"), quality=86, method=6)
    sheet.save(os.path.join(DST, f"{c}.webp"), quality=82, method=6)
    print(c, f"{n} frames of {fw}x{fh}", os.path.getsize(os.path.join(DST, f"{c}.webp")) // 1024, "KB")
