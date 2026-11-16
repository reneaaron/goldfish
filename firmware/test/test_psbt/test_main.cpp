// PSBT review/signing against SeedSigner's single-frame testnet vector, and
// its native segwit 2-of-3 multisig test wallet.
// Run: pio test -e test
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_crypto.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_transaction.h>
#include <string>
#include "../../src/app/multisig.h"
#include "../../src/app/psbt_review.h"
#include "../../src/app/wallet.h"
#include "../fixtures/multisig_vectors.h"
#include "../fixtures/seedsigner_vectors.h"

static uint8_t raw[4096];
static size_t raw_len;

static void load_seed(const char *mnemonic) {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    TEST_ASSERT_EQUAL(WALLY_OK, bip39_mnemonic_to_bytes(nullptr, mnemonic, e.bytes, sizeof(e.bytes), &e.len));
    TEST_ASSERT_TRUE(e.len == 16 || e.len == 32);
    TEST_ASSERT_TRUE(wallet_load(e));
}

void setUp() {
    multisig_clear();
    TEST_ASSERT_EQUAL(WALLY_OK, wally_base64_to_bytes(SINGLESIG_FRAME, 0, raw, sizeof(raw), &raw_len));
    TEST_ASSERT_GREATER_THAN(0, raw_len);
}

void test_review_matches_seedsigner() {
    load_seed(SINGLESIG_MNEMONIC);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.testnet);
    TEST_ASSERT_EQUAL_UINT64(25000, r.input_total);
    TEST_ASSERT_EQUAL_UINT64(24857, r.spend);     // SeedSigner: spend_amount
    TEST_ASSERT_EQUAL_UINT64(0, r.change);
    TEST_ASSERT_EQUAL_UINT64(143, r.fee);
    TEST_ASSERT_EQUAL(2, r.n_outputs);
}

void test_sign_adds_signature() {
    load_seed(SINGLESIG_MNEMONIC);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(1, psbt_sign());
    static uint8_t out[8192];
    size_t n = psbt_serialize(out, sizeof(out));
    // The signed PSBT, trimmed to the transaction and the signature. The same
    // bytes uBitcoin produced before the move to libwally.
    static const char *SIGNED =
        "70736274ff010072020000000100e8e5ecbed8722b354904c6c161b089afd4e2b571803dc7f6d18d8403fb451a0100000000"
        "fdffffff02983a00000000000017a91456e55911c769436ce00016bd75150d6070f856e68781260000000000001600141a2c"
        "4e2dd096c3460e2936799c7fe7005887326b0000000000220203f665119bd0b9b6fa3e67272bebe59ddf1a7ed627ad0f301d"
        "84b1b03f2d0e9eb547304402202507d0e0d7986bc711575faca13864dca01f29326b1b725d65eae72d08fa487c0220432daa"
        "2df9215e3f0079600c924bd03ce6ea5a588e407fe3e79928e2c7958c1a01000000";
    static char hex[2 * sizeof(out) + 1];
    for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", out[i]);
    TEST_ASSERT_EQUAL_STRING(SIGNED, hex);
    // Signing again changes nothing: still the one signature.
    TEST_ASSERT_EQUAL(1, psbt_sign());
    static uint8_t again[8192];
    TEST_ASSERT_EQUAL(n, psbt_serialize(again, sizeof(again)));
    TEST_ASSERT_EQUAL_MEMORY(out, again, n);
    TEST_ASSERT_EQUAL(0, psbt_serialize(again, n - 1));   // doesn't fit
    psbt_clear();
}

void test_wrong_seed_rejected() {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = 16;                                   // "abandon ... about"
    TEST_ASSERT_TRUE(wallet_load(e));
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_NOT_OUR_SEED, psbt_load(raw, raw_len, &r));
}

void test_garbage_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    uint8_t junk[64];
    memset(junk, 0x42, sizeof(junk));
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(junk, sizeof(junk), &r));
}

