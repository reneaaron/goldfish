// BIP-85 BIP39 child seeds against the spec's test vectors.
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <Bitcoin.h>
#include "../../src/app/bip85.h"
#include "../../src/app/seed_create.h"
#include "../../src/app/wallet.h"

// BIP-85 test master key
static const char *XPRV =
    "xprv9s21ZrQH143K2LBWUUQRFXhucrQqBpKdRRxNVq2zBqsx8HVqFk2uYo8kmbaLLHRdqtQpUm98uKfu3vca1LqdGhUtyoFnCNkfmXRyPXLjbKb";

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static void words_of(const SeedEntropy &e, char *out) {
    uint16_t idx[24];
    int n = seed_word_indexes(e, idx);
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        if (i) strcat(out, " ");
        strcat(out, seed_word(idx[i]));
    }
}

void test_12_words() {
    HDPrivateKey root(XPRV);
    SeedEntropy e;
    TEST_ASSERT_TRUE(bip85_bip39_entropy(root, 12, 0, &e));
    char h[65], w[300];
    hex(e.bytes, e.len, h);
    TEST_ASSERT_EQUAL_STRING("6250b68daf746d12a24d58b4787a714b", h);
    words_of(e, w);
    TEST_ASSERT_EQUAL_STRING("girl mad pet galaxy egg matter matrix prison refuse sense ordinary nose", w);
}

void test_24_words() {
    HDPrivateKey root(XPRV);
    SeedEntropy e;
    TEST_ASSERT_TRUE(bip85_bip39_entropy(root, 24, 0, &e));
    char h[65], w[300];
    hex(e.bytes, e.len, h);
    TEST_ASSERT_EQUAL_STRING("ae131e2312cdc61331542efe0d1077bac5ea803adf24b313a4f0e48e9c51f37f", h);
    words_of(e, w);
    TEST_ASSERT_EQUAL_STRING(
        "puppy ocean match cereal symbol another shed magic wrap hammer bulb intact gadget divorce twin tonight "
        "reason outdoor destroy simple truth cigar social volcano",
        w);
}

void test_index_changes_child() {
    HDPrivateKey root(XPRV);
    SeedEntropy a, b;
    TEST_ASSERT_TRUE(bip85_bip39_entropy(root, 12, 0, &a));
    TEST_ASSERT_TRUE(bip85_bip39_entropy(root, 12, 1, &b));
    TEST_ASSERT_FALSE(memcmp(a.bytes, b.bytes, 16) == 0);
}

void test_rejects_bad_input() {
    HDPrivateKey root(XPRV);
    SeedEntropy e;
    TEST_ASSERT_FALSE(bip85_bip39_entropy(root, 18, 0, &e));
    TEST_ASSERT_FALSE(bip85_bip39_entropy(root, 12, 0x80000000u, &e));
    TEST_ASSERT_TRUE(bip85_bip39_entropy(root, 12, BIP85_MAX_INDEX, &e));
}

// The 12-word child loads like any seed; its fingerprint matches.
void test_child_fingerprint() {
    HDPrivateKey root(XPRV);
    SeedEntropy e;
    TEST_ASSERT_TRUE(bip85_bip39_entropy(root, 12, 0, &e));
    char fp[9], loaded[9];
    TEST_ASSERT_TRUE(seed_fingerprint_hex(e, fp));
    TEST_ASSERT_TRUE(wallet_load(e));
    wallet_fingerprint_hex(loaded);
    wallet_wipe();
    TEST_ASSERT_EQUAL_STRING(loaded, fp);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_12_words);
    RUN_TEST(test_24_words);
    RUN_TEST(test_index_changes_child);
    RUN_TEST(test_rejects_bad_input);
    RUN_TEST(test_child_fingerprint);
    return UNITY_END();
}
