#include "psbt_review.h"
#include <new>
#include <stdio.h>
#include <string.h>
#include <PSBT.h>
#include "wallet.h"

alignas(PSBT) static uint8_t psbt_mem[sizeof(PSBT)];
static PSBT *psbt;

void psbt_clear() {
    if (psbt) {
        psbt->~PSBT();
        psbt = nullptr;
    }
    memset(psbt_mem, 0, sizeof(psbt_mem));
}

static bool fp_equal(const uint8_t a[4], const uint8_t b[4]) { return memcmp(a, b, 4) == 0; }

static void format_path(const PSBTDerivation &d, char *out, size_t cap) {
    size_t o = snprintf(out, cap, "m");
    for (uint8_t i = 0; i < d.derivationLen && o < cap; i++) {
        uint32_t v = d.derivation[i];
        o += snprintf(out + o, cap - o, "/%u%s", unsigned(v & 0x7FFFFFFF), (v & 0x80000000) ? "h" : "");
    }
}

const char *psbt_error_text(PsbtError e) {
    switch (e) {
        case PSBT_OK: return "";
        case PSBT_PARSE_FAILED: return "This isn't a valid transaction. Export it again from your wallet.";
        case PSBT_NOT_OUR_SEED: return "This transaction is for a different wallet. Scan the seed QR it belongs to.";
        case PSBT_UNSUPPORTED: return "Only single-key native SegWit wallets are supported for now.";
        case PSBT_MISSING_AMOUNTS: return "Your wallet left out the input amounts, so the fee can't be checked.";
        case PSBT_SUSPICIOUS: return "An address claims to be your change but isn't. The transaction may have been tampered with.";
    }
    return "";
}

FeeLevel psbt_fee_level(uint64_t fee, uint64_t sent, uint32_t rate) {
    if (fee == 0) return FEE_OK;
    if (fee * 2 >= sent || rate >= 500) return FEE_VERY_HIGH;
    if (fee * 10 >= sent || rate >= 50) return FEE_HIGH;
    return FEE_OK;
}

uint32_t psbt_fee_rate(uint64_t fee, int n_inputs, int n_outputs) {
    uint64_t vsize = 11 + 68 * uint64_t(n_inputs) + 31 * uint64_t(n_outputs);
    uint64_t r = (fee + vsize - 1) / vsize;
    return r > UINT32_MAX ? UINT32_MAX : uint32_t(r);
}

PsbtError psbt_load(const uint8_t *data, size_t len, PsbtReview *r) {
    psbt_clear();
    memset(r, 0, sizeof(*r));
    if (!wallet_has_seed()) return r->error = PSBT_NOT_OUR_SEED;

    psbt = new (psbt_mem) PSBT();
    if (psbt->parse(data, len) != len || !psbt->isValid()) return r->error = PSBT_PARSE_FAILED;

    const HDPrivateKey &root = *wallet_root();
    uint8_t fp[4];
    wallet_fingerprint(fp);

    // Inputs: all ours, native segwit, with amounts.
    r->n_inputs = int(psbt->tx.inputsNumber);
    if (r->n_inputs == 0) return r->error = PSBT_PARSE_FAILED;
    for (size_t i = 0; i < psbt->tx.inputsNumber; i++) {
        const PSBTInputMetadata &m = psbt->txInsMeta[i];
        bool ours = false;
        for (uint8_t k = 0; k < m.derivationsLen; k++) {
            const PSBTDerivation &d = m.derivations[k];
            if (!fp_equal(d.fingerprint, fp)) continue;
            ours = true;
            if (d.derivationLen >= 2) r->testnet = (d.derivation[1] & 0x7FFFFFFF) == 1;
        }
        if (!ours) return r->error = PSBT_NOT_OUR_SEED;
        if (m.txOut.amount == 0) return r->error = PSBT_MISSING_AMOUNTS;
        if (m.txOut.scriptPubkey.type() != P2WPKH) return r->error = PSBT_UNSUPPORTED;
        r->input_total += m.txOut.amount;
    }

    // Outputs
    if (psbt->tx.outputsNumber > PSBT_MAX_OUTPUTS) return r->error = PSBT_UNSUPPORTED;
    r->n_outputs = int(psbt->tx.outputsNumber);
    const Network *net = r->testnet ? &Testnet : &Mainnet;
    uint64_t out_total = 0;
    for (size_t i = 0; i < psbt->tx.outputsNumber; i++) {
        PsbtOutput &o = r->outputs[i];
        const TxOut &txo = psbt->tx.txOuts[i];
        o.amount = txo.amount;
        out_total += txo.amount;
        if (txo.address(o.address, sizeof(o.address), net) == 0)
            snprintf(o.address, sizeof(o.address), "(non-standard output)");

        const PSBTOutputMetadata &m = psbt->txOutsMeta[i];
        const PSBTDerivation *claim = nullptr;
        for (uint8_t k = 0; k < m.derivationsLen; k++)
            if (fp_equal(m.derivations[k].fingerprint, fp)) claim = &m.derivations[k];

        if (claim && psbt->isMine(uint8_t(i), root)) {
            format_path(*claim, o.path, sizeof(o.path));
            bool internal = claim->derivationLen >= 2 && claim->derivation[claim->derivationLen - 2] == 1;
            if (internal) { o.is_change = true; r->change += o.amount; }
            else { o.is_self = true; r->self += o.amount; }
        } else if (claim) {
            o.suspicious = true;
            r->error = PSBT_SUSPICIOUS;
        } else {
            r->spend += o.amount;
        }
    }
    if (out_total > r->input_total) return r->error = PSBT_PARSE_FAILED;
    r->fee = r->input_total - out_total;
    r->fee_rate = psbt_fee_rate(r->fee, r->n_inputs, r->n_outputs);
    r->fee_level = psbt_fee_level(r->fee, r->spend ? r->spend : r->self, r->fee_rate);
    return r->error;
}

int psbt_sign() {
    if (!psbt || !wallet_has_seed()) return 0;
    return psbt->sign(*wallet_root());
}

size_t psbt_serialize(uint8_t *out, size_t cap) {
    if (!psbt || psbt->length() > cap) return 0;
    return psbt->serialize(out, cap);
}
