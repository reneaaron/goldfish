// The board builds with -Os; this file is where reading a word spends its
// time (about 28M multiply-adds), so it gets -O2.
#pragma GCC optimize("O2")
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
// Zeros all around each channel, so the convolutions have no edges to check:
// every row starts with two (two keep each pair of columns 4-byte aligned),
// which are also the previous row's right edge, and every channel starts with
// a row of them, which is also the previous channel's bottom edge; a row and
// two more close the last channel. From b4 on a channel is one row, the
// steps.
#define PAD 2
#define STRIDE(w) ((w) + PAD)
#define PLANE(h, w) (((h) + 1) * STRIDE(w))
#define LAYER(c, h, w) ((c) * PLANE(h, w) + STRIDE(w) + PAD)   // c channels and the closing zeros
#define STEPS(c) ((c) * STRIDE(WR_T) + PAD)                     // the same from b4 on
#define BUF_A LAYER(WM_B2_CO, 8, 48)
#define BUF_B LAYER(WM_B1_CO, 16, 96)
static_assert(BUF_A >= LAYER(1, 32, 192) && BUF_A >= STEPS(WM_B4_CO) && BUF_B >= LAYER(WM_B3_CO, 4, 48),
              "buffers too small");
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
// maths only, the same as qnormalise() in export_words.py. Row y goes to
// in + y * stride.
static void normalise(const uint8_t *strip, int16_t *in, int stride) {
    const int n = WR_H * WR_W;
    static uint16_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) hist[255 - strip[i]]++;
    int64_t paper2 = kth(hist, n / 2 - 1) + kth(hist, n / 2);
    int64_t lo = kth(hist, 6081), hi = kth(hist, 6082);   // 99th percentile: 0.99 * 6143 = 6081.57
    int64_t top200 = 2 * (lo * 100 + 57 * (hi - lo)) - 100 * paper2;
    if (top200 < 16 * 200) top200 = 16 * 200;
    for (int y = 0; y < WR_H; y++)
        for (int x = 0; x < WR_W; x++) {
            int64_t num = (2 * (int64_t)(255 - strip[y * WR_W + x]) - paper2) * 100 * 4096;
            int64_t q = floor_div(2 * num + top200, 2 * top200);
            in[y * stride + x] = int16_t(q < -2048 ? -2048 : q > 6144 ? 6144 : q);
        }
    secure_zero(hist, sizeof(hist));
}

void word_reader_normalise(const uint8_t *strip, int16_t *in) { normalise(strip, in, WR_W); }

// ---------------------------------------------------------------- network
static inline int16_t requant(int64_t acc, int32_t mult, int shift) {
    int64_t v = (acc * mult + ((int64_t)1 << (shift - 1))) >> shift;
    return int16_t(v < 0 ? 0 : v > 32767 ? 32767 : v);   // ReLU, int16
}

// Speed (on the board a word spent ~2.2 s in the network at first, ~0.85 s
// with each sum kept in memory and updated once per input channel): the two
// cores take half the output channels each, and six neighbouring outputs of
// one output channel are summed in registers over every input channel, so
// each sum is stored once. A kernel row's 3 weights are held packed in pairs, and the
// inputs are read as aligned pairs of columns, each pair used by the two or
// three outputs that need it: per kernel row, five loads and 12 DSP
// instructions (SMLAD: two multiply-adds in one) for 18 multiply-adds.
// Integer sums don't depend on the order: the same result as the plain loops,
// bit for bit.
//
// One input channel adds at most 9 x 32767 x 128 (under 2^26) to a sum, so 32
// of them stay under 2^31: sums are 32-bit per group of 32 input channels and
// 64-bit across groups.

