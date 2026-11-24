# Metal backup: plan (draft)

Goal: a `/metal/` page next to `/cards/` that shows how to put a 12-word seed
on stainless washers on a bolt, from parts any hardware store or online shop has.
Fits Goldfish: the device forgets, the backup is the only copy, so it should
outlive paper.

Status: plan only. The stack is our M8 prototype (`~/projects/3d/bitcoin-backup`:
12 washers on an M8 × 35 bolt, knurled Stackbox case). Nothing is fire-tested by
us yet; the fire claims are from material data and other people's tests (see
"Test before we claim").

## What goes on each washer

- First **4 letters** of the word (BIP-39 words are unique by their first 4) plus
  its **number** (`01`..`12`). The number matters: a loose stack can be put back
  in any order.
- One face only, stamped in an arc: `01 ABAN` (~35 mm of the ~51 mm around the
  middle of the ring). Stamping the back face bends the front one.
- Passphrase never on the same stack (same rule as the cards page).

## Washer size: why not DIN 125

The stamped band is the ring between hole and edge: `(OD - ID) / 2`.

| Washer (M8) | OD × ID × t | Ring | Fits stamps |
|---|---|---|---|
| DIN 125 / ISO 7089 | 16 × 8.4 × 1.6 | 3.8 mm | 3 mm only, hard to read |
| **DIN 9021 / ISO 7093-1** | 24 × 8.4 × 2 | 7.8 mm | **6 mm**: recommended |
| RS "Kotflügel" A4 | 30 × 8.4 × 1.5 | 10.8 mm | 6 mm, but thin: bends when stamped; doesn't fit the Stackbox |

Measured on the prototype: OD 23.5 mm, ID 8.5 mm (both inside DIN 9021
tolerance).

Material: **A4 (316)** first choice (better against salt water, burial, and
scales ~40× slower than 304 at 1000 °C); A2 (304) is fine indoors and is what
Baumärkte stock. Never zinc-plated steel (OBI's cheap "Karosseriescheibe" is
zinc-plated), brass, or aluminium.

## Parts list

Same stack as the prototype: 12 washers × 2 mm = 24 mm; with bolt head and
nut the assembly is ~40.3 mm long, 24 mm across, and fits the Stackbox as is.

