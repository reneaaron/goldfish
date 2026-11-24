#pragma once
#include <stdint.h>

// Reads one handwritten BIP-39 word from a strip: one row of a straightened
// photo of a backup, WR_H x WR_W grayscale, the word's letters filling about
// two thirds of the height, an optional row number in front ("7", "7.", "7)").
// Capitals, lowercase or mixed. Experimental.
//
// A small convolutional network (word_model.h, trained by
// handwriting/train_words.py, exported by handwriting/export_words.py) gives,
// for 48 steps across the strip, the probability of each letter, digit, '.',
// ')' or nothing. The letters are never spelled out on their own: every
// BIP-39 word (and the first 4 letters of the longer ones, the usual short
// form) is scored by its best path through those probabilities (CTC), and the
// best word is offered only if it clearly beats the runner-up.
//
// Integer maths (int8 weights, int16 activations), bit for bit the same as
// qforward() in export_words.py. Strips and buffers can show seed words:
// word_reader_free() wipes them.

#define WR_H 32
#define WR_W 192
#define WR_T 48             // steps across the strip
#define WR_C 39             // blank, A-Z, 0-9, '.', ')'
#define WR_MIN_MARGIN 5.0f  // natural log: the word must be ~150x likelier than the next
#define WR_TOPK 32          // most candidates word_reader_top gives

struct WordResult {
    int word;       // BIP-39 index of the best word, -1 if none
    float margin;   // its score lead over the next best word
};

// The network's buffers: 53.5 KB from the heap, and 29 KB more, from the heap
// too or, with lend, WR_LEND_BYTES of the caller's idle memory (4-byte aligned,
// wiped, not freed, by word_reader_free): the board's heap can't hold both.
#define WR_LEND_BYTES (29 * 1024)
bool word_reader_init(void *lend = nullptr);
void word_reader_free();
bool word_reader_holds(const void *lend);   // initialised with this loan

// allowed: which word indexes may be picked (nullptr = all). True when the
// best word leads by WR_MIN_MARGIN; out is filled either way.
bool word_reader_read(const uint8_t *strip, bool (*allowed)(int), WordResult *out);

// The k (2..WR_TOPK) likeliest words for a strip, best first: their indexes
// and scores (natural log, comparable within one strip). Returns how many.
int word_reader_top(const uint8_t *strip, bool (*allowed)(int), int k, int *words, float *scores);

// Platform hook: fn(a) and fn(b) at the same time, back when both are done.
// The board runs fn(a) on its second core; elsewhere (a weak default here)
// one after the other.
void platform_parallel(void (*fn)(void *), void *a, void *b);

// Exposed for tests.
void word_reader_normalise(const uint8_t *strip, int16_t *in);   // WR_H * WR_W
// Log-probabilities, WR_T x WR_C; t2: the last hidden layer (96 x WR_T), or nullptr.
void word_reader_logp(const uint8_t *strip, float *logp, int16_t *t2);
// Best-path word scores from log-probabilities (after word_reader_init: it
// works in the network's buffers).
void word_reader_decode(const float *logp, bool (*allowed)(int), WordResult *out);
