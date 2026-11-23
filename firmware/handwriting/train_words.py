"""Train the whole-word strip reader (CTC) and score it on real photos.

Model: a strip (STRIP_H x STRIP_W, see strips.py) -> conv layers -> 48 time
steps x NUM_CLASSES letter probabilities. Words are never spelled out
greedily: decode() scores every BIP-39 word (and the first 4 letters of the
longer ones) under the CTC model, with an optional row number in front, and
the best word must beat the runner-up by a margin.

Run from firmware/:
  uv run --with-requirements handwriting/requirements.txt python handwriting/train_words.py \
      --emnist <npz from EMNIST byclass, 0-9 + A-Z> --fonts <dir of .ttf> --out <checkpoint dir> \
      [--eval <dir with labels.json> ...] [--steps 20000]
  ... --eval-only --out <checkpoint dir> --eval <dir> [--show]
"""
import argparse
import glob
import json
import os
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image

from strips import (ALPHABET, NUM_CLASSES, PREFIX_CHARS, STRIP_H, STRIP_W, Glyphs, bip39_words, encode, make_strip,
                    normalise, real_strip)

T_STEPS = STRIP_W // 4


class Net(nn.Module):
    def __init__(self):
        super().__init__()
        def block(ci, co):
            return nn.Sequential(nn.Conv2d(ci, co, 3, padding=1, bias=False), nn.BatchNorm2d(co), nn.ReLU())
        self.b1, self.b2, self.b3, self.b4 = block(1, 16), block(16, 32), block(32, 64), block(64, 96)
        self.t1 = nn.Sequential(nn.Conv1d(96, 96, 3, padding=1, bias=False), nn.BatchNorm1d(96), nn.ReLU())
        self.t2 = nn.Sequential(nn.Conv1d(96, 96, 3, padding=1, bias=False), nn.BatchNorm1d(96), nn.ReLU())
        self.out = nn.Conv1d(96, NUM_CLASSES, 1)

    def forward(self, x):                      # B x 1 x 32 x 192
        x = F.max_pool2d(self.b1(x), 2)        # 16 x 16 x 96
        x = F.max_pool2d(self.b2(x), 2)        # 32 x 8 x 48
        x = F.max_pool2d(self.b3(x), (2, 1))   # 64 x 4 x 48
        x = self.b4(x).amax(2)                 # 96 x 48
        x = self.t2(self.t1(x))
        return self.out(x)                     # B x C x 48


# ---------------------------------------------------------------- data
class Stream(torch.utils.data.IterableDataset):
    def __init__(self, emnist, fonts, seed):
        self.emnist, self.fonts, self.seed = emnist, fonts, seed

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        rng = np.random.default_rng(self.seed * 1000 + (info.id if info else 0))
        d = np.load(self.emnist)
        g = Glyphs(d["x"], d["y"], self.fonts)
        words = bip39_words()
        while True:
            s = make_strip(words[rng.integers(2048)], g, words, rng)
            if s is None:
                continue
            img, label = s
            yield torch.from_numpy(normalise(img))[None], torch.tensor(encode(label), dtype=torch.long)


def collate(batch):
    xs = torch.stack([b[0] for b in batch])
    ys = [b[1] for b in batch]
    return xs, torch.cat(ys), torch.tensor([len(y) for y in ys])


