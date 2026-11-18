// Whole-word strip reader against handwriting/export_words.py: the integer
// network bit for bit, log-probabilities and the word read for synthetic
// strips (test/fixtures/word_vectors.h).
// Run: pio test -e test -f test_word_reader
#include <unity.h>
#include <math.h>
#include <string.h>
#include "../../src/app/seed_create.h"
#include "../../src/app/word_reader.h"
#include "../fixtures/word_vectors.h"

static float logp[WR_T * WR_C];
static int16_t buf[96 * WR_T > WR_H * WR_W ? 96 * WR_T : WR_H * WR_W];

void setUp() { TEST_ASSERT_TRUE(word_reader_init()); }
void tearDown() { word_reader_free(); }

static void test_normalise_matches() {
    word_reader_normalise(wv_strip[0], buf);
    TEST_ASSERT_EQUAL_INT16_ARRAY(wv_in0, buf, WR_H * WR_W);
}

static void test_network_bit_exact() {
    word_reader_logp(wv_strip[0], logp, buf);
    TEST_ASSERT_EQUAL_INT16_ARRAY(wv_t2_0, buf, 96 * WR_T);
}

static void test_logp_matches() {
    for (int v = 0; v < WV_N; v++) {
        word_reader_logp(wv_strip[v], logp, nullptr);
        for (int i = 0; i < WR_T * WR_C; i++) TEST_ASSERT_FLOAT_WITHIN(1e-4f, wv_logp[v][i], logp[i]);
    }
}

static void test_words_match() {
    for (int v = 0; v < WV_N; v++) {
        WordResult r;
        bool ok = word_reader_read(wv_strip[v], nullptr, &r);
        TEST_ASSERT_EQUAL_STRING(wv_word[v], seed_word(r.word));
        TEST_ASSERT_FLOAT_WITHIN(1e-2f, wv_margin[v], r.margin);
        TEST_ASSERT_EQUAL(wv_margin[v] >= WR_MIN_MARGIN, ok);
    }
}

static int banned;
static bool not_banned(int w) { return w != banned; }

// A word the caller rules out (e.g. a last word with the wrong checksum) is
// never picked, not even when it's what's written.
static void test_allowed_filter() {
    WordResult r;
    word_reader_read(wv_strip[0], nullptr, &r);
    banned = r.word;
    word_reader_read(wv_strip[0], not_banned, &r);
    TEST_ASSERT_NOT_EQUAL(banned, r.word);
}

static void test_blank_strip() {
    static uint8_t blank[WR_H * WR_W];
    memset(blank, 200, sizeof(blank));
    WordResult r;
    TEST_ASSERT_FALSE(word_reader_read(blank, nullptr, &r));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normalise_matches);
    RUN_TEST(test_network_bit_exact);
    RUN_TEST(test_logp_matches);
    RUN_TEST(test_words_match);
    RUN_TEST(test_allowed_filter);
    RUN_TEST(test_blank_strip);
    return UNITY_END();
}
