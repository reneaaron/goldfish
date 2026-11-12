#pragma once
#include <stddef.h>
#include <stdint.h>

// QR decoding of grayscale camera frames with quirc. The platform (board camera
// or simulator image loader) supplies frames; the app only sees decoded payloads.

struct QrPayload {
    uint8_t data[1024];
    size_t len;
};

bool qrscan_init(int width, int height);
// Decode one 8-bit grayscale frame (width*height bytes). Returns true and fills
// `out` if a QR code was found and decoded.
bool qrscan_frame(const uint8_t *gray, QrPayload *out);
void qrscan_free();

// Platform hook: return the latest grayscale frame, or nullptr if none is ready.
// Implemented in src/board (camera) and src/sim (image files).
const uint8_t *platform_camera_frame(int *width, int *height);
