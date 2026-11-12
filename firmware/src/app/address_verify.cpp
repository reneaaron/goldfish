#include "address_verify.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <Bitcoin.h>
#include "wallet.h"

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

// Account chain keys, cached for the address being checked.
// Keyed by seed fingerprint, so loading a different seed never reuses them.
static HDPublicKey chains[2];
static bool chains_testnet, chains_ready;
static uint8_t chains_fp[4];

static bool ensure_chains(bool testnet) {
    if (!wallet_has_seed()) return false;
    uint8_t fp[4];
    wallet_fingerprint(fp);
    if (chains_ready && chains_testnet == testnet && memcmp(fp, chains_fp, 4) == 0) return true;
    HDPrivateKey acct = wallet_root()->derive(testnet ? "m/84h/1h/0h/" : "m/84h/0h/0h/");
    HDPublicKey xpub = acct.xpub();
    acct = HDPrivateKey();
    xpub.type = P2WPKH;
    xpub.network = testnet ? &Testnet : &Mainnet;
    chains[0] = xpub.child(0);
    chains[1] = xpub.child(1);
    chains_testnet = testnet;
    memcpy(chains_fp, fp, 4);
    chains_ready = true;
    return true;
}

size_t address_at(bool testnet, int chain, int index, char *out, size_t cap) {
    if (chain < 0 || chain > 1 || !ensure_chains(testnet)) return 0;
    HDPublicKey k = chains[chain].child(uint32_t(index));
    int n = k.address(out, cap);
    return n > 0 ? size_t(n) : 0;
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
