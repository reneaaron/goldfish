// Browser simulator: the app compiled to WebAssembly (web/Makefile), drawn on
// a <canvas> by firmware/web/sim.js, in the landing page's hero. Pointer = touch.
//
// Camera: the page hands over one grayscale picture at a time (the sample the
// user picked) through web_camera_buffer / web_camera_set.
// From it, exactly like the desktop simulator's image file (src/sim/fit.h):
// the board's 240x180 view, and twice that for the sharper bands.
#include <lvgl.h>
#include <emscripten.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../app/qrscan.h"
#include "../app/ui.h"
#include "../app/device_entropy.h"
#include "../sim/fit.h"

// ------------------------------------------------------------ camera
#define CAM_W FIT_W
#define CAM_H FIT_H
static uint8_t cam_frame[CAM_W * CAM_H];
static uint8_t *src;          // the page's picture, src_w x src_h, 8-bit gray
static int src_w, src_h;
static size_t src_room;
static bool src_ok, frame_stale;

extern "C" {

// Room for a w x h picture; the page writes it, then calls web_camera_set.
EMSCRIPTEN_KEEPALIVE uint8_t *web_camera_buffer(int w, int h) {
    size_t need = (size_t)w * h;
    if (need > src_room) {
        if (src) { memset(src, 0, src_room); free(src); }
        src = (uint8_t *)malloc(need);
        src_room = src ? need : 0;
    }
    return src;
}

EMSCRIPTEN_KEEPALIVE void web_camera_set(int w, int h) {
    src_ok = src && w > 0 && h > 0 && (size_t)w * h <= src_room;
    src_w = w; src_h = h;
    frame_stale = true;
}

EMSCRIPTEN_KEEPALIVE void web_camera_clear() {
    if (src) memset(src, 0, src_room);
    src_ok = false;
}

}   // extern "C"

// ------------------------------------------------------------ Generate on device
// No TRNG, motion sensor or sensor noise in a browser: the browser's random
// source (getentropy -> crypto.getRandomValues) stands in for all three. Not
// this device's hardware; the board's are in src/board/entropy.cpp.
bool platform_trng(uint8_t *out, size_t n) {
    for (size_t got = 0; got < n; got += 256)
        if (getentropy(out + got, n - got < 256 ? n - got : 256) != 0) return false;
    return true;
}

bool platform_motion(int16_t out[6]) { return platform_trng((uint8_t *)out, 12); }