// Output 1 of SINGLESIG_FRAME (₿9,857) pays our own m/84h/1h/0h/1/0, but the
// frame has no output metadata, so it's reviewed as a spend. This appends a
// BIP32 derivation to that output's map (the last byte of the PSBT is its
// terminator) with our fingerprint, `path` and the real pubkey at it, and
// points the output script at `hash` (nullptr: the key at `path`).
static const uint32_t H = 0x80000000;

// P2WPKH script and pubkey of the loaded seed's key at `path`.
static void p2wpkh_at(const uint32_t path[5], uint8_t pub[33], uint8_t script[22]) {
    struct ext_key k;
    TEST_ASSERT_EQUAL(WALLY_OK, bip32_key_from_parent_path(wallet_root(), path, 5, BIP32_FLAG_KEY_PRIVATE, &k));
    memcpy(pub, k.pub_key, 33);
    script[0] = 0x00;
    script[1] = 20;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_hash160(k.pub_key, 33, script + 2, 20));
}

// Needs the seed loaded.
static void claim_output(const uint32_t path[5], const uint8_t *hash = nullptr) {
    const uint32_t change0[] = {H + 84, H + 1, H, 1, 0};
    uint8_t pub[33], was[22], now[22];
    p2wpkh_at(change0, pub, was);
    p2wpkh_at(path, pub, now);

    uint8_t *spk = nullptr;                       // in the unsigned tx
    for (size_t i = 0; i + sizeof(was) <= raw_len && !spk; i++)
        if (memcmp(raw + i, was, sizeof(was)) == 0) spk = raw + i;
    TEST_ASSERT_NOT_NULL(spk);
    memcpy(spk + 2, hash ? hash : now + 2, 20);

    uint8_t *p = raw + raw_len - 1;               // output 1's map terminator
    TEST_ASSERT_EQUAL(0x00, *p);
    *p++ = 34;                                    // key: type + compressed pubkey
    *p++ = 0x02;                                  // PSBT_OUT_BIP32_DERIVATION
    memcpy(p, pub, 33);
    p += 33;
    *p++ = 4 + 5 * 4;                             // value: fingerprint + path
    wallet_fingerprint(p);
    p += 4;
    for (int i = 0; i < 5; i++)
        for (int b = 0; b < 4; b++) *p++ = uint8_t(path[i] >> (8 * b));   // little-endian
    *p++ = 0x00;
    raw_len = p - raw;
}

static void expect_suspicious() {
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_SUSPICIOUS, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.outputs[1].suspicious);
    TEST_ASSERT_FALSE(r.outputs[1].is_change);
    TEST_ASSERT_FALSE(r.outputs[1].is_self);
    TEST_ASSERT_EQUAL_UINT64(0, r.change);
    TEST_ASSERT_EQUAL_UINT64(0, r.self);
}

// Our real pubkey and path attached to an output that pays someone else.
void test_foreign_script_with_our_derivation_is_suspicious() {
    load_seed(SINGLESIG_MNEMONIC);
    const uint32_t path[] = {H + 84, H + 1, H, 1, 0};
    uint8_t foreign[20];
    memset(foreign, 0x11, sizeof(foreign));       // P2WPKH of a key we don't have
    claim_output(path, foreign);
    expect_suspicious();
}

void test_real_change_is_change() {
    load_seed(SINGLESIG_MNEMONIC);
    const uint32_t path[] = {H + 84, H + 1, H, 1, 0};
    claim_output(path);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.outputs[1].is_change);
    TEST_ASSERT_FALSE(r.outputs[1].suspicious);
    TEST_ASSERT_EQUAL_STRING("m/84h/1h/0h/1/0", r.outputs[1].path);
    TEST_ASSERT_EQUAL_UINT64(9857, r.change);
    TEST_ASSERT_EQUAL_UINT64(15000, r.spend);
    TEST_ASSERT_EQUAL_UINT64(143, r.fee);
}

