#include "multisig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wally_core.h>
#include <wally_address.h>
#include <wally_bip32.h>
#include <wally_crypto.h>
#include <wally_descriptor.h>
#include "ur.h"
#include "wallet.h"

#define MAX_PATH_LEN 8                 // our key's origin path, e.g. 48h/0h/0h/2h
#define MAX_TEXT 2800                  // 15 keys of about 170 characters each

static struct wally_descriptor *desc;
static MsInfo info;
static uint8_t seed_fp[4];             // the seed it was loaded with
static uint32_t our_path[MAX_PATH_LEN];
static size_t our_path_len;

void multisig_clear() {
    if (desc) wally_descriptor_free(desc);
    desc = nullptr;
    memset(&info, 0, sizeof(info));
    our_path_len = 0;
}

bool multisig_loaded() {
    if (!desc || !wallet_has_seed()) return false;
    uint8_t fp[4];
    wallet_fingerprint(fp);
    return memcmp(fp, seed_fp, 4) == 0;
}

const MsInfo *multisig_info() { return multisig_loaded() ? &info : nullptr; }

// ------------------------------------------------------------ descriptor text
static bool starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

// Copy `in` to `out`, writing /<0;1>/* for /0/* and /{0,1}/* and dropping the
// checksum, which no longer matches. Returns the number of replacements, -1 if
// it doesn't fit.
static int normalize(const char *in, char *out, size_t cap) {
    static const char *const from[] = {"/0/*", "/{0,1}/*"};
    static const char to[] = "/<0;1>/*";
    int n = 0;
    size_t o = 0;
    while (*in && *in != '#') {
        size_t skip = 0;
        for (const char *f : from)
            if (starts_with(in, f)) skip = strlen(f);
        if (o + sizeof(to) >= cap) return -1;
        if (skip) {
            memcpy(out + o, to, sizeof(to) - 1);
            o += sizeof(to) - 1;
            in += skip;
            n++;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = 0;
    return n;
}

// Is key `i` of `d` the loaded seed's key at the origin path it names?
static bool key_is_ours(const struct wally_descriptor *d, size_t i, uint32_t *path, size_t *path_len) {
    char *origin = nullptr, *xpub = nullptr;
    struct ext_key theirs, mine;
    bool ok = wally_descriptor_get_key_origin_path_str(d, i, &origin) == WALLY_OK &&
              bip32_path_from_str(origin, 0, 0, 0, path, MAX_PATH_LEN, path_len) == WALLY_OK &&
              *path_len <= MAX_PATH_LEN &&
              wally_descriptor_get_key(d, i, &xpub) == WALLY_OK &&
              bip32_key_from_base58(xpub, &theirs) == WALLY_OK &&
              bip32_key_from_parent_path(wallet_root(), path, *path_len,
                                         BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &mine) == WALLY_OK &&
              memcmp(mine.pub_key, theirs.pub_key, sizeof(mine.pub_key)) == 0 &&
              memcmp(mine.chain_code, theirs.chain_code, sizeof(mine.chain_code)) == 0;
    if (origin) wally_free_string(origin);
    if (xpub) wally_free_string(xpub);
    return ok;
}

// Check a parsed descriptor: wsh(sortedmulti(M, N distinct xpubs with origins,
// each /<0;1>/*)), one of them ours. Fills `out` and our path.
static MsResult inspect(const struct wally_descriptor *d, MsInfo *out, uint32_t *path, size_t *path_len) {
    uint32_t features = 0, paths = 0, variants = 0, keys = 0;
    if (wally_descriptor_get_features(d, &features) != WALLY_OK ||
        wally_descriptor_get_num_paths(d, &paths) != WALLY_OK ||
        wally_descriptor_get_num_variants(d, &variants) != WALLY_OK ||
        wally_descriptor_get_num_keys(d, &keys) != WALLY_OK)
        return MS_BAD;
    const uint32_t need = WALLY_MS_IS_RANGED | WALLY_MS_IS_MULTIPATH | WALLY_MS_IS_DESCRIPTOR;
    const uint32_t refuse = WALLY_MS_IS_PRIVATE | WALLY_MS_IS_RAW | WALLY_MS_IS_UNCOMPRESSED | WALLY_MS_IS_X_ONLY |
                            WALLY_MS_IS_TAPROOT | WALLY_MS_IS_ELEMENTS;
    if ((features & need) != need || (features & refuse) || paths != 2 || variants != 1 ||
        keys < 1 || keys > MS_MAX_KEYS)
        return MS_UNSUPPORTED;

    char *canon = nullptr;
    if (wally_descriptor_canonicalize(d, WALLY_MS_CANONICAL_NO_CHECKSUM, &canon) != WALLY_OK) return MS_BAD;
    static const char prefix[] = "wsh(sortedmulti(";
    long m = starts_with(canon, prefix) ? strtol(canon + sizeof(prefix) - 1, nullptr, 10) : 0;
    wally_free_string(canon);
    if (m < 1 || m > long(keys)) return MS_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    out->m = int(m);
    out->n = int(keys);
    out->ours = -1;
    uint8_t fp[4];
    wallet_fingerprint(fp);
    char *seen[MS_MAX_KEYS] = {nullptr};
    MsResult r = MS_OK;
    for (size_t i = 0; i < keys && r == MS_OK; i++) {
        char *child = nullptr;
        if (wally_descriptor_get_key_origin_fingerprint(d, i, out->fingerprint[i], 4) != WALLY_OK ||
            wally_descriptor_get_key_child_path_str(d, i, &child) != WALLY_OK ||
            strcmp(child[0] == '/' ? child + 1 : child, "<0;1>/*") != 0 ||
            wally_descriptor_get_key(d, i, &seen[i]) != WALLY_OK)
            r = MS_UNSUPPORTED;
        // The network comes from the keys (xpub or tpub), all of them alike.
        struct ext_key k;
        if (r == MS_OK) {
            bool test = bip32_key_from_base58(seen[i], &k) == WALLY_OK && k.version == BIP32_VER_TEST_PUBLIC;
            if (i && test != out->testnet) r = MS_BAD;
            out->testnet = test;
        }
        if (child) wally_free_string(child);
        for (size_t k = 0; k < i && r == MS_OK; k++)
            if (strcmp(seen[k], seen[i]) == 0) r = MS_BAD;   // the same key twice
        if (r == MS_OK && out->ours < 0 && memcmp(out->fingerprint[i], fp, 4) == 0 && key_is_ours(d, i, path, path_len))
            out->ours = int(i);
    }
    for (char *s : seen)
        if (s) wally_free_string(s);
    if (r == MS_OK && out->ours < 0) r = MS_NOT_OURS;
    return r;
}

MsResult multisig_load(const char *text, size_t len) {
    static char a[MAX_TEXT], b[MAX_TEXT];
    while (len && (text[0] == ' ' || text[0] == '\n' || text[0] == '\r' || text[0] == '\t')) { text++; len--; }
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == '\t')) len--;
    if (len >= sizeof(a)) return MS_NOT_DESCRIPTOR;
    memcpy(a, text, len);
    a[len] = 0;
    if (strlen(a) != len) return MS_NOT_DESCRIPTOR;
    if (!starts_with(a, "wsh(")) {
        static const char *const others[] = {"sh(", "wpkh(", "pkh(", "tr(", "pk(", "combo(", "multi(", "sortedmulti("};
        for (const char *other : others)
            if (starts_with(a, other)) return MS_UNSUPPORTED;
        return MS_NOT_DESCRIPTOR;
    }
    if (!wallet_has_seed()) return MS_NOT_OURS;

    // As written first, so a checksum is checked against the text it was made
    // for. /{0,1}/* is not descriptor syntax libwally knows, and /0/* names
    // only the receive chain: both are then read as /<0;1>/*.
    struct wally_descriptor *d = nullptr;
    uint32_t paths = 0;
    bool as_written = wally_descriptor_parse(a, nullptr, WALLY_NETWORK_NONE, 0, &d) == WALLY_OK;
    if (!as_written && !strstr(a, "{0,1}")) return MS_BAD;
    if (!as_written || wally_descriptor_get_num_paths(d, &paths) != WALLY_OK || paths != 2) {
        if (d) wally_descriptor_free(d);
        d = nullptr;
        int replaced = normalize(a, b, sizeof(b));
        if (replaced < 0) return MS_BAD;
        if (replaced == 0) return MS_UNSUPPORTED;
        if (wally_descriptor_parse(b, nullptr, WALLY_NETWORK_NONE, 0, &d) != WALLY_OK) return MS_BAD;
    }

    MsInfo found;
    uint32_t path[MAX_PATH_LEN];
    size_t path_len = 0;
    MsResult r = inspect(d, &found, path, &path_len);
    if (r == MS_OK && wally_descriptor_set_network(d, found.testnet ? WALLY_NETWORK_BITCOIN_TESTNET
                                                                    : WALLY_NETWORK_BITCOIN_MAINNET) != WALLY_OK)
        r = MS_BAD;
    if (r != MS_OK) {
        wally_descriptor_free(d);
        return r;
    }
    multisig_clear();
    desc = d;
    info = found;
    memcpy(our_path, path, sizeof(our_path));
    our_path_len = path_len;
    wallet_fingerprint(seed_fp);
    return MS_OK;
}

