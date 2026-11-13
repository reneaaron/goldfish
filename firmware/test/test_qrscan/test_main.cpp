// QR decoding of the pictures in sim/samples, fitted into a frame the size the
// board camera delivers (240x180).
// Run: pio test -e test
#include <unity.h>
#include <math.h>
#include <string.h>
#include "../../src/app/qrscan.h"
#include "../../src/app/seedqr.h"
#include "../fixtures/seedsigner_vectors.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../src/sim/stb_image.h"

#define CAM_W 240
#define CAM_H 180
static uint8_t frame[CAM_W * CAM_H];
static QrPayload p;

// "attack pizza motion ... expire", 24 words
static const char *V1_STANDARD =
    "011513251154012711900771041507421289190620080870026613431420201617920614089619290300152408010643";

static void load(const char *path) {
    int w, h, n;
    uint8_t *img = stbi_load(path, &w, &h, &n, 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(img, path);
    memset(frame, 128, sizeof(frame));
    float s = fminf((float)CAM_W / w, (float)CAM_H / h);
    int dw = int(w * s), dh = int(h * s), ox = (CAM_W - dw) / 2, oy = (CAM_H - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            frame[(oy + y) * CAM_W + ox + x] = img[int(y / s) * w + int(x / s)];
    stbi_image_free(img);
}

void setUp() {
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_TRUE(qrscan_init(CAM_W, CAM_H));
}
void tearDown() { qrscan_free(); }

void test_standard_seedqr() {
    load("sim/samples/vector1_standard_24word.png");
    TEST_ASSERT_TRUE(qrscan_frame(frame, &p));
    TEST_ASSERT_EQUAL(strlen(V1_STANDARD), p.len);
    TEST_ASSERT_EQUAL_MEMORY(V1_STANDARD, p.data, p.len);
}

void test_compact_seedqr() {
    load("sim/samples/vector4_compact_12word.png");
    TEST_ASSERT_TRUE(qrscan_frame(frame, &p));
    TEST_ASSERT_EQUAL(16, p.len);
    SeedEntropy e;
    TEST_ASSERT_EQUAL(SEEDQR_OK, seedqr_decode(p.data, p.len, &e));
    TEST_ASSERT_EQUAL(12, seedqr_word_count(e));
}

void test_psbt() {
    load("sim/samples/testpsbt_height_demise_base64.png");
    TEST_ASSERT_TRUE(qrscan_frame(frame, &p));
    TEST_ASSERT_EQUAL(strlen(SINGLESIG_FRAME), p.len);
    TEST_ASSERT_EQUAL_MEMORY(SINGLESIG_FRAME, p.data, p.len);
}

void test_no_qr() {
    memset(frame, 128, sizeof(frame));
    TEST_ASSERT_FALSE(qrscan_frame(frame, &p));
    TEST_ASSERT_EQUAL(0, p.len);
}

void test_frame_before_init() {
    qrscan_free();
    TEST_ASSERT_FALSE(qrscan_frame(frame, &p));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_standard_seedqr);
    RUN_TEST(test_compact_seedqr);
    RUN_TEST(test_psbt);
    RUN_TEST(test_no_qr);
    RUN_TEST(test_frame_before_init);
    return UNITY_END();
}
