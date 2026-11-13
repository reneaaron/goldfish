#include "seed_create.h"
#include <string.h>
#include <wally_crypto.h>

extern const char *const BIP39_ENGLISH[2048];   // bip39_english.cpp

int seed_dice_rolls(int words) { return words == 24 ? 99 : 50; }
int seed_coin_flips(int words) { return words == 24 ? 256 : 128; }

static bool all_in(const char *s, size_t n, char lo, char hi) {
    for (size_t i = 0; i < n; i++)
        if (s[i] < lo || s[i] > hi) return false;
    return true;
}

static void hash_to_entropy(const char *s, size_t n, int words, SeedEntropy *out) {
    uint8_t h[32];
    wally_sha256((const uint8_t *)s, n, h, 32);
    out->len = words == 24 ? 32 : 16;
    memcpy(out->bytes, h, out->len);
    secure_zero(h, sizeof(h));
}

bool seed_from_dice(const char *rolls, size_t n, int words, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if ((words != 12 && words != 24) || n != (size_t)seed_dice_rolls(words) || !all_in(rolls, n, '1', '6')) return false;
    hash_to_entropy(rolls, n, words, out);
    return true;
}

bool seed_from_coins(const char *flips, size_t n, int words, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if ((words != 12 && words != 24) || n != (size_t)seed_coin_flips(words) || !all_in(flips, n, '0', '1')) return false;
    hash_to_entropy(flips, n, words, out);
    return true;
}

const char *seed_word(int index) { return index >= 0 && index < 2048 ? BIP39_ENGLISH[index] : ""; }

int seed_words_with_prefix(const char *prefix, int *first) {
    size_t n = strlen(prefix);
    int count = 0;
    *first = -1;
    for (int i = 0; i < 2048; i++) {
        if (strncmp(BIP39_ENGLISH[i], prefix, n) != 0) continue;
        if (*first < 0) *first = i;
        count++;
    }
    return count;
}

int seed_final_word_flips(int words) { return words == 24 ? 3 : 7; }

static void put_bits(uint8_t *bits, size_t &pos, unsigned v, int n) {
    for (int b = n - 1; b >= 0; b--, pos++)
        if ((v >> b) & 1) bits[pos / 8] |= 0x80 >> (pos % 8);
}

bool seed_final_word(const uint16_t *idx, int n, const char *flips, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if ((n != 12 && n != 24) || strlen(flips) != (size_t)seed_final_word_flips(n) || !all_in(flips, strlen(flips), '0', '1'))
        return false;
    size_t pos = 0;
    for (int i = 0; i < n - 1; i++) put_bits(out->bytes, pos, idx[i] & 0x7FF, 11);
    for (const char *f = flips; *f; f++) put_bits(out->bytes, pos, unsigned(*f - '0'), 1);
    out->len = pos / 8;                                  // 128 or 256 bits: the whole entropy
    return true;
}

// The checksum is the first len/4 bits of SHA256(entropy).
static uint8_t checksum_bits(const uint8_t *ent, size_t len) {
    uint8_t h[32];
    wally_sha256(ent, len, h, 32);
    uint8_t cs = uint8_t(h[0] >> (8 - len / 4));
    secure_zero(h, sizeof(h));
    return cs;
}

static uint16_t get_bits(const uint8_t *bits, size_t pos, int n) {
    uint16_t v = 0;
    for (int b = 0; b < n; b++, pos++) v = uint16_t(v << 1 | ((bits[pos / 8] >> (7 - pos % 8)) & 1));
    return v;
}

bool seed_from_words(const uint16_t *idx, int n, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if (n != 12 && n != 24) return false;
    uint8_t bits[33] = {0};                            // 24 * 11 = 264 bits
    size_t pos = 0;
    for (int i = 0; i < n; i++) put_bits(bits, pos, idx[i] & 0x7FF, 11);
    size_t len = n == 24 ? 32 : 16;
    bool ok = get_bits(bits, len * 8, int(len / 4)) == checksum_bits(bits, len);
    if (ok) {
        memcpy(out->bytes, bits, len);
        out->len = len;
    }
    secure_zero(bits, sizeof(bits));
    return ok;
}

int seed_valid_last_words(const uint16_t *idx, int n, uint16_t *out) {
    if (n != 12 && n != 24) return 0;
    const int free_bits = seed_final_word_flips(n), cs_bits = 11 - free_bits;
    int count = 0;
    for (unsigned v = 0; v < (1u << free_bits); v++) {
        SeedEntropy e = {};
        size_t pos = 0;
        for (int i = 0; i < n - 1; i++) put_bits(e.bytes, pos, idx[i] & 0x7FF, 11);
        put_bits(e.bytes, pos, v, free_bits);
        out[count++] = uint16_t(v << cs_bits | checksum_bits(e.bytes, pos / 8));
        seedqr_wipe(&e);
    }
    return count;
}

int seed_word_indexes(const SeedEntropy &e, uint16_t out[24]) {
    uint8_t bits[33] = {0};
    memcpy(bits, e.bytes, e.len);
    bits[e.len] = uint8_t(checksum_bits(e.bytes, e.len) << (8 - e.len / 4));
    int n = int(e.len * 3 / 4);
    for (int w = 0; w < n; w++) out[w] = get_bits(bits, size_t(w) * 11, 11);
    secure_zero(bits, sizeof(bits));
    return n;
}
