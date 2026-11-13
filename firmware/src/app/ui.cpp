#include "ui.h"
#include <lvgl.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <Conversion.h>   // fromBase64 (uBitcoin)
#include "address_verify.h"
#include "amount.h"
#include "psbt_review.h"
#include "seed_create.h"
#include "qrscan.h"
#include "seedqr.h"
#include "theme.h"
#include "ur.h"
#include "wallet.h"
#include "src/libs/qrcode/qrcodegen.h"

using namespace th;

static UiScreen current = UI_HOME;
static bool fresh_backup;                // Seed loaded screen follows a checked backup

static void show(lv_obj_t *scr) { lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, true); }
static void go_cb(lv_event_t *e) { ui_goto((UiScreen)(intptr_t)lv_event_get_user_data(e)); }
static void to_menu_cb(lv_event_t *) { ui_goto(UI_SEED_MENU); }
static void to_home_cb(lv_event_t *) { ui_goto(UI_HOME); }
#define GO(s) ((void *)(intptr_t)(s))

static void place_bottom(lv_obj_t *o) { lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -M); }

// Secondary + primary side by side at the bottom.
static void bottom_pair(lv_obj_t *secondary, lv_obj_t *primary) {
    lv_obj_set_size(secondary, 76, 48);
    lv_obj_set_width(primary, CW - 76 - 8);
    lv_obj_align(secondary, LV_ALIGN_BOTTOM_LEFT, M, -M);
    lv_obj_align(primary, LV_ALIGN_BOTTOM_RIGHT, -M, -M);
    lv_obj_set_style_bg_opa(secondary, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(secondary, SURFACE_2, 0);
}

// The space from y down to the bottom buttons. Scrolls if what goes in it is
// taller (only for huge amounts), so nothing runs under the buttons.
static lv_obj_t *scroll_area(lv_obj_t *scr, int y) {
    lv_obj_t *a = lv_obj_create(scr);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, CW, H - M - BTN_H - 4 - y);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_scroll_dir(a, LV_DIR_VER);
    lv_obj_set_style_width(a, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(a, TEXT_3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(a, 2, LV_PART_SCROLLBAR);
    return a;
}

// White rounded card holding a QR code (the white is the quiet zone).
static lv_obj_t *qr_card(lv_obj_t *parent, int y, lv_obj_t **qr_out) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 176, 176);
    lv_obj_set_style_bg_color(c, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *qr = lv_qrcode_create(c);
    lv_qrcode_set_size(qr, 154);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_center(qr);
    *qr_out = qr;
    return c;
}

// ---------------------------------------------------------------- home
static void build_home() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_QR, ACCENT, ACCENT_DIM, 76), LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_align(text(scr, "Signer", &inter_30_semi, TEXT), LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_align(para(scr, "Your seed stays in memory only, until you unplug."), LV_ALIGN_TOP_MID, 0, 146);
    // Two secondary ways in, same height as the primary so the three read as a set.
    const char *labels[] = {"Enter words", "New seed"};
    const UiScreen dest[] = {UI_RESTORE, UI_NEW_SEED};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = btn_ghost(scr, labels[i], go_cb, GO(dest[i]), TEXT);
        lv_obj_set_size(b, (CW - 8) / 2, 48);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_align(b, i ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, i ? -M : M, -M - 48 - 8);
    }
    place_bottom(btn_primary(scr, ICON_CAMERA "   Scan QR", go_cb, GO(UI_SCAN_SEED)));
    show(scr);
}

// ---------------------------------------------------------------- scanning
#define PREV_W 240
#define PREV_H 180
static uint8_t preview_buf[PREV_W * PREV_H];
static lv_image_dsc_t preview_dsc;
static lv_obj_t *preview_img, *scan_msg, *scan_sub, *scan_bar;
static lv_timer_t *scan_timer;
enum ScanMode { SCAN_SEED, SCAN_PSBT, SCAN_ADDRESS, SCAN_CONFIRM };
static ScanMode scan_mode;
static AddressCheck check;
static int qr_w, qr_h;
static UrDecoder ur_dec;
static PsbtReview review;
static int out_index;

static void scan_stop() {
    if (scan_timer) lv_timer_delete(scan_timer);
    scan_timer = nullptr;
    qrscan_free();
    qr_w = qr_h = 0;
    memset(preview_buf, 0, sizeof(preview_buf));
}

static bool starts_with_ci(const QrPayload &p, const char *s) {
    size_t n = strlen(s);
    return p.len >= n && strncasecmp((const char *)p.data, s, n) == 0;
}

static bool looks_like_psbt(const QrPayload &p) {
    return starts_with_ci(p, "ur:crypto-psbt") || starts_with_ci(p, "cHNidP") || starts_with_ci(p, "B$") ||
           (p.len >= 5 && memcmp(p.data, "psbt\xff", 5) == 0);
}

static void scan_status(const char *msg, const char *sub, lv_color_t c) {
    lv_label_set_text(scan_msg, msg);
    lv_obj_set_style_text_color(scan_msg, c, 0);
    lv_label_set_text(scan_sub, sub ? sub : "");
}

static void psbt_complete(const uint8_t *data, size_t len) {
    scan_stop();
    psbt_load(data, len, &review);
    ur_decoder_reset(&ur_dec);
    ui_goto(review.error == PSBT_OK ? UI_PSBT_OVERVIEW : UI_PSBT_ERROR);
}

static void handle_seed_payload(QrPayload &p) {
    SeedEntropy e;
    SeedQrResult r = seedqr_decode(p.data, p.len, &e);
    bool is_psbt = looks_like_psbt(p);
    memset(&p, 0, sizeof(p));
    if (r == SEEDQR_OK) {
        scan_stop();
        scan_status("Loading seed" "\xE2\x80\xA6", "This takes a few seconds", TEXT);
        lv_refr_now(nullptr);
        bool ok = wallet_load(e);
        seedqr_wipe(&e);
        ui_goto(ok ? UI_SEED_LOADED : UI_HOME);
        return;
    }
    seedqr_wipe(&e);
    if (r == SEEDQR_BAD_CHECKSUM || r == SEEDQR_BAD_WORD_INDEX) scan_status("Damaged QR", "The checksum is wrong", RED);
    else if (is_psbt) scan_status("That's a transaction", "Load your seed first", AMBER);
    else scan_status("No seed found", "This QR doesn't hold a seed", AMBER);
}

