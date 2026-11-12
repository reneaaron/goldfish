// New seeds from dice, coins and words. Dice vectors are SeedSigner's
// tests/test_mnemonic_generation.py; coin vectors were computed with Python's
// hashlib and the BIP39 wordlist, independent of this code. No RNG anywhere.
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <Bitcoin.h>
#include "../../src/app/seed_create.h"

static void assert_mnemonic(const char *expected, const SeedEntropy &e) {
    TEST_ASSERT_EQUAL_STRING(expected, mnemonicFromEntropy(e.bytes, e.len));
}

void test_dice_12() {
    SeedEntropy e;
    const char *r = "12345612345612345612345612345612345612345612345612";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 12, &e));
    assert_mnemonic("unveil nice picture region tragic fault cream strike tourist control recipe tourist", e);
    r = "66666666666666666666666666666666666666666666666666";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 12, &e));
    assert_mnemonic("senior morning song proud recycle toy search apple trigger lend vibrant arrest", e);
}

void test_dice_24() {
    SeedEntropy e;
    const char *r = "522222222222222222222222222222222222222222222555555555555555555555555555555555555555555555555555555";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 24, &e));
    assert_mnemonic("resource timber firm banner horror pupil frozen main pear direct pioneer broken grid core insane "
                    "begin sister pony end debate task silk empty curious", e);
}

void test_dice_rejects() {
    SeedEntropy e;
    const char *r = "1234561234561234561234561234561234561234561234561";   // 49
    TEST_ASSERT_FALSE(seed_from_dice(r, strlen(r), 12, &e));
    r = "12345612345612345612345612345612345612345612345610";             // a 0
    TEST_ASSERT_FALSE(seed_from_dice(r, strlen(r), 12, &e));
    r = "12345612345612345612345612345612345612345612345612";
    TEST_ASSERT_FALSE(seed_from_dice(r, strlen(r), 24, &e));             // 50 rolls aren't enough for 24
}

void test_coins() {
    char f[257];
    SeedEntropy e;
    for (int i = 0; i < 128; i++) f[i] = (i % 2) ? '0' : '1';
    TEST_ASSERT_TRUE(seed_from_coins(f, 128, 12, &e));
    assert_mnemonic("blanket comfort model weekend box scatter busy board ghost oval purchase vapor", e);
    for (int i = 0; i < 256; i++) f[i] = "1101"[i % 4];
    TEST_ASSERT_TRUE(seed_from_coins(f, 256, 24, &e));
    assert_mnemonic("multiply poverty relax ozone gorilla cheap coffee steak inch picture upgrade doll tank wrong grace "
                    "letter parrot drive mass attack journey journey pull eager", e);
    f[0] = '2';
    TEST_ASSERT_FALSE(seed_from_coins(f, 256, 24, &e));
}

static int parse(const char *mn, uint16_t *idx) {
    int n = 0;
    char w[16];
    for (const char *p = mn; *p;) {
        size_t l = strcspn(p, " ");
        memcpy(w, p, l); w[l] = 0;
        int first;
        TEST_ASSERT_EQUAL(1, seed_words_with_prefix(w, &first) >= 1);
        TEST_ASSERT_EQUAL_STRING(w, seed_word(first));
        idx[n++] = uint16_t(first);
        p += l; while (*p == ' ') p++;
    }
    return n;
}

void test_words() {
    const char *mn = "crew marble private differ race truly blush basket crater affair prepare unique";
    uint16_t idx[24];
    int n = parse(mn, idx);
    TEST_ASSERT_EQUAL(12, n);
    SeedEntropy e;
    TEST_ASSERT_TRUE(seed_from_words(idx, n, &e));
    const uint8_t want[16] = {0x33, 0x50, 0xf6, 0xac, 0x9e, 0xeb, 0x07, 0xd2, 0xc6, 0x20, 0x99, 0x32, 0x80, 0x8a, 0xa7, 0xf6};
    TEST_ASSERT_EQUAL(16, e.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, e.bytes, 16);
    TEST_ASSERT_TRUE(seed_last_word_ok(idx, 12, idx[11]));
    TEST_ASSERT_FALSE(seed_last_word_ok(idx, 12, uint16_t(idx[11] ^ 1)));   // flips a checksum bit
    idx[11] ^= 1;
    TEST_ASSERT_FALSE(seed_from_words(idx, n, &e));
}

void test_last_word_counts() {
    // 11 words leave 7 free bits: 128 valid last words. 23 words leave 3: 8.
    uint16_t idx[24] = {0};
    int ok12 = 0, ok24 = 0;
    for (int w = 0; w < 2048; w++) {
        ok12 += seed_last_word_ok(idx, 12, uint16_t(w));
        ok24 += seed_last_word_ok(idx, 24, uint16_t(w));
    }
    TEST_ASSERT_EQUAL(128, ok12);
    TEST_ASSERT_EQUAL(8, ok24);
}

void test_prefix() {
    int first;
    TEST_ASSERT_EQUAL(2048, seed_words_with_prefix("", &first));
    TEST_ASSERT_EQUAL(0, first);
    TEST_ASSERT_EQUAL(1, seed_words_with_prefix("zoo", &first));
    TEST_ASSERT_EQUAL_STRING("zoo", seed_word(first));
    TEST_ASSERT_EQUAL(0, seed_words_with_prefix("qq", &first));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_dice_12);
    RUN_TEST(test_dice_24);
    RUN_TEST(test_dice_rejects);
    RUN_TEST(test_coins);
    RUN_TEST(test_words);
    RUN_TEST(test_last_word_counts);
    RUN_TEST(test_prefix);
    return UNITY_END();
}
