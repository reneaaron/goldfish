# RP2350 signer firmware

Firmware for a stateless, camera-based Bitcoin signer on the Waveshare
RP2350-Touch-LCD-2 (RP2350A, 16MB flash, 2" 240×320 touch screen, OV5640
camera connector). The board has no Wi-Fi or Bluetooth hardware.

Status: early. Seed loading, new seeds with a SeedQR backup, xpub export and
PSBT review/signing work in the simulator; the board's camera driver isn't ported yet. Not for real funds.

![Seed flow in the simulator](docs/screens/flow.png)
![Transaction flow in the simulator](docs/screens/flow_psbt.png)
![Verify address in the simulator](docs/screens/flow_verify.png)
![New seed in the simulator](docs/screens/flow_new_seed.png)
![Enter words and back up as QR in the simulator](docs/screens/flow_restore_backup.png)

## How it works

Seeds come in as a SeedQR (Standard or Compact, 12 or 24 words), the
format SeedSigner uses (the UI just calls it a seed QR), as typed BIP39
words, or are made on the device. There is no passphrase yet.
The seed lives in RAM only: "Wipe seed" or unplugging removes it.

1. Home: Scan QR, Enter words, or New seed
2. Seed loaded: shows the fingerprint to check against your wallet
3. Wallet: Sign transaction, Verify address, Connect wallet, Back up as QR, Wipe now
4. Connect wallet: shows the native segwit xpub as `[fingerprint/84h/0h/0h]zpub…`, the
   same static format SeedSigner exports, readable by Sparrow
5. Verify address: scan a receive address (plain or `bitcoin:` URI) from your
   wallet app; the device checks the first 500 receive and 100 change
   addresses of this seed and says which one it is, or that it isn't yours
6. Sign transaction: scan a PSBT (animated UR `crypto-psbt`, or a single
   base64 frame), review the amount, fee and change, check each output's
   address, slide to sign, then show the signed PSBT as an animated UR

Enter words restores from a words backup: pick 12 or 24, then type each
word. The keyboard only offers letters that lead to a word and, on the last
word, only words with a valid checksum. The seed loads right away.

New seed is a tool: it makes a 12 or 24 word seed and its backup, but
doesn't load it. To use the seed, load it like any other (Scan QR or Enter
words). The device has no RNG of any kind, not even in tests: every bit
comes from the user and can be re-checked offline.

- Dice: 50 or 99 rolls; coins: 128 or 256 flips (heads = 1). The input
  string is hashed with SHA256, same as SeedSigner and bitcoiner.guide/seed,
  so the result can be checked there.
- Draw words: e.g. from a box. Type the first 11 (or 23) words, then flip a
  coin for each free bit of the last word (7 for 12 words, 3 for 24). The
  flips are the bits themselves, so the last word can be checked by hand.

Then an offline warning, the words 4 per page to write down, and an
optional QR card: a Compact SeedQR (21×21 for 12 words, 25×25 for 24)
shown whole, then copied one block at a time, labelled with column letters
and row numbers like the card's grid (21×21 splits 5+5+5+6, so no thin
edge blocks). Scanning the finished card checks it. The flow ends with the
fingerprint to write on the backup; leaving it wipes the seed.

Back up as QR runs the same card flow for whatever seed is loaded, scanned
or typed, e.g. to turn a words backup into a QR card.

Review follows SeedSigner's rules: every input must belong to the loaded seed,
change counts as change only if the seed re-derives that exact output script,
and an output that claims our fingerprint but doesn't verify blocks signing.
Single-sig native segwit only, mainnet or testnet (from the PSBT's derivation
paths). BBQr input isn't supported yet.

## Stack

- PlatformIO with the Arduino core for RP2350 (earlephilhower/arduino-pico)
- LVGL 9.2 for the UI
- uBitcoin for BIP39/BIP32/PSBT
- quirc for QR decoding (vendored in `lib/quirc`)
- Inter and JetBrains Mono, converted for LVGL (`src/app/fonts`, SIL OFL)

## Build

```bash
pio run
```

The firmware ends up at `.pio/build/rp2350_touch_lcd_2/firmware.uf2`.

## Tests

```bash
pio test -e test
```

- SeedQR decoding: the vectors in SeedSigner's SeedQR spec
- Key derivation: the BIP84 test vector (fingerprint `73c5da0a`, zpub)
- UR: SeedSigner's 29-part `crypto-psbt` vector, plus an encode/decode round trip
- PSBT review: SeedSigner's single-sig testnet vector (25,000 in, 24,857 out,
  143 fee), signing, wrong seed and garbage input
- Verify address: BIP84 first receive/change addresses, URIs, testnet,
  foreign addresses, and a different seed loaded afterwards

## Desktop simulator

`src/app` also builds for Linux, in a 240×320 X11 window where the mouse acts
as the touch screen (a white dot shows the pointer). Needs `libx11-dev`.

```bash
pio run -e sim && .pio/build/sim/program
```

Camera: the simulator "sees" the image at `sim/camera.png` (or
`$SIM_CAMERA`), reloaded when the file changes. With the scan screen open:

```bash
cp sim/samples/vector4_standard_12word.png sim/camera.png
```

`sim/samples` holds SeedSigner's public test SeedQRs, a photo of a stamped
plate, and `testseed_height_demise_*` / `testpsbt_height_demise_base64.png`, a
matching seed and testnet PSBT from SeedSigner's tests for trying the signing
flow.

Screenshots of the whole flow, no window:

```bash
SIM_SHOTS=docs/screens .pio/build/sim/program
```

The simulator doesn't model the hardware: SPI timing, the PIO camera,
the 520KB RAM limit and secure boot only show up on the real board.

## Layout

- `src/app`: UI, SeedQR, wallet and QR scanning, shared by both targets
- `src/board`: display (SPI), touch (I2C), camera (stub) and `setup()`/`loop()`
- `src/sim`: X11 window, mouse input and the image-file camera
- `test`: unit tests (native)

## Flash

1. Hold BOOT on the board (bottom edge, right of USB-C seen from the back) and
   plug in USB-C. A drive called `RP2350` appears.
2. Copy `firmware.uf2` onto it. The board reboots into the firmware.

Or with the board already running PlatformIO firmware: `pio run -t upload`.

Serial log: `pio device monitor`.

## What to check on first boot

- The home screen appears with the right colours (if colours look inverted
  or red/blue swapped, adjust `0x21`/`0x36` in `src/board/display.cpp`).
- Buttons react where you touch. If touches are mirrored or swapped, fix the
  mapping in `src/board/touch.cpp` (marked TODO).
- The scan screen stays empty: the camera isn't ported yet.

## Pin map

See `include/board_pins.h`. Taken from Waveshare's demo code
(RP2350-Touch-LCD-2.zip on the Waveshare wiki), which also contains a camera
example (`C/02-CAM`, OV5640 over PIO) to port next.

## Next steps

- Camera: port Waveshare's PIO capture into `src/board/camera.cpp`
- Test against Sparrow end to end (testnet)
- Testnet xpub export (m/84'/1'/0', vpub)
- BBQr input
- Secure boot with picotool
