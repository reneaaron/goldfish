// Page reader + word reader end to end on sim/samples/handwritten_page.png:
// a synthetic photo (handwriting/make_page.py) of a 24-word seed, numbered, in two
// columns whose rows don't line up, ruled paper, a margin line, a 4 degree
// tilt, uneven light.
// Run: pio test -e test -f test_page_reader
#include <unity.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app/page_reader.h"
#include "../../src/app/seed_create.h"
#include "../../src/app/word_reader.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../src/sim/stb_image.h"

// A valid 24-word test seed (handwriting/make_page.py); never use it.
static const char *const WORDS[24] = {
    "calm", "scene", "merit", "capable", "noble", "great", "task", "reject", "also", "fury", "media",
    "vendor", "kiwi", "leaf", "census", "when", "skate", "canal", "obvious", "page", "ball", "certain",
    "unable", "test"};

static Page page;
static uint8_t *gray;
static int gw, gh;

void setUp() {
    TEST_ASSERT_TRUE(word_reader_init());
    int w, h, n;
    uint8_t *img = stbi_load("sim/samples/handwritten_page.png", &w, &h, &n, 1);
    TEST_ASSERT_NOT_NULL(img);
    gw = w / 2; gh = h / 2;   // 700x900, like page_cli 1000
    gray = (uint8_t *)malloc((size_t)gw * gh);
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
            gray[y * gw + x] = uint8_t((img[2 * y * w + 2 * x] + img[2 * y * w + 2 * x + 1] + img[(2 * y + 1) * w + 2 * x] +
                                        img[(2 * y + 1) * w + 2 * x + 1]) / 4);
    stbi_image_free(img);
}

void tearDown() {
    free(gray);
    word_reader_free();
}

// Every word offered is the right one, in reading order (left column top to
// bottom, then the right one), and nearly all of them are offered.
static void test_reads_page_in_order() {
    TEST_ASSERT_TRUE(page_find(gray, gw, gh, &page));
    static uint8_t strip[WR_H * WR_W];
    int next = 0, offered = 0;
    for (int i = 0; i < page.n; i++) {
        page_strip(gray, &page, i, strip);
        WordResult r;
        if (!word_reader_read(strip, nullptr, &r)) continue;
        int k = next;
        while (k < 24 && strcmp(WORDS[k], seed_word(r.word))) k++;
        TEST_ASSERT_TRUE_MESSAGE(k < 24, seed_word(r.word));   // a word that isn't next on the page
        next = k + 1;
        offered++;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(20, offered);
}

static void test_blank_page() {
    memset(gray, 210, (size_t)gw * gh);
    TEST_ASSERT_FALSE(page_find(gray, gw, gh, &page));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_reads_page_in_order);
    RUN_TEST(test_blank_page);
    return UNITY_END();
}