// ------------------------------------------------------------ crypto-output
// tag 401 wsh ( tag 407 sortedmulti { 1: M, 2: [ tag 303 crypto-hdkey, ... ] } ),
// each key { 3: key data, 4: chain code, 5: use info, 6: origin, 7: children,
// 8: parent fingerprint }. Rewritten as descriptor text for multisig_load().

static bool cbor_skip(Cbor *c, int depth = 0) {
    uint8_t major; uint64_t v;
    if (depth > 8 || !cbor_head(c, &major, &v)) return false;
    if (major == 2 || major == 3) {
        if (v > uint64_t(c->end - c->p)) return false;
        c->p += v;
    } else if (major == 4 || major == 5) {
        if (v > uint64_t(c->end - c->p)) return false;   // each item is at least a byte
        for (uint64_t i = 0; i < (major == 5 ? 2 * v : v); i++)
            if (!cbor_skip(c, depth + 1)) return false;
    } else if (major == 6) {
        return cbor_skip(c, depth + 1);
    }
    return true;
}

static bool cbor_expect(Cbor *c, uint8_t want_major, uint64_t *v) {
    uint8_t major;
    return cbor_head(c, &major, v) && major == want_major;
}

static bool cbor_tag(Cbor *c, uint64_t tag) {
    uint64_t v;
    return cbor_expect(c, 6, &v) && v == tag;
}

