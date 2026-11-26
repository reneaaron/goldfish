# Security

Goldfish is early and not for real funds. The code hasn't been reviewed or
audited.

## Reporting a vulnerability

Report it privately through
[GitHub's private vulnerability reporting](https://github.com/reneaaron/goldfish/security/advisories/new),
not in a public issue. That goes for anything that could leak a seed or key,
sign something the screen didn't show, accept a PSBT output that isn't what it
claims, or get a different file onto the web flasher than the release built.

This is a one-person project: replies are best effort, and there is no bounty.

Only the latest release is fixed.

## What it protects, and what it doesn't

- Nothing is written to flash. The seed, the passphrase-derived key and the
  multisig descriptor live in RAM and go when the board is unplugged or wiped.
  While it's powered they are in RAM.
- The board has no Wi-Fi or Bluetooth hardware, and the firmware has no USB
  stack. It is still USB-powered, and its ROM bootloader stays reachable over
  USB.
- There is no secure boot: the bootloader takes any UF2. Whoever can hold
  BOOT and plug the board in can flash other firmware. The firmware doesn't use
  the RP2350's secure boot or lock its debug port.
- PSBT review follows SeedSigner's rules: every input must belong to the
  loaded seed, an output counts as change only if the seed re-derives that
  exact script, and multisig scripts come from the confirmed descriptor, never
  the PSBT. See [`firmware/README.md`](firmware/README.md).
- The browser simulator (the website's hero, [`firmware/web/sim.js`](firmware/web/sim.js)) is a demo. Don't type a real seed into
  it.

## Checking a release before flashing

Each `v*` tag gets a GitHub release built by
[`release.yml`](.github/workflows/release.yml), with `firmware.uf2`,
`SHA256SUMS` and a build provenance attestation:

```bash
gh attestation verify firmware.uf2 --repo reneaaron/goldfish \
  --signer-workflow reneaaron/goldfish/.github/workflows/release.yml --source-ref refs/tags/v0.1.0
```

The build is reproducible, so building the tag yourself gives the same hash
(see [`firmware/README.md`](firmware/README.md)). The web flasher shows a hash
too, but the page and the file come from the same place, so check against the
release.