static void handle_psbt_payload(QrPayload &p) {
    static uint8_t raw[UR_MAX_MESSAGE];
    if (starts_with_ci(p, "ur:")) {
        const uint8_t *payload; size_t plen;
        UrResult r = ur_decoder_add(&ur_dec, (const char *)p.data, p.len, &payload, &plen);
        if (r == UR_COMPLETE) { psbt_complete(payload, plen); return; }
        if (r == UR_PROGRESS) {
            char m[32];
            snprintf(m, sizeof(m), "%d%% received", ur_decoder_percent(&ur_dec));
            scan_status(m, "Hold steady until it's done", ACCENT);
            lv_obj_remove_flag(scan_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(scan_bar, ur_decoder_percent(&ur_dec), LV_ANIM_ON);
        } else if (r == UR_WRONG_TYPE) {
            scan_status("Not a transaction", "Show the transaction QR instead", AMBER);
        }
        return;
    }
    if (starts_with_ci(p, "cHNidP")) {
        size_t n = fromBase64((const char *)p.data, p.len, raw, sizeof(raw));
        if (n) { psbt_complete(raw, n); return; }
    }
    if (p.len >= 5 && memcmp(p.data, "psbt\xff", 5) == 0) { psbt_complete(p.data, p.len); return; }
    if (starts_with_ci(p, "B$")) { scan_status("BBQr isn't supported yet", "Switch your wallet to UR", AMBER); return; }

    SeedEntropy e;
    bool seed = seedqr_decode(p.data, p.len, &e) == SEEDQR_OK;
    seedqr_wipe(&e);
    if (seed) scan_status("That's a seed", "A seed is already loaded", AMBER);
    else scan_status("Not a transaction", "Show the transaction QR instead", AMBER);
}

static void handle_address_payload(QrPayload &p) {
    AddressParse r = address_parse(p.data, p.len, &check);
    if (r == ADDR_OK) {
        scan_stop();
        ui_goto(UI_VERIFY_RESULT);
        return;
    }
    if (r == ADDR_UNSUPPORTED) { scan_status("Not supported yet", "Only bc1q addresses for now", AMBER); return; }
    if (looks_like_psbt(p)) { scan_status("That's a transaction", "Use Sign transaction instead", AMBER); return; }
    SeedEntropy e;
    bool seed = seedqr_decode(p.data, p.len, &e) == SEEDQR_OK;
    seedqr_wipe(&e);
    if (seed) scan_status("That's a seed", "Show a receive address instead", AMBER);
    else scan_status("Not an address", "Show the receive address QR", AMBER);
}

static void handle_confirm_payload(QrPayload &p);

static void scan_tick(lv_timer_t *) {
    int w, h;
    const uint8_t *frame = platform_camera_frame(&w, &h);
    if (!frame) return;
    if (w != qr_w || h != qr_h) {
        if (!qrscan_init(w, h)) { scan_status("Camera error", "Restart the device", RED); return; }
        qr_w = w; qr_h = h;
    }
    for (int y = 0; y < PREV_H; y++)
        for (int x = 0; x < PREV_W; x++)
            preview_buf[y * PREV_W + x] = frame[(y * h / PREV_H) * w + (x * w / PREV_W)];
    lv_obj_invalidate(preview_img);

    static QrPayload p;   // static: 3KB is a lot of stack on the RP2350
    if (!qrscan_frame(frame, &p)) return;
    if (scan_mode == SCAN_SEED) handle_seed_payload(p);
    else if (scan_mode == SCAN_PSBT) handle_psbt_payload(p);
    else if (scan_mode == SCAN_CONFIRM) handle_confirm_payload(p);
    else handle_address_payload(p);
    memset(&p, 0, sizeof(p));
}

static void scan_back_cb(lv_event_t *) {
    scan_stop();
    ur_decoder_reset(&ur_dec);
    ui_goto(scan_mode == SCAN_SEED ? UI_HOME : scan_mode == SCAN_CONFIRM ? UI_BACKUP_BLOCK : UI_SEED_MENU);
}

// Viewfinder corner brackets around the target area of the preview.
static void corners(lv_obj_t *parent, int cx, int cy, int size) {
    const int L = 22, T = 3, h = size / 2;
    for (int i = 0; i < 4; i++) {
        int sx = (i & 1) ? 1 : -1, sy = (i & 2) ? 1 : -1;
        int x0 = cx + sx * h, y0 = cy + sy * h;
        for (int k = 0; k < 2; k++) {
            lv_obj_t *a = lv_obj_create(parent);
            lv_obj_remove_style_all(a);
            lv_obj_set_style_bg_color(a, ACCENT, 0);
            lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(a, 2, 0);
            int w = k ? T : L, hh = k ? L : T;
            lv_obj_set_size(a, w, hh);
            lv_obj_set_pos(a, sx < 0 ? x0 : x0 - w, sy < 0 ? y0 : y0 - hh);
        }
    }
}

static void build_scan(ScanMode mode) {
    scan_mode = mode;
    bool seed = mode == SCAN_SEED;
    if (mode == SCAN_PSBT) ur_decoder_reset(&ur_dec);
    lv_obj_t *scr = screen();
    static const char *titles[] = {"Scan seed", "Scan transaction", "Verify address", "Check card"};
    static const char *prompts[] = {"Point at your seed QR", "Point at your wallet's QR", "Point at the address QR",
                                    "Point at your new card"};
    static const char *hints[] = {"Hold it flat and well lit", "Sparrow, Nunchuk or any PSBT wallet",
                                  "The receive address in your wallet", "It must hold the seed you just made"};
    topbar(scr, titles[mode], scan_back_cb);

    preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_dsc.header.cf = LV_COLOR_FORMAT_L8;
    preview_dsc.header.w = PREV_W;
    preview_dsc.header.h = PREV_H;
    preview_dsc.header.stride = PREV_W;
    preview_dsc.data_size = sizeof(preview_buf);
    preview_dsc.data = preview_buf;
    preview_img = lv_image_create(scr);
    lv_image_set_src(preview_img, &preview_dsc);
    lv_obj_set_pos(preview_img, 0, BAR);
    corners(scr, W / 2, BAR + PREV_H / 2, 150);

    scan_msg = text(scr, prompts[mode], &inter_17_semi, TEXT);
    lv_obj_align(scan_msg, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + 14);
    scan_sub = text(scr, hints[mode], &inter_13, TEXT_3);
    (void)seed;
    lv_obj_align(scan_sub, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + 40);

    scan_bar = lv_bar_create(scr);
    lv_obj_set_size(scan_bar, CW, 4);
    lv_bar_set_range(scan_bar, 0, 100);
    lv_obj_set_style_bg_color(scan_bar, SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan_bar, ACCENT, LV_PART_INDICATOR);
    lv_obj_align(scan_bar, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(scan_bar, LV_OBJ_FLAG_HIDDEN);

    show(scr);
    scan_timer = lv_timer_create(scan_tick, 100, nullptr);
}

// ---------------------------------------------------------------- seed screens
// Small quiet text button, e.g. for destructive actions that shouldn't compete
// with the main action. Red only appears on the confirmation screen.
static lv_obj_t *quiet_link(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, SURFACE_2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = text(b, s, &inter_13, TEXT_3);
    lv_obj_center(l);
    return b;
}

static void build_seed_loaded() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_CHECK, GREEN, GREEN_DIM, 52), LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_align(text(scr, fresh_backup ? "Backup checked" : "Seed loaded", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, 92);

    // The fingerprint is what the user checks, so it gets the space.
    lv_obj_align(caption(scr, "FINGERPRINT"), LV_ALIGN_TOP_MID, 0, 138);
    char fp[9], spaced[12];
    wallet_fingerprint_hex(fp);
    snprintf(spaced, sizeof(spaced), "%.4s %.4s", fp, fp + 4);
    lv_obj_align(text(scr, spaced, &mono_26, TEXT), LV_ALIGN_TOP_MID, 0, 156);
    if (fresh_backup) lv_obj_align(para(scr, "Write it on your card.", &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, 194);
    fresh_backup = false;

    place_bottom(btn_primary(scr, "Continue", go_cb, GO(UI_SEED_MENU)));
    show(scr);
}

static void backup_existing_cb(lv_event_t *);

static void build_seed_menu() {
    lv_obj_t *scr = screen();
    char fp[9];
    wallet_fingerprint_hex(fp);
    // Account header: key badge, then title and fingerprint left-aligned.
    lv_obj_t *badge = lv_obj_create(scr);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 40, 40);
    lv_obj_set_style_radius(badge, 12, 0);
    lv_obj_set_style_bg_color(badge, ACCENT_DIM, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_pos(badge, M, 16);
    lv_obj_center(text(badge, ICON_KEY, &inter_17_semi, ACCENT));
    lv_obj_t *title = text(scr, "Wallet", &inter_17_semi, TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(title, M + 52, 15);
    char spaced[12];
    snprintf(spaced, sizeof(spaced), "%.4s %.4s", fp, fp + 4);
    lv_obj_t *sub = text(scr, spaced, &mono_16, TEXT_2, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(sub, M + 52, 36);

    // Four rows: a little shorter than list_row's default so Wipe still fits.
    const int RH = 50;
    lv_obj_t *c = card(scr, CW, 4 * RH + 3);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 66);
    struct { const char *icon, *title; UiScreen s; } rows[] = {
        {ICON_SIGN, "Sign transaction", UI_SCAN_PSBT},
        {ICON_CHECK_CIRCLE, "Verify address", UI_SCAN_ADDRESS},
        {ICON_WALLET, "Connect wallet", UI_EXPORT_XPUB},
        {ICON_QR, "Back up as QR", UI_BACKUP_WARNING},
    };
    for (int i = 0; i < 4; i++) {
        if (i) divider(c, i * (RH + 1) - 1);
        lv_obj_t *r = list_row(c, rows[i].icon, rows[i].title, nullptr, i == 3 ? backup_existing_cb : go_cb, GO(rows[i].s));
        lv_obj_set_height(r, RH);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, i * (RH + 1));
    }

    lv_obj_align(quiet_link(scr, ICON_TRASH "  Wipe now", go_cb, GO(UI_WIPE_CONFIRM)), LV_ALIGN_BOTTOM_MID, 0, -6);
    show(scr);
}

static void build_export_xpub() {
    lv_obj_t *scr = screen();
    bool tn = wallet_testnet();
    topbar(scr, "Connect wallet", to_menu_cb);

    static char xpub[180];
    size_t n = wallet_export_xpub(xpub, sizeof(xpub));
    lv_obj_t *qr;
    qr_card(scr, BAR + 6, &qr);
    lv_qrcode_update(qr, xpub, n);

    lv_obj_align(text(scr, "Scan with your wallet app", &inter_15, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 6 + 176 + 10);
    lv_obj_align(text(scr, tn ? "vpub  \xC2\xB7  Native SegWit  \xC2\xB7  m/84'/1'/0'"
                              : "zpub  \xC2\xB7  Native SegWit  \xC2\xB7  m/84'/0'/0'", &inter_13, tn ? AMBER : TEXT_3),
                 LV_ALIGN_TOP_MID, 0, BAR + 6 + 176 + 32);
    lv_obj_align(quiet_link(scr, tn ? "Network: Testnet  " ICON_RIGHT : "Network: Mainnet  " ICON_RIGHT, go_cb, GO(UI_NETWORK)),
                 LV_ALIGN_BOTTOM_MID, 0, -2);
    show(scr);
}

// ---------------------------------------------------------------- network
static void to_export_cb(lv_event_t *) { ui_goto(UI_EXPORT_XPUB); }
static void network_cb(lv_event_t *e) {
    wallet_set_testnet(lv_event_get_user_data(e) != nullptr);
    ui_goto(UI_EXPORT_XPUB);
}

static void build_network() {
    lv_obj_t *scr = screen();
    topbar(scr, "Network", to_export_cb);
    lv_obj_t *c = card(scr, CW, 2 * 62 + 1);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 8);
    bool tn = wallet_testnet();
    lv_obj_t *r1 = list_row(c, ICON_WALLET, "Mainnet", nullptr, network_cb, nullptr);
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 0);
    divider(c, 62);
    lv_obj_t *r2 = list_row(c, ICON_WALLET, "Testnet", nullptr, network_cb, (void *)1);
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 63);
    lv_obj_align(text(tn ? r2 : r1, ICON_CHECK, &inter_17_semi, ACCENT), LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_align(para(scr, "Testnet also covers signet and Mutinynet.", &inter_13, TEXT_3),
                 LV_ALIGN_TOP_MID, 0, BAR + 8 + 125 + 16);
    show(scr);
}

static void wipe_cb(lv_event_t *) {
    psbt_clear();
    wallet_wipe();
    ui_goto(UI_HOME);
}

static void build_wipe_confirm() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_TRASH, RED, RED_DIM, 64), LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_align(text(scr, "Wipe seed?", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, 126);
    char fp[9], msg[128];
    wallet_fingerprint_hex(fp);
    snprintf(msg, sizeof(msg), "This removes %s from the device. Scan your seed QR to use it again.", fp);
    lv_obj_align(para(scr, msg), LV_ALIGN_TOP_MID, 0, 162);
    bottom_pair(btn_ghost(scr, "Cancel", go_cb, GO(UI_SEED_MENU), TEXT), btn_danger(scr, "Wipe", wipe_cb));
    show(scr);
}

