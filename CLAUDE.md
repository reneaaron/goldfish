# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Goldfish ("Sign and forget."): a stateless, camera-based Bitcoin signer for the Waveshare RP2350-Touch-LCD-2 (240×320 touch screen, OV5640 camera, no radio). The seed lives in RAM only; all I/O is QR codes through the camera and the screen. Status: works in the desktop simulator; on one real board, QR scanning and a 2-of-3 multisig with Sparrow on signet (set up, signed, broadcast) work, and single-sig transactions have been signed with Sparrow, on mainnet too; not for real funds.

- `firmware/`: PlatformIO project (the only code that builds and has tests)
- `companion/`: the watch-only wallet app for Goldfish setups (replaces Sparrow, Nunchuk); draft brand name Bowl, marks in `marketing/companion/`. Only a skeleton so far, no stack chosen; `companion/README.md` has its rules and layout. Code says `companion`, never `bowl`, so a rename touches only copy and icons
- `case/`: printed case in gcad (`.gcad` parts, `.gasm` assembly)
- `hardware/`: parts list, board notes
- `flasher/`: one static page that copies `firmware.uf2` onto the board's BOOT drive
- `parts/`, `assemble/`, `start/`: the website's build guide: what to buy and the case to print (`parts/files` is a symlink to `case/exports`; the case picture is a landing render, and the 3D view loads three.js from jsDelivr only when clicked), connecting the camera and closing the case, then first use (a seed, then Sparrow as the wallet app; its screenshots are the landing page's `img/screens`, retaken from the latest release on deploy, and its button and screen names must match `ui.cpp`). Keep `parts/` in step with `hardware/README.md` and the printing notes in `case/README.md`
- `landing/`: the project website's landing page; `.github/workflows/pages.yml` publishes it on GitHub Pages (landing at `/`, flasher with the latest GitHub release's firmware at `/flasher/`, cards at `/cards/`, the steel backup guide at `/metal/`, the build guide at `/parts/`, `/assemble/` and `/start/`, the browser simulator built from `main` at `/sim/`, which the landing hero runs) on pushes to `main` that touch the site and after each release. `.github/workflows/release.yml` builds a pushed `v*` tag (no cache) and publishes `firmware.uf2`, `SHA256SUMS` and a build provenance attestation as a GitHub release, after checking the toolchain archives against `firmware/toolchain.sha256` (update it when bumping the platform); the site workflow checks both before offering the file. Actions are pinned by commit SHA (Dependabot proposes updates); `main` takes changes only through PRs with green CI, and `v*` tags can't be moved or deleted
- `firmware/web/`: `Makefile` for the browser (WebAssembly) build of the firmware, plus `sim.js`, which runs it on the landing hero's screen after "Plug it in"
- `cards/`: one self-contained page (no external requests) that prints blank cards for 12-word seeds only: the 21×21 SeedQR grid (full page or wallet cards) and word cards; the grid's labels and part lines must match the device's `build_backup_block` in `ui.cpp`
- `metal/`: one short page on stamping a 12-word seed into M8 × 24 stainless washers (DIN 9021) on an M8 × 35 bolt: what to buy, three steps, an optional printed jig and a 12-washer box (`metal/files`, made by `hardware/stackbox_12.scad`). Its picture is `marketing/renders/metal.py`. No fire claims beyond the melting point until a stack has been tested; the plan and open tests are in `hardware/metal-backup-plan.md`
- `marketing/`: Blender render scripts that consume `case/exports/case.stl` and `firmware/docs/screens/*.png`

## Commands

All from `firmware/`:

