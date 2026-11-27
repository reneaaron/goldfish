#include "lifehash.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wally_crypto.h>

// See lifehash.h. Names follow the reference where it helps to compare.
// Numbers are doubles and the operations are kept in the reference's order,
// so the result rounds the same way as Sparrow's.
namespace {

constexpr int N = 16;                 // cells per side; the picture mirrors them to 32
constexpr int MAX_GENERATIONS = 150;
constexpr int GRID_BYTES = N * N / 8;

struct Color {
    double r, g, b;
};

// Java's Math.min/max, which toucan uses: NaN in, NaN out.
double jmin(double a, double b) { return isnan(a) || isnan(b) ? NAN : a < b ? a : b; }
double jmax(double a, double b) { return isnan(a) || isnan(b) ? NAN : a > b ? a : b; }
double clamped(double n) { return jmax(jmin(n, 1), 0); }
double lerp_from(double from_a, double from_b, double t) { return (from_a - t) / (from_a - from_b); }

Color lerp_to(const Color &c, const Color &other, double t) {
    double f = clamped(t);
    return {clamped(c.r * (1 - f) + other.r * f), clamped(c.g * (1 - f) + other.g * f),
            clamped(c.b * (1 - f) + other.b * f)};
}
const Color WHITE = {1, 1, 1}, BLACK = {0, 0, 0};
Color lighten(const Color &c, double t) { return lerp_to(c, WHITE, t); }
Color darken(const Color &c, double t) { return lerp_to(c, BLACK, t); }
double luminance(const Color &c) {
    return sqrt(pow(0.299 * c.r, 2) + pow(0.587 * c.g, 2) + pow(0.114 * c.b, 2));
}

// A gradient: up to four colours blended in equal segments, read forwards or backwards.
struct Gradient {
    Color colors[4];
    int count;
    bool reversed;
};

Color blend(const Color *colors, int count, double t) {
    if (count == 2) return lerp_to(colors[0], colors[1], t);
    if (t >= 1) return colors[count - 1];
    if (t <= 0) return colors[0];
    if (isnan(t)) return {NAN, NAN, NAN};
    int segments = count - 1;
    double s = t * segments;
    int segment = int(s);
    return lerp_to(colors[segment], colors[segment + 1], fmod(s, 1));
}
Color at(const Gradient &g, double t) { return blend(g.colors, g.count, g.reversed ? 1 - t : t); }

Color spectrum_cmyk_safe(double t) {
    static const uint8_t stops[7][3] = {{0, 168, 222}, {41, 60, 130}, {210, 59, 130}, {217, 63, 53},
                                        {244, 228, 81}, {0, 158, 84}, {0, 168, 222}};
    Color c[7];
    // Divided as floats, as toucan does (the reference divides doubles): now
    // and then that is one step of a colour's 256, and Sparrow's is the one to match.
    for (int i = 0; i < 7; i++) c[i] = {float(stops[i][0]) / 255.0f, float(stops[i][1]) / 255.0f, float(stops[i][2]) / 255.0f};
    return blend(c, 7, t);
}

// The digest's bits, most significant first.
struct Bits {
    const uint8_t *data;
    int at;
    bool next() {
        bool b = (data[at >> 3] >> (7 - (at & 7))) & 1;
        at++;
        return b;
    }
    int next_uint(int bits) {
        int v = 0;
        while (bits--) v = (v << 1) | (next() ? 1 : 0);
        return v;
    }
    double next_frac() { return next_uint(16) / 65535.0; }
};

Gradient monochromatic(Bits &e) {
    double hue = e.next_frac();
    bool is_tint = e.next();
    bool is_reversed = e.next();
    double key_advance = e.next_frac() * 0.3 + 0.05;
    double neutral_advance = e.next_frac() * 0.3 + 0.05;
    Color key = spectrum_cmyk_safe(hue);
    double contrast_brightness = 0;
    if (is_tint) {
        contrast_brightness = 1;
        key = darken(key, 0.5);
    }
    Color neutral = lerp_to(BLACK, WHITE, contrast_brightness);
    return {{lerp_to(key, neutral, key_advance), lerp_to(neutral, key, neutral_advance)}, 2, is_reversed};
}

Gradient complementary(Bits &e) {
    double spectrum1 = e.next_frac();
    double spectrum2 = fmod(spectrum1 + 0.5, 1);
    double lighter_advance = e.next_frac() * 0.3;
    double darker_advance = e.next_frac() * 0.3;
    bool is_reversed = e.next();
    Color c1 = spectrum_cmyk_safe(spectrum1), c2 = spectrum_cmyk_safe(spectrum2);
    bool first_lighter = luminance(c1) > luminance(c2);
    const Color &darker = first_lighter ? c2 : c1, &lighter = first_lighter ? c1 : c2;
    return {{darken(darker, darker_advance), lighten(lighter, lighter_advance)}, 2, is_reversed};
}

Gradient triadic(Bits &e) {
    double spectrum1 = e.next_frac();
    double spectrum2 = fmod(spectrum1 + 1.0 / 3, 1);
    double spectrum3 = fmod(spectrum1 + 2.0 / 3, 1);
    double lighter_advance = e.next_frac() * 0.3;
    double darker_advance = e.next_frac() * 0.3;
    bool is_reversed = e.next();
    Color c[3] = {spectrum_cmyk_safe(spectrum1), spectrum_cmyk_safe(spectrum2), spectrum_cmyk_safe(spectrum3)};
    for (int i = 1; i < 3; i++)   // by luminance, darkest first; equal ones keep their order
        for (int j = i; j > 0 && luminance(c[j]) < luminance(c[j - 1]); j--) {
            Color t = c[j];
            c[j] = c[j - 1];
            c[j - 1] = t;
        }
    return {{lighten(c[2], lighter_advance), c[1], darken(c[0], darker_advance)}, 3, is_reversed};
}

Gradient analogous(Bits &e) {
    double spectrum1 = e.next_frac();
    double spectrum2 = fmod(spectrum1 + 1.0 / 12, 1);
    double spectrum3 = fmod(spectrum1 + 2.0 / 12, 1);
    double spectrum4 = fmod(spectrum1 + 3.0 / 12, 1);
    double advance = e.next_frac() * 0.5 + 0.2;
    bool is_reversed = e.next();
    Color c1 = spectrum_cmyk_safe(spectrum1), c2 = spectrum_cmyk_safe(spectrum2);
    Color c3 = spectrum_cmyk_safe(spectrum3), c4 = spectrum_cmyk_safe(spectrum4);
    bool up = luminance(c1) < luminance(c4);
    const Color &darkest = up ? c1 : c4, &dark = up ? c2 : c3, &light = up ? c3 : c2, &lightest = up ? c4 : c1;
    return {{darken(darkest, advance), darken(dark, advance / 2), lighten(light, advance / 2), lighten(lightest, advance)},
            4, is_reversed};
}

bool alive(const uint8_t *grid, int x, int y) {
    x = (x + N) % N;
    y = (y + N) % N;
    int i = y * N + x;
    return (grid[i >> 3] >> (7 - (i & 7))) & 1;
}

// Conway's Game of Life on a 16x16 board whose edges wrap around.
void next_generation(const uint8_t *cur, uint8_t *next) {
    memset(next, 0, GRID_BYTES);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int n = 0;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                    if ((ox || oy) && alive(cur, x + ox, y + oy)) n++;
            if (alive(cur, x, y) ? (n == 2 || n == 3) : n == 3) {
                int i = y * N + x;
                next[i >> 3] |= 0x80 >> (i & 7);
            }
        }
}