// ---------------------------------------------------------------- new seed, restore, backup
// New seed is a tool: entropy only ever comes from the user (dice, coins, or
// words drawn by hand with coin flips for the last word, see seed_create.h),
// then the user writes down the words and optionally copies a QR card. The
// new seed is never loaded: the user loads it like any other, by Scan QR or
// Enter words. Enter words loads straight away. "Back up as QR" runs the card
// flow for the loaded seed.
static int new_words = 12;               // 12 or 24
static char entropy_in[257];             // dice '1'..'6' or coins '1'/'0' so far
static int entropy_n;
static bool entropy_dice;
static uint16_t word_idx[24];
static int word_n;
static char word_prefix[9];
static uint8_t last_ok[256];             // bitmap: valid last words for word_idx
static SeedEntropy pending;              // len 0: none
static uint8_t qr_buf[qrcodegen_BUFFER_LEN_FOR_VERSION(2)];   // the pending seed's SeedQR
static int qr_n;                         // modules per side, 0: none
static int block_i;
static bool backup_existing;             // backing up the loaded seed, not a new one
static bool drawing;                     // Draw words: n-1 words by hand, then coins for the last
static bool final_flips;                 // the coin screen is collecting the last word's bits
static bool card_checked;                // Seed ready follows a checked card
static int words_page;                   // Write down words: 4 per page

static void secure_zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static void new_seed_wipe() {
    seedqr_wipe(&pending);
    secure_zero(entropy_in, sizeof(entropy_in));
    secure_zero(word_idx, sizeof(word_idx));
    secure_zero(word_prefix, sizeof(word_prefix));
    secure_zero(last_ok, sizeof(last_ok));
    secure_zero(qr_buf, sizeof(qr_buf));
    entropy_n = word_n = qr_n = block_i = words_page = 0;
    backup_existing = drawing = final_flips = card_checked = false;
}

void ui_seed_entry(UiScreen method, int words, bool draw) {
    new_seed_wipe();
    new_words = words == 24 ? 24 : 12;
    drawing = draw;
    ui_goto(method);
}

static void words_cb(lv_event_t *e) {
    new_words = (int)(intptr_t)lv_event_get_user_data(e);
    ui_goto(UI_NEW_SEED);
}
static void method_cb(lv_event_t *e) { ui_seed_entry((UiScreen)(intptr_t)lv_event_get_user_data(e), new_words); }
static void draw_cb(lv_event_t *) { ui_seed_entry(UI_ENTER_WORDS, new_words, true); }
static void to_new_seed_cb(lv_event_t *) { ui_seed_entry(UI_NEW_SEED, new_words); }
static void restore_cb(lv_event_t *e) { ui_seed_entry(UI_ENTER_WORDS, (int)(intptr_t)lv_event_get_user_data(e)); }

void ui_backup_loaded() {
    new_seed_wipe();
    if (!wallet_entropy(&pending)) return;
    new_words = wallet_word_count();
    backup_existing = true;
    ui_goto(UI_BACKUP_WARNING);
}
static void backup_existing_cb(lv_event_t *) { ui_backup_loaded(); }
// Leaving the card flow: back to the wallet, or for a new seed, to Seed ready.
static void backup_exit_cb(lv_event_t *) { ui_goto(backup_existing ? UI_SEED_MENU : UI_SEED_READY); }

// Enter words: restore from a words backup.
static void build_restore() {
    lv_obj_t *scr = screen();
    topbar(scr, "Enter words", to_home_cb);
    lv_obj_t *c = card(scr, CW, 2 * 62 + 1);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 4);
    lv_obj_t *r1 = list_row(c, ICON_KEYBOARD, "12 words", nullptr, restore_cb, (void *)12);
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 0);
    divider(c, 62);
    lv_obj_t *r2 = list_row(c, ICON_KEYBOARD, "24 words", nullptr, restore_cb, (void *)24);
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 63);
    lv_obj_align(para(scr, "Type the words of your backup. The seed loads right away.", &inter_13, TEXT_3),
                 LV_ALIGN_TOP_MID, 0, BAR + 4 + 125 + 14);
    show(scr);
}

// Two-way toggle, e.g. 12 / 24 words.
static void segmented(lv_obj_t *parent, int y, const char *a, const char *b, bool a_on, lv_event_cb_t cb, void *ua,
                      void *ub) {
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, CW, 40);
    lv_obj_set_style_radius(seg, 12, 0);
    lv_obj_set_style_bg_color(seg, SURFACE_2, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_align(seg, LV_ALIGN_TOP_MID, 0, y);
    for (int i = 0; i < 2; i++) {
        bool on = i == 0 ? a_on : !a_on;
        lv_obj_t *o = lv_button_create(seg);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, (CW - 8) / 2, 32);
        lv_obj_set_pos(o, 4 + i * (CW - 8) / 2, 4);
        lv_obj_set_style_radius(o, 9, 0);
        lv_obj_set_style_bg_color(o, ACCENT_DIM, 0);
        lv_obj_set_style_bg_opa(o, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, i == 0 ? ua : ub);
        lv_obj_center(text(o, i == 0 ? a : b, &inter_15, on ? ACCENT : TEXT_2));
    }
}

static void build_new_seed() {
    lv_obj_t *scr = screen();
    topbar(scr, "New seed", to_home_cb);
    segmented(scr, BAR + 4, "12 words", "24 words", new_words == 12, words_cb, (void *)12, (void *)24);

    lv_obj_t *c = card(scr, CW, 3 * 62 + 2);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 56);
    char dice[24], coins[24];
    snprintf(dice, sizeof(dice), "%d rolls of one die", seed_dice_rolls(new_words));
    snprintf(coins, sizeof(coins), "%d coin flips", seed_coin_flips(new_words));
    lv_obj_t *r1 = list_row(c, ICON_DICE, "Dice rolls", dice, method_cb, GO(UI_ENTROPY_DICE));
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 0);
    divider(c, 62);
    lv_obj_t *r2 = list_row(c, ICON_COINS, "Coin flips", coins, method_cb, GO(UI_ENTROPY_COINS));
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 63);
    divider(c, 125);
    char draw[32];
    snprintf(draw, sizeof(draw), "Pick %d words yourself", new_words - 1);
    lv_obj_t *r3 = list_row(c, ICON_KEYBOARD, "Draw words", draw, draw_cb);
    lv_obj_align(r3, LV_ALIGN_TOP_MID, 0, 126);
    show(scr);
}

// ---------------------------------------------------------------- dice / coins
static lv_obj_t *ent_count, *ent_tail, *ent_bar;

static int entropy_needed() {
    if (final_flips) return seed_final_word_flips(new_words);
    return entropy_dice ? seed_dice_rolls(new_words) : seed_coin_flips(new_words);
}