```bash
pio test -e test                           # all unit tests (native, Unity)
pio test -e test -f test_psbt              # one suite (folder name under test/)
uv run --with-requirements e2e/requirements.txt pytest e2e                      # end-to-end, against the simulator (builds it)
uv run --with-requirements e2e/requirements.txt pytest e2e/test_multisig.py -k <name>   # one e2e test
pio run -e sim && .pio/build/sim/program   # simulator: 240×320 X11 window, mouse = touch (needs libx11-dev)
pio run                                    # board firmware -> .pio/build/rp2350_touch_lcd_2/firmware.uf2
pio run -t upload                          # flash a board already running this firmware
SIM_SHOTS=docs/screens .pio/build/sim/program   # headless scripted run, writes a screenshot per screen
```

Simulator camera: it decodes the image at `sim/camera.png` (or `$SIM_CAMERA`), reloaded on change. With a scan screen open, `cp sim/samples/<file>.png sim/camera.png`. `sim/samples` has handwritten test pages (`handwritten_page_demo.png`: the height demise seed, which the landing hero offers), SeedSigner's public test SeedQRs and a matching seed + testnet PSBT (`testseed_height_demise_*`, `testpsbt_height_demise_base64.png`), plus the same PSBT on mainnet (`mainpsbt_height_demise_base64.png`, from `sim/make_mainnet_psbt.py`) that `SIM_SHOTS` signs.

Case: `gcad check stateless-signer.gasm` (add `--set clip=0` for the screw variant), `gcad export parts/case.gcad exports/case.stl`.

Renders: `blender -b -P marketing/renders/render.py` (`ONLY=1,3 SAMPLES=64 PCT=50` for a quick preview).

## Firmware architecture

Three PlatformIO environments share one source tree, split by `build_src_filter`:

| Env | Sources | Purpose |
|---|---|---|
| `rp2350_touch_lcd_2` (default) | `src/app` + `src/board` | Arduino core (earlephilhower), SPI display, I2C touch, PIO camera |
| `sim` | `src/app` + `src/sim` | Linux X11 window, mouse input, image-file camera |
| `test` | an explicit list of `src/app/*.cpp` | native Unity tests, no LVGL |

Not a PlatformIO env: `web/Makefile` builds `src/app` + `src/web` with Emscripten (`pio pkg install -e sim` first for LVGL and libwally, then `make -C web -j`); `src/web` implements the same platform hooks as `src/sim` against a canvas and a picture the page hands over.

Rules that follow from this:

- `src/app` must not touch hardware. The platform boundary is small: the two camera hooks in `app/qrscan.h` (`platform_camera_frame`, `platform_camera_stop`), implemented in `src/board`, `src/sim` and `src/web`; `platform_parallel` in `app/word_reader.h` (the board runs half of a word's network on its second core; a weak default in `word_reader.cpp` runs both halves in turn for sim and tests); `platform_backlight` in `app/ui.h` (PWM on the board, set from the brightness slider; a no-op in sim and web); plus each target's own `main` that sets up LVGL display/input and calls `ui_build()`.
- The `test` env lists its source files one by one in `platformio.ini`. A new logic module in `src/app` has to be added to that list, and must not include LVGL: `ui.cpp` and `theme.cpp` are the only LVGL-dependent app files and are deliberately not in the test build.
- The board's heap is small (~110 KB after static data, 92 KB of it the app's). The sim gives the app exactly `APP_HEAP_BYTES` (`include/heap_budget.h`, `src/sim/heap.cpp` via `--wrap=malloc`) and fits its camera to the board's 240x180 view, so "Out of memory" shows up in the sim and e2e; `heap_check.py` fails the board build if static data eats into that budget. New memory on the scan path: borrow idle static buffers (as the word reader borrows `preview_buf`) rather than adding heap.
- `version.py` (a PlatformIO pre-script for the board and sim builds) writes the git tag or short commit hash to the ignored `include/version.h`; the home screen shows it.
- Each test suite is `test/test_<name>/test_main.cpp` with its own `main()`; shared vectors live in `test/fixtures/`.
- `e2e/` (pytest) plays the user and a wallet app against the `sim` binary run headless with `SIM_CONTROL=1`: commands on stdin (`run_control()` in `src/sim/main.cpp`), on a fake clock that only advances when a command says so. `e2e/device.py` reads the screen, touches it and feeds QR images to the camera; `e2e/hostwallet.py` is an embit-based stand-in for Sparrow that never sees the seed. `device.py` parses the `ICON_*` defines in `theme.h` and the `UI_*` enum in `ui.h` by regex, so keep those in their current one-per-line form. A failing test saves the screen as `e2e/failure_*.png`.

