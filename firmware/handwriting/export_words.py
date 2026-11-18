"""Export the whole-word strip reader for the firmware (src/app/word_reader.cpp).

Integer pipeline, so the C code can be checked bit for bit against qforward()
here:
  input   uint8 strip -> int16, 4096 = 1.0 (qnormalise, integer maths only)
  layers  int8 weights (batch norm folded in, per output channel scale),
          int16 activations, int32 sums with bias, rescaled by an integer
          multiplier and shift per output channel, then ReLU
  last    float logits (sum * scale + bias), log-softmax in float
Activation scales are set so the largest value seen on calibration strips is
ACT_MAX, leaving 4x headroom before clamping at 32767, and keeping the int32
sums far from overflow (127 * 32767 * 576 would not fit; 127 * 8191 * 576 does).

Writes src/app/word_model.h and test/fixtures/word_vectors.h, and prints the
quantised model's scores on the eval sets next to the float model's.

Run from firmware/handwriting:
  uv run --with-requirements requirements.txt python export_words.py --ckpt <words.pt> --data <gen_strips dir> \
      [--eval <dir with labels.json> ...]
"""
import argparse
import json
import os

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image

from strips import NUM_CLASSES, STRIP_H, STRIP_W, real_strip
from train_words import Lexicon, Net, T_STEPS, gpu_normalise, load_real

ACT_MAX = 8191
IN_ONE = 4096                 # input scale: 4096 = 1.0
LAYERS = ["b1", "b2", "b3", "b4", "t1", "t2"]
POOL = {"b1": (2, 2), "b2": (2, 2), "b3": (2, 1)}


def fold(net):
    """(weight, bias) per layer with batch norm folded in, float64."""
    out = {}
    for name in LAYERS:
        conv, bn = getattr(net, name)[0], getattr(net, name)[1]
        g = (bn.weight / torch.sqrt(bn.running_var + bn.eps)).double()
        w = conv.weight.double() * g.view(-1, *([1] * (conv.weight.dim() - 1)))
        b = bn.bias.double() - bn.running_mean.double() * g
        out[name] = (w.detach().numpy(), b.detach().numpy())
    out["out"] = (net.out.weight.double().detach().numpy(), net.out.bias.double().detach().numpy())
    return out


def conv(x, w, pad=1):
    """float64 convolution of a single example (C x H x W or C x T)."""
    xt, wt = torch.from_numpy(x)[None], torch.from_numpy(w)
    f = F.conv2d if w.ndim == 4 else F.conv1d
    return f(xt, wt, padding=pad)[0].numpy()