void test_receive_address_is_self() {
    load_seed(SINGLESIG_MNEMONIC);
    const uint32_t path[] = {H + 84, H + 1, H, 0, 3};
    claim_output(path);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.outputs[1].is_self);
    TEST_ASSERT_FALSE(r.outputs[1].is_change);
    TEST_ASSERT_EQUAL_STRING("m/84h/1h/0h/0/3", r.outputs[1].path);
    TEST_ASSERT_EQUAL_UINT64(9857, r.self);
    TEST_ASSERT_EQUAL_UINT64(0, r.change);
    TEST_ASSERT_EQUAL_UINT64(15000, r.spend);
}

// Really our key and script, but at paths a BIP84 wallet never scans.
void test_our_key_at_odd_path_is_suspicious() {
    const uint32_t paths[][5] = {
        {H + 84, H + 1, H, 2, 0},                 // not receive or change
        {H + 84, H + 1, H, 1, H + 0},             // hardened index
        {H + 84, H + 0, H, 1, 0},                 // mainnet coin, testnet inputs
        {H + 44, H + 1, H, 1, 0},                 // not BIP84
        {H + 84, H + 1, 0, 1, 0},                 // unhardened account
    };
    for (const auto &path : paths) {
        setUp();
        load_seed(SINGLESIG_MNEMONIC);
        claim_output(path);
        expect_suspicious();
    }
}

// Re-serializes the PSBT in `raw` after changing it with libwally.
static void edit_psbt(void (*edit)(struct wally_psbt *)) {
    struct wally_psbt *p = nullptr;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_from_bytes(raw, raw_len, 0, &p));
    edit(p);
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_to_bytes(p, 0, raw, sizeof(raw), &raw_len));
    wally_psbt_free(p);
}

// A sighash other than ALL would leave outputs out of the signature: what the
// screen showed is then not what was signed.
void test_other_sighash_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    PsbtReview r;
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_set_input_sighash(p, 0, WALLY_SIGHASH_NONE));
    });
    TEST_ASSERT_EQUAL(PSBT_UNSUPPORTED, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(0, psbt_sign());

    setUp();
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK,
                          wally_psbt_set_input_sighash(p, 0, WALLY_SIGHASH_ALL | WALLY_SIGHASH_ANYONECANPAY));
    });
    TEST_ASSERT_EQUAL(PSBT_UNSUPPORTED, psbt_load(raw, raw_len, &r));

    setUp();                                      // ALL, spelled out, is fine
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_set_input_sighash(p, 0, WALLY_SIGHASH_ALL));
    });
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(1, psbt_sign());
    psbt_clear();
}

// A PSBT that already carries our signature still counts as signed.
void test_already_signed() {
    load_seed(SINGLESIG_MNEMONIC);
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_sign_bip32(p, wallet_root(), EC_FLAG_GRIND_R));
        TEST_ASSERT_EQUAL(1, p->inputs[0].signatures.num_items);
    });
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(1, psbt_sign());
    static uint8_t out[8192];
    TEST_ASSERT_GREATER_THAN(0, psbt_serialize(out, sizeof(out)));
    psbt_clear();
}

// The same transaction as PSBT v2: amounts and scripts live in the output maps.
void test_psbt_v2() {
    load_seed(SINGLESIG_MNEMONIC);
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_set_version(p, 0, WALLY_PSBT_VERSION_2));
    });
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.testnet);
    TEST_ASSERT_EQUAL_UINT64(25000, r.input_total);
    TEST_ASSERT_EQUAL_UINT64(24857, r.spend);
    TEST_ASSERT_EQUAL_UINT64(143, r.fee);
    TEST_ASSERT_EQUAL(2, r.n_outputs);
    TEST_ASSERT_EQUAL_UINT64(15000, r.outputs[0].amount);
    TEST_ASSERT_EQUAL_STRING("2N1Agr9voB5g4BhaTihLCuGFTFbbPBHVt7T", r.outputs[0].address);
    TEST_ASSERT_EQUAL_STRING("tb1qrgkyutwsjmp5vr3fxeuecll8qpvgwvnth2wa5k", r.outputs[1].address);
    TEST_ASSERT_EQUAL(1, psbt_sign());
    psbt_clear();
}

