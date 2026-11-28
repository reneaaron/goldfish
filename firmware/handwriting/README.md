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
| `make_card.py` | The landing page's demo backup `sim/samples/handwritten_page_demo.png`: the project's word card (`cards/`), filled in |
| `make_layouts.py`, `layouts.md` | A synthetic eval set of the backup layouts people write on (wallet makers' recovery cards, printable sheets, our own card), redrawn generically and filled with test seeds; `layouts.md` lists the templates and the per-layout baseline |
| `page_cli.cpp`, `build.sh` | The firmware's page + word reader, built natively |
| `eval_pages.py` | Scores the firmware end to end (photo in, words out) on labelled photos |
| `evalset/labels.json` | 16 web photos of handwritten demo seeds (348 words): source pages, image links, word boxes |
| `fetch_evalset.py`, `fetch_fonts.sh` | Download those photos and the fonts into `data/` |
| `page_strips.py` | Cuts training strips from labelled page photos (e.g. `data/gptimage2/train`), several crops per word |
| `device_strips.py` | The same, but cut by the firmware's own page reader (`out/page_cli`), matched to the labelled boxes |
| `fetch_gptimage2.py` | Downloads Haller's gptimage2-v2 set (1,444 AI-generated photos of handwritten test seeds, MIT) into `data/gptimage2/`: `train/` and `val/` (Haller's split, ~90/10), and by style `caps/`, `lower/`, `cursive/` |

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

The larger test set (3.4 GB download): each page's true word list, plus word
boxes that Haller's own detector and readers placed. Those boxes leave out the
words Haller couldn't read clearly, so word-level scores on them run high;
`eval_pages.py` also scores the words offered against each page's list, which
doesn't depend on the boxes.

```bash
uv run python fetch_gptimage2.py
```

```bash
$CAP uv run --with-requirements requirements.txt python gen_strips.py --emnist data/emnist_byclass_train.npz --fonts data/fonts --n 500000 --out data/strips
$CAP uv run --with-requirements requirements.txt python train_words.py --data data/strips --out data/wordmodel --steps 20000 --init model/words.pt --eval data/seedimgs
$CAP uv run --with-requirements requirements.txt python export_words.py --ckpt data/wordmodel/words.pt --data data/strips --eval data/seedimgs
```

Then from `firmware/`: `pio test -e test -f test_word_reader`, and the end-to-end score:

```bash
handwriting/build.sh && python3 handwriting/eval_pages.py handwriting/data/seedimgs --side 1000
python3 handwriting/eval_pages.py handwriting/data/gptimage2/val --side 1000
```

Per layout (`layouts.md`), from `firmware/handwriting`, then from `firmware/`:

```bash
uv run --with-requirements requirements.txt python make_layouts.py --emnist data/emnist_byclass_train.npz --fonts data/fonts --out data/layouts --per-layout 20
python3 handwriting/eval_pages.py handwriting/data/layouts --side 1000
```

Train only on `data/gptimage2/train`; `val` is the test set.

Copy a checkpoint you keep over `model/words.pt`, next to the `word_model.h` it produced.

## Where it stands

`model/words.pt` was fine-tuned in two runs of 20k steps from the
synthetic-only model: 60% synthetic strips (run 2 adding the 402 Google
handwriting fonts Haller lists, and whole-word script drawing) and 40% strips
cut from the gptimage2 `train` pages (`page_strips.py`). Word level, int8, at
margin 5: real photos 340 of 384, 0 wrong (was 314, 1 wrong); gptimage2 `val`
1,856 of 1,894, 0 wrong (was 1,483, 0 wrong). Haller-1's best reader gets 328
(2 wrong) and 1,748 (0 wrong) on the same boxes.

End to end at 1000 pixels: real photos 257 of 384 with 4 wrong (was 248, 2
wrong): a word in letter boxes, one after its 4-digit BIP-39 number, two in a
tiny photo. `val`: 1,533 of 1,894 boxed words (81%), 4 wrong; of the 2,232
words listed, 1,750 offered, and 92 offered words that aren't on the page
(titles, fingerprints). Haller-1 end to end on `val` fills in 1,898 right and
55 wrong (1,319 it calls sure, 0 wrong); on the real photos 178 right and
122 wrong (24 sure). Finding the words loses more than reading them: 12% of
`val` words are never found, most on pale pencil and tinted paper.

A third run with 25% strips cut by the firmware's own page reader
(`device_strips.py`) read more words end to end (265 real, 1,565 `val`) but
kept the same 4 wrong and added a confidently wrong word on `val`, so it
wasn't kept.

Test only on the real photos and on `data/gptimage2/val`: the other gptimage2
pages are training data.
