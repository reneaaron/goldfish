// quirc, the QR decoder every camera frame goes through. First byte picks:
//   0: decode a raw cell grid (quirc_code) directly: reaches the data parser
//   1: a small grayscale image through qrscan_frame (finder/grid detection)
#include <stdint.h>
#include <string.h>
#include <quirc.h>
#include "../src/app/qrscan.h"
#include "fuzz_check.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    uint8_t mode = data[0], p = data[1];
    data += 2; size -= 2;
    if (mode & 1) {
        int w = 21 + (p & 0x0f) * 8, h = 21 + (p >> 4) * 8;
        static int cw, ch;
        if (w != cw || h != ch) { if (!qrscan_init(w, h)) return 0; cw = w; ch = h; }
        static uint8_t img[200 * 200];
        size_t n = size_t(w) * h;
        memset(img, 0xff, n);
        memcpy(img, data, size < n ? size : n);
        static QrPayload out;
        if (qrscan_frame(img, &out)) FUZZ_CHECK(out.len <= sizeof(out.data));
        return 0;
    }
    static struct quirc_code code;
    static struct quirc_data qd;
    memset(&code, 0, sizeof(code));
    code.size = 17 + 4 * (1 + p % QUIRC_MAX_VERSION);
    size_t bytes = (size_t(code.size) * code.size + 7) / 8;
    memcpy(code.cell_bitmap, data, size < bytes ? size : bytes);
    if (quirc_decode(&code, &qd) == QUIRC_SUCCESS) {
        FUZZ_CHECK(qd.payload_len >= 0 && qd.payload_len <= QUIRC_MAX_PAYLOAD);
        FUZZ_CHECK(qd.version >= 1 && qd.version <= QUIRC_MAX_VERSION);
    }
    return 0;
}