// Halves of a word as int16 (low first), as the DSP instructions take them;
// the same arithmetic in C where the CPU has none, so the tests run the code
// the board runs.
#if defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
static inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc) { return __smlad(a, b, acc); }    // lo*lo + hi*hi
static inline int32_t smlatb(uint32_t a, uint32_t b, int32_t acc) { return __smlatb(a, b, acc); }  // hi(a)*lo(b)
static inline int32_t smlabt(uint32_t a, uint32_t b, int32_t acc) { return __smlabt(a, b, acc); }  // lo(a)*hi(b)
#else
static inline int32_t lo16(uint32_t v) { return int16_t(v & 0xffff); }
static inline int32_t hi16(uint32_t v) { return int16_t(v >> 16); }
static inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc) { return acc + lo16(a) * lo16(b) + hi16(a) * hi16(b); }
static inline int32_t smlatb(uint32_t a, uint32_t b, int32_t acc) { return acc + hi16(a) * lo16(b); }
static inline int32_t smlabt(uint32_t a, uint32_t b, int32_t acc) { return acc + lo16(a) * hi16(b); }
#endif
static inline uint32_t pair_at(const int16_t *p) {   // p[0] low, p[1] high
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint32_t pack(int8_t lo, int8_t hi) { return uint16_t(int16_t(lo)) | uint32_t(uint16_t(int16_t(hi))) << 16; }

// One output channel's weights, per kernel row (k0, k1, k2): the pairs
// (k0, k1), (k1, k2) and (k0, k2). One set per core.
#define KPACK_N (WM_B4_CI * 9)
static_assert(WM_T1_CI * 3 <= KPACK_N && WM_T2_CI * 3 <= KPACK_N, "kpack too small");
static uint32_t kpack[2][KPACK_N];

static void pack_weights(const int8_t *k, int rows, uint32_t *kp) {
    for (int r = 0; r < rows; r++, k += 3) {
        kp[3 * r] = pack(k[0], k[1]);
        kp[3 * r + 1] = pack(k[1], k[2]);
        kp[3 * r + 2] = pack(k[0], k[2]);
    }
}

// The sums of outputs x0..x0+5 over n input channels of R kernel rows each.
// d: the first channel's first row from column x0 - 2 (in the zeros when x0
// is 0); S, CH: the row and channel strides; k: packed weights.
//
// Output x takes columns x-1, x, x+1. With column pairs w0 = (x0-2, x0-1),
// w1 = (x0, x0+1), ...: x0 = hi(w0) k0 + w1 . (k1, k2), and
// x0+1 = w1 . (k0, k1) + lo(w2) k2, and so on along the pairs.
template <int S, int CH, int R>
static inline void sum6(const int16_t *d, const uint32_t *k, int n, int32_t *o) {
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0;
    for (const int16_t *end = d + n * CH; d < end; d += CH) {
#pragma GCC unroll 3
        for (int r = 0; r < R; r++, k += 3) {
            const int16_t *p = d + r * S;
            const uint32_t k01 = k[0], k12 = k[1], k02 = k[2];
            uint32_t w0 = pair_at(p), w1 = pair_at(p + 2);
            a0 = smlatb(w0, k02, a0);
            a0 = smlad(w1, k12, a0);
            a1 = smlad(w1, k01, a1);
            a2 = smlatb(w1, k02, a2);
            uint32_t w2 = pair_at(p + 4);
            a1 = smlabt(w2, k02, a1);
            a2 = smlad(w2, k12, a2);
            a3 = smlad(w2, k01, a3);
            a4 = smlatb(w2, k02, a4);
            uint32_t w3 = pair_at(p + 6);
            a3 = smlabt(w3, k02, a3);
            a4 = smlad(w3, k12, a4);
            a5 = smlad(w3, k01, a5);
            a5 = smlabt(pair_at(p + 8), k02, a5);
        }
    }
    o[0] = a0, o[1] = a1, o[2] = a2, o[3] = a3, o[4] = a4, o[5] = a5;
}

// A layer's output channels [c0, c1): the two cores take half each
// (platform_parallel), every channel's outputs its own, the input shared.
struct Part {
    const int16_t *in;
    const int8_t *wt;
    const int32_t *bias, *mult;
    const int8_t *shift;
    int16_t *out;
    int c0, c1;
    uint32_t *kp;
};

__attribute__((weak)) void platform_parallel(void (*fn)(void *), void *a, void *b) {
    fn(a);
    fn(b);
}

static void halves(void (*fn)(void *), const int16_t *in, const int8_t *wt, const int32_t *bias, const int32_t *mult,
                   const int8_t *shift, int co, int16_t *out) {
    Part a = {in, wt, bias, mult, shift, out, co / 2, co, kpack[1]};
    Part b = {in, wt, bias, mult, shift, out, 0, co / 2, kpack[0]};
    platform_parallel(fn, &a, &b);
}

// 3x3 convolution (zero padding) + ReLU, then max over PH x PW blocks, from
// CI channels of H x W (H / PH == 1: into the one-row layout). The output
// starts as zeros: the border, and max() starts from 0 (ReLU).
template <int H, int W, int CI, int PH, int PW>
static void conv2d_part(void *arg) {
    const Part &p = *(const Part *)arg;
    constexpr int S = STRIDE(W), CH = PLANE(H, W), OH = H / PH, OW = W / PW, OS = STRIDE(OW);
    constexpr int OCH = OH == 1 ? OS : PLANE(OH, OW), G = CI < 32 ? CI : 32;
    static_assert(H % PH == 0 && W % PW == 0 && W % 6 == 0 && CI % G == 0, "shape");
    for (int c = p.c0; c < p.c1; c++) {
        pack_weights(p.wt + c * CI * 9, CI * 3, p.kp);
        int16_t *o = p.out + c * OCH + (OH == 1 ? 0 : OS) + PAD;
        for (int y = 0; y < H; y++) {
            int16_t *orow = o + (y / PH) * OS;
            for (int x0 = 0; x0 < W; x0 += 6) {
                int64_t tot[6];
                for (int i = 0; i < 6; i++) tot[i] = p.bias[c];
                for (int g = 0; g < CI; g += G) {
                    int32_t s[6];
                    sum6<S, CH, 3>(p.in + g * CH + y * S + x0, p.kp + g * 9, G, s);
                    for (int i = 0; i < 6; i++) tot[i] += s[i];
                }
                for (int i = 0; i < 6; i++) {
                    int16_t v = requant(tot[i], p.mult[c], p.shift[c]);
                    int16_t &dst = orow[(x0 + i) / PW];
                    if (v > dst) dst = v;
                }
            }
        }
    }
}

template <int H, int W, int CI, int PH, int PW>
static void conv2d(const int16_t *in, const int8_t *wt, const int32_t *bias, const int32_t *mult, const int8_t *shift,
                   int co, int16_t *out) {
    constexpr int OH = H / PH, OW = W / PW;
    memset(out, 0, sizeof(int16_t) * (OH == 1 ? STEPS(co) : LAYER(co, OH, OW)));
    halves(conv2d_part<H, W, CI, PH, PW>, in, wt, bias, mult, shift, co, out);
}

// Convolution over the WR_T steps, kernel 3, zero padding, + ReLU (96
// channels of 3 terms stay under 2^31: one 32-bit sum).
template <int CI>
static void conv1d_part(void *arg) {
    const Part &p = *(const Part *)arg;
    constexpr int S = STRIDE(WR_T);
    static_assert(WR_T % 6 == 0 && CI * 3 * 32767 * 128LL < (1LL << 31), "shape");
    for (int c = p.c0; c < p.c1; c++) {
        pack_weights(p.wt + c * CI * 3, CI, p.kp);
        for (int t0 = 0; t0 < WR_T; t0 += 6) {
            int32_t s[6];
            sum6<S, S, 1>(p.in + t0, p.kp, CI, s);
            for (int i = 0; i < 6; i++)
                p.out[c * S + PAD + t0 + i] = requant(int64_t(p.bias[c]) + s[i], p.mult[c], p.shift[c]);
        }
    }
}

template <int CI>
static void conv1d(const int16_t *in, const int8_t *wt, const int32_t *bias, const int32_t *mult, const int8_t *shift,
                   int co, int16_t *out) {
    memset(out, 0, sizeof(int16_t) * STEPS(co));
    halves(conv1d_part<CI>, in, wt, bias, mult, shift, co, out);
}

void word_reader_logp(const uint8_t *strip, float *logp, int16_t *t2) {
    constexpr int S = STRIDE(WR_T);
    memset(buf_a, 0, sizeof(int16_t) * LAYER(1, WR_H, WR_W));
    normalise(strip, buf_a + STRIDE(WR_W) + PAD, STRIDE(WR_W));
    conv2d<32, 192, WM_B1_CI, 2, 2>(buf_a, wm_b1_w, wm_b1_b, wm_b1_m, wm_b1_s, WM_B1_CO, buf_b);
    conv2d<16, 96, WM_B2_CI, 2, 2>(buf_b, wm_b2_w, wm_b2_b, wm_b2_m, wm_b2_s, WM_B2_CO, buf_a);
    conv2d<8, 48, WM_B3_CI, 2, 1>(buf_a, wm_b3_w, wm_b3_b, wm_b3_m, wm_b3_s, WM_B3_CO, buf_b);
    conv2d<4, 48, WM_B4_CI, 4, 1>(buf_b, wm_b4_w, wm_b4_b, wm_b4_m, wm_b4_s, WM_B4_CO, buf_a);   // max over height
    conv1d<WM_T1_CI>(buf_a, wm_t1_w, wm_t1_b, wm_t1_m, wm_t1_s, WM_T1_CO, buf_b);
    conv1d<WM_T2_CI>(buf_b, wm_t2_w, wm_t2_b, wm_t2_m, wm_t2_s, WM_T2_CO, buf_a);
    const int16_t *h = buf_a + PAD;   // channel i, step t: h[i * S + t]
    if (t2)
        for (int i = 0; i < WM_T2_CO; i++) memcpy(t2 + i * WR_T, h + i * S, sizeof(int16_t) * WR_T);
    secure_zero(buf_b, sizeof(int16_t) * BUF_B);   // done with: logp may be in it (word_reader_read)
    for (int t = 0; t < WR_T; t++) {
        float *lp = logp + t * WR_C, mx = -1e30f, sum = 0;
        for (int c = 0; c < WR_C; c++) {
            int32_t acc = 0;
            const int8_t *row = wm_out_w + c * WM_T2_CO;
            for (int i = 0; i < WM_T2_CO; i++) acc += h[i * S + t] * row[i];
            lp[c] = (float)acc * wm_out_scale[c] + wm_out_b[c];
            if (lp[c] > mx) mx = lp[c];
        }
        for (int c = 0; c < WR_C; c++) sum += expf(lp[c] - mx);
        float lse = mx + logf(sum);
        for (int c = 0; c < WR_C; c++) lp[c] -= lse;
    }
    secure_zero(buf_a, sizeof(int16_t) * BUF_A);
}

// ---------------------------------------------------------------- words
#define NO_SCORE -1e30f
#define CLASS_DIGIT0 27       // 1-26 A-Z, 27-36 0-9, 37 '.', 38 ')'

// A word's score is the best path of its letters through the
// log-probabilities, with an optional row number in front (any digits, '.'
// or ')', or nothing): the same as Lexicon.scores(viterbi=True) in
// handwriting/train_words.py. A word of more than 4 letters also scores as
// its first 4 (BIP-39 words are unique in them), and keeps the better.
//
// Speed: the path of a word's first d letters is the same for every word that
// starts with them, so the words are walked as a tree of prefixes (the list
// is in alphabetical order), each prefix adding its letter's two states (the
// letter, then a blank) over all WR_T steps. A prefix whose best possible
// ending can't reach the second best word so far is not followed: no word
// under it could change the result. The likeliest letters are followed first
// so that bar rises early. Each score is the same float sum as a word scored
// on its own, so the result doesn't depend on the order (ties go to the
// lower index, as in list order).
#define MAX_DEPTH 8   // the longest BIP-39 word
#define SLACK 0.05f   // float rounding of a bound's sum against the path's own

struct Child { int lo, hi; float ub; };   // a next letter: its words [lo, hi), their bound

// Lives in buf_a (free once the log-probabilities are out): no RAM of its own,
// and the walk's recursion keeps its lists of next letters here, not on the stack.
struct Decoder {
    const float *logp;
    bool (*allowed)(int);
    float g[WR_T];            // g[t]: the best row-number path up to step t-1, summed step by step
    float rest[WR_T + 1];     // rest[t]: the best any path can still gain from step t on
    float let[MAX_DEPTH + 1][WR_T], blank[MAX_DEPTH + 1][WR_T];   // a prefix's two states, per step
    Child kids[MAX_DEPTH + 1][26];
    float best, second;
    int word;
};
static_assert(sizeof(Decoder) <= sizeof(int16_t) * BUF_A, "decoder doesn't fit in buf_a");

// The states of the prefix of depth d (letter cls, 1-26) from those of its parent.
static void extend(Decoder *dc, int d, int cls, bool skip) {
    const float *lp = dc->logp;
    float *A = dc->let[d], *B = dc->blank[d];
    const float *pb = dc->blank[d - 1], *pl = dc->let[d - 1];
    A[0] = d == 1 ? lp[cls] : NO_SCORE;
    B[0] = NO_SCORE;
    for (int t = 1; t < WR_T; t++) {
        const float *row = lp + t * WR_C;
        float v = A[t - 1];
        if (pb[t - 1] > v) v = pb[t - 1];
        if (skip && pl[t - 1] > v) v = pl[t - 1];
        if (d == 1 && dc->g[t] > v) v = dc->g[t];   // from the row number into the word
        A[t] = v + row[cls];
        float u = B[t - 1];
        if (A[t - 1] > u) u = A[t - 1];
        B[t] = u + row[0];
    }
}

// The best any word under the prefix of depth d can score.
static float bound(const Decoder *dc, int d) {
    float ub = NO_SCORE;
    for (int t = 0; t < WR_T; t++) {
        float v = dc->let[d][t] > dc->blank[d][t] ? dc->let[d][t] : dc->blank[d][t];
        if (v + dc->rest[t + 1] > ub) ub = v + dc->rest[t + 1];
    }
    return ub;
}

static float ending(const Decoder *dc, int d) {
    float a = dc->blank[d][WR_T - 1], b = dc->let[d][WR_T - 1];
    return a > b ? a : b;
}

static void offer(Decoder *dc, int w, float sc) {
    if (sc > dc->best || (sc == dc->best && w < dc->word)) {
        dc->second = dc->best;
        dc->best = sc;
        dc->word = w;
    } else if (sc > dc->second) dc->second = sc;
}

// Words [lo, hi) of the list share their first d letters, whose states are in
// dc at depth d; sc4: their 4-letter score once d >= 4 (one word is left by then).
static void walk(Decoder *dc, int lo, int hi, int d, float sc4) {
    if (d == 4) sc4 = ending(dc, 4);
    if (d >= 1 && (d < 4 || sc4 + SLACK < dc->second) && bound(dc, d) + SLACK < dc->second) return;
    if (dc->allowed) {
        int w = lo;
        while (w < hi && !dc->allowed(w)) w++;
        if (w == hi) return;
    }
    // the word that ends here (in the list before the longer ones)
    if (d >= 1 && (int)strlen(seed_word(lo)) == d) {
        if (!dc->allowed || dc->allowed(lo)) {
            float sc = ending(dc, d);
            if (d > 4 && sc4 > sc) sc = sc4;
            offer(dc, lo, sc);
        }
        lo++;
    }
    // the next letters, likeliest first
    Child *kids = dc->kids[d];
    int n = 0;
    for (int w = lo; w < hi;) {
        char c = seed_word(w)[d];
        int e = w + 1;
        while (e < hi && seed_word(e)[d] == c) e++;
        kids[n++] = {w, e, 0};
        w = e;
    }
    if (n > 1) {
        for (int k = 0; k < n; k++) {
            int cls = seed_word(kids[k].lo)[d] - 'a' + 1;
            extend(dc, d + 1, cls, d >= 1 && seed_word(kids[k].lo)[d] != seed_word(kids[k].lo)[d - 1]);
            kids[k].ub = bound(dc, d + 1);
        }
        for (int i = 1; i < n; i++)   // insertion sort, highest bound first
            for (int j = i; j > 0 && kids[j].ub > kids[j - 1].ub; j--) {
                Child tmp = kids[j];
                kids[j] = kids[j - 1];
                kids[j - 1] = tmp;
            }
    }
    for (int k = 0; k < n; k++) {
        if (n > 1 && kids[k].ub + SLACK < dc->second && (d < 4 || sc4 + SLACK < dc->second)) break;   // sorted: none after either
        const char *s = seed_word(kids[k].lo);
        extend(dc, d + 1, s[d] - 'a' + 1, d >= 1 && s[d] != s[d - 1]);
        walk(dc, kids[k].lo, kids[k].hi, d + 1, sc4);
    }
}

void word_reader_decode(const float *logp, bool (*allowed)(int), WordResult *out) {
    out->word = -1;
    out->margin = 0;
    if (!buf_a) return;
    Decoder &dc = *(Decoder *)buf_a;
    dc.logp = logp;
    dc.allowed = allowed;
    float g = 0;
    for (int t = 0; t < WR_T; t++) {
        const float *lp = logp + t * WR_C;
        float v = lp[0];
        for (int c = CLASS_DIGIT0; c < WR_C; c++) if (lp[c] > v) v = lp[c];
        if (t == 0) g = v;
        else { dc.g[t] = g; g += v; }
    }
    dc.rest[WR_T] = 0;
    for (int t = WR_T - 1; t >= 0; t--) {
        const float *lp = logp + t * WR_C;
        float v = lp[0];
        for (int c = 1; c < WR_C; c++) if (lp[c] > v) v = lp[c];
        dc.rest[t] = dc.rest[t + 1] + v;
    }
    // depth 0: the blank before the word, after an optional row number
    dc.blank[0][0] = logp[0];
    dc.let[0][0] = NO_SCORE;
    for (int t = 1; t < WR_T; t++) {
        float v = dc.blank[0][t - 1];
        if (dc.g[t] > v) v = dc.g[t];
        dc.blank[0][t] = v + logp[t * WR_C];
        dc.let[0][t] = NO_SCORE;
    }
    dc.best = dc.second = NO_SCORE;
    dc.word = -1;
    walk(&dc, 0, 2048, 0, 0);
    out->word = dc.word;
    out->margin = dc.best - dc.second;
    secure_zero(&dc, sizeof(dc));
}

bool word_reader_read(const uint8_t *strip, bool (*allowed)(int), WordResult *out) {
    out->word = -1;
    out->margin = 0;
    if (!buf_a) return false;
    float *logp = (float *)buf_b;   // free once the network is done with it (see word_reader_logp)
    static_assert(sizeof(float) * WR_T * WR_C <= sizeof(int16_t) * BUF_B, "logp doesn't fit in buf_b");
    word_reader_logp(strip, logp, nullptr);
    word_reader_decode(logp, allowed, out);
    secure_zero(logp, sizeof(float) * WR_T * WR_C);
    return out->word >= 0 && out->margin >= WR_MIN_MARGIN;
}
