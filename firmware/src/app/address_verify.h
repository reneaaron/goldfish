#pragma once
#include <stddef.h>
#include <stdint.h>

// Check that a receive address shown by a wallet app really belongs to the
// loaded seed, by deriving our own addresses and comparing (issue #1).
// Native SegWit single-sig only: m/84'/0'/0' (bc1q...) or m/84'/1'/0' (tb1q...).

#define VERIFY_RECEIVE_LIMIT 500
#define VERIFY_CHANGE_LIMIT 100

enum VerifyStatus {
    VERIFY_SEARCHING,
    VERIFY_FOUND,
    VERIFY_NOT_FOUND,
};

enum AddressParse {
    ADDR_OK,
    ADDR_NOT_ADDRESS,     // not a Bitcoin address at all
    ADDR_UNSUPPORTED,     // a Bitcoin address, but not native SegWit (bc1q/tb1q)
};

struct AddressCheck {
    char address[96];     // lower-case, without "bitcoin:" or parameters
    bool testnet;
    VerifyStatus status;
    int chain;            // 0 receive, 1 change (when found)
    int index;
    int checked;          // addresses compared so far
    int total;
};

// Accepts a plain address or a BIP-21 "bitcoin:" URI, any case.
AddressParse address_parse(const uint8_t *data, size_t len, AddressCheck *out);
// Compare up to `budget` more of our addresses. Returns true when finished.
// Needs a loaded seed; call repeatedly from a timer to keep the UI responsive.
bool address_verify_step(AddressCheck *c, int budget);
// Our address at m/84'/coin'/0'/chain/index. Returns length, 0 on error.
size_t address_at(bool testnet, int chain, int index, char *out, size_t cap);