static const uint8_t *cbor_bytes(Cbor *c, size_t want) {
    uint64_t v;
    if (!cbor_expect(c, 2, &v) || v != want || v > uint64_t(c->end - c->p)) return nullptr;
    c->p += v;
    return c->p - v;
}

// One "index, hardened" pair of a crypto-keypath.
static bool cbor_component(Cbor *c, uint32_t *out) {
    uint64_t v, hardened;
    if (!cbor_expect(c, 0, &v) || v >= BIP32_INITIAL_HARDENED_CHILD) return false;
    if (!cbor_expect(c, 7, &hardened) || (hardened != 20 && hardened != 21)) return false;
    *out = uint32_t(v) | (hardened == 21 ? BIP32_INITIAL_HARDENED_CHILD : 0);
    return true;
}

struct UrKey {
    const uint8_t *key, *chain_code;
    bool testnet, has_fp;
    uint32_t fp, parent_fp;
    uint32_t path[MAX_PATH_LEN];
    size_t path_len;
};

// A crypto-keypath map. Origin: fills k->path and k->fp. Children: must be
// 0/* or <0;1>/*, or left out, as Sparrow does: all three are read as <0;1>/*.
static bool ur_keypath(Cbor *c, UrKey *k, bool origin) {
    uint64_t entries;
    if (!cbor_tag(c, 304) || !cbor_expect(c, 5, &entries)) return false;
    for (uint64_t e = 0; e < entries; e++) {
        uint64_t key, v;
        if (!cbor_expect(c, 0, &key)) return false;
        if (key == 2 && origin) {
            if (!cbor_expect(c, 0, &v) || v > 0xFFFFFFFFu) return false;
            k->fp = uint32_t(v);
            k->has_fp = true;
        } else if (key == 1 && origin) {
            if (!cbor_expect(c, 4, &v) || v % 2 || v / 2 > MAX_PATH_LEN) return false;
            k->path_len = size_t(v / 2);
            for (size_t i = 0; i < k->path_len; i++)
                if (!cbor_component(c, &k->path[i])) return false;
        } else if (key == 1) {
            // 0/*: [0, false, [], false]. <0;1>/*: [[0, false, 1, false], [], false],
            // which some encoders write with a hardened flag after the pair too.
            uint32_t a, b;
            uint64_t items, f;
            if (!cbor_expect(c, 4, &items) || (items != 3 && items != 4)) return false;
            Cbor peek = *c;
            uint8_t major;
            if (!cbor_head(&peek, &major, &v)) return false;
            if (major == 4) {
                if (!cbor_expect(c, 4, &v) || v != 4 || !cbor_component(c, &a) || !cbor_component(c, &b) ||
                    a != 0 || b != 1)
                    return false;
                if (items == 4 && (!cbor_expect(c, 7, &f) || f != 20)) return false;
            } else if (items != 4 || !cbor_component(c, &a) || a != 0) {
                return false;
            }
            if (!cbor_expect(c, 4, &v) || v != 0 || !cbor_expect(c, 7, &f) || f != 20) return false;
        } else if (!cbor_skip(c)) {
            return false;
        }
    }
    return true;
}

