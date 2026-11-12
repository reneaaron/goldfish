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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_review_matches_seedsigner);
    RUN_TEST(test_sign_adds_signature);
    RUN_TEST(test_wrong_seed_rejected);
    RUN_TEST(test_garbage_rejected);
    return UNITY_END();
}
