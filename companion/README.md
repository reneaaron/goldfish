# Companion app

The wallet app for Goldfish setups, the part Sparrow or Nunchuk plays today:
it holds the watch-only wallet (descriptor, addresses, history), builds
transactions as PSBTs, shows them to Goldfish as QR codes, reads the signed
PSBT back from Goldfish's screen and broadcasts it. Draft brand name **Dove**
(`marketing/companion/`).

Status: 0.1, in Tauri 2, Linux desktop only: single-sig native segwit
(`84h`) or `wsh(sortedmulti)` multisig, signet or mainnet. Not for real funds;
the guides still use Sparrow.

## Rules

- Watch-only. The companion never sees a seed, a passphrase or a private key;
  it gets the xpub or the multisig descriptor from Goldfish's screen.
- Native segwit only, like the signer: single-sig P2WPKH and `wsh(sortedmulti)`.
- The network comes from the descriptor's keys, never from a setting.
- Code says `companion`, never `dove`: package names, identifiers, paths, CI
  jobs. "Dove" appears only in user-facing copy and icons, so a rename touches
  only those.

## What it does

- First use is a short wizard: welcome, one signer or multisig, link (scan
  or paste), for multisig a backup step (the descriptor as text with a Copy
  button: the seeds alone don't bring a multisig wallet back), server (public or your own node, checked before it's saved),
  then a check on Goldfish: the first address for one signer, the
  descriptor for multisig.
- The watch-only key and the server are saved in `settings.json` in the app's
  data folder (`~/.local/share/dev.goldfish.companion/` on Linux), so the
  wallet reopens on start. Settings → Forget this wallet removes the key.
