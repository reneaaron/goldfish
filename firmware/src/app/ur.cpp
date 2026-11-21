#include "ur.h"
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
#include <wally_crypto.h>

// ------------------------------------------------------------ bytewords
static const char *WORDLIST[256] = {
    "able","acid","also","apex","aqua","arch","atom","aunt","away","axis","back","bald","barn","belt","beta","bias",
    "blue","body","brag","brew","bulb","buzz","calm","cash","cats","chef","city","claw","code","cola","cook","cost",
    "crux","curl","cusp","cyan","dark","data","days","deli","dice","diet","door","down","draw","drop","drum","dull",
    "duty","each","easy","echo","edge","epic","even","exam","exit","eyes","fact","fair","fern","figs","film","fish",
    "fizz","flap","flew","flux","foxy","free","frog","fuel","fund","gala","game","gear","gems","gift","girl","glow",
    "good","gray","grim","guru","gush","gyro","half","hang","hard","hawk","heat","help","high","hill","holy","hope",
    "horn","huts","iced","idea","idle","inch","inky","into","iris","iron","item","jade","jazz","join","jolt","jowl",
    "judo","jugs","jump","junk","jury","keep","keno","kept","keys","kick","kiln","king","kite","kiwi","knob","lamb",
    "lava","lazy","leaf","legs","liar","limp","lion","list","logo","loud","love","luau","luck","lung","main","many",
    "math","maze","memo","menu","meow","mild","mint","miss","monk","nail","navy","need","news","next","noon","note",
    "numb","obey","oboe","omit","onyx","open","oval","owls","paid","part","peck","play","plus","poem","pool","pose",
    "puff","puma","purr","quad","quiz","race","ramp","real","redo","rich","road","rock","roof","ruby","ruin","runs",
    "rust","safe","saga","scar","sets","silk","skew","slot","soap","solo","song","stub","surf","swan","taco","task",
    "taxi","tent","tied","time","tiny","toil","tomb","toys","trip","tuna","twin","ugly","undo","unit","urge","user",
    "vast","very","veto","vial","vibe","view","visa","void","vows","wall","wand","warm","wasp","wave","waxy","webs",
    "what","when","whiz","wolf","work","yank","yawn","yell","yoga","yurt","zaps","zero","zest","zinc","zone","zoom",
};

static int16_t minimal_lut[26 * 26];   // (first, last) letter -> byte
static bool lut_ready;

static void build_minimal() {
    for (int i = 0; i < 26 * 26; i++) minimal_lut[i] = -1;
    for (int b = 0; b < 256; b++) {
        const char *w = WORDLIST[b];
        minimal_lut[(w[0] - 'a') * 26 + (w[3] - 'a')] = int16_t(b);
    }
    lut_ready = true;
}

static uint32_t crc32(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// Minimal bytewords -> bytes, checks and strips the trailing CRC32.
static bool bytewords_decode(const char *s, size_t n, uint8_t *out, size_t cap, size_t *outlen) {
    if (!lut_ready) build_minimal();
    if (n % 2 || n / 2 < 5 || n / 2 > cap) return false;
    for (size_t i = 0; i < n / 2; i++) {
        int a = tolower((unsigned char)s[2 * i]) - 'a', b = tolower((unsigned char)s[2 * i + 1]) - 'a';
        if (a < 0 || a > 25 || b < 0 || b > 25) return false;
        int v = minimal_lut[a * 26 + b];
        if (v < 0) return false;
        out[i] = uint8_t(v);
    }
    size_t body = n / 2 - 4;
    uint32_t want = (uint32_t(out[body]) << 24) | (uint32_t(out[body + 1]) << 16) | (uint32_t(out[body + 2]) << 8) | out[body + 3];
    if (crc32(out, body) != want) return false;
    *outlen = body;
    return true;
}

static size_t bytewords_encode(const uint8_t *d, size_t n, char *out, size_t cap) {
    uint8_t crc[4];
    uint32_t c = crc32(d, n);
    crc[0] = c >> 24; crc[1] = c >> 16; crc[2] = c >> 8; crc[3] = c;
    if (2 * (n + 4) + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n + 4; i++) {
        const char *w = WORDLIST[i < n ? d[i] : crc[i - n]];
        out[o++] = char(toupper(w[0]));
        out[o++] = char(toupper(w[3]));
    }
    out[o] = 0;
    return o;
}

// ------------------------------------------------------------ minimal CBOR
bool cbor_head(Cbor *c, uint8_t *major, uint64_t *val) {
    if (c->p >= c->end) return false;
    uint8_t b = *c->p++;
    *major = b >> 5;
    uint8_t ai = b & 31;
    if (ai < 24) { *val = ai; return true; }
    int n = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : 0;
    if (!n || c->end - c->p < n) return false;
    uint64_t v = 0;
    while (n--) v = (v << 8) | *c->p++;
    *val = v;
    return true;
}

static uint8_t *cbor_put(uint8_t *p, uint8_t major, uint64_t v) {
    if (v < 24) { *p++ = uint8_t(major << 5 | v); return p; }
    int n = v < 0x100 ? 1 : v < 0x10000 ? 2 : v < 0x100000000ull ? 4 : 8;
    *p++ = uint8_t(major << 5 | (n == 1 ? 24 : n == 2 ? 25 : n == 4 ? 26 : 27));
    for (int i = n - 1; i >= 0; i--) *p++ = uint8_t(v >> (8 * i));
    return p;
}

// ------------------------------------------------------------ fountain codes
// Xoshiro256** seeded from sha256(seed), as in the bc-ur reference.
struct Xoshiro { uint64_t s[4]; };
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static void xo_seed(Xoshiro *x, const uint8_t *seed, size_t n) {
    uint8_t h[32];
    wally_sha256(seed, n, h, 32);
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v = (v << 8) | h[8 * i + k];
        x->s[i] = v;
    }
}

