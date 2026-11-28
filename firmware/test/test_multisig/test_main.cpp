// Multisig descriptor: SeedSigner's native segwit 2-of-3 test wallet, with
// addresses and scripts worked out by embit.
// Run: pio test -e test
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <wally_core.h>
#include <wally_bip39.h>
#include "../../src/app/multisig.h"
#include "../../src/app/ur.h"
#include "../../src/app/wallet.h"
#include "../fixtures/multisig_vectors.h"

static void load_seed(const char *mnemonic) {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    TEST_ASSERT_EQUAL(WALLY_OK, bip39_mnemonic_to_bytes(nullptr, mnemonic, e.bytes, sizeof(e.bytes), &e.len));
    TEST_ASSERT_TRUE(wallet_load(e));
}

static MsResult load(const std::string &s) { return multisig_load(s.c_str(), s.size()); }

static std::string replace_all(std::string s, const std::string &from, const std::string &to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

static std::string no_checksum() {
    std::string s = MS_DESCRIPTOR;
    return s.substr(0, s.find('#'));
}

static void expect_address(const char *want, int chain, uint32_t index) {
    char a[96];
    TEST_ASSERT_GREATER_THAN(0, multisig_address(chain, index, a, sizeof(a)));
    TEST_ASSERT_EQUAL_STRING(want, a);
}

void setUp() {
    multisig_clear();
    load_seed(MS_MNEMONIC);
}

void test_load() {
    TEST_ASSERT_FALSE(multisig_loaded());
    TEST_ASSERT_NULL(multisig_info());
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    const MsInfo *i = multisig_info();
    TEST_ASSERT_NOT_NULL(i);
    TEST_ASSERT_EQUAL(2, i->m);
    TEST_ASSERT_EQUAL(3, i->n);
    TEST_ASSERT_TRUE(i->testnet);
    TEST_ASSERT_EQUAL(0, i->ours);
    const uint8_t fps[3][4] = {{0x0f, 0xb8, 0x82, 0xff}, {0x03, 0xcd, 0x0a, 0x2b}, {0x0f, 0x88, 0x90, 0x44}};
    TEST_ASSERT_EQUAL_MEMORY(fps, i->fingerprint, sizeof(fps));
}

void test_addresses() {
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    expect_address(MS_ADDR_RECEIVE_0, 0, 0);
    expect_address(MS_ADDR_RECEIVE_4, 0, 4);
    expect_address(MS_ADDR_CHANGE_0, 1, 0);
    expect_address(MS_ADDR_CHANGE_4, 1, 4);
    char a[96];
    TEST_ASSERT_EQUAL(0, multisig_address(2, 0, a, sizeof(a)));
    TEST_ASSERT_EQUAL(0, multisig_address(0, 0x80000000u, a, sizeof(a)));
}

void test_witness_script() {
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    uint8_t ws[MS_MAX_WITNESS_SCRIPT], want[MS_MAX_WITNESS_SCRIPT], spk[34];
    size_t want_len = 0;
    TEST_ASSERT_EQUAL(WALLY_OK, wally_hex_to_bytes(MS_WITNESS_SCRIPT_CHANGE_4, want, sizeof(want), &want_len));
    TEST_ASSERT_EQUAL(want_len, multisig_witness_script(1, 4, ws, sizeof(ws)));
    TEST_ASSERT_EQUAL_MEMORY(want, ws, want_len);
    TEST_ASSERT_EQUAL(34, multisig_script(1, 4, spk, sizeof(spk)));
    TEST_ASSERT_EQUAL_HEX8(0x00, spk[0]);
    TEST_ASSERT_EQUAL_HEX8(0x20, spk[1]);
}

void test_path() {
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    const uint32_t H = 0x80000000;
    int chain = -1;
    uint32_t index = 99;
    const uint32_t ok[] = {H + 48, H + 1, H, H + 2, 1, 4};
    TEST_ASSERT_TRUE(multisig_path(ok, 6, &chain, &index));
    TEST_ASSERT_EQUAL(1, chain);
    TEST_ASSERT_EQUAL(4, index);
    const uint32_t other_account[] = {H + 48, H + 1, H + 1, H + 2, 1, 4};
    const uint32_t chain2[] = {H + 48, H + 1, H, H + 2, 2, 4};
    const uint32_t hardened[] = {H + 48, H + 1, H, H + 2, 1, H + 4};
    TEST_ASSERT_FALSE(multisig_path(other_account, 6, &chain, &index));
    TEST_ASSERT_FALSE(multisig_path(chain2, 6, &chain, &index));
    TEST_ASSERT_FALSE(multisig_path(hardened, 6, &chain, &index));
    TEST_ASSERT_FALSE(multisig_path(ok, 5, &chain, &index));
}

// The same wallet however the coordinator writes the two chains.
void test_spellings() {
    for (const char *form : {"/0/*", "/{0,1}/*"}) {
        multisig_clear();
        TEST_ASSERT_EQUAL(MS_OK, load(replace_all(no_checksum(), "/<0;1>/*", form)));
        expect_address(MS_ADDR_RECEIVE_4, 0, 4);
        expect_address(MS_ADDR_CHANGE_4, 1, 4);
    }
    multisig_clear();
    TEST_ASSERT_EQUAL(MS_OK, load("  " + replace_all(no_checksum(), "h/", "'/") + "\n"));
    expect_address(MS_ADDR_CHANGE_4, 1, 4);
}

void test_bad_checksum() {
    std::string s = MS_DESCRIPTOR;
    s[s.size() - 1] = s[s.size() - 1] == 'q' ? 'p' : 'q';
    TEST_ASSERT_EQUAL(MS_BAD, load(s));
    // also when the checksum belongs to a /0/* descriptor
    TEST_ASSERT_EQUAL(MS_BAD, load(replace_all(no_checksum(), "/<0;1>/*", "/0/*") + "#qqqqqqqq"));
    TEST_ASSERT_FALSE(multisig_loaded());
}

void test_not_descriptor() {
    TEST_ASSERT_EQUAL(MS_NOT_DESCRIPTOR, load("tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl"));
    TEST_ASSERT_EQUAL(MS_NOT_DESCRIPTOR, load(""));
    TEST_ASSERT_EQUAL(MS_BAD, load("wsh(sortedmulti(2,"));
}

void test_unsupported() {
    TEST_ASSERT_EQUAL(MS_UNSUPPORTED, load(replace_all(no_checksum(), "sortedmulti", "multi")));
    TEST_ASSERT_EQUAL(MS_UNSUPPORTED, load("sh(" + no_checksum() + ")"));
    TEST_ASSERT_EQUAL(MS_UNSUPPORTED, load(replace_all(no_checksum(), "/<0;1>/*", "/*")));
    TEST_ASSERT_EQUAL(MS_BAD, load(replace_all(no_checksum(), "sortedmulti(2,", "sortedmulti(4,")));   // 4 of 3
    TEST_ASSERT_FALSE(multisig_loaded());
}

// Keys without [fingerprint/path]: refused, with their own reason. Ours with
// fingerprint 00000000 can't be found; a cosigner's is no problem.
void test_no_origin() {
    std::string d = no_checksum();
    std::string bare = d;
    size_t at = bare.find("[03cd0a2b/48h/1h/0h/2h]");
    bare.erase(at, strlen("[03cd0a2b/48h/1h/0h/2h]"));
    TEST_ASSERT_EQUAL(MS_NO_ORIGIN, load(bare));
    TEST_ASSERT_EQUAL(MS_NO_ORIGIN, load(replace_all(d, "[0fb882ff/", "[00000000/")));
    TEST_ASSERT_FALSE(multisig_loaded());
    TEST_ASSERT_EQUAL(MS_OK, load(replace_all(d, "[03cd0a2b/", "[00000000/")));
}

void test_not_ours() {
    SeedEntropy e;
    memset(&e, 0, sizeof(e));
    e.len = 16;                                   // "abandon ... about"
    TEST_ASSERT_TRUE(wallet_load(e));
    TEST_ASSERT_EQUAL(MS_NOT_OURS, load(MS_DESCRIPTOR));
}

// Our fingerprint on somebody else's key: not our wallet.
void test_our_fingerprint_on_another_key() {
    TEST_ASSERT_EQUAL(MS_NOT_OURS, load(replace_all(replace_all(no_checksum(), "0fb882ff", "00000000"), "03cd0a2b", "0fb882ff")));
}

void test_same_key_twice() {
    std::string s = no_checksum(), first = s.substr(s.find('['), s.find(',', s.find('[')) - s.find('['));
    TEST_ASSERT_EQUAL(MS_BAD, load(s.substr(0, s.size() - 2) + "," + first + "))"));
}

// A descriptor belongs to the seed it was loaded with.
void test_dropped_with_the_seed() {
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    TEST_ASSERT_TRUE(wallet_set_passphrase("x"));
    TEST_ASSERT_FALSE(multisig_loaded());
    char a[96];
    TEST_ASSERT_EQUAL(0, multisig_address(0, 0, a, sizeof(a)));
    TEST_ASSERT_TRUE(wallet_set_passphrase(""));
    TEST_ASSERT_TRUE(multisig_loaded());
    load_seed(MS_MNEMONIC_2);                     // another cosigner: still not the seed it was checked against
    TEST_ASSERT_FALSE(multisig_loaded());
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    TEST_ASSERT_EQUAL(1, multisig_info()->ours);
}

static UrDecoder dec;

void test_ur_crypto_output() {
    const uint8_t *payload = nullptr;
    size_t len = 0;
    ur_decoder_reset(&dec);
    TEST_ASSERT_EQUAL(UR_COMPLETE, ur_decoder_add(&dec, MS_UR_OUTPUT, strlen(MS_UR_OUTPUT), &payload, &len));
    TEST_ASSERT_EQUAL(UR_TYPE_OUTPUT, dec.type);
    TEST_ASSERT_EQUAL(MS_OK, multisig_load_ur_output(payload, len));
    TEST_ASSERT_EQUAL(2, multisig_info()->m);
    TEST_ASSERT_EQUAL(3, multisig_info()->n);
    TEST_ASSERT_TRUE(multisig_info()->testnet);
    expect_address(MS_ADDR_RECEIVE_0, 0, 0);
    expect_address(MS_ADDR_CHANGE_4, 1, 4);

    // Truncated or padded CBOR is refused, wherever it is cut.
    static uint8_t copy[1024];
    TEST_ASSERT_LESS_THAN(sizeof(copy), len);
    memcpy(copy, payload, len);
    multisig_clear();
    for (size_t n = 0; n < len; n++) TEST_ASSERT_NOT_EQUAL(MS_OK, multisig_load_ur_output(copy, n));
    copy[len] = 0;
    TEST_ASSERT_NOT_EQUAL(MS_OK, multisig_load_ur_output(copy, len + 1));
    TEST_ASSERT_FALSE(multisig_loaded());
}

// Sparrow leaves the children out of each key: read as <0;1>/*.
void test_ur_crypto_output_sparrow() {
    const uint8_t *payload = nullptr;
    size_t len = 0;
    load_seed(SPARROW_MNEMONIC);
    ur_decoder_reset(&dec);
    TEST_ASSERT_EQUAL(UR_COMPLETE, ur_decoder_add(&dec, SPARROW_UR_OUTPUT, strlen(SPARROW_UR_OUTPUT), &payload, &len));
    TEST_ASSERT_EQUAL(UR_TYPE_OUTPUT, dec.type);
    TEST_ASSERT_EQUAL(MS_OK, multisig_load_ur_output(payload, len));
    TEST_ASSERT_EQUAL(2, multisig_info()->m);
    TEST_ASSERT_EQUAL(3, multisig_info()->n);
    TEST_ASSERT_EQUAL(2, multisig_info()->ours);
    TEST_ASSERT_TRUE(multisig_info()->testnet);
    expect_address(SPARROW_ADDR_RECEIVE_0, 0, 0);
    expect_address(SPARROW_ADDR_CHANGE_4, 1, 4);
    multisig_clear();
}

// One wallet at a time: with replace = false a loaded wallet stays, whatever is scanned.
void test_no_replace() {
    TEST_ASSERT_EQUAL(MS_OK, multisig_load(MS_DESCRIPTOR, strlen(MS_DESCRIPTOR), false));   // nothing loaded yet
    std::string same = replace_all(no_checksum(), "<0;1>", "0");                             // another spelling
    TEST_ASSERT_EQUAL(MS_SAME, multisig_load(same.c_str(), same.size(), false));
    std::string other = replace_all(no_checksum(), "sortedmulti(2,", "sortedmulti(1,");      // a 1-of-3
    TEST_ASSERT_EQUAL(MS_IN_USE, multisig_load(other.c_str(), other.size(), false));
    TEST_ASSERT_EQUAL(2, multisig_info()->m);                                                // kept
    std::string theirs = replace_all(no_checksum(), "0fb882ff", "0fb882fe");                 // not ours: says so first
    TEST_ASSERT_EQUAL(MS_NOT_OURS, multisig_load(theirs.c_str(), theirs.size(), false));
    TEST_ASSERT_EQUAL(MS_OK, load(other));                                                   // replace = true, as before
    TEST_ASSERT_EQUAL(1, multisig_info()->m);
}

// Our key in the wallet: the one at the descriptor's path, as Connect wallet's multisig key.
void test_export_key() {
    char out[200], want[200];
    TEST_ASSERT_EQUAL(0, multisig_export_key(out, sizeof(out)));   // no wallet
    TEST_ASSERT_EQUAL(MS_OK, load(MS_DESCRIPTOR));
    TEST_ASSERT_GREATER_THAN(0, multisig_export_key(out, sizeof(out)));
    wallet_set_testnet(true);
    wallet_export_xpub(want, sizeof(want), true);
    wallet_set_testnet(false);
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_STRING_LEN("[0fb882ff/48h/1h/0h/2h]Vpub", out, 27);
    TEST_ASSERT_EQUAL(0, multisig_export_key(out, 40));            // doesn't fit: nothing
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_load);
    RUN_TEST(test_addresses);
    RUN_TEST(test_witness_script);
    RUN_TEST(test_path);
    RUN_TEST(test_spellings);
    RUN_TEST(test_bad_checksum);
    RUN_TEST(test_not_descriptor);
    RUN_TEST(test_unsupported);
    RUN_TEST(test_no_origin);
    RUN_TEST(test_not_ours);
    RUN_TEST(test_our_fingerprint_on_another_key);
    RUN_TEST(test_same_key_twice);
    RUN_TEST(test_dropped_with_the_seed);
    RUN_TEST(test_ur_crypto_output);
    RUN_TEST(test_ur_crypto_output_sparrow);
    RUN_TEST(test_no_replace);
    RUN_TEST(test_export_key);
    return UNITY_END();
}
