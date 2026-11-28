"""Score the firmware's page reader + word reader end to end on labelled photos.

labels.json (next to the images):
  [{"file": "img01.png", "words": [{"text": "dutch", "box": [x0, y0, x1, y1]}, ...]}, ...]
text is the word as written (4 letters for a short form), box the word's
pixels in the image.

Each labelled word is matched to the found word whose box covers most of it.
  right   matched, the right word, offered
  WRONG   matched, a wrong word, offered
  none    matched, not offered (the user types it)
  missed  no found word covers it
Found words that match no label but were offered anyway are counted as
"stray" (a number, a heading, a margin line read as a word).

An entry may also give "seed", the page's words in order. The words offered
are then scored against that list too, without boxes or order:
  in list      offered, one of the page's words (each counts once)
  not in list  offered, not one of them (a misread, or a title read as a word)
  whole page   every word of the list offered, nothing else offered

Pages with a "layout" (make_layouts.py) are also scored per layout.

Usage (from firmware/, after handwriting/build.sh):
  python3 handwriting/eval_pages.py <dir> [<dir> ...] [--side 640] [--jobs 8] [--show]
"""
import argparse
import json
import os
import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

CLI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "page_cli")


def bbox(corners):
    xs, ys = corners[0::2], corners[1::2]
    return min(xs), min(ys), max(xs), max(ys)


def covered(label, found):
    """Fraction of the label box inside the found box."""
    x0, y0, x1, y1 = label
    a0, b0, a1, b1 = found
    iw, ih = max(0, min(x1, a1) - max(x0, a0)), max(0, min(y1, b1) - max(y0, b0))
    return iw * ih / max(1, (x1 - x0) * (y1 - y0))


def right_word(text, word):
    return word == text or (len(text) == 4 and word.startswith(text))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--side", type=int, default=640, help="longer image side the reader sees")
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() // 3))
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()
    total, per_dir, per_layout = Counter(), {}, {}
    for d in args.dirs:
        labels = json.load(open(os.path.join(d, "labels.json")))
        files = [os.path.join(d, e["file"]) for e in labels]
        chunks = [files[i::args.jobs] for i in range(args.jobs) if files[i::args.jobs]]
        run = lambda c: subprocess.run([CLI, str(args.side)] + c, capture_output=True, text=True, check=True).stdout
        with ThreadPoolExecutor(len(chunks)) as pool:
            out = "".join(pool.map(run, chunks))
        results = {json.loads(l)["file"]: json.loads(l) for l in out.splitlines()}
        t = Counter()
        for e, f in zip(labels, files):
            # rows the reader only guessed: the device uses them only when words
            # are missing; here, without a word count, leave them out
            found = [w for w in results[f].get("words", []) if not w.get("guessed")]
            boxes = [bbox(w["corners"]) for w in found]
            used = set()
            img_t = Counter()
            if "seed" in e:
                left = Counter(e["seed"])
                for w in found:
                    if w["ok"]:
                        hit = next((s for s in left if left[s] and right_word(s, w["word"])), None)
                        if hit:
                            left[hit] -= 1
                        img_t["in list" if hit else "not in list"] += 1
                img_t["list"] = len(e["seed"])
                img_t["pages"] = 1
                img_t["whole page"] = img_t["in list"] == len(e["seed"]) and not img_t["not in list"]
            for lw in e.get("words", []):
                cov = [covered(lw["box"], b) for b in boxes]
                k = max(range(len(cov)), key=cov.__getitem__) if cov else -1
                if k < 0 or cov[k] < 0.5:
                    v = "missed"
                else:
                    used.add(k)
                    w = found[k]
                    v = ("right" if right_word(lw["text"], w["word"]) else "WRONG") if w["ok"] else "none"
                    if args.show and v != "right":
                        print(f"  {e['file']:12} {lw['text']:9} {v:6} read={w['word']:9} margin={w['margin']:.1f}")
                if args.show and v == "missed":
                    print(f"  {e['file']:12} {lw['text']:9} missed")
                img_t[v] += 1
            img_t["stray"] = sum(1 for k, w in enumerate(found) if k not in used and w["ok"])
            img_t["found"] = len(found)
            if args.show:
                print(f"{e['file']:12} {len(e['words']):3} words: " + ", ".join(f"{k} {img_t[k]}" for k in
                      ("right", "WRONG", "none", "missed", "stray")) + f" (found {len(found)})")
            t += img_t
            if "layout" in e:
                per_layout[e["layout"]] = per_layout.get(e["layout"], Counter()) + img_t
        per_dir[d] = t
        total += t
    rows = list(per_dir.items()) + [("total", total)]
    if per_layout:
        rows += [("", None)] + sorted(per_layout.items())
    for d, t in rows:
        if t is None:
            print("per layout:")
            continue
        n = t["right"] + t["WRONG"] + t["none"] + t["missed"]
        print(f"{os.path.basename(d.rstrip('/')):18} {n:4} words: right {t['right']} ({100 * t['right'] / max(1, n):.0f}%), "
              f"WRONG {t['WRONG']}, none {t['none']}, missed {t['missed']}, stray offered {t['stray']}")
        if t["pages"]:
            print(f"{'':18} {t['list']:4} listed: in list {t['in list']} ({100 * t['in list'] / t['list']:.0f}%), "
                  f"not in list {t['not in list']}, whole page {t['whole page']} of {t['pages']}")


if __name__ == "__main__":
    main()
