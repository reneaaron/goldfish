"""Draw synthetic training strips once and save them, so training runs at GPU
speed instead of waiting for the CPU to draw strips.

Run from firmware/:
  uv run --with-requirements handwriting/requirements.txt python handwriting/gen_strips.py \
      --emnist <EMNIST byclass npz> --fonts <dir of .ttf> --n 800000 --out <dir>
"""
import argparse
import glob
import os
from multiprocessing import Pool

import numpy as np

from strips import STRIP_H, STRIP_W, Glyphs, bip39_words, encode, make_strip

MAX_LABEL = 14
_g = _words = None


def _init(emnist, fonts):
    # Each worker loads EMNIST as uint8 (~550 MB); converting it all to float
    # in every worker is what once ran the machine out of memory.
    global _g, _words
    os.nice(19)
    d = np.load(emnist)
    _g = Glyphs(d["x"], d["y"], fonts)
    _words = bip39_words()


def _chunk(args):
    seed, start, n = args
    rng = np.random.default_rng(seed)
    xs = np.zeros((n, STRIP_H, STRIP_W), np.uint8)
    ys = np.zeros((n, MAX_LABEL), np.uint8)
    lens = np.zeros(n, np.uint8)
    i = 0
    while i < n:
        s = make_strip(_words[rng.integers(2048)], _g, _words, rng)
        if s is None:
            continue
        lab = encode(s[1])
        xs[i], ys[i, :len(lab)], lens[i] = s[0], lab, len(lab)
        i += 1
    return start, xs, ys, lens


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emnist", required=True)
    ap.add_argument("--fonts", required=True)
    ap.add_argument("--n", type=int, default=800000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--out", required=True, help="directory: x.npy, y.npy, len.npy")
    args = ap.parse_args()
    fonts = sorted(glob.glob(os.path.join(args.fonts, "*.ttf")))
    os.makedirs(args.out, exist_ok=True)
    # results go straight to files on disk, not into the parent's memory
    x = np.lib.format.open_memmap(os.path.join(args.out, "x.npy"), "w+", np.uint8, (args.n, STRIP_H, STRIP_W))
    y = np.lib.format.open_memmap(os.path.join(args.out, "y.npy"), "w+", np.uint8, (args.n, MAX_LABEL))
    ln = np.lib.format.open_memmap(os.path.join(args.out, "len.npy"), "w+", np.uint8, (args.n,))
    chunk = 2000
    jobs = [(args.seed * 1000003 + i, i * chunk, min(chunk, args.n - i * chunk)) for i in range((args.n + chunk - 1) // chunk)]
    with Pool(args.workers, initializer=_init, initargs=(args.emnist, fonts)) as p:
        for done, (start, xs, ys, lens) in enumerate(p.imap_unordered(_chunk, jobs), 1):
            x[start:start + len(xs)], y[start:start + len(xs)], ln[start:start + len(xs)] = xs, ys, lens
            if done % 50 == 0:
                print(f"{done * chunk} strips", flush=True)
    x.flush(); y.flush(); ln.flush()
    print("saved", args.n, "strips to", args.out)


if __name__ == "__main__":
    main()
