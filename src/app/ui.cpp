#include "ui.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include "qrscan.h"
#include "seedqr.h"
#include "wallet.h"

// ---------------------------------------------------------------- styling
static const lv_color_t ACCENT = LV_COLOR_MAKE(0xF0, 0x73, 0x10);
static const lv_color_t DANGER = LV_COLOR_MAKE(0xD9, 0x3A, 0x2B);
static const lv_color_t MUTED = LV_COLOR_MAKE(0x8A, 0x8A, 0x85);

static UiScreen current = UI_HOME;

static lv_obj_t *new_screen(const char *title) {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = lv_label_create(scr);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    }
    return scr;
}

static void show(lv_obj_t *scr) {
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// Full-width button, 48px tall (comfortable for a fingertip on a 2" screen).
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud = nullptr) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 208, 48);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

static void go_cb(lv_event_t *e) { ui_goto((UiScreen)(intptr_t)lv_event_get_user_data(e)); }
#define GO(s) ((void *)(intptr_t)(s))

// ---------------------------------------------------------------- home
static void build_home() {
    lv_obj_t *scr = new_screen(nullptr);
    lv_obj_t *t = label(scr, "Stateless\nsigner", &lv_font_montserrat_28, lv_color_white());
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *s = label(scr, "Your seed is never stored.\nUnplug to wipe.", &lv_font_montserrat_14, MUTED);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_align(button(scr, "Scan SeedQR", ACCENT, go_cb, GO(UI_SCAN_SEED)), LV_ALIGN_BOTTOM_MID, 0, -28);
    show(scr);
}

// ---------------------------------------------------------------- scanning
// Camera preview, downscaled for display. The full frame goes to quirc.
#define PREV_W 200
#define PREV_H 150
static uint8_t preview_buf[PREV_W * PREV_H];
static lv_image_dsc_t preview_dsc;
static lv_obj_t *preview_img, *scan_msg;
static lv_timer_t *scan_timer;
static bool scanning_seed;
static int qr_w, qr_h;

static void scan_stop() {
    if (scan_timer) lv_timer_delete(scan_timer);
    scan_timer = nullptr;
    qrscan_free();
    qr_w = qr_h = 0;
    memset(preview_buf, 0, sizeof(preview_buf));
}

static bool looks_like_psbt(const QrPayload &p) {
    if (p.len >= 14 && strncasecmp((const char *)p.data, "UR:CRYPTO-PSBT", 14) == 0) return true;
    if (p.len >= 6 && memcmp(p.data, "cHNidP", 6) == 0) return true;      // base64 "psbt\xff"
    if (p.len >= 4 && strncasecmp((const char *)p.data, "B$", 2) == 0) return true;  // BBQr
    return false;
}

static void scan_set_msg(const char *m, lv_color_t c) {
    lv_label_set_text(scan_msg, m);
    lv_obj_set_style_text_color(scan_msg, c, 0);
}

static void handle_payload(QrPayload &p) {
    SeedEntropy e;
    SeedQrResult r = seedqr_decode(p.data, p.len, &e);
    bool is_psbt = looks_like_psbt(p);
    memset(&p, 0, sizeof(p));

    if (scanning_seed) {
        if (r == SEEDQR_OK) {
            scan_stop();
            scan_set_msg("Loading seed...", lv_color_white());
            lv_refr_now(nullptr);
            bool ok = wallet_load(e);
            seedqr_wipe(&e);
            ui_goto(ok ? UI_SEED_LOADED : UI_HOME);
            return;
        }
        seedqr_wipe(&e);
        if (r == SEEDQR_BAD_CHECKSUM || r == SEEDQR_BAD_WORD_INDEX)
            scan_set_msg("Invalid SeedQR (checksum)", DANGER);
        else if (is_psbt)
            scan_set_msg("That's a transaction.\nScan your SeedQR first.", DANGER);
        else
            scan_set_msg("Not a SeedQR", DANGER);
        return;
    }

    // Transaction mode
    seedqr_wipe(&e);
    if (r == SEEDQR_OK) {
        scan_set_msg("That's a seed.\nA seed is already loaded.", DANGER);
    } else if (is_psbt) {
        scan_set_msg("Transaction found.\nReview and signing: next step.", ACCENT);
    } else {
        scan_set_msg("Not a transaction", DANGER);
    }
}

static void scan_tick(lv_timer_t *) {
    int w, h;
    const uint8_t *frame = platform_camera_frame(&w, &h);
    if (!frame) return;
    if (w != qr_w || h != qr_h) {
        if (!qrscan_init(w, h)) { scan_set_msg("Out of memory", DANGER); return; }
        qr_w = w; qr_h = h;
    }
    // nearest-neighbour preview, cropped to 4:3
    for (int y = 0; y < PREV_H; y++)
        for (int x = 0; x < PREV_W; x++)
            preview_buf[y * PREV_W + x] = frame[(y * h / PREV_H) * w + (x * w / PREV_W)];
    lv_obj_invalidate(preview_img);

    QrPayload p;
    if (qrscan_frame(frame, &p)) handle_payload(p);
}

static void scan_cancel_cb(lv_event_t *) {
    scan_stop();
    ui_goto(scanning_seed ? UI_HOME : UI_SEED_MENU);
}

