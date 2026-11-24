#pragma once
// The camera's view of a picture, for the desktop and browser simulators:
// fitted into w x h (each pixel the mean of the picture's pixels it covers),
// grey around it. Both give the app the board's sizes: a 240x180 frame and
// still, and the still's view at 480x360 for the sharper bands.
#include <math.h>
#include <stdint.h>
#include <string.h>

#define FIT_W 240
#define FIT_H 180

static inline void fit(const uint8_t *img, int iw, int ih, uint8_t *out, int w, int h) {
    memset(out, 128, (size_t)w * h);
    float s = fminf((float)w / iw, (float)h / ih);
    int dw = int(iw * s), dh = int(ih * s), ox = (w - dw) / 2, oy = (h - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int x0 = int(x / s), x1 = int((x + 1) / s), y0 = int(y / s), y1 = int((y + 1) / s);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            int sum = 0, k = 0;
            for (int j = y0; j < y1 && j < ih; j++)
                for (int i = x0; i < x1 && i < iw; i++) sum += img[j * iw + i], k++;
            out[(oy + y) * w + ox + x] = uint8_t(k ? sum / k : 128);
        }
}

// The still (w x h, room for two: the board lends both frame slots) from the zoomed view.
static inline void fit_still(const uint8_t *zoomed, uint8_t *still, int w, int h, int zoom) {
    const int zw = w * zoom;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sum = 0;
            for (int j = 0; j < zoom; j++)
                for (int i = 0; i < zoom; i++) sum += zoomed[(y * zoom + j) * zw + x * zoom + i];
            still[y * w + x] = uint8_t(sum / (zoom * zoom));
        }
}
