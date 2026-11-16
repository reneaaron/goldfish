#include "bip85.h"
#include <string.h>
#include <wally_bip32.h>
#include <wally_bip85.h>

bool bip85_bip39_entropy(const struct ext_key &root, int words, uint32_t index, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if ((words != 12 && words != 24) || index > BIP85_MAX_INDEX) return false;
    // libwally writes the whole 64-byte HMAC and says how much of it is entropy.
    uint8_t entropy[64];
    size_t n = 0;
    bool ok = bip85_get_bip39_entropy(&root, nullptr, uint32_t(words), index, entropy, sizeof(entropy), &n) ==
                  WALLY_OK &&
              n == size_t(words == 12 ? 16 : 32);
    if (ok) {
        out->len = n;
        memcpy(out->bytes, entropy, n);
    }
    secure_zero(entropy, sizeof(entropy));
    return ok;
}
