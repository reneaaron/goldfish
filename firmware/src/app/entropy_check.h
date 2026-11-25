#pragma once

// Do these dice rolls or coin flips look made up? Advisory only: the answer
// never changes how a seed is derived (seed_create.h), and there is no RNG
// here either. Run once, after the last roll is in: stats shown while typing
// nudge people to "fix" their numbers.
//
// Each test gives a p-value (how likely real dice or coins give something at
// least this odd); a test is flagged when its p-value times the number of
// tests in the battery (Bonferroni) is below 0.01. Plain arithmetic on the
// stack, no allocation.
//
// Dice ('1'..'6'):
//   ENT_STEPS     the step (roll[i] - roll[i-1]) mod 6 is uneven (chi-square,
//                 5 df): counting up or down, cycling through the faces
//   ENT_REPEATS   the same face again k rolls later, k = 2..12, too often for
//                 some k (binomial, times 11 for the lags): a repeating sequence
//   ENT_PAIRS     the same face twice in a row, too often or too rarely (two-
//                 sided binomial; people avoid repeats)
//   ENT_STREAK    the longest run of one face is too long (exact)
//   ENT_UNEVEN    face counts too uneven (chi-square, upper tail)
//   ENT_TOO_EVEN  face counts too even (chi-square, lower tail)
// Coins ('1' heads, '0' tails): ENT_REPEATS (lags 2..12), ENT_PAIRS (heads and
// tails switch too often or too rarely), ENT_STREAK, ENT_UNEVEN (heads far
// from half, two-sided binomial).
enum EntropyFlag {
    ENT_STEPS = 1,
    ENT_REPEATS = 2,
    ENT_PAIRS = 4,
    ENT_STREAK = 8,
    ENT_UNEVEN = 16,
    ENT_TOO_EVEN = 32,
};

// The flagged tests, 0 if nothing looks odd. dice: s is '1'..'6', else '0'/'1'.
int entropy_check(const char *s, int n, bool dice);
