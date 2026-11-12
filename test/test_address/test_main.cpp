// Verify address against the BIP84 test vectors ("abandon ... about").
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include "../../src/app/address_verify.h"
#include "../../src/app/wallet.h"

static void load_abandon() {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = 16;
    TEST_ASSERT_TRUE(wallet_load(e));
}

static VerifyStatus run(const char *text, AddressCheck *c) {
    TEST_ASSERT_EQUAL(ADDR_OK, address_parse((const uint8_t *)text, strlen(text), c));
    while (!address_verify_step(c, 50)) {}
    return c->status;
}

void setUp() { load_abandon(); }

void test_bip84_first_receive() {
    char a[96];
    TEST_ASSERT_GREATER_THAN(0, address_at(false, 0, 0, a, sizeof(a)));
    TEST_ASSERT_EQUAL_STRING("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu", a);
}

void test_bip84_first_change() {
    char a[96];
    TEST_ASSERT_GREATER_THAN(0, address_at(false, 1, 0, a, sizeof(a)));
    TEST_ASSERT_EQUAL_STRING("bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el", a);
}

void test_verify_receive_from_uri() {
    AddressCheck c;
    TEST_ASSERT_EQUAL(VERIFY_FOUND, run("BITCOIN:BC1QCR8TE4KR609GCAWUTMRZA0J4XV80JY8Z306FYU?amount=0.001", &c));
    TEST_ASSERT_EQUAL(0, c.chain);
    TEST_ASSERT_EQUAL(0, c.index);
}

void test_verify_deeper_receive() {
    char a[96];
    address_at(false, 0, 137, a, sizeof(a));
    AddressCheck c;
    TEST_ASSERT_EQUAL(VERIFY_FOUND, run(a, &c));
    TEST_ASSERT_EQUAL(0, c.chain);
    TEST_ASSERT_EQUAL(137, c.index);
}

void test_verify_change() {
    AddressCheck c;
    TEST_ASSERT_EQUAL(VERIFY_FOUND, run("bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el", &c));
    TEST_ASSERT_EQUAL(1, c.chain);
}

void test_verify_testnet() {
    char a[96];
    address_at(true, 0, 3, a, sizeof(a));
    TEST_ASSERT_EQUAL(0, strncmp(a, "tb1q", 4));
    AddressCheck c;
    TEST_ASSERT_EQUAL(VERIFY_FOUND, run(a, &c));
    TEST_ASSERT_TRUE(c.testnet);
    TEST_ASSERT_EQUAL(3, c.index);
}

void test_foreign_address_not_found() {
    AddressCheck c;
    // BIP84 vector for a different seed's style; not derived from "abandon"
    TEST_ASSERT_EQUAL(VERIFY_NOT_FOUND, run("bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq", &c));
    TEST_ASSERT_EQUAL(c.total, c.checked);
}

void test_other_seed_not_reused() {
    char a[96];
    address_at(false, 0, 0, a, sizeof(a));          // abandon seed, caches its keys
    SeedEntropy e;
    memset(&e, 0x11, sizeof(e));
    e.len = 16;
    TEST_ASSERT_TRUE(wallet_load(e));               // a different seed
    AddressCheck c;
    TEST_ASSERT_EQUAL(VERIFY_NOT_FOUND, run(a, &c));
}

void test_parse_rejects() {
    AddressCheck c;
    const char *taproot = "bc1p5d7rjq7g6rdk2yhzks9smlaqtedr4dekq08ge8ztwac72sfr9rusxg3297";
    TEST_ASSERT_EQUAL(ADDR_UNSUPPORTED, address_parse((const uint8_t *)taproot, strlen(taproot), &c));
    const char *legacy = "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2";
    TEST_ASSERT_EQUAL(ADDR_UNSUPPORTED, address_parse((const uint8_t *)legacy, strlen(legacy), &c));
    const char *junk = "hello world";
    TEST_ASSERT_EQUAL(ADDR_NOT_ADDRESS, address_parse((const uint8_t *)junk, strlen(junk), &c));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_bip84_first_receive);
    RUN_TEST(test_bip84_first_change);
    RUN_TEST(test_verify_receive_from_uri);
    RUN_TEST(test_verify_deeper_receive);
    RUN_TEST(test_verify_change);
    RUN_TEST(test_verify_testnet);
    RUN_TEST(test_foreign_address_not_found);
    RUN_TEST(test_other_seed_not_reused);
    RUN_TEST(test_parse_rejects);
    return UNITY_END();
}
