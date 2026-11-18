#include "word_reader.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "seed_create.h"
#include "seedqr.h"
#include "word_model.h"

static_assert(WM_H == WR_H && WM_W == WR_W && WM_T == WR_T && WM_C == WR_C, "word_model.h doesn't match");

// Activations ping-pong between two buffers, int16, channels x rows x columns:
//   input A (1x32x192) -> b1 B (16x16x96) -> b2 A (32x8x48) -> b3 B (64x4x48)
//   -> b4 A (96x48) -> t1 B (96x48) -> t2 A (96x48) -> logits (float)
#define BUF_A (WM_B2_CO * 8 * 48)
#define BUF_B (WM_B1_CO * 16 * 96)
static int16_t *buf_a, *buf_b;

bool word_reader_init() {
    if (buf_a && buf_b) return true;
    word_reader_free();
    buf_a = (int16_t *)malloc(sizeof(int16_t) * BUF_A);
    buf_b = (int16_t *)malloc(sizeof(int16_t) * BUF_B);
    if (!buf_a || !buf_b) { word_reader_free(); return false; }
    return true;
}

void word_reader_free() {
    if (buf_a) { secure_zero(buf_a, sizeof(int16_t) * BUF_A); free(buf_a); }
    if (buf_b) { secure_zero(buf_b, sizeof(int16_t) * BUF_B); free(buf_b); }
    buf_a = buf_b = nullptr;
}

// ---------------------------------------------------------------- input
static int64_t floor_div(int64_t n, int64_t d) {   // d > 0
    int64_t q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

// The k-th smallest value (0-based) from a histogram.
static int kth(const uint16_t *hist, int k) {
    for (int v = 0, seen = 0; v < 256; v++)
        if ((seen += hist[v]) > k) return v;
    return 255;
}

// Ink high, paper (the median) 0, the 99th percentile of ink 4096. Integer
// maths only, the same as qnormalise() in export_words.py.
void word_reader_normalise(const uint8_t *strip, int16_t *in) {
    const int n = WR_H * WR_W;
    static uint16_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) hist[255 - strip[i]]++;
    int64_t paper2 = kth(hist, n / 2 - 1) + kth(hist, n / 2);
    int64_t lo = kth(hist, 6081), hi = kth(hist, 6082);   // 99th percentile: 0.99 * 6143 = 6081.57
    int64_t top200 = 2 * (lo * 100 + 57 * (hi - lo)) - 100 * paper2;
    if (top200 < 16 * 200) top200 = 16 * 200;
    for (int i = 0; i < n; i++) {
        int64_t num = (2 * (int64_t)(255 - strip[i]) - paper2) * 100 * 4096;
        int64_t q = floor_div(2 * num + top200, 2 * top200);
        in[i] = int16_t(q < -2048 ? -2048 : q > 6144 ? 6144 : q);
    }
    secure_zero(hist, sizeof(hist));
}

// ---------------------------------------------------------------- network
static inline int16_t requant(int64_t acc, int32_t mult, int shift) {
    int64_t v = (acc * mult + ((int64_t)1 << (shift - 1))) >> shift;
    return int16_t(v < 0 ? 0 : v > 32767 ? 32767 : v);   // ReLU, int16
}

// 3x3 convolution (zero padding) + ReLU, then max over ph x pw blocks
// (rows/columns that don't fill a block are dropped, as in PyTorch).
static void conv2d(const int16_t *in, int ci, int h, int w, const int8_t *wt, const int32_t *bias, const int32_t *mult,
                   const int8_t *shift, int co, int ph, int pw, int16_t *out) {
    int oh = h / ph, ow = w / pw;
    for (int c = 0; c < co; c++) {
        const int8_t *k = wt + c * ci * 9;
        for (int oy = 0; oy < oh; oy++)
            for (int ox = 0; ox < ow; ox++) {
                int16_t best = 0;
                for (int dy = 0; dy < ph; dy++)
                    for (int dx = 0; dx < pw; dx++) {
                        int y = oy * ph + dy, x = ox * pw + dx;
                        int64_t acc = bias[c];   // 64-bit: int8 x int16 x 576 terms can pass 2^31
                        for (int i = 0; i < ci; i++) {
                            const int16_t *src = in + i * h * w;
                            const int8_t *kk = k + i * 9;
                            for (int ky = 0; ky < 3; ky++) {
                                int sy = y + ky - 1;
                                if (sy < 0 || sy >= h) continue;
                                const int16_t *row = src + sy * w;
                                for (int kx = 0; kx < 3; kx++) {
                                    int sx = x + kx - 1;
                                    if (sx >= 0 && sx < w) acc += row[sx] * kk[ky * 3 + kx];
                                }
                            }
                        }
                        int16_t v = requant(acc, mult[c], shift[c]);
                        if (v > best) best = v;
                    }
                out[(c * oh + oy) * ow + ox] = best;
            }
    }
}

// Convolution over the WR_T steps, kernel 3, zero padding, + ReLU.
static void conv1d(const int16_t *in, int ci, const int8_t *wt, const int32_t *bias, const int32_t *mult,
                   const int8_t *shift, int co, int16_t *out) {
    for (int c = 0; c < co; c++)
        for (int t = 0; t < WR_T; t++) {
            int64_t acc = bias[c];
            for (int i = 0; i < ci; i++) {
                const int8_t *kk = wt + (c * ci + i) * 3;
                const int16_t *src = in + i * WR_T;
                for (int k = 0; k < 3; k++) {
                    int s = t + k - 1;
                    if (s >= 0 && s < WR_T) acc += src[s] * kk[k];
                }
            }
            out[c * WR_T + t] = requant(acc, mult[c], shift[c]);
        }
}