// An input that brings the whole previous transaction instead of a witness
// UTXO: the amount is in there, and libwally signs it the same way.
void test_non_witness_utxo_input() {
    load_seed(SINGLESIG_MNEMONIC);
    const uint32_t path[] = {H + 84, H + 1, H, 0, 0};
    uint8_t pub[33], ours[22], theirs[22] = {0x00, 20};
    p2wpkh_at(path, pub, ours);
    memset(theirs + 2, 0x11, 20);

    struct wally_tx *prev = nullptr, *tx = nullptr;
    uint8_t txid[32] = {0};
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_init_alloc(2, 0, 1, 1, &prev));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_input(prev, txid, 32, 0, 0xfffffffd, nullptr, 0, nullptr, 0));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_output(prev, 50000, ours, sizeof(ours), 0));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_get_txid(prev, txid, sizeof(txid)));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_init_alloc(2, 0, 1, 1, &tx));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_input(tx, txid, 32, 0, 0xfffffffd, nullptr, 0, nullptr, 0));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_output(tx, 49000, theirs, sizeof(theirs), 0));

    struct wally_psbt *p = nullptr;
    uint8_t fp[4];
    wallet_fingerprint(fp);
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_from_tx(tx, 0, 0, &p));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_set_input_utxo(p, 0, prev));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_add_input_keypath(p, 0, pub, sizeof(pub), fp, sizeof(fp), path, 5));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_to_bytes(p, 0, raw, sizeof(raw), &raw_len));
    wally_psbt_free(p);
    wally_tx_free(tx);
    wally_tx_free(prev);

    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL_UINT64(50000, r.input_total);
    TEST_ASSERT_EQUAL_UINT64(49000, r.spend);
    TEST_ASSERT_EQUAL_UINT64(1000, r.fee);
    TEST_ASSERT_EQUAL(1, psbt_sign());
    psbt_clear();
}

// ------------------------------------------------------------ multisig
// 2-of-3 wsh(sortedmulti): 100,000,000 in; change 99,649,000 at 1/4, a
// self-transfer of 100,000 at 0/2, 250,000 to somebody else; fee 1,000.
static void load_multisig(const char *psbt_b64, const char *mnemonic = MS_MNEMONIC, bool descriptor = true) {
    load_seed(mnemonic);
    if (descriptor) TEST_ASSERT_EQUAL(MS_OK, multisig_load(MS_DESCRIPTOR, strlen(MS_DESCRIPTOR)));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_base64_to_bytes(psbt_b64, 0, raw, sizeof(raw), &raw_len));
}

void test_multisig_review() {
    load_multisig(MS_PSBT);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(2, r.ms_m);
    TEST_ASSERT_EQUAL(3, r.ms_n);
    TEST_ASSERT_TRUE(r.testnet);
    TEST_ASSERT_EQUAL_UINT64(100000000, r.input_total);
    TEST_ASSERT_EQUAL_UINT64(99649000, r.change);
    TEST_ASSERT_EQUAL_UINT64(100000, r.self);
    TEST_ASSERT_EQUAL_UINT64(250000, r.spend);
    TEST_ASSERT_EQUAL_UINT64(1000, r.fee);
    TEST_ASSERT_EQUAL(3, r.n_outputs);
    TEST_ASSERT_TRUE(r.outputs[0].is_change);
    TEST_ASSERT_EQUAL_STRING(MS_ADDR_CHANGE_4, r.outputs[0].address);
    TEST_ASSERT_EQUAL_STRING("m/48h/1h/0h/2h/1/4", r.outputs[0].path);
    TEST_ASSERT_TRUE(r.outputs[1].is_self);
    TEST_ASSERT_FALSE(r.outputs[2].is_change || r.outputs[2].is_self || r.outputs[2].suspicious);
    psbt_clear();
}