static void build_scan(bool seed) {
    scanning_seed = seed;
    lv_obj_t *scr = new_screen(seed ? "Scan SeedQR" : "Scan transaction");

    preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_dsc.header.cf = LV_COLOR_FORMAT_L8;
    preview_dsc.header.w = PREV_W;
    preview_dsc.header.h = PREV_H;
    preview_dsc.header.stride = PREV_W;
    preview_dsc.data_size = sizeof(preview_buf);
    preview_dsc.data = preview_buf;
    preview_img = lv_image_create(scr);
    lv_image_set_src(preview_img, &preview_dsc);
    lv_obj_align(preview_img, LV_ALIGN_TOP_MID, 0, 52);

    scan_msg = label(scr, seed ? "Hold your SeedQR\nin front of the camera" : "Show the PSBT QR\nfrom your wallet",
                     &lv_font_montserrat_14, MUTED);
    lv_obj_align(scan_msg, LV_ALIGN_TOP_MID, 0, 212);

    lv_obj_align(button(scr, "Cancel", lv_color_hex(0x333333), scan_cancel_cb), LV_ALIGN_BOTTOM_MID, 0, -16);
    show(scr);
    scan_timer = lv_timer_create(scan_tick, 100, nullptr);
}

// ---------------------------------------------------------------- seed screens
static void build_seed_loaded() {
    lv_obj_t *scr = new_screen("Seed loaded");
    char fp[9];
    wallet_fingerprint_hex(fp);
    lv_obj_t *cap = label(scr, "Fingerprint", &lv_font_montserrat_14, MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_t *f = label(scr, fp, &lv_font_montserrat_28, ACCENT);
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, 92);
    char info[64];
    snprintf(info, sizeof(info), "%d words, no passphrase\nCheck it matches your wallet.", wallet_word_count());
    lv_obj_t *i = label(scr, info, &lv_font_montserrat_14, MUTED);
    lv_obj_align(i, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_align(button(scr, "Continue", ACCENT, go_cb, GO(UI_SEED_MENU)), LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_align(button(scr, "Wipe", lv_color_hex(0x333333), go_cb, GO(UI_WIPE_CONFIRM)), LV_ALIGN_BOTTOM_MID, 0, -20);
    show(scr);
}

static void build_seed_menu() {
    char fp[9];
    wallet_fingerprint_hex(fp);
    lv_obj_t *scr = new_screen(fp);
    lv_obj_t *cap = label(scr, "Seed loaded", &lv_font_montserrat_14, MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_align(button(scr, "Scan transaction", ACCENT, go_cb, GO(UI_SCAN_PSBT)), LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_align(button(scr, "Export xpub", lv_color_hex(0x333333), go_cb, GO(UI_EXPORT_XPUB)), LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_align(button(scr, "Wipe seed", DANGER, go_cb, GO(UI_WIPE_CONFIRM)), LV_ALIGN_BOTTOM_MID, 0, -20);
    show(scr);
}

static void build_export_xpub() {
    lv_obj_t *scr = new_screen("Export xpub");
    static char xpub[180];
    size_t n = wallet_export_xpub(xpub, sizeof(xpub));
    lv_obj_t *qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, 196);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, xpub, n);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t *d = label(scr, "Native segwit  m/84'/0'/0'", &lv_font_montserrat_14, MUTED);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 254);
    lv_obj_align(button(scr, "Done", lv_color_hex(0x333333), go_cb, GO(UI_SEED_MENU)), LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_height(lv_obj_get_child(scr, -1), 40);
    show(scr);
}

static void wipe_cb(lv_event_t *) {
    wallet_wipe();
    ui_goto(UI_HOME);
}

static void build_wipe_confirm() {
    lv_obj_t *scr = new_screen("Wipe seed?");
    char fp[9], msg[96];
    wallet_fingerprint_hex(fp);
    snprintf(msg, sizeof(msg), "Removes seed %s\nfrom memory.\n\nYou can load it again\nfrom your SeedQR.", fp);
    lv_obj_t *m = label(scr, msg, &lv_font_montserrat_14, MUTED);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_align(button(scr, "Wipe", DANGER, wipe_cb), LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_align(button(scr, "Keep", lv_color_hex(0x333333), go_cb, GO(UI_SEED_MENU)), LV_ALIGN_BOTTOM_MID, 0, -20);
    show(scr);
}

// ---------------------------------------------------------------- navigation
void ui_goto(UiScreen s) {
    if (s != UI_SCAN_SEED && s != UI_SCAN_PSBT) scan_stop();
    // Screens that need a seed fall back to home without one, and vice versa.
    bool needs_seed = s == UI_SEED_LOADED || s == UI_SEED_MENU || s == UI_SCAN_PSBT ||
                      s == UI_EXPORT_XPUB || s == UI_WIPE_CONFIRM;
    if (needs_seed && !wallet_has_seed()) s = UI_HOME;
    if ((s == UI_HOME || s == UI_SCAN_SEED) && wallet_has_seed()) s = UI_SEED_MENU;
    current = s;
    switch (s) {
        case UI_HOME: build_home(); break;
        case UI_SCAN_SEED: build_scan(true); break;
        case UI_SEED_LOADED: build_seed_loaded(); break;
        case UI_SEED_MENU: build_seed_menu(); break;
        case UI_SCAN_PSBT: build_scan(false); break;
        case UI_EXPORT_XPUB: build_export_xpub(); break;
        case UI_WIPE_CONFIRM: build_wipe_confirm(); break;
    }
}

UiScreen ui_current() { return current; }

void ui_build() { ui_goto(UI_HOME); }
