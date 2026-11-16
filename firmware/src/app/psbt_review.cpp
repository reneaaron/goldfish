#include "psbt_review.h"
#include <stdio.h>
#include <string.h>
#include <wally_core.h>
#include <wally_address.h>
#include <wally_bip32.h>
#include <wally_crypto.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>
#include "multisig.h"
#include "wallet.h"

#define MAX_PATH_LEN 10

static struct wally_psbt *psbt;
static bool reviewed_ok;                 // psbt_load returned PSBT_OK for it
static int signed_inputs;                // inputs we signed; once signed, it is trimmed

void psbt_clear() {
    if (psbt) wally_psbt_free(psbt);
    psbt = nullptr;
    reviewed_ok = false;
    signed_inputs = 0;
}

const char *psbt_error_text(PsbtError e) {
    switch (e) {
        case PSBT_OK: return "";
        case PSBT_PARSE_FAILED: return "This isn't a valid transaction. Export it again from your wallet.";
        case PSBT_NOT_OUR_SEED: return "This transaction is for a different wallet. Scan the seed QR it belongs to.";
        case PSBT_UNSUPPORTED: return "Only native SegWit wallets are supported for now, single-key or multisig.";
        case PSBT_MISSING_AMOUNTS: return "Your wallet left out the input amounts, so the fee can't be checked.";
        case PSBT_SUSPICIOUS: return "An address claims to be your change but isn't. The transaction may have been tampered with.";
        case PSBT_NEEDS_DESCRIPTOR: return "This is a multisig transaction. Scan the wallet's descriptor first, then the transaction again.";
        case PSBT_WRONG_WALLET: return "This transaction doesn't spend from the multisig wallet you loaded.";
    }
    return "";
}

// One BIP32 derivation claimed for our fingerprint in a PSBT keypath map.
struct Claim {
    const uint8_t *pubkey;
    size_t pubkey_len;
    uint32_t path[MAX_PATH_LEN];
    size_t path_len;
};

// The last keypath entry carrying our fingerprint, if any. A path longer than
// MAX_PATH_LEN is reported with path_len 0, which never verifies.
static bool find_claim(const struct wally_map *keypaths, const uint8_t fp[4], Claim *c) {
    bool found = false;
    for (size_t k = 0; k < keypaths->num_items; k++) {
        uint8_t their[4];
        if (wally_map_keypath_get_item_fingerprint(keypaths, k, their, sizeof(their)) != WALLY_OK) continue;
        if (memcmp(their, fp, 4) != 0) continue;
        found = true;
        c->pubkey = keypaths->items[k].key;
        c->pubkey_len = keypaths->items[k].key_len;
        if (wally_map_keypath_get_item_path(keypaths, k, c->path, MAX_PATH_LEN, &c->path_len) != WALLY_OK ||
            c->path_len > MAX_PATH_LEN)
            c->path_len = 0;
    }
    return found;
}

static void format_path(const Claim &c, char *out, size_t cap) {
    size_t o = snprintf(out, cap, "m");
    for (size_t i = 0; i < c.path_len && o < cap; i++) {
        uint32_t v = c.path[i];
        o += snprintf(out + o, cap - o, "/%u%s", unsigned(v & 0x7FFFFFFF), (v & 0x80000000) ? "h" : "");
    }
}

// True if the claim is a BIP84 address of ours that pays to exactly this
// script: the path is m/84h/coin'/account'/{0,1}/n, the key there is the one
// in the metadata, and the script is its P2WPKH (0x00 0x14 <hash160(pubkey)>).
static bool claim_verifies(const Claim &c, const uint8_t *script, size_t script_len, bool testnet, int *chain) {
    const uint32_t H = BIP32_INITIAL_HARDENED_CHILD;
    if (c.path_len != 5 || c.path[0] != H + 84 || c.path[1] != H + (testnet ? 1 : 0)) return false;
    if (!(c.path[2] & H) || c.path[3] > 1 || (c.path[4] & H)) return false;
    if (c.pubkey_len != EC_PUBLIC_KEY_LEN) return false;
    struct ext_key k;
    uint8_t h[HASH160_LEN];
    bool ok = bip32_key_from_parent_path(wallet_root(), c.path, c.path_len,
                                         BIP32_FLAG_KEY_PRIVATE | BIP32_FLAG_SKIP_HASH, &k) == WALLY_OK &&
              memcmp(k.pub_key, c.pubkey, EC_PUBLIC_KEY_LEN) == 0 &&
              wally_hash160(k.pub_key, EC_PUBLIC_KEY_LEN, h, sizeof(h)) == WALLY_OK &&
              script_len == 2 + HASH160_LEN && script[0] == 0x00 && script[1] == HASH160_LEN &&
              memcmp(script + 2, h, HASH160_LEN) == 0;
    secure_zero(&k, sizeof(k));
    *chain = int(c.path[3]);
    return ok;
}

