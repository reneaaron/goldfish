"""Whole-seed score: on how many photos does the firmware read the whole seed
with nothing to fix by hand? A page counts when the words it lists (found and
guessed rows, in reading order) are exactly the seed's: every one filled in
and right, none extra, none missing.

The seed is a labels.json entry's "seed", or else its labelled words in
order, when there are 12 or 24 of them and they make a valid seed (photos of
several seeds, part of one, or made-up words are left out).

Usage (from firmware/, after handwriting/build.sh):
  uv run --with embit python handwriting/eval_seeds.py <dir> [<dir> ...] [--side 1000] [--show]
"""
import argparse
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

from embit import bip39
from embit.wordlists.bip39 import WORDLIST

CLI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "page_cli")


def full(w):
    """A 4-letter short form -> its word."""
    if w in WORDLIST:
        return w
    m = [x for x in WORDLIST if x.startswith(w)] if len(w) == 4 else []
    return m[0] if len(m) == 1 else w


def seed_of(e):
    words = e.get("seed") or [w["text"] for w in e.get("words", [])]
    words = [full(w) for w in words]
    if len(words) not in (12, 24):
        return None
    try:
        return words if bip39.mnemonic_is_valid(" ".join(words)) else None
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--side", type=int, default=1000)
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() // 3))
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()
    for d in args.dirs:
        pages = [(os.path.join(d, e["file"]), s) for e in json.load(open(os.path.join(d, "labels.json")))
                 for s in [seed_of(e)] if s]
        files = [f for f, _ in pages]
        chunks = [files[i::args.jobs] for i in range(args.jobs) if files[i::args.jobs]]
        run = lambda c: subprocess.run(["nice", "-n", "19", CLI, str(args.side)] + c, capture_output=True,
                                       text=True, check=True).stdout
        with ThreadPoolExecutor(max(1, len(chunks))) as pool:
            out = "".join(pool.map(run, chunks))
        results = {json.loads(l)["file"]: json.loads(l) for l in out.splitlines()}
        whole = 0
        for f, seed in pages:
            words = results[f].get("words", [])
            read = [w["word"] if w["ok"] else "?" for w in words]
            ok = read == seed
            whole += ok
            if args.show:
                wrong = sum(1 for a, b in zip(read, seed) if a != b) + abs(len(read) - len(seed))
                print(f"  {os.path.basename(f):12} {'WHOLE' if ok else f'{wrong:2} to fix'}  read {len(read)} of {len(seed)}"
                      + ("" if ok else f"  {' '.join(read)}"))
        print(f"{os.path.basename(d.rstrip('/')):10} whole seed read {whole} of {len(pages)} pages")


if __name__ == "__main__":
    main()
