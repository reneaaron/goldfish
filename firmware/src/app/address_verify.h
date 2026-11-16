#pragma once
#include <stddef.h>
#include <stdint.h>

// Check that a receive address shown by a wallet app really belongs to the
// loaded seed, by deriving our own addresses and comparing (issue #1).
// Native SegWit only: single-sig m/84'/0'/0' (bc1q...) or m/84'/1'/0' (tb1q...),
// and, with its descriptor loaded (multisig.h), the multisig wallet's P2WSH
// addresses (bc1q/tb1q, 62 characters).

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
    ADDR_NEEDS_DESCRIPTOR,// a P2WSH address, and no multisig descriptor is loaded
};

struct AddressCheck {
    char address[96];     // lower-case, without "bitcoin:" or parameters
    bool testnet;
    bool multisig;        // P2WSH: checked against the loaded multisig descriptor
    VerifyStatus status;
    int chain;            // 0 receive, 1 change (when found)
    int index;
    int checked;          // addresses compared so far
    int total;
    uint64_t amount;      // BIP-21 "amount=", in bitcoins (BIP177 base unit); 0 if none
    bool amount_bad;      // the URI has an amount= that isn't a valid BTC amount
};

// Accepts a plain address or a BIP-21 "bitcoin:" URI, any case.
AddressParse address_parse(const uint8_t *data, size_t len, AddressCheck *out);
// Compare up to `budget` more of our addresses. Returns true when finished.
// Needs a loaded seed; call repeatedly from a timer to keep the UI responsive.
bool address_verify_step(AddressCheck *c, int budget);
// Forget the keys cached for the last check. wallet_wipe() calls this.
void address_verify_forget();
// Our address at m/84'/coin'/0'/chain/index. Returns length, 0 on error.
size_t address_at(bool testnet, int chain, int index, char *out, size_t cap);
