// New seeds from dice, coins and words. Dice vectors are SeedSigner's
// tests/test_mnemonic_generation.py; coin and drawn-last-word vectors were
// computed with Python's hashlib and embit following SeedSigner's
// helpers/mnemonic_generation.py, independent of this code. No RNG anywhere.
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <initializer_list>
#include <wally_core.h>
#include <wally_bip39.h>
#include "../../src/app/seed_create.h"

static void assert_mnemonic(const char *expected, const SeedEntropy &e) {
    char *mn = nullptr;
    TEST_ASSERT_EQUAL(WALLY_OK, bip39_mnemonic_from_bytes(nullptr, e.bytes, e.len, &mn));
    TEST_ASSERT_EQUAL_STRING(expected, mn);
    wally_free_string(mn);
}

void test_dice_12() {
    SeedEntropy e;
    const char *r = "12345612345612345612345612345612345612345612345612";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 12, &e));
    assert_mnemonic("unveil nice picture region tragic fault cream strike tourist control recipe tourist", e);
    r = "66666666666666666666666666666666666666666666666666";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 12, &e));
    assert_mnemonic("senior morning song proud recycle toy search apple trigger lend vibrant arrest", e);
    r = "11111111111111111111111111111111111111111111111111";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 12, &e));
    assert_mnemonic("diet glad hat rural panther lawsuit act drop gallery urge where fit", e);
}

void test_dice_24() {
    SeedEntropy e;
    const char *r = "522222222222222222222222222222222222222222222555555555555555555555555555555555555555555555555555555";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 24, &e));
    assert_mnemonic("resource timber firm banner horror pupil frozen main pear direct pioneer broken grid core insane "
                    "begin sister pony end debate task silk empty curious", e);
    r = "222222222222222222222222222222222222222222222555555555555555555555555555555555555555555555555555555";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 24, &e));
    assert_mnemonic("garden uphold level clog sword globe armor issue two cute scorpion improve verb artwork blind "
                    "tail raw butter combine move produce foil feature wave", e);
    r = "222222222222222222222222222222222222222222222555555555555555555555555555555555555555555555555555556";
    TEST_ASSERT_TRUE(seed_from_dice(r, strlen(r), 24, &e));
    assert_mnemonic("lizard broken love tired depend eyebrow excess lonely advance father various cram ignore panic "
                    "feed plunge miss regret boring unique galaxy fan detail fly", e);
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
    memset(f, '0', 256);                              // all tails
    TEST_ASSERT_TRUE(seed_from_coins(f, 128, 12, &e));
    assert_mnemonic("earth naive tongue material rebel cotton credit quarter market peanut memory other", e);
    TEST_ASSERT_TRUE(seed_from_coins(f, 256, 24, &e));
    assert_mnemonic("gun library main saddle doctor meat pizza bone brave output matter chef merry flag abuse puppy "
                    "first rotate era tent news arrest pepper finger", e);
    memset(f, '1', 128);                              // all heads
    TEST_ASSERT_TRUE(seed_from_coins(f, 128, 12, &e));
    assert_mnemonic("exit pulp believe feature horror vehicle home more patrol hair drink resist", e);
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
    uint16_t last[128];
    TEST_ASSERT_EQUAL(128, seed_valid_last_words(idx, 12, last));
    bool found = false;
    for (int i = 0; i < 128; i++) found |= last[i] == idx[11];
    TEST_ASSERT_TRUE(found);                          // "unique" is one of them
    idx[11] ^= 1;                                     // flips a checksum bit
    TEST_ASSERT_FALSE(seed_from_words(idx, n, &e));
}

void test_last_word_counts() {
    // 11 words leave 7 free bits: 128 valid last words. 23 words leave 3: 8.
    // Cross-check against brute force: exactly these words pass the checksum.
    uint16_t idx[24] = {0}, last[128];
    for (int n : {12, 24}) {
        int count = seed_valid_last_words(idx, n, last), brute = 0;
        TEST_ASSERT_EQUAL(n == 12 ? 128 : 8, count);
        for (int w = 0; w < 2048; w++) {
            idx[n - 1] = uint16_t(w);
            SeedEntropy e;
            bool ok = seed_from_words(idx, n, &e);
            bool listed = false;
            for (int i = 0; i < count; i++) listed |= last[i] == w;
            TEST_ASSERT_EQUAL(ok, listed);
            brute += ok;
        }
        idx[n - 1] = 0;
        TEST_ASSERT_EQUAL(count, brute);
    }
}

