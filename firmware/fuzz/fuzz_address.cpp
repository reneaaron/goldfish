// Address / BIP-21 parsing for Verify address.
#include <stdint.h>
#include <string.h>
#include "../src/app/address_verify.h"
#include "fuzz_check.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    AddressCheck c;
    AddressParse r = address_parse(data, size, &c);
    FUZZ_CHECK(memchr(c.address, 0, sizeof(c.address)));
    if (r == ADDR_OK) {
        FUZZ_CHECK(strlen(c.address) == 42);
        FUZZ_CHECK(!strncmp(c.address, c.testnet ? "tb1q" : "bc1q", 4));
        FUZZ_CHECK(c.status == VERIFY_SEARCHING && c.total > 0);
    }
    return 0;
}