uint8_t to_byte(double v) {
    double c = clamped(v) * 255;
    return isnan(c) ? 0 : uint8_t(int(c));   // as Java casts: NaN is 0
}

}  // namespace

bool lifehash_v2(const uint8_t *data, size_t len, uint8_t rgb[LIFEHASH_RGB]) {
    uint8_t digest[32];
    static const uint8_t none = 0;
    if (wally_sha256(len ? data : &none, len, digest, sizeof(digest)) != WALLY_OK) return false;

    // Every generation so far, to stop at the first one seen before, and the
    // grey level of each cell: how late it was last alive.
    uint8_t *history = (uint8_t *)malloc(MAX_GENERATIONS * GRID_BYTES);
    double *frac = (double *)malloc(N * N * sizeof(double));
    if (!history || !frac) {
        free(history);
        free(frac);
        return false;
    }
    uint8_t cur[GRID_BYTES], next[GRID_BYTES];
    wally_sha256(digest, sizeof(digest), next, sizeof(next));   // version 2: not the digest itself
    int count = 0;
    while (count < MAX_GENERATIONS) {
        memcpy(cur, next, GRID_BYTES);
        bool seen = false;
        for (int i = 0; i < count && !seen; i++) seen = !memcmp(history + i * GRID_BYTES, cur, GRID_BYTES);
        if (seen) break;
        memcpy(history + count++ * GRID_BYTES, cur, GRID_BYTES);
        next_generation(cur, next);
    }
    for (int p = 0; p < N * N; p++) frac[p] = 0;
    for (int i = 0; i < count; i++) {
        double f = clamped(lerp_from(0, count, i + 1));
        for (int p = 0; p < N * N; p++)
            if (alive(history + i * GRID_BYTES, p % N, p / N)) frac[p] = f;
    }
    free(history);
    double lo = INFINITY, hi = -INFINITY;
    for (int p = 0; p < N * N; p++) {
        lo = jmin(lo, frac[p]);
        hi = jmax(hi, frac[p]);
    }
    for (int p = 0; p < N * N; p++) frac[p] = lerp_from(lo, hi, frac[p]);

    Bits e = {digest, 0};
    e.next_uint(2);                    // version 2 skips two bits
    Gradient g;
    switch (e.next_uint(2)) {
        case 0: g = monochromatic(e); break;
        case 1: g = complementary(e); break;
        case 2: g = triadic(e); break;
        default: g = analogous(e); break;
    }
    bool snowflake = e.next();         // mirrored; otherwise a pinwheel, turned

    const int S = LIFEHASH_SIDE, M = S - 1;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            Color c = at(g, frac[y * N + x]);
            uint8_t px[3] = {to_byte(c.r), to_byte(c.g), to_byte(c.b)};
            // The four copies: as it is, then each quarter's reflection or turn.
            int at4[4][2] = {{x, y},
                             {snowflake ? M - x : M - y, snowflake ? y : x},
                             {snowflake ? x : y, snowflake ? M - y : M - x},
                             {M - x, M - y}};
            for (auto &q : at4) memcpy(rgb + (q[1] * S + q[0]) * 3, px, 3);
        }
    free(frac);
    return true;
}
