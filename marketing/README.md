# Marketing

## Brand

**Goldfish**, tagline **Sign and forget.** The mark is a fish built from QR
modules on a 9 × 5 grid, with a QR finder pattern for an eye. The SVGs in
`mascot/` are the source of truth; don't redraw it:

- `goldfish-mark.svg`: orange fish, ink eye, for light grounds (the device
  logo is rasterized from it with `firmware/src/app/images/svg2c.py`)
- `goldfish-icon.svg` / `goldfish-icon-dark.svg`: app tiles on orange and on ink

Colours: fish orange `#FF7A2F` (the mark and the orange case), orange text `#C2501A`, ink
`#14222B`, grey text `#5A6B75`, paper `#EDF2F4`, border `#D9E2E6`, white.
Case colours: orange (`#EE8B57` on the site, the fish orange `#FF7A2F` as it looks
printed; the renders start from `#FF7A2F`), black `#303030`, blue `#0A7ED0`, white
`#DAD7D0`, defined once in `landing/brand.css` for the site; the renders use the same four.
The eye must contrast with both the body and the ground. Type: Rethink Sans
for headings, Inter for device text, JetBrains Mono for numbers.

## Product renders

`renders/render.py` renders the device in Blender from the real printed case
(`case/exports/case.stl`) with the firmware's actual screens
(`firmware/docs/screens/*.png`, made by the simulator) on the display. When
the case or the UI changes, regenerate the inputs and render again:

```bash
cd firmware && SIM_SHOTS=docs/screens .pio/build/sim/program && cd ..   # screens (PPM; convert to PNG)
blender -b -P marketing/renders/render.py                               # all shots
ONLY=1,3 SAMPLES=64 PCT=50 blender -b -P marketing/renders/render.py    # quick preview of some
```

| Shot | Shows |
|---|---|
| `out/01_hero_review.png` | Reviewing a transaction |
| `out/02_back.png` | Back: camera, USB-C |
| `out/03_colorways.png` | Four colours across the flow: seed loaded, slide to sign, signed QR, address verified |
| `out/04_edge.png` | USB-C tunnel |
| `out/05_wallet_topdown.png` | Wallet menu |
| `out/06_lineup.png` | Four devices: start, new seed, wallet menu, connect wallet |
| `out/07_hero_home.png` | Start screen, whole device (README hero) |
| `out/08_slide_to_sign.png` | Confirm and slide to sign |
| `out/09_new_seed_dice.png` | New seed from dice rolls |
| `out/14_sign_flow.png` | Three devices: ready to sign, slide to sign, signed QR |

### Lifestyle scenes

`renders/scenes.py` puts the same device in context:

```bash
blender -b -P marketing/renders/scenes.py            # all scenes
ONLY=a SAMPLES=64 PCT=50 blender -b -P marketing/renders/scenes.py
```

| Scene | Shows |
|---|---|
| `out/10_desk.png` | On a desk with a USB-C cable, reviewing a transaction; a laptop behind shows the transaction QR |
| `out/11_flatlay.png` | Flat lay on slate: wallet menu, paper SeedQR backup card, cable |
| `out/12_backup_closeup.png` | Seed loaded, next to the SeedQR card it was scanned from |
| `out/13_backup_copy.png` | Copying the seed onto the paper SeedQR card, part by part |

`renders/metal.py` renders the steel backup for the `/metal/` page
(`out/15_metal_stack.png`): the device showing the words, nine stamped M8
washers on a bolt and three loose ones (twelve in all), and the printed box with its lid off. Real
part sizes; the stamped words are the screenshot seed's. `CPU=1` when the GPU is
busy. The box is the [Knurled Blockmit Stackbox](https://makerworld.com/en/models/196485)
by Printer_Gobrrr (CC BY-NC-SA), its body shortened to 45 mm for 12 washers
(`hardware/stackbox_12.scad`);
`renders/assets/stackbox_*.stl` are decimated copies.

```bash
blender -b -P marketing/renders/metal.py
```

The laptop shows a generic wallet window (`renders/assets/laptop_screen.png`,
drawn for these renders), not any real app's interface. The SeedQR card
(`renders/assets/seedqr_card.png`, drawn by `make_seedqr_card.py`) is the real
wallet card from `cards/`, filled in with a scannable Compact SeedQR of
SeedSigner's public 12-word test vector, the same seed the
screens show (fingerprint `0948 d268`). Shared device code lives in
`renders/device.py`.

The blue is sampled from a printed case; the other colours are plausible PLA
colours, not tested prints. Screens show example
data from SeedSigner's public test vectors; the transaction is SeedSigner's
test transaction redone on mainnet (`firmware/sim/make_mainnet_psbt.py`), with a
made-up coin.

## Copy guidelines

Claims must match what the hardware and firmware actually do:

- "No Wi-Fi or Bluetooth hardware" is true for the RP2350 board.
- "Nothing is saved" is true: the seed lives in RAM only.
- Don't call it "air-gapped" without qualification while it's powered over
  USB, and don't claim audits, secure boot or wallet compatibility that hasn't
  been tested.
