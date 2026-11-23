"""Training strips cut by the firmware's own page reader (out/page_cli, from
build.sh), so the word reader learns the crops it really gets: each page of
labelled photos (labels.json with word boxes, e.g. data/gptimage2/train) is
read at several sizes, and a strip is kept when its word box and a labelled
box mostly overlap; it gets that word. Same output as gen_strips.py.

Run from firmware/handwriting, after build.sh:
  uv run --with numpy --with pillow python device_strips.py data/gptimage2/train --out data/strips_device
"""
import argparse
import json
import os
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np

from strips import STRIP_H, STRIP_W, encode

CLI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "page_cli")
MAX_LABEL = 14


def bbox(corners):
    xs, ys = corners[0::2], corners[1::2]
    return min(xs), min(ys), max(xs), max(ys)


def inside(a, b):
    """Fraction of box a inside box b."""
    iw = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    ih = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    return iw * ih / max(1, (a[2] - a[0]) * (a[3] - a[1]))


def run(files, side, dump):
    env = dict(os.environ, PAGE_DUMP=dump)
    out = subprocess.run(["nice", "-n", "19", CLI, str(side)] + files, capture_output=True, text=True, check=True,
                         env=env).stdout
    return [json.loads(l) for l in out.splitlines()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--sides", default="640,1000,1600", help="longer side limits; page_cli shrinks by whole factors")
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() // 3))
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    entries = [(os.path.join(d, e["file"]), e) for d in args.dirs
               for e in json.load(open(os.path.join(d, "labels.json"))) if e.get("words")]
    by_file = dict(entries)
    files = list(by_file)
    xs, words = [], []
    for side in map(int, args.sides.split(",")):
        with tempfile.TemporaryDirectory() as dump:
            chunks = [files[i::args.jobs] for i in range(args.jobs)]
            with ThreadPoolExecutor(args.jobs) as pool:
                results = [r for rs in pool.map(lambda c: run(c, side, dump), chunks) for r in rs]
            kept = 0
            for r in results:
                labels = by_file[r["file"]]["words"]
                for i, w in enumerate(r.get("words", [])):
                    found = bbox(w["corners"])
                    best = max(labels, key=lambda l: inside(l["box"], found), default=None)
                    if best is None or inside(best["box"], found) < 0.8 or inside(found, best["box"]) < 0.4:
                        continue
                    path = os.path.join(dump, f"{os.path.basename(r['file'])}_{i:02d}.pgm")
                    raw = open(path, "rb").read()
                    xs.append(np.frombuffer(raw[-STRIP_H * STRIP_W:], np.uint8).reshape(STRIP_H, STRIP_W))
                    words.append(best["text"].upper())
                    kept += 1
            print(f"side {side}: {kept} strips", flush=True)
    os.makedirs(args.out, exist_ok=True)
    y = np.zeros((len(words), MAX_LABEL), np.uint8)
    ln = np.zeros(len(words), np.uint8)
    for i, w in enumerate(words):
        lab = encode(w)
        y[i, :len(lab)], ln[i] = lab, len(lab)
    np.save(os.path.join(args.out, "x.npy"), np.stack(xs))
    np.save(os.path.join(args.out, "y.npy"), y)
    np.save(os.path.join(args.out, "len.npy"), ln)
    print(len(words), "strips")


if __name__ == "__main__":
    main()
