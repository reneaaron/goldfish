// Wallet derivation against the BIP84 test vector ("abandon ... about").
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip32.h>
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
    char fp2[9];
    TEST_ASSERT_TRUE(seed_fingerprint_hex(zero_entropy_12(), fp2));   // same, without loading
    TEST_ASSERT_EQUAL_STRING("73c5da0a", fp2);
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

void test_vpub() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    wallet_set_testnet(true);
    char out[200];
    size_t n = wallet_export_xpub(out, sizeof(out));
    wallet_set_testnet(false);
    TEST_ASSERT_GREATER_THAN(0, n);
    // m/84'/1'/0' of the same seed, the account Sparrow derives on testnet/signet
    TEST_ASSERT_EQUAL_STRING(
        "[73c5da0a/84h/1h/0h]vpub5Y6cjg78GGuNLsaPhmYsiw4gYX3HoQiRBiSwDaBXKUafCt9bNwWQiitDk5VZ5BVxYnQdwoTyXSs2JHRPAgjAvtbBrf8ZhDYe2jWAqvZVnsc",
        out);
}

void test_wipe() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    SeedEntropy e;
    TEST_ASSERT_TRUE(wallet_entropy(&e));
    TEST_ASSERT_EQUAL(16, e.len);
    wallet_wipe();
    TEST_ASSERT_FALSE(wallet_has_seed());
    TEST_ASSERT_FALSE(wallet_entropy(&e));
    TEST_ASSERT_EQUAL(0, e.len);
    char out[200];
    TEST_ASSERT_EQUAL(0, wallet_export_xpub(out, sizeof(out)));
}

// BIP39 test vector (Trezor's vectors.json): "abandon ... about" with the
// passphrase TREZOR gives this root key.
void test_passphrase() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    TEST_ASSERT_FALSE(wallet_has_passphrase());
    TEST_ASSERT_TRUE(wallet_set_passphrase("TREZOR"));
    TEST_ASSERT_TRUE(wallet_has_passphrase());
    char *xprv = nullptr;
    TEST_ASSERT_EQUAL(WALLY_OK, bip32_key_to_base58(wallet_root(), BIP32_FLAG_KEY_PRIVATE, &xprv));
    TEST_ASSERT_EQUAL_STRING(
        "xprv9s21ZrQH143K3h3fDYiay8mocZ3afhfULfb5GX8kCBdno77K4HiA15Tg23wpbeF1pLfs1c5SPmYHrEpTuuRhxMwvKDwqdKiGJS9XFKzUsAF",
        xprv);
    wally_free_string(xprv);
    char fp[9];
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("b4e3f5ed", fp);         // hash160 of that key's public key
    // The seed itself is unchanged: same backup, same word count.
    SeedEntropy e;
    TEST_ASSERT_TRUE(wallet_entropy(&e));
    TEST_ASSERT_EQUAL(16, e.len);
    TEST_ASSERT_EQUAL(12, wallet_word_count());

    TEST_ASSERT_TRUE(wallet_set_passphrase(""));      // back to the seed alone
    TEST_ASSERT_FALSE(wallet_has_passphrase());
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("73c5da0a", fp);

    TEST_ASSERT_TRUE(wallet_set_passphrase("TREZOR"));
    wallet_wipe();                                    // wiped with the seed
    TEST_ASSERT_FALSE(wallet_has_passphrase());
    TEST_ASSERT_FALSE(wallet_set_passphrase("TREZOR"));
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12())); // a fresh load has none
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("73c5da0a", fp);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fingerprint);
    RUN_TEST(test_zpub);
    RUN_TEST(test_vpub);
    RUN_TEST(test_wipe);
    RUN_TEST(test_passphrase);
    return UNITY_END();
}