// Without the descriptor nothing in the PSBT can be checked: no review, no signature.
void test_multisig_needs_descriptor() {
    load_multisig(MS_PSBT, MS_MNEMONIC, false);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_NEEDS_DESCRIPTOR, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(0, psbt_sign());
    psbt_clear();
}

// Is `der_sig` (DER plus sighash byte, hex) a signature by `pubkey` over input 0 of `p`?
static bool signs_input(struct wally_psbt *p, const uint8_t *pubkey, const uint8_t *der_sig, size_t der_len) {
    struct wally_tx *tx = nullptr;
    uint8_t hash[32], compact[EC_SIGNATURE_LEN], ws[MS_MAX_WITNESS_SCRIPT];
    size_t ws_len = 0;
    bool ok = der_len > 1 && der_sig[der_len - 1] == WALLY_SIGHASH_ALL &&
              wally_psbt_get_global_tx_alloc(p, &tx) == WALLY_OK &&
              wally_psbt_get_input_witness_script(p, 0, ws, sizeof(ws), &ws_len) == WALLY_OK && ws_len <= sizeof(ws) &&
              wally_psbt_get_input_signature_hash(p, 0, tx, ws, ws_len, 0, hash, sizeof(hash)) == WALLY_OK &&
              wally_ec_sig_from_der(der_sig, der_len - 1, compact, sizeof(compact)) == WALLY_OK &&
              wally_ec_sig_verify(pubkey, EC_PUBLIC_KEY_LEN, hash, sizeof(hash), EC_FLAG_ECDSA, compact,
                                  sizeof(compact)) == WALLY_OK;
    if (tx) wally_tx_free(tx);
    return ok;
}

// Our signature is valid for the transaction, over the same hash embit signed.
void test_multisig_sign() {
    load_multisig(MS_PSBT);
    struct wally_psbt *before = nullptr, *after = nullptr;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_from_bytes(raw, raw_len, 0, &before));
    uint8_t pub[EC_PUBLIC_KEY_LEN], embit_sig[80];
    size_t n = 0, embit_len = 0;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_hex_to_bytes(MS_SIG_PUBKEY, pub, sizeof(pub), &n));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_hex_to_bytes(MS_SIG, embit_sig, sizeof(embit_sig), &embit_len));
    TEST_ASSERT_TRUE(signs_input(before, pub, embit_sig, embit_len));

    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(1, psbt_sign());
    static uint8_t out[8192];
    size_t out_len = psbt_serialize(out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, out_len);
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_from_bytes(out, out_len, 0, &after));
    TEST_ASSERT_EQUAL(1, after->inputs[0].signatures.num_items);
    const struct wally_map_item &sig = after->inputs[0].signatures.items[0];
    TEST_ASSERT_EQUAL(EC_PUBLIC_KEY_LEN, sig.key_len);
    TEST_ASSERT_EQUAL_MEMORY(pub, sig.key, EC_PUBLIC_KEY_LEN);
    TEST_ASSERT_TRUE(signs_input(before, pub, sig.value, sig.value_len));
    wally_psbt_free(before);
    wally_psbt_free(after);
    psbt_clear();
}

// A cosigner signed first: ours is added, theirs is kept.
void test_multisig_second_signature() {
    load_multisig(MS_PSBT_COSIGNED);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(1, psbt_sign());
    static uint8_t out[8192];
    size_t out_len = psbt_serialize(out, sizeof(out));
    struct wally_psbt *after = nullptr;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_from_bytes(out, out_len, 0, &after));
    TEST_ASSERT_EQUAL(2, after->inputs[0].signatures.num_items);
    wally_psbt_free(after);
    psbt_clear();
}

// The other cosigner's seed reviews and signs the same transaction.
void test_multisig_other_cosigner() {
    load_multisig(MS_PSBT, MS_MNEMONIC_2);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL_UINT64(99649000, r.change);
    TEST_ASSERT_EQUAL(1, psbt_sign());
    psbt_clear();
}

