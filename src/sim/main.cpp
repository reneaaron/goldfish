// Desktop simulator: a 240x320 X11 window, mouse = touch.
//
//   pio run -e sim && .pio/build/sim/program
//
// Camera: the simulator "sees" the image at $SIM_CAMERA (default
// sim/camera.png), reloaded whenever the file changes. Copy a SeedQR picture
// there while the scan screen is open, e.g. cp sim/samples/vector4_standard_12word.png sim/camera.png
//
// Scripted screenshots of the whole flow (no window):
//   SIM_SHOTS=docs/screens .pio/build/sim/program
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../app/qrscan.h"
#include "../app/ui.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

static uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

// ------------------------------------------------------------ fake camera
// 320x240 grayscale like the OV5640 will deliver; the image is fitted inside.
#define CAM_W 320
#define CAM_H 240
static uint8_t cam_frame[CAM_W * CAM_H];
static bool cam_ok;
static time_t cam_mtime;
static const char *cam_path() {
    const char *p = getenv("SIM_CAMERA");
    return p ? p : "sim/camera.png";
}

static void cam_load(const char *path) {
    int w, h, n;
    uint8_t *img = stbi_load(path, &w, &h, &n, 1);
    cam_ok = img != nullptr;
    if (!img) return;
    memset(cam_frame, 128, sizeof(cam_frame));
    float s = fminf((float)CAM_W / w, (float)CAM_H / h);
    int dw = int(w * s), dh = int(h * s), ox = (CAM_W - dw) / 2, oy = (CAM_H - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            cam_frame[(oy + y) * CAM_W + ox + x] = img[int(y / s) * w + int(x / s)];
    stbi_image_free(img);
}

const uint8_t *platform_camera_frame(int *width, int *height) {
    struct stat st;
    if (stat(cam_path(), &st) == 0) {
        if (st.st_mtime != cam_mtime || !cam_ok) {
            cam_mtime = st.st_mtime;
            cam_load(cam_path());
        }
    } else {
        cam_ok = false;
    }
    if (!cam_ok) return nullptr;
    *width = CAM_W;
    *height = CAM_H;
    return cam_frame;
}

// ------------------------------------------------------------ screenshots
static uint8_t shot_buf[240 * 320 * 2];
static lv_display_t *shot_disp;

static void pump(int ms) {
    for (int t = 0; t < ms; t += 10) { lv_timer_handler(); usleep(10000); }
}

static void save(const char *dir, const char *name) {
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(shot_disp);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n240 320\n255\n");
    for (int i = 0; i < 240 * 320; i++) {
        uint16_t c = shot_buf[2 * i] | (shot_buf[2 * i + 1] << 8);
        uint8_t rgb[3] = {uint8_t((c >> 11) << 3), uint8_t(((c >> 5) & 0x3F) << 2), uint8_t((c & 0x1F) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

// Walk the flow: home, scan a 12-word Standard SeedQR, menu, xpub, wipe.
static int run_shots(const char *dir) {
    shot_disp = lv_display_create(240, 320);
    lv_display_set_color_format(shot_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(shot_disp, shot_buf, nullptr, sizeof(shot_buf), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(shot_disp, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });

    setenv("SIM_CAMERA", "sim/samples/does-not-exist.png", 1);
    ui_build();
    pump(100); save(dir, "1_home");
    ui_goto(UI_SCAN_SEED);
    pump(300); save(dir, "2_scan_waiting");
    setenv("SIM_CAMERA", "sim/samples/vector4_standard_12word.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_SEED; i++) pump(50);
    if (ui_current() != UI_SEED_LOADED) { fprintf(stderr, "SeedQR was not decoded\n"); return 1; }
    pump(100); save(dir, "3_seed_loaded");
    ui_goto(UI_SEED_MENU);    pump(100); save(dir, "4_seed_menu");
    ui_goto(UI_EXPORT_XPUB);  pump(100); save(dir, "5_export_xpub");
    ui_goto(UI_WIPE_CONFIRM); pump(100); save(dir, "6_wipe_confirm");
    return 0;
}

int main() {
    lv_init();
    lv_tick_set_cb(now_ms);

    if (const char *dir = getenv("SIM_SHOTS")) return run_shots(dir);

    lv_display_t *d = lv_x11_window_create("RP2350 signer (sim)", 240, 320);
    lv_x11_inputs_create(d, nullptr);  // the X11 driver hides the system cursor
    // Draw a fingertip-sized dot as the cursor instead.
    for (lv_indev_t *in = lv_indev_get_next(nullptr); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) continue;
        lv_obj_t *dot = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_50, 0);
        lv_obj_set_style_border_color(dot, lv_color_black(), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_translate_x(dot, -7, 0);   // centre the dot on the hotspot
        lv_obj_set_style_translate_y(dot, -7, 0);
        lv_indev_set_cursor(in, dot);
    }
    ui_build();
    while (true) {
        uint32_t wait = lv_timer_handler();
        usleep((wait < 5 ? wait : 5) * 1000);
    }
}