void word_reader_logp(const uint8_t *strip, float *logp, int16_t *t2) {
    word_reader_normalise(strip, buf_a);
    conv2d(buf_a, 1, 32, 192, wm_b1_w, wm_b1_b, wm_b1_m, wm_b1_s, WM_B1_CO, 2, 2, buf_b);
    conv2d(buf_b, WM_B2_CI, 16, 96, wm_b2_w, wm_b2_b, wm_b2_m, wm_b2_s, WM_B2_CO, 2, 2, buf_a);
    conv2d(buf_a, WM_B3_CI, 8, 48, wm_b3_w, wm_b3_b, wm_b3_m, wm_b3_s, WM_B3_CO, 2, 1, buf_b);
    conv2d(buf_b, WM_B4_CI, 4, 48, wm_b4_w, wm_b4_b, wm_b4_m, wm_b4_s, WM_B4_CO, 4, 1, buf_a);   // max over height
    conv1d(buf_a, WM_T1_CI, wm_t1_w, wm_t1_b, wm_t1_m, wm_t1_s, WM_T1_CO, buf_b);
    conv1d(buf_b, WM_T2_CI, wm_t2_w, wm_t2_b, wm_t2_m, wm_t2_s, WM_T2_CO, buf_a);
    if (t2) memcpy(t2, buf_a, sizeof(int16_t) * WM_T2_CO * WR_T);
    for (int t = 0; t < WR_T; t++) {
        float *lp = logp + t * WR_C, mx = -1e30f, sum = 0;
        for (int c = 0; c < WR_C; c++) {
            int32_t acc = 0;
            const int8_t *row = wm_out_w + c * WM_T2_CO;
            for (int i = 0; i < WM_T2_CO; i++) acc += buf_a[i * WR_T + t] * row[i];
            lp[c] = (float)acc * wm_out_scale[c] + wm_out_b[c];
            if (lp[c] > mx) mx = lp[c];
        }
        for (int c = 0; c < WR_C; c++) sum += expf(lp[c] - mx);
        float lse = mx + logf(sum);
        for (int c = 0; c < WR_C; c++) lp[c] -= lse;
    }
    secure_zero(buf_a, sizeof(int16_t) * BUF_A);
    secure_zero(buf_b, sizeof(int16_t) * BUF_B);
}

// ---------------------------------------------------------------- words
#define NO_SCORE -1e30f
#define CLASS_DIGIT0 27       // 1-26 A-Z, 27-36 0-9, 37 '.', 38 ')'
#define MAX_EXT (2 * 8 + 1)   // a word's letters with a blank around each

// Best path of the letters of t (m of them, lowercase) through the
// log-probabilities, with an optional row number in front (any digits, '.'
// or ')', or nothing): the same as Lexicon.scores(viterbi=True) in
// handwriting/train_words.py.
static float path_score(const float *logp, const float *g_emit, const char *t, int m) {
    int ext[MAX_EXT], e = 2 * m + 1;
    bool skip[MAX_EXT];
    for (int s = 0; s < e; s++) {
        ext[s] = s & 1 ? t[s / 2] - 'a' + 1 : 0;
        skip[s] = s >= 2 && ext[s] != 0 && ext[s] != ext[s - 2];
    }
    float a[MAX_EXT], n[MAX_EXT];
    for (int s = 0; s < e; s++) a[s] = NO_SCORE;
    a[0] = logp[0];
    a[1] = logp[ext[1]];
    float g = g_emit[0];
    for (int tt = 1; tt < WR_T; tt++) {
        const float *lp = logp + tt * WR_C;
        for (int s = 0; s < e; s++) {
            float v = a[s];
            if (s >= 1 && a[s - 1] > v) v = a[s - 1];
            if (skip[s] && a[s - 2] > v) v = a[s - 2];
            if (s <= 1 && g > v) v = g;   // from the row number into the word
            n[s] = v + lp[ext[s]];
        }
        memcpy(a, n, sizeof(float) * e);
        g += g_emit[tt];
    }
    return a[e - 1] > a[e - 2] ? a[e - 1] : a[e - 2];
}

void word_reader_decode(const float *logp, bool (*allowed)(int), WordResult *out) {
    float g_emit[WR_T];
    for (int t = 0; t < WR_T; t++) {
        const float *lp = logp + t * WR_C;
        float v = lp[0];
        for (int c = CLASS_DIGIT0; c < WR_C; c++) if (lp[c] > v) v = lp[c];
        g_emit[t] = v;
    }
    float best = NO_SCORE, second = NO_SCORE;
    out->word = -1;
    for (int w = 0; w < 2048; w++) {
        if (allowed && !allowed(w)) continue;
        const char *s = seed_word(w);
        int len = (int)strlen(s);
        float sc = path_score(logp, g_emit, s, len);
        if (len > 4) {   // the first 4 letters: BIP-39 words are unique in them
            float sc4 = path_score(logp, g_emit, s, 4);
            if (sc4 > sc) sc = sc4;
        }
        if (sc > best) { second = best; best = sc; out->word = w; }
        else if (sc > second) second = sc;
    }
    out->margin = best - second;
}

bool word_reader_read(const uint8_t *strip, bool (*allowed)(int), WordResult *out) {
    static float logp[WR_T * WR_C];
    out->word = -1;
    out->margin = 0;
    if (!buf_a) return false;
    word_reader_logp(strip, logp, nullptr);
    word_reader_decode(logp, allowed, out);
    secure_zero(logp, sizeof(logp));
    return out->word >= 0 && out->margin >= WR_MIN_MARGIN;
}
