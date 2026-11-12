#pragma once
#include <stddef.h>
#include <stdint.h>

// Uniform Resources (BCR-2020-005) for PSBTs: "ur:crypto-psbt/...".
// Decodes single and multi-part URs, including fountain-coded (mixed) parts,
// and encodes a payload as a cycling sequence of single-fragment parts.

#define UR_MAX_MESSAGE 8192      // largest CBOR message we accept
#define UR_MAX_FRAGMENTS 128

enum UrResult {
    UR_NOT_UR = 0,     // not a ur: string
    UR_BAD,            // malformed or checksum mismatch
    UR_WRONG_TYPE,     // a UR, but not crypto-psbt
    UR_PROGRESS,       // part accepted, more needed
    UR_COMPLETE,       // message assembled
};

struct UrDecoder {
    uint32_t seq_len, message_len, checksum;
    size_t fragment_len;
    uint8_t received[UR_MAX_FRAGMENTS];         // 1 once a pure fragment is known
    uint8_t message[UR_MAX_MESSAGE + 256];      // fragments, back to back
    size_t found;                               // pure fragments known
    // mixed parts we couldn't reduce yet
    struct Mixed { uint8_t idx[UR_MAX_FRAGMENTS]; uint8_t n; uint16_t slot; };
    Mixed mixed[64];
    uint8_t mixed_data[64][256];
    size_t mixed_n;
};

void ur_decoder_reset(UrDecoder *d);
// Feed one scanned QR string. On UR_COMPLETE, *payload/*len point at the PSBT
// bytes (the crypto-psbt CBOR byte string, unwrapped) inside the decoder.
UrResult ur_decoder_add(UrDecoder *d, const char *text, size_t len,
                        const uint8_t **payload, size_t *payload_len);
// 0..100 for a progress indicator
int ur_decoder_percent(const UrDecoder *d);

// Encode `data` as crypto-psbt part number `seq` (1-based, cycles through the
// fragments). Writes an upper-case string (QR alphanumeric mode). Returns its
// length, 0 on error.
size_t ur_encode_part(const uint8_t *data, size_t len, size_t fragment_len,
                      uint32_t seq, char *out, size_t cap);
size_t ur_part_count(size_t len, size_t fragment_len);
