#pragma once
#include <stddef.h>
#include <stdint.h>

// BIP177: the base unit is the bitcoin, shown as a whole number with the ₿
// symbol. BTC keeps meaning 100,000,000 of them.
void fmt_btc(uint64_t v, char *out, size_t cap);          // "₿150,000,000"
void fmt_btc_legacy(uint64_t v, char *out, size_t cap);   // "1.5 BTC"