// The multisig version: the path is our key's in the loaded descriptor plus
// {0,1}/n, the key there is the one in the metadata, and the script is the
// descriptor's P2WSH for that chain and index. The cosigner keys and witness
// script the PSBT carries are not trusted for this.
static bool multisig_claim_verifies(const Claim &c, const uint8_t *script, size_t script_len, int *chain) {
    uint32_t index;
    uint8_t ours[WALLY_SCRIPTPUBKEY_P2WSH_LEN];
    if (!multisig_path(c.path, c.path_len, chain, &index) || c.pubkey_len != EC_PUBLIC_KEY_LEN) return false;
    struct ext_key k;
    bool ok = bip32_key_from_parent_path(wallet_root(), c.path, c.path_len,
                                         BIP32_FLAG_KEY_PRIVATE | BIP32_FLAG_SKIP_HASH, &k) == WALLY_OK &&
              memcmp(k.pub_key, c.pubkey, EC_PUBLIC_KEY_LEN) == 0 &&
              multisig_script(*chain, index, ours, sizeof(ours)) == sizeof(ours) &&
              script_len == sizeof(ours) && memcmp(script, ours, sizeof(ours)) == 0;
    secure_zero(&k, sizeof(k));
    return ok;
}

static void format_address(const uint8_t *script, size_t len, bool testnet, char *out, size_t cap) {
    char *s = nullptr;
    if (wally_addr_segwit_from_bytes(script, len, testnet ? "tb" : "bc", 0, &s) != WALLY_OK &&
        wally_scriptpubkey_to_address(script, len,
                                      testnet ? WALLY_NETWORK_BITCOIN_TESTNET : WALLY_NETWORK_BITCOIN_MAINNET,
                                      &s) != WALLY_OK)
        s = nullptr;
    snprintf(out, cap, "%s", s ? s : "(non-standard output)");
    if (s) wally_free_string(s);
}

