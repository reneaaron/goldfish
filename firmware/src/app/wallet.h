#pragma once
#include <stddef.h>
#include <stdint.h>
#include "seedqr.h"

class HDPrivateKey;

// The one seed this device holds, in RAM only. Nothing is ever written to flash:
// unplugging the device, or wallet_wipe(), removes it.

bool wallet_has_seed();
// Derive the root key from SeedQR entropy (no passphrase). Slow on the RP2350:
// BIP39 runs 2048 rounds of PBKDF2-HMAC-SHA512.
bool wallet_load(const SeedEntropy &e);
void wallet_wipe();
// The root key, for signing. nullptr without a seed. Don't copy it.
const HDPrivateKey *wallet_root();

void wallet_fingerprint(uint8_t out[4]);
void wallet_fingerprint_hex(char out[9]);
int wallet_word_count();

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