// Change that keeps the wallet's key paths and witness script but pays another script.
void test_multisig_fake_change() {
    load_multisig(MS_PSBT_TAMPERED);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_SUSPICIOUS, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_TRUE(r.outputs[0].suspicious);
    TEST_ASSERT_FALSE(r.outputs[0].is_change);
    TEST_ASSERT_EQUAL_UINT64(0, r.change);
    TEST_ASSERT_EQUAL(0, psbt_sign());
    psbt_clear();
}

// The same keys as a 3-of-3 are a different wallet: its scripts differ.
void test_multisig_wrong_wallet() {
    load_seed(MS_MNEMONIC);
    std::string d = MS_DESCRIPTOR;
    d = d.substr(0, d.find('#'));
    d.replace(d.find("(2,"), 3, "(3,");
    TEST_ASSERT_EQUAL(MS_OK, multisig_load(d.c_str(), d.size()));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_base64_to_bytes(MS_PSBT, 0, raw, sizeof(raw), &raw_len));
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_WRONG_WALLET, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(0, psbt_sign());
    psbt_clear();
}

// A single-sig transaction is reviewed as before with a descriptor loaded.
void test_single_sig_with_descriptor_loaded() {
    load_seed(MS_MNEMONIC);
    TEST_ASSERT_EQUAL(MS_OK, multisig_load(MS_DESCRIPTOR, strlen(MS_DESCRIPTOR)));
    load_seed(SINGLESIG_MNEMONIC);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_OK, psbt_load(raw, raw_len, &r));
    TEST_ASSERT_EQUAL(0, r.ms_m);
    TEST_ASSERT_EQUAL_UINT64(24857, r.spend);
    psbt_clear();
}

// Our fingerprint, path and real pubkey on an input whose script pays someone
// else: not our coin, so the whole transaction is refused (not signed).
void test_input_with_foreign_script_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    const uint32_t path[] = {H + 84, H + 1, H, 0, 0};
    uint8_t pub[33], ours[22], theirs[22] = {0x00, 20};
    p2wpkh_at(path, pub, ours);
    memset(theirs + 2, 0x11, 20);

    struct wally_tx *tx = nullptr;
    uint8_t txid[32];
    memset(txid, 0x22, sizeof(txid));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_init_alloc(2, 0, 1, 1, &tx));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_input(tx, txid, 32, 0, 0xfffffffd, nullptr, 0, nullptr, 0));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_output(tx, 49000, theirs, sizeof(theirs), 0));

    struct wally_psbt *p = nullptr;
    struct wally_tx_output utxo;
    uint8_t fp[4];
    wallet_fingerprint(fp);
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_from_tx(tx, 0, 0, &p));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_output_init(50000, theirs, sizeof(theirs), &utxo));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_set_input_witness_utxo(p, 0, &utxo));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_add_input_keypath(p, 0, pub, sizeof(pub), fp, sizeof(fp), path, 5));
    TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_to_bytes(p, 0, raw, sizeof(raw), &raw_len));
    wally_psbt_free(p);
    wally_tx_free(tx);

    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_NOT_OUR_SEED, psbt_load(raw, raw_len, &r));
}

// A transaction without outputs isn't valid: refused at review, not at signing.
void test_no_outputs_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    edit_psbt([](struct wally_psbt *p) {
        while (p->num_outputs) TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_remove_output(p, 0));
    });
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(raw, raw_len, &r));
}

