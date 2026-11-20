// Browser simulator: the app compiled to WebAssembly (web/Makefile), drawn on
// a <canvas> by try/index.html. Pointer = touch.
//
// Camera: the page hands over one grayscale picture at a time (the sample the
// user picked) through web_camera_buffer / web_camera_set.
// From it, exactly like the desktop simulator's image file: the live frame is
// that picture fitted into 320x240, the still is it box-filtered down.
#include <lvgl.h>
#include <emscripten.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../app/qrscan.h"
#include "../app/ui.h"

// ------------------------------------------------------------ camera
#define CAM_W 320
#define CAM_H 240
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

const uint8_t *platform_camera_frame(int *width, int *height) {
    if (!src_ok) return nullptr;
    if (frame_stale) {
        memset(cam_frame, 128, sizeof(cam_frame));
        float s = fminf((float)CAM_W / src_w, (float)CAM_H / src_h);
        int dw = int(src_w * s), dh = int(src_h * s), ox = (CAM_W - dw) / 2, oy = (CAM_H - dh) / 2;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
                cam_frame[(oy + y) * CAM_W + ox + x] = src[int(y / s) * src_w + int(x / s)];
        frame_stale = false;
    }
    *width = CAM_W;
    *height = CAM_H;
    return cam_frame;
}

const char *platform_camera_error() { return src_ok ? nullptr : "Pick a picture beside the device"; }

void platform_camera_stop() { EM_ASM({ if (Module.onCameraStop) Module.onCameraStop(); }); }

void platform_backlight(int) {}   // the canvas has no backlight

// The board reboots into its USB drive to take new firmware; here, start over.
void platform_reboot_to_bootloader() { EM_ASM({ if (Module.onReboot) Module.onReboot(); }); }

// A still: the picture box-filtered down to at most 1000 pixels on its longer
// side (the sharper picture platform_camera_band cuts from), and that halved
// (CAM_ZOOM) for the still itself. Same as src/sim/main.cpp.
static uint8_t *still, *zoomed;
static int still_w, still_h;

uint8_t *platform_camera_still(int *width, int *height, size_t *room) {
    platform_camera_still_free();
    if (!src_ok) return nullptr;
    const int w = src_w, h = src_h;
    int f = 1;
    while ((w + f - 1) / f > 1000 || (h + f - 1) / f > 1000) f++;
    still_w = w / f / CAM_ZOOM; still_h = h / f / CAM_ZOOM;
    const int zw = still_w * CAM_ZOOM, zh = still_h * CAM_ZOOM;
    zoomed = (uint8_t *)malloc((size_t)zw * zh);
    still = (uint8_t *)malloc((size_t)still_w * still_h);
    for (int y = 0; zoomed && y < zh; y++)
        for (int x = 0; x < zw; x++) {
            int sum = 0;
            for (int j = 0; j < f; j++)
                for (int i = 0; i < f; i++) sum += src[(y * f + j) * w + x * f + i];
            zoomed[y * zw + x] = uint8_t(sum / (f * f));
        }
    for (int y = 0; zoomed && still && y < still_h; y++)
        for (int x = 0; x < still_w; x++) {
            int sum = 0;
            for (int j = 0; j < CAM_ZOOM; j++)
                for (int i = 0; i < CAM_ZOOM; i++) sum += zoomed[(y * CAM_ZOOM + j) * zw + x * CAM_ZOOM + i];
            still[y * still_w + x] = uint8_t(sum / (CAM_ZOOM * CAM_ZOOM));
        }
    if (!zoomed || !still) { platform_camera_still_free(); return nullptr; }
    *width = still_w;
    *height = still_h;
    *room = (size_t)still_w * still_h;
    return still;
}

bool platform_camera_band(int y0, int rows, uint8_t *buf) {
    const int zw = still_w * CAM_ZOOM, zh = still_h * CAM_ZOOM;
    if (!zoomed || y0 < 0 || rows <= 0 || y0 + rows > zh) return false;
    memcpy(buf, zoomed + (size_t)y0 * zw, (size_t)rows * zw);
    return true;
}

void platform_camera_still_free() {
    if (still) { memset(still, 0, (size_t)still_w * still_h); free(still); }
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