// While Generate on device reads the camera: the picture (mid-grey if there is
// none) with a little noise added, as a sensor would have.
static uint8_t noisy_frame[CAM_W * CAM_H];
static const uint8_t *noisy(const uint8_t *f) {
    static uint8_t r[CAM_W * CAM_H];
    if (!platform_trng(r, sizeof(r))) return nullptr;
    for (int i = 0; i < CAM_W * CAM_H; i++) {
        int v = (f ? f[i] : 128) + (r[i] & 7) - 3;
        noisy_frame[i] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    memset(r, 0, sizeof(r));
    return noisy_frame;
}

static const uint8_t *camera_frame(int *width, int *height);
const uint8_t *platform_camera_frame(int *width, int *height) {
    const uint8_t *f = camera_frame(width, height);
    if (ui_current() != UI_ENTROPY_DEVICE) return f;
    *width = CAM_W;
    *height = CAM_H;
    return noisy(f);
}

static const uint8_t *camera_frame(int *width, int *height) {
    if (!src_ok) return nullptr;
    if (frame_stale) {
        fit(src, src_w, src_h, cam_frame, CAM_W, CAM_H);
        frame_stale = false;
    }
    *width = CAM_W;
    *height = CAM_H;
    return cam_frame;
}

// No picture yet isn't an error: the page beside the device asks for one.
const char *platform_camera_error() { return nullptr; }

void platform_camera_stop() { EM_ASM({ if (Module.onCameraStop) Module.onCameraStop(); }); }

void platform_backlight(int) {}   // the canvas has no backlight

// The board reboots into its USB drive to take new firmware; here, start over.
void platform_reboot_to_bootloader() { EM_ASM({ if (Module.onReboot) Module.onReboot(); }); }

// A still: the board's sizes, as in src/sim/main.cpp.
static uint8_t *still, *zoomed;
static int still_w, still_h;

uint8_t *platform_camera_still(int *width, int *height, size_t *room) {
    platform_camera_still_free();
    if (!src_ok) return nullptr;
    still_w = CAM_W; still_h = CAM_H;
    const int zw = still_w * CAM_ZOOM, zh = still_h * CAM_ZOOM;
    zoomed = (uint8_t *)malloc((size_t)zw * zh);
    still = (uint8_t *)calloc(2, (size_t)still_w * still_h);   // the board lends both frame slots
    if (!zoomed || !still) { platform_camera_still_free(); return nullptr; }
    fit(src, src_w, src_h, zoomed, zw, zh);
    fit_still(zoomed, still, still_w, still_h, CAM_ZOOM);
    *width = still_w;
    *height = still_h;
    *room = (size_t)still_w * still_h * 2;
    return still;
}

bool platform_camera_band(int y0, int rows, uint8_t *buf) {
    const int zw = still_w * CAM_ZOOM, zh = still_h * CAM_ZOOM;
    if (!zoomed || y0 < 0 || rows <= 0 || y0 + rows > zh) return false;
    memcpy(buf, zoomed + (size_t)y0 * zw, (size_t)rows * zw);
    return true;
}

void platform_camera_still_free() {
    if (still) { memset(still, 0, (size_t)still_w * still_h * 2); free(still); }
    if (zoomed) { memset(zoomed, 0, (size_t)still_w * still_h * CAM_ZOOM * CAM_ZOOM); free(zoomed); }
    still = zoomed = nullptr;
}

// ------------------------------------------------------------ display, touch
static uint16_t fb[240 * 320];

// Pointer events queue up: LVGL reads its input every 30 ms, and a quick click's
// press and release would otherwise both land between two reads and be lost.
struct Touch { int16_t x, y; bool down; };
static Touch touches[64], last;
static int touch_head, touch_count;

extern "C" {
EMSCRIPTEN_KEEPALIVE uint16_t *web_framebuffer() { return fb; }
EMSCRIPTEN_KEEPALIVE void web_touch(int x, int y, int down) {
    Touch t = {(int16_t)x, (int16_t)y, down != 0};
    if (touch_count && t.down && touches[(touch_head + touch_count - 1) % 64].down) {   // a move: replace
        touches[(touch_head + touch_count - 1) % 64] = t;
        return;
    }
    if (touch_count == 64) {   // full: keep the newest state, so a release is never lost
        touches[(touch_head + 63) % 64] = t;
        return;
    }
    touches[(touch_head + touch_count++) % 64] = t;
}
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *) {
    if (lv_display_flush_is_last(d))
        EM_ASM({ Module.onFlush(); });
    lv_display_flush_ready(d);
}

static void touch_read(lv_indev_t *, lv_indev_data_t *data) {
    if (touch_count) {
        last = touches[touch_head];
        touch_head = (touch_head + 1) % 64;
        touch_count--;
    }
    data->point.x = last.x;
    data->point.y = last.y;
    data->state = last.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t now_ms() { return (uint32_t)emscripten_get_now(); }

static void loop() { lv_timer_handler(); }

// The cable comes out: nothing runs any more. The page then zeroes all of this
// instance's memory and drops it; plugging back in starts a new one.
extern "C" EMSCRIPTEN_KEEPALIVE void web_unplug() { emscripten_cancel_main_loop(); }

int main() {
    lv_init();
    lv_tick_set_cb(now_ms);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, fb, nullptr, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);
    ui_build();
    emscripten_set_main_loop(loop, 0, false);
    return 0;
}