static void entropy_refresh() {
    int need = entropy_needed();
    lv_label_set_text_fmt(ent_count, "%d of %d", entropy_n, need);
    lv_bar_set_value(ent_bar, entropy_n * 100 / need, LV_ANIM_OFF);
    // The most recent entries, so a mistyped one is easy to spot and undo.
    char t[40];
    int show_n = entropy_dice ? 10 : 16, from = entropy_n > show_n ? entropy_n - show_n : 0, o = 0;
    for (int i = from; i < entropy_n; i++) {
        if (i > from && (entropy_dice || (i % 4) == 0)) t[o++] = ' ';
        t[o++] = entropy_in[i];
    }
    t[o] = 0;
    lv_label_set_text(ent_tail, t);
    secure_zero(t, sizeof(t));
}

void ui_entropy_input(char c) {
    if ((ui_current() != UI_ENTROPY_DICE && ui_current() != UI_ENTROPY_COINS) || entropy_n >= entropy_needed()) return;
    entropy_in[entropy_n++] = c;
    if (entropy_n < entropy_needed()) { entropy_refresh(); return; }
    entropy_in[entropy_n] = 0;
    bool ok = final_flips   ? seed_final_word(word_idx, new_words, entropy_in, &pending)
              : entropy_dice ? seed_from_dice(entropy_in, entropy_n, new_words, &pending)
                             : seed_from_coins(entropy_in, entropy_n, new_words, &pending);
    secure_zero(entropy_in, sizeof(entropy_in));
    secure_zero(word_idx, sizeof(word_idx));
    entropy_n = word_n = 0;
    final_flips = false;
    ui_goto(ok ? UI_BACKUP_WARNING : UI_NEW_SEED);
}

static void entropy_key_cb(lv_event_t *e) { ui_entropy_input((char)(intptr_t)lv_event_get_user_data(e)); }
static void entropy_undo_cb(lv_event_t *) {
    if (entropy_n) entropy_in[--entropy_n] = 0;
    entropy_refresh();
}

