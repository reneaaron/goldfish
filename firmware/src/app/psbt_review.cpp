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
#define MAX_CLAIMS 4                     // keypath entries with our fingerprint per input/output

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
        case PSBT_NO_FINGERPRINT: return "Your wallet app has your key without its fingerprint. Add it again from Connect wallet.";
        case PSBT_UNSUPPORTED: return "Only native SegWit wallets are supported for now, single-key or multisig.";
        case PSBT_SIGHASH: return "It asks for a sighash other than ALL. Use the default in your wallet app.";
        case PSBT_TOO_MANY_OUTPUTS: return "It has over 16 outputs, too many for this screen. Pay fewer addresses at once.";
        case PSBT_TOO_BIG: return "It's too big for this signer. Select fewer coins in your wallet app to split it into smaller ones.";
        case PSBT_MISSING_AMOUNTS: return "Your wallet left out the input amounts, so the fee can't be checked.";
        case PSBT_SUSPICIOUS: return "An address claims to be your change but isn't. The transaction may have been tampered with.";
        case PSBT_NEEDS_DESCRIPTOR: return "A multisig transaction needs its wallet's descriptor QR.";
        case PSBT_WRONG_WALLET: return "This transaction doesn't spend from the multisig wallet you loaded.";
        case PSBT_MIXED_NETWORKS: return "It spends mainnet and testnet coins together. Make one transaction per network.";
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

// Every keypath entry carrying our fingerprint, in map order: the first
// MAX_CLAIMS go into `out`, the return value counts all of them. libwally
// signs an input with the first one it can derive, so each one has to verify,
// not just one. A path longer than MAX_PATH_LEN gets path_len 0, which never
// verifies.
static size_t find_claims(const struct wally_map *keypaths, const uint8_t fp[4], Claim out[MAX_CLAIMS]) {
    size_t n = 0;
    for (size_t k = 0; k < keypaths->num_items; k++) {
        uint8_t their[4];
        if (wally_map_keypath_get_item_fingerprint(keypaths, k, their, sizeof(their)) != WALLY_OK) continue;
        if (memcmp(their, fp, 4) != 0) continue;
        if (n < MAX_CLAIMS) {
            Claim *c = &out[n];
            c->pubkey = keypaths->items[k].key;
            c->pubkey_len = keypaths->items[k].key_len;
            if (wally_map_keypath_get_item_path(keypaths, k, c->path, MAX_PATH_LEN, &c->path_len) != WALLY_OK ||
                c->path_len > MAX_PATH_LEN)
                c->path_len = 0;
        }
        n++;
    }
    return n;
}

