"""Download Haller's gptimage2-v2 set (1,444 AI-generated photos of
handwritten test seeds, MIT, https://github.com/haller-project/haller release
haller-1) into data/gptimage2/, as a test set for eval_pages.py and
train_words.py --eval. 3.4 GB in two parts, checked against their SHA-256.

labels.json gets one entry per photo:
  "seed"   the page's words, in order (the truth: transcribed independently)
  "words"  word boxes, only on the 1,305 pages whose boxes Haller accepted.
           They are pseudo-labels: Haller's own detector and readers placed
           them, keeping a word only when they read it clearly, so word-level
           scores on them favour Haller and leave out its hardest words.

Run from firmware/handwriting:
  uv run python fetch_gptimage2.py
"""
import hashlib
import json
import os
import shutil
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
URL = "https://github.com/haller-project/haller/releases/download/haller-1/"
PARTS = {
    "haller-gptimage2-v2.tar.part-00": "5034bcb00a454659da7852a1368e14a296ee1c51410736cf7711c868f182170c",
    "haller-gptimage2-v2.tar.part-01": "052aac1b7b9690793392bcf05149073e0542dcbf2efa5f770d3b84b572f704d1",
}
ROOT = "gptimage2-v2/"
KEEP = ("README.md", "LICENSE", "labels.jsonl", "pseudo_boxes.jsonl", "images/")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class Joined:
    """The parts read one after the other, as one file (tarfile streams it)."""

    def __init__(self, paths):
        self.files = [open(p, "rb") for p in paths]

    def read(self, n=-1):
        out = b""
        while self.files and (n < 0 or len(out) < n):
            chunk = self.files[0].read(-1 if n < 0 else n - len(out))
            if not chunk:
                self.files.pop(0).close()
                continue
            out += chunk
        return out


def download(dl):
    os.makedirs(dl, exist_ok=True)
    for name, digest in PARTS.items():
        path = os.path.join(dl, name)
        if not (os.path.exists(path) and sha256(path) == digest):
            print("downloading", name, flush=True)
            with urllib.request.urlopen(URL + name, timeout=60) as r, open(path, "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
            if sha256(path) != digest:
                raise SystemExit(f"{name}: SHA-256 mismatch")
        print("ok  ", name, flush=True)


def extract(dl, out):
    with tarfile.open(fileobj=Joined([os.path.join(dl, n) for n in PARTS]), mode="r|") as tar:
        for m in tar:
            rel = m.name[len(ROOT):] if m.name.startswith(ROOT) else None
            if not rel or not m.isfile() or not rel.startswith(KEEP) or ".." in rel.split("/"):
                continue
            dest = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with tar.extractfile(m) as src, open(dest, "wb") as f:
                shutil.copyfileobj(src, f)


STYLES = {"capitales": "caps", "minuscules": "lower", "cursive": "cursive"}


def convert(out):
    """labels.json for the whole set, and one per handwriting style in
    caps/, lower/ and cursive/ (eval_pages.py then scores each)."""
    boxes = {}
    for line in open(os.path.join(out, "pseudo_boxes.jsonl")):
        p = json.loads(line)
        boxes[p["id"]] = p["mots"]
    sets = {name: [] for name in STYLES.values()}
    for line in open(os.path.join(out, "labels.jsonl")):
        e = json.loads(line)
        seed = [w.lower() for w in e["casse_mots"]]
        words = []
        for b in boxes.get(e["id"], []):
            xs, ys = [p[0] for p in b["quad"]], [p[1] for p in b["quad"]]
            words.append({"text": seed[b["position"] - 1],
                          "box": [round(min(xs)), round(min(ys)), round(max(xs)), round(max(ys))]})
        entry = {"file": f"../images/{e['id']}.png", "seed": seed, "words": words, "factors": e["facteurs"]}
        sets[STYLES[e["style_reg"]]].append(entry)
        sets.setdefault(e["split"], []).append(entry)   # train/ (to train on) and val/ (to test on), ~90/10
    for name, entries in sets.items():
        os.makedirs(os.path.join(out, name), exist_ok=True)
        json.dump(entries, open(os.path.join(out, name, "labels.json"), "w"))
    whole = [dict(e, file=e["file"][3:]) for name in STYLES.values() for e in sets[name]]
    json.dump(whole, open(os.path.join(out, "labels.json"), "w"))
    print(", ".join(f"{name} {len(entries)}" for name, entries in sets.items()), "photos;",
          sum(len(e["words"]) for e in whole), "word boxes")


def main():
    data = os.path.join(HERE, "data")
    dl, out = os.path.join(data, "gptimage2-v2-dl"), os.path.join(data, "gptimage2")
    download(dl)
    if not os.path.exists(os.path.join(out, "pseudo_boxes.jsonl")):
        extract(dl, out)
    convert(out)


if __name__ == "__main__":
    main()
