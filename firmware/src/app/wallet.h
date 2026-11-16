#pragma once
#include <stddef.h>
#include <stdint.h>
#include "seedqr.h"

struct ext_key;   // libwally-core, <wally_bip32.h>

// The one seed this device holds, in RAM only. Nothing is ever written to flash:
// unplugging the device, or wallet_wipe(), removes it.

bool wallet_has_seed();
// Derive the root key from SeedQR entropy (no passphrase). Slow on the RP2350:
// BIP39 runs 2048 rounds of PBKDF2-HMAC-SHA512.
bool wallet_load(const SeedEntropy &e);
void wallet_wipe();
// BIP-39 passphrase ("25th word"): derive the root key again from the loaded
// seed with `pass`; "" goes back to the seed alone. As slow as wallet_load.
// The bytes are used as given (no NFKD), so callers pass ASCII. Only the
// resulting key is kept, never the passphrase; wallet_wipe() removes it too.
bool wallet_set_passphrase(const char *pass);
bool wallet_has_passphrase();
// The root key, for signing. nullptr without a seed. Don't copy it.
const struct ext_key *wallet_root();

// Fingerprint of a seed without loading it (as slow as wallet_load).
bool seed_fingerprint_hex(const SeedEntropy &e, char out[9]);

void wallet_fingerprint(uint8_t out[4]);
void wallet_fingerprint_hex(char out[9]);
int wallet_word_count();
// Copy of the loaded seed's entropy, for making a QR backup. The caller wipes it.
bool wallet_entropy(SeedEntropy *out);

// Network for the xpub export. Testnet also covers signet and Mutinynet (same
// coin type, xpub version and tb1 addresses). Kept in RAM like everything else:
// back to mainnet on every boot. Signing and Verify address don't use it; they
// read the network from the PSBT's derivation paths or from the address.
bool wallet_testnet();
void wallet_set_testnet(bool testnet);

// Native segwit single-sig account, as SeedSigner's static export:
// mainnet "[fingerprint/84h/0h/0h]zpub...", testnet "[fingerprint/84h/1h/0h]vpub...".
// Returns length written.
size_t wallet_export_xpub(char *out, size_t cap);
