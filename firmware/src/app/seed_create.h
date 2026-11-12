#pragma once
#include <stddef.h>
#include <stdint.h>
#include "seedqr.h"

// New seeds from entropy the user supplies: dice rolls, coin flips or words
// they pick themselves. There is deliberately no RNG anywhere on this device,
// hardware or pseudo, not even for tests: every bit comes from the user and
// can be re-checked offline.
//
// Dice and coins match SeedSigner (helpers/mnemonic_generation.py) and
// bitcoiner.guide/seed: the input string is hashed with SHA256 and the first
// 16 (12 words) or 32 (24 words) bytes are the BIP39 entropy.

int seed_dice_rolls(int words);   // 50 for 12 words, 99 for 24
int seed_coin_flips(int words);   // 128 for 12 words, 256 for 24

// rolls: ASCII '1'..'6', exactly seed_dice_rolls() of them.
bool seed_from_dice(const char *rolls, size_t n, int words, SeedEntropy *out);
// flips: '1' = heads, '0' = tails, exactly seed_coin_flips() of them.
bool seed_from_coins(const char *flips, size_t n, int words, SeedEntropy *out);

// BIP39 English wordlist.
const char *seed_word(int index);                 // 0..2047
// Index of the first word starting with `prefix` and how many do; 0 if none.
int seed_words_with_prefix(const char *prefix, int *first);
// 12 or 24 word indexes; false if the checksum (last word) is wrong.
bool seed_from_words(const uint16_t *idx, int n, SeedEntropy *out);
// Would `last` complete the first n-1 words to a valid mnemonic of n words?
bool seed_last_word_ok(const uint16_t *idx, int n, uint16_t last);
