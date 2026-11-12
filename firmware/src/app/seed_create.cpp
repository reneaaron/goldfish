#include "seed_create.h"
#include <stdio.h>
#include <string.h>
#include <Hash.h>      // sha256 from uBitcoin

extern "C" const char *const *mnemonic_wordlist(void);   // uBitcoin's trezor-crypto bip39.c

static void secure_zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

int seed_dice_rolls(int words) { return words == 24 ? 99 : 50; }
int seed_coin_flips(int words) { return words == 24 ? 256 : 128; }

static bool all_in(const char *s, size_t n, char lo, char hi) {
    for (size_t i = 0; i < n; i++)
        if (s[i] < lo || s[i] > hi) return false;
    return true;
}

static void hash_to_entropy(const char *s, size_t n, int words, SeedEntropy *out) {
    uint8_t h[32];
    sha256((const uint8_t *)s, n, h);
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

const char *seed_word(int index) { return index >= 0 && index < 2048 ? mnemonic_wordlist()[index] : ""; }

int seed_words_with_prefix(const char *prefix, int *first) {
    size_t n = strlen(prefix);
    int count = 0;
    *first = -1;
    for (int i = 0; i < 2048; i++) {
        if (strncmp(mnemonic_wordlist()[i], prefix, n) != 0) continue;
        if (*first < 0) *first = i;
        count++;
    }
    return count;
}

// The word indexes are exactly a Standard SeedQR payload, so its decoder
// does the checksum.
bool seed_from_words(const uint16_t *idx, int n, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if (n != 12 && n != 24) return false;
    char digits[97];
    for (int i = 0; i < n; i++) snprintf(digits + i * 4, 5, "%04u", unsigned(idx[i] & 0x7FF));
    bool ok = seedqr_decode((const uint8_t *)digits, n * 4, out) == SEEDQR_OK;
    secure_zero(digits, sizeof(digits));
    return ok;
}

bool seed_last_word_ok(const uint16_t *idx, int n, uint16_t last) {
    uint16_t all[24];
    if (n != 12 && n != 24) return false;
    memcpy(all, idx, (n - 1) * sizeof(uint16_t));
    all[n - 1] = last;
    SeedEntropy e;
    bool ok = seed_from_words(all, n, &e);
    seedqr_wipe(&e);
    secure_zero(all, sizeof(all));
    return ok;
}
