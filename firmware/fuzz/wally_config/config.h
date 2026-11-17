// Native sanitizer builds only: libwally's x86 SSE4 SHA-256 inline asm
// crashes under ASan, and the RP2350 never takes that path anyway.
#include_next <config.h>
#undef HAVE_INLINE_ASM
