#include "amount.h"
#include <stdio.h>
#include <string.h>

// "24,857"
static size_t group(unsigned long long v, char *out, size_t cap) {
    char raw[24];
    snprintf(raw, sizeof(raw), "%llu", v);
    size_t n = strlen(raw), o = 0;
    for (size_t i = 0; i < n && o + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
    return o;
}

void fmt_btc(uint64_t v, char *out, size_t cap) {
    size_t o = snprintf(out, cap, "\xE2\x82\xBF");   // U+20BF
    group(v, out + o, cap - o);
}

void fmt_btc_legacy(uint64_t v, char *out, size_t cap) {
    size_t o = group(v / 100000000ull, out, cap);
    char f[9];
    snprintf(f, sizeof(f), "%08llu", (unsigned long long)(v % 100000000ull));
    int n = 8;
    while (n && f[n - 1] == '0') n--;
    if (n) snprintf(out + o, cap - o, ".%.*s BTC", n, f);
    else snprintf(out + o, cap - o, " BTC");
}
