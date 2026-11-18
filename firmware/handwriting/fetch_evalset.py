"""Download the web photos of handwritten demo seeds listed in
evalset/labels.json into data/seedimgs/ (as PNG, with the labels), for
eval_pages.py and train_words.py --eval. The photos belong to their sites
(see "source"), so they aren't in the repository; a link may have gone.

Run from firmware/handwriting:
  uv run --with pillow python fetch_evalset.py
"""
import io
import json
import os
import urllib.request

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    labels = json.load(open(os.path.join(HERE, "evalset", "labels.json")))
    out = os.path.join(HERE, "data", "seedimgs")
    os.makedirs(out, exist_ok=True)
    kept = []
    for e in labels:
        try:
            req = urllib.request.Request(e["image_url"], headers={"User-Agent": "Mozilla/5.0"})
            data = urllib.request.urlopen(req, timeout=30).read()
            Image.open(io.BytesIO(data)).convert("RGB").save(os.path.join(out, e["file"]))
            kept.append(e)
            print("ok  ", e["file"], e["source"])
        except Exception as ex:   # gone, moved, or blocked
            print("FAIL", e["file"], e["image_url"], ex)
    json.dump(kept, open(os.path.join(out, "labels.json"), "w"), indent=1)
    print(f"{len(kept)} of {len(labels)} photos")


if __name__ == "__main__":
    main()