static lv_obj_t *entropy_key(lv_obj_t *scr, int x, int y, int w, int h, const char *icon, const char *small, char v) {
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, SURFACE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, entropy_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    if (!small) {
        lv_obj_center(text(b, icon, &icons_40, TEXT));   // a die face
        return b;
    }
    // A coin: round, with the side's picture, and its value underneath.
    lv_obj_t *coin = lv_obj_create(b);
    lv_obj_remove_style_all(coin);
    lv_obj_remove_flag(coin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(coin, 68, 68);
    lv_obj_set_style_radius(coin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(coin, ACCENT_DIM, 0);
    lv_obj_set_style_bg_opa(coin, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(coin, ACCENT, 0);
    lv_obj_set_style_border_width(coin, 2, 0);
    lv_obj_align(coin, LV_ALIGN_CENTER, 0, -12);
    lv_obj_center(text(coin, icon, &icons_40, ACCENT));
    lv_obj_align(text(b, small, &inter_13, TEXT_3), LV_ALIGN_CENTER, 0, 40);
    return b;
}

// From the last word's coin flips, back to the drawn words.
static void final_flips_back_cb(lv_event_t *) {
    final_flips = false;
    secure_zero(entropy_in, sizeof(entropy_in));
    entropy_n = 0;
    word_idx[--word_n] = 0;
    ui_goto(UI_ENTER_WORDS);
}

static void build_entropy(bool dice) {
    entropy_dice = dice;
    lv_obj_t *scr = screen();
    topbar(scr, final_flips ? "Last word" : dice ? "Dice rolls" : "Coin flips",
           final_flips ? final_flips_back_cb : to_new_seed_cb);
    ent_count = text(scr, "", &inter_22_semi, TEXT);
    lv_obj_align(ent_count, LV_ALIGN_TOP_MID, 0, BAR + 2);
    ent_bar = lv_bar_create(scr);
    lv_obj_set_size(ent_bar, CW, 4);
    lv_bar_set_range(ent_bar, 0, 100);
    lv_obj_set_style_bg_color(ent_bar, SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ent_bar, ACCENT, LV_PART_INDICATOR);
    lv_obj_align(ent_bar, LV_ALIGN_TOP_MID, 0, BAR + 36);
    ent_tail = text(scr, "", &mono_16, TEXT_2);
    lv_obj_align(ent_tail, LV_ALIGN_TOP_MID, 0, BAR + 50);
    lv_obj_align(text(scr, dice ? "Roll one die, tap what it shows" : "Flip a coin, tap what it shows",
                      &inter_13, TEXT_3),
                 LV_ALIGN_TOP_MID, 0, BAR + 76);

    const int y0 = BAR + 104;
    if (dice) {
        static const char *faces[] = {ICON_DICE_1, ICON_DICE_2, ICON_DICE_3, ICON_DICE_4, ICON_DICE_5, ICON_DICE_6};
        const int kw = (CW - 16) / 3, kh = 56;
        for (int i = 0; i < 6; i++)
            entropy_key(scr, M + (i % 3) * (kw + 8), y0 + (i / 3) * (kh + 8), kw, kh, faces[i], nullptr, char('1' + i));
    } else {
        const int kw = (CW - 8) / 2;
        entropy_key(scr, M, y0, kw, 120, ICON_HEADS, "Heads = 1", '1');
        entropy_key(scr, M + kw + 8, y0, kw, 120, ICON_TAILS, "Tails = 0", '0');
    }
    lv_obj_align(quiet_link(scr, "Undo", entropy_undo_cb, nullptr), LV_ALIGN_BOTTOM_MID, 0, -8);
    entropy_refresh();
    show(scr);
}

// ---------------------------------------------------------------- enter words
static lv_obj_t *word_field, *word_kb, *word_sugg[2];
static int sugg_idx[2];
static const char *kb_map[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                               "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
                               "z", "x", "c", "v", "b", "n", "m", "Del", ""};

static bool word_is_last() { return !drawing && word_n == new_words - 1; }
static bool word_allowed(int i) { return !word_is_last() || ((last_ok[i >> 3] >> (i & 7)) & 1); }

// Words the user may pick with this prefix: count, and the first `max` of them.
static int word_matches(const char *prefix, int *out, int max) {
    int first, cnt = seed_words_with_prefix(prefix, &first), n = 0;
    for (int i = first; cnt && i < first + cnt; i++) {
        if (!word_allowed(i)) continue;
        if (n < max) out[n] = i;
        n++;
    }
    return n;
}

static void words_refresh() {
    char f[16];
    snprintf(f, sizeof(f), "%s_", word_prefix);
    lv_label_set_text(word_field, f);
    int n = word_prefix[0] ? word_matches(word_prefix, sugg_idx, 2) : 0;
    for (int i = 0; i < 2; i++) {
        if (i < n) {
            lv_obj_remove_flag(word_sugg[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(word_sugg[i], 0), seed_word(sugg_idx[i]));
        } else {
            lv_obj_add_flag(word_sugg[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Only letters that still lead to a word (on the last word: one with a valid checksum).
    size_t l = strlen(word_prefix);
    for (int b = 0; b < 26; b++) {
        char p[10];
        memcpy(p, word_prefix, l);
        p[l] = lv_buttonmatrix_get_button_text(word_kb, b)[0];
        p[l + 1] = 0;
        bool ok = l + 1 < sizeof(word_prefix) && word_matches(p, nullptr, 0) > 0;
        if (ok) lv_buttonmatrix_clear_button_ctrl(word_kb, b, LV_BUTTONMATRIX_CTRL_DISABLED);
        else lv_buttonmatrix_set_button_ctrl(word_kb, b, LV_BUTTONMATRIX_CTRL_DISABLED);
    }
}

static bool word_accept(int idx) {
    if (idx < 0 || !word_allowed(idx)) return false;
    word_idx[word_n++] = uint16_t(idx);
    word_prefix[0] = 0;
    if (drawing && word_n == new_words - 1) {   // the last word comes from coin flips
        final_flips = true;
        entropy_n = 0;
        ui_goto(UI_ENTROPY_COINS);
        return true;
    }
    if (word_n == new_words) {
        SeedEntropy e;
        bool ok = seed_from_words(word_idx, word_n, &e);
        secure_zero(word_idx, sizeof(word_idx));
        word_n = 0;
        if (ok && word_field) {
            lv_label_set_text(word_field, "Loading" "\xE2\x80\xA6");
            lv_refr_now(nullptr);
        }
        ok = ok && wallet_load(e);
        seedqr_wipe(&e);
        ui_goto(ok ? UI_SEED_LOADED : UI_RESTORE);
        return ok;
    }
    if (word_is_last()) {
        memset(last_ok, 0, sizeof(last_ok));
        for (int i = 0; i < 2048; i++)
            if (seed_last_word_ok(word_idx, new_words, uint16_t(i))) last_ok[i >> 3] |= 1 << (i & 7);
    }
    ui_goto(UI_ENTER_WORDS);
    return true;
}

bool ui_enter_word(const char *w) {
    int first;
    if (ui_current() != UI_ENTER_WORDS || !seed_words_with_prefix(w, &first) || strcmp(seed_word(first), w)) return false;
    return word_accept(first);
}

static void sugg_cb(lv_event_t *e) { word_accept(sugg_idx[(intptr_t)lv_event_get_user_data(e)]); }

static void kb_cb(lv_event_t *e) {
    uint32_t b = lv_buttonmatrix_get_selected_button(word_kb);
    if (b == LV_BUTTONMATRIX_BUTTON_NONE) return;
    size_t l = strlen(word_prefix);
    if (b == 26) { if (l) word_prefix[l - 1] = 0; }
    else if (l + 1 < sizeof(word_prefix)) { word_prefix[l] = lv_buttonmatrix_get_button_text(word_kb, b)[0]; word_prefix[l + 1] = 0; }
    words_refresh();
}

static void words_back_cb(lv_event_t *) {
    if (!word_n) { if (drawing) to_new_seed_cb(nullptr); else ui_goto(UI_RESTORE); return; }
    word_idx[--word_n] = 0;
    word_prefix[0] = 0;
    ui_goto(UI_ENTER_WORDS);
}

static void build_enter_words() {
    lv_obj_t *scr = screen();
    char title[24];
    snprintf(title, sizeof(title), "Word %d of %d", word_n + 1, drawing ? new_words - 1 : new_words);
    topbar(scr, title, words_back_cb);

    lv_obj_t *f = card(scr, CW, 40);
    lv_obj_set_style_pad_ver(f, 0, 0);
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, BAR + 2);
    word_field = text(f, "", &mono_22, TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(word_field, LV_ALIGN_LEFT_MID, 0, 0);
    if (word_is_last()) lv_obj_align(text(f, "checksum", &inter_13, TEXT_3), LV_ALIGN_RIGHT_MID, 0, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *s = lv_button_create(scr);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, (CW - 8) / 2, 40);
        lv_obj_set_pos(s, M + i * ((CW - 8) / 2 + 8), BAR + 50);
        lv_obj_set_style_radius(s, 12, 0);
        lv_obj_set_style_bg_color(s, ACCENT_DIM, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, ACCENT, LV_STATE_PRESSED);
        lv_obj_add_event_cb(s, sugg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(text(s, "", &inter_17_semi, ACCENT));
        word_sugg[i] = s;
    }

    word_kb = lv_buttonmatrix_create(scr);
    lv_buttonmatrix_set_map(word_kb, kb_map);
    lv_obj_set_size(word_kb, W, 3 * 44 + 8);
    lv_obj_align(word_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(word_kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(word_kb, 0, 0);
    lv_obj_set_style_pad_all(word_kb, 4, 0);
    lv_obj_set_style_pad_gap(word_kb, 3, 0);
    lv_obj_set_style_bg_color(word_kb, SURFACE_2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(word_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(word_kb, 7, LV_PART_ITEMS);
    lv_obj_set_style_border_width(word_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(word_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(word_kb, &inter_17_semi, LV_PART_ITEMS);
    lv_obj_set_style_text_color(word_kb, TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(word_kb, ACCENT_DIM, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(word_kb, LV_OPA_30, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(word_kb, TEXT_3, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_buttonmatrix_set_button_width(word_kb, 26, 2);   // "Del" spans two keys
    lv_obj_add_event_cb(word_kb, kb_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    words_refresh();
    show(scr);
}

// ---------------------------------------------------------------- backup
// Always a Compact SeedQR (byte mode, ECC L): 21x21 for 12 words, 25x25 for 24,
// the fewest squares to copy by hand.
static bool backup_make_qr() {
    static uint8_t tmp[sizeof(qr_buf)], seg_buf[sizeof(qr_buf)];
    qrcodegen_Segment seg = qrcodegen_makeBytes(pending.bytes, pending.len, seg_buf);
    bool ok = qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 2, qrcodegen_Mask_AUTO, false, tmp, qr_buf);
    secure_zero(tmp, sizeof(tmp));
    secure_zero(seg_buf, sizeof(seg_buf));
    qr_n = ok ? qrcodegen_getSize(qr_buf) : 0;
    return ok && qr_n == (new_words == 24 ? 25 : 21);
}

static void build_backup_warning() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_WARNING, AMBER, AMBER_DIM, 64), LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_align(text(scr, "Keep it offline", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_align(para(scr, backup_existing ? "Your seed QR is your private key. Never photograph it or scan it with a phone."
                                           : "Your words are your private key. Never photograph them or type them into a phone."),
                 LV_ALIGN_TOP_MID, 0, 154);
    bottom_pair(btn_ghost(scr, backup_existing ? "Cancel" : "Discard", backup_existing ? to_menu_cb : to_new_seed_cb,
                          nullptr, TEXT),
                btn_primary(scr, "I understand", go_cb, GO(backup_existing ? UI_BACKUP_QR : UI_BACKUP_WORDS)));
    show(scr);
}

// Blocks of 5 modules; a remainder joins the last block instead of making a
// sliver (21 = 5+5+5+6). Lines on screen and on the card sit on these edges.
static int blocks_per_side() { return qr_n / 5; }
static bool block_edge(int m) { return m == qr_n || (m % 5 == 0 && m / 5 < blocks_per_side()); }
static int block_len(int b) { return b + 1 < blocks_per_side() ? 5 : qr_n - 5 * b; }

// Part of the SeedQR, drawn module by module: dark squares, light grid lines
// (darker every 5 modules, like the lines on the paper card).
struct GridView { int c0, r0, cols, rows, cell; bool minor; };
static GridView whole_view, block_view;

static void grid_draw_cb(lv_event_t *e) {
    const GridView *g = (const GridView *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t *)lv_event_get_target(e), &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 0;
    auto rect = [&](int x1, int y1, int x2, int y2, lv_color_t c) {
        lv_area_t r = {a.x1 + x1, a.y1 + y1, a.x1 + x2, a.y1 + y2};
        d.bg_color = c;
        lv_draw_rect(layer, &d, &r);
    };
    for (int r = 0; r < g->rows; r++)
        for (int c = 0; c < g->cols; c++)
            if (qrcodegen_getModule(qr_buf, g->c0 + c, g->r0 + r))
                rect(c * g->cell, r * g->cell, (c + 1) * g->cell, (r + 1) * g->cell, lv_color_black());
    const lv_color_t major = LV_COLOR_MAKE(0x9A, 0x96, 0x8E), minor = LV_COLOR_MAKE(0xD4, 0xD0, 0xC8);
    for (int i = 0; i <= g->cols; i++) {
        bool m = block_edge(g->c0 + i);
        if (m || g->minor) rect(i * g->cell, 0, i * g->cell, g->rows * g->cell, m ? major : minor);
    }
    for (int i = 0; i <= g->rows; i++) {
        bool m = block_edge(g->r0 + i);
        if (m || g->minor) rect(0, i * g->cell, g->cols * g->cell, i * g->cell, m ? major : minor);
    }
}

static lv_obj_t *grid(lv_obj_t *parent, GridView *g) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, g->cols * g->cell + 1, g->rows * g->cell + 1);
    lv_obj_add_event_cb(o, grid_draw_cb, LV_EVENT_DRAW_MAIN, g);
    return o;
}

static lv_obj_t *white_card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, LV_COLOR_MAKE(0xF6, 0xF3, 0xEC), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 14, 0);
    return c;
}

static void to_warning_cb(lv_event_t *) { ui_goto(UI_BACKUP_WARNING); }
static void to_whole_qr_cb(lv_event_t *) { ui_goto(UI_BACKUP_QR); }


static void block_start_cb(lv_event_t *) { block_i = 0; ui_goto(UI_BACKUP_BLOCK); }

static void build_backup_qr() {
    if (!backup_make_qr()) { backup_exit_cb(nullptr); return; }
    lv_obj_t *scr = screen();
    topbar(scr, "Back up", to_warning_cb);
    const int cell = 180 / qr_n;
    whole_view = {0, 0, qr_n, qr_n, cell, false};
    lv_obj_t *c = white_card(scr, qr_n * cell + 17, qr_n * cell + 17);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 2);
    lv_obj_center(grid(c, &whole_view));
    place_bottom(btn_primary(scr, "Start copying", block_start_cb));
    show(scr);
}

static void col_label(int col, char out[4]) {
    if (col < 26) snprintf(out, 4, "%c", 'A' + col);
    else snprintf(out, 4, "A%c", 'A' + col - 26);
}

void ui_backup_next() {
    int n = blocks_per_side() * blocks_per_side();
    if (block_i + 1 < n) { block_i++; ui_goto(UI_BACKUP_BLOCK); }
    else ui_goto(UI_BACKUP_SCAN);
}
static void block_next_cb(lv_event_t *) { ui_backup_next(); }
static void block_prev_cb(lv_event_t *) {
    if (block_i > 0) { block_i--; ui_goto(UI_BACKUP_BLOCK); }
    else ui_goto(UI_BACKUP_QR);
}
static void block_gesture_cb(lv_event_t *) {
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT) ui_backup_next();
    else if (d == LV_DIR_RIGHT) block_prev_cb(nullptr);
}

// One 5x5 block at a time, labelled with the card's column letters and row numbers.
static void build_backup_block() {
    lv_obj_t *scr = screen();
    int bs = blocks_per_side(), n = bs * bs;
    char title[32];
    snprintf(title, sizeof(title), "Part %d of %d", block_i + 1, n);
    lv_obj_t *bar = topbar(scr, title, to_whole_qr_cb);

    // Where this block sits in the QR: one square per block, this one lit,
    // the ones already copied dimmed.
    const int sq = bs > 4 ? 6 : 7, gap = 2, side = bs * sq + (bs - 1) * gap;
    lv_obj_t *map = lv_obj_create(bar);
    lv_obj_remove_style_all(map);
    lv_obj_set_size(map, side, side);
    lv_obj_align(map, LV_ALIGN_RIGHT_MID, -14, 2);
    for (int b = 0; b < n; b++) {
        lv_obj_t *q = lv_obj_create(map);
        lv_obj_remove_style_all(q);
        lv_obj_set_size(q, sq, sq);
        lv_obj_set_pos(q, (b % bs) * (sq + gap), (b / bs) * (sq + gap));
        lv_obj_set_style_radius(q, 1, 0);
        lv_obj_set_style_bg_color(q, b == block_i ? ACCENT : b < block_i ? TEXT_3 : SURFACE_2, 0);
        lv_obj_set_style_bg_opa(q, LV_OPA_COVER, 0);
    }

    // One cell size per QR, so squares don't change size between blocks.
    const int cell = qr_n % 5 ? 28 : 32;
    int c0 = (block_i % bs) * 5, r0 = (block_i / bs) * 5;
    block_view = {c0, r0, block_len(block_i % bs), block_len(block_i / bs), cell, true};
    lv_obj_t *c = white_card(scr, CW, (qr_n % 5 ? 6 : 5) * cell + 32);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 2);
    const int gx = 30, gy = 24;
    lv_obj_set_pos(grid(c, &block_view), gx, gy);
    const lv_color_t lab = LV_COLOR_MAKE(0x6E, 0x6A, 0x62);
    for (int i = 0; i < block_view.cols; i++) {
        char l[4];
        col_label(c0 + i, l);
        lv_obj_t *t = text(c, l, &inter_13, lab);
        lv_obj_set_width(t, cell);
        lv_obj_set_pos(t, gx + i * cell, 4);
    }
    for (int i = 0; i < block_view.rows; i++) {
        char l[12];
        snprintf(l, sizeof(l), "%d", r0 + i + 1);
        lv_obj_t *t = text(c, l, &inter_13, lab, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(t, 22);
        lv_obj_set_pos(t, 2, gy + i * cell + cell / 2 - 8);
    }
    lv_obj_add_event_cb(scr, block_gesture_cb, LV_EVENT_GESTURE, nullptr);
    bool last = block_i + 1 == n;
    // The seed is already backed up elsewhere (wallet, or the written words), so checking is optional.
    lv_obj_t *secondary = last ? btn_ghost(scr, "Done", backup_exit_cb, nullptr, TEXT)
                                                  : btn_ghost(scr, ICON_LEFT, block_prev_cb, nullptr, TEXT);
    bottom_pair(secondary, btn_primary(scr, last ? "Check card" : "Next part", block_next_cb));
    show(scr);
}

// ---------------------------------------------------------------- write down words
static int words_pages() { return new_words / 4; }

static void words_next_cb(lv_event_t *) {
    if (words_page + 1 < words_pages()) { words_page++; ui_goto(UI_BACKUP_WORDS); }
    else ui_goto(UI_BACKUP_CHOICE);
}
static void words_prev_cb(lv_event_t *) {
    if (words_page > 0) { words_page--; ui_goto(UI_BACKUP_WORDS); }
    else ui_goto(UI_BACKUP_WARNING);
}

static void build_backup_words() {
    lv_obj_t *scr = screen();
    char title[32];
    snprintf(title, sizeof(title), "Words %d-%d of %d", words_page * 4 + 1, words_page * 4 + 4, new_words);
    topbar(scr, title, words_prev_cb);
    uint16_t idx[24];
    seed_word_indexes(pending, idx);
    lv_obj_t *c = card(scr, CW, 4 * 44 + 16);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 4);
    for (int i = 0; i < 4; i++) {
        int w = words_page * 4 + i;
        char num[12];
        snprintf(num, sizeof(num), "%d", w + 1);
        lv_obj_t *n = text(c, num, &inter_15, TEXT_3, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(n, 26);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, i * 44 + 8);
        bool last = w == new_words - 1;
        lv_obj_t *t = text(c, seed_word(idx[w]), &mono_22, last && drawing ? ACCENT : TEXT, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 40, i * 44 + 2);
        if (i) divider(c, i * 44 - 6);
    }
    secure_zero(idx, sizeof(idx));
    bool last = words_page + 1 == words_pages();
    bottom_pair(btn_ghost(scr, ICON_LEFT, words_prev_cb, nullptr, TEXT),
                btn_primary(scr, last ? "Written down" : "Next", words_next_cb));
    show(scr);
}

static void to_last_words_cb(lv_event_t *) {
    words_page = words_pages() - 1;
    ui_goto(UI_BACKUP_WORDS);
}

static void build_backup_choice() {
    lv_obj_t *scr = screen();
    topbar(scr, "QR card", to_last_words_cb);
    lv_obj_align(icon_badge(scr, ICON_QR, ACCENT, ACCENT_DIM, 64), LV_ALIGN_TOP_MID, 0, BAR + 8);
    lv_obj_align(text(scr, "Add a QR card?", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 86);
    lv_obj_align(para(scr, "Copy it square by square, then load the seed by scanning instead of typing.", &inter_13,
                      TEXT_2),
                 LV_ALIGN_TOP_MID, 0, BAR + 122);
    bottom_pair(btn_ghost(scr, "Skip", go_cb, GO(UI_SEED_READY), TEXT), btn_primary(scr, "Make card", go_cb, GO(UI_BACKUP_QR)));
    show(scr);
}

// ---------------------------------------------------------------- seed ready
// The end of New seed. The seed isn't loaded: Done wipes it from the device.
static void build_seed_ready() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_CHECK, GREEN, GREEN_DIM, 52), LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_align(text(scr, card_checked ? "Card checked" : "Seed ready", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_align(caption(scr, "FINGERPRINT"), LV_ALIGN_TOP_MID, 0, 132);
    char fp[9], spaced[12];
    seed_fingerprint_hex(pending, fp);   // slow on the board, like loading a seed
    snprintf(spaced, sizeof(spaced), "%.4s %.4s", fp, fp + 4);
    lv_obj_align(text(scr, spaced, &mono_26, TEXT), LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_align(para(scr, "Write it on your backup. To use the seed, load it with Scan QR or Enter words.", &inter_13,
                      TEXT_3),
                 LV_ALIGN_TOP_MID, 0, 190);
    place_bottom(btn_primary(scr, "Done", to_home_cb));
    show(scr);
}

static void handle_confirm_payload(QrPayload &p) {
    SeedEntropy e;
    SeedQrResult r = seedqr_decode(p.data, p.len, &e);
    memset(&p, 0, sizeof(p));
    bool same = r == SEEDQR_OK && e.len == pending.len && memcmp(e.bytes, pending.bytes, e.len) == 0;
    seedqr_wipe(&e);
    if (same && backup_existing) {
        scan_stop();
        new_seed_wipe();
        fresh_backup = true;
        ui_goto(UI_SEED_LOADED);
        return;
    }
    if (same) {
        scan_stop();
        card_checked = true;
        ui_goto(UI_SEED_READY);
        return;
    }
    if (r == SEEDQR_OK) scan_status("Wrong seed", "This card holds a different seed", RED);
    else if (r == SEEDQR_BAD_CHECKSUM || r == SEEDQR_BAD_WORD_INDEX) scan_status("Damaged QR", "Check the squares again", RED);
    else scan_status("No seed found", "Some squares are probably off", AMBER);
}

// ---------------------------------------------------------------- transaction
static void discard_psbt_cb(lv_event_t *) {
    psbt_clear();
    ui_goto(UI_SEED_MENU);
}

static void build_psbt_error() {
    lv_obj_t *scr = screen();
    bool sus = review.error == PSBT_SUSPICIOUS;
    lv_obj_align(icon_badge(scr, ICON_WARNING, sus ? RED : AMBER, sus ? RED_DIM : AMBER_DIM, 64), LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_align(text(scr, sus ? "Don't sign this" : "Can't sign this", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_align(para(scr, psbt_error_text(review.error)), LV_ALIGN_TOP_MID, 0, 162);
    place_bottom(btn_primary(scr, "Back to wallet", discard_psbt_cb));
    show(scr);
}

static int recipient_count() {
    int n = 0;
    for (int i = 0; i < review.n_outputs; i++)
        if (!review.outputs[i].is_change && !review.outputs[i].is_self) n++;
    return n;
}

static void build_psbt_overview() {
    lv_obj_t *scr = screen();
    topbar(scr, "Review", discard_psbt_cb);

    bool self_only = review.spend == 0;
    uint64_t headline = self_only ? review.self : review.spend;
    char cap[48];
    snprintf(cap, sizeof(cap), "%s%s", self_only ? "SENDING TO YOURSELF" : "YOU'RE SENDING", review.testnet ? "  \xC2\xB7  TESTNET" : "");
    lv_obj_t *cl = caption(scr, cap);
    if (review.testnet) lv_obj_set_style_text_color(cl, AMBER, 0);
    lv_obj_align(cl, LV_ALIGN_TOP_MID, 0, BAR + 12);
    lv_obj_align(amount(scr, headline, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 32);

    int nrec = recipient_count();
    char rec[8], fee[40], chg[40];
    snprintf(rec, sizeof(rec), "%d", nrec);
    fmt_btc(review.fee, fee, sizeof(fee));
    fmt_btc(review.change, chg, sizeof(chg));
    const KvRow rows[] = {
        {nrec == 1 ? "Recipient" : "Recipients", rec, TEXT, false},
        {"Fee", fee, TEXT, true},
        {"Change", review.change ? chg : "None", review.change ? GREEN : TEXT_3, true},
    };
    kv_card(scroll_area(scr, BAR + 92), rows, 3);

    out_index = 0;
    place_bottom(btn_primary(scr, "Check addresses", go_cb, GO(UI_PSBT_OUTPUT)));
    show(scr);
}

// Address in groups of four, alternating brightness so it can be compared
// chunk by chunk against the wallet. Four groups per line.
static void address_chunks(lv_obj_t *parent, const char *a) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(box, 6, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 20);
    size_t n = strlen(a);
    for (size_t g = 0; g * 4 < n; g++) {
        char chunk[5];
        snprintf(chunk, sizeof(chunk), "%.4s", a + g * 4);
        text(box, chunk, &mono_16, (g % 2) ? TEXT_2 : TEXT, LV_TEXT_ALIGN_LEFT);
    }
}

void ui_next_output() {
    if (out_index + 1 < review.n_outputs) { out_index++; ui_goto(UI_PSBT_OUTPUT); }
    else ui_goto(UI_PSBT_SIGN);
}

static void next_output_cb(lv_event_t *) { ui_next_output(); }
static void prev_output_cb(lv_event_t *) {
    if (out_index > 0) { out_index--; ui_goto(UI_PSBT_OUTPUT); }
    else ui_goto(UI_PSBT_OVERVIEW);
}

static void build_psbt_output() {
    const PsbtOutput &o = review.outputs[out_index];
    lv_obj_t *scr = screen();
    char title[32];
    snprintf(title, sizeof(title), "Address %d of %d", out_index + 1, review.n_outputs);
    topbar(scr, title, prev_output_cb);

    bool ours = o.is_change || o.is_self;
    lv_obj_t *tag = ours ? chip(scr, o.is_change ? ICON_CHECK "  YOUR CHANGE" : ICON_CHECK "  YOUR ADDRESS", GREEN, GREEN_DIM)
                         : chip(scr, "SENDING TO", TEXT_2, SURFACE_2);
    lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, BAR + 8);
    lv_obj_align(amount(scr, o.amount, ours ? GREEN : TEXT), LV_ALIGN_TOP_MID, 0, BAR + 36);

    lv_obj_t *c = card(scr, CW, ours ? 112 : 96);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 90);
    lv_obj_align(caption(c, "ADDRESS"), LV_ALIGN_TOP_LEFT, 0, 0);
    address_chunks(c, o.address);
    if (ours) {
        char v[72];
        snprintf(v, sizeof(v), ICON_CHECK "  Belongs to this seed");
        lv_obj_align(text(c, v, &inter_13, GREEN, LV_TEXT_ALIGN_LEFT), LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    bool last = out_index + 1 >= review.n_outputs;
    place_bottom(btn_primary(scr, last ? "Continue" : "Next address", next_output_cb));
    show(scr);
}

// ---------------------------------------------------------------- sign
static uint8_t signed_buf[UR_MAX_MESSAGE];
static size_t signed_len;
static lv_obj_t *swipe_hint;

bool ui_swipe_sign() {
    if (psbt_sign() <= 0) { review.error = PSBT_PARSE_FAILED; ui_goto(UI_PSBT_ERROR); return false; }
    signed_len = psbt_serialize(signed_buf, sizeof(signed_buf));
    psbt_clear();
    if (!signed_len) { review.error = PSBT_UNSUPPORTED; ui_goto(UI_PSBT_ERROR); return false; }
    ui_goto(UI_PSBT_SIGNED);
    return true;
}

static void swipe_cb(lv_event_t *e) {
    lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        int opa = 255 - v * 255 / 50;
        lv_obj_set_style_text_opa(swipe_hint, opa < 0 ? 0 : opa, 0);
        return;
    }
    if (v >= 96) ui_swipe_sign();
    else {
        lv_slider_set_value(s, 0, LV_ANIM_ON);   // not far enough: snap back
        lv_obj_set_style_text_opa(swipe_hint, LV_OPA_COVER, 0);
    }
}

static void back_to_outputs_cb(lv_event_t *) {
    out_index = review.n_outputs - 1;
    ui_goto(UI_PSBT_OUTPUT);
}

static void build_psbt_sign() {
    lv_obj_t *scr = screen();
    topbar(scr, "Confirm", back_to_outputs_cb);
    if (review.testnet) {
        lv_obj_t *t = caption(scr, "TESTNET");
        lv_obj_set_style_text_color(t, AMBER, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, BAR - 2);
    }

    uint64_t send = review.spend ? review.spend : review.self;
    char vals[3][40];
    fmt_btc(send, vals[0], sizeof(vals[0]));
    fmt_btc(review.fee, vals[1], sizeof(vals[1]));
    fmt_btc(send + review.fee, vals[2], sizeof(vals[2]));
    const KvRow rows[] = {
        {review.spend ? "Send" : "Move", vals[0], TEXT, false},
        {"Fee", vals[1], TEXT_2, false},
        {"Total", vals[2], ACCENT, true},
    };
    lv_obj_t *c = kv_card(scr, rows, 3, &mono_16);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 18);
    int card_h = lv_obj_get_style_height(c, 0);

    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_remove_style_all(sl);
    lv_obj_set_size(sl, CW, 56);
    lv_slider_set_range(sl, 0, 100);
    lv_obj_set_style_bg_color(sl, SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(sl, 28, LV_PART_MAIN);          // knob stays inside the track
    lv_obj_set_style_bg_color(sl, ACCENT_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 28, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, -4, LV_PART_KNOB);
    lv_obj_set_style_bg_image_src(sl, ICON_ARROW_RIGHT, LV_PART_KNOB);
    lv_obj_set_style_text_font(sl, &inter_17_semi, LV_PART_KNOB);
    lv_obj_set_style_text_color(sl, LV_COLOR_MAKE(0x14, 0x0C, 0x02), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, swipe_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(sl, swipe_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, BAR + 18 + card_h + 14);
    swipe_hint = text(sl, "Slide to sign", &inter_15, TEXT_2);
    lv_obj_align(swipe_hint, LV_ALIGN_CENTER, 14, 0);

    lv_obj_align(text(scr, "This only signs. Your wallet sends it.", &inter_13, TEXT_3),
                 LV_ALIGN_BOTTOM_MID, 0, -M);
    show(scr);
}

// ---------------------------------------------------------------- signed
static lv_timer_t *anim_timer;
static lv_obj_t *anim_qr, *anim_lbl;
static uint32_t anim_seq;
#define FRAGMENT_LEN 80

static void anim_tick(lv_timer_t *) {
    static char part[400];
    anim_seq++;
    size_t n = ur_encode_part(signed_buf, signed_len, FRAGMENT_LEN, anim_seq, part, sizeof(part));
    if (n) lv_qrcode_update(anim_qr, part, n);
    size_t count = ur_part_count(signed_len, FRAGMENT_LEN);
    if (count > 1)
        lv_label_set_text_fmt(anim_lbl, "Scan this with your wallet  \xC2\xB7  %u/%u", unsigned((anim_seq - 1) % count + 1),
                              unsigned(count));
}

static void signed_done_cb(lv_event_t *) {
    if (anim_timer) lv_timer_delete(anim_timer);
    anim_timer = nullptr;
    memset(signed_buf, 0, sizeof(signed_buf));
    signed_len = 0;
    ui_goto(UI_SEED_MENU);
}

static void build_psbt_signed() {
    lv_obj_t *scr = screen();
    topbar(scr, "Signed", nullptr, ICON_CHECK, GREEN, GREEN_DIM);
    qr_card(scr, BAR + 4, &anim_qr);
    anim_lbl = text(scr, "Scan this with your wallet", &inter_13, TEXT_2);
    lv_obj_align(anim_lbl, LV_ALIGN_TOP_MID, 0, BAR + 4 + 176 + 10);
    place_bottom(btn_primary(scr, "Done", signed_done_cb));
    show(scr);
    anim_seq = 0;
    anim_tick(nullptr);
    if (anim_timer) lv_timer_delete(anim_timer);
    anim_timer = lv_timer_create(anim_tick, 200, nullptr);
}

// ---------------------------------------------------------------- verify address
static lv_timer_t *verify_timer;
static lv_obj_t *verify_bar, *verify_lbl;

static void verify_tick(lv_timer_t *) {
    bool done = address_verify_step(&check, 25);
    lv_bar_set_value(verify_bar, check.checked * 100 / check.total, LV_ANIM_OFF);
    lv_label_set_text_fmt(verify_lbl, "%d of %d addresses", check.checked, check.total);
    if (done) {
        lv_timer_delete(verify_timer);
        verify_timer = nullptr;
        ui_goto(UI_VERIFY_RESULT);   // rebuild with the result
    }
}

// The address, plus the amount a BIP-21 URI asks for (if any). Returns its height.
static int address_card(lv_obj_t *parent, int y) {
    bool req = check.amount || check.amount_bad;
    lv_obj_t *c = card(parent, CW, 96);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_align(caption(c, "ADDRESS"), LV_ALIGN_TOP_LEFT, 0, 0);
    address_chunks(c, check.address);
    if (!req) return 96;
    char b[40] = "Not valid";
    if (!check.amount_bad) fmt_btc(check.amount, b, sizeof(b));
    divider(c, 82);
    int h = 88 + kv(c, 88, "Requests", b, check.amount_bad ? AMBER : TEXT) + 2 * CARD_PY;
    lv_obj_set_height(c, h);
    return h;
}

bool verify_done() { return check.status != VERIFY_SEARCHING; }

static void build_verify() {
    lv_obj_t *scr = screen();
    if (check.status == VERIFY_SEARCHING) {
        topbar(scr, "Verify address");
        lv_obj_align(text(scr, "Checking" "\xE2\x80\xA6", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 12);
        int h = address_card(scr, BAR + 52);
        verify_bar = lv_bar_create(scr);
        lv_obj_set_size(verify_bar, CW, 6);
        lv_bar_set_range(verify_bar, 0, 100);
        lv_obj_set_style_bg_color(verify_bar, SURFACE_2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(verify_bar, ACCENT, LV_PART_INDICATOR);
        lv_obj_align(verify_bar, LV_ALIGN_TOP_MID, 0, BAR + 52 + h + 24);
        verify_lbl = text(scr, "", &inter_13, TEXT_3);
        lv_obj_align(verify_lbl, LV_ALIGN_TOP_MID, 0, BAR + 52 + h + 40);
        show(scr);
        if (verify_timer) lv_timer_delete(verify_timer);
        verify_timer = lv_timer_create(verify_tick, 30, nullptr);
        return;
    }

    bool ok = check.status == VERIFY_FOUND;
    bool req = check.amount || check.amount_bad;   // tighter, to make room for the requested amount
    lv_obj_align(icon_badge(scr, ok ? ICON_CHECK : ICON_WARNING, ok ? GREEN : RED, ok ? GREEN_DIM : RED_DIM, 44),
                 LV_ALIGN_TOP_MID, 0, req ? 6 : 16);
    lv_obj_align(text(scr, ok ? "Address verified" : "Not from this seed", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0,
                 req ? 54 : 68);
    if (ok) {
        char tag[40];
        snprintf(tag, sizeof(tag), ICON_CHECK "  %s #%d%s", check.chain ? "CHANGE" : "RECEIVE", check.index,
                 check.testnet ? "  \xC2\xB7  TESTNET" : "");
        lv_obj_align(chip(scr, tag, GREEN, GREEN_DIM), LV_ALIGN_TOP_MID, 0, req ? 88 : 104);
        address_card(scroll_area(scr, req ? 120 : 140), 0);
    } else {
        char m[96];
        snprintf(m, sizeof(m), "Checked %d addresses. Don't send here unless you're sure.", check.total);
        lv_obj_align(para(scr, m, &inter_13, TEXT_2), LV_ALIGN_TOP_MID, 0, req ? 84 : 100);
        address_card(scroll_area(scr, req ? 120 : 146), 0);
    }
    bottom_pair(btn_ghost(scr, "Scan", go_cb, GO(UI_SCAN_ADDRESS), TEXT), btn_primary(scr, "Done", to_menu_cb));
    show(scr);
}

// ---------------------------------------------------------------- navigation
// Making or entering a seed: only while none is loaded.
static bool seed_entry_screen(UiScreen s) {
    return s == UI_NEW_SEED || s == UI_ENTROPY_DICE || s == UI_ENTROPY_COINS || s == UI_RESTORE || s == UI_ENTER_WORDS;
}
// The card flow: for a new seed, or the loaded one.
static bool backup_screen(UiScreen s) {
    return s == UI_BACKUP_WARNING || s == UI_BACKUP_WORDS || s == UI_BACKUP_CHOICE || s == UI_BACKUP_QR ||
           s == UI_BACKUP_BLOCK || s == UI_BACKUP_SCAN || s == UI_SEED_READY;
}

void ui_goto(UiScreen s) {
    if (s != UI_SCAN_SEED && s != UI_SCAN_PSBT && s != UI_SCAN_ADDRESS && s != UI_BACKUP_SCAN) scan_stop();
    // The new seed lives only inside its own flow: leaving it wipes everything.
    if (!seed_entry_screen(s) && !backup_screen(s)) new_seed_wipe();
    bool needs_pending = backup_screen(s) && s != UI_BACKUP_BLOCK && s != UI_BACKUP_SCAN;
    bool needs_qr = s == UI_BACKUP_BLOCK || s == UI_BACKUP_SCAN;
    if ((needs_pending && !pending.len) || (needs_qr && (!pending.len || !qr_n))) { new_seed_wipe(); s = UI_HOME; }
    if (s != UI_VERIFY_RESULT && verify_timer) { lv_timer_delete(verify_timer); verify_timer = nullptr; }
    if (s != UI_PSBT_SIGNED && anim_timer) { lv_timer_delete(anim_timer); anim_timer = nullptr; }
    bool no_seed_yet = s == UI_HOME || s == UI_SCAN_SEED || seed_entry_screen(s);
    if (!no_seed_yet && !backup_screen(s) && !wallet_has_seed()) s = UI_HOME;
    if (no_seed_yet && wallet_has_seed()) { new_seed_wipe(); s = UI_SEED_MENU; }
    current = s;
    switch (s) {
        case UI_HOME: build_home(); break;
        case UI_SCAN_SEED: build_scan(SCAN_SEED); break;
        case UI_SEED_LOADED: build_seed_loaded(); break;
        case UI_SEED_MENU: build_seed_menu(); break;
        case UI_SCAN_PSBT: build_scan(SCAN_PSBT); break;
        case UI_SCAN_ADDRESS: build_scan(SCAN_ADDRESS); break;
        case UI_VERIFY_RESULT: build_verify(); break;
        case UI_EXPORT_XPUB: build_export_xpub(); break;
        case UI_NETWORK: build_network(); break;
        case UI_WIPE_CONFIRM: build_wipe_confirm(); break;
        case UI_PSBT_ERROR: build_psbt_error(); break;
        case UI_PSBT_OVERVIEW: build_psbt_overview(); break;
        case UI_PSBT_OUTPUT: build_psbt_output(); break;
        case UI_PSBT_SIGN: build_psbt_sign(); break;
        case UI_PSBT_SIGNED: build_psbt_signed(); break;
        case UI_NEW_SEED: build_new_seed(); break;
        case UI_ENTROPY_DICE: build_entropy(true); break;
        case UI_ENTROPY_COINS: build_entropy(false); break;
        case UI_RESTORE: build_restore(); break;
        case UI_ENTER_WORDS: build_enter_words(); break;
        case UI_BACKUP_WARNING: build_backup_warning(); break;
        case UI_BACKUP_WORDS: build_backup_words(); break;
        case UI_BACKUP_CHOICE: build_backup_choice(); break;
        case UI_SEED_READY: build_seed_ready(); break;
        case UI_BACKUP_QR: build_backup_qr(); break;
        case UI_BACKUP_BLOCK: build_backup_block(); break;
        case UI_BACKUP_SCAN: build_scan(SCAN_CONFIRM); break;
    }
}

UiScreen ui_current() { return current; }

void ui_build() { ui_goto(UI_HOME); }
