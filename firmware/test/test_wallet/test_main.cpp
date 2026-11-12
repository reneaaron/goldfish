// Wallet derivation against the BIP84 test vector ("abandon ... about").
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include "../../src/app/wallet.h"

static SeedEntropy zero_entropy_12() {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = 16;                     // all-zero entropy = "abandon x11 about"
    return e;
}

void test_fingerprint() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    char fp[9];
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("73c5da0a", fp);
    TEST_ASSERT_EQUAL(12, wallet_word_count());
}

void test_zpub() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    char out[200];
    TEST_ASSERT_GREATER_THAN(0, wallet_export_xpub(out, sizeof(out)));
    // BIP84 account 0 zpub
    TEST_ASSERT_EQUAL_STRING(
        "[73c5da0a/84h/0h/0h]zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs",
        out);
}

void test_wipe() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    wallet_wipe();
    TEST_ASSERT_FALSE(wallet_has_seed());
    char out[200];
    TEST_ASSERT_EQUAL(0, wallet_export_xpub(out, sizeof(out)));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fingerprint);
    RUN_TEST(test_zpub);
    RUN_TEST(test_wipe);
    return UNITY_END();
}