static uint64_t xo_next(Xoshiro *x) {
    uint64_t *s = x->s;
    uint64_t r = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
}

static double xo_double(Xoshiro *x) { return double(xo_next(x)) / (double(UINT64_MAX) + 1.0); }
static uint64_t xo_int(Xoshiro *x, uint64_t lo, uint64_t hi) {
    return uint64_t(xo_double(x) * double(hi - lo + 1)) + lo;
}

// Which fragments a part with sequence number `seq` mixes together.
static size_t choose_fragments(uint32_t seq, uint32_t seq_len, uint32_t checksum, uint8_t *out) {
    if (seq <= seq_len) { out[0] = uint8_t(seq - 1); return 1; }
    uint8_t seed[8] = {uint8_t(seq >> 24), uint8_t(seq >> 16), uint8_t(seq >> 8), uint8_t(seq),
                       uint8_t(checksum >> 24), uint8_t(checksum >> 16), uint8_t(checksum >> 8), uint8_t(checksum)};
    Xoshiro x;
    xo_seed(&x, seed, sizeof(seed));

    // degree: weighted 1/i, sampled with Vose's alias method
    static double P[UR_MAX_FRAGMENTS], prob[UR_MAX_FRAGMENTS];
    static int alias[UR_MAX_FRAGMENTS], S[UR_MAX_FRAGMENTS], L[UR_MAX_FRAGMENTS];
    int n = int(seq_len);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += 1.0 / (i + 1);
    for (int i = 0; i < n; i++) P[i] = (1.0 / (i + 1)) * n / sum;
    int ns = 0, nl = 0;
    for (int i = n - 1; i >= 0; i--) { if (P[i] < 1.0) S[ns++] = i; else L[nl++] = i; }
    while (ns && nl) {
        int a = S[--ns], g = L[--nl];
        prob[a] = P[a];
        alias[a] = g;
        P[g] += P[a] - 1.0;
        if (P[g] < 1.0) S[ns++] = g; else L[nl++] = g;
    }
    while (nl) prob[L[--nl]] = 1.0;
    while (ns) prob[S[--ns]] = 1.0;
    double r1 = xo_double(&x), r2 = xo_double(&x);
    int i = int(n * r1);
    int degree = (r2 < prob[i] ? i : alias[i]) + 1;

    // shuffle indexes, take the first `degree`
    uint8_t remaining[UR_MAX_FRAGMENTS];
    int rem = n;
    for (int k = 0; k < n; k++) remaining[k] = uint8_t(k);
    for (int k = 0; k < degree; k++) {
        int idx = int(xo_int(&x, 0, uint64_t(rem - 1)));
        out[k] = remaining[idx];
        memmove(&remaining[idx], &remaining[idx + 1], size_t(rem - idx - 1));
        rem--;
    }
    return size_t(degree);
}

// ------------------------------------------------------------ decoder
void ur_decoder_reset(UrDecoder *d) {
    volatile uint8_t *v = (volatile uint8_t *)d;
    for (size_t i = 0; i < sizeof(*d); i++) v[i] = 0;
}

int ur_decoder_percent(const UrDecoder *d) {
    if (!d->seq_len) return 0;
    return int(d->found * 100 / d->seq_len);
}

static void xor_into(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] ^= src[i];
}

static void add_pure(UrDecoder *d, int idx, const uint8_t *data);

