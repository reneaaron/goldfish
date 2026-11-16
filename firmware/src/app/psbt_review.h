#pragma once
#include <stddef.h>
#include <stdint.h>

// Review and signing of a single-sig native segwit PSBT with the loaded seed,
// following SeedSigner's checks: every input must be ours, change is only
// "change" if the key at its claimed derivation path really produces that
// output script, and anything claiming our fingerprint that doesn't verify is
// flagged as suspicious.

#define PSBT_MAX_OUTPUTS 8

struct PsbtOutput {
    char address[96];
    uint64_t amount;
    bool is_change;      // verified ours, internal chain (/1/n)
    bool is_self;        // verified ours, receive chain (/0/n): self-transfer
    bool suspicious;     // claims our fingerprint but doesn't verify
    char path[48];       // derivation path for ours, e.g. m/84h/0h/0h/1/3
};

enum PsbtError {
    PSBT_OK = 0,
    PSBT_PARSE_FAILED,
    PSBT_NOT_OUR_SEED,      // inputs signed by a different fingerprint
    PSBT_UNSUPPORTED,       // not single-sig native segwit, or too many outputs
    PSBT_MISSING_AMOUNTS,   // inputs without witness UTXO: fee unknown
    PSBT_SUSPICIOUS,        // a "change" output failed verification
};

struct PsbtReview {
    PsbtError error;
    bool testnet;
    int n_inputs;
    uint64_t input_total;
    uint64_t spend;          // to other people
    uint64_t self;           // to our own receive addresses
    uint64_t change;
    uint64_t fee;
    int n_outputs;
    PsbtOutput outputs[PSBT_MAX_OUTPUTS];
};

// Parse and review raw PSBT bytes against the loaded seed. Keeps the PSBT for
// signing until psbt_clear().
PsbtError psbt_load(const uint8_t *data, size_t len, PsbtReview *out);
// Sign every input we can, if the review passed. Returns the number of
// signatures now on the inputs, so 0 means nothing is signed.
int psbt_sign();
// Serialize the (signed) PSBT. Returns bytes written, 0 if it doesn't fit.
size_t psbt_serialize(uint8_t *out, size_t cap);
void psbt_clear();

const char *psbt_error_text(PsbtError e);
