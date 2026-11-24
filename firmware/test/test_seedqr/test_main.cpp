// SeedQR decoding against the vectors in SeedSigner's docs/seed_qr/README.md.
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include "../../src/app/seedqr.h"

static void from_bits(const char *bits, uint8_t *out, size_t nbytes) {
    memset(out, 0, nbytes);
    for (size_t i = 0; i < nbytes * 8; i++)
        if (bits[i] == '1') out[i / 8] |= 0x80 >> (i % 8);
}

// "attack pizza motion ... expire", 24 words
static const char *V1_STANDARD =
    "011513251154012711900771041507421289190620080870026613431420201617920614089619290300152408010643";
static const char *V1_COMPACT_BITS =
    "0000111001110100101101100100000100000111111110010100110011000000110011001111101011100110101000010011110111001011111011000011011001100010000101010100111111101100011001111110000011100000000010011001100111000000011110001001001001011001011111010001100100001010";

// 12-word example from the spec section
static const char *SPEC12_STANDARD = "192402220235174306311124037817700641198012901210";
static const char *SPEC12_BITS =
    "111100001000001101111000011101011110110011110100111011110001100100001011110101101110101001010000001111101111001010000101010010111010";

void test_standard_24() {
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_OK, seedqr_decode((const uint8_t *)V1_STANDARD, strlen(V1_STANDARD), &e));
    TEST_ASSERT_EQUAL(32, e.len);
    TEST_ASSERT_EQUAL(24, seedqr_word_count(e));
    uint8_t want[32];
    from_bits(V1_COMPACT_BITS, want, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, e.bytes, 32);
}

void test_compact_24() {
    uint8_t raw[32];
    from_bits(V1_COMPACT_BITS, raw, 32);
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_OK, seedqr_decode(raw, 32, &e));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, e.bytes, 32);
}

void test_standard_12() {
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_OK, seedqr_decode((const uint8_t *)SPEC12_STANDARD, 48, &e));
    TEST_ASSERT_EQUAL(16, e.len);
    uint8_t want[16];
    from_bits(SPEC12_BITS, want, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, e.bytes, 16);
}

void test_bad_checksum() {
    char s[49];
    memcpy(s, SPEC12_STANDARD, 49);
    memcpy(s + 44, "1211", 4);     // change the last (checksum) word
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_BAD_CHECKSUM, seedqr_decode((const uint8_t *)s, 48, &e));
    TEST_ASSERT_EQUAL(0, e.len);
}

void test_bad_index() {
    char s[49];
    memcpy(s, SPEC12_STANDARD, 49);
    memcpy(s, "2048", 4);
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_BAD_WORD_INDEX, seedqr_decode((const uint8_t *)s, 48, &e));
}

// A passphrase card of 16 or 32 characters isn't taken for Compact entropy.
void test_text_is_not_compact() {
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_TEXT, seedqr_decode((const uint8_t *)"correct horse ba", 16, &e));
    TEST_ASSERT_EQUAL(0, e.len);
    TEST_ASSERT_EQUAL(SEEDQR_TEXT, seedqr_decode((const uint8_t *)"0123456789012345", 16, &e));
}

void test_not_seedqr() {
    const char *url = "https://example.com";
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_NOT_SEEDQR, seedqr_decode((const uint8_t *)url, strlen(url), &e));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_standard_24);
    RUN_TEST(test_compact_24);
    RUN_TEST(test_standard_12);
    RUN_TEST(test_bad_checksum);
    RUN_TEST(test_bad_index);
    RUN_TEST(test_not_seedqr);
    RUN_TEST(test_text_is_not_compact);
    return UNITY_END();
}
