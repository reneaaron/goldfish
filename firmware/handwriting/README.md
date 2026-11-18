# Handwriting: the whole-word reader's model and tools

The firmware reads a photo of a handwritten backup with `src/app/page_reader.cpp`
(finds the words) and `src/app/word_reader.cpp` (reads each one). The
network in `src/app/word_model.h` is generated here. None of this runs on the
device; it needs Python with PyTorch (a GPU helps, a CPU works).

## Files

| File | What it does |
|---|---|
| `strips.py` | Draws synthetic training strips: one word (optionally with a row number), from EMNIST handwritten letters or a handwriting font, capitals, lowercase or mixed, with ruled lines, neighbouring rows, tilt, blur, shadows, noise |
| `gen_strips.py` | Draws a set of strips once and saves it (`x.npy`, `y.npy`, `len.npy`), so training runs at GPU speed |
| `train_words.py` | Trains the strip reader (CTC); scores it on labelled photos; `--init` continues from a checkpoint |
| `export_words.py` | Folds batch norm, quantises to int8 weights / int16 activations, writes `src/app/word_model.h` and `test/fixtures/word_vectors.h`, and checks the integer model against the float one |
| `model/words.pt` | The checkpoint `word_model.h` was exported from: start fine-tuning here |
| `make_page.py` | The synthetic test page `sim/samples/handwritten_page.png` |
| `page_cli.cpp`, `build.sh` | The firmware's page + word reader, built natively |
| `eval_pages.py` | Scores the firmware end to end (photo in, words out) on labelled photos |
| `evalset/labels.json` | 16 web photos of handwritten demo seeds (348 words): source pages, image links, word boxes |
| `fetch_evalset.py`, `fetch_fonts.sh` | Download those photos and the fonts into `data/` |

`data/` is not in git: downloads, generated strips, checkpoints and private photos go there.

## Run it

All from `firmware/handwriting`. Training and strip drawing can use a lot of
memory; run them capped so a mistake can't freeze the machine:

```bash
CAP="systemd-run --user --scope -q -p MemoryMax=16G -p MemorySwapMax=0 -- nice -n 10"
```

```bash
./fetch_fonts.sh
uv run --with-requirements requirements.txt python -c "import torchvision, numpy as np; ds = torchvision.datasets.EMNIST('data/emnist', split='byclass', train=True, download=True); np.savez_compressed('data/emnist_byclass_train.npz', x=ds.data.numpy().transpose(0, 2, 1), y=ds.targets.numpy())"
uv run --with pillow python fetch_evalset.py
```

```bash
$CAP uv run --with-requirements requirements.txt python gen_strips.py --emnist data/emnist_byclass_train.npz --fonts data/fonts --n 500000 --out data/strips
$CAP uv run --with-requirements requirements.txt python train_words.py --data data/strips --out data/wordmodel --steps 20000 --init model/words.pt --eval data/seedimgs
$CAP uv run --with-requirements requirements.txt python export_words.py --ckpt data/wordmodel/words.pt --data data/strips --eval data/seedimgs
```

Then from `firmware/`: `pio test -e test -f test_word_reader`, and the end-to-end score:

```bash
handwriting/build.sh && python3 handwriting/eval_pages.py handwriting/data/seedimgs --side 1000
```

Copy a checkpoint you keep over `model/words.pt`, next to the `word_model.h` it produced.

## Where it stands

Trained on synthetic strips only (20k steps). On hand-marked word boxes of
real photos it reads 82% of words with 2 wrong; end to end (finding the words
too) 65% with 2 wrong, at 1000 pixels. The web photos are only for scoring:
training on them would make the score meaningless. The next gain is real
training data: photos of written words at the board camera's distance,
labelled like `evalset/labels.json`, mixed into training.