- Optional password (offered in the first-use wizard after the server, or
  Settings → Password): the key and the server then go to
  `settings.age` instead, encrypted in the [age](https://age-encryption.org)
  format with a scrypt passphrase, and `settings.json` is deleted. Dove asks for the password on start and keeps it in memory
  while it runs, to encrypt again on each save. Forgot it? "Start over" on
  the unlock screen deletes `settings.age`; link the wallet again from the
  signer. What the password protects is privacy (balance, addresses, history
  for anyone who gets the disk), not coins: the file only ever holds a
  watch-only key. It does not hide anything from the server, which still sees
  every address Dove asks about, and a deleted `settings.json` may stay
  readable in the disk's free space.
- Server: your own Electrum server (`ssl://host:50002`, `tcp://host:50001`) or
  Esplora API (`https://…`), optionally through a proxy. Tor:
  `socks5://127.0.0.1:9050` for Electrum; the Esplora client takes only an
  HTTP proxy (Tor's `HTTPTunnelPort`). TLS isn't checked against a domain on
  .onion, .local and private addresses, where self-signed certificates are
  normal. Empty means a public Electrum server (Blockstream on mainnet,
  mempool.space on signet): Esplora on mempool.space hangs on `/blocks` in
  the client's HTTP library.
- Broadcast: not through that server, but straight to Bitcoin's p2p
  network over Tor built into Dove, so peers and anyone watching the network
  can't tell which IP it came from. The server still sees it at the next
  sync, since it knows the wallet's addresses. Tor is arti (it keeps its
  state in the app's data folder, `tor/`); it starts when a payment is made,
  so it's up by the time the signed PSBT comes back, and never for a wallet
  that's only looked at. The broadcast runs a short-lived bdk_kyoto node
  that follows headers from the wallet's last block. Each peer connection
  gets its own Tor circuit. The transaction goes to one random peer; it
  counts as sent once it comes back over a second connection that listens
  to what the network relays, and goes out again to a random peer otherwise
  (three rounds of a minute). If Tor or the peers can't be reached, Dove
  says so, keeps the transaction, and offers to send it through the server
  instead. Your own server (.onion, .local or a private address) and regtest
  get it directly, without Tor.
- Multisig: link from the wallet's descriptor (`<0;1>`, `{0,1}` or `0`
  children, checksum checked when present) or from the cosigners' keys plus
  the signatures needed; Goldfish's multisig Connect wallet key (`Vpub`) is
  one of them. "Connect your signers" shows the descriptor for Goldfish's Scan
  QR (a text QR, or an animated `bytes` UR when long); Goldfish keeps it in
  RAM only, so it is connected again whenever it is plugged in. Each
  cosigner's signed PSBT is merged in, and the QR then carries the
  signatures so far for the next one; Broadcast once enough have signed.
- A signer without a camera: the Review screen's ⋮ menu copies the PSBT
  (with the signatures so far) as base64 or saves it as a binary `.psbt`,
  and takes the signed one back from a file (a PSBT, binary, base64 or hex,
  or the finished transaction, such as a Coldcard's `.txn`) or pasted text,
  like a scan. Inputs the other wallet already finalized count once their
  signatures check out.
  For Sparrow with a USB device (Trezor, Ledger) or a Coldcard's SD card.
  Exports add each input's previous transaction, which Trezor and Ledger
  require; Goldfish's QR leaves it out, and it's dropped again on the way
  back. Untested with a real device so far; hardware wallets may also need
  the multisig wallet registered on them first.
- Fees: Fast / Normal / Slow suggestions from the server (next block, ~1 hour,
  ~1 day), fractional sat/vB, and Speed up (RBF) on a pending send, which
  Goldfish signs like any other transaction.
- Update Goldfish (Settings → Update Goldfish), a screen of its own in three
  steps. Check for update downloads the latest release's firmware (the one
  `.uf2` its `SHA256SUMS` lists, `firmware.uf2` so far) by
  tag (nothing goes to GitHub before that) and accepts it only if it matches
  the release's `SHA256SUMS` and is a UF2 image for the RP2350; the tag and
  SHA-256 are shown. That proves the file is the one the release lists, not
  who built it (the build attestation isn't checked here; the flasher page's
  workflow does). The board can't tell Dove its version (no USB stack), so
  you compare the tag with the corner of Goldfish's home screen. Then the
  update-mode steps: plug it into this computer, tap the version, Restart;
  Goldfish comes up as the `RP2350` drive, which Dove finds (`INFO_UF2.TXT`
  under `/run/media/$USER`, `/media/$USER` or `/media`) and copies the file
  onto. The board restarts as soon as the last block is in, so a final flush
  that fails because the drive is gone counts as done. An HTTP proxy in
  Settings is used; a SOCKS one refuses, rather than going around it.
- Look: the draft brand, Dove (mark, wordmark and app icon); its blue (the
  case blue) where Goldfish's screen uses orange, which is otherwise only in
  the mark's feet; and
  Goldfish's own tokens and fonts otherwise.

## Layout

| Part | Where |
|---|---|
| core: wallet logic, no UI (BDK, Electrum and Esplora, descriptors, PSBTs, UR codec, QR rendering) | `src-tauri/src/lib.rs` |
| firmware update: GitHub release download, SHA256SUMS and UF2 checks, the RP2350 drive (tests inside) | `src-tauri/src/firmware.rs` |
| app: Tauri commands, webcam QR reader (V4L2 + rqrr, in Rust because WebKitGTK's `getUserMedia` is unreliable), the page (plain HTML/JS, no build step) | `src-tauri/src/main.rs`, `src-tauri/src/camera.rs`, `ui/` |
| tests for core | `#[cfg(test)]` in `lib.rs` |
| e2e: round trips with the firmware simulator, over QR codes both ways | `e2e/`, driving `src-tauri/src/bin/cli.rs` |

## Shared with the firmware

- `e2e/` drives the simulator through `firmware/e2e/device.py`, and uses
  `firmware/e2e/hostwallet.py` (embit) for the other cosigners.
- Test vectors: read `firmware/test/fixtures/` and `firmware/sim/samples/`
  rather than copying them.
- Marks come from `marketing/companion/`: `ui/dove.svg` is a copy of
  `dove-mark.svg`, and `src-tauri/icons/icon.png` is `dove-icon.svg` drawn at
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

The dock shows a generic icon until a desktop entry names the app: run
`./desktop-entry.sh` once (it writes `goldfish-companion.desktop` and the icon
into `~/.local/share`, launching the debug build of this checkout, so run it
from the main checkout rather than a worktree).

## Release

A `dove-v*` tag (`dove-v0.1.0`) runs `.github/workflows/companion-release.yml`:
it checks the tag against the version in `Cargo.toml` and `tauri.conf.json`
(bump both, and `Cargo.lock`, first), runs the tests, builds the `.deb` and
the AppImage with `cargo tauri build` (the tools Tauri downloads for the
AppImage are fetched first and checked against `appimage-tools.sha256`), and
publishes them with `SHA256SUMS` and a build provenance attestation per file. The release is never marked
latest: Dove's Update Goldfish and the website take the latest release as
Goldfish's, whose tags are `v*`. These builds aren't reproducible, unlike the
firmware.

```bash
gh attestation verify dove-0.1.0-x86_64.AppImage --repo reneaaron/goldfish \
  --signer-workflow reneaaron/goldfish/.github/workflows/companion-release.yml --source-ref refs/tags/dove-v0.1.0
```

## Not done yet

Android/iOS builds, CPFP, coin control and labels, multisig descriptors as
`crypto-output` UR (only text is read), swaps and buying.
