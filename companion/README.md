# Goldfish Companion (spike)

A watch-only wallet app for Goldfish, in Tauri 2. It does Sparrow's part of
the job: it links the key from Goldfish's **Connect wallet** QR, syncs over
Electrum or Esplora, shows receive addresses, builds transactions, shows them as an
animated `crypto-psbt` QR, reads the signed one back with the webcam, and
broadcasts. The seed never comes here; this app can't spend on its own.

Spike status: Linux desktop only, single-sig native segwit (`84h`) or
`wsh(sortedmulti)` multisig, signet or mainnet. Not for real funds.

- The watch-only key and the server are saved in `settings.json` in the app's
  data folder (`~/.local/share/dev.goldfish.companion/` on Linux), so the
  wallet reopens on start. Settings → Forget this wallet removes the key.
  The seed never comes here.
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
  one of them. "Show this wallet to Goldfish" shows the descriptor for
  Goldfish's Scan QR (a text QR, or an animated `bytes` UR when long);
  Goldfish keeps it in RAM only, so show it again after a restart. Each
  cosigner's signed PSBT is merged in, and the QR then carries the
  signatures so far for the next one; Broadcast once enough have signed.
- Fees: Fast / Normal / Slow suggestions from the server (next block, ~1 hour,
  ~1 day), fractional sat/vB, and Speed up (RBF) on a pending send, which
  Goldfish signs like any other transaction.

## Layout

- `src-tauri/src/lib.rs`: wallet logic with no UI (BDK, Electrum and Esplora, UR codec,
  QR rendering), with unit tests
- `src-tauri/src/camera.rs`: webcam QR reader (V4L2 + rqrr). Capture runs in
  Rust because WebKitGTK's `getUserMedia` is unreliable
- `src-tauri/src/main.rs`: Tauri commands; `ui/`: the page (plain HTML/JS, no
  build step)
- `src-tauri/src/bin/cli.rs` + `e2e/`: the same logic driven against the
  Goldfish simulator, over QR codes both ways

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