# ---------------------------------------------------------------- decoding
class Lexicon:
    """CTC scores of all BIP-39 words at once, with an optional prefix of row
    number characters (any mix of PREFIX_CHARS) before the word."""

    def __init__(self, viterbi=False):
        # viterbi: score each word by its single best alignment (max instead
        # of log-sum-exp), which is what the firmware can afford
        self.op = np.maximum if viterbi else np.logaddexp
        self.words = bip39_words()
        targets, owner = [], []
        for i, w in enumerate(self.words):
            targets.append(w.upper()); owner.append(i)
            if len(w) > 4:
                targets.append(w[:4].upper()); owner.append(i)
        self.owner = np.array(owner)
        n, smax = len(targets), 2 * 8 + 1
        self.ext = np.zeros((n, smax), np.int64)          # blank-interleaved labels
        self.end = np.zeros(n, np.int64)
        self.skip = np.zeros((n, smax), bool)
        for k, t in enumerate(targets):
            ids = encode(t)
            e = [0]
            for c in ids:
                e += [c, 0]
            self.ext[k, :len(e)] = e
            self.end[k] = len(e)
            for s in range(2, len(e)):
                self.skip[k, s] = e[s] != 0 and e[s] != e[s - 2]
        self.valid = np.arange(smax)[None, :] < self.end[:, None]
        self.prefix_ids = [ALPHABET.index(c) + 1 for c in PREFIX_CHARS]

    def scores(self, logp):
        """logp: T x C log-probabilities -> per-word best log-likelihood."""
        T = logp.shape[0]
        n, smax = self.ext.shape
        ninf = -1e30
        op = self.op
        g_emit = op.reduce(np.concatenate([logp[:, :1], logp[:, self.prefix_ids]], 1), axis=1)
        em = logp[:, self.ext]                            # T x n x smax
        alpha = np.full((n, smax), ninf)
        g = g_emit[0]
        alpha[:, 0] = em[0, :, 0]
        alpha[:, 1] = em[0, :, 1]
        for t in range(1, T):
            prev = alpha
            a1 = np.concatenate([np.full((n, 1), ninf), prev[:, :-1]], 1)
            a2 = np.concatenate([np.full((n, 2), ninf), prev[:, :-2]], 1)
            a2 = np.where(self.skip, a2, ninf)
            new = op(op(prev, a1), a2)
            # from the prefix state into the first blank or first letter
            new[:, 0] = op(new[:, 0], g)
            new[:, 1] = op(new[:, 1], g)
            alpha = np.where(self.valid, new + em[t], ninf)
            g = g + g_emit[t]
        last = alpha[np.arange(n), self.end - 1]
        last2 = alpha[np.arange(n), self.end - 2]
        tot = op(last, last2)
        best = np.full(len(self.words), ninf)
        np.maximum.at(best, self.owner, tot)
        return best

    def decode(self, logp):
        s = self.scores(logp)
        order = np.argsort(-s)
        return self.words[order[0]], s[order[0]] - s[order[1]], self.words[order[1]]


def greedy(logp):
    ids = logp.argmax(1)
    out, prev = [], 0
    for i in ids:
        if i != prev and i != 0:
            out.append(ALPHABET[i - 1])
        prev = i
    return "".join(out)


def load_real(dirs):
    items = []
    for d in dirs:
        for entry in json.load(open(os.path.join(d, "labels.json"))):
            img = Image.open(os.path.join(d, entry["file"]))
            for w in entry["words"]:
                items.append((f"{os.path.basename(d.rstrip('/'))}/{entry['file']}", w["text"], real_strip(img, w["box"])))
    return items


def evaluate(net, items, lex, dev, margin, show=False):
    net.eval()
    tally = {"right": 0, "WRONG": 0, "none": 0, "best_right": 0}
    with torch.no_grad():
        x = torch.from_numpy(np.stack([normalise(s) for _, _, s in items]))[:, None].to(dev)
        logp = F.log_softmax(net(x), 1).permute(0, 2, 1).cpu().numpy()
    for (src, text, _), lp in zip(items, logp):
        word, m, second = lex.decode(lp)
        ok = word == text or (len(text) == 4 and word.startswith(text))
        tally["best_right"] += ok
        verdict = ("right" if ok else "WRONG") if m >= margin else "none"
        tally[verdict] += 1
        if show:
            print(f"  {src:24} {text:9} {verdict:5} best={word:9} margin={m:5.1f} 2nd={second:9} greedy={greedy(lp)}")
    return tally


def gpu_normalise(x):
    """normalise() from strips.py on a batch of uint8 strips, on the GPU."""
    a = 255.0 - x.float().flatten(1)
    a = a - torch.quantile(a, 0.5, dim=1, keepdim=True)
    top = torch.quantile(a, 0.99, dim=1, keepdim=True).clamp(min=16.0)
    return (a / top).clamp(-0.5, 1.5).view(x.shape)


