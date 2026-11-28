#pragma once
#include <stddef.h>
#include <stdint.h>
// LifeHash version 2: a small picture that stands for some bytes, here a
// fingerprint, easier to recognise and compare than eight hex digits.
// Sparrow shows the same picture beside a keystore's master fingerprint
// (its LifeHashIcon: version 2 of the fingerprint's 4 bytes).
//
// The algorithm is Wolf McNally's (Blockchain Commons, bc-lifehash,
// BSD-2-Clause-Patent). This is a port of version 2 only, written to give
// pixel for pixel what Sparrow's Java port (toucan) gives; the tests compare
// against pictures made by it.

constexpr int LIFEHASH_SIDE = 32;                                  // pixels per side
constexpr size_t LIFEHASH_RGB = LIFEHASH_SIDE * LIFEHASH_SIDE * 3;  // bytes of a picture

// The picture for `data`: 32 rows of 32 pixels, 3 bytes each (red, green,
// blue). Uses about 5 KB of heap while it runs and frees it; false if that
// isn't to be had.
bool lifehash_v2(const uint8_t *data, size_t len, uint8_t rgb[LIFEHASH_RGB]);
