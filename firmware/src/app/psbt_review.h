#pragma once
#include <stddef.h>
#include <stdint.h>

// Review and signing of a native segwit PSBT with the loaded seed, single-sig
// or wsh(sortedmulti) multisig, following SeedSigner's checks: every input
// must be ours, change is only "change" if the key at its claimed derivation
// path really produces that output script, and anything claiming our
// fingerprint that doesn't verify is flagged as suspicious.
//
// Multisig needs the wallet's descriptor (multisig.h): a PSBT can name any
// cosigner keys it likes next to ours, so inputs and change are checked
// against the scripts the descriptor derives, never the PSBT's own.

#define PSBT_MAX_OUTPUTS 16

struct PsbtOutput {
    char address[96];
    uint64_t amount;
    bool is_change;      // verified ours, internal chain (/1/n)
    bool is_self;        // verified ours, receive chain (/0/n): self-transfer
    bool suspicious;     // claims our fingerprint but doesn't verify
    char path[48];       // our key's derivation path for ours, e.g. m/84h/0h/0h/1/3
};

enum PsbtError {
    PSBT_OK = 0,
    PSBT_PARSE_FAILED,
    PSBT_NOT_OUR_SEED,      // inputs signed by a different fingerprint
    PSBT_NO_FINGERPRINT,    // inputs without key origin, or with fingerprint 00000000
    PSBT_UNSUPPORTED,       // not native segwit single-sig or multisig
    PSBT_SIGHASH,           // an input asks for a sighash other than ALL
    PSBT_TOO_MANY_OUTPUTS,  // more than PSBT_MAX_OUTPUTS outputs
    PSBT_TOO_BIG,           // past UR_MAX_MESSAGE (set by the scanner), or more than the heap holds
    PSBT_MISSING_AMOUNTS,   // inputs without witness UTXO: fee unknown
    PSBT_SUSPICIOUS,        // a "change" output failed verification
    PSBT_NEEDS_DESCRIPTOR,  // multisig inputs, but no wallet descriptor is loaded
    PSBT_WRONG_WALLET,      // multisig inputs that aren't the loaded descriptor's
    PSBT_NO_PREVIOUS_TX,    // more than one input without its full previous transaction: the fee can't be checked
};

enum FeeLevel { FEE_OK = 0, FEE_HIGH, FEE_VERY_HIGH };

struct PsbtReview {
    PsbtError error;
    bool testnet;
    int ms_m, ms_n;          // M of N for a multisig transaction, else 0
    int n_inputs;
    uint64_t input_total;
    uint64_t spend;          // to other people
    uint64_t self;           // to our own receive addresses
    uint64_t change;
    uint64_t fee;
    uint32_t fee_rate;       // ₿/vB, estimated (see psbt_fee_rate)
    FeeLevel fee_level;
    int n_outputs;
    PsbtOutput outputs[PSBT_MAX_OUTPUTS];
};

// Parse and review raw PSBT bytes against the loaded seed. Keeps the PSBT for
// signing until psbt_clear().
PsbtError psbt_load(const uint8_t *data, size_t len, PsbtReview *out);
// Sign every input we can, if the review passed. Returns the number of inputs
// that now carry our signature, so 0 means nothing is signed by us; -1 if
// signing ran out of memory (nothing is signed then either).
int psbt_sign();
// Serialize the (signed) PSBT. Returns bytes written, 0 if it doesn't fit.
size_t psbt_serialize(uint8_t *out, size_t cap);
void psbt_clear();

const char *psbt_error_text(PsbtError e);

// Fee levels, flagged on review as bitcoin.design asks. A fee is judged both
// against what's being sent (to others, or to yourself when nothing goes out)
// and by its rate, so neither a small send nor a big one hides it:
//   high:      10% of the amount, or 50 ₿/vB (paid only in fee spikes)
//   very high: 50% of the amount, or 500 ₿/vB (only the worst spikes)
// The device can't see the mempool, so the rate bars sit well above normal
// fees; small sends are caught by their share of the amount instead.
// A very high fee is also how a tampered PSBT would siphon funds. Neither
// level blocks signing: sweeping dust can legitimately cost that much.
FeeLevel psbt_fee_level(uint64_t fee, uint64_t sent, uint32_t rate);
// What a fee is judged against: sent to others, else to our own receive
// addresses, else to change (a consolidation into a change address).
uint64_t psbt_fee_base(const PsbtReview &r);
// Signed size of one input in vB: P2WPKH when m is 0, else wsh(sortedmulti)
// m-of-n. Signatures are counted at 72 bytes, so the size errs a little high.
uint32_t psbt_input_vsize(int m, int n);
// Fee rate in ₿/vB for a transaction of `vsize` vB, rounded up.
uint32_t psbt_fee_rate(uint64_t fee, uint64_t vsize);
