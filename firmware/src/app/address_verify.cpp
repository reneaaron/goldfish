#include "address_verify.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <wally_core.h>
#include <wally_address.h>
#include <wally_bip32.h>
#include "wallet.h"

// BIP-21 amounts are decimal BTC ("0.001"); BIP177 shows them as whole
// bitcoins, so parse exactly (no floating point). Sets bad for an amount= that
// isn't a plain decimal, has more than 8 decimals or exceeds 21M BTC.
static uint64_t uri_amount(const char *q, size_t len, bool *bad) {
    const char *end = q + len;
    while (q < end) {
        const char *amp = (const char *)memchr(q, '&', end - q);
        if (!amp) amp = end;
        if (amp - q >= 7 && strncasecmp(q, "amount=", 7) == 0) {
            uint64_t whole = 0, frac = 0;
            int fd = -1, digits = 0;   // fd: fraction digits seen, -1 before the dot
            *bad = true;
            for (const char *p = q + 7; p < amp; p++) {
                if (*p == '.' && fd < 0) { fd = 0; continue; }
                if (!isdigit((unsigned char)*p)) return 0;
                digits++;
                if (fd < 0) {
                    whole = whole * 10 + uint64_t(*p - '0');
                    if (whole > 21000000) return 0;
                } else {
                    if (++fd > 8) return 0;
                    frac = frac * 10 + uint64_t(*p - '0');
                }
            }
            for (int i = fd < 0 ? 0 : fd; i < 8; i++) frac *= 10;
            uint64_t v = whole * 100000000ull + frac;
            if (!digits || v > 2100000000000000ull) return 0;
            *bad = false;
            return v;
        }
        q = amp + 1;
    }
    return 0;
}

AddressParse address_parse(const uint8_t *data, size_t len, AddressCheck *out) {
    memset(out, 0, sizeof(*out));
    const char *s = (const char *)data;
    if (len >= 8 && strncasecmp(s, "bitcoin:", 8) == 0) { s += 8; len -= 8; }
    size_t n = 0;
    while (n < len && s[n] != '?' && n < sizeof(out->address) - 1) {
        out->address[n] = char(tolower((unsigned char)s[n]));
        n++;
    }
    out->address[n] = 0;
    if (n < len && s[n] == '?') out->amount = uri_amount(s + n + 1, len - n - 1, &out->amount_bad);
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)out->address[i])) return ADDR_NOT_ADDRESS;

    const char *a = out->address;
    bool bech = strncmp(a, "bc1", 3) == 0 || strncmp(a, "tb1", 3) == 0;
    bool legacy = (a[0] == '1' || a[0] == '3' || a[0] == 'm' || a[0] == 'n' || a[0] == '2') && n >= 26 && n <= 35;
    if (!bech && !legacy) return ADDR_NOT_ADDRESS;
    // P2WPKH: "bc1q" + 38 characters
    if (!bech || a[3] != 'q' || n != 42) return ADDR_UNSUPPORTED;

    out->testnet = a[0] == 't';
    out->status = VERIFY_SEARCHING;
    out->total = VERIFY_RECEIVE_LIMIT + VERIFY_CHANGE_LIMIT;
    return ADDR_OK;
}

// Account chain keys (public only), cached for the address being checked.
// Keyed by seed fingerprint, so loading a different seed never reuses them.
static struct ext_key chains[2];
static bool chains_testnet, chains_ready;
static uint8_t chains_fp[4];

void address_verify_forget() {
    secure_zero(chains, sizeof(chains));
    chains_ready = false;
}

static bool ensure_chains(bool testnet) {
    if (!wallet_has_seed()) return false;
    uint8_t fp[4];
    wallet_fingerprint(fp);
    if (chains_ready && chains_testnet == testnet && memcmp(fp, chains_fp, 4) == 0) return true;
    chains_ready = false;
    const uint32_t H = BIP32_INITIAL_HARDENED_CHILD;
    const uint32_t path[3] = {H | 84, H | (testnet ? 1u : 0u), H | 0};
    struct ext_key acct;
    bool ok = bip32_key_from_parent_path(wallet_root(), path, 3, BIP32_FLAG_KEY_PRIVATE, &acct) == WALLY_OK &&
              bip32_key_from_parent(&acct, 0, BIP32_FLAG_KEY_PUBLIC, &chains[0]) == WALLY_OK &&
              bip32_key_from_parent(&acct, 1, BIP32_FLAG_KEY_PUBLIC, &chains[1]) == WALLY_OK;
    secure_zero(&acct, sizeof(acct));     // drop the account private key
    if (!ok) return false;
    chains_testnet = testnet;
    memcpy(chains_fp, fp, 4);
    chains_ready = true;
    return true;
}

size_t address_at(bool testnet, int chain, int index, char *out, size_t cap) {
    if (chain < 0 || chain > 1 || index < 0 || !ensure_chains(testnet)) return 0;
    struct ext_key k;
    char *addr = nullptr;
    if (bip32_key_from_parent(&chains[chain], uint32_t(index), BIP32_FLAG_KEY_PUBLIC, &k) != WALLY_OK ||
        wally_bip32_key_to_addr_segwit(&k, testnet ? "tb" : "bc", 0, &addr) != WALLY_OK)
        return 0;
    size_t n = strlen(addr);
    if (n < cap) memcpy(out, addr, n + 1);
    wally_free_string(addr);
    return n < cap ? n : 0;
}

bool address_verify_step(AddressCheck *c, int budget) {
    if (c->status != VERIFY_SEARCHING) return true;
    char buf[96];
    // Interleave receive and change so likely addresses come up early:
    // receive 0..499, change 0..99, alternating while both have room.
    while (budget-- > 0 && c->checked < c->total) {
        int k = c->checked;
        int chain, idx;
        if (k < 2 * VERIFY_CHANGE_LIMIT) { chain = k % 2; idx = k / 2; }
        else { chain = 0; idx = k - VERIFY_CHANGE_LIMIT; }
        c->checked++;
        if (!address_at(c->testnet, chain, idx, buf, sizeof(buf))) { c->status = VERIFY_NOT_FOUND; return true; }
        if (strcmp(buf, c->address) == 0) {
            c->status = VERIFY_FOUND;
            c->chain = chain;
            c->index = idx;
            return true;
        }
    }
    if (c->checked >= c->total) { c->status = VERIFY_NOT_FOUND; return true; }
    return false;
}