// Remove known fragments from stored mixed parts; any that drop to one
// fragment become pure.
static void reduce_mixed(UrDecoder *d) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t m = 0; m < d->mixed_n; m++) {
            UrDecoder::Mixed &mx = d->mixed[m];
            uint8_t keep = 0;
            for (uint8_t k = 0; k < mx.n; k++) {
                uint8_t idx = mx.idx[k];
                if (d->received[idx]) xor_into(d->mixed_data[mx.slot], &d->message[idx * d->fragment_len], d->fragment_len);
                else mx.idx[keep++] = idx;
            }
            mx.n = keep;
            if (mx.n == 1) {
                add_pure(d, mx.idx[0], d->mixed_data[mx.slot]);
                mx.n = 0;
                changed = true;
            }
        }
        // compact out empty entries
        size_t w = 0;
        for (size_t m = 0; m < d->mixed_n; m++)
            if (d->mixed[m].n > 0) d->mixed[w++] = d->mixed[m];
        d->mixed_n = w;
    }
}

static void add_pure(UrDecoder *d, int idx, const uint8_t *data) {
    if (d->received[idx]) return;
    memcpy(&d->message[size_t(idx) * d->fragment_len], data, d->fragment_len);
    d->received[idx] = 1;
    d->found++;
}

// Store a mixed part, minus the fragments we already know: one that comes
// down to a single fragment is new data even when the store is full (dropping
// those stalled the scan for good once the store filled up). A camera reads
// the same frame many times, so a part already stored is skipped.
static void add_mixed(UrDecoder *d, uint32_t seq, uint8_t *idx, size_t deg, const uint8_t *data) {
    static uint8_t part[256];
    memcpy(part, data, d->fragment_len);
    size_t n = 0;
    for (size_t k = 0; k < deg; k++) {
        if (d->received[idx[k]]) xor_into(part, &d->message[idx[k] * d->fragment_len], d->fragment_len);
        else idx[n++] = idx[k];
    }
    if (n == 1) add_pure(d, idx[0], part);
    if (n < 2 || d->mixed_n == 64) return;
    for (size_t m = 0; m < d->mixed_n; m++)
        if (d->mixed[m].seq == seq) return;
    bool used[64] = {false};
    for (size_t m = 0; m < d->mixed_n; m++) used[d->mixed[m].slot] = true;
    uint16_t slot = 0;
    while (used[slot]) slot++;
    UrDecoder::Mixed &mx = d->mixed[d->mixed_n++];
    mx.slot = slot;
    mx.seq = seq;
    mx.n = uint8_t(n);
    memcpy(mx.idx, idx, n);
    memcpy(d->mixed_data[slot], part, d->fragment_len);
}

// The assembled message: a CBOR byte string for crypto-psbt and bytes (the
// payload is what's inside), any CBOR for crypto-output (the payload is all of it).
static UrResult unwrap(UrDecoder *d, const uint8_t **payload, size_t *plen) {
    Cbor c = {d->message, d->message + d->message_len};
    if (d->type != UR_TYPE_OUTPUT) {
        uint8_t major; uint64_t v;
        if (!cbor_head(&c, &major, &v) || major != 2 || v > size_t(c.end - c.p)) return UR_BAD;
        c.end = c.p + v;
    }
    *payload = c.p;
    *plen = size_t(c.end - c.p);
    return UR_COMPLETE;
}

static UrResult finish(UrDecoder *d, const uint8_t **payload, size_t *plen) {
    if (crc32(d->message, d->message_len) != d->checksum) return UR_BAD;
    return unwrap(d, payload, plen);
}

