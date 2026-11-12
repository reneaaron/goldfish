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
#include "../app/wallet.h"
#include "../app/address_verify.h"
#include "src/libs/qrcode/qrcodegen.h"

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
static char cam_loaded_path[512];
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

// Script helper: show `text` as a QR code to the camera, instead of a file.
static bool cam_override;
static void cam_show_qr(const char *text) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return;
    int n = qrcodegen_getSize(qr), scale = 200 / (n + 8);
    int ox = (CAM_W - n * scale) / 2, oy = (CAM_H - n * scale) / 2;
    memset(cam_frame, 235, sizeof(cam_frame));
    for (int y = 0; y < n * scale; y++)
        for (int x = 0; x < n * scale; x++)
            cam_frame[(oy + y) * CAM_W + ox + x] = qrcodegen_getModule(qr, x / scale, y / scale) ? 20 : 235;
    cam_ok = true;
    cam_override = true;
}

const uint8_t *platform_camera_frame(int *width, int *height) {
    if (cam_override) { *width = CAM_W; *height = CAM_H; return cam_frame; }
    struct stat st;
    if (stat(cam_path(), &st) == 0) {
        if (st.st_mtime != cam_mtime || !cam_ok || strcmp(cam_loaded_path, cam_path()) != 0) {
            cam_mtime = st.st_mtime;
            snprintf(cam_loaded_path, sizeof(cam_loaded_path), "%s", cam_path());
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
    pump(300); save(dir, "1_home");
    ui_goto(UI_SCAN_SEED);
    pump(300); save(dir, "2_scan_waiting");
    setenv("SIM_CAMERA", "sim/samples/vector4_standard_12word.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_SEED; i++) pump(50);
    if (ui_current() != UI_SEED_LOADED) { fprintf(stderr, "SeedQR was not decoded\n"); return 1; }
    pump(300); save(dir, "3_seed_loaded");
    ui_goto(UI_SEED_MENU);    pump(300); save(dir, "4_seed_menu");
    ui_goto(UI_EXPORT_XPUB);  pump(300); save(dir, "5_export_xpub");
    ui_goto(UI_WIPE_CONFIRM); pump(300); save(dir, "6_wipe_confirm");

    // Transaction: SeedSigner's single-sig testnet vector and its seed.
    wallet_wipe();
    ui_goto(UI_SCAN_SEED);
    setenv("SIM_CAMERA", "sim/samples/testseed_height_demise_standard_12word.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_SEED; i++) pump(50);
    if (ui_current() != UI_SEED_LOADED) { fprintf(stderr, "test seed was not decoded\n"); return 1; }
    ui_goto(UI_SCAN_PSBT);
    setenv("SIM_CAMERA", "sim/samples/testpsbt_height_demise_base64.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_PSBT; i++) pump(50);
    if (ui_current() != UI_PSBT_OVERVIEW) { save(dir, "debug_psbt_scan"); fprintf(stderr, "PSBT was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "7_psbt_overview");
    ui_goto(UI_PSBT_OUTPUT); pump(300); save(dir, "8_psbt_output_1");
    ui_next_output();        pump(300); save(dir, "9_psbt_output_2");
    ui_next_output();        pump(300); save(dir, "10_psbt_sign");
    if (!ui_swipe_sign()) { fprintf(stderr, "signing failed\n"); return 1; }
    pump(400); save(dir, "11_psbt_signed");

    // Verify address: one of ours (testnet receive #7), then a foreign one.
    ui_goto(UI_SEED_MENU); pump(300); save(dir, "12_wallet_menu");
    char ours[96];
    address_at(true, 0, 7, ours, sizeof(ours));
    char uri[128];
    snprintf(uri, sizeof(uri), "bitcoin:%s?amount=0.001", ours);
    ui_goto(UI_SCAN_ADDRESS);
    cam_show_qr(uri);
    for (int i = 0; i < 200 && !(ui_current() == UI_VERIFY_RESULT && verify_done()); i++) pump(30);
    pump(300); save(dir, "13_verify_found");
    ui_goto(UI_SCAN_ADDRESS);
    cam_show_qr("tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl");   // "abandon" seed, not ours
    for (int i = 0; i < 400 && !(ui_current() == UI_VERIFY_RESULT && verify_done()); i++) pump(30);
    pump(300); save(dir, "14_verify_not_found");
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
