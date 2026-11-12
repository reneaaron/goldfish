#include "ui.h"
#include <lvgl.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <Conversion.h>   // fromBase64 (uBitcoin)
#include "psbt_review.h"
#include "qrscan.h"
#include "seedqr.h"
#include "theme.h"
#include "ur.h"
#include "wallet.h"

using namespace th;

static UiScreen current = UI_HOME;

static void show(lv_obj_t *scr) { lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, true); }
static void go_cb(lv_event_t *e) { ui_goto((UiScreen)(intptr_t)lv_event_get_user_data(e)); }
static void to_menu_cb(lv_event_t *) { ui_goto(UI_SEED_MENU); }
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
    lv_obj_align(icon_badge(scr, ICON_QR, ACCENT, ACCENT_DIM, 76), LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_align(text(scr, "Signer", &inter_30_semi, TEXT), LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_align(para(scr, "Scan your seed QR to begin. Nothing is saved: unplug and it's gone."), LV_ALIGN_TOP_MID, 0, 172);
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
static bool scanning_seed;
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
    if (scanning_seed) handle_seed_payload(p);
    else handle_psbt_payload(p);
    memset(&p, 0, sizeof(p));
}

static void scan_back_cb(lv_event_t *) {
    scan_stop();
    ur_decoder_reset(&ur_dec);
    ui_goto(scanning_seed ? UI_HOME : UI_SEED_MENU);
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

static void build_scan(bool seed) {
    scanning_seed = seed;
    if (!seed) ur_decoder_reset(&ur_dec);
    lv_obj_t *scr = screen();
    topbar(scr, seed ? "Scan seed" : "Scan transaction", scan_back_cb);

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

    scan_msg = text(scr, seed ? "Point at your seed QR" : "Point at your wallet's QR", &inter_17_semi, TEXT);
    lv_obj_align(scan_msg, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + 14);
    scan_sub = text(scr, seed ? "Hold it flat and well lit" : "Sparrow, Nunchuk or any PSBT wallet",
                    &inter_13, TEXT_3);
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
    lv_obj_align(text(scr, "Seed loaded", &inter_22_semi, TEXT), LV_ALIGN_TOP_MID, 0, 92);

    // The fingerprint is what the user checks, so it gets the space.
    lv_obj_align(caption(scr, "FINGERPRINT"), LV_ALIGN_TOP_MID, 0, 138);
    char fp[9], spaced[12];
    wallet_fingerprint_hex(fp);
    snprintf(spaced, sizeof(spaced), "%.4s %.4s", fp, fp + 4);
    lv_obj_align(text(scr, spaced, &mono_26, TEXT), LV_ALIGN_TOP_MID, 0, 156);

    place_bottom(btn_primary(scr, "Continue", go_cb, GO(UI_SEED_MENU)));
    show(scr);
}

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

    lv_obj_t *c = card(scr, CW, 126);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *r1 = list_row(c, ICON_SIGN, "Sign transaction", nullptr, go_cb, GO(UI_SCAN_PSBT));
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 0);
    divider(c, 62);
    lv_obj_t *r2 = list_row(c, ICON_WALLET, "Connect wallet", nullptr, go_cb, GO(UI_EXPORT_XPUB));
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 63);


    lv_obj_align(quiet_link(scr, ICON_TRASH "  Wipe now", go_cb, GO(UI_WIPE_CONFIRM)), LV_ALIGN_BOTTOM_MID, 0, -12);
    show(scr);
}

static void build_export_xpub() {
    lv_obj_t *scr = screen();
    topbar(scr, "Connect wallet", to_menu_cb);

    static char xpub[180];
    size_t n = wallet_export_xpub(xpub, sizeof(xpub));
    lv_obj_t *qr;
    qr_card(scr, BAR + 6, &qr);
    lv_qrcode_update(qr, xpub, n);

    lv_obj_align(text(scr, "Scan with your wallet app", &inter_15, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 6 + 176 + 10);
    lv_obj_align(text(scr, "xpub  \xC2\xB7  Native SegWit  \xC2\xB7  m/84'/0'/0'", &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0,
                 BAR + 6 + 176 + 34);
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
    char s[32], b[48];
    fmt_sats(headline, s, sizeof(s));
    snprintf(b, sizeof(b), "%s sats", s);
    lv_obj_align(text(scr, b, &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, BAR + 64);

    lv_obj_t *c = card(scr, CW, 112);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 92);
    int nrec = recipient_count();
    snprintf(b, sizeof(b), "%d", nrec);
    kv(c, 0, nrec == 1 ? "Recipient" : "Recipients", b);
    divider(c, 26);
    fmt_sats(review.fee, s, sizeof(s));
    snprintf(b, sizeof(b), "%s sats", s);
    kv(c, 34, "Network fee", b);
    divider(c, 60);
    if (review.change) {
        char btc[32];
        fmt_btc_grouped(review.change, btc, sizeof(btc));
        snprintf(b, sizeof(b), ICON_CHECK " %s", btc);
        kv(c, 68, "Change", b, GREEN);
    } else {
        kv(c, 68, "Change", "None", TEXT_3);
    }

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
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 84);
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

    lv_obj_t *c = card(scr, CW, 106);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 18);
    uint64_t send = review.spend ? review.spend : review.self;
    char b[32];
    fmt_btc_grouped(send, b, sizeof(b));
    kv(c, 0, review.spend ? "Send" : "Move", b, TEXT, &mono_16);
    fmt_btc_grouped(review.fee, b, sizeof(b));
    kv(c, 28, "Fee", b, TEXT_2, &mono_16);
    divider(c, 56);
    fmt_btc_grouped(send + review.fee, b, sizeof(b));
    kv(c, 64, "Total", b, ACCENT, &mono_16);

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
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, BAR + 18 + 106 + 18);
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

// ---------------------------------------------------------------- navigation
void ui_goto(UiScreen s) {
    if (s != UI_SCAN_SEED && s != UI_SCAN_PSBT) scan_stop();
    if (s != UI_PSBT_SIGNED && anim_timer) { lv_timer_delete(anim_timer); anim_timer = nullptr; }
    bool needs_seed = s != UI_HOME && s != UI_SCAN_SEED;
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
        case UI_PSBT_ERROR: build_psbt_error(); break;
        case UI_PSBT_OVERVIEW: build_psbt_overview(); break;
        case UI_PSBT_OUTPUT: build_psbt_output(); break;
        case UI_PSBT_SIGN: build_psbt_sign(); break;
        case UI_PSBT_SIGNED: build_psbt_signed(); break;
    }
}

UiScreen ui_current() { return current; }

void ui_build() { ui_goto(UI_HOME); }
