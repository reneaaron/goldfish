// PSBT review, signing and serialization on untrusted bytes, with the
// SeedSigner test seed loaded so mutations of its PSBT reach the "ours" paths.
// First byte picks the transport the UI accepts: raw "psbt\xff" bytes or base64.
#include <stdint.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip39.h>
#include "../src/app/wallet.h"
#include "../test/fixtures/seedsigner_vectors.h"
#include "fuzz_psbt_common.h"

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    wally_init(0);
    if (bip39_mnemonic_to_bytes(nullptr, SINGLESIG_MNEMONIC, e.bytes, sizeof(e.bytes), &e.len) != WALLY_OK ||
        !wallet_load(e))
        abort();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_psbt_input(data, size);
    return 0;
}
