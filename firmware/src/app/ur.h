#pragma once
#include <stddef.h>
#include <stdint.h>

// Uniform Resources (BCR-2020-005): "ur:crypto-psbt/..." for PSBTs, and
// "ur:crypto-output/..." or "ur:bytes/..." for a multisig wallet descriptor.
// Decodes single and multi-part URs, including fountain-coded (mixed) parts,
// and encodes a PSBT as a cycling sequence of single-fragment parts.

#define UR_MAX_MESSAGE 16384     // largest CBOR message we accept
#define UR_MAX_FRAGMENTS 128

enum UrResult {
    UR_NOT_UR = 0,     // not a ur: string
    UR_BAD,            // malformed or checksum mismatch
    UR_WRONG_TYPE,     // a UR, but none of the types below
    UR_PROGRESS,       // part accepted, more needed
    UR_COMPLETE,       // message assembled
    UR_TOO_BIG,        // a well-formed part of a message past UR_MAX_MESSAGE
};

enum UrType {
    UR_TYPE_PSBT,      // crypto-psbt: the PSBT bytes
    UR_TYPE_OUTPUT,    // crypto-output: an output descriptor as CBOR (BCR-2020-010)
    UR_TYPE_BYTES,     // bytes: here, an output descriptor as text
};

struct UrDecoder {
    UrType type;                                // of the message being assembled
    uint32_t seq_len, message_len, checksum;
    size_t fragment_len;
    uint8_t received[UR_MAX_FRAGMENTS];         // 1 once a pure fragment is known
    uint8_t message[UR_MAX_MESSAGE + 256];      // fragments, back to back
    size_t found;                               // pure fragments known
    // mixed parts we couldn't reduce yet: only their still unknown fragments
    struct Mixed { uint8_t idx[UR_MAX_FRAGMENTS]; uint8_t n; uint16_t slot; uint32_t seq; };
    Mixed mixed[64];
    uint8_t mixed_data[64][256];
    size_t mixed_n;
};

void ur_decoder_reset(UrDecoder *d);
// Feed one scanned QR string. On UR_COMPLETE, d->type says what it is and
// *payload/*len point inside the decoder: at the unwrapped CBOR byte string
// for crypto-psbt and bytes, at the whole CBOR message for crypto-output.
UrResult ur_decoder_add(UrDecoder *d, const char *text, size_t len,
                        const uint8_t **payload, size_t *payload_len);
// 0..100 for a progress indicator
int ur_decoder_percent(const UrDecoder *d);

// Encode `data` as crypto-psbt part number `seq` (1-based): parts 1..count are
// the fragments, later ones fountain-coded mixes of them, as the bc-ur encoder.
// Writes an upper-case string (QR alphanumeric mode). Returns its length, 0 on
// error (including more than UR_MAX_FRAGMENTS fragments).
size_t ur_encode_part(const uint8_t *data, size_t len, size_t fragment_len,
                      uint32_t seq, char *out, size_t cap);
size_t ur_part_count(size_t len, size_t fragment_len);

// Minimal CBOR reader, shared with the crypto-output decoder (multisig.cpp).
struct Cbor { const uint8_t *p, *end; };
// Reads one item head: its major type and value (or length). False at the end
// of the data or for an indefinite length.
bool cbor_head(Cbor *c, uint8_t *major, uint64_t *val);
