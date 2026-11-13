#include "bip85.h"
#include <string.h>
#include <Bitcoin.h>
#include <Hash.h>

bool bip85_bip39_entropy(const HDPrivateKey &root, int words, uint32_t index, SeedEntropy *out) {
    memset(out, 0, sizeof(*out));
    if ((words != 12 && words != 24) || index > BIP85_MAX_INDEX) return false;
    const uint32_t H = 0x80000000;
    uint32_t path[] = {H + 83696968, H + 39, H + 0, H + uint32_t(words), H + index};
    HDPrivateKey child = root.derive(path, 5);
    uint8_t k[32], entropy[64];
    child.getSecret(k);
    child = HDPrivateKey();
    static const char KEY[] = "bip-entropy-from-k";
    sha512Hmac((const uint8_t *)KEY, sizeof(KEY) - 1, k, sizeof(k), entropy);
    out->len = words == 12 ? 16 : 32;
    memcpy(out->bytes, entropy, out->len);
    secure_zero(k, sizeof(k));
    secure_zero(entropy, sizeof(entropy));
    return true;
}