def pool(x, name):
    if name in POOL:
        ph, pw = POOL[name]
        c, h, w = x.shape
        return x[:, :h // ph * ph, :w // pw * pw].reshape(c, h // ph, ph, w // pw, pw).max(axis=(2, 4))
    if name == "b4":
        return x.max(axis=1)
    return x


def float_forward(p, x, record=None):
    """Folded float model on one normalised strip (H x W)."""
    a = x[None].astype(np.float64)
    for name in LAYERS:
        w, b = p[name]
        a = np.maximum(conv(a, w) + b.reshape(-1, *([1] * (a.ndim - 1))), 0)
        a = pool(a, name)
        if record is not None:
            record[name] = max(record.get(name, 0), a.max())
    w, b = p["out"]
    return conv(a, w, pad=0) + b[:, None]


# ---------------------------------------------------------------- integer model
def qnormalise(strip):
    """uint8 strip -> int16 input, integer maths only (word_reader.cpp does the
    same): ink high, paper (median) 0, the 99th percentile of ink 4096."""
    a = 255 - strip.astype(np.int64).ravel()
    s = np.sort(a)
    n = a.size
    paper2 = int(s[n // 2 - 1] + s[n // 2])                   # 2 x median
    lo, hi = int(s[6081]), int(s[6082])                        # 99th percentile: 0.99 * 6143 = 6081.57
    top200 = 2 * (lo * 100 + 57 * (hi - lo)) - 100 * paper2
    top200 = max(top200, 16 * 200)
    num = (2 * a - paper2) * 100 * IN_ONE
    q = np.floor_divide(2 * num + top200, 2 * top200)
    return np.clip(q, -IN_ONE // 2, IN_ONE * 3 // 2).reshape(strip.shape).astype(np.int64)


def quantise(p, act_scale):
    """Integer tables per layer: int8 w, int32 bias, int32 multiplier, shift."""
    q = {}
    s_in = 1.0 / IN_ONE
    for name in LAYERS + ["out"]:
        w, b = p[name]
        flat = w.reshape(w.shape[0], -1)
        sw = np.maximum(np.abs(flat).max(1), 1e-12) / 127
        wq = np.clip(np.round(flat / sw[:, None]), -127, 127).astype(np.int8).reshape(w.shape)
        if name == "out":
            q[name] = dict(w=wq, scale=(s_in * sw).astype(np.float32), bias=b.astype(np.float32))
            break
        s_out = act_scale[name]
        bq = np.round(b / (s_in * sw)).astype(np.int64)
        m = s_in * sw / s_out
        shift = np.array([30 - int(np.floor(np.log2(v))) for v in m])   # m * 2^shift in [2^30, 2^31)
        mult = np.round(m * (2.0 ** shift)).astype(np.int64)
        mult = np.where(mult >= 2 ** 31, mult // 2, mult)
        shift = np.where(np.round(m * (2.0 ** shift)) >= 2 ** 31, shift - 1, shift)
        q[name] = dict(w=wq, bias=bq.astype(np.int32), mult=mult.astype(np.int32), shift=shift.astype(np.int32))
        s_in = s_out
    return q


def requant(acc, mult, shift):
    m = mult.astype(np.int64).reshape(-1, *([1] * (acc.ndim - 1)))
    sh = shift.astype(np.int64).reshape(-1, *([1] * (acc.ndim - 1)))
    r = (acc * m + (np.int64(1) << (sh - 1))) >> sh
    return np.clip(r, 0, 32767)                                  # ReLU and int16


def qforward(q, strip, record=None):
    """The firmware's integer forward pass. Returns (T x C log-probs float32)."""
    a = qnormalise(strip)[None]
    for name in LAYERS:
        L = q[name]
        acc = np.rint(conv(a.astype(np.float64), L["w"].astype(np.float64))).astype(np.int64)
        acc += L["bias"].astype(np.int64).reshape(-1, *([1] * (acc.ndim - 1)))
        a = pool(requant(acc, L["mult"], L["shift"]), name)
        if record is not None:
            record[name] = a.copy()
    L = q["out"]
    acc = np.rint(conv(a.astype(np.float64), L["w"].astype(np.float64), pad=0)).astype(np.int64)
    logits = acc.astype(np.float32) * L["scale"][:, None] + L["bias"][:, None]
    lp = logits - logits.max(0, keepdims=True)
    lp = lp - np.log(np.exp(lp).sum(0, keepdims=True))
    return lp.T.astype(np.float32)                               # T x C


# ---------------------------------------------------------------- C output
def c_ints(a):
    return ", ".join(str(int(v)) for v in np.asarray(a).ravel())


def c_float(v):
    s = f"{float(v):.9g}"
    return s + ("f" if any(ch in s for ch in ".en") else ".0f")   # "-30f" isn't a C literal


def c_floats(a):
    return ", ".join(c_float(v) for v in np.asarray(a).ravel())


def write_model(path, q, note):
    with open(path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("// Generated by handwriting/export_words.py; do not edit. Whole-word strip reader,\n")
        f.write(f"// {note}\n")
        f.write(f"#define WM_H {STRIP_H}\n#define WM_W {STRIP_W}\n#define WM_T {T_STEPS}\n#define WM_C {NUM_CLASSES}\n")
        for name in LAYERS:
            L = q[name]
            co, ci = L["w"].shape[:2]
            f.write(f"#define WM_{name.upper()}_CO {co}\n#define WM_{name.upper()}_CI {ci}\n")
            f.write(f"static const int8_t wm_{name}_w[] = {{{c_ints(L['w'])}}};\n")
            f.write(f"static const int32_t wm_{name}_b[] = {{{c_ints(L['bias'])}}};\n")
            f.write(f"static const int32_t wm_{name}_m[] = {{{c_ints(L['mult'])}}};\n")
            f.write(f"static const int8_t wm_{name}_s[] = {{{c_ints(L['shift'])}}};\n")
        L = q["out"]
        f.write(f"static const int8_t wm_out_w[] = {{{c_ints(L['w'])}}};\n")
        f.write(f"static const float wm_out_scale[] = {{{c_floats(L['scale'])}}};\n")
        f.write(f"static const float wm_out_b[] = {{{c_floats(L['bias'])}}};\n")


def write_vectors(path, q, strips, lex):
    with open(path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("// Generated by handwriting/export_words.py: synthetic strips, the integer\n")
        f.write("// network's input and t2 output for the first, the log-probabilities and\n")
        f.write("// the word read (best-path scoring, all 2048 words allowed) for each.\n")
        f.write(f"#define WV_N {len(strips)}\n")
        f.write(f"static const uint8_t wv_strip[WV_N][{STRIP_H * STRIP_W}] = {{\n")
        for s in strips:
            f.write(f"    {{{c_ints(s)}}},\n")
        f.write("};\n")
        rec = {}
        lp0 = qforward(q, strips[0], rec)
        f.write(f"static const int16_t wv_in0[] = {{{c_ints(qnormalise(strips[0]))}}};\n")
        f.write(f"static const int16_t wv_t2_0[] = {{{c_ints(rec['t2'])}}};\n")
        f.write(f"static const float wv_logp[WV_N][{T_STEPS * NUM_CLASSES}] = {{\n")
        words, margins = [], []
        for s in strips:
            lp = qforward(q, s)
            f.write(f"    {{{c_floats(lp)}}},\n")
            w, m, _ = lex.decode(lp)
            words.append(w); margins.append(m)
        f.write("};\n")
        f.write(f"static const char *const wv_word[WV_N] = {{{', '.join(chr(34) + w + chr(34) for w in words)}}};\n")
        f.write(f"static const float wv_margin[WV_N] = {{{c_floats(margins)}}};\n")
    return words, margins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--data", required=True, help="gen_strips.py output, for calibration and test strips")
    ap.add_argument("--eval", nargs="*", default=[])
    ap.add_argument("--margin", type=float, default=5.0)
    args = ap.parse_args()
    net = Net()
    net.load_state_dict(torch.load(args.ckpt, map_location="cpu"))
    net.eval()
    p = fold(net)

    xs = np.load(os.path.join(args.data, "x.npy"), mmap_mode="r")
    rng = np.random.default_rng(5)
    calib = [np.array(xs[i]) for i in rng.choice(len(xs), 512, replace=False)]
    rec = {}
    for s in calib:
        x = gpu_normalise(torch.from_numpy(s)[None]).numpy()[0]
        float_forward(p, x, rec)
    act_scale = {k: v / ACT_MAX for k, v in rec.items()}
    print("activation maxima", {k: round(float(v), 2) for k, v in rec.items()})
    q = quantise(p, act_scale)

    lex = Lexicon(viterbi=True)
    real = load_real(args.eval)
    note = "no eval sets given"
    if real:
        tallies = {}
        for kind in ("float", "int"):
            t = {"right": 0, "WRONG": 0, "none": 0}
            for src, text, strip in real:
                if kind == "float":
                    with torch.no_grad():
                        x = torch.from_numpy(gpu_normalise(torch.from_numpy(strip)[None]).numpy())[None]
                        lp = F.log_softmax(net(x), 1)[0].T.numpy()
                else:
                    lp = qforward(q, strip)
                w, m, _ = lex.decode(lp)
                ok = w == text or (len(text) == 4 and w.startswith(text))
                t["right" if ok and m >= args.margin else "WRONG" if m >= args.margin else "none"] += 1
            tallies[kind] = t
            print(f"{kind:5}: {t['right']}/{len(real)} right, {t['WRONG']} wrong, {t['none']} none")
        t = tallies["int"]
        note = f"real photos ({len(real)} words): {t['right']} read, {t['WRONG']} wrong, {t['none']} not read at margin {args.margin}."
    write_model(os.path.join("..", "src", "app", "word_model.h"), q, note)
    test = [np.array(xs[i]) for i in range(6)]
    words, margins = write_vectors(os.path.join("..", "test", "fixtures", "word_vectors.h"), q, test, lex)
    print("test vectors:", list(zip(words, [round(float(m), 1) for m in margins])))


if __name__ == "__main__":
    main()
