#include "qrscan.h"
#include <string.h>
#include <quirc.h>
#include <quirc_internal.h>   // num_capstones: finder patterns seen

static struct quirc *q;
static int qw, qh;
static int finders;

bool qrscan_init(int width, int height) {
    if (!q) q = quirc_new();
    if (!q) return false;
    qw = width; qh = height;
    return quirc_resize(q, width, height) >= 0;
}

bool qrscan_frame(const uint8_t *gray, QrPayload *out) {
    if (!q) return false;
    int w, h;
    uint8_t *img = quirc_begin(q, &w, &h);
    memcpy(img, gray, size_t(w) * h);
    quirc_end(q);
    finders = q->num_capstones;

    bool found = false;
    int n = quirc_count(q);
    for (int i = 0; i < n && !found; i++) {
        // static: quirc_data holds an 8.9KB payload, too big for the RP2350 stack
        static struct quirc_code code;
        static struct quirc_data data;
        quirc_extract(q, i, &code);
        if (quirc_decode(&code, &data) == QUIRC_SUCCESS && size_t(data.payload_len) <= sizeof(out->data)) {
            memcpy(out->data, data.payload, data.payload_len);
            out->len = data.payload_len;
            found = true;
        }
        // quirc_data can hold a seed, and quirc_code its modules: wipe both
        volatile uint8_t *v = (volatile uint8_t *)&data;
        for (size_t k = 0; k < sizeof(data); k++) v[k] = 0;
        v = (volatile uint8_t *)&code;
        for (size_t k = 0; k < sizeof(code); k++) v[k] = 0;
    }
    // the frame buffer holds a picture of the QR; clear it too
    memset(img, 0, size_t(w) * h);
    return found;
}

int qrscan_finders() { return q ? finders : 0; }

void qrscan_free() {
    if (q) quirc_destroy(q);
    q = nullptr;
    finders = 0;
}