// A finalized input drops partial signatures when serialized: the signed QR
// would carry no signature from us. Written into the raw bytes, since libwally
// strips the key paths of an input it finalizes itself.
void test_finalized_input_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    size_t at = 5;                                // after "psbt\xff": walk the global map
    while (raw[at] != 0x00) {
        TEST_ASSERT_TRUE(raw[at] < 0xfd);         // single-byte key length
        at += 1 + raw[at];
        size_t vlen = raw[at], skip = 1;
        if (vlen == 0xfd) { vlen = raw[at + 1] | (raw[at + 2] << 8); skip = 3; }
        at += skip + vlen;
    }
    at++;                                         // input 0's map starts here
    const uint8_t final_scriptsig[] = {0x01, 0x07, 0x01, 0x51};   // PSBT_IN_FINAL_SCRIPTSIG = OP_1
    memmove(raw + at + sizeof(final_scriptsig), raw + at, raw_len - at);
    memcpy(raw + at, final_scriptsig, sizeof(final_scriptsig));
    raw_len += sizeof(final_scriptsig);
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(raw, raw_len, &r));
}

// A full previous transaction that isn't the one this input spends: the
// amount shown would come from the witness UTXO, signing from the other copy.
void test_mismatched_previous_tx_rejected() {
    load_seed(SINGLESIG_MNEMONIC);
    edit_psbt([](struct wally_psbt *p) {
        struct wally_tx *prev = nullptr;
        uint8_t txid[32] = {0}, script[22] = {0x00, 20};
        TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_init_alloc(2, 0, 1, 1, &prev));
        TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_input(prev, txid, 32, 0, 0xfffffffd, nullptr, 0, nullptr, 0));
        TEST_ASSERT_EQUAL(WALLY_OK, wally_tx_add_raw_output(prev, 300000, script, sizeof(script), 0));
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_input_set_utxo(&p->inputs[0], prev));
        wally_tx_free(prev);
    });
    PsbtReview r;
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(raw, raw_len, &r));
}

// Scripts a native single-sig input never has: libwally would sign with them,
// so signing would fail (or sign something else) after review passed.
void test_extra_input_scripts_rejected() {
    static const uint8_t script[] = {0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    static uint8_t saved[4096];
    static size_t saved_len;
    load_seed(SINGLESIG_MNEMONIC);
    memcpy(saved, raw, raw_len);
    saved_len = raw_len;
    PsbtReview r;
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_input_set_redeem_script(&p->inputs[0], script, sizeof(script)));
    });
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(raw, raw_len, &r));
    memcpy(raw, saved, saved_len);
    raw_len = saved_len;
    edit_psbt([](struct wally_psbt *p) {
        TEST_ASSERT_EQUAL(WALLY_OK, wally_psbt_input_set_witness_script(&p->inputs[0], script, sizeof(script)));
    });
    TEST_ASSERT_EQUAL(PSBT_PARSE_FAILED, psbt_load(raw, raw_len, &r));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_review_matches_seedsigner);
    RUN_TEST(test_sign_adds_signature);
    RUN_TEST(test_wrong_seed_rejected);
    RUN_TEST(test_garbage_rejected);
    RUN_TEST(test_foreign_script_with_our_derivation_is_suspicious);
    RUN_TEST(test_real_change_is_change);
    RUN_TEST(test_receive_address_is_self);
    RUN_TEST(test_our_key_at_odd_path_is_suspicious);
    RUN_TEST(test_other_sighash_rejected);
    RUN_TEST(test_already_signed);
    RUN_TEST(test_psbt_v2);
    RUN_TEST(test_non_witness_utxo_input);
    RUN_TEST(test_multisig_review);
    RUN_TEST(test_multisig_needs_descriptor);
    RUN_TEST(test_multisig_sign);
    RUN_TEST(test_multisig_second_signature);
    RUN_TEST(test_multisig_other_cosigner);
    RUN_TEST(test_multisig_fake_change);
    RUN_TEST(test_multisig_wrong_wallet);
    RUN_TEST(test_single_sig_with_descriptor_loaded);
    RUN_TEST(test_input_with_foreign_script_rejected);
    RUN_TEST(test_no_outputs_rejected);
    RUN_TEST(test_finalized_input_rejected);
    RUN_TEST(test_mismatched_previous_tx_rejected);
    RUN_TEST(test_extra_input_scripts_rejected);
    return UNITY_END();
}
