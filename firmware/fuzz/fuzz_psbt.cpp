// PSBT review, signing and serialization on untrusted bytes, with the
// SeedSigner test seed loaded so mutations of its PSBT reach the "ours" paths.
// First byte picks the transport the UI accepts: raw "psbt\xff" bytes or base64.
#include <stdint.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip39.h>
#include <wally_psbt.h>
#include "../src/app/psbt_review.h"
#include "../src/app/wallet.h"
#include "../src/app/ur.h"
#include "../test/fixtures/seedsigner_vectors.h"
#include "fuzz_check.h"

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    wally_init(0);
    if (bip39_mnemonic_to_bytes(nullptr, SINGLESIG_MNEMONIC, e.bytes, sizeof(e.bytes), &e.len) != WALLY_OK ||
        !wallet_load(e))
        abort();
    return 0;
}

static void check_review(const PsbtReview &r) {
    FUZZ_CHECK(r.n_outputs >= 0 && r.n_outputs <= PSBT_MAX_OUTPUTS);
    for (int i = 0; i < r.n_outputs; i++) {
        FUZZ_CHECK(memchr(r.outputs[i].address, 0, sizeof(r.outputs[i].address)));
        FUZZ_CHECK(memchr(r.outputs[i].path, 0, sizeof(r.outputs[i].path)));
    }
    if (r.error != PSBT_OK) return;
    // What the user sees must add up: every sat in is spent, kept or paid as fee.
    unsigned __int128 out = 0, kept = 0;
    for (int i = 0; i < r.n_outputs; i++) out += r.outputs[i].amount;
    kept = (unsigned __int128)r.spend + r.self + r.change;
    FUZZ_CHECK(kept == out);
    FUZZ_CHECK(out + r.fee == r.input_total);
    FUZZ_CHECK(r.fee <= r.input_total);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t mode = data[0];
    data++; size--;
    static uint8_t raw[UR_MAX_MESSAGE];
    if (mode & 1) {   // base64, as handle_psbt_payload does for "cHNidP..."
        if (size < 6 || memcmp(data, "cHNidP", 6) != 0) return 0;
        static char b64[UR_MAX_MESSAGE * 2];
        if (size >= sizeof(b64)) return 0;
        memcpy(b64, data, size);
        b64[size] = 0;
        size_t n = 0;
        if (wally_base64_to_bytes(b64, 0, raw, sizeof(raw), &n) != WALLY_OK || n == 0 || n > sizeof(raw)) return 0;
        data = raw;
        size = n;
    }

    static PsbtReview r;
    PsbtError e = psbt_load(data, size, &r);
    check_review(r);
    if (e == PSBT_OK) {
        int sigs = psbt_sign();
        static uint8_t out[UR_MAX_MESSAGE * 2];
        size_t n = psbt_serialize(out, sizeof(out));
        if (n) {
            // The signed PSBT must parse, keep the exact transaction that was
            // reviewed, and carry as many signed inputs as psbt_sign reported.
            struct wally_psbt *orig = nullptr, *signd = nullptr;
            FUZZ_CHECK(wally_psbt_from_bytes(data, size, WALLY_PSBT_PARSE_FLAG_COMPLETE, &orig) == WALLY_OK);
            FUZZ_CHECK(wally_psbt_from_bytes(out, n, 0, &signd) == WALLY_OK);
            uint8_t a[32], b[32];
            FUZZ_CHECK(wally_psbt_get_id(orig, 0, a, sizeof(a)) == WALLY_OK);
            FUZZ_CHECK(wally_psbt_get_id(signd, 0, b, sizeof(b)) == WALLY_OK);
            FUZZ_CHECK(memcmp(a, b, 32) == 0);
            int with_sig = 0;
            for (size_t i = 0; i < signd->num_inputs; i++) with_sig += signd->inputs[i].signatures.num_items > 0;
            if (!(sigs > 0 && with_sig >= sigs)) fprintf(stderr, "sigs=%d with_sig=%d inputs=%zu\n", sigs, with_sig, signd->num_inputs);
            FUZZ_CHECK(sigs > 0 && with_sig >= sigs);
            wally_psbt_free(orig);
            wally_psbt_free(signd);
        }
    }
    psbt_clear();
    return 0;
}
