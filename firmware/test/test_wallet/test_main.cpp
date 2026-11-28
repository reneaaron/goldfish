// Wallet derivation against the BIP84 test vector ("abandon ... about").
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip32.h>
#include "../../src/app/wallet.h"
#include "../fixtures/bip39_vectors.h"

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

// The key for a multisig wallet: BIP-48 account 0, script type 2 (P2WSH),
// with SLIP-132's Zpub / Vpub. Expected values worked out by embit.
void test_multisig_keys() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    TEST_ASSERT_FALSE(wallet_export_multisig());   // single-sig unless asked
    wallet_set_export_multisig(true);
    char out[200];
    TEST_ASSERT_GREATER_THAN(0, wallet_export_xpub(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "[73c5da0a/48h/0h/0h/2h]Zpub74Jru6aftwwHxCUCWEvP6DgrfFsdA4U6ZRtQ5i8qJpMcC39yZGv3egBhQfV3MS9pZtH5z8iV5qWkJsK6ESs6mSzt4qvGhzJxPeeVS2e1zUG",
        out);
    wallet_set_testnet(true);
    size_t n = wallet_export_xpub(out, sizeof(out));
    wallet_set_testnet(false);
    wallet_set_export_multisig(false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "[73c5da0a/48h/1h/0h/2h]Vpub5n95dMZrDHj6SeBgJ1oz4Fae2N2eJNuWK3VTKDb2dzGpMFLUHLmtyDfen7AaQxwQ5mZnMyXdVrkEaoMLVTH8FmVBRVWPGFYWhmtDUGehGmq",
        out);
    // and back to the single-sig account
    TEST_ASSERT_GREATER_THAN(0, wallet_export_xpub(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING_LEN("[73c5da0a/84h/0h/0h]zpub", out, 24);
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

// In two steps, for showing the fingerprint before it is used: trying a
// passphrase gives its fingerprint and changes nothing; only using it does.
void test_passphrase_tried_first() {
    uint8_t f[4];
    char fp[9];
    wallet_wipe();
    TEST_ASSERT_FALSE(wallet_passphrase_try("TREZOR", f));     // no seed
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    TEST_ASSERT_FALSE(wallet_passphrase_use());                // nothing tried
    TEST_ASSERT_TRUE(wallet_passphrase_try("TREZOR", f));
    const uint8_t trezor[4] = {0xb4, 0xe3, 0xf5, 0xed};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(trezor, f, 4);
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("73c5da0a", fp);                  // still the seed alone
    TEST_ASSERT_FALSE(wallet_has_passphrase());

    wallet_passphrase_drop();                                  // not wanted
    TEST_ASSERT_FALSE(wallet_passphrase_use());
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("73c5da0a", fp);

    TEST_ASSERT_TRUE(wallet_passphrase_try("TREZOR", f));
    TEST_ASSERT_TRUE(wallet_passphrase_use());
    TEST_ASSERT_TRUE(wallet_has_passphrase());
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("b4e3f5ed", fp);
    TEST_ASSERT_FALSE(wallet_passphrase_use());                // used once

    TEST_ASSERT_TRUE(wallet_passphrase_try("", f));            // and back
    const uint8_t alone[4] = {0x73, 0xc5, 0xda, 0x0a};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(alone, f, 4);
    TEST_ASSERT_TRUE(wallet_passphrase_use());
    TEST_ASSERT_FALSE(wallet_has_passphrase());

    TEST_ASSERT_TRUE(wallet_passphrase_try("TREZOR", f));      // a tried key goes with the seed
    wallet_wipe();
    TEST_ASSERT_FALSE(wallet_passphrase_use());
}

// 24 words: all-zero 32-byte entropy = "abandon x23 art". BIP-84's vectors
// are 12 words only, so the fingerprint and zpub were worked out by embit.
void test_24_words() {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = 32;
    TEST_ASSERT_TRUE(wallet_load(e));
    TEST_ASSERT_EQUAL(24, wallet_word_count());
    char fp[9];
    wallet_fingerprint_hex(fp);
    TEST_ASSERT_EQUAL_STRING("5436d724", fp);
    char out[200];
    TEST_ASSERT_GREATER_THAN(0, wallet_export_xpub(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "[5436d724/84h/0h/0h]zpub6qTBTNftBzVTjgVcSUw7vW5N1KQbV93Jnrw314RHGkCkSx4vk6nEWH1MJfReXi2WThvuDRiRpyT7cDoakEcZMQ1iZPgfJgQrcVMR4aJWh6S",
        out);
    wallet_wipe();
}

// Every 12- and 24-word BIP-39 vector: the entropy, loaded with the
// passphrase TREZOR, gives the vector's root key.
void test_bip39_vectors() {
    for (int i = 0; i < BIP39_VECTOR_COUNT; i++) {
        const Bip39Vector &v = BIP39_VECTORS[i];
        SeedEntropy e;
        memset(&e, 0, sizeof(e));
        TEST_ASSERT_EQUAL(WALLY_OK, wally_hex_to_bytes(v.entropy_hex, e.bytes, sizeof(e.bytes), &e.len));
        TEST_ASSERT_TRUE(wallet_load(e));
        TEST_ASSERT_EQUAL(e.len == 16 ? 12 : 24, wallet_word_count());
        TEST_ASSERT_TRUE(wallet_set_passphrase("TREZOR"));
        char *xprv = nullptr;
        TEST_ASSERT_EQUAL(WALLY_OK, bip32_key_to_base58(wallet_root(), BIP32_FLAG_KEY_PRIVATE, &xprv));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(v.xprv, xprv, v.entropy_hex);
        wally_free_string(xprv);
        wallet_wipe();
    }
}

// The exported key follows the passphrase: same path, the passphrase's root.
// Expected values worked out by embit.
void test_passphrase_export() {
    TEST_ASSERT_TRUE(wallet_load(zero_entropy_12()));
    TEST_ASSERT_TRUE(wallet_set_passphrase("TREZOR"));
    char out[200];
    TEST_ASSERT_GREATER_THAN(0, wallet_export_xpub(out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "[b4e3f5ed/84h/0h/0h]zpub6rXDN3yuixCtvAyzKn3uWLDr8qXKEQ2orduEL5AAysdHZmuPVUL2vbMQDEhp8L9hRsgM8J8idDNPdZjrob8R5e7x7UoYXVdfjDG96TLa7La",
        out);
    wallet_set_testnet(true);
    size_t n = wallet_export_xpub(out, sizeof(out));
    wallet_set_testnet(false);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "[b4e3f5ed/84h/1h/0h]vpub5Y1FMRorHhCpQ7SksCWpqf8HLtMibo6yE4kyGHGys7aHxvrdtTJne1Jdi4FtdLJ9B9Jm2z8cNgKa7Xtvjoa9ap6XEd7C4KLuDE69ca86npL",
        out);
    wallet_wipe();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fingerprint);
    RUN_TEST(test_zpub);
    RUN_TEST(test_vpub);
    RUN_TEST(test_multisig_keys);
    RUN_TEST(test_wipe);
    RUN_TEST(test_passphrase);
    RUN_TEST(test_passphrase_tried_first);
    RUN_TEST(test_24_words);
    RUN_TEST(test_bip39_vectors);
    RUN_TEST(test_passphrase_export);
    return UNITY_END();
}
