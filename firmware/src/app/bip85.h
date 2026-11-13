#pragma once
#include <stdint.h>
#include "seedqr.h"

class HDPrivateKey;

// BIP-85 child seeds (https://github.com/bitcoin/bips/blob/master/bip-0085.mediawiki),
// BIP39 application, English: m/83696968'/39'/0'/{words}'/{index}'.
// The same root, word count and index always give the same child; the network
// plays no part.

constexpr uint32_t BIP85_MAX_INDEX = 0x7FFFFFFF;   // hardened, so 31 bits

// words: 12 or 24. Wipe `out` (seedqr_wipe) once it's no longer needed.
bool bip85_bip39_entropy(const HDPrivateKey &root, int words, uint32_t index, SeedEntropy *out);