def saved_batches(dirs, dev, batch=256):
    """Batches from gen_strips.py / page_strips.py outputs. Each dir may end in
    ":share" (e.g. data/strips_gpt:0.3): that share of every batch comes from
    it; dirs without one split the rest."""
    sets, shares = [], []
    for spec in dirs:
        d, _, share = spec.partition(":")
        x = torch.from_numpy(np.load(os.path.join(d, "x.npy"))).to(dev)
        y = torch.from_numpy(np.load(os.path.join(d, "y.npy")).astype(np.int64)).to(dev)
        ln = torch.from_numpy(np.load(os.path.join(d, "len.npy")).astype(np.int64)).to(dev)
        sets.append((x, y, ln))
        shares.append(float(share) if share else None)
        print("strips", d, len(x), flush=True)
    rest = (1 - sum(v for v in shares if v)) / max(1, shares.count(None))
    counts = [round(batch * (v if v else rest)) for v in shares]
    while True:
        xs, ys, lens = [], [], []
        for (x, y, ln), n in zip(sets, counts):
            idx = torch.randint(0, len(x), (n,), device=dev)
            xs.append(x[idx]), ys.append(y[idx]), lens.append(ln[idx])
        x, y, lens = torch.cat(xs), torch.cat(ys), torch.cat(lens)
        keep = torch.arange(y.shape[1], device=dev)[None, :] < lens[:, None]
        yield gpu_normalise(x)[:, None], y[keep], lens


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emnist")
    ap.add_argument("--fonts")
    ap.add_argument("--out", required=True)
    ap.add_argument("--eval", nargs="*", default=[])
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--margin", type=float, default=5.0)
    ap.add_argument("--eval-only", action="store_true")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--viterbi", action="store_true", help="best-path word scores, as in the firmware")
    ap.add_argument("--data", nargs="*", help="directories from gen_strips.py / page_strips.py (dir[:share]): train on them, on the GPU, instead of drawing strips")
    ap.add_argument("--init", help="start from these weights (e.g. model/words.pt) instead of from scratch")
    args = ap.parse_args()
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    os.makedirs(args.out, exist_ok=True)
    lex = Lexicon(args.viterbi)
    real = {d: load_real([d]) for d in args.eval}
    net = Net().to(dev)
    ckpt = os.path.join(args.out, "words.pt")

    def report(tag):
        for d, items in real.items():
            t = evaluate(net, items, lex, dev, args.margin, args.show)
            n = len(items)
            print(f"{tag}: {os.path.basename(d.rstrip('/'))} {t['right']}/{n} right, {t['WRONG']} wrong, {t['none']} none "
                  f"(best guess right {t['best_right']}/{n})", flush=True)

    if args.eval_only:
        net.load_state_dict(torch.load(ckpt, map_location=dev))
        report("eval")
        return

    if args.init:
        net.load_state_dict(torch.load(args.init, map_location=dev))
    print("params", sum(p.numel() for p in net.parameters()))
    if args.data:
        dl = saved_batches(args.data, dev)
    else:
        fonts = sorted(glob.glob(os.path.join(args.fonts, "*.ttf")))
        dl = torch.utils.data.DataLoader(Stream(args.emnist, fonts, 1), batch_size=256, num_workers=6,
                                         collate_fn=collate, persistent_workers=True, prefetch_factor=4)
    opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 3e-3, total_steps=args.steps)
    ctc = nn.CTCLoss(blank=0, zero_infinity=True)
    t0 = time.time()
    for step, (x, y, ylen) in enumerate(dl):
        if step == args.steps:
            break
        net.train()
        x = x.to(dev)
        lp = F.log_softmax(net(x), 1).permute(2, 0, 1)    # T x B x C
        loss = ctc(lp, y, torch.full((x.shape[0],), T_STEPS, dtype=torch.long), ylen)
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        if (step + 1) % 1000 == 0 or step + 1 == args.steps:
            print(f"step {step + 1} loss {loss.item():.3f} {time.time() - t0:.0f}s", flush=True)
            report(f"step {step + 1}")
            torch.save(net.state_dict(), ckpt)
    torch.save(net.state_dict(), ckpt)
    report("final")


if __name__ == "__main__":
    main()
