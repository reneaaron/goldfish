// BIP177 amount formatting.
// Run: pio test -e test
#include <unity.h>
#include "../../src/app/amount.h"

void setUp() {}

static void check(uint64_t v, const char *btc, const char *legacy) {
    char b[40];
    fmt_btc(v, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(btc, b);
    fmt_btc_legacy(v, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(legacy, b);
}

void test_amounts() {
    check(0, "\xE2\x82\xBF" "0", "0 BTC");
    check(1, "\xE2\x82\xBF" "1", "0.00000001 BTC");
    check(143, "\xE2\x82\xBF" "143", "0.00000143 BTC");
    check(24857, "\xE2\x82\xBF" "24,857", "0.00024857 BTC");
    check(100000000, "\xE2\x82\xBF" "100,000,000", "1 BTC");
    check(150000000, "\xE2\x82\xBF" "150,000,000", "1.5 BTC");
    check(123456789012, "\xE2\x82\xBF" "123,456,789,012", "1,234.56789012 BTC");
    check(2100000000000000ull, "\xE2\x82\xBF" "2,100,000,000,000,000", "21,000,000 BTC");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_amounts);
    return UNITY_END();
}
