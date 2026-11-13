#include "wallet.h"
#include <new>
#include <string.h>
#include <stdio.h>
#include <Bitcoin.h>

extern "C" void mnemonic_clear(void);   // uBitcoin's trezor-crypto bip39.c

// The root key lives in this buffer so the whole object can be zeroed after
// its destructor runs, instead of trusting the class to clean up.
alignas(HDPrivateKey) static uint8_t root_mem[sizeof(HDPrivateKey)];
static HDPrivateKey *root;
static int words;
static SeedEntropy entropy;               // kept for the QR backup, wiped with the key
static bool testnet;

bool wallet_testnet() { return testnet; }
void wallet_set_testnet(bool t) { testnet = t; }

bool wallet_has_seed() { return root != nullptr; }
const HDPrivateKey *wallet_root() { return root; }

void wallet_wipe() {
    if (root) {
        root->~HDPrivateKey();
        root = nullptr;
    }
    secure_zero(root_mem, sizeof(root_mem));
    seedqr_wipe(&entropy);
    words = 0;
}

bool wallet_load(const SeedEntropy &e) {
    wallet_wipe();
    const char *mn = mnemonicFromEntropy(e.bytes, e.len);
    if (!mn) return false;
    root = new (root_mem) HDPrivateKey(mn, strlen(mn), "", 0);
    mnemonic_clear();                    // wipes the static mnemonic buffer
    words = seedqr_word_count(e);
    entropy = e;
    return true;
}

bool seed_fingerprint_hex(const SeedEntropy &e, char out[9]) {
    out[0] = 0;
    const char *mn = mnemonicFromEntropy(e.bytes, e.len);
    if (!mn) return false;
    alignas(HDPrivateKey) static uint8_t mem[sizeof(HDPrivateKey)];
    HDPrivateKey *k = new (mem) HDPrivateKey(mn, strlen(mn), "", 0);
    mnemonic_clear();
    uint8_t fp[4];
    k->fingerprint(fp);
    k->~HDPrivateKey();
    secure_zero(mem, sizeof(mem));
    snprintf(out, 9, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
    return true;
}

void wallet_fingerprint(uint8_t out[4]) {
    memset(out, 0, 4);
    if (root) root->fingerprint(out);
}

void wallet_fingerprint_hex(char out[9]) {
    uint8_t fp[4];
    wallet_fingerprint(fp);
    snprintf(out, 9, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
}

int wallet_word_count() { return words; }

bool wallet_entropy(SeedEntropy *out) {
    *out = entropy;
    return root != nullptr;
}

size_t wallet_export_xpub(char *out, size_t cap) {
    if (!root || cap == 0) return 0;
    char fp[9];
    wallet_fingerprint_hex(fp);
    const char *coin = testnet ? "1h" : "0h";
    char path[16];
    snprintf(path, sizeof(path), "m/84h/%s/0h/", coin);
    HDPrivateKey acct = root->derive(path);
    HDPublicKey xpub = acct.xpub();
    xpub.type = P2WPKH;                  // zpub / vpub version bytes
    xpub.network = testnet ? &Testnet : &Mainnet;
    char key[120];
    size_t n = xpub.toString(key, sizeof(key));
    acct = HDPrivateKey();               // drop the account private key
    if (n == 0) return 0;
    return snprintf(out, cap, "[%s/84h/%s/0h]%s", fp, coin, key);
}