### Layers inside `src/app`

- `ui.cpp` (one large file) holds every screen. `UiScreen` in `ui.h` enumerates them and `ui_goto()` switches; the flow diagram is the comment at the top of `ui.h`. `ui.h` also exports action functions (`ui_swipe_sign`, `ui_enter_word`, `ui_passphrase_key`, …) that mirror the on-screen controls so the simulator's scripted `SIM_SHOTS` run in `src/sim/main.cpp` can drive the whole flow without pointer events. A new screen or control usually needs its step added there to appear in `docs/screens`.
- `theme.h/.cpp`: colours, metrics, fonts, widget helpers (namespace `th`). Icon codepoints are listed in `theme.h`; fonts in `fonts/` are generated with `lv_font_conv` (see `fonts/README.md`).
- Logic modules, all plain C++ over libwally with module-level state in RAM:
  - `wallet`: the one loaded seed/root key, optional BIP-39 passphrase (only the derived key is kept), xpub export
  - `seedqr`, `seed_create`, `bip85`, `bip39_english`: seed in/out and creation (no RNG anywhere: all entropy is user input)
  - `qrscan` (quirc), `ur` (animated UR `crypto-psbt` / `crypto-output` / `bytes`)
  - `psbt_review`: parse, review and sign; `multisig`: the `wsh(sortedmulti)` descriptor; `address_verify`; `amount` (BIP177 `₿` formatting plus legacy BTC)
  - `page_reader` (finds handwritten words on a photo of a backup) and `word_reader` (reads one as a whole word with a small int8 network, `word_model.h`, generated by `handwriting/` in Python; see `handwriting/README.md`)

### Security invariants (don't weaken these in passing)

- Nothing is written to flash. Seed, passphrase-derived key, Connect wallet's network and key type, and the multisig descriptor are all RAM-only and go on wipe or unplug.
- PSBT review follows SeedSigner's rules: every input must belong to the loaded seed; an output counts as change only if the seed re-derives that exact output script; an output that claims our fingerprint but doesn't verify blocks signing. At most one input's amount may rest on the PSBT alone (no full previous transaction); with two, false amounts over two signings could hide a fee.
- Multisig inputs and change are checked against scripts derived from the confirmed descriptor, never the PSBT's own witness scripts. The descriptor is bound to the root key it was checked with (other seed or other passphrase = not used).
- Native segwit only (single-sig P2WPKH, `wsh(sortedmulti)`). The network comes from the PSBT's derivation paths, the address, or the descriptor's keys, not from the Connect wallet setting.

### Libraries

- libwally-core is pinned by commit in `platformio.ini`. PlatformIO only fetches it (`lib_ignore`); `lib/wally` builds it from the single-file amalgamation with `BUILD_MINIMAL` and no Elements (`build.py`, `config/config.h`).
- quirc is vendored in `lib/quirc`. LVGL 9.2.2, configured in `include/lv_conf.h`. Pins in `include/board_pins.h` (from Waveshare's demo code).

## Docs and copy

- `firmware/README.md` documents behaviour in detail (flows, supported formats, what each test suite covers). Keep it and the root `README.md` in step with behaviour changes.
- `firmware/docs/screens/*.png` come from the `SIM_SHOTS` run (it writes PPM; convert to PNG) and feed both READMEs and the marketing renders.
- Copy must match what the hardware and firmware really do (`marketing/README.md`): "no Wi-Fi or Bluetooth hardware" and "nothing is saved" are true; don't say "air-gapped" unqualified while it's USB-powered, and don't claim audits, secure boot, or wallet compatibility that hasn't been tested.
