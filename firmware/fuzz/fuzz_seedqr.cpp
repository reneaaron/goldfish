// SeedQR decoding. For Standard SeedQRs that decode, re-derive the 4-digit
// word indexes from the entropy (plus its BIP39 checksum) and check they match
// what was scanned, so a wrong seed can never load silently.
#include <stdint.h>
#include <string.h>
#include <wally_crypto.h>
#include "../src/app/seedqr.h"
#include "fuzz_check.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    SeedEntropy e;
    SeedQrResult r = seedqr_decode(data, size, &e);
    if (r != SEEDQR_OK) {
        static const SeedEntropy zero = {};
        FUZZ_CHECK(memcmp(&e, &zero, sizeof(e)) == 0);   // nothing half-decoded left behind
        return 0;
    }
    FUZZ_CHECK(e.len == 16 || e.len == 32);
    bool digits = (size == 48 || size == 96);
    for (size_t i = 0; digits && i < size; i++) digits = data[i] >= '0' && data[i] <= '9';
    if (!digits) {
        FUZZ_CHECK(size == e.len && memcmp(data, e.bytes, e.len) == 0);
        return 0;
    }
    FUZZ_CHECK(size / 4 == size_t(seedqr_word_count(e)));
    uint8_t bits[33] = {0};
    memcpy(bits, e.bytes, e.len);
    uint8_t h[32];
    FUZZ_CHECK(wally_sha256(e.bytes, e.len, h, sizeof(h)) == WALLY_OK);
    bits[e.len] = h[0];
    for (size_t w = 0; w < size / 4; w++) {
        unsigned idx = 0;
        for (size_t b = 0; b < 11; b++) {
            size_t bit = w * 11 + b;
            idx = (idx << 1) | ((bits[bit / 8] >> (7 - bit % 8)) & 1);
        }
        char want[5];
        snprintf(want, sizeof(want), "%04u", idx);
        FUZZ_CHECK(memcmp(want, data + 4 * w, 4) == 0);
    }
    return 0;
}