// Why an input has no claim of ours. A wallet app that has our key without its
// origin (a bare xpub) leaves the key path out or writes fingerprint 00000000:
// that is the cause when a zeroed entry's path derives our key, or when no
// entry names any other seed. Otherwise the input is another seed's.
static PsbtError why_not_ours(const struct wally_map *keypaths) {
    static const uint8_t none[4] = {0, 0, 0, 0};
    bool all_zero = true;
    for (size_t k = 0; k < keypaths->num_items; k++) {
        uint8_t their[4];
        uint32_t path[MAX_PATH_LEN];
        size_t path_len = 0;
        if (wally_map_keypath_get_item_fingerprint(keypaths, k, their, sizeof(their)) != WALLY_OK ||
            memcmp(their, none, 4) != 0) {
            all_zero = false;
            continue;
        }
        const struct wally_map_item &item = keypaths->items[k];
        if (item.key_len != EC_PUBLIC_KEY_LEN ||
            wally_map_keypath_get_item_path(keypaths, k, path, MAX_PATH_LEN, &path_len) != WALLY_OK ||
            path_len > MAX_PATH_LEN)
            continue;
        struct ext_key key;
        bool ours = bip32_key_from_parent_path(wallet_root(), path, path_len,
                                               BIP32_FLAG_KEY_PRIVATE | BIP32_FLAG_SKIP_HASH, &key) == WALLY_OK &&
                    memcmp(key.pub_key, item.key, EC_PUBLIC_KEY_LEN) == 0;
        secure_zero(&key, sizeof(key));
        if (ours) return PSBT_NO_FINGERPRINT;
    }
    return all_zero ? PSBT_NO_FINGERPRINT : PSBT_NOT_OUR_SEED;
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

// With the full previous transaction attached, it must be the one this input
// spends, and agree with the witness UTXO the amount was taken from.
// Otherwise review and signing would each trust a different copy.
static bool utxo_matches_prevout(size_t i, const struct wally_tx_output *used) {
    const struct wally_tx *prev = psbt->inputs[i].utxo;
    uint8_t want[WALLY_TXHASH_LEN], got[WALLY_TXHASH_LEN];
    uint32_t vout;
    if (wally_psbt_get_input_previous_txid(psbt, i, want, sizeof(want)) != WALLY_OK ||
        wally_psbt_get_input_output_index(psbt, i, &vout) != WALLY_OK ||
        wally_tx_get_txid(prev, got, sizeof(got)) != WALLY_OK || memcmp(want, got, sizeof(want)) != 0 ||
        vout >= prev->num_outputs)
        return false;
    const struct wally_tx_output &o = prev->outputs[vout];
    return o.satoshi == used->satoshi && o.script_len == used->script_len &&
           memcmp(o.script, used->script, o.script_len) == 0;
}

// Our multisig witness script for a verified claim's chain and index.
static size_t claim_witness_script(const Claim &c, uint8_t *out, size_t cap) {
    int chain;
    uint32_t index;
    if (!multisig_path(c.path, c.path_len, &chain, &index)) return 0;
    return multisig_witness_script(chain, index, out, cap);
}

#define MAX_WITNESS_SCRIPT 520           // the consensus limit for a pushed script

static bool witness_script_matches(size_t i, const Claim &c) {
    static uint8_t ours[MAX_WITNESS_SCRIPT], theirs[MAX_WITNESS_SCRIPT];   // static: RP2350 stack
    size_t n = claim_witness_script(c, ours, sizeof(ours)), m = 0;
    return n && wally_psbt_get_input_witness_script(psbt, i, theirs, sizeof(theirs), &m) == WALLY_OK &&
           m == n && memcmp(ours, theirs, n) == 0;
}

uint64_t psbt_fee_base(const PsbtReview &r) {
    return r.spend ? r.spend : r.self ? r.self : r.change;
}

FeeLevel psbt_fee_level(uint64_t fee, uint64_t sent, uint32_t rate) {
    if (fee == 0) return FEE_OK;
    if (fee * 2 >= sent || rate >= 500) return FEE_VERY_HIGH;
    if (fee * 10 >= sent || rate >= 50) return FEE_HIGH;
    return FEE_OK;
}

uint32_t psbt_input_vsize(int m, int n) {
    if (!m) return 68;                                   // 41 + (1 + 73 + 34) / 4
    uint32_t script = 34 * uint32_t(n) + 3;              // OP_m <33-byte key>*n OP_n OP_CHECKMULTISIG
    uint32_t wu = 1 + 1 + 73 * uint32_t(m) + (script < 253 ? 1 : 3) + script;   // count, dummy, sigs, script
    return 41 + (wu + 3) / 4;
}

uint32_t psbt_fee_rate(uint64_t fee, uint64_t vsize) {
    uint64_t r = (fee + vsize - 1) / vsize;
    return r > UINT32_MAX ? UINT32_MAX : uint32_t(r);
}

PsbtError psbt_load(const uint8_t *data, size_t len, PsbtReview *r) {
    psbt_clear();
    memset(r, 0, sizeof(*r));
    if (!wallet_has_seed()) return r->error = PSBT_NOT_OUR_SEED;

    int ret = wally_psbt_from_bytes(data, len, WALLY_PSBT_PARSE_FLAG_COMPLETE, &psbt);
    if (ret != WALLY_OK) {
        psbt = nullptr;
        return r->error = ret == WALLY_ENOMEM ? PSBT_TOO_BIG : PSBT_PARSE_FAILED;
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
        Claim claims[MAX_CLAIMS];
        const size_t n_claims = find_claims(&in.keypaths, fp, claims);
        if (n_claims == 0) return r->error = why_not_ours(&in.keypaths);
        if (n_claims > MAX_CLAIMS) return r->error = PSBT_PARSE_FAILED;
        // One network for the whole review: the first input names it, and an
        // input under the other coin type would be shown with the wrong one.
        if (claims[0].path_len >= 2) {
            const bool testnet = (claims[0].path[1] & 0x7FFFFFFF) == 1;
            if (i && testnet != r->testnet) return r->error = PSBT_MIXED_NETWORKS;
            r->testnet = testnet;
        }

        // Anything but ALL leaves outputs or other inputs out of the signature,
        // so what is shown here would not be what gets signed.
        if (in.sighash != 0 && in.sighash != WALLY_SIGHASH_ALL) return r->error = PSBT_SIGHASH;

        // A finalized input keeps no partial signatures when serialized, so
        // the signed QR would carry nothing from us.
        size_t final_scriptsig_len = 0;
        if (in.final_witness || wally_psbt_get_input_final_scriptsig_len(psbt, i, &final_scriptsig_len) != WALLY_OK ||
            final_scriptsig_len)
            return r->error = PSBT_PARSE_FAILED;

        // The witness UTXO, or the spent output of the full previous transaction.
        const struct wally_tx_output *utxo = nullptr;
        if (wally_psbt_get_input_best_utxo(psbt, i, &utxo) != WALLY_OK || !utxo || utxo->satoshi == 0)
            return r->error = PSBT_MISSING_AMOUNTS;
        if (in.utxo && !utxo_matches_prevout(i, utxo)) return r->error = PSBT_PARSE_FAILED;
        size_t type = 0;
        if (wally_scriptpubkey_get_type(utxo->script, utxo->script_len, &type) != WALLY_OK ||
            (type != WALLY_SCRIPT_TYPE_P2WPKH && type != WALLY_SCRIPT_TYPE_P2WSH))
            return r->error = PSBT_UNSUPPORTED;
        if (i && multi != (type == WALLY_SCRIPT_TYPE_P2WSH)) return r->error = PSBT_UNSUPPORTED;
        multi = type == WALLY_SCRIPT_TYPE_P2WSH;
        // Native segwit has no redeem script, and P2WPKH no witness script.
        // libwally would sign with them, so review and signing would disagree.
        size_t redeem_len = 0, witness_len = 0;
        if (wally_psbt_get_input_redeem_script_len(psbt, i, &redeem_len) != WALLY_OK || redeem_len ||
            wally_psbt_get_input_witness_script_len(psbt, i, &witness_len) != WALLY_OK || (!multi && witness_len))
            return r->error = PSBT_PARSE_FAILED;
        if (multi && !multisig_loaded()) return r->error = PSBT_NEEDS_DESCRIPTOR;
        // Our fingerprint alone isn't enough: the key at every claimed path must
        // be one this input's script pays to.
        for (size_t k = 0; k < n_claims; k++) {
            int chain;
            if (multi ? !multisig_claim_verifies(claims[k], utxo->script, utxo->script_len, &chain)
                      : !claim_verifies(claims[k], utxo->script, utxo->script_len, r->testnet, &chain))
                return r->error = multi ? PSBT_WRONG_WALLET : PSBT_NOT_OUR_SEED;
        }
        // libwally signs a P2WSH input with the PSBT's witness script, so it has
        // to be exactly the one the descriptor derives for this input.
        if (multi && !witness_script_matches(i, claims[0])) return r->error = PSBT_WRONG_WALLET;
        r->input_total += utxo->satoshi;
    }
    if (multi) {
        r->ms_m = multisig_info()->m;
        r->ms_n = multisig_info()->n;
        r->testnet = multisig_info()->testnet;
    }

    // Outputs: a transaction without any isn't valid, and can't be signed.
    if (psbt->num_outputs == 0) return r->error = PSBT_PARSE_FAILED;
    if (psbt->num_outputs > PSBT_MAX_OUTPUTS) return r->error = PSBT_TOO_MANY_OUTPUTS;
    r->n_outputs = int(psbt->num_outputs);
    uint64_t out_total = 0, out_vsize = 0;
    for (size_t i = 0; i < psbt->num_outputs; i++) {
        PsbtOutput &o = r->outputs[i];
        const struct wally_psbt_output &m = psbt->outputs[i];
        // PSBT v0 keeps amounts and scripts in the global transaction, v2 per output.
        const uint8_t *script = v0 ? psbt->tx->outputs[i].script : m.script;
        const size_t script_len = v0 ? psbt->tx->outputs[i].script_len : m.script_len;
        o.amount = v0 ? psbt->tx->outputs[i].satoshi : m.amount;
        out_total += o.amount;
        out_vsize += 8 + (script_len < 253 ? 1 : 3) + script_len;
        format_address(script, script_len, r->testnet, o.address, sizeof(o.address));

        Claim claims[MAX_CLAIMS];
        const size_t n_claims = find_claims(&m.keypaths, fp, claims);
        const bool claimed = n_claims > 0;
        // Ours only if every claim verifies, all for the same chain.
        bool verified = claimed && n_claims <= MAX_CLAIMS;
        int chain = 0;
        for (size_t k = 0; verified && k < n_claims; k++) {
            int ck;
            verified = multi ? multisig_claim_verifies(claims[k], script, script_len, &ck)
                             : claim_verifies(claims[k], script, script_len, r->testnet, &ck);
            if (k == 0) chain = ck;
            else verified = verified && ck == chain;
        }
        if (verified) {
            format_path(claims[0], o.path, sizeof(o.path));
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
    r->fee_rate = psbt_fee_rate(r->fee, 11 + uint64_t(r->n_inputs) * psbt_input_vsize(r->ms_m, r->ms_n) + out_vsize);
    r->fee_level = psbt_fee_level(r->fee, psbt_fee_base(*r), r->fee_rate);
    return r->error;
}

// Inputs carrying a signature for the key the PSBT claims for our seed. Other
// signatures don't count: a cosigner may have signed a multisig PSBT first.
static int inputs_signed_by_us() {
    uint8_t fp[4];
    wallet_fingerprint(fp);
    int n = 0;
    for (size_t i = 0; i < psbt->num_inputs; i++) {
        Claim claims[MAX_CLAIMS];
        size_t at = 0;
        if (find_claims(&psbt->inputs[i].keypaths, fp, claims) &&
            wally_map_find(&psbt->inputs[i].signatures, claims[0].pubkey, claims[0].pubkey_len, &at) == WALLY_OK && at)
            n++;
    }
    return n;
}

// Every signature of ours on every input must verify, with SIGHASH_ALL, over
// exactly what review checked: the amount of the input's UTXO and a script
// code derived from our own key (single-sig) or the descriptor (multisig),
// not from scripts the PSBT carries. This catches any way a PSBT can get
// libwally to sign something other than what was shown, including ones review
// doesn't know about. Called before the UTXOs and key paths are trimmed.
// *err gets libwally's error when the check itself fails (WALLY_ENOMEM: out of memory).
static bool signatures_match_review(int *err) {
    uint8_t fp[4];
    wallet_fingerprint(fp);
    struct wally_psbt *v0 = nullptr;     // PSBT v2 has no global tx: use a v0 copy
    const struct wally_tx *tx = psbt->tx;
    if (psbt->version != WALLY_PSBT_VERSION_0) {
        int e = wally_psbt_clone_alloc(psbt, 0, &v0);
        if (e == WALLY_OK) e = wally_psbt_set_version(v0, 0, WALLY_PSBT_VERSION_0);
        if (e != WALLY_OK) {
            if (v0) wally_psbt_free(v0);
            *err = e;
            return false;
        }
        tx = v0->tx;
    }
    bool ok = tx != nullptr;
    for (size_t i = 0; ok && i < psbt->num_inputs; i++) {
        const struct wally_psbt_input &in = psbt->inputs[i];
        Claim claims[MAX_CLAIMS];
        const size_t n_claims = find_claims(&in.keypaths, fp, claims);
        const struct wally_tx_output *utxo = nullptr;
        ok = n_claims > 0 && n_claims <= MAX_CLAIMS && wally_psbt_get_input_best_utxo(psbt, i, &utxo) == WALLY_OK && utxo;
        size_t signed_here = 0;
        for (size_t k = 0; ok && k < n_claims; k++) {
            size_t at = 0;
            if (wally_map_find(&in.signatures, claims[k].pubkey, claims[k].pubkey_len, &at) != WALLY_OK || !at) continue;
            const struct wally_map_item &sig = in.signatures.items[at - 1];
            static uint8_t code[MAX_WITNESS_SCRIPT];                 // static: RP2350 stack
            uint8_t hash[SHA256_LEN], compact[EC_SIGNATURE_LEN];
            size_t code_len = 0;
            if (utxo->script_len == WALLY_SCRIPTPUBKEY_P2WSH_LEN)
                code_len = claim_witness_script(claims[k], code, sizeof(code));
            else if (wally_scriptpubkey_p2pkh_from_bytes(claims[k].pubkey, claims[k].pubkey_len, WALLY_SCRIPT_HASH160,
                                                         code, sizeof(code), &code_len) != WALLY_OK)
                code_len = 0;
            ok = code_len && sig.value_len > 1 && sig.value[sig.value_len - 1] == WALLY_SIGHASH_ALL &&
                 wally_tx_get_btc_signature_hash(tx, i, code, code_len, utxo->satoshi, WALLY_SIGHASH_ALL,
                                                 WALLY_TX_FLAG_USE_WITNESS, hash, sizeof(hash)) == WALLY_OK &&
                 wally_ec_sig_from_der(sig.value, sig.value_len - 1, compact, sizeof(compact)) == WALLY_OK &&
                 wally_ec_sig_verify(claims[k].pubkey, claims[k].pubkey_len, hash, sizeof(hash), EC_FLAG_ECDSA,
                                     compact, sizeof(compact)) == WALLY_OK;
            signed_here++;
        }
        ok = ok && signed_here > 0;      // every input carries our signature
    }
    if (v0) wally_psbt_free(v0);
    return ok;
}

// Sign, then drop what the coordinator already has (UTXOs and key paths), so
// the signed PSBT stays small enough for few QR frames. Only a PSBT that
// passed review is signed. A signature of ours that was already there counts too.
int psbt_sign() {
    if (!psbt || !reviewed_ok || !wallet_has_seed()) return 0;
    if (signed_inputs) return signed_inputs;      // the key paths are gone: nothing more to sign
    int ret = wally_psbt_sign_bip32(psbt, wallet_root(), EC_FLAG_GRIND_R);
    if (ret != WALLY_OK || !signatures_match_review(&ret)) {
        psbt_clear();                    // never hand out a signature over anything else
        return ret == WALLY_ENOMEM ? -1 : 0;
    }
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
