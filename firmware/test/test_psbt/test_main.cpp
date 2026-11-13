// PSBT review/signing against SeedSigner's single-frame testnet vector.
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <Bitcoin.h>
#include <Conversion.h>
#include "../../src/app/psbt_review.h"
#include "../../src/app/wallet.h"
#include "../fixtures/seedsigner_vectors.h"

static uint8_t raw[4096];
static size_t raw_len;

static void load_seed(const char *mnemonic) {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = mnemonicToEntropy(std::string(mnemonic), e.bytes, sizeof(e.bytes));
    TEST_ASSERT_TRUE(e.len == 16 || e.len == 32);
    TEST_ASSERT_TRUE(wallet_load(e));
}

void setUp() {
    raw_len = fromBase64(SINGLESIG_FRAME, strlen(SINGLESIG_FRAME), raw, sizeof(raw));
    TEST_ASSERT_GREATER_THAN(0, raw_len);
}

void test_review_matches_seedsigner() {
    load_seed(SINGLESIG_MNEMONIC);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.testnet);
    TEST_ASSERT_EQUAL_UINT64(25000, r.input_total);
    TEST_ASSERT_EQUAL_UINT64(24857, r.spend);     // SeedSigner: spend_amount
    TEST_ASSERT_EQUAL_UINT64(0, r.change);
    TEST_ASSERT_EQUAL_UINT64(143, r.fee);
    TEST_ASSERT_EQUAL(2, r.n_outputs);
}

void test_sign_adds_signature() {
    load_seed(SINGLESIG_MNEMONIC);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(1, psbt_sign());
    static uint8_t out[8192];
    size_t n = psbt_serialize(out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(raw_len, n);         // signature added
    psbt_clear();
}

void test_wrong_seed_rejected() {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = 16;                                   // "abandon ... about"
    TEST_ASSERT_TRUE(wallet_load(e));
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_NOT_OUR_SEED, psbt_load(raw, raw_len, &r));
}

void test_garbage_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    uint8_t junk[64];
    memset(junk, 0x42, sizeof(junk));
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(junk, sizeof(junk), &r));
}

// Output 1 of SINGLESIG_FRAME (₿9,857) pays our own m/84h/1h/0h/1/0, but
// the frame has no output metadata, so it's reviewed as a spend. This appends
// a BIP32 derivation to that output's map (the last byte of the PSBT is its
// terminator) with our fingerprint, that path and the real pubkey at it.
// With `foreign_script` the output is first repointed at someone else's key.
static void claim_output_as_change(bool foreign_script) {
    HDPrivateKey root(std::string(SINGLESIG_MNEMONIC), std::string(""));
    uint32_t path[] = {0x80000000 + 84, 0x80000000 + 1, 0x80000000, 1, 0};
    PublicKey pub = root.derive(path, 5).publicKey();

    if (foreign_script) {
        Script ours(pub, P2WPKH);
        uint8_t *spk = nullptr;                   // in the unsigned tx
        for (size_t i = 0; i + ours.scriptLen <= raw_len && !spk; i++)
            if (memcmp(raw + i, ours.scriptArray, ours.scriptLen) == 0) spk = raw + i;
        TEST_ASSERT_NOT_NULL(spk);
        memset(spk + 2, 0x11, 20);                // P2WPKH of a key we don't have
    }

    uint8_t *p = raw + raw_len - 1;               // output 1's map terminator
    TEST_ASSERT_EQUAL(0x00, *p);
    *p++ = 34;                                    // key: type + compressed pubkey
    *p++ = 0x02;                                  // PSBT_OUT_BIP32_DERIVATION
    TEST_ASSERT_EQUAL(33, pub.sec(p, 33));
    p += 33;
    *p++ = 4 + 5 * 4;                             // value: fingerprint + path
    root.fingerprint(p);
    p += 4;
    for (int i = 0; i < 5; i++, p += 4) intToLittleEndian(path[i], p, 4);
    *p++ = 0x00;
    raw_len = p - raw;
}

// Our real pubkey and path attached to an output that pays someone else.
void test_foreign_script_with_our_derivation_is_suspicious() {
    load_seed(SINGLESIG_MNEMONIC);
    claim_output_as_change(true);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_SUSPICIOUS, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.outputs[1].suspicious);
    TEST_ASSERT_FALSE(r.outputs[1].is_change);
    TEST_ASSERT_FALSE(r.outputs[1].is_self);
    TEST_ASSERT_EQUAL_UINT64(0, r.change);
}

void test_real_change_is_change() {
    load_seed(SINGLESIG_MNEMONIC);
    claim_output_as_change(false);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.outputs[1].is_change);
    TEST_ASSERT_FALSE(r.outputs[1].suspicious);
    TEST_ASSERT_EQUAL_STRING("m/84h/1h/0h/1/0", r.outputs[1].path);
    TEST_ASSERT_EQUAL_UINT64(9857, r.change);
    TEST_ASSERT_EQUAL_UINT64(15000, r.spend);
    TEST_ASSERT_EQUAL_UINT64(143, r.fee);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_review_matches_seedsigner);
    RUN_TEST(test_sign_adds_signature);
    RUN_TEST(test_wrong_seed_rejected);
    RUN_TEST(test_garbage_rejected);
    RUN_TEST(test_foreign_script_with_our_derivation_is_suspicious);
    RUN_TEST(test_real_change_is_change);
    return UNITY_END();
}
