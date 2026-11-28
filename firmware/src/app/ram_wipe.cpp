#include "ram_wipe.h"
#include <string.h>
#include <lvgl.h>
#define LV_MEM_WIPE_IMPL   // the real lv_tlsf_free/realloc here, not the macros
#include <lv_mem_wipe.h>
#include "src/display/lv_display_private.h"
#include "src/stdlib/builtin/lv_tlsf.h"

static void zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

void lv_wipe_tlsf_free(void *tlsf, void *p) {
    if (!p) return;
    zero(p, lv_tlsf_block_size(p));
    lv_tlsf_free(tlsf, p);
}

// No realloc in place: shrinking would free the tail and growing could merge
// into a neighbour without our zeroing, so a smaller size keeps the block and a
// larger one moves, wiping the old block.
void *lv_wipe_tlsf_realloc(void *tlsf, void *p, size_t size) {
    if (!p) return lv_tlsf_malloc(tlsf, size);
    const size_t old = lv_tlsf_block_size(p);
    if (size <= old) return p;
    void *n = lv_tlsf_malloc(tlsf, size);
    if (!n) return nullptr;
    memcpy(n, p, old);
    lv_wipe_tlsf_free(tlsf, p);
    return n;
}

void ram_wipe_lvgl() {
    lv_display_t *d = lv_display_get_default();
    // Only partial mode draws into scratch buffers. In direct mode the buffer is
    // the framebuffer itself (web, sim screenshots): it holds just what is shown.
    if (!d || d->render_mode != LV_DISPLAY_RENDER_MODE_PARTIAL) return;
    lv_draw_buf_t *bufs[] = {d->buf_1, d->buf_2};
    for (lv_draw_buf_t *b : bufs)
        if (b && b->data) zero(b->data, b->data_size);
}
