"""Cut training strips from labelled page photos (labels.json with word boxes,
e.g. data/gptimage2/train from fetch_gptimage2.py), several variants per word:
the page shrunk to a random size like the reader's view, the box shifted a
little, as the page reader would cut it. Same output as gen_strips.py.

Run from firmware/handwriting:
  uv run --with-requirements requirements.txt python page_strips.py data/gptimage2/train --per-word 8 --out data/strips_gpt
"""
import argparse
import json
import os
from multiprocessing import Pool

import numpy as np
from PIL import Image, ImageFilter

from strips import STRIP_H, STRIP_W, encode, real_strip

MAX_LABEL = 14


def _page(args):
    d, entry, per_word, seed = args
    rng = np.random.default_rng(seed)
    img = Image.open(os.path.join(d, entry["file"])).convert("L")
    xs, ys = [], []
    for _ in range(per_word):
        side = rng.uniform(600, 1300)               # longer side the reader sees
        f = side / max(img.size)
        page = img.resize((round(img.width * f), round(img.height * f)), Image.BILINEAR)
        if rng.random() < 0.3:
            page = page.filter(ImageFilter.GaussianBlur(rng.uniform(0.3, 1.0)))
        for w in entry["words"]:
            x0, y0, x1, y1 = (v * f for v in w["box"])
            h = y1 - y0
            dx0, dx1, dy0, dy1 = rng.normal(0, 0.12, 4) * h
            box = (x0 + dx0, y0 + dy0, x1 + dx1, y1 + dy1)
            if box[2] - box[0] < 4 or box[3] - box[1] < 4:
                continue
            xs.append(real_strip(page, box))
            ys.append(w["text"].upper())
    return xs, ys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--per-word", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    jobs = [(d, e, args.per_word, args.seed * 100000 + i)
            for d in args.dirs for i, e in enumerate(json.load(open(os.path.join(d, "labels.json")))) if e.get("words")]
    with Pool(max(1, os.cpu_count() // 3)) as pool:
        res = pool.map(_page, jobs, chunksize=4)
    strips = [s for xs, _ in res for s in xs]
    words = [w for _, ws in res for w in ws]
    os.makedirs(args.out, exist_ok=True)
    y = np.zeros((len(words), MAX_LABEL), np.uint8)
    ln = np.zeros(len(words), np.uint8)
    for i, w in enumerate(words):
        lab = encode(w)
        y[i, :len(lab)], ln[i] = lab, len(lab)
    np.save(os.path.join(args.out, "x.npy"), np.stack(strips).astype(np.uint8))
    np.save(os.path.join(args.out, "y.npy"), y)
    np.save(os.path.join(args.out, "len.npy"), ln)
    print(len(words), "strips from", len(jobs), "pages")


if __name__ == "__main__":
    main()