PsbtError psbt_load(const uint8_t *data, size_t len, PsbtReview *r) {
    psbt_clear();
    memset(r, 0, sizeof(*r));
    if (!wallet_has_seed()) return r->error = PSBT_NOT_OUR_SEED;

    if (wally_psbt_from_bytes(data, len, WALLY_PSBT_PARSE_FLAG_COMPLETE, &psbt) != WALLY_OK) {
        psbt = nullptr;
        return r->error = PSBT_PARSE_FAILED;
    }
    const bool v0 = psbt->version == WALLY_PSBT_VERSION_0;
    if (v0 && !psbt->tx) return r->error = PSBT_PARSE_FAILED;

    uint8_t fp[4];
    wallet_fingerprint(fp);

    // Inputs: all ours, native segwit, with amounts, and all of one kind:
    // single-sig, or the loaded multisig wallet.
    bool multi = false;
    r->n_inputs = int(psbt->num_inputs);
    if (r->n_inputs == 0) return r->error = PSBT_PARSE_FAILED;
    for (size_t i = 0; i < psbt->num_inputs; i++) {
        const struct wally_psbt_input &in = psbt->inputs[i];
        Claim c;
        if (!find_claim(&in.keypaths, fp, &c)) return r->error = PSBT_NOT_OUR_SEED;
        if (c.path_len >= 2) r->testnet = (c.path[1] & 0x7FFFFFFF) == 1;

        // Anything but ALL leaves outputs or other inputs out of the signature,
        // so what is shown here would not be what gets signed.
        if (in.sighash != 0 && in.sighash != WALLY_SIGHASH_ALL) return r->error = PSBT_UNSUPPORTED;

        // The witness UTXO, or the spent output of the full previous transaction.
        const struct wally_tx_output *utxo = nullptr;
        if (wally_psbt_get_input_best_utxo(psbt, i, &utxo) != WALLY_OK || !utxo || utxo->satoshi == 0)
            return r->error = PSBT_MISSING_AMOUNTS;
        size_t type = 0;
        if (wally_scriptpubkey_get_type(utxo->script, utxo->script_len, &type) != WALLY_OK ||
            (type != WALLY_SCRIPT_TYPE_P2WPKH && type != WALLY_SCRIPT_TYPE_P2WSH))
            return r->error = PSBT_UNSUPPORTED;
        if (i && multi != (type == WALLY_SCRIPT_TYPE_P2WSH)) return r->error = PSBT_UNSUPPORTED;
        multi = type == WALLY_SCRIPT_TYPE_P2WSH;
        if (multi) {
            int chain;
            if (!multisig_loaded()) return r->error = PSBT_NEEDS_DESCRIPTOR;
            if (!multisig_claim_verifies(c, utxo->script, utxo->script_len, &chain)) return r->error = PSBT_WRONG_WALLET;
        }
        r->input_total += utxo->satoshi;
    }
    if (multi) {
        r->ms_m = multisig_info()->m;
        r->ms_n = multisig_info()->n;
        r->testnet = multisig_info()->testnet;
    }

    // Outputs
    if (psbt->num_outputs > PSBT_MAX_OUTPUTS) return r->error = PSBT_UNSUPPORTED;
    r->n_outputs = int(psbt->num_outputs);
    uint64_t out_total = 0;
    for (size_t i = 0; i < psbt->num_outputs; i++) {
        PsbtOutput &o = r->outputs[i];
        const struct wally_psbt_output &m = psbt->outputs[i];
        // PSBT v0 keeps amounts and scripts in the global transaction, v2 per output.
        const uint8_t *script = v0 ? psbt->tx->outputs[i].script : m.script;
        const size_t script_len = v0 ? psbt->tx->outputs[i].script_len : m.script_len;
        o.amount = v0 ? psbt->tx->outputs[i].satoshi : m.amount;
        out_total += o.amount;
        format_address(script, script_len, r->testnet, o.address, sizeof(o.address));

        Claim c;
        bool claimed = find_claim(&m.keypaths, fp, &c);
        int chain = 0;
        if (claimed && (multi ? multisig_claim_verifies(c, script, script_len, &chain)
                              : claim_verifies(c, script, script_len, r->testnet, &chain))) {
            format_path(c, o.path, sizeof(o.path));
            if (chain == 1) { o.is_change = true; r->change += o.amount; }
            else { o.is_self = true; r->self += o.amount; }
        } else if (claimed) {
            o.suspicious = true;
            r->error = PSBT_SUSPICIOUS;
        } else {
            r->spend += o.amount;
        }
    }
    if (out_total > r->input_total) return r->error = PSBT_PARSE_FAILED;
    r->fee = r->input_total - out_total;
    reviewed_ok = r->error == PSBT_OK;
    return r->error;
}

// Inputs carrying a signature for the key the PSBT claims for our seed. Other
// signatures don't count: a cosigner may have signed a multisig PSBT first.
static int inputs_signed_by_us() {
    uint8_t fp[4];
    wallet_fingerprint(fp);
    int n = 0;
    for (size_t i = 0; i < psbt->num_inputs; i++) {
        Claim c;
        size_t at = 0;
        if (find_claim(&psbt->inputs[i].keypaths, fp, &c) &&
            wally_map_find(&psbt->inputs[i].signatures, c.pubkey, c.pubkey_len, &at) == WALLY_OK && at)
            n++;
    }
    return n;
}

// Sign, then drop what the coordinator already has (UTXOs and key paths), so
// the signed PSBT stays small enough for few QR frames. Only a PSBT that
// passed review is signed. A signature of ours that was already there counts too.
int psbt_sign() {
    if (!psbt || !reviewed_ok || !wallet_has_seed()) return 0;
    if (signed_inputs) return signed_inputs;      // the key paths are gone: nothing more to sign
    if (wally_psbt_sign_bip32(psbt, wallet_root(), EC_FLAG_GRIND_R) != WALLY_OK) return 0;
    int n = inputs_signed_by_us();
    if (n > 0) {
        for (size_t i = 0; i < psbt->num_inputs; i++) {
            wally_psbt_input_set_utxo(&psbt->inputs[i], nullptr);
            wally_psbt_input_set_witness_utxo(&psbt->inputs[i], nullptr);
            wally_map_clear(&psbt->inputs[i].keypaths);
        }
        for (size_t i = 0; i < psbt->num_outputs; i++) wally_map_clear(&psbt->outputs[i].keypaths);
    }
    return signed_inputs = n;
}

size_t psbt_serialize(uint8_t *out, size_t cap) {
    size_t n = 0;
    if (!psbt || wally_psbt_to_bytes(psbt, 0, out, cap, &n) != WALLY_OK || n > cap) return 0;
    return n;
}
