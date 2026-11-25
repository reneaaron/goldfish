#include "entropy_check.h"
#include <math.h>

static const double ALPHA = 0.01;

// P(X = k), X ~ Binomial(n, p).
static double binom_pmf(int n, int k, double p) {
    return exp(lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0) + k * log(p) + (n - k) * log1p(-p));
}
static double binom_ge(int n, int k, double p) {   // P(X >= k)
    double s = 0;
    for (int j = k < 0 ? 0 : k; j <= n; j++) s += binom_pmf(n, j, p);
    return s > 1 ? 1 : s;
}
static double binom_le(int n, int k, double p) {   // P(X <= k)
    double s = 0;
    for (int j = 0; j <= k && j <= n; j++) s += binom_pmf(n, j, p);
    return s > 1 ? 1 : s;
}
static double binom_two_sided(int n, int k, double p) {
    double lo = binom_le(n, k, p), hi = binom_ge(n, k, p), m = 2 * (lo < hi ? lo : hi);
    return m > 1 ? 1 : m;
}

// Regularized lower incomplete gamma P(a, x) (Numerical Recipes: series below
// a+1, continued fraction above).
static double gamma_p(double a, double x) {
    if (x <= 0) return 0;
    double front = exp(-x + a * log(x) - lgamma(a));
    if (x < a + 1) {
        double ap = a, del = 1 / a, sum = del;
        for (int i = 0; i < 500 && fabs(del) > fabs(sum) * 1e-15; i++) {
            ap += 1;
            del *= x / ap;
            sum += del;
        }
        return sum * front;
    }
    const double tiny = 1e-300;
    double b = x + 1 - a, c = 1 / tiny, d = 1 / b, h = d;
    for (int i = 1; i < 500; i++) {
        double an = -i * (i - a);
        b += 2;
        d = an * d + b;
        if (fabs(d) < tiny) d = tiny;
        c = b + an / c;
        if (fabs(c) < tiny) c = tiny;
        d = 1 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1) < 1e-15) break;
    }
    return 1 - front * h;
}

// Chi-square statistic of k counts over n trials, all equally likely.
static double chi2(const int *count, int k, int n) {
    double e = double(n) / k, s = 0;
    for (int i = 0; i < k; i++) s += (count[i] - e) * (count[i] - e) / e;
    return s;
}

// P(longest run >= len) in n trials where each trial repeats the previous
// one with probability q. Exact: track the current run's length while every
// run so far is shorter than len.
static double longest_run_ge(int n, int len, double q) {
    if (len <= 1 || n <= 0) return 1;
    const int MAXR = 40;
    if (len > MAXR) return 0;   // q^39 * 256 < 1e-9 even for coins
    double f[MAXR] = {0};       // f[r]: the current run is r long
    f[1] = 1;
    for (int i = 1; i < n; i++) {
        double total = 0;
        for (int r = 1; r < len; r++) total += f[r];
        for (int r = len - 1; r > 1; r--) f[r] = f[r - 1] * q;
        f[1] = total * (1 - q);
    }
    double below = 0;
    for (int r = 1; r < len; r++) below += f[r];
    return below > 1 ? 0 : 1 - below;
}

int entropy_check(const char *s, int n, bool dice) {
    const int faces = dice ? 6 : 2;
    const double q = 1.0 / faces;   // chance a given roll matches a given earlier one
    if (n < 2) return 0;
    int v[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        v[i] = dice ? s[i] - '1' : s[i] - '0';
        if (v[i] < 0 || v[i] >= faces) return 0;   // not ours to judge
    }
    const int tests = dice ? 6 : 4;
    double p[6];
    int flag[6];
    int k = 0;

    if (dice) {   // step pattern
        int step[6] = {0};
        for (int i = 1; i < n; i++) step[(v[i] - v[i - 1] + 6) % 6]++;
        flag[k] = ENT_STEPS;
        p[k++] = 1 - gamma_p(2.5, chi2(step, 6, n - 1) / 2);
    }

    // Repeating sequence: the same value k later, for some lag k.
    double best = 1;
    for (int lag = 2; lag <= 12 && lag < n; lag++) {
        int m = 0;
        for (int i = lag; i < n; i++) m += v[i] == v[i - lag];
        double pl = binom_ge(n - lag, m, q);
        if (pl < best) best = pl;
    }
    flag[k] = ENT_REPEATS;
    p[k++] = best * 11 > 1 ? 1 : best * 11;

    // Same twice in a row (dice) / switches (coins): two-sided, the same count.
    int same = 0;
    for (int i = 1; i < n; i++) same += v[i] == v[i - 1];
    flag[k] = ENT_PAIRS;
    p[k++] = binom_two_sided(n - 1, same, q);

    int longest = 1, run = 1;
    for (int i = 1; i < n; i++) {
        run = v[i] == v[i - 1] ? run + 1 : 1;
        if (run > longest) longest = run;
    }
    flag[k] = ENT_STREAK;
    p[k++] = longest_run_ge(n, longest, q);

    int count[6] = {0};
    for (int i = 0; i < n; i++) count[v[i]]++;
    if (dice) {
        double x = chi2(count, 6, n), lower = gamma_p(2.5, x / 2);
        flag[k] = ENT_UNEVEN;
        p[k++] = 1 - lower;
        flag[k] = ENT_TOO_EVEN;
        p[k++] = lower;
    } else {
        flag[k] = ENT_UNEVEN;
        p[k++] = binom_two_sided(n, count[1], 0.5);
    }

    int out = 0;
    for (int i = 0; i < k; i++)
        if (p[i] * tests < ALPHA) out |= flag[i];
    return out;
}
