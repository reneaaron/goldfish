// UR crypto-psbt decoding/encoding against SeedSigner's multi-part vector.
// Run: pio test -e test
#include <unity.h>
#include <string.h>
#include <wally_core.h>
#include "../../src/app/ur.h"
#include "../fixtures/seedsigner_vectors.h"

static UrDecoder dec;

static void expect_base64(const uint8_t *p, size_t n, const char *want) {
    char *b64 = nullptr;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_base64_from_bytes(p, n, 0, &b64));
    TEST_ASSERT_EQUAL_STRING(want, b64);
    wally_free_string(b64);
}

void test_decode_multipart() {
    ur_decoder_reset(&dec);
    const uint8_t *payload = nullptr;
    size_t plen = 0;
    UrResult r = UR_NOT_UR;
    for (int i = 0; i < UR_MULTISIG_PARTS_N; i++) {
        r = ur_decoder_add(&dec, UR_MULTISIG_PARTS[i], strlen(UR_MULTISIG_PARTS[i]), &payload, &plen);
        TEST_ASSERT_TRUE(r == UR_PROGRESS || r == UR_COMPLETE);
        if (i < UR_MULTISIG_PARTS_N - 1) TEST_ASSERT_EQUAL(UR_PROGRESS, r);
    }
    TEST_ASSERT_EQUAL(UR_COMPLETE, r);
    expect_base64(payload, plen, UR_MULTISIG_BASE64);
}

void test_corrupt_part_rejected() {
    ur_decoder_reset(&dec);
    char s[2000];
    strcpy(s, UR_MULTISIG_PARTS[0]);
    size_t n = strlen(s);
    s[n - 10] = s[n - 10] == 'A' ? 'B' : 'A';   // flip a byteword letter
    const uint8_t *p; size_t pl;
    TEST_ASSERT_EQUAL(UR_BAD, ur_decoder_add(&dec, s, n, &p, &pl));
}

void test_not_ur() {
    const uint8_t *p; size_t pl;
    TEST_ASSERT_EQUAL(UR_NOT_UR, ur_decoder_add(&dec, "cHNidP8B", 8, &p, &pl));
    TEST_ASSERT_EQUAL(UR_WRONG_TYPE, ur_decoder_add(&dec, "ur:crypto-seed/abcd", 19, &p, &pl));
}

// Parts whose header would make the decoder write outside its buffer.
void test_out_of_range_parts_rejected() {
    const uint8_t *p; size_t pl;
    // seq 0 (sequence numbers are 1-based)
    const char *seq0 = "UR:CRYPTO-PSBT/1-2/LPAEAOBBAEHDBKFPFPFPFPFPFPFPFPFPFPDWLDCSWT";
    ur_decoder_reset(&dec);
    TEST_ASSERT_EQUAL(UR_BAD, ur_decoder_add(&dec, seq0, strlen(seq0), &p, &pl));
    // 100 fragments of 200 bytes for a 1-byte message: 20 KB, past the buffer
    static char big[600];
    strcpy(big, "UR:CRYPTO-PSBT/100-100/LPCSIECSIEADAEHDSP");
    for (int i = 0; i < 200; i++) strcat(big, "FP");
    strcat(big, "CYVARDIS");
    ur_decoder_reset(&dec);
    TEST_ASSERT_EQUAL(UR_BAD, ur_decoder_add(&dec, big, strlen(big), &p, &pl));
    TEST_ASSERT_EQUAL(0, (int)dec.found);
}

void test_encode_roundtrip() {
    // decode the vector, re-encode with a different fragment size, decode again
    ur_decoder_reset(&dec);
    const uint8_t *payload = nullptr; size_t plen = 0;
    for (int i = 0; i < UR_MULTISIG_PARTS_N; i++)
        ur_decoder_add(&dec, UR_MULTISIG_PARTS[i], strlen(UR_MULTISIG_PARTS[i]), &payload, &plen);
    static uint8_t psbt[8192];
    memcpy(psbt, payload, plen);

    size_t count = ur_part_count(plen, 100);
    TEST_ASSERT_GREATER_THAN(1, count);
    static UrDecoder d2;
    ur_decoder_reset(&d2);
    char part[1200];
    UrResult r = UR_NOT_UR;
    const uint8_t *p2 = nullptr; size_t pl2 = 0;
    for (uint32_t seq = 3; seq < 3 + count; seq++) {   // start mid-cycle, as a camera would
        size_t n = ur_encode_part(psbt, plen, 100, seq, part, sizeof(part));
        TEST_ASSERT_GREATER_THAN(0, n);
        r = ur_decoder_add(&d2, part, n, &p2, &pl2);
    }
    TEST_ASSERT_EQUAL(UR_COMPLETE, r);
    TEST_ASSERT_EQUAL(plen, pl2);
    TEST_ASSERT_EQUAL_MEMORY(psbt, p2, plen);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_decode_multipart);
    RUN_TEST(test_corrupt_part_rejected);
    RUN_TEST(test_not_ur);
    RUN_TEST(test_encode_roundtrip);
    RUN_TEST(test_out_of_range_parts_rejected);
    return UNITY_END();
}
