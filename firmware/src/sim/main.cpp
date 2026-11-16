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

void platform_camera_stop() {}

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

// Walk the flow: home, scan a 12-word Standard SeedQR, menu, xpub, wipe.
static int run_shots(const char *dir) {
    headless_display();

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
    wallet_set_testnet(true);
    ui_goto(UI_NETWORK);      pump(300); save(dir, "5b_network");
    ui_goto(UI_EXPORT_XPUB);  pump(300); save(dir, "5c_export_vpub");
    wallet_set_testnet(false);
    ui_goto(UI_KEY_TYPE);     pump(300); save(dir, "5d_key_type");
    wallet_set_export_multisig(true);
    ui_goto(UI_EXPORT_XPUB);  pump(300); save(dir, "5e_export_multisig");
    wallet_set_export_multisig(false);
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
    ui_goto(UI_BACKUP_BLOCK);   pump(300); save(dir, "21_backup_block_1");
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
    ui_goto(UI_RESTORE); pump(300); save(dir, "26_restore");
    ui_seed_entry(UI_ENTER_WORDS, 12);
    const char *words[] = {"crew", "marble", "private", "differ", "race", "truly", "blush", "basket", "crater", "affair", "prepare"};
    for (const char *w : words)
        if (!ui_enter_word(w)) { fprintf(stderr, "word %s refused\n", w); return 1; }
    pump(300); save(dir, "27_words_last");
    if (ui_enter_word("zoo")) { fprintf(stderr, "invalid last word accepted\n"); return 1; }
    if (!ui_enter_word("unique") || ui_current() != UI_SEED_LOADED) { fprintf(stderr, "words did not load\n"); return 1; }
    ui_goto(UI_SEED_MENU); pump(300); save(dir, "28_menu_backup");
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
    if (ui_current() != UI_SEED_LOADED || !wallet_has_seed()) { fprintf(stderr, "existing backup check failed\n"); return 1; }

    // BIP-85 child seed of the loaded seed: words, QR card, card check.
    ui_goto(UI_SEED_MENU);    pump(300); save(dir, "30_menu_child");
    ui_goto(UI_CHILD_LENGTH); pump(300); save(dir, "31_child_length");
    ui_goto(UI_CHILD_INDEX);
    // Keypad: the largest child number, a digit past it is ignored, Del back to 0.
    for (const char *d = "21474836479"; *d; d++) ui_child_digit(*d - '0');
    if (strcmp(ui_child_number(), "2147483647")) { fprintf(stderr, "child number not capped: %s\n", ui_child_number()); return 1; }
    for (int i = 0; i < 11; i++) ui_child_digit(-1);
    if (strcmp(ui_child_number(), "0")) { fprintf(stderr, "Del did not clear: %s\n", ui_child_number()); return 1; }
    ui_child_seed(12, 0x80000000u);   // past the hardened range: stays on the keypad with a note
    if (ui_current() != UI_CHILD_INDEX) { fprintf(stderr, "bad child number not refused\n"); return 1; }
    pump(300); save(dir, "32b_child_failed");
    ui_child_digit(0);
    ui_child_digit(3);
    pump(300); save(dir, "32_child_index");
    ui_child_seed(12, (uint32_t)strtoul(ui_child_number(), nullptr, 10));
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
    if (!ui_enter_word("about") || ui_current() != UI_SEED_LOADED) { fprintf(stderr, "abandon seed did not load\n"); return 1; }
    pump(300); save(dir, "35_seed_loaded_link");
    ui_goto(UI_PASSPHRASE); pump(300); save(dir, "36_passphrase_empty");
    ui_passphrase_key("OK");          // empty and none set: nothing happens
    if (ui_current() != UI_SEED_LOADED || wallet_has_passphrase()) { fprintf(stderr, "empty passphrase was applied\n"); return 1; }
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
    if (ui_current() != UI_SEED_LOADED || !wallet_has_passphrase() || strcmp(pfp, "b4e3f5ed")) {
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
    if (ui_current() != UI_SEED_LOADED) { fprintf(stderr, "multisig seed did not load\n"); return 1; }
    static uint8_t ms_psbt[4096];
    size_t ms_psbt_len = 0;
    if (wally_base64_to_bytes(MS_PSBT, 0, ms_psbt, sizeof(ms_psbt), &ms_psbt_len) != WALLY_OK) return 1;
    // The transaction as an animated UR, part after part, until the scanner leaves.
    auto show_psbt = [&]() {
        static char part[600];
        for (uint32_t seq = 1; seq < 200 && ui_current() == UI_SCAN_PSBT; seq++) {
            if (!ur_encode_part(ms_psbt, ms_psbt_len, 100, seq, part, sizeof(part))) break;
            cam_show_qr(part);
            pump(250);
        }
    };
    ui_goto(UI_SCAN_PSBT);
    show_psbt();
    if (ui_current() != UI_PSBT_ERROR) { fprintf(stderr, "multisig PSBT without descriptor: screen %d\n", ui_current()); return 1; }
    pump(300); save(dir, "43_multisig_needs_descriptor");
    ui_goto(UI_SCAN_PSBT);
    static char ur_upper[1024];   // upper case: a smaller, alphanumeric QR
    for (size_t i = 0; i <= strlen(MS_UR_OUTPUT); i++) ur_upper[i] = char(toupper((unsigned char)MS_UR_OUTPUT[i]));
    cam_show_qr(ur_upper);
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_PSBT; i++) pump(50);
    if (ui_current() != UI_MULTISIG) { save(dir, "debug_descriptor_scan"); fprintf(stderr, "descriptor was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "44_multisig_wallet");
    // Leaving without "Use wallet" drops it: the transaction is still refused.
    ui_goto(UI_SCAN_PSBT);
    show_psbt();
    if (ui_current() != UI_PSBT_ERROR) { fprintf(stderr, "unconfirmed descriptor was used\n"); return 1; }
    ui_goto(UI_SCAN_PSBT);
    cam_show_qr(MS_DESCRIPTOR);   // this time as plain text
    for (int i = 0; i < 100 && ui_current() == UI_SCAN_PSBT; i++) pump(50);
    if (ui_current() != UI_MULTISIG) { save(dir, "debug_descriptor_text"); fprintf(stderr, "text descriptor was not accepted (screen %d)\n", ui_current()); return 1; }
    ui_multisig_use();
    if (ui_current() != UI_SCAN_PSBT) { fprintf(stderr, "Use wallet did not return to the scanner\n"); return 1; }
    show_psbt();
    if (ui_current() != UI_PSBT_OVERVIEW) { fprintf(stderr, "multisig PSBT was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "45_multisig_overview");
    ui_goto(UI_PSBT_OUTPUT); pump(300); save(dir, "46_multisig_change");
    ui_next_output();
    ui_next_output();
    ui_next_output();        pump(300); save(dir, "47_multisig_sign");
    if (!ui_swipe_sign()) { fprintf(stderr, "multisig signing failed\n"); return 1; }
    pump(400); save(dir, "48_multisig_signed");
    ui_goto(UI_SEED_MENU);   pump(300); save(dir, "49_menu_multisig");
    ui_goto(UI_SCAN_ADDRESS);
    cam_show_qr(MS_ADDR_CHANGE_4);
    for (int i = 0; i < 400 && !(ui_current() == UI_VERIFY_RESULT && verify_done()); i++) pump(30);
    if (ui_current() != UI_VERIFY_RESULT) { fprintf(stderr, "multisig address was not accepted (screen %d)\n", ui_current()); return 1; }
    pump(300); save(dir, "50_multisig_verify");
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