| # | Part | Norm | Qty | Price seen (Nov 2026) |
|---|---|---|---|---|
| 1 | Washer M8, 8.4 × 24 × 2, A4 (or A2) | DIN 9021 / ISO 7093-1 | 12 | A2: [Hornbach Reyher, 100 pcs 30.50 €](https://www.hornbach.de/p/unterlegscheibe-reyher-din-9021-o-8-4-mm-edelstahl-a2-100-stueck/10316873/) (0.31 €/pc), [Galaxus ~0.19 CHF/pc](https://www.galaxus.ch/en/s4/product/helpmate-karosseriescheibe-84-24-200-mm-edelstahl-a2-din-9021-m8-nuts-washers-61146767); A4: [Würth 0412928](https://eshop.wuerth.de/Washer-with-large-outside-diameter-DIN-9021-A4-stainless-steel-plain-WSH-DIN9021-A4-140HV-D84/0412928.sku/en/US/EUR) (trade, no public price), [eHorst](https://www.ehorst.de/en/Unterlegscheibe-DIN-9021-A4-8-4-mm-Edelstahl-fuer-M-8-1163596.html), eBay |
| 2 | Hex bolt M8 × 35, full thread, A4 | DIN 933 / ISO 4017 | 1 | [SFS A4 M8×35, ~0.34 CHF/pc per 100](https://www.sfs.ch/CH/en/dl/p/306679); A2 M8×40 [Hornbach 25 pcs](https://www.hornbach.at/p/sechskantschraube-o-schaft-reyher-din-933-m8x40-mm-edelstahl-a2-25-stueck/10510090/) |
| 3 | Hex nut M8, A4 | DIN 934 / ISO 4032 | 1 | < 1 € |
| 4 | Letter punches 6 mm, A–Z | — | 1 set | [Gedore 27 pcs 17.70 €](https://www.jh-profishop.de/schlagbuchstaben); [RS PRO 48.35 €](https://uk.rs-online.com/web/p/number-letter-punch-sets/8754742); [SAM ~100 €, rated 60 HRC](https://uk.rs-online.com/web/p/number-letter-punch-sets/2215718) |
| 5 | Number punches 6 mm, 0–9 | — | 1 set | [Tolsen ~10 $](https://zoro.com/tolsen-steel-punch-stamp-die-set-metal-steel-tool-numbers-0-9-9-piece-set-6mm-25097/i/G011212307) |
| 6 | Hammer ~500 g, steel block or vise anvil | — | have | |
| 7 | Stamping jig (holds washer, guides punch) | printed | 1 | free: Blockmit V3, thing:4590734 (prototype used it at −0.30 mm XY comp for 6 mm punches) |
| 8 | 3–4 spare washers to practise | | | |

Rough total first time: **~40–65 €** (punches are most of it). Every further
backup: **~5–8 €** in washers, bolt and nut.

No nylon lock nut: nylon melts first. Tighten hard; a drop of thread lock is
optional (burns off in a fire, but the washers stay on the bolt anyway).

## Heat: numbers and norms

There's no ISO/EN norm for seed backups. What exists, and what we can cite:

| Reference | What it says | Takeaway |
|---|---|---|
| [ISO 834-1:2025](https://www.iso.org/standard/83943.html) standard fire curve, `T = 20 + 345·log10(8t + 1)` | ~840 °C at 30 min, ~945 °C at 60 min, ~1100 °C only after ~3 h | Building fire stays well under stainless' melting point |
| Hydrocarbon curve (EN 1363-2) | 1100 °C within 5 min (fuel, car fires) | Still under 1400 °C |
| Stainless 304 melting range | ~1400–1420 °C (data sheet) | |
| Stainless 316 melting range | ~1375–1400 °C; oxidation limit ~870 °C intermittent / 925 °C continuous ([calphad.com](https://calphad.com/aisi_316_stainless_steel/)) | Surface scales above ~900 °C; stamped depth stays |
| Brass / aluminium / zinc | melt ~900 °C / 660 °C / 420 °C | Why we exclude them |
| EN 1047-1, UL 72 | Data/paper safes: inside stays < 170 °C (paper) | Paper cards need a safe; steel doesn't |

[Lopp's stress tests](https://blog.lopp.net/metal-bitcoin-seed-storage-stress-tests-round-vi/)
heat to ~1100 °C (2000 °F) then quench in water. Washer-and-bolt devices
([Safu Ninja](https://jlopp.github.io/metal-bitcoin-storage-reviews/reviews/safu-ninja),
Cypherwheel) lost no data; **stamped** words survived where engraved ones
didn't ([ColdTi](https://jlopp.github.io/metal-bitcoin-storage-reviews/reviews/coldti)).
His note: washers are a pain to stamp, so the jig is not optional.

## Case

The knurled Stackbox from the prototype (PLA, threaded lid, body shortened
54 → 45 mm by `short.scad`) already fits this stack.

- Move it into this repo as gcad under `case/` (like the signer case) so the
  `/parts/` exports pipeline can ship the STL.
- Say plainly: the printed case is for **hiding and tamper evidence** (a printed
  seal or a numbered sticker over the lid), not fire. PLA softens at ~60 °C; the
  steel inside is what survives.

## Site page `/metal/`

Built: `metal/index.html`, shaped like `parts/` (Rethink Sans from Google
Fonts), linked from `/cards/`, `/start/` and the landing page. No checklist
sheet: a filled-in one would be a second copy of the seed. Originally planned:

1. Why steel (fire table above, short).
2. Parts table with norms, so it works in any store: "DIN 9021 M8 A4" is what
   you ask for.
3. Stamping how-to: jig, practise washer, one firm blow per letter, `01 ABAN`.
4. A printable **checklist sheet** (boxes 1–12 with 4 letter slots) to tick off
   while stamping from the device screen, then shred.
5. Restore: type the 4 letters into Goldfish's word entry.
6. Link from `/cards/` and `/start/`.

## Test before we claim

Before any "fireproof" copy (`marketing/README.md` rule):

- [ ] Stamp one full 12-word stack (time it, note what goes wrong).
- [ ] Torch / kiln to ~1000 °C for 30 min, quench in water, photo, read back.
- [ ] Vise crush, then read back.
- [ ] Restore that seed on the device from the washers alone.

Until then the page says: "stainless steel melts at ~1400 °C, above what a
house fire reaches" and links the sources, nothing more.

## Open questions

- Word entry already suggests words by prefix (`word_matches` in `ui.cpp`), so 4
  letters give one match. Check on the device that it's one tap, then say so on
  the page.
- Ship the jig STL ourselves or keep pointing to thing:4590734 (license?)?
