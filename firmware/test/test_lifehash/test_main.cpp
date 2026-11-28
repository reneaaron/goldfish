// LifeHash version 2 against pictures made by Sparrow's Java port (toucan).
// Run: pio test -e test -f test_lifehash
#include <string.h>
#include <unity.h>
#include <wally_core.h>
#include <wally_crypto.h>
#include "../../src/app/lifehash.h"
#include "../fixtures/lifehash_vectors.h"

void setUp() {}

static size_t unhex(const char *hex, uint8_t *out, size_t room) {
    size_t n = 0;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_hex_to_bytes(hex, out, room, &n));
    return n;
}

// toucan's own test: the first bytes of the picture for "Hello".
void test_hello_starts_as_toucan_says() {
    static const uint8_t first[] = {146, 126, 130, 178, 104, 92, 182, 101, 87, 202, 88, 64, 199, 89, 66,
                                    197, 90, 69, 182, 101, 87, 180, 102, 89, 159, 117, 114, 210, 82, 54};
    uint8_t rgb[LIFEHASH_RGB];
    TEST_ASSERT_TRUE(lifehash_v2((const uint8_t *)"Hello", 5, rgb));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, rgb, sizeof(first));
}

void test_whole_pictures_match() {
    for (const LifehashPicture &v : LIFEHASH_PICTURES) {
        uint8_t data[8], want[LIFEHASH_RGB], rgb[LIFEHASH_RGB];
        size_t n = unhex(v.data_hex, data, sizeof(data));
        TEST_ASSERT_EQUAL(LIFEHASH_RGB, unhex(v.rgb_hex, want, sizeof(want)));
        TEST_ASSERT_TRUE(lifehash_v2(data, n, rgb));
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(want, rgb, LIFEHASH_RGB, v.data_hex);
    }
}

// Many fingerprints, every pixel: compared by the picture's SHA-256.
void test_fingerprints_match() {
    for (const LifehashSum &v : LIFEHASH_SUMS) {
        uint8_t data[8], want[32], got[32], rgb[LIFEHASH_RGB];
        size_t n = unhex(v.data_hex, data, sizeof(data));
        unhex(v.picture_sha256, want, sizeof(want));
        TEST_ASSERT_TRUE(lifehash_v2(data, n, rgb));
        wally_sha256(rgb, sizeof(rgb), got, sizeof(got));
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(want, got, 32, v.data_hex);
    }
}

// Both symmetries and all four colour schemes come up among the fingerprints
// (or the comparison above would leave one untested): a picture mirrors left
// to right, or it doesn't and turns instead.
void test_both_patterns_occur() {
    int mirrored = 0, turned = 0;
    for (const LifehashSum &v : LIFEHASH_SUMS) {
        uint8_t data[8], rgb[LIFEHASH_RGB];
        size_t n = unhex(v.data_hex, data, sizeof(data));
        TEST_ASSERT_TRUE(lifehash_v2(data, n, rgb));
        bool mirror = true;
        for (int y = 0; y < LIFEHASH_SIDE && mirror; y++)
            for (int x = 0; x < LIFEHASH_SIDE / 2 && mirror; x++)
                mirror = !memcmp(rgb + (y * LIFEHASH_SIDE + x) * 3, rgb + (y * LIFEHASH_SIDE + 31 - x) * 3, 3);
        mirror ? mirrored++ : turned++;
    }
    TEST_ASSERT_GREATER_THAN(50, mirrored);
    TEST_ASSERT_GREATER_THAN(50, turned);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hello_starts_as_toucan_says);
    RUN_TEST(test_whole_pictures_match);
    RUN_TEST(test_fingerprints_match);
    RUN_TEST(test_both_patterns_occur);
    return UNITY_END();
}
