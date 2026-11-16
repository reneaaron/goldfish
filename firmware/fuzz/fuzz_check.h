#pragma once
#include <stdio.h>
#include <stdlib.h>

// An invariant the code under test must never break. Aborts so the driver
// (or libFuzzer) saves the input as a crash.
#define FUZZ_CHECK(cond)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FUZZ_CHECK failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            abort();                                                              \
        }                                                                         \
    } while (0)
