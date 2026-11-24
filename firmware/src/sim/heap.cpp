// The board's heap, in the simulator: malloc, calloc, realloc and free from
// the app, LVGL, libwally and quirc (linked with --wrap, see platformio.ini;
// strdup too, and the aligned allocators stop the simulator)
// come from one APP_HEAP_BYTES arena, first fit, so running out (or a heap
// too split for a block) happens here as it would on a board. Libraries
// loaded at run time (libc's own, X11) keep the real heap; so do the
// simulator's camera pictures, which the board keeps in static RAM.
//
// 64-bit pointers make a few structures larger than on the board: a little
// stricter, never looser.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "heap_budget.h"

extern "C" {
void *__real_malloc(size_t);
void __real_free(void *);
void *__real_realloc(void *, size_t);
}

// A block: a header, then its bytes. Blocks tile the arena; free neighbours merge.
struct Hdr { size_t size; size_t used; };   // size: the bytes after the header
#define ALIGN 16
static_assert(sizeof(Hdr) % ALIGN == 0, "header keeps blocks aligned");
alignas(ALIGN) static uint8_t arena[APP_HEAP_BYTES];
static bool ready;

static Hdr *first() { return (Hdr *)arena; }
static Hdr *next(Hdr *h) { return (Hdr *)((uint8_t *)(h + 1) + h->size); }
static bool in_arena(const void *p) { return p >= arena && p < arena + sizeof(arena); }

static void init() {
    first()->size = sizeof(arena) - sizeof(Hdr);
    first()->used = 0;
    ready = true;
}

static bool in_arena_hdr(const Hdr *h) { return (const uint8_t *)h < arena + sizeof(arena); }

// h and every free block right after it, as one (h free).
static void merge_next(Hdr *h) {
    for (Hdr *b = next(h); in_arena_hdr(b) && !b->used; b = next(h)) h->size += sizeof(Hdr) + b->size;
}

// The first n bytes of h stay h's, a block of the rest is freed (if big enough to be one).
static void split(Hdr *h, size_t n) {
    if (h->size < n + sizeof(Hdr) + ALIGN) return;
    Hdr *rest = (Hdr *)((uint8_t *)(h + 1) + n);
    rest->size = h->size - n - sizeof(Hdr);
    rest->used = 0;
    h->size = n;
    merge_next(rest);
}

static size_t round_up(size_t n) { return n ? (n + ALIGN - 1) / ALIGN * ALIGN : ALIGN; }

extern "C" void *__wrap_malloc(size_t n) {
    if (!ready) init();
    if (n > sizeof(arena)) return nullptr;
    n = round_up(n);
    // first fit; free neighbours are merged on the way (free only merges forward)
    for (Hdr *h = first(); in_arena_hdr(h); h = next(h)) {
        if (h->used) continue;
        merge_next(h);
        if (h->size < n) continue;
        split(h, n);
        h->used = 1;
        return h + 1;
    }
    return nullptr;   // the app says so: "Out of memory", as on the board
}

extern "C" void __wrap_free(void *p) {
    if (!p) return;
    if (!in_arena(p)) { __real_free(p); return; }   // from a library loaded at run time
    Hdr *h = (Hdr *)p - 1;
    h->used = 0;
    merge_next(h);
}

extern "C" void *__wrap_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return nullptr;
    void *p = __wrap_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

extern "C" void *__wrap_realloc(void *p, size_t n) {
    if (!p) return __wrap_malloc(n);
    if (!in_arena(p)) return __real_realloc(p, n);
    if (n == 0) { __wrap_free(p); return nullptr; }
    Hdr *h = (Hdr *)p - 1;
    size_t want = round_up(n);
    if (want <= h->size) { split(h, want); return p; }   // shrink in place, the rest freed
    merge_next(h);                                        // grow in place if the next block is free
    if (want <= h->size) { split(h, want); return p; }
    void *q = __wrap_malloc(n);
    if (!q) return nullptr;
    memcpy(q, p, h->size);
    __wrap_free(p);
    return q;
}

// Allocators the arena doesn't model: nothing linked statically calls them
// today; one that starts to stops the simulator here instead of quietly
// taking memory the board doesn't have (add it to the arena then).
static void *unmodelled(const char *fn) {
    fprintf(stderr, "[sim] %s isn't in the board heap model (src/sim/heap.cpp)\n", fn);
    abort();
}
extern "C" int __wrap_posix_memalign(void **, size_t, size_t) { unmodelled("posix_memalign"); return -1; }
extern "C" void *__wrap_memalign(size_t, size_t) { return unmodelled("memalign"); }
extern "C" void *__wrap_aligned_alloc(size_t, size_t) { return unmodelled("aligned_alloc"); }
extern "C" void *__wrap_valloc(size_t) { return unmodelled("valloc"); }
extern "C" void *__wrap_reallocarray(void *, size_t, size_t) { return unmodelled("reallocarray"); }

extern "C" char *__wrap_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)__wrap_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
extern "C" char *__wrap_strndup(const char *s, size_t max) {
    size_t n = strnlen(s, max);
    char *d = (char *)__wrap_malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
