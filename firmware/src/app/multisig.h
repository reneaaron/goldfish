#pragma once
#include <stddef.h>
#include <stdint.h>

// The multisig wallet the loaded seed is a cosigner of (issue #7), from its
// output descriptor: wsh(sortedmulti(M,[fingerprint/path]xpub/<0;1>/*,...)).
// The descriptor is what makes change checkable: a PSBT only says which keys an
// output claims to use, the descriptor says which keys the wallet really has.
// It lives in RAM only, like the seed, and belongs to the root key it was
// checked against: it is not used while another seed, or the same seed with
// another passphrase, is loaded.

#define MS_MAX_KEYS 15                // standardness limit for P2WSH multisig
#define MS_MAX_WITNESS_SCRIPT (3 + 34 * MS_MAX_KEYS)

enum MsResult {
    MS_OK = 0,
    MS_NOT_DESCRIPTOR,    // not an output descriptor at all
    MS_BAD,               // a descriptor, but malformed or its checksum is wrong
    MS_UNSUPPORTED,       // valid, but not wsh(sortedmulti(...)) with key origins
    MS_NOT_OURS,          // the loaded seed is not one of its keys
};

struct MsInfo {
    int m, n;                         // M of N
    bool testnet;                     // from the keys: tpub
    int ours;                         // index of the loaded seed's key
    uint8_t fingerprint[MS_MAX_KEYS][4];
};

// Parse a descriptor and check that the loaded seed's key at the path it
// names really is one of its keys. Replaces the loaded descriptor on MS_OK.
// Accepts /<0;1>/*, /{0,1}/* and /0/* (taken as receive, with change at 1).
MsResult multisig_load(const char *text, size_t len);
// The same from a crypto-output UR's CBOR (BCR-2020-010), as Sparrow shows it.
MsResult multisig_load_ur_output(const uint8_t *cbor, size_t len);
// A descriptor is loaded and belongs to the loaded seed.
bool multisig_loaded();
const MsInfo *multisig_info();        // nullptr unless multisig_loaded()
void multisig_clear();

// True if `path` is our key's origin path followed by chain (0 receive,
// 1 change) and an unhardened index.
bool multisig_path(const uint32_t *path, size_t len, int *chain, uint32_t *index);
// The wallet's P2WSH scriptPubKey (34 bytes) at chain/index. Returns its length, 0 on error.
size_t multisig_script(int chain, uint32_t index, uint8_t *out, size_t cap);
// The witness script behind it. Returns its length, 0 on error.
size_t multisig_witness_script(int chain, uint32_t index, uint8_t *out, size_t cap);
// The address. Returns its length, 0 on error.
size_t multisig_address(int chain, uint32_t index, char *out, size_t cap);