static bool ur_hdkey(Cbor *c, UrKey *k) {
    uint64_t entries;
    memset(k, 0, sizeof(*k));
    if (!cbor_tag(c, 303) || !cbor_expect(c, 5, &entries)) return false;
    for (uint64_t e = 0; e < entries; e++) {
        uint64_t key, v;
        if (!cbor_expect(c, 0, &key)) return false;
        if (key == 3) {
            if (!(k->key = cbor_bytes(c, EC_PUBLIC_KEY_LEN))) return false;
        } else if (key == 4) {
            if (!(k->chain_code = cbor_bytes(c, 32))) return false;
        } else if (key == 5) {                       // crypto-coininfo { 1: type, 2: network }
            uint64_t n;
            if (!cbor_tag(c, 305) || !cbor_expect(c, 5, &n)) return false;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t ck;
                if (!cbor_expect(c, 0, &ck) || !cbor_expect(c, 0, &v)) return false;
                if (ck == 2 && v == 1) k->testnet = true;
            }
        } else if (key == 6 || key == 7) {
            if (!ur_keypath(c, k, key == 6)) return false;
        } else if (key == 8) {
            if (!cbor_expect(c, 0, &v) || v > 0xFFFFFFFFu) return false;
            k->parent_fp = uint32_t(v);
        } else if (!cbor_skip(c)) {
            return false;
        }
    }
    return k->key && k->chain_code;
}

