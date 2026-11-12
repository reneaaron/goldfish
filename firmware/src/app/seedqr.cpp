#include "seedqr.h"
#include <string.h>
#include <Hash.h>      // sha256 from uBitcoin

// Wipe that the compiler can't optimise away.
static void secure_zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

void seedqr_wipe(SeedEntropy *e) { secure_zero(e, sizeof(*e)); }

// Standard SeedQR: 4-digit word indexes -> 11-bit groups -> entropy || checksum.
static SeedQrResult decode_standard(const uint8_t *d, size_t len, SeedEntropy *out) {
    const size_t words = len / 4;
    const size_t ent_bytes = words == 12 ? 16 : 32;
    const size_t cs_bits = ent_bytes / 4;          // 4 bits for 12 words, 8 for 24
    uint8_t bits[33] = {0};                        // 24 * 11 = 264 bits
    size_t bitpos = 0;

    for (size_t w = 0; w < words; w++) {
        uint16_t idx = 0;
        for (int k = 0; k < 4; k++) idx = idx * 10 + (d[w * 4 + k] - '0');
        if (idx > 2047) { secure_zero(bits, sizeof(bits)); return SEEDQR_BAD_WORD_INDEX; }
        for (int b = 10; b >= 0; b--, bitpos++)
            if (idx & (1u << b)) bits[bitpos / 8] |= 0x80 >> (bitpos % 8);
    }

    uint8_t hash[32];
    sha256(bits, ent_bytes, hash);
    // Compare the checksum bits that follow the entropy with the hash prefix.
    uint8_t given = bits[ent_bytes];
    uint8_t expected = hash[0];
    uint8_t mask = uint8_t(0xFF << (8 - cs_bits));
    bool ok = (given & mask) == (expected & mask);

    if (ok) {
        memcpy(out->bytes, bits, ent_bytes);
        out->len = ent_bytes;
    }
    secure_zero(bits, sizeof(bits));
    secure_zero(hash, sizeof(hash));
    return ok ? SEEDQR_OK : SEEDQR_BAD_CHECKSUM;
}

size_t seedqr_standard_digits(const SeedEntropy &e, char out[97]) {
    uint8_t bits[33] = {0};
    memcpy(bits, e.bytes, e.len);
    uint8_t hash[32];
    sha256(e.bytes, e.len, hash);
    bits[e.len] = hash[0];                         // checksum: first len/4 bits of the hash
    const size_t words = e.len * 3 / 4;
    for (size_t w = 0; w < words; w++) {
        uint16_t idx = 0;
        for (size_t b = w * 11; b < w * 11 + 11; b++) idx = uint16_t(idx << 1 | ((bits[b / 8] >> (7 - b % 8)) & 1));
        for (int k = 3; k >= 0; k--, idx /= 10) out[w * 4 + k] = char('0' + idx % 10);
    }
    out[words * 4] = 0;
    secure_zero(bits, sizeof(bits));
    secure_zero(hash, sizeof(hash));
    return words * 4;
}

SeedQrResult seedqr_decode(const uint8_t *data, size_t len, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if (len == 48 || len == 96) {
        bool digits = true;
        for (size_t i = 0; i < len; i++) digits &= data[i] >= '0' && data[i] <= '9';
        if (digits) return decode_standard(data, len, out);
    }
    // Compact SeedQR is raw bytes. A 16/32 byte payload of digits is ambiguous in
    // theory; SeedSigner treats byte-mode payloads of these lengths as Compact.
    if (len == 16 || len == 32) {
        memcpy(out->bytes, data, len);
        out->len = len;
        return SEEDQR_OK;
    }
    return SEEDQR_NOT_SEEDQR;
}
