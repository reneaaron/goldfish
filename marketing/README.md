# Marketing

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
| `out/02_back.png` | Back: camera, clip slots, USB-C |
| `out/03_colorways.png` | Three colours across the flow: seed loaded, slide to sign, signed QR |
| `out/04_edge.png` | USB-C and button pinholes |
| `out/05_wallet_topdown.png` | Wallet menu |
| `out/06_lineup.png` | Four devices: start, new seed, wallet menu, connect wallet |

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

The laptop shows a generic wallet window (`renders/assets/laptop_screen.png`,
drawn for these renders), not any real app's interface. The SeedQR card
(`renders/assets/seedqr_card.png`, drawn by `make_seedqr_card.py`) is a real,
scannable QR of SeedSigner's public 12-word test vector, the same seed the
screens show (fingerprint `0948 d268`). Shared device code lives in
`renders/device.py`.

Colours are plausible PLA colours, not tested prints. Screens show testnet
example data from SeedSigner's public test vectors.

## Copy guidelines

Claims must match what the hardware and firmware actually do:

- "No Wi-Fi or Bluetooth hardware" is true for the RP2350 board.
- "Nothing is saved" is true: the seed lives in RAM only.
- Don't call it "air-gapped" without qualification while it's powered over
  USB, and don't claim audits, secure boot or wallet compatibility that hasn't
  been tested.

## Landing page

`landing/builds/signer/` is a scroll-driven landing page: one continuous Blender
camera flight that the scroll scrubs, from a bare board through the printed case
to a signed transaction and an unplug. Same inputs as the renders above, so the
device and its screens are the real ones.

```bash
blender -b -P marketing/renders/flight.py                       # 696 wide frames
ASPECT=tall OUTDIR=.../tall blender -b -P marketing/renders/flight.py   # phone crop
cd marketing/landing/builds/signer && ./encode-legs.sh <wide-dir> <tall-dir>
python3 -m http.server 4512                                     # then open it
```

`flight.py` takes `FRAMES=1,120` (single stills while you compose), `STEP=`,
`ENGINE=eevee|cycles`, `SAMPLES=` and `PCT=`. `encode-legs.sh` cuts the frames
into the five legs the page mounts, encodes them with a dense GOP so they scrub
instead of playing, and pulls each poster from the encoded mp4.

The brief, the feeling curve and what was verified are in
`landing/builds/signer/BRIEF.md`; the scroll screenshots are in `lab/`.
