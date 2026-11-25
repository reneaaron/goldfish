// The advisory "these rolls look made up" check. The fair sequences were
// rolled once with Python's secrets module and pasted here; there is no RNG
// in this test. False alarm rates on fair input were measured once outside
// the repo (200,000 simulated sequences per length): see firmware/README.md.
// Run: pio test -e test -f test_entropy_check
#include <unity.h>
#include <string.h>
#include "../../src/app/entropy_check.h"

static int dice(const char *s) { return entropy_check(s, (int)strlen(s), true); }
static int coins(const char *s) { return entropy_check(s, (int)strlen(s), false); }

void test_fair_dice_pass() {
    const char *fair[] = {
        "22242454136553153234335553456544655622342664655563",
        "12621315425246345631225546133463261232564411131154",
        "23113113416623141633155121314626266344441662252342",
        "22123521316533133132126421523366112444113644642552",
        "63124551326645213355162443316625514432216653411526",   // e2e's rolls
        "661362561321234261616132452361513634154111355625234345546333142641313453332216131216122354215153161",
        "452653522422222364335143655126212223443343435622551623613263431354235141146535653511665123141314665",
    };
    for (const char *s : fair) TEST_ASSERT_EQUAL_MESSAGE(0, dice(s), s);
}

void test_fair_coins_pass() {
    const char *fair[] = {
        "00011011000011011101000010100010110100000000010101010010011101001100011110000001001110000001100110000010010111000110011010110110",
        "01100001011010011101010101011011000001110001000101001110101000001010011110011001001101110010110101111001001001100111110101100001",
        "1011101010101010011011011011000100001011110001010110000000110001101010011111001000101111110011010000011011001011001110001001010111010010000000100010101110011110011000010011011111010010101110001100111000001110000000001000000010100011100011001101110101110110",
    };
    for (const char *s : fair) TEST_ASSERT_EQUAL_MESSAGE(0, coins(s), s);
}

void test_counting_flagged() {
    int f = dice("12345612345612345612345612345612345612345612345612");
    TEST_ASSERT_TRUE(f & ENT_STEPS);
    TEST_ASSERT_TRUE(f & ENT_REPEATS);
    TEST_ASSERT_TRUE(f & ENT_PAIRS);      // never the same face twice
    TEST_ASSERT_TRUE(f & ENT_TOO_EVEN);
    TEST_ASSERT_TRUE(dice("65432165432165432165432165432165432165432165432165") & ENT_STEPS);
}

void test_no_repeats_flagged() {
    // Random otherwise, but never the same face twice in a row.
    TEST_ASSERT_TRUE(dice("23652532143514652132616535416361251426125345134543") & ENT_PAIRS);
}

void test_too_even_flagged() {
    // Faces 9, 9, 8, 8, 8, 8 times, in a shuffled order.
    TEST_ASSERT_TRUE(dice("14245245426141632633641545154352563663221261112335") & ENT_TOO_EVEN);
}

void test_lazy_dice_flagged() {
    TEST_ASSERT_TRUE(dice("66666666666666666666666666666666666666666666666666") & ENT_STREAK);
    TEST_ASSERT_TRUE(dice("11111111111111111111111116666666666666666666666666") & ENT_UNEVEN);
}

void test_coin_patterns_flagged() {
    char s[129];
    for (int i = 0; i < 128; i++) s[i] = "10"[i % 2];
    s[128] = 0;
    TEST_ASSERT_TRUE(coins(s) & ENT_PAIRS);   // switches every time
    for (int i = 0; i < 128; i++) s[i] = "1101"[i % 4];
    TEST_ASSERT_TRUE(coins(s) & ENT_UNEVEN);
    TEST_ASSERT_TRUE(coins(s) & ENT_REPEATS);
    for (int i = 0; i < 128; i++) s[i] = i < 64 ? '1' : '0';
    TEST_ASSERT_TRUE(coins(s) & ENT_STREAK);
}

void test_not_ours_to_judge() {
    TEST_ASSERT_EQUAL(0, dice("1"));
    TEST_ASSERT_EQUAL(0, entropy_check("1234x", 5, true));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fair_dice_pass);
    RUN_TEST(test_fair_coins_pass);
    RUN_TEST(test_counting_flagged);
    RUN_TEST(test_no_repeats_flagged);
    RUN_TEST(test_too_even_flagged);
    RUN_TEST(test_lazy_dice_flagged);
    RUN_TEST(test_coin_patterns_flagged);
    RUN_TEST(test_not_ours_to_judge);
    return UNITY_END();
}
