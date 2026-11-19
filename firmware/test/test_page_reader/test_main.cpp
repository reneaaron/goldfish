// Page reader + word reader end to end on sim/samples/handwritten_page.png:
// a synthetic photo (handwriting/make_page.py) of a 24-word seed, numbered, in two
// columns whose rows don't line up, ruled paper, a margin line, a 4 degree
// tilt, uneven light.
// Run: pio test -e test -f test_page_reader
#include <unity.h>
#include <stdio.h>
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

// Box-filtered resize of the full sample, w x h.
static uint8_t *load_resized(int tw, int th) {
    int w, h, n;
    uint8_t *img = stbi_load("sim/samples/handwritten_page.png", &w, &h, &n, 1);
    TEST_ASSERT_NOT_NULL(img);
    uint8_t *out = (uint8_t *)malloc((size_t)tw * th);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int xa = x * w / tw, xb = (x + 1) * w / tw, ya = y * h / th, yb = (y + 1) * h / th, sum = 0;
            for (int yy = ya; yy < yb; yy++)
                for (int xx = xa; xx < xb; xx++) sum += img[yy * w + xx];
            out[y * tw + x] = uint8_t(sum / ((xb - xa) * (yb - ya)));
        }
    stbi_image_free(img);
    return out;
}

// Counts the words offered, failing on one out of order.
static int read_in_order(const uint8_t *strips, int n) {
    int next = 0, offered = 0;
    for (int i = 0; i < n; i++) {
        WordResult r;
        if (!word_reader_read(strips + (size_t)i * WR_H * WR_W, nullptr, &r)) continue;
        int k = next;
        while (k < 24 && strcmp(WORDS[k], seed_word(r.word))) k++;
        TEST_ASSERT_TRUE_MESSAGE(k < 24, seed_word(r.word));
        next = k + 1;
        offered++;
    }
    return offered;
}

// The board: the words are found on its 240x320 preview frame, then cut from
// a picture twice that size, captured as bands of rows (it can't hold more).
// The page here is 200 pixels wide, a little short of filling the view: the
// preview's own strips then read about half the words, the sharper ones nearly all.
static void test_board_sized_bands() {
    const int lw = 200, lh = lw * 1800 / 1400, Z = 2, BAND = 100;
    uint8_t *low = load_resized(lw, lh), *hi = load_resized(lw * Z, lh * Z);
    TEST_ASSERT_TRUE(page_find(low, lw, lh, &page));
    static uint8_t strips[PAGE_MAX_WORDS][WR_H * WR_W];
    for (int i = 0; i < page.n; i++) page_strip(low, &page, i, strips[i]);
    int low_offered = read_in_order(&strips[0][0], page.n);
    for (int i = 0; i < page.n; i++) {
        int y0, y1;
        page_strip_rows(&page, i, Z, 0, &y0, &y1);
        TEST_ASSERT_TRUE(y1 > y0 && y1 - y0 <= BAND);   // one band holds a word
        int b0 = y1 - BAND < 0 ? 0 : y1 - BAND;          // a band ending at the word, as the board may get it
        page_strip_band(hi + (size_t)b0 * lw * Z, b0, BAND, Z, 0, 0, &page, i, strips[i]);
    }
    int hi_offered = read_in_order(&strips[0][0], page.n);
    printf("board-sized page: %d words found, %d read from the preview, %d from 2x bands\n", page.n, low_offered,
           hi_offered);
    TEST_ASSERT_GREATER_OR_EQUAL(20, hi_offered);
    TEST_ASSERT_GREATER_THAN(low_offered, hi_offered);
    free(low);
    free(hi);
}

// The sharp picture is a later frame: here everything in it sits 8 pixels
// right and 6 up from where the first picture has it (a hand that moved).
// Cut where page_find saw the words, the strips miss their starts; with each
// word's shift found by page_band_shift they read as well as unmoved.
static void test_band_shift() {
    const int lw = 200, lh = lw * 1800 / 1400, Z = 2, SX = 8, SY = -6, M = PAGE_SHIFT * Z, BAND = 100 + 2 * M;
    uint8_t *low = load_resized(lw, lh), *hi = load_resized(lw * Z, lh * Z);
    const int hw = lw * Z, hh = lh * Z;
    uint8_t *moved = (uint8_t *)malloc((size_t)hw * hh);
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
            int sx = x - SX < 0 ? 0 : x - SX >= hw ? hw - 1 : x - SX, sy = y - SY < 0 ? 0 : y - SY >= hh ? hh - 1 : y - SY;
            moved[y * hw + x] = hi[sy * hw + sx];
        }
    TEST_ASSERT_TRUE(page_find(low, lw, lh, &page));
    static uint8_t plain[PAGE_MAX_WORDS][WR_H * WR_W], fixed[PAGE_MAX_WORDS][WR_H * WR_W];
    int right = 0;
    for (int i = 0; i < page.n; i++) {
        int y0, y1, dx, dy;
        page_strip_rows(&page, i, Z, M, &y0, &y1);
        TEST_ASSERT_TRUE(y1 - y0 <= BAND);
        int b0 = y1 - BAND < 0 ? 0 : y1 - BAND;
        const uint8_t *band = moved + (size_t)b0 * hw;
        page_strip_band(band, b0, BAND, Z, 0, 0, &page, i, plain[i]);
        page_band_shift(low, lw, lh, band, b0, BAND, Z, &page, i, &dx, &dy);
        right += abs(dx - SX) <= 2 && abs(dy - SY) <= 2;
        page_strip_band(band, b0, BAND, Z, dx, dy, &page, i, fixed[i]);
    }
    int plain_offered = read_in_order(&plain[0][0], page.n), fixed_offered = read_in_order(&fixed[0][0], page.n);
    printf("moved 8 right, 6 up: shift found for %d of %d words; %d read uncorrected, %d corrected\n", right, page.n,
           plain_offered, fixed_offered);
    TEST_ASSERT_GREATER_OR_EQUAL(page.n - 2, right);   // a guessed (empty) word has nothing to match
    TEST_ASSERT_GREATER_OR_EQUAL(20, fixed_offered);
    free(low);
    free(hi);
    free(moved);
}

static void test_blank_page() {
    memset(gray, 210, (size_t)gw * gh);
    TEST_ASSERT_FALSE(page_find(gray, gw, gh, &page));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_reads_page_in_order);
    RUN_TEST(test_board_sized_bands);
    RUN_TEST(test_band_shift);
    RUN_TEST(test_blank_page);
    return UNITY_END();
}
