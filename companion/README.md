# Companion app

The wallet app for Goldfish setups, the part Sparrow or Nunchuk plays today:
it holds the watch-only wallet (descriptor, addresses, history), builds
transactions as PSBTs, shows them to Goldfish as QR codes, reads the signed
PSBT back from Goldfish's screen and broadcasts it. Draft brand name **Bowl**
(`marketing/companion/`).

Status: a spike in Tauri 2, Linux desktop only: single-sig native segwit
(`84h`) or `wsh(sortedmulti)` multisig, signet or mainnet. Not for real funds;
the guides still use Sparrow.

## Rules

- Watch-only. The companion never sees a seed, a passphrase or a private key;
  it gets the xpub or the multisig descriptor from Goldfish's screen.
- Native segwit only, like the signer: single-sig P2WPKH and `wsh(sortedmulti)`.
- The network comes from the descriptor's keys, never from a setting.
- Code says `companion`, never `bowl`: package names, identifiers, paths, CI
  jobs. "Bowl" appears only in user-facing copy and icons, so a rename touches
  only those.

## What it does

- The watch-only key and the server are saved in `settings.json` in the app's
  data folder (`~/.local/share/dev.goldfish.companion/` on Linux), so the
  wallet reopens on start. Settings → Forget this wallet removes the key.
- Server: your own Electrum server (`ssl://host:50002`, `tcp://host:50001`) or
  Esplora API (`https://…`), optionally through a proxy. Tor:
  `socks5://127.0.0.1:9050` for Electrum; the Esplora client takes only an
  HTTP proxy (Tor's `HTTPTunnelPort`). TLS isn't checked against a domain on
  .onion, .local and private addresses, where self-signed certificates are
  normal. Empty means a public Electrum server (Blockstream on mainnet,
  mempool.space on signet): Esplora on mempool.space hangs on `/blocks` in
  the client's HTTP library.
- Multisig: link from the wallet's descriptor (`<0;1>`, `{0,1}` or `0`
  children, checksum checked when present) or from the cosigners' keys plus
  the signatures needed; Goldfish's multisig Connect wallet key (`Vpub`) is
  one of them. "Connect Goldfish" shows the descriptor for Goldfish's Scan
  QR (a text QR, or an animated `bytes` UR when long); Goldfish keeps it in
  RAM only, so connect again each time it's plugged in. Each
  cosigner's signed PSBT is merged in, and the QR then carries the
  signatures so far for the next one; Broadcast once enough have signed.
- Fees: Fast / Normal / Slow suggestions from the server (next block, ~1 hour,
  ~1 day), fractional sat/vB, and Speed up (RBF) on a pending send, which
  Goldfish signs like any other transaction.
- Look: Bowl's mark, wordmark and app icon; its blue (the case blue) where
  Goldfish's screen uses orange, which in Bowl is only the fish's eye; and
  Goldfish's own tokens and fonts otherwise.

## Layout

| Part | Where |
|---|---|
| core: wallet logic, no UI (BDK, Electrum and Esplora, descriptors, PSBTs, UR codec, QR rendering) | `src-tauri/src/lib.rs` |
| app: Tauri commands, webcam QR reader (V4L2 + rqrr, in Rust because WebKitGTK's `getUserMedia` is unreliable), the page (plain HTML/JS, no build step) | `src-tauri/src/main.rs`, `src-tauri/src/camera.rs`, `ui/` |
| tests for core | `#[cfg(test)]` in `lib.rs` |
| e2e: round trips with the firmware simulator, over QR codes both ways | `e2e/`, driving `src-tauri/src/bin/cli.rs` |

## Shared with the firmware

- `e2e/` drives the simulator through `firmware/e2e/device.py`, and uses
  `firmware/e2e/hostwallet.py` (embit) for the other cosigners.
- Test vectors: read `firmware/test/fixtures/` and `firmware/sim/samples/`
  rather than copying them.
- Marks come from `marketing/companion/`: `ui/bowl.svg` is a copy of
  `bowl-mark.svg`, and `src-tauri/icons/icon.png` is `bowl-icon.svg` drawn at
  256 px.

## Run

Needs Rust, the Tauri Linux libraries (`libwebkit2gtk-4.1-dev` and friends),
and libclang (the V4L2 bindings are generated at build time).

```bash
cd src-tauri
cargo run                 # the app; GOLDFISH_CAMERA=/dev/video2 for another camera
cargo test                # logic tests
cd .. && uv run --with-requirements ../firmware/e2e/requirements.txt pytest e2e   # with the simulator
```

## Not done yet

Android/iOS builds, CPFP, coin control and labels, multisig descriptors as
`crypto-output` UR (only text is read), swaps and buying.
