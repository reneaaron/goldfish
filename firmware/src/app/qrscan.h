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
// Platform hooks for reading a whole handwritten page (page_reader.h), in two
// pictures of the same view taken while the user holds still:
// - platform_camera_still: what the preview showed (only that: the user framed
//   the page by it), grayscale, *width x *height, enough to find the words.
//   The simulator: the camera image file, at most 500 pixels on its longer
//   side. The board: the middle 240x180 rows of its last frame. Kept until
//   platform_camera_still_free(), which wipes it; once done with the picture
//   the app may write over *room bytes from it (at least width x height:
//   reading a page keeps its strips there).
// - platform_camera_band: rows [y0, y0 + rows) of a fresh picture of the
//   still's view CAM_ZOOM times as large (width * CAM_ZOOM wide), into buf: the board can't hold
//   all of it, so the words' strips are cut band by band. Works after
//   platform_camera_stop (it wakes the camera; stop it again after). False
//   if no picture came.
#define CAM_ZOOM 2
uint8_t *platform_camera_still(int *width, int *height, size_t *room);
bool platform_camera_band(int y0, int rows, uint8_t *buf);
void platform_camera_still_free();