UrResult ur_decoder_add(UrDecoder *d, const char *text, size_t len, const uint8_t **payload, size_t *plen) {
    if (len < 4 || strncasecmp(text, "ur:", 3) != 0) return UR_NOT_UR;
    const char *p = text + 3, *end = text + len;
    const char *slash = (const char *)memchr(p, '/', size_t(end - p));
    if (!slash) return UR_BAD;
    static const char *const types[] = {"crypto-psbt", "crypto-output", "bytes"};
    int type = -1;
    for (int t = 0; t < 3; t++)
        if (size_t(slash - p) == strlen(types[t]) && strncasecmp(p, types[t], size_t(slash - p)) == 0) type = t;
    if (type < 0) return UR_WRONG_TYPE;
    p = slash + 1;

    static uint8_t buf[UR_MAX_MESSAGE + 64];
    size_t n;
    const char *slash2 = (const char *)memchr(p, '/', size_t(end - p));
    if (!slash2) {
        // single-part UR: the whole message
        if (!bytewords_decode(p, size_t(end - p), buf, sizeof(buf), &n)) return UR_BAD;
        ur_decoder_reset(d);
        memcpy(d->message, buf, n);
        d->message_len = uint32_t(n);
        d->checksum = 0;
        d->type = UrType(type);
        return unwrap(d, payload, plen);
    }

    // multi-part: "seq-len/bytewords"
    if (!bytewords_decode(slash2 + 1, size_t(end - slash2 - 1), buf, sizeof(buf), &n)) return UR_BAD;
    Cbor c = {buf, buf + n};
    uint8_t major; uint64_t arr, seq, seq_len, msg_len, checksum, frag_len;
    if (!cbor_head(&c, &major, &arr) || major != 4 || arr != 5) return UR_BAD;
    if (!cbor_head(&c, &major, &seq) || major != 0) return UR_BAD;
    if (!cbor_head(&c, &major, &seq_len) || major != 0) return UR_BAD;
    if (!cbor_head(&c, &major, &msg_len) || major != 0) return UR_BAD;
    if (!cbor_head(&c, &major, &checksum) || major != 0) return UR_BAD;
    if (!cbor_head(&c, &major, &frag_len) || major != 2 || frag_len != size_t(c.end - c.p)) return UR_BAD;
    // Fragments are stored back to back in d->message, so the fragment count
    // must be exactly what the message needs (as bc-ur requires) and fit the
    // buffer. seq is 1-based: 0 would index fragment 255.
    if (seq == 0 || seq > UINT32_MAX || seq_len == 0 || msg_len == 0 || frag_len == 0 || frag_len > 256 ||
        seq_len != (msg_len + frag_len - 1) / frag_len)
        return UR_BAD;
    // A good part of a message we have no room for: say so, instead of nothing.
    if (msg_len > UR_MAX_MESSAGE || seq_len > UR_MAX_FRAGMENTS) return UR_TOO_BIG;
    if (seq_len * frag_len > sizeof(d->message)) return UR_BAD;

    if (d->type != type || d->seq_len != seq_len || d->checksum != checksum || d->message_len != msg_len ||
        d->fragment_len != frag_len) {
        ur_decoder_reset(d);   // a different message: start over
        d->type = UrType(type);
        d->seq_len = uint32_t(seq_len);
        d->checksum = uint32_t(checksum);
        d->message_len = uint32_t(msg_len);
        d->fragment_len = size_t(frag_len);
    }

    uint8_t idx[UR_MAX_FRAGMENTS];
    size_t deg = choose_fragments(uint32_t(seq), uint32_t(seq_len), uint32_t(checksum), idx);
    if (deg == 1) add_pure(d, idx[0], c.p);
    else add_mixed(d, uint32_t(seq), idx, deg, c.p);
    reduce_mixed(d);

    if (d->found == d->seq_len) return finish(d, payload, plen);
    return UR_PROGRESS;
}

// ------------------------------------------------------------ encoder
size_t ur_part_count(size_t len, size_t fragment_len) {
    uint8_t head[9];
    size_t msg = size_t(cbor_put(head, 2, len) - head) + len;
    return (msg + fragment_len - 1) / fragment_len;
}

size_t ur_encode_part(const uint8_t *data, size_t len, size_t fragment_len, uint32_t seq, char *out, size_t cap) {
    if (fragment_len == 0 || fragment_len > 256) return 0;
    static uint8_t msg[UR_MAX_MESSAGE + 16];
    uint8_t *m = cbor_put(msg, 2, len);
    if (size_t(m - msg) + len > sizeof(msg)) return 0;
    memcpy(m, data, len);
    size_t msg_len = size_t(m - msg) + len;
    size_t count = (msg_len + fragment_len - 1) / fragment_len;
    uint32_t checksum = crc32(msg, msg_len);

    if (count == 1) {
        int h = snprintf(out, cap, "UR:CRYPTO-PSBT/");
        size_t w = bytewords_encode(msg, msg_len, out + h, cap - size_t(h));
        return w ? size_t(h) + w : 0;
    }

    if (count > UR_MAX_FRAGMENTS || seq == 0) return 0;
    // Parts 1..count are the fragments in turn; after that each part XORs a
    // pseudo-random set of them (fountain code), so a scanner that missed a
    // frame doesn't wait for it to come round again.
    uint8_t idx[UR_MAX_FRAGMENTS];
    size_t deg = choose_fragments(seq, uint32_t(count), checksum, idx);
    uint8_t part[300];
    uint8_t *p = cbor_put(part, 4, 5);
    p = cbor_put(p, 0, seq);
    p = cbor_put(p, 0, count);
    p = cbor_put(p, 0, msg_len);
    p = cbor_put(p, 0, checksum);
    p = cbor_put(p, 2, fragment_len);
    memset(p, 0, fragment_len);
    for (size_t k = 0; k < deg; k++) {
        size_t start = idx[k] * fragment_len;
        size_t take = msg_len - start < fragment_len ? msg_len - start : fragment_len;
        for (size_t i = 0; i < take; i++) p[i] ^= msg[start + i];
    }
    p += fragment_len;

    int h = snprintf(out, cap, "UR:CRYPTO-PSBT/%u-%u/", unsigned(seq), unsigned(count));
    if (h <= 0 || size_t(h) >= cap) return 0;
    size_t w = bytewords_encode(part, size_t(p - part), out + h, cap - size_t(h));
    return w ? size_t(h) + w : 0;
}
