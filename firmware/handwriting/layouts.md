# Backup layouts people write on

The paper templates people write their seed on: the recovery cards shipped in
the box with hardware wallets, plus popular printable sheets. `make_layouts.py`
redraws each *layout* generically, with no maker's artwork or logo, fills it in
with test seeds, and writes an eval set that `eval_pages.py` scores per layout.

Surveyed 2026-11 from vendors' own PDFs and docs. "Unconfirmed" means no
source showed the layout.

## Templates

| Template | Words | Layout | Numbers, order | Printed extras | Layout in `make_layouts.py` |
|---|---|---|---|---|---|
| Ledger recovery sheet (Nano X user guide) | 24 | 2 columns of lines | `1.`–`24.`, down | title, warning, "Confidential" | `lines2_24_dot` |
| Trezor One card ([SEED_24_DIY.pdf](https://static.trezor.io/2/19/146/147/SEED_24_DIY_a98497df0a.pdf)) | 24 | 2 columns of lines | `1`–`24`, down | warnings, PIN line, label | `lines2_24_bare` |
| Trezor Model T / Safe 3 legacy card ([SEED_12_DIY.pdf](https://static.trezor.io/2/19/146/147/SEED_12_DIY_91908109d6.pdf)), 141 × 80 mm | 12 | 8 letter boxes per word, printed turned 90° | number tiles 1–12 | name, PIN | `letters8_12` (upright) |
| BitBox02 card ([backupcard_print.pdf](https://shiftcrypto.io/backupcard/backupcard_print.pdf)) | 24 | 2 columns of lines | `01`–`24`, down | wallet name, passphrase, notes | `lines2_24_zero` |
| COLDCARD backup card ([recovery-cards](https://coldcard.com/docs/recovery-cards/)), 2 × 3.5 in | 24 | 2 columns, 8 letter boxes per word | `1`–`24`, down | PIN boxes, anti-phishing words | `letters8_24` |
| COLDCARD printable recovery card, 5 × 3 in | 24 | 2 columns, 4 letter boxes per word | `1`–`24`, down | xfp, derivation, notes | `letters4_24` |
| Foundation Passport security card ([securitycard.pdf](https://docs.foundation.xyz/images/securitycard.pdf)) | 24 | 4 columns × 6 lines | `1.`–`24.`, down | backup code boxes | `lines4_24` |
| SeedSigner sheets ([templates](https://github.com/SeedSigner/seedsigner/tree/dev/docs/seed_qr/printable_templates)) | 12 / 24 | 1 or 2 columns of lines | `1.`, down | fingerprint, warning | `lines1_12`, `lines2_24_dot` |
| SeedSigner trading card, Jade and Krux SeedQR templates | 12 / 24 | a SeedQR grid, sometimes with a list | `1.`, down | grid labels | `seedqr_list_12` |
| Goldfish word card (`cards/`), ID-1 | 12 | 2 columns of 6 lines | `1`–`12`, down | fingerprint boxes | `goldfish_card` |

Left out:
- Trezor Safe 3/5's current card: it is 20-word SLIP-39, not BIP-39.
- Metal kits (Cryptosteel, Billfodl, Cryptotag, Blockplate, SEEDPLATE and others): stamped letters or tiles, not handwriting.

Unconfirmed: the layouts of the Ledger Stax/Flex sheet, Jade's in-box card and Keystone's card; most physical sizes. Every template confirmed runs down the columns.

## Baseline

`make_layouts.py --per-layout 20` (seed 1), `eval_pages.py`. "Right" is labelled words read right and offered; "whole page" means every word offered and nothing else. "Before" is the commit that added this file; "now" adds letter-box words to the page reader.

| Layout | Right at 1000, before | now | Right at 640, before | now | Whole page at 1000 (now) |
|---|---|---|---|---|---|
| `lines4_24` | 97% | 97% | 94% | 94% | 6 of 20 |
| `goldfish_card` | 93% | 93% | 90% | 90% | 10 of 20 |
| `lines2_24_bare` | 93% | 93% | 85% | 85% | 2 of 20 |
| `lines1_12` | 92% | 92% | 90% | 90% | 5 of 20 |
| `lines2_24_dot` | 90% | 90% | 88% | 88% | 3 of 20 |
| `lines2_24_zero` | 90% | 90% | 87% | 87% | 0 of 20 |
| `seedqr_list_12` | 89% | 89% | 79% | 79% | 10 of 20 |
| `letters8_24` | 65% | 65% | 58% | 58% | 0 of 20 |
| `letters8_12` | 45% | 65% | 44% | 70% | 0 of 20 |
| `letters4_24` | 41% | 62% | 34% | 43% | 0 of 20 |

Lines read well. Letter boxes failed in two ways:

- **Words never found** (the 4-box card, Trezor's legacy card): the reader joined letters into a word only when they sat less than 0.9 letter heights apart, and these boxes put letters 1.2-1.6 letter heights apart, so each letter was a piece of its own, dropped as too narrow. Partial words appeared only where leftover dots of a box line bridged two letters. Now a lone letter with another lone letter up to 2.2 letter heights along its line joins its neighbours; a lone row number between two columns has no lone letter beside it and stays apart, and a few boxed pieces in a column of ordinary words (a fingerprint written one character per box) are left out.
- **Words found but not read** (the 8-box COLDCARD card, whose narrow boxes keep letters close enough to join): a box line stands between every two letters in the word's picture, which the word reader never saw in training. The same pages drawn without box lines read 85% instead of 66%. `strips.py` now draws letter-box strips; a model fine-tuned with them read every layout better (`letters8_24` 72%, `letters4_24` 74%) but more real photos wrong, so it isn't in the firmware yet (`README.md`).

No letter-box page reads whole yet. These pages are synthetic and cleaner than real photos (the real photos in `seedimgs` read 64% at the same size), so the gap between layouts says more than the numbers themselves.
