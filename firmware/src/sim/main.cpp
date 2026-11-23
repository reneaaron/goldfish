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
//
// Remote control for the end-to-end tests in e2e/ (no window):
//   SIM_CONTROL=1 .pio/build/sim/program      commands on stdin, see run_control()
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../app/qrscan.h"
#include "../app/ui.h"
#include "../app/wallet.h"
#include "../app/address_verify.h"
#include "../app/seed_create.h"
#include "../app/ur.h"
#include <wally_core.h>
#include "../../test/fixtures/multisig_vectors.h"   // SeedSigner's public 2-of-3 test wallet
#include "src/libs/qrcode/qrcodegen.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"   // key positions, for run_control()
#include "src/misc/lv_area_private.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

// Under SIM_CONTROL time only passes when a command says so: runs are fast and repeatable.
static bool fake_clock;
static uint32_t fake_ms;

static uint32_t now_ms() {
    if (fake_clock) return fake_ms;
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

// Script helper: show a hand-made card, copied exactly from the QR the device
// draws on its backup screens (ui_backup_qr), so the check covers that encoding.
static bool cam_show_matrix(const uint8_t *qr) {
    if (!qr) return false;
    int n = qrcodegen_getSize(qr), scale = 200 / (n + 8);
    int ox = (CAM_W - n * scale) / 2, oy = (CAM_H - n * scale) / 2;
    memset(cam_frame, 235, sizeof(cam_frame));
    for (int y = 0; y < n * scale; y++)
        for (int x = 0; x < n * scale; x++)
            cam_frame[(oy + y) * CAM_W + ox + x] = qrcodegen_getModule(qr, x / scale, y / scale) ? 20 : 235;
    cam_ok = true;
    cam_override = true;
    return true;
}

const uint8_t *platform_camera_frame(int *width, int *height) {
    if (cam_override) { *width = CAM_W; *height = CAM_H; return cam_ok ? cam_frame : nullptr; }
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

const char *platform_camera_error() { return nullptr; }

void platform_camera_stop() {}

void platform_backlight(int) {}   // the window has no backlight

// The board vanishes from the screen and comes back as a drive; here, just quit.
void platform_reboot_to_bootloader() { fprintf(stderr, "[sim] reboot to bootloader\n"); exit(0); }

// A still: the camera image file, box-filtered down to at most 1000 pixels on
// its longer side (the sharper picture platform_camera_band cuts from), and
// that halved (CAM_ZOOM) for the still itself.
static uint8_t *still, *zoomed;
static int still_w, still_h;

uint8_t *platform_camera_still(int *width, int *height, size_t *room) {
    platform_camera_still_free();
    if (cam_override) return nullptr;
    int w, h, n;
    uint8_t *img = stbi_load(cam_path(), &w, &h, &n, 1);
    if (!img) return nullptr;
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
                for (int i = 0; i < f; i++) sum += img[(y * f + j) * w + x * f + i];
            zoomed[y * zw + x] = uint8_t(sum / (f * f));
        }
    for (int y = 0; zoomed && still && y < still_h; y++)
        for (int x = 0; x < still_w; x++) {
            int sum = 0;
            for (int j = 0; j < CAM_ZOOM; j++)
                for (int i = 0; i < CAM_ZOOM; i++) sum += zoomed[(y * CAM_ZOOM + j) * zw + x * CAM_ZOOM + i];
            still[y * still_w + x] = uint8_t(sum / (CAM_ZOOM * CAM_ZOOM));
        }
    stbi_image_free(img);
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

// ------------------------------------------------------------ screenshots
static uint8_t shot_buf[240 * 320 * 2];
static lv_display_t *shot_disp;

static void pump(int ms) {
    for (int t = 0; t < ms; t += 10) {
        if (fake_clock) fake_ms += 10;
        lv_timer_handler();
        if (!fake_clock) usleep(10000);
    }
}

static bool save_ppm(const char *path) {
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(shot_disp);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return false; }
    fprintf(f, "P6\n240 320\n255\n");
    for (int i = 0; i < 240 * 320; i++) {
        uint16_t c = shot_buf[2 * i] | (shot_buf[2 * i + 1] << 8);
        uint8_t rgb[3] = {uint8_t((c >> 11) << 3), uint8_t(((c >> 5) & 0x3F) << 2), uint8_t((c & 0x1F) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

static void save(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    if (save_ppm(path)) printf("wrote %s\n", path);
}

static void headless_display() {
    shot_disp = lv_display_create(240, 320);
    lv_display_set_color_format(shot_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(shot_disp, shot_buf, nullptr, sizeof(shot_buf), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(shot_disp, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });
}

// Walk the flow: home, scan a 12-word Standard SeedQR, wallet, options, xpub, wipe.
static int run_shots(const char *dir) {
    headless_display();

    setenv("SIM_CAMERA", "sim/samples/does-not-exist.png", 1);
    ui_build();
    pump(300); save(dir, "1_home");
    ui_brightness_panel(true); pump(300); save(dir, "1c_brightness");
    ui_brightness_panel(false); pump(300);
    ui_goto(UI_UPDATE); pump(300); save(dir, "1b_update");
    ui_goto(UI_SCAN_SEED);
    pump(300); save(dir, "2_scan_waiting");
    setenv("SIM_CAMERA", "sim/samples/vector4_standard_12word.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_SEED; i++) pump(50);
    if (ui_current() != UI_WALLET) { fprintf(stderr, "SeedQR was not decoded\n"); return 1; }
    pump(300); save(dir, "3_seed_loaded");
    ui_goto(UI_WALLET_OPTIONS);    pump(300); save(dir, "4_seed_menu");
    ui_goto(UI_EXPORT_XPUB);  pump(300); save(dir, "5_export_xpub");
    ui_key_details();         pump(300); save(dir, "5f_export_details");
    wallet_set_testnet(true);
    ui_goto(UI_NETWORK);      pump(300); save(dir, "5b_network");
    ui_goto(UI_EXPORT_XPUB);  pump(300); save(dir, "5c_export_vpub");
    wallet_set_testnet(false);
    wallet_set_export_multisig(true);
    ui_goto(UI_EXPORT_XPUB);  pump(300); save(dir, "5e_export_multisig");
    wallet_set_export_multisig(false);
    ui_goto(UI_WIPE_CONFIRM); pump(300); save(dir, "6_wipe_confirm");

    // Transaction: SeedSigner's single-sig vector and its seed, on mainnet
    // (sim/make_mainnet_psbt.py) so the screens carry no TESTNET label.
    wallet_wipe();
    ui_goto(UI_SCAN_SEED);
    setenv("SIM_CAMERA", "sim/samples/testseed_height_demise_standard_12word.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_SEED; i++) pump(50);
    if (ui_current() != UI_WALLET) { fprintf(stderr, "test seed was not decoded\n"); return 1; }
    ui_goto(UI_SCAN);
    setenv("SIM_CAMERA", "sim/samples/mainpsbt_height_demise_base64.png", 1);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN; i++) pump(50);
    if (ui_current() != UI_PSBT_OVERVIEW) { save(dir, "debug_psbt_scan"); fprintf(stderr, "PSBT was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "7_psbt_overview");
    ui_goto(UI_PSBT_OUTPUT); pump(300); save(dir, "8_psbt_output_1");
    ui_next_output();        pump(300); save(dir, "9_psbt_output_2");
    ui_next_output();        pump(300); save(dir, "10_psbt_sign");
    if (!ui_swipe_sign()) { fprintf(stderr, "signing failed\n"); return 1; }
    pump(400); save(dir, "11_psbt_signed");

    // Verify address: one of ours (receive #7), then a foreign one.
    ui_goto(UI_WALLET); pump(300); save(dir, "12_wallet_menu");
    char ours[96];
    address_at(false, 0, 7, ours, sizeof(ours));
    char uri[128];
    snprintf(uri, sizeof(uri), "bitcoin:%s?amount=0.001", ours);
    ui_goto(UI_SCAN);
    cam_show_qr(uri);
    for (int i = 0; i < 200 && !(ui_current() == UI_VERIFY_RESULT && verify_done()); i++) pump(30);
    pump(300); save(dir, "13_verify_found");
    ui_goto(UI_SCAN);
    cam_show_qr("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");   // "abandon" seed, not ours
    for (int i = 0; i < 400 && !(ui_current() == UI_VERIFY_RESULT && verify_done()); i++) pump(30);
    pump(300); save(dir, "14_verify_not_found");

    // New seed from 50 dice rolls (SeedSigner's test vector): write down the
    // words, copy a QR card and check it. New seed never loads the seed.
    wallet_wipe();
    cam_override = false;
    ui_goto(UI_NEW_SEED); pump(300); save(dir, "15_new_seed");
    const char *rolls = "12345612345612345612345612345612345612345612345612";
    ui_seed_entry(UI_ENTROPY_DICE, 12);
    for (int i = 0; i < 23; i++) ui_entropy_input(rolls[i]);
    pump(300); save(dir, "16_dice");
    for (int i = 23; i < 50; i++) ui_entropy_input(rolls[i]);
    if (ui_current() != UI_BACKUP_WARNING) { fprintf(stderr, "dice did not make a seed\n"); return 1; }
    pump(300); save(dir, "17_backup_warning");
    ui_goto(UI_BACKUP_WORDS);   pump(300); save(dir, "18_backup_words");
    ui_goto(UI_BACKUP_CHOICE);  pump(300); save(dir, "19_backup_choice");
    ui_goto(UI_BACKUP_QR);      pump(300); save(dir, "20_backup_qr");
    // Part 1 is all pre-printed finder square; show the first part with squares to copy.
    ui_goto(UI_BACKUP_BLOCK); ui_backup_next(); pump(300); save(dir, "21_backup_block_1");
    while (ui_current() == UI_BACKUP_BLOCK) ui_backup_next();
    if (ui_current() != UI_BACKUP_SCAN) { fprintf(stderr, "no card check after the last block\n"); return 1; }
    setenv("SIM_CAMERA", "sim/samples/vector4_compact_12word.png", 1);   // somebody else's seed
    for (int i = 0; i < 40; i++) pump(50);
    save(dir, "22_check_wrong_card");
    if (!cam_show_matrix(ui_backup_qr())) { fprintf(stderr, "no backup QR on screen\n"); return 1; }
    for (int i = 0; i < 100 && ui_current() == UI_BACKUP_SCAN; i++) pump(50);
    if (ui_current() != UI_SEED_READY || wallet_has_seed()) { fprintf(stderr, "card check failed (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "23_seed_ready");

    // Coin flips, up to the count.
    cam_override = false;
    ui_seed_entry(UI_ENTROPY_COINS, 12);
    for (int i = 0; i < 37; i++) ui_entropy_input("1101"[i % 4]);
    pump(300); save(dir, "24_coins");

    // Draw words: 11 by hand, the last word's 7 bits from coin flips. The
    // expected last word, "prize", was computed in Python.
    ui_seed_entry(UI_ENTER_WORDS, 12, true);
    const char *drawn[] = {"crawl", "focus", "rescue", "cable", "view", "pledge", "rather", "dinner", "cousin", "unfair", "day"};
    uint16_t didx[11];
    for (int i = 0; i < 11; i++) {
        if (!ui_enter_word(drawn[i])) { fprintf(stderr, "drawn word %s refused\n", drawn[i]); return 1; }
        int first;
        seed_words_with_prefix(drawn[i], &first);
        didx[i] = uint16_t(first);
    }
    if (ui_current() != UI_ENTROPY_COINS) { fprintf(stderr, "no coin flips for the last word\n"); return 1; }
    pump(300); save(dir, "25_last_word_coins");
    for (const char *f = "1010101"; *f; f++) ui_entropy_input(*f);
    if (ui_current() != UI_BACKUP_WARNING) { fprintf(stderr, "last word flips did not make a seed\n"); return 1; }
    uint16_t all[24];
    SeedEntropy e;
    seed_final_word(didx, 12, "1010101", &e);
    seed_word_indexes(e, all);
    seedqr_wipe(&e);
    if (strcmp(seed_word(all[11]), "prize")) { fprintf(stderr, "last word %s, not prize\n", seed_word(all[11])); return 1; }
    ui_goto(UI_HOME);

    // Restore from words: loads straight away, then "Back up as QR" with an
    // optional card check.
    ui_goto(UI_HOME);
    ui_seed_entry(UI_ENTER_WORDS, 12); pump(300); save(dir, "26_restore");
    const char *words[] = {"crew", "marble", "private", "differ", "race", "truly", "blush", "basket", "crater", "affair", "prepare"};
    for (const char *w : words)
        if (!ui_enter_word(w)) { fprintf(stderr, "word %s refused\n", w); return 1; }
    pump(300); save(dir, "27_words_last");
    if (ui_enter_word("zoo")) { fprintf(stderr, "invalid last word accepted\n"); return 1; }
    if (!ui_enter_word("unique") || ui_current() != UI_WALLET) { fprintf(stderr, "words did not load\n"); return 1; }
    ui_goto(UI_WALLET_OPTIONS); pump(300); save(dir, "28_menu_backup");
    ui_backup_loaded();
    if (ui_current() != UI_BACKUP_WARNING) { fprintf(stderr, "backup did not start\n"); return 1; }
    ui_goto(UI_BACKUP_QR);
    ui_goto(UI_BACKUP_BLOCK);
    while (true) {
        ui_backup_next();
        if (ui_current() != UI_BACKUP_BLOCK) break;
    }
    // ui_backup_next from the last block goes to the check; capture the last block first.
    ui_goto(UI_BACKUP_BLOCK); pump(300); save(dir, "29_backup_last_block");
    ui_backup_next();
    if (!cam_show_matrix(ui_backup_qr())) { fprintf(stderr, "no backup QR on screen\n"); return 1; }
    for (int i = 0; i < 100 && ui_current() == UI_BACKUP_SCAN; i++) pump(50);
    if (ui_current() != UI_WALLET || !wallet_has_seed()) { fprintf(stderr, "existing backup check failed\n"); return 1; }

    // BIP-85 child seed of the loaded seed: words, QR card, card check.
    ui_goto(UI_WALLET_OPTIONS);    pump(300); save(dir, "30_menu_child");
    ui_goto(UI_CHILD_INDEX);
    // Keypad: the largest child number, a digit past it is ignored, Del back to 0.
    for (const char *d = "21474836479"; *d; d++) ui_child_digit(*d - '0');
    if (strcmp(ui_child_number(), "2147483647")) { fprintf(stderr, "child number not capped: %s\n", ui_child_number()); return 1; }
    for (int i = 0; i < 11; i++) ui_child_digit(-1);
    if (strcmp(ui_child_number(), "0")) { fprintf(stderr, "Del did not clear: %s\n", ui_child_number()); return 1; }
    ui_child_seed(0x80000000u);   // past the hardened range: stays on the keypad with a note
    if (ui_current() != UI_CHILD_INDEX) { fprintf(stderr, "bad child number not refused\n"); return 1; }
    pump(300); save(dir, "32b_child_failed");
    ui_child_digit(0);
    ui_child_digit(3);
    pump(300); save(dir, "32_child_index");
    ui_child_seed((uint32_t)strtoul(ui_child_number(), nullptr, 10));
    if (ui_current() != UI_BACKUP_WARNING) { fprintf(stderr, "child seed did not start\n"); return 1; }
    ui_goto(UI_BACKUP_WORDS); pump(300); save(dir, "33_child_words");
    ui_goto(UI_BACKUP_QR);
    ui_goto(UI_BACKUP_BLOCK);
    while (ui_current() == UI_BACKUP_BLOCK) ui_backup_next();
    if (!cam_show_matrix(ui_backup_qr())) { fprintf(stderr, "no child QR on screen\n"); return 1; }
    for (int i = 0; i < 100 && ui_current() == UI_BACKUP_SCAN; i++) pump(50);
    if (ui_current() != UI_SEED_READY || !wallet_has_seed()) { fprintf(stderr, "child card check failed\n"); return 1; }
    pump(600); save(dir, "34_child_ready");

    // BIP-39 passphrase: "abandon ... about" with TREZOR (the BIP39 test vector's
    // seed; its fingerprint, b4e3f5ed, was checked against the vector's root key).
    wallet_wipe();
    ui_goto(UI_HOME);
    ui_seed_entry(UI_ENTER_WORDS, 12);
    for (int i = 0; i < 11; i++) ui_enter_word("abandon");
    if (!ui_enter_word("about") || ui_current() != UI_WALLET) { fprintf(stderr, "abandon seed did not load\n"); return 1; }
    pump(300); save(dir, "35_seed_loaded_link");
    ui_goto(UI_PASSPHRASE); pump(300); save(dir, "36_passphrase_empty");
    ui_passphrase_key("OK");          // empty and none set: nothing happens
    if (ui_current() != UI_WALLET || wallet_has_passphrase()) { fprintf(stderr, "empty passphrase was applied\n"); return 1; }
    ui_goto(UI_PASSPHRASE);
    ui_passphrase_key("x");
    ui_passphrase_key("Del");
    ui_passphrase_key("ABC");
    for (const char *c = "TREZOR"; *c; c++) { char k[2] = {*c, 0}; ui_passphrase_key(k); }
    pump(300); save(dir, "37_passphrase_typed");
    ui_passphrase_key("123");  pump(300); save(dir, "38_passphrase_digits");
    ui_passphrase_key("#+=");  pump(300); save(dir, "39_passphrase_symbols");
    ui_passphrase_key("OK");
    char pfp[9];
    wallet_fingerprint_hex(pfp);
    if (ui_current() != UI_WALLET || !wallet_has_passphrase() || strcmp(pfp, "b4e3f5ed")) {
        fprintf(stderr, "passphrase fingerprint %s\n", pfp);
        return 1;
    }
    pump(300); save(dir, "40_passphrase_set");
    // Typing stops at 64 characters; then an empty passphrase removes it again.
    ui_goto(UI_PASSPHRASE);
    for (int i = 0; i < 70; i++) { char k[2] = {char('a' + i % 26), 0}; ui_passphrase_key(k); }
    pump(300); save(dir, "41_passphrase_full");
    for (int i = 0; i < 70; i++) ui_passphrase_key("Del");
    pump(300); save(dir, "42_passphrase_remove");
    ui_passphrase_key("OK");
    wallet_fingerprint_hex(pfp);
    if (wallet_has_passphrase() || strcmp(pfp, "73c5da0a")) { fprintf(stderr, "passphrase was not removed\n"); return 1; }

    // Multisig: SeedSigner's 2-of-3 testnet wallet. Its transaction is refused
    // until the descriptor has been scanned and confirmed.
    wallet_wipe();
    ui_goto(UI_HOME);
    ui_seed_entry(UI_ENTER_WORDS, 12);
    char ms_words[128];
    snprintf(ms_words, sizeof(ms_words), "%s", MS_MNEMONIC);
    for (char *w = strtok(ms_words, " "); w; w = strtok(nullptr, " ")) ui_enter_word(w);
    if (ui_current() != UI_WALLET) { fprintf(stderr, "multisig seed did not load\n"); return 1; }
    static uint8_t ms_psbt[4096];
    size_t ms_psbt_len = 0;
    if (wally_base64_to_bytes(MS_PSBT, 0, ms_psbt, sizeof(ms_psbt), &ms_psbt_len) != WALLY_OK) return 1;
    // The transaction as an animated UR, part after part, until the scanner leaves.
    auto show_psbt = [&]() {
        static char part[600];
        for (uint32_t seq = 1; seq < 200 && ui_current() == UI_SCAN; seq++) {
            if (!ur_encode_part(ms_psbt, ms_psbt_len, 100, seq, part, sizeof(part))) break;
            cam_show_qr(part);
            pump(250);
        }
    };
    ui_goto(UI_SCAN);
    show_psbt();
    if (ui_current() != UI_PSBT_ERROR) { fprintf(stderr, "multisig PSBT without descriptor: screen %d\n", ui_current()); return 1; }
    pump(300); save(dir, "43_multisig_needs_descriptor");
    ui_goto(UI_SCAN);
    static char ur_upper[1024];   // upper case: a smaller, alphanumeric QR
    for (size_t i = 0; i <= strlen(MS_UR_OUTPUT); i++) ur_upper[i] = char(toupper((unsigned char)MS_UR_OUTPUT[i]));
    cam_show_qr(ur_upper);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN; i++) pump(50);
    if (ui_current() != UI_MULTISIG) { save(dir, "debug_descriptor_scan"); fprintf(stderr, "descriptor was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "44_multisig_wallet");
    // Leaving without "Use wallet" drops it: the transaction is still refused.
    ui_goto(UI_SCAN);
    show_psbt();
    if (ui_current() != UI_PSBT_ERROR) { fprintf(stderr, "unconfirmed descriptor was used\n"); return 1; }
    ui_goto(UI_SCAN);
    cam_show_qr(MS_DESCRIPTOR);   // this time as plain text
    for (int i = 0; i < 100 && ui_current() == UI_SCAN; i++) pump(50);
    if (ui_current() != UI_MULTISIG) { save(dir, "debug_descriptor_text"); fprintf(stderr, "text descriptor was not accepted (screen %d)\n", ui_current()); return 1; }
    ui_multisig_use();
    if (ui_current() != UI_SCAN) { fprintf(stderr, "Use wallet did not return to the scanner\n"); return 1; }
    show_psbt();
    if (ui_current() != UI_PSBT_OVERVIEW) { fprintf(stderr, "multisig PSBT was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "45_multisig_overview");
    ui_goto(UI_PSBT_OUTPUT); pump(300); save(dir, "46_multisig_change");
    ui_next_output();
    ui_next_output();
    ui_next_output();        pump(300); save(dir, "47_multisig_sign");
    if (!ui_swipe_sign()) { fprintf(stderr, "multisig signing failed\n"); return 1; }
    pump(400); save(dir, "48_multisig_signed");
    ui_goto(UI_WALLET);   pump(300); save(dir, "49_menu_multisig");
    ui_goto(UI_SCAN);
    cam_show_qr(MS_ADDR_CHANGE_4);
    for (int i = 0; i < 400 && !(ui_current() == UI_VERIFY_RESULT && verify_done()); i++) pump(30);
    if (ui_current() != UI_VERIFY_RESULT) { fprintf(stderr, "multisig address was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "50_multisig_verify");

    // Written words on Scan seed (experimental): a photo of a written 24-word test seed
    // (handwriting/make_page.py). Words read are used; the rest are typed.
    static const char *const page_words[24] = {
        "calm", "scene", "merit", "capable", "noble", "great", "task", "reject", "also", "fury", "media", "vendor",
        "kiwi", "leaf", "census", "when", "skate", "canal", "obvious", "page", "ball", "certain", "unable", "test"};
    wallet_wipe();
    ui_goto(UI_HOME);
    cam_override = false;
    setenv("SIM_CAMERA", "sim/samples/handwritten_page.png", 1);
    ui_goto(UI_SCAN_SEED);
    pump(300); save(dir, "51_scan_page");
    pump(1850); save(dir, "51a_page_hold_still");   // held still: it starts by itself, the words found before the sharp picture
    for (int i = 0; i < 400 && ui_current() == UI_SCAN_SEED; i++) pump(20);   // hold still: the words, then the sharp picture
    pump(300); save(dir, "51b_page_found");
    if (!ui_page_found_read()) { fprintf(stderr, "no words found (screen %d)\n", ui_current()); return 1; }
    for (int i = 0; i < 400 && ui_current() == UI_PAGE_READING; i++) {
        pump(20);
        if (i == 40) save(dir, "52_page_reading");   // a few words in
    }
    if (ui_current() != UI_PAGE_REVIEW) { fprintf(stderr, "page not read (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "53_page_list");
    fprintf(stderr, "page list:");
    for (int j = 0; j < ui_page_count(); j++) fprintf(stderr, " %s", ui_page_slot(j) >= 0 ? seed_word(ui_page_slot(j)) : "?");
    fprintf(stderr, "\n");
    ui_page_open(11);   // word 12: too faint to find, guessed from the gap in its column
    pump(300); save(dir, "53b_page_guessed");
    ui_goto(UI_PAGE_REVIEW);
    // Check the list like a person with the paper next to it: keep words that
    // match, remove pieces that aren't words, type unread or wrong ones, add
    // missing ones before the word that follows them.
    int k = 0, used = 0, guessed = 0, i = 0;
    bool shot_word = false, shot_guess = false;
    while (k < 24) {
        if (ui_current() != UI_PAGE_REVIEW) { fprintf(stderr, "not on the list (screen %d)\n", ui_current()); return 1; }
        if (i >= ui_page_count()) {
            ui_page_add();
            if (!ui_enter_word(page_words[k])) { fprintf(stderr, "added word %s refused\n", page_words[k]); return 1; }
            i++, k++;
            continue;
        }
        int w = ui_page_slot(i);
        if (w >= 0 && !strcmp(seed_word(w), page_words[k])) { i++, k++, used++; continue; }
        bool later = false;
        for (int j = k + 1; w >= 0 && j < 24; j++) later |= !strcmp(seed_word(w), page_words[j]);
        int next = -1;
        for (int j = i + 1; j < ui_page_count() && next < 0; j++) next = ui_page_slot(j);
        ui_page_open(i);
        if (!shot_word) { pump(300); save(dir, "54_page_word"); shot_word = true; }
        if (later) ui_page_insert();
        else if (w < 0 && next >= 0 && !strcmp(seed_word(next), page_words[k])) { ui_page_remove(); continue; }
        else if (ui_page_guess() >= 0 && !strcmp(seed_word(ui_page_guess()), page_words[k])) {   // the guess matches the paper
            if (!shot_guess) { pump(300); save(dir, "54a_page_guess"); shot_guess = true; }
            ui_page_use_guess();
            i++, k++, guessed++;
            continue;
        }
        else ui_page_type();
        if (!ui_enter_word(page_words[k])) { fprintf(stderr, "typed word %s refused\n", page_words[k]); return 1; }
        i++, k++;
    }
    while (ui_page_count() > 24) { ui_page_open(24); ui_page_remove(); }
    // The last word typed wrong: the checksum stops it, and its screen offers
    // the word that fits, read again from its picture.
    ui_page_open(23); ui_page_type();
    if (!ui_enter_word("zoo")) { fprintf(stderr, "zoo refused\n"); return 1; }
    if (ui_page_confirm()) { fprintf(stderr, "wrong last word accepted\n"); return 1; }
    ui_page_open(23);
    for (int i = 0; i < 40 && ui_page_suggestion() < 0; i++) pump(50);
    if (ui_page_suggestion() < 0 || strcmp(seed_word(ui_page_suggestion()), "test")) {
        fprintf(stderr, "no checksum suggestion for the last word\n");
        return 1;
    }
    pump(300); save(dir, "54b_page_suggestion");
    ui_page_use_suggestion();
    pump(300); save(dir, "55_page_ready");
    if (!ui_page_confirm() || ui_current() != UI_WALLET || !wallet_has_seed()) {
        fprintf(stderr, "page seed did not load\n");
        return 1;
    }
    fprintf(stderr, "page: %d of 24 words used as read, %d from a best guess\n", used, guessed);
    return 0;
}

// ------------------------------------------------------------ remote control
// For the end-to-end tests (e2e/): a test plays the user and the wallet app.
// It only gets what they get: it reads the screen, touches it, and holds
// pictures up to the camera. One command per line on stdin; the reply is any
// number of data lines, then "ok" or "err <why>".
//
//   widgets              what is on screen, one per line, tab separated:
//                        kind x1 y1 x2 y2 flags text
//                        kind: label, key (a keyboard key), slider, qr
//                        flags: "-", or "off" if scrolled out of view or disabled
//   tap X Y              touch and release
//   drag X1 Y1 X2 Y2     touch, move, release
//   wait MS              let time pass
//   camera [PATH]        hold a picture (PNG/JPEG) up to the camera; no path: take it away
//   shot PATH            save the screen as a PPM
//   screen               the UiScreen number
static lv_point_t touch_at;
static bool touch_down;

static void dump_text(const char *t) {
    for (; t && *t; t++) {
        if (*t == '\n') fputs("\\n", stdout);
        else if (*t == '\t') putchar(' ');
        else putchar(*t);
    }
}

static void dump_widgets(lv_obj_t *o, lv_area_t clip) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_area_t vis;
    bool on = lv_area_intersect(&vis, &a, &clip);
    const char *kind = lv_obj_check_type(o, &lv_label_class) ? "label"
                     : lv_obj_check_type(o, &lv_slider_class) ? "slider"
                     : lv_obj_check_type(o, &lv_qrcode_class) ? "qr" : nullptr;
    if (kind) {
        printf("%s\t%d\t%d\t%d\t%d\t%s\t", kind, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, on ? "-" : "off");
        if (kind[0] == 'l') dump_text(lv_label_get_text(o));
        putchar('\n');
    }
    if (lv_obj_check_type(o, &lv_buttonmatrix_class)) {
        lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)o;
        for (uint32_t i = 0; i < bm->btn_cnt; i++) {
            const lv_area_t *b = &bm->button_areas[i];
            bool dead = lv_buttonmatrix_has_button_ctrl(o, i, LV_BUTTONMATRIX_CTRL_DISABLED) ||
                        lv_buttonmatrix_has_button_ctrl(o, i, LV_BUTTONMATRIX_CTRL_HIDDEN);
            printf("key\t%d\t%d\t%d\t%d\t%s\t", (int)(a.x1 + b->x1), (int)(a.y1 + b->y1), (int)(a.x1 + b->x2),
                   (int)(a.y1 + b->y2), on && !dead ? "-" : "off");
            dump_text(lv_buttonmatrix_get_button_text(o, i));
            putchar('\n');
        }
    }
    if (!on) vis = a;   // children of something out of view are out of view too
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        if (on) dump_widgets(lv_obj_get_child(o, i), vis);
        else { lv_area_t none = {0, 0, -1, -1}; dump_widgets(lv_obj_get_child(o, i), none); }
    }
}

static int run_control() {
    fake_clock = true;
    headless_display();
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, [](lv_indev_t *, lv_indev_data_t *d) {
        d->point = touch_at;
        d->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
    cam_override = true;   // no picture until a "camera" command
    cam_ok = false;
    ui_build();
    pump(300);

    static char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *err = nullptr;
        int a, b, c, d;
        if (!strcmp(line, "widgets")) {
            lv_area_t all = {0, 0, 239, 319};
            dump_widgets(lv_screen_active(), all);
            dump_widgets(lv_layer_top(), all);   // the brightness panel
        } else if (sscanf(line, "tap %d %d", &a, &b) == 2) {
            touch_at.x = a; touch_at.y = b;
            touch_down = true;  pump(80);
            touch_down = false; pump(300);
        } else if (sscanf(line, "drag %d %d %d %d", &a, &b, &c, &d) == 4) {
            touch_at.x = a; touch_at.y = b;
            touch_down = true; pump(80);
            for (int i = 1; i <= 10; i++) {
                touch_at.x = a + (c - a) * i / 10;
                touch_at.y = b + (d - b) * i / 10;
                pump(40);
            }
            touch_down = false; pump(300);
        } else if (sscanf(line, "wait %d", &a) == 1) {
            pump(a);
        } else if (!strncmp(line, "camera", 6)) {
            cam_ok = false;
            if (line[6] == ' ') {
                cam_load(line + 7);
                if (!cam_ok) err = "cannot read the picture";
            }
        } else if (!strncmp(line, "page ", 5)) {   // a written page: the still is read from this file
            setenv("SIM_CAMERA", line + 5, 1);
            cam_override = false;
            cam_ok = false;
        } else if (!strncmp(line, "shot ", 5)) {
            if (!save_ppm(line + 5)) err = "cannot write the screenshot";
        } else if (!strcmp(line, "screen")) {
            printf("%d\n", (int)ui_current());
        } else {
            err = "unknown command";
        }
        if (err) printf("err %s\n", err);
        else puts("ok");
        fflush(stdout);
    }
    return 0;
}

int main() {
    lv_init();
    lv_tick_set_cb(now_ms);

    if (const char *dir = getenv("SIM_SHOTS")) return run_shots(dir);
    if (getenv("SIM_CONTROL")) return run_control();

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
