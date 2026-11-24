# Fuzzing and differential testing

Everything an attacker can send reaches the device through the camera:
QR → quirc → UR / base64 / SeedQR / address parser → PSBT review → signing.
These tools test that path on a PC, with AddressSanitizer and UBSan.

## Fuzzers (`fuzz/`)

| Fuzzer | Code under test | Checks beyond "doesn't crash" |
|---|---|---|
| `fuzz_psbt` | `psbt_load` → `psbt_sign` → `psbt_serialize` (libwally PSBT parser) | totals add up; signed PSBT parses, same transaction, signed inputs as reported |
| `fuzz_ur` | `ur_decoder_add`: raw text, structured parts with valid CRC, encode/decode round trip | payload stays in bounds, progress 0–100 %, round trip is exact |
| `fuzz_seedqr` | `seedqr_decode` | decoded entropy re-encodes to the scanned digits; nothing left behind on failure |
| `fuzz_address` | `address_parse` | accepted addresses are 42-char bc1q/tb1q |
| `fuzz_qr` | quirc: cell-grid decoding and small images through `qrscan_frame` | payload length in bounds |
| `fuzz_multisig` | with a 2-of-3 cosigner seed: `multisig_load` (descriptor text), `multisig_load_ur_output` (crypto-output CBOR), PSBT review and signing with the descriptor loaded, `address_parse` + `address_verify_step` for P2WSH | a loaded descriptor is usable: M-of-N in range, our key found, scripts and addresses derive; the `fuzz_psbt` checks on multisig PSBTs |

```bash
cd firmware
pio pkg install -e test                    # fetches libwally once
uv run --with segno --with embit fuzz/seeds.py   # starting corpus from the test vectors
fuzz/build.sh                              # GCC + ASan/UBSan
fuzz/run.sh 600                            # all fuzzers, 10 minutes, restarts after crashes
fuzz/triage.sh                             # group crashes by cause
fuzz/out/fuzz_ur fuzz/crashes/ur/crash-…   # reproduce one
fuzz/out/fuzz_qr fuzz/regressions/qr/*     # inputs that crashed before a fix
```

`seeds.py` also writes malformed inputs in the shapes that crashed
SeedSigner: a cosigner key without a key origin, empty derivation paths (in
descriptors, crypto-output keys and PSBT key paths), sign-message QRs (which
this device must refuse), truncated or oversized UR parts and wrong CRCs,
PSBTs at and past `PSBT_MAX_OUTPUTS` and the four-claim limit, and SeedQRs of
off-by-one lengths.

Inputs that crashed a fuzzer before a fix live in `regressions/<fuzzer>/`;
`seeds.py` copies them into the corpus.

UBSan runs with `abort_on_error=1`, so its findings are saved as crashes
like ASan's (without it, GCC's UBSan exits without the driver noticing).

GCC has no libFuzzer, so `driver.cpp` is a small coverage-guided fuzzer
built on `-fsanitize-coverage=trace-pc`. The harnesses use the libFuzzer
interface; with clang installed you can build them with `-fsanitize=fuzzer`
instead for much faster runs.

libwally is built from the firmware's own amalgamation and config, except
that `wally_config/config.h` turns off its x86 SSE4 SHA-256 assembly, which
crashes under AddressSanitizer (the RP2350 never uses it).

## Differential PSBT oracle (`oracle/`)

`oracle.py` generates random single-sig PSBTs with embit (honest ones, and
attacks: fake change, foreign inputs, taproot inputs, change in other
accounts or far past the gap limit, mixed networks, amounts above 21M BTC,
more than 8 outputs), runs them through `psbt_cli` (the firmware's review and
signing code), and compares with what embit says is true:

- **hard**: every amount, address, change/self/spend label and the fee match
  the real transaction; PSBTs the firmware is meant to refuse (inputs or
  change that aren't really ours, change outside the BIP84 account, more than
  8 outputs, totals above 21M BTC) are refused; every
  signature verifies for exactly the reviewed transaction with SIGHASH_ALL.
- **soft**: policy issues that get through to signing (counted only).

```bash
cd firmware
oracle/build.sh
uv run --with embit oracle/oracle.py -n 3000 --seed 1
```
