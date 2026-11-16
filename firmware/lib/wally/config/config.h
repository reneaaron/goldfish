// config.h for libwally-core's amalgamation build (normally written by its
// configure script). Little-endian targets only: RP2350 and the desktop builds.
#ifndef LIBWALLYCORE_CONFIG_H
#define LIBWALLYCORE_CONFIG_H

#include <stddef.h>

// Keeps the compiler from dropping libwally's wipes (wally_clear) as dead
// stores: with this it follows each memset with a memory barrier.
#if defined(__GNUC__) || defined(__clang__)
#define HAVE_INLINE_ASM 1
#endif

#include "ccan_config.h"

#endif
