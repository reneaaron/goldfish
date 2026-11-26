# Companion app

The wallet app for Goldfish setups, the part Sparrow or Nunchuk plays today:
it holds the watch-only wallet (descriptor, addresses, history), builds
transactions as PSBTs, shows them to Goldfish as QR codes, reads the signed
PSBT back from Goldfish's screen and broadcasts it. Draft brand name **Dove**
(`marketing/companion/`).

Status: nothing built yet; no stack chosen. Until then, Sparrow is the wallet
app the guides use.

## Rules

- Watch-only. The companion never sees a seed, a passphrase or a private key;
  it gets the xpub or the multisig descriptor from Goldfish's screen.
- Native segwit only, like the signer: single-sig P2WPKH and `wsh(sortedmulti)`.
- The network comes from the descriptor's keys, never from a setting.
- Code says `companion`, never `dove`: package names, identifiers, paths, CI
  jobs. "Dove" appears only in user-facing copy and icons, so a rename touches
  only those.

## Layout

| Folder | Holds |
|---|---|
| `core/` | Wallet logic, no UI: descriptor and xpub import, address derivation, PSBT building and finalizing, UR encode/decode, coin selection |
| `app/` | The UI shell (screens, camera, QR display); calls `core` |
| `tests/` | Unit tests for `core` |
| `e2e/` | Companion ↔ Goldfish round trips against the firmware simulator |

## Shared with the firmware

- Test vectors: read `firmware/test/fixtures/` and `firmware/sim/samples/`
  (SeedSigner test SeedQRs, a matching testnet and mainnet PSBT) rather than
  copying them.
- `firmware/e2e/hostwallet.py` is an embit stand-in for the wallet app that the
  firmware's end-to-end tests drive today. Once `core` exists, those tests can
  use it instead.
- Icons come from `marketing/companion/`.
