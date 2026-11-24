# Steel backup: decisions and open work

The `/metal/` page (`metal/index.html`) is built. This file keeps why it looks
the way it does, and what is still untested.

## Decisions

- **12 words only**, like `/cards/`.
- **Washer: DIN 9021 / ISO 7093-1 M8, 8.4 × 24 × 2 mm, stainless A4 (or A2).**
  The letters go on the ring between hole and edge, `(OD - ID) / 2`: 7.8 mm
  here, enough for 6 mm punches. A plain DIN 125 M8 washer leaves 3.8 mm. M8 × 24
  is also what most DIY steel backups, the Blockmit jigs and the Stackbox use.
- **Bolt M8 × 35 (DIN 933), one nut (DIN 934).** Our prototype stack: 12 washers
  = 24 mm, assembled ~40.3 mm long.
- **Stamp:** number on the top arc, first four letters on the bottom arc, both
  reading upright: `01` / `UNVE`. Four letters are unique in BIP-39; Goldfish's
  word entry offers the one matching word after them.
- **Page:** dead simple, ~130 words: picture, what to buy, three steps, two
  optional prints. No tables, no heat chart.
- **Fire copy:** only the melting point (stainless ~1,400 °C). Context, not on
  the page: the ISO 834-1 standard fire curve reaches ~945 °C after an hour;
  Lopp's stress tests (~1,100 °C, then quenched) lost no words on washer-and-bolt
  designs, and stamped letters outlasted engraved ones.
- **Jig:** [Parametric Washer Stamping Jig](https://www.printables.com/model/855641-parametric-washer-stamping-jig)
  (ISO 7093 M8, 6 mm punches). The original Blockmit V3 warps, per a remix's author.
- **Box:** the Knurled Blockmit Stackbox (CC BY-NC-SA), body shortened to the
  45 mm we printed for 12 washers. `hardware/stackbox_12.scad` squeezes the
  27 mm knurled band to 18 mm instead of cutting 9 mm out: the knurl only lines
  up again every 12 mm (or 6 mm with a 2.86° twist), so a 9 mm cut left a seam.
  No link to boxes for 24–25 washers.
- **Prices (Nov 2026):** washers ~€0.31/pc (Hornbach, A2), punches €18–100
  (Gedore to SAM). The page rounds to USD: ~$5 parts, ~$35 punches, ~$40 total.

## Open

- [ ] Print the squeezed Stackbox body and check the 12-washer bolt fits.
- [ ] Stamp a full 12-word stack (time it, note what goes wrong).
- [ ] Heat to ~1,000 °C for 30 min, quench in water, read it back.
- [ ] Crush in a vise, read it back.
- [ ] Restore that seed on the device from the washers alone.

Until the heat test is done, the page says nothing about fire beyond the
melting point.
