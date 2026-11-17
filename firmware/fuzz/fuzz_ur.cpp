// UR crypto-psbt decoder. Includes ur.cpp directly to reach its static
// bytewords/CBOR helpers. First byte picks the mode:
//   0: raw QR text, parts separated by '\n', fed to one decoder
//   1: structured: [len][CBOR part body]..., each wrapped in valid bytewords+CRC
//      so mutations get past the checksum and into the fountain decoder
//   2: round trip: encode [frag_len][data] as parts, feed them back shuffled
#include "../src/app/ur.cpp"
#include <vector>
#include "fuzz_check.h"

static UrDecoder dec;

static void check_complete(UrResult r, const uint8_t *payload, size_t plen) {
    if (r != UR_COMPLETE) return;
    FUZZ_CHECK(payload >= dec.message);
    FUZZ_CHECK(payload + plen <= dec.message + sizeof(dec.message));
    FUZZ_CHECK(plen <= UR_MAX_MESSAGE);
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < plen; i++) sink ^= payload[i];   // ASan checks every byte
    (void)sink;
}

static void feed(const char *s, size_t n) {
    const uint8_t *payload = nullptr;
    size_t plen = 0;
    UrResult r = ur_decoder_add(&dec, s, n, &payload, &plen);
    check_complete(r, payload, plen);
    int pct = ur_decoder_percent(&dec);
    FUZZ_CHECK(pct >= 0 && pct <= 100);
}

static void mode_text(const uint8_t *d, size_t n) {
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || d[i] == '\n') {
            // own heap copy so ASan sees overreads of each part
            std::vector<char> part(d + start, d + i);
            feed(part.data(), part.size());
            start = i + 1;
        }
    }
}

static void mode_struct(const uint8_t *d, size_t n) {
    static char text[2 * 600 + 64];
    size_t i = 0;
    while (i < n) {
        size_t len = d[i++];
        if (len > n - i) len = n - i;
        int h = snprintf(text, sizeof(text), "ur:crypto-psbt/1-2/");
        size_t w = bytewords_encode(d + i, len, text + h, sizeof(text) - size_t(h));
        i += len;
        if (!w) continue;
        std::vector<char> part(text, text + h + w);
        feed(part.data(), part.size());
    }
}

static void mode_roundtrip(const uint8_t *d, size_t n) {
    if (n < 2) return;
    size_t frag = 10 + d[0] % 200;
    uint8_t order_seed = d[1];
    d += 2; n -= 2;
    size_t count = ur_part_count(n, frag);
    if (count == 0 || count > UR_MAX_FRAGMENTS) return;
    std::vector<uint32_t> order(count);
    for (size_t k = 0; k < count; k++) order[k] = uint32_t(k + 1);
    // deterministic shuffle
    uint32_t s = order_seed * 2654435761u + 1;
    for (size_t k = count; k > 1; k--) { s = s * 1103515245u + 12345u; std::swap(order[k - 1], order[s % k]); }
    // repeat a few parts like a camera would
    order.push_back(order[0]);
    static char text[2 * 600 + 64];
    const uint8_t *payload = nullptr;
    size_t plen = 0;
    UrResult r = UR_NOT_UR;
    for (uint32_t seq : order) {
        size_t w = ur_encode_part(d, n, frag, seq, text, sizeof(text));
        FUZZ_CHECK(w > 0);
        r = ur_decoder_add(&dec, text, w, &payload, &plen);
        if (r == UR_COMPLETE) break;
        FUZZ_CHECK(r == UR_PROGRESS);
    }
    FUZZ_CHECK(r == UR_COMPLETE);
    FUZZ_CHECK(plen == n && memcmp(payload, d, n) == 0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    ur_decoder_reset(&dec);
    switch (data[0] % 3) {
        case 0: mode_text(data + 1, size - 1); break;
        case 1: mode_struct(data + 1, size - 1); break;
        case 2: mode_roundtrip(data + 1, size - 1); break;
    }
    return 0;
}
