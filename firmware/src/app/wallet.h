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

// Fingerprint of a seed without loading it (as slow as wallet_load).
bool seed_fingerprint_hex(const SeedEntropy &e, char out[9]);

void wallet_fingerprint(uint8_t out[4]);
void wallet_fingerprint_hex(char out[9]);
int wallet_word_count();
// Copy of the loaded seed's entropy, for making a QR backup. The caller wipes it.
bool wallet_entropy(SeedEntropy *out);

// Native segwit single-sig account (m/84'/0'/0'), as SeedSigner's static
// export: "[fingerprint/84h/0h/0h]zpub...". Returns length written.
size_t wallet_export_xpub(char *out, size_t cap);
