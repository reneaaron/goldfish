#pragma once
#include <stddef.h>
#include <stdint.h>

// QR decoding of grayscale camera frames with quirc. The platform (board camera
// or simulator image loader) supplies frames; the app only sees decoded payloads.

struct QrPayload {
    uint8_t data[3072];     // enough for a single-frame base64 PSBT
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
// Platform hook: why no frame is coming, as one short line for the scan screen;
// nullptr while the camera works. The board has no serial log to print it to.
const char *platform_camera_error();
// Platform hook: scanning ended. Stop the camera and wipe its frame buffer.
void platform_camera_stop();
