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

Colours are plausible PLA colours, not tested prints. Screens show testnet
example data from SeedSigner's public test vectors.

## Copy guidelines

Claims must match what the hardware and firmware actually do:

- "No Wi-Fi or Bluetooth hardware" is true for the RP2350 board.
- "Nothing is saved" is true: the seed lives in RAM only.
- Don't call it "air-gapped" without qualification while it's powered over
  USB, and don't claim audits, secure boot or wallet compatibility that hasn't
  been tested.
