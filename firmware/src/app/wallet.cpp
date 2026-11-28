#include "wallet.h"
#include "address_verify.h"
#include <string.h>
#include <stdio.h>
#include <wally_core.h>
#include <wally_bip32.h>
#include <wally_bip39.h>

static struct ext_key root;
static bool loaded;
static int words;
static SeedEntropy entropy;               // kept for the QR backup, wiped with the key
static bool passphrase;                   // the root key was derived with one
static bool testnet;

bool wallet_testnet() { return testnet; }
void wallet_set_testnet(bool t) { testnet = t; }

bool wallet_has_seed() { return loaded; }
const struct ext_key *wallet_root() { return loaded ? &root : nullptr; }

void wallet_wipe() {
    secure_zero(&root, sizeof(root));
    loaded = false;
    seedqr_wipe(&entropy);
    address_verify_forget();              // its cached account keys go with the seed
    words = 0;
    passphrase = false;
}

// Entropy -> mnemonic -> BIP39 seed -> master key.
static bool root_from_entropy(const SeedEntropy &e, const char *pass, struct ext_key *out) {
    char *mn = nullptr;
    if (bip39_mnemonic_from_bytes(nullptr, e.bytes, e.len, &mn) != WALLY_OK) return false;
    uint8_t seed[BIP39_SEED_LEN_512];
    bool ok = bip39_mnemonic_to_seed512(mn, pass, seed, sizeof(seed)) == WALLY_OK &&
              bip32_key_from_seed(seed, sizeof(seed), BIP32_VER_MAIN_PRIVATE, 0, out) == WALLY_OK;
    wally_free_string(mn);                // zeroes the mnemonic before freeing it
    secure_zero(seed, sizeof(seed));
    return ok;
}

bool wallet_load(const SeedEntropy &e) {
    wallet_wipe();
    if (!root_from_entropy(e, "", &root)) {
        secure_zero(&root, sizeof(root));
        return false;
    }
    loaded = true;
    words = seedqr_word_count(e);
    entropy = e;
    return true;
}

bool wallet_set_passphrase(const char *pass) {
    if (!loaded) return false;
    struct ext_key k;
    bool ok = root_from_entropy(entropy, pass, &k);
    if (ok) {
        root = k;                         // replaces the root key
        passphrase = pass[0] != 0;
    }
    secure_zero(&k, sizeof(k));
    return ok;
}

bool wallet_has_passphrase() { return passphrase; }

bool seed_fingerprint_hex(const SeedEntropy &e, char out[9]) {
    out[0] = 0;
    struct ext_key k;
    uint8_t fp[4];
    bool ok = root_from_entropy(e, "", &k) &&
              bip32_key_get_fingerprint(&k, fp, sizeof(fp)) == WALLY_OK;
    secure_zero(&k, sizeof(k));
    if (!ok) return false;
    snprintf(out, 9, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
    return true;
}

void wallet_fingerprint(uint8_t out[4]) {
    memset(out, 0, 4);
    if (loaded) bip32_key_get_fingerprint(&root, out, 4);
}

void wallet_fingerprint_hex(char out[9]) {
    uint8_t fp[4];
    wallet_fingerprint(fp);
    snprintf(out, 9, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
}

int wallet_word_count() { return words; }

bool wallet_entropy(SeedEntropy *out) {
    *out = entropy;
    return loaded;
}

size_t wallet_export_xpub(char *out, size_t cap, bool multisig) {
    if (!loaded || cap == 0) return 0;
    char fp[9];
    wallet_fingerprint_hex(fp);
    const char *coin = testnet ? "1h" : "0h";
    const uint32_t H = BIP32_INITIAL_HARDENED_CHILD;
    // BIP-84 account 0, or BIP-48 account 0 with script type 2 (P2WSH).
    const bool ms = multisig;
    const uint32_t path[4] = {H | (ms ? 48u : 84u), H | (testnet ? 1u : 0u), H | 0, H | 2};
    const size_t path_len = ms ? 4 : 3;

    struct ext_key acct;
    uint8_t ser[BIP32_SERIALIZED_LEN];
    bool ok = bip32_key_from_parent_path(&root, path, path_len, BIP32_FLAG_KEY_PRIVATE, &acct) == WALLY_OK &&
              bip32_key_serialize(&acct, BIP32_FLAG_KEY_PUBLIC, ser, sizeof(ser)) == WALLY_OK;
    secure_zero(&acct, sizeof(acct));     // drop the account private key
    if (!ok) return 0;

    // SLIP-132 version bytes for native segwit: zpub / vpub, multisig Zpub / Vpub.
    const uint32_t ver = ms ? (testnet ? 0x02575483 : 0x02aa7ed3) : (testnet ? 0x045f1cf6 : 0x04b24746);
    ser[0] = ver >> 24; ser[1] = ver >> 16; ser[2] = ver >> 8; ser[3] = ver;
    char *key = nullptr;
    if (wally_base58_from_bytes(ser, sizeof(ser), BASE58_FLAG_CHECKSUM, &key) != WALLY_OK) return 0;
    size_t n = snprintf(out, cap, ms ? "[%s/48h/%s/0h/2h]%s" : "[%s/84h/%s/0h]%s", fp, coin, key);
    wally_free_string(key);
    return n;
}
