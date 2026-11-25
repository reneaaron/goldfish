#include "ram_wipe.h"
#include <stdint.h>
#include <lvgl.h>
#include <lv_pool.h>
#include "src/display/lv_display_private.h"
#include "src/stdlib/builtin/lv_tlsf.h"

alignas(8) static uint8_t pool[LV_MEM_SIZE];

void *lv_app_pool(size_t size) { return size <= sizeof(pool) ? pool : nullptr; }

static void zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

// A free TLSF block keeps its free-list links in its first two words, and its
// last word is the next block's back link: everything between is stale data.
static void wipe_free(void *ptr, size_t size, int used, void *) {
    const size_t keep_front = 2 * sizeof(void *), keep_back = sizeof(void *);
    if (used || size <= keep_front + keep_back) return;
    zero((uint8_t *)ptr + keep_front, size - keep_front - keep_back);
}

void ram_wipe_lvgl() {
    // The TLSF control structure sits at the start of the pool, the blocks after it.
    lv_tlsf_walk_pool(lv_tlsf_get_pool(pool), wipe_free, nullptr);
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    lv_draw_buf_t *bufs[] = {d->buf_1, d->buf_2};
    for (lv_draw_buf_t *b : bufs)
        if (b && b->data) zero(b->data, b->data_size);
}