MsResult multisig_load_ur_output(const uint8_t *cbor, size_t len) {
    Cbor c = {cbor, cbor + len};
    uint64_t tag, entries, m = 0, n = 0;
    if (!cbor_expect(&c, 6, &tag)) return MS_BAD;
    if (tag != 401 || !cbor_tag(&c, 407)) return MS_UNSUPPORTED;
    if (!cbor_expect(&c, 5, &entries) || entries != 2) return MS_BAD;
    uint64_t k1, k2;
    if (!cbor_expect(&c, 0, &k1) || k1 != 1 || !cbor_expect(&c, 0, &m) || !cbor_expect(&c, 0, &k2) || k2 != 2 ||
        !cbor_expect(&c, 4, &n))
        return MS_BAD;
    if (n < 1 || n > MS_MAX_KEYS || m < 1 || m > n) return MS_UNSUPPORTED;

    static char text[MAX_TEXT];
    size_t o = size_t(snprintf(text, sizeof(text), "wsh(sortedmulti(%u", unsigned(m)));
    for (uint64_t i = 0; i < n; i++) {
        UrKey k;
        if (!ur_hdkey(&c, &k)) return MS_BAD;
        if (!k.has_fp || !k.path_len) return MS_UNSUPPORTED;
        bool testnet = k.testnet || (k.path_len >= 2 && k.path[1] == (BIP32_INITIAL_HARDENED_CHILD | 1));
        // The xpub, as BIP32 serializes it.
        uint8_t ser[BIP32_SERIALIZED_LEN], *p = ser;
        auto be32 = [&p](uint32_t v) { *p++ = v >> 24; *p++ = v >> 16; *p++ = v >> 8; *p++ = v; };
        be32(testnet ? BIP32_VER_TEST_PUBLIC : BIP32_VER_MAIN_PUBLIC);
        *p++ = uint8_t(k.path_len);                  // depth
        be32(k.parent_fp);
        be32(k.path[k.path_len - 1]);                // child number
        memcpy(p, k.chain_code, 32);
        memcpy(p + 32, k.key, EC_PUBLIC_KEY_LEN);
        char *xpub = nullptr;
        if (wally_base58_from_bytes(ser, sizeof(ser), BASE58_FLAG_CHECKSUM, &xpub) != WALLY_OK) return MS_BAD;

        if (o < sizeof(text)) o += size_t(snprintf(text + o, sizeof(text) - o, ",[%08x", unsigned(k.fp)));
        for (size_t j = 0; j < k.path_len && o < sizeof(text); j++)
            o += size_t(snprintf(text + o, sizeof(text) - o, "/%u%s", unsigned(k.path[j] & ~BIP32_INITIAL_HARDENED_CHILD),
                                 (k.path[j] & BIP32_INITIAL_HARDENED_CHILD) ? "h" : ""));
        if (o < sizeof(text)) o += size_t(snprintf(text + o, sizeof(text) - o, "]%s/<0;1>/*", xpub));
        wally_free_string(xpub);
        if (o >= sizeof(text)) return MS_BAD;
    }
    if (c.p != c.end || o + 3 > sizeof(text)) return MS_BAD;
    memcpy(text + o, "))", 3);
    return multisig_load(text, o + 2);
}

// ------------------------------------------------------------ scripts
bool multisig_path(const uint32_t *path, size_t len, int *chain, uint32_t *index) {
    if (!multisig_loaded() || len != our_path_len + 2) return false;
    if (memcmp(path, our_path, our_path_len * sizeof(uint32_t)) != 0) return false;
    if (path[len - 2] > 1 || (path[len - 1] & BIP32_INITIAL_HARDENED_CHILD)) return false;
    *chain = int(path[len - 2]);
    *index = path[len - 1];
    return true;
}

// depth 0: the scriptPubKey; depth 1: what wsh() wraps. libwally builds the
// inner script in the output buffer first, so it gets room for that.
static size_t script_at(uint32_t depth, int chain, uint32_t index, uint8_t *out, size_t cap) {
    uint8_t buf[MS_MAX_WITNESS_SCRIPT + 64];
    size_t n = 0;
    if (!multisig_loaded() || chain < 0 || chain > 1 || (index & BIP32_INITIAL_HARDENED_CHILD)) return 0;
    if (wally_descriptor_to_script(desc, depth, 0, 0, uint32_t(chain), index, 0, buf, sizeof(buf), &n) != WALLY_OK ||
        n > sizeof(buf) || n > cap)
        return 0;
    memcpy(out, buf, n);
    return n;
}

size_t multisig_script(int chain, uint32_t index, uint8_t *out, size_t cap) {
    return script_at(0, chain, index, out, cap);
}

size_t multisig_witness_script(int chain, uint32_t index, uint8_t *out, size_t cap) {
    return script_at(1, chain, index, out, cap);
}

size_t multisig_address(int chain, uint32_t index, char *out, size_t cap) {
    if (!multisig_loaded() || chain < 0 || chain > 1 || (index & BIP32_INITIAL_HARDENED_CHILD)) return 0;
    char *addr = nullptr;
    if (wally_descriptor_to_address(desc, 0, uint32_t(chain), index, 0, &addr) != WALLY_OK) return 0;
    size_t n = strlen(addr);
    if (n < cap) memcpy(out, addr, n + 1);
    wally_free_string(addr);
    return n < cap ? n : 0;
}