void test_prefix() {
    int first;
    TEST_ASSERT_EQUAL(2048, seed_words_with_prefix("", &first));
    TEST_ASSERT_EQUAL(0, first);
    TEST_ASSERT_EQUAL(1, seed_words_with_prefix("zoo", &first));
    TEST_ASSERT_EQUAL_STRING("zoo", seed_word(first));
    TEST_ASSERT_EQUAL(0, seed_words_with_prefix("qq", &first));
}

void test_final_word() {
    uint16_t idx[24];
    int n = parse("crawl focus rescue cable view pledge rather dinner cousin unfair day", idx);
    TEST_ASSERT_EQUAL(11, n);
    SeedEntropy e;
    TEST_ASSERT_TRUE(seed_final_word(idx, 12, "1010101", &e));
    assert_mnemonic("crawl focus rescue cable view pledge rather dinner cousin unfair day prize", e);
    TEST_ASSERT_TRUE(seed_final_word(idx, 12, "0000000", &e));
    assert_mnemonic("crawl focus rescue cable view pledge rather dinner cousin unfair day account", e);
    TEST_ASSERT_TRUE(seed_final_word(idx, 12, "1111111", &e));
    assert_mnemonic("crawl focus rescue cable view pledge rather dinner cousin unfair day zoo", e);
    TEST_ASSERT_FALSE(seed_final_word(idx, 12, "101010", &e));      // 6 flips: one short
    n = parse("bubble father debate ankle injury fence mesh evolve section wet coyote violin pyramid flower rent "
              "arrow round clutch myth safe base skin mobile", idx);
    TEST_ASSERT_EQUAL(23, n);
    TEST_ASSERT_TRUE(seed_final_word(idx, 24, "101", &e));
    TEST_ASSERT_EQUAL(32, e.len);
    uint16_t all[24];
    TEST_ASSERT_EQUAL(24, seed_word_indexes(e, all));
    TEST_ASSERT_EQUAL_STRING("sausage", seed_word(all[23]));
    TEST_ASSERT_TRUE(seed_final_word(idx, 24, "000", &e));
    seed_word_indexes(e, all);
    TEST_ASSERT_EQUAL_STRING("aware", seed_word(all[23]));
    TEST_ASSERT_TRUE(seed_final_word(idx, 24, "111", &e));
    seed_word_indexes(e, all);
    TEST_ASSERT_EQUAL_STRING("wild", seed_word(all[23]));
}

void test_word_indexes() {
    const uint8_t ent[16] = {0x33, 0x50, 0xf6, 0xac, 0x9e, 0xeb, 0x07, 0xd2, 0xc6, 0x20, 0x99, 0x32, 0x80, 0x8a, 0xa7, 0xf6};
    SeedEntropy e = {};
    memcpy(e.bytes, ent, 16);
    e.len = 16;
    uint16_t idx[24], want[24];
    TEST_ASSERT_EQUAL(12, seed_word_indexes(e, idx));
    parse("crew marble private differ race truly blush basket crater affair prepare unique", want);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, idx, 12);
}

// Our wordlist table is the one libwally uses for mnemonics.
void test_wordlist_matches_libwally() {
    for (int i = 0; i < 2048; i++) {
        char *w = nullptr;
        TEST_ASSERT_EQUAL(WALLY_OK, bip39_get_word(nullptr, size_t(i), &w));
        TEST_ASSERT_EQUAL_STRING(w, seed_word(i));
        wally_free_string(w);
    }
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
    RUN_TEST(test_final_word);
    RUN_TEST(test_word_indexes);
    RUN_TEST(test_wordlist_matches_libwally);
    return UNITY_END();
}
