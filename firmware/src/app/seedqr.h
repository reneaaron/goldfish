#pragma once
#include <stddef.h>
#include <stdint.h>

// SeedQR decoding (https://github.com/SeedSigner/seedsigner/blob/dev/docs/seed_qr/README.md)
//
// Standard SeedQR: numeric QR, each BIP39 word as its 4-digit index (0000-2047),
//                  48 digits for 12 words, 96 for 24.
// Compact SeedQR:  byte QR holding the raw entropy, 16 bytes for 12 words, 32 for 24.
//
// Both decode to BIP39 entropy. The caller turns that into a mnemonic/seed and
// must wipe it (seedqr_wipe) as soon as it is no longer needed.

enum SeedQrResult {
    SEEDQR_OK = 0,
    SEEDQR_NOT_SEEDQR,      // wrong length or characters: probably some other QR
    SEEDQR_BAD_WORD_INDEX,  // a 4-digit group above 2047
    SEEDQR_BAD_CHECKSUM,    // Standard SeedQR whose last word doesn't match
};

struct SeedEntropy {
    uint8_t bytes[32];
    size_t len;             // 16 (12 words) or 32 (24 words)
};

// Decode a scanned QR payload. `data`/`len` are the raw bytes the QR decoder
// returned; for numeric QRs that is the ASCII digit string.
SeedQrResult seedqr_decode(const uint8_t *data, size_t len, SeedEntropy *out);

// Number of BIP39 words for the decoded entropy (12 or 24).
inline int seedqr_word_count(const SeedEntropy &e) { return int(e.len * 3 / 4); }

void seedqr_wipe(SeedEntropy *e);
