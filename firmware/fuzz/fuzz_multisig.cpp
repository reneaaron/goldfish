// Multisig: everything a cosigner's scanner reads with SeedSigner's 2-of-3
// test cosigner seed loaded. First byte picks:
//   0: descriptor text (multisig_load), as a plain-text QR or ur:bytes
//   1: crypto-output CBOR (multisig_load_ur_output), as the assembled UR
//   2: a PSBT ([transport][bytes], as fuzz_psbt) with the wallet's descriptor
//      loaded, so mutations reach the multisig review and signing paths
//   3: an address or bitcoin: URI with the descriptor loaded, plus a few
//      steps of the multisig address search
#include <stdint.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip39.h>
#include "../src/app/address_verify.h"
#include "../src/app/multisig.h"
#include "../src/app/wallet.h"
#include "../test/fixtures/multisig_vectors.h"
#include "fuzz_psbt_common.h"

static bool replaced;   // a fuzzed descriptor replaced MS_DESCRIPTOR

static void load_wallet_descriptor() {
    if (multisig_loaded() && !replaced) return;
    if (multisig_load(MS_DESCRIPTOR, strlen(MS_DESCRIPTOR)) != MS_OK) abort();
    replaced = false;
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    wally_init(0);
    if (bip39_mnemonic_to_bytes(nullptr, MS_MNEMONIC, e.bytes, sizeof(e.bytes), &e.len) != WALLY_OK ||
        !wallet_load(e))
        abort();
    load_wallet_descriptor();
    return 0;
}

// A descriptor that loaded must be one the rest of the firmware can use.
static void check_loaded(MsResult r) {
    if (r != MS_OK) return;
    replaced = true;
    const MsInfo *i = multisig_info();
    FUZZ_CHECK(i != nullptr);
    FUZZ_CHECK(i->n >= 1 && i->n <= MS_MAX_KEYS && i->m >= 1 && i->m <= i->n);
    FUZZ_CHECK(i->ours >= 0 && i->ours < i->n);
    uint8_t spk[64], ws[MS_MAX_WITNESS_SCRIPT + 64];
    char addr[96];
    for (int chain = 0; chain < 2; chain++) {
        FUZZ_CHECK(multisig_script(chain, 0, spk, sizeof(spk)) == 34);
        size_t n = multisig_witness_script(chain, 0, ws, sizeof(ws));
        FUZZ_CHECK(n == size_t(3 + 34 * i->n));
        FUZZ_CHECK(multisig_address(chain, 0, addr, sizeof(addr)) == 62);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t mode = data[0];
    data++; size--;
    switch (mode % 4) {
        case 0: {
            // own heap copy, not NUL-terminated, so ASan sees overreads
            char *text = (char *)malloc(size ? size : 1);
            memcpy(text, data, size);
            check_loaded(multisig_load(text, size));
            free(text);
            break;
        }
        case 1: {
            uint8_t *cbor = (uint8_t *)malloc(size ? size : 1);
            memcpy(cbor, data, size);
            check_loaded(multisig_load_ur_output(cbor, size));
            free(cbor);
            break;
        }
        case 2:
            load_wallet_descriptor();
            fuzz_psbt_input(data, size);
            break;
        case 3: {
            load_wallet_descriptor();
            uint8_t *text = (uint8_t *)malloc(size ? size : 1);
            memcpy(text, data, size);
            static AddressCheck c;
            AddressParse r = address_parse(text, size, &c);
            free(text);
            FUZZ_CHECK(memchr(c.address, 0, sizeof(c.address)));
            if (r == ADDR_OK) {
                for (int k = 0; k < 3 && !address_verify_step(&c, 4); k++) {}
                FUZZ_CHECK(c.checked >= 0 && c.checked <= c.total);
                if (c.status == VERIFY_FOUND) FUZZ_CHECK(c.chain == 0 || c.chain == 1);
            }
            break;
        }
    }
    return 0;
}
