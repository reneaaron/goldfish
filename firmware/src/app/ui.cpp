#include "ui.h"
#include <lvgl.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <wally_core.h>
#include <src/libs/qrcode/qrcodegen.h>
#include "address_verify.h"
#include "amount.h"
#include "bip85.h"
#include "multisig.h"
#include "page_reader.h"
#include "psbt_review.h"
#include "seed_create.h"
#include "qrscan.h"
#include "seedqr.h"
#include "theme.h"
#include "ur.h"
#include "version.h"
#include "wallet.h"
#include "word_reader.h"
#include "src/libs/qrcode/qrcodegen.h"

using namespace th;

static UiScreen current = UI_HOME;
static bool fresh_backup;                // Seed loaded screen follows a checked backup

static void show(lv_obj_t *scr) { lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, true); }
static void go_cb(lv_event_t *e) { ui_goto((UiScreen)(intptr_t)lv_event_get_user_data(e)); }
static void to_menu_cb(lv_event_t *) { ui_goto(UI_WALLET_OPTIONS); }
static void to_wallet_cb(lv_event_t *) { ui_goto(UI_WALLET); }
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
    // Full width, with the side margins as padding: the scrollbar sits at the
    // very edge of the screen, clear of the cards.
    lv_obj_set_size(a, W, H - M - BTN_H - 4 - y);
    lv_obj_set_style_pad_hor(a, M, 0);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_scroll_dir(a, LV_DIR_VER);
    lv_obj_set_style_pad_right(a, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(a, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(a, TEXT_3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(a, 2, LV_PART_SCROLLBAR);
    return a;
}

// White rounded card holding a QR code (the white is the quiet zone).
// The QR is drawn here, not with lv_qrcode: that one always uses byte mode and
// ECC medium, then grows the version to fill the space at the same scale, so an
// animated PSBT frame came out 73 modules wide at 2 px. Here an uppercase UR
// part goes in alphanumeric mode at ECC low, at the smallest version, as large
// as fits: a 30-byte fragment is 33 modules at 5 px (about 0.64 mm each), a
// 41-module code 4 px.
constexpr int QR_PX = 165;               // the code itself: 33 modules x 5 px
constexpr int QR_CARD = QR_PX + 16;      // plus the white quiet zone
static const lv_color_t QR_LIGHT = lv_color_white();
// A 1-bit canvas for a QR, side x side, in the given static buffer: palette
// index 0 light, 1 dark.
static lv_obj_t *qr_canvas(lv_obj_t *parent, lv_draw_buf_t *buf, uint8_t *data, uint32_t size, int side) {
    lv_draw_buf_init(buf, side, side, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO, data, size);
    lv_draw_buf_set_flag(buf, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_obj_t *qr = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(qr, buf);
    lv_canvas_set_palette(qr, 0, lv_color_to_32(QR_LIGHT, LV_OPA_COVER));
    lv_canvas_set_palette(qr, 1, lv_color_to_32(lv_color_black(), LV_OPA_COVER));
    lv_obj_center(qr);
    return qr;
}

static lv_obj_t *qr_card(lv_obj_t *parent, int y, lv_obj_t **qr_out) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, QR_CARD, QR_CARD);
    lv_obj_set_style_bg_color(c, QR_LIGHT, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_border_color(c, BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    static uint8_t buf_data[LV_DRAW_BUF_SIZE(QR_PX, QR_PX, LV_COLOR_FORMAT_I1)];
    static lv_draw_buf_t buf;
    lv_obj_t *qr = qr_canvas(c, &buf, buf_data, sizeof(buf_data), QR_PX);
    *qr_out = qr;
    return c;
}

// Draws text (NUL-terminated) into a qr_card's canvas.
static void qr_set(lv_obj_t *canvas, const char *s) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(canvas);
    uint8_t *px = buf->data + 8;   // past the two-colour palette
    uint32_t stride = buf->header.stride;
    int side = buf->header.w;   // square: QR_PX, or QR_FULL when tapped full screen
    memset(px, 0, stride * side);
    if (qrcodegen_encodeText(s, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                             qrcodegen_Mask_AUTO, false)) {
        int n = qrcodegen_getSize(qr), scale = side / n, off = (side - n * scale) / 2;
        for (int y = 0; y < n * scale; y++)
            for (int x = 0; x < n * scale; x++)
                if (qrcodegen_getModule(qr, x / scale, y / scale))
                    px[(off + y) * stride + ((off + x) >> 3)] |= 0x80 >> ((off + x) & 7);
    }
    lv_image_cache_drop(buf);
    lv_obj_invalidate(canvas);
}

// ---------------------------------------------------------------- home
static lv_obj_t *quiet_link(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud);
static void restore_cb(lv_event_t *);
static void child_start_cb(lv_event_t *);

// The keys are narrower than a fingertip, so a character key (KB_CHAR) shows
// itself in a balloon above the finger and is only typed on release. LVGL
// drops the press when the finger slides off the key; kb_touch_cb keeps it
// and follows the finger:
// - onto another character key: that one is held instead, so a wrong key is
//   corrected by sliding;
// - onto anything else (a key that is off, Del, OK): the held key stays;
// - well above the keyboard: the press is called off.
// A lifting finger rolls and often reports a last position on the next key,
// so a key held for less than KB_SETTLE ms when the finger lifts gives way to
// the one held before it (kb_lift_cb).
constexpr int KB_CHAR = LV_BUTTONMATRIX_CTRL_POPOVER | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
constexpr uint32_t KB_NONE = LV_BUTTONMATRIX_BUTTON_NONE, KB_SETTLE = 100;
constexpr int KB_CALL_OFF = 24;   // px above the keyboard
static bool kb_sliding;           // a key callback during the pick-up isn't a key press
static bool kb_follow;            // this press began on a character key (or on none)
static uint32_t kb_held = KB_NONE, kb_before = KB_NONE, kb_since;
static const char *const *kb_landed_map;   // the keys when the finger landed

static bool kb_is_char(lv_obj_t *kb, uint32_t b) {
    return b != KB_NONE && lv_buttonmatrix_has_button_ctrl(kb, b, LV_BUTTONMATRIX_CTRL_POPOVER);
}

static void kb_touch_cb(lv_event_t *e) {
    if (kb_sliding) return;
    lv_obj_t *kb = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t b = lv_buttonmatrix_get_selected_button(kb);
    if (code == LV_EVENT_PRESSED) {
        // A layer key has swapped the keys under the finger; LVGL still holds
        // its index, which can be a character (or past the end) in the new map.
        bool swapped = lv_buttonmatrix_get_map(kb) != kb_landed_map;
        if (swapped) lv_buttonmatrix_set_selected_button(kb, b = KB_NONE);
        kb_follow = !swapped && (b == KB_NONE || kb_is_char(kb, b));   // Del, OK and the layer keys stay as LVGL has them
        kb_held = kb_before = b;
    } else if (code == LV_EVENT_PRESSING && kb_follow) {
        lv_point_t p;
        lv_indev_get_point(lv_event_get_indev(e), &p);
        lv_area_t a;
        lv_obj_get_coords(kb, &a);
        if (p.y < a.y1 - KB_CALL_OFF) {
            b = KB_NONE;
        } else if (b == KB_NONE) {               // LVGL let go of the key, or there was none
            kb_sliding = true;
            lv_obj_send_event(kb, LV_EVENT_PRESSED, lv_event_get_indev(e));   // the key under the finger
            kb_sliding = false;
            b = lv_buttonmatrix_get_selected_button(kb);
            if (!kb_is_char(kb, b)) b = kb_held;
        }
        lv_buttonmatrix_set_selected_button(kb, b);
        if (b != kb_held) {
            kb_before = kb_held;
            kb_held = b;
            kb_since = lv_tick_get();
        }
    }
    // LVGL only redraws the key's own column; the balloon is wider.
    static uint32_t shown = KB_NONE;
    b = lv_obj_has_state(kb, LV_STATE_PRESSED) ? lv_buttonmatrix_get_selected_button(kb) : KB_NONE;
    if (b != shown) lv_obj_invalidate(kb);
    shown = b;
}

// Before LVGL acts on the key the finger lands on.
static void kb_land_cb(lv_event_t *e) {
    if (!kb_sliding) kb_landed_map = lv_buttonmatrix_get_map(lv_event_get_target_obj(e));
}

// Before LVGL types the held key on release.
static void kb_lift_cb(lv_event_t *e) {
    lv_obj_t *kb = lv_event_get_target_obj(e);
    if (kb_follow && kb_held != KB_NONE && kb_before != KB_NONE && lv_tick_elaps(kb_since) < KB_SETTLE)
        lv_buttonmatrix_set_selected_button(kb, kb_before);
}

// The balloon: LVGL's own popover is the pressed key stretched one row up, as
// narrow as the key and with the same small letter. Over its upper half goes a
// wider box with the character in a large font, kept inside the screen.
static void kb_draw_cb(lv_event_t *e) {
    lv_obj_t *kb = lv_event_get_target_obj(e);
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(t);
    uint32_t b = lv_buttonmatrix_get_selected_button(kb);
    if (base->part != LV_PART_ITEMS || base->id1 != b || !lv_obj_has_state(kb, LV_STATE_PRESSED) ||
        !lv_buttonmatrix_has_button_ctrl(kb, b, LV_BUTTONMATRIX_CTRL_POPOVER)) return;
    if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_LABEL) {   // the small letter: the balloon has it
        lv_draw_task_get_label_dsc(t)->opa = LV_OPA_TRANSP;
        return;
    }
    if (lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;
    constexpr int BW = 46;
    lv_area_t a;
    lv_draw_task_get_area(t, &a);                 // the key and the row above it
    a.y2 = a.y1 + (a.y2 - a.y1) / 2 - 2;
    a.x1 = LV_CLAMP(2, (a.x1 + a.x2 + 1 - BW) / 2, W - 2 - BW);
    a.x2 = a.x1 + BW - 1;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = ACCENT;
    r.radius = 10;
    lv_draw_rect(base->layer, &r, &a);
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = &mono_26;
    l.color = lv_color_white();
    l.align = LV_TEXT_ALIGN_CENTER;
    l.text = lv_buttonmatrix_get_button_text(kb, b);
    a.y1 += (a.y2 - a.y1 + 1 - lv_font_get_line_height(l.font)) / 2;
    lv_draw_label(base->layer, &l, &a);
}

// Look of the on-screen keyboards (button matrices): Enter words and Passphrase.
static void kb_style(lv_obj_t *kb) {
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_bg_color(kb, SURFACE_2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 7, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &inter_17_semi, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, ACCENT_DIM, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(kb, kb_touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(kb, kb_touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(kb, kb_touch_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(kb, kb_touch_cb, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(kb, kb_land_cb, lv_event_code_t(LV_EVENT_PRESSED | LV_EVENT_PREPROCESS), nullptr);
    lv_obj_add_event_cb(kb, kb_lift_cb,lv_event_code_t(LV_EVENT_RELEASED | LV_EVENT_PREPROCESS), nullptr);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_SCROLLABLE);   // a slide over the keys is not a scroll
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(kb, kb_draw_cb, LV_EVENT_DRAW_TASK_ADDED, nullptr);
}

static void build_home() {
    lv_obj_t *scr = screen();
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &goldfish_logo);
    // Logo, name and tagline (about 116px tall) centred in the 176px above Enter words.
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_align(text(scr, "goldfish", &rethink_32, TEXT), LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_align(para(scr, "Sign and forget."), LV_ALIGN_TOP_MID, 0, 128);
    // Which build this is, to compare with the one the flasher offers.
    // Tapping it opens Update firmware: the case has no BOOT button.
    lv_obj_t *ver = text(scr, FW_VERSION, &inter_13, TEXT_3);
    lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_flag(ver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ver, 12);
    lv_obj_add_event_cb(ver, go_cb, LV_EVENT_CLICKED, GO(UI_UPDATE));
    // The board has no Wi-Fi or Bluetooth hardware, and this firmware has no USB stack
    // (USB is power only): say so where a phone shows its radios.
    lv_obj_t *radios = text(scr, ICON_WIFI_OFF "  " ICON_BT_OFF, &inter_13, TEXT_3);
    lv_obj_align(radios, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_t *usb = lv_image_create(scr);
    lv_image_set_src(usb, &usb_off_13);
    lv_obj_update_layout(radios);
    lv_obj_align_to(usb, radios, LV_ALIGN_OUT_RIGHT_MID, 7, 0);
    // Loading a seed you have: scan it or type it, grouped at the bottom.
    lv_obj_t *words = btn_ghost(scr, ICON_KEYBOARD "   Enter words", restore_cb, nullptr, TEXT);
    lv_obj_set_height(words, 48);
    lv_obj_set_style_bg_opa(words, LV_OPA_COVER, 0);
    lv_obj_align(words, LV_ALIGN_BOTTOM_MID, 0, -40 - 48 - 8);
    lv_obj_t *scan = btn_primary(scr, ICON_CAMERA "   Scan", go_cb, GO(UI_SCAN_SEED));
    lv_obj_align(scan, LV_ALIGN_BOTTOM_MID, 0, -40);
    // Making a new seed is a different job, so it sits apart and quieter.
    lv_obj_align(quiet_link(scr, ICON_DICE "  Create new seed", go_cb, GO(UI_NEW_SEED)), LV_ALIGN_BOTTOM_MID, 0, -4);
    show(scr);
}

// ---------------------------------------------------------------- scanning
#define PREV_W 240
#define PREV_H 180
static uint8_t preview_buf[PREV_W * PREV_H];
static lv_image_dsc_t preview_dsc;
static lv_obj_t *preview_img, *scan_msg, *scan_sub, *scan_bar;
static lv_obj_t *scan_hints;   // what the wallet scanner takes, until a status
// Scan speed, per second: frames from the camera, QRs decoded, quirc's time
// per frame (average and slowest). A long press on the preview shows it in
// its own line at the bottom, above the progress bar.
static lv_obj_t *scan_stats;
static uint32_t st_t0, st_frames, st_codes, st_ms, st_max;
static lv_timer_t *scan_timer;
// SCAN_SEED: a SeedQR, read as soon as it is seen, or written words, read once the camera holds still on them
enum ScanMode { SCAN_SEED, SCAN_WALLET, SCAN_CONFIRM };
static ScanMode scan_mode;
static AddressCheck check;
static int qr_w, qr_h;
static UrDecoder ur_dec;
static PsbtReview review;
static int out_index;

// Written words on Scan seed are read without a tap, once the view has held
// still on them for STEADY_MS: the sharper picture that follows needs the
// user still anyway. A QR never starts it (any finder pattern seen restarts
// the wait), but a blurry one can look like words: Words found offers Retake.
#define STEADY_MS 2100
#define PROBE_MS 500        // at most this often: finding the words takes a while on the board
#define STEADY_DIFF 6       // mean change per pixel between frames (of 255) that counts as moving
#define PAGE_MIN_WORDS 8
static uint8_t steady_ref[(PREV_W / 4) * (PREV_H / 4)];   // the last preview, small: a picture of the seed, wiped by scan_stop
static bool steady_have;
static uint32_t steady_since, last_probe;
// Back from the page flow (Retake, Back, a failure): no new try until the
// view changes, or the same picture would be taken again at once.
static bool page_retake, page_armed;

// camera_off false: just the preview stops (reading a page takes more pictures)
static void scan_stop(bool camera_off = true) {
    if (scan_timer) lv_timer_delete(scan_timer);
    scan_timer = nullptr;
    if (camera_off) platform_camera_stop();
    qrscan_free();
    qr_w = qr_h = 0;
    memset(preview_buf, 0, sizeof(preview_buf));
    memset(steady_ref, 0, sizeof(steady_ref));
    steady_have = false;
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
    if (scan_hints) lv_obj_add_flag(scan_hints, LV_OBJ_FLAG_HIDDEN);
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
        ui_goto(ok ? UI_WALLET : UI_HOME);
        return;
    }
    seedqr_wipe(&e);
    if (r == SEEDQR_BAD_CHECKSUM || r == SEEDQR_BAD_WORD_INDEX) scan_status("Damaged QR", "The checksum is wrong", RED);
    else if (is_psbt) scan_status("That's a transaction", "Load your seed first", AMBER);
    else scan_status("No seed found", "This QR doesn't hold a seed", AMBER);
}

// A multisig descriptor shown to the scanner. It is only used once the user
// has checked it on the Multisig wallet screen (ms_confirmed).
static bool ms_confirmed;

static void descriptor_result(MsResult r) {
    if (r == MS_OK) {
        ms_confirmed = false;
        scan_stop();
        ur_decoder_reset(&ur_dec);
        ui_goto(UI_MULTISIG);
    } else if (r == MS_NOT_OURS) {
        scan_status("Wrong descriptor", "This seed isn't one of its keys", AMBER);
    } else if (r == MS_UNSUPPORTED) {
        scan_status("Not supported yet", "Only wsh(sortedmulti) multisig", AMBER);
    } else {
        scan_status("Damaged descriptor", "Export it again", RED);
    }
}

// Plain text descriptor. False if the QR isn't one.
static bool handle_descriptor_text(const QrPayload &p) {
    MsResult r = multisig_load((const char *)p.data, p.len);
    if (r == MS_NOT_DESCRIPTOR) return false;
    descriptor_result(r);
    return true;
}

// A UR part: a transaction or a multisig descriptor.
static void handle_ur(QrPayload &p) {
    const uint8_t *payload; size_t plen;
    UrResult r = ur_decoder_add(&ur_dec, (const char *)p.data, p.len, &payload, &plen);
    if (r == UR_COMPLETE && ur_dec.type == UR_TYPE_PSBT) {
        psbt_complete(payload, plen);
    } else if (r == UR_COMPLETE) {
        MsResult m = ur_dec.type == UR_TYPE_OUTPUT ? multisig_load_ur_output(payload, plen)
                                                   : multisig_load((const char *)payload, plen);
        if (m != MS_NOT_DESCRIPTOR) descriptor_result(m);
        else scan_status("Nothing to sign or verify", "Show a transaction or an address", AMBER);
    } else if (r == UR_PROGRESS) {
        char m[32];
        snprintf(m, sizeof(m), "%d%% received", ur_decoder_percent(&ur_dec));
        scan_status(m, "Hold steady until it's done", ACCENT);
        lv_obj_remove_flag(scan_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(scan_bar, ur_decoder_percent(&ur_dec), LV_ANIM_ON);
    } else if (r == UR_WRONG_TYPE) {
        scan_status("Nothing to sign or verify", "Show a transaction or an address", AMBER);
    } else if (r == UR_TOO_BIG) {
        scan_status("Transaction too big", "Spend fewer coins at once", AMBER);
    }
}

// The wallet's one scanner: a transaction to sign, an address to verify, or a
// multisig descriptor. Each is told apart by its own format, so the order only
// decides which message a QR that is none of them gets.
static void handle_wallet_payload(QrPayload &p) {
    static uint8_t raw[UR_MAX_MESSAGE];
    if (starts_with_ci(p, "ur:")) { handle_ur(p); return; }
    if (starts_with_ci(p, "cHNidP")) {
        // libwally wants a C string without line breaks: drop whitespace in place.
        size_t len = 0;
        for (size_t i = 0; i < p.len; i++)
            if (!isspace(p.data[i])) p.data[len++] = p.data[i];
        p.len = len;
        size_t n = 0;
        if (len < sizeof(p.data)) {
            p.data[len] = 0;
            if (wally_base64_to_bytes((const char *)p.data, 0, raw, sizeof(raw), &n) == WALLY_OK && n &&
                n <= sizeof(raw)) {
                psbt_complete(raw, n);
                return;
            }
        }
    }
    if (p.len >= 5 && memcmp(p.data, "psbt\xff", 5) == 0) { psbt_complete(p.data, p.len); return; }
    if (starts_with_ci(p, "B$")) { scan_status("BBQr not supported", "Export it as UR instead", AMBER); return; }
    if (handle_descriptor_text(p)) return;

    AddressParse r = address_parse(p.data, p.len, &check);
    if (r == ADDR_OK) {
        scan_stop();
        ui_goto(UI_VERIFY_RESULT);
        return;
    }
    if (r == ADDR_UNSUPPORTED) { scan_status("Not supported yet", "Only bc1q addresses for now", AMBER); return; }
    if (r == ADDR_NEEDS_DESCRIPTOR) { scan_status("Multisig address", "Scan the descriptor first", AMBER); return; }

    SeedEntropy e;
    bool seed = seedqr_decode(p.data, p.len, &e) == SEEDQR_OK;
    seedqr_wipe(&e);
    if (seed) scan_status("That's a seed", "A seed is already loaded", AMBER);
    else scan_status("Nothing to sign or verify", "Show a transaction or an address", AMBER);
}

static void handle_confirm_payload(QrPayload &p);
static bool page_probe(const uint8_t *frame, int w, int h);

// True once the view has held still on what looks like written words.
static bool page_watch(const uint8_t *frame, int w, int h) {
    uint32_t now = lv_tick_get();
    int diff = 0, k = 0;
    for (int y = 0; y < PREV_H; y += 4)
        for (int x = 0; x < PREV_W; x += 4, k++) {
            uint8_t v = preview_buf[y * PREV_W + x];
            diff += abs(v - steady_ref[k]);
            steady_ref[k] = v;
        }
    diff /= k;
    if (!steady_have) { steady_have = true; steady_since = now; return false; }
    if (diff > STEADY_DIFF) page_armed = true;
    if (diff > STEADY_DIFF || qrscan_finders()) { steady_since = now; return false; }
    if (!page_armed || now - steady_since < STEADY_MS || now - last_probe < PROBE_MS) return false;
    last_probe = now;
    return page_probe(frame, w, h);
}

static bool cam_err;   // the scan screen is showing platform_camera_error()

static void scan_tick(lv_timer_t *) {
    int w, h;
    const uint8_t *frame = platform_camera_frame(&w, &h);
    if (!frame) {
        const char *err = platform_camera_error();
        if (err && !cam_err) { scan_status("Camera error", err, RED); cam_err = true; }
        return;
    }
    if (cam_err) { scan_status("Camera is back", "Show the QR again", TEXT); cam_err = false; }
    if (w != qr_w || h != qr_h) {
        if (!qrscan_init(w, h)) { scan_status("Camera error", "Restart the device", RED); return; }
        qr_w = w; qr_h = h;
    }
    for (int y = 0; y < PREV_H; y++)
        for (int x = 0; x < PREV_W; x++)
            preview_buf[y * PREV_W + x] = frame[(y * h / PREV_H) * w + (x * w / PREV_W)];
    lv_obj_invalidate(preview_img);

    static QrPayload p;   // static: 3KB is a lot of stack on the RP2350
    uint32_t t = lv_tick_get();
    bool found = qrscan_frame(frame, &p);
    uint32_t dt = lv_tick_elaps(t);
    st_frames++;
    st_codes += found;
    st_ms += dt;
    if (dt > st_max) st_max = dt;
    if (lv_tick_elaps(st_t0) >= 1000) {
        uint32_t span = lv_tick_elaps(st_t0);
        lv_label_set_text_fmt(scan_stats, "%lu.%lu fps  %lu.%lu qr/s  %lu ms (%lu)",
                              (unsigned long)(st_frames * 1000 / span), (unsigned long)(st_frames * 10000 / span % 10),
                              (unsigned long)(st_codes * 1000 / span), (unsigned long)(st_codes * 10000 / span % 10),
                              (unsigned long)(st_frames ? st_ms / st_frames : 0), (unsigned long)st_max);
        st_t0 = lv_tick_get();
        st_frames = st_codes = st_ms = st_max = 0;
    }
    if (!found) {
        if (scan_mode == SCAN_SEED && page_watch(frame, w, h)) ui_page_read();   // this timer is gone now
        return;
    }
    if (scan_mode == SCAN_SEED) handle_seed_payload(p);
    else if (scan_mode == SCAN_WALLET) handle_wallet_payload(p);
    else handle_confirm_payload(p);
    memset(&p, 0, sizeof(p));
}

static void page_flow_wipe();
static void page_tick(lv_timer_t *t);
static lv_timer_t *page_timer;

static void scan_back_cb(lv_event_t *) {
    if (scan_mode == SCAN_SEED && page_timer) {   // finding the words: cancel, the live camera again
        page_flow_wipe();
        page_retake = true;
        ui_goto(UI_SCAN_SEED);
        return;
    }
    scan_stop();
    ur_decoder_reset(&ur_dec);
    ui_goto(scan_mode == SCAN_SEED ? UI_HOME : scan_mode == SCAN_CONFIRM ? UI_BACKUP_BLOCK : UI_WALLET);
}

// Viewfinder corner brackets around the target area of the preview.
static void corners(lv_obj_t *parent, int cx, int cy, int cw, int ch) {
    const int L = 22, T = 3;
    for (int i = 0; i < 4; i++) {
        int sx = (i & 1) ? 1 : -1, sy = (i & 2) ? 1 : -1;
        int x0 = cx + sx * cw / 2, y0 = cy + sy * ch / 2;
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

// preview_buf on screen, under the top bar.
static void preview_image(lv_obj_t *scr) {
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
}

static void build_scan(ScanMode mode) {
    scan_mode = mode;
    cam_err = false;
    bool seed = mode == SCAN_SEED;
    if (mode == SCAN_WALLET) ur_decoder_reset(&ur_dec);
    lv_obj_t *scr = screen();
    static const char *titles[] = {"Scan seed", "Scan", "Check card"};
    // The title says what is scanned; one line says what to do. The second
    // line stays empty until there is something to report, except on Scan
    // seed: there it says how words are read (nothing to tap), until a status.
    // The wallet's scanner says what it takes instead, until a status.
    static const char *prompts[] = {"QR code or written words", "", "Let's check it matches"};
    topbar(scr, titles[mode], scan_back_cb);
    preview_image(scr);
    st_t0 = lv_tick_get();
    st_frames = st_codes = st_ms = st_max = 0;
    scan_stats = text(scr, "", &inter_13, TEXT_3, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(scan_stats, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_flag(scan_stats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preview_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(preview_img, [](lv_event_t *) {
        if (lv_obj_has_flag(scan_stats, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(scan_stats, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(scan_stats, LV_OBJ_FLAG_HIDDEN);
        if (scan_hints) lv_obj_add_flag(scan_hints, LV_OBJ_FLAG_HIDDEN);   // it would sit under the readout
    }, LV_EVENT_LONG_PRESSED, nullptr);
    // a seed may be a written page: the whole picture (a QR is read anywhere in it)
    if (seed) corners(scr, W / 2, BAR + PREV_H / 2, W - 16, PREV_H - 16);
    else corners(scr, W / 2, BAR + PREV_H / 2, 150, 150);
    page_armed = !page_retake;
    page_retake = false;
    last_probe = lv_tick_get();

    scan_msg = text(scr, prompts[mode], &inter_17_semi, TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(scan_msg, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + (seed ? 18 : 14));
    scan_sub = text(scr, seed ? "Hold still to read words" : "", &inter_13, TEXT_3, LV_TEXT_ALIGN_CENTER);
    // one line each, kept off the screen edge: status texts are written to fit, this only catches a slip
    lv_obj_t *lines[] = {scan_msg, scan_sub};
    for (lv_obj_t *l : lines) {
        lv_obj_set_width(l, CW);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_obj_align(scan_sub, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + (seed ? 44 : 40));
    scan_hints = nullptr;
    if (mode == SCAN_WALLET) {
        scan_hints = text(scr, "Transactions, addresses\nand multisig wallets", &inter_15, TEXT_2, LV_TEXT_ALIGN_CENTER);
        lv_obj_align(scan_hints, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + 16);
    }

    scan_bar = lv_bar_create(scr);
    lv_obj_set_size(scan_bar, CW, 4);
    lv_bar_set_range(scan_bar, 0, 100);
    lv_obj_set_style_bg_color(scan_bar, SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan_bar, ACCENT, LV_PART_INDICATOR);
    lv_obj_align(scan_bar, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(scan_bar, LV_OBJ_FLAG_HIDDEN);

    show(scr);
    // often: the camera has the next frame under way, the tick only collects it
    scan_timer = lv_timer_create(scan_tick, 30, nullptr);
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

// Home once a seed is loaded, with the same shape as Home before one: the
// logo and a title on top, the fingerprint to check against the wallet app,
// the everyday action as the primary button at the bottom, everything else
// one step away.
static void build_wallet() {
    lv_obj_t *scr = screen();
    bool pass = wallet_has_passphrase();
    const MsInfo *ms = multisig_info();   // a multisig wallet is loaded for this seed
    const char *note = fresh_backup ? (pass ? "Backup checked. The card has no passphrase."
                                            : "Backup checked. Write the fingerprint on your card.")
                                    : nullptr;
    fresh_backup = false;
    // Chips and a backup note together don't fit under the logo: drop it.
    int y = (pass || ms) && note ? -44 : 0;
    if (!y) {
        lv_obj_t *logo = lv_image_create(scr);
        lv_image_set_src(logo, &goldfish_logo);
        lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 18);
    }
    lv_obj_align(text(scr, "Ready to sign", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, y + 80);
    char fp[9], spaced[12];
    wallet_fingerprint_hex(fp);
    snprintf(spaced, sizeof(spaced), "%.4s %.4s", fp, fp + 4);
    lv_obj_align(fingerprint_pill(scr, spaced), LV_ALIGN_TOP_MID, 0, y + 118);

    // What the fingerprint stands for, when it isn't just the seed; otherwise
    // what happens to it.
    y += 160;
    if (pass || ms) {
        lv_obj_t *row = lv_obj_create(scr);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 6, 0);
        if (pass) chip(row, ICON_SHIELD " PASSPHRASE", ACCENT, ACCENT_DIM);
        if (ms) {
            char tag[24];
            snprintf(tag, sizeof(tag), ICON_USERS " %d OF %d", ms->m, ms->n);
            chip(row, tag, ACCENT, ACCENT_DIM);
        }
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y - 4);
        y += 28;
    } else if (!note) {
        note = "Forgotten when you unplug.";
    }
    if (note) lv_obj_align(para(scr, note, &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *opts = btn_ghost(scr, ICON_WALLET "   Wallet options", go_cb, GO(UI_WALLET_OPTIONS), TEXT);
    lv_obj_set_height(opts, 48);
    lv_obj_set_style_bg_opa(opts, LV_OPA_COVER, 0);
    lv_obj_align(opts, LV_ALIGN_BOTTOM_MID, 0, -M - 48 - 8);
    lv_obj_align(btn_primary(scr, ICON_CAMERA "   Scan QR", go_cb, GO(UI_SCAN)), LV_ALIGN_BOTTOM_MID, 0, -M);
    show(scr);
}

// ---------------------------------------------------------------- passphrase
// BIP-39 passphrase, typed after the seed is loaded. The keyboard only has
// printable ASCII, which NFKD leaves as it is, so the bytes go to PBKDF2 as
// typed. The text lives in pass_buf until OK or until the screen is left
// (ui_goto wipes it); after that only the derived key remains.
constexpr int PASS_MAX = 64, PASS_COLS = 18;   // 4 lines of mono_16 in the field
static char pass_buf[PASS_MAX + 1];
static char txt_pass[PASS_MAX + 8];            // as shown: line breaks and the cursor
static bool pass_hidden;
static int pass_layer;                         // index into pass_maps
static lv_obj_t *pass_field, *pass_note, *pass_kb, *pass_toggle;
static const char *const pass_maps[4][36] = {
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
     "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
     "z", "x", "c", "v", "b", "n", "m", "Del", "\n",
     "ABC", "123", "space", "OK", ""},
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
     "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
     "Z", "X", "C", "V", "B", "N", "M", "Del", "\n",
     "abc", "123", "space", "OK", ""},
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
     "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
     ".", ",", "?", "!", "'", "#", "_", "Del", "\n",
     "abc", "#+=", "space", "OK", ""},
    {"[", "]", "{", "}", "<", ">", "\n",
     "%", "^", "*", "+", "=", "\n",
     "|", "\\", "~", "`", "Del", "\n",
     "abc", "123", "space", "OK", ""},
};

static void pass_wipe() {
    secure_zero(pass_buf, sizeof(pass_buf));
    secure_zero(txt_pass, sizeof(txt_pass));
    pass_layer = 0;                            // pass_hidden stays as the user set it
}

static void pass_refresh() {
    int n = (int)strlen(pass_buf), o = 0;
    for (int i = 0; i <= n; i++) {             // one cell per character, then the cursor
        if (i && i % PASS_COLS == 0) txt_pass[o++] = '\n';
        txt_pass[o++] = i == n ? '_' : pass_hidden ? '*' : pass_buf[i];
    }
    txt_pass[o] = 0;
    lv_label_set_text_static(pass_field, txt_pass);
    // Bottom line of the field: what an empty passphrase does, or that it's full.
    if (n == PASS_MAX) lv_label_set_text_fmt(pass_note, "%d max", PASS_MAX);
    else lv_label_set_text(pass_note, n ? "" : wallet_has_passphrase() ? "Leave empty to remove it"
                                                                        : "Letters, digits and symbols");
    lv_obj_align(pass_note, n ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(lv_obj_get_child(pass_toggle, 0), pass_hidden ? "Show" : "Hide");
}

static void pass_set_layer(int layer) {
    pass_layer = layer;
    const char *const *map = pass_maps[layer];
    lv_buttonmatrix_set_map(pass_kb, map);
    // Wider keys for everything that isn't a single character; OK looks like a
    // primary button. Only Del repeats while held: a repeating letter would add
    // characters unnoticed, and a repeating layer key would flip layers.
    // Single characters get the popover (KB_CHAR).
    static lv_buttonmatrix_ctrl_t ctrl[36];
    int b = 0;
    for (int i = 0; map[i][0]; i++) {
        if (!strcmp(map[i], "\n")) continue;
        bool key = map[i][1] != 0;
        int c = !key ? 1 : !strcmp(map[i], "space") ? 4 : !strcmp(map[i], "OK") ? 2 | LV_BUTTONMATRIX_CTRL_CHECKED : 2;
        if (strcmp(map[i], "Del")) c |= LV_BUTTONMATRIX_CTRL_NO_REPEAT;
        if (!key) c |= KB_CHAR;
        ctrl[b++] = lv_buttonmatrix_ctrl_t(c);
    }
    lv_buttonmatrix_set_ctrl_map(pass_kb, ctrl);
}

void ui_passphrase_key(const char *k) {
    if (ui_current() != UI_PASSPHRASE) return;
    size_t n = strlen(pass_buf);
    if (!strcmp(k, "OK")) {
        if (!n && !wallet_has_passphrase()) { ui_goto(UI_WALLET); return; }   // nothing to change
        lv_label_set_text_static(pass_field, "Deriving key" "\xE2\x80\xA6");
        lv_label_set_text(pass_note, "This takes a few seconds");
        lv_obj_align(pass_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_refr_now(nullptr);
        if (wallet_set_passphrase(pass_buf)) { ui_goto(UI_WALLET); return; }   // wipes pass_buf
        pass_refresh();                        // the old key is still loaded: say so, keep the text
        lv_label_set_text(pass_note, "Couldn't apply it. Try again.");
        lv_obj_align(pass_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        return;
    }
    if (!strcmp(k, "Del")) { if (n) pass_buf[n - 1] = 0; }
    else if (!strcmp(k, "abc")) pass_set_layer(0);
    else if (!strcmp(k, "ABC")) pass_set_layer(1);
    else if (!strcmp(k, "123")) pass_set_layer(2);
    else if (!strcmp(k, "#+=")) pass_set_layer(3);
    else if (n < PASS_MAX) {
        char c = !strcmp(k, "space") ? ' ' : k[1] ? 0 : k[0];
        if (c >= 0x20 && c <= 0x7E) { pass_buf[n] = c; pass_buf[n + 1] = 0; }
    }
    pass_refresh();
}

static void pass_kb_cb(lv_event_t *) {
    uint32_t b = lv_buttonmatrix_get_selected_button(pass_kb);
    if (b != LV_BUTTONMATRIX_BUTTON_NONE && !kb_sliding) ui_passphrase_key(lv_buttonmatrix_get_button_text(pass_kb, b));
}
static void pass_toggle_cb(lv_event_t *) {
    pass_hidden = !pass_hidden;
    pass_refresh();
}


static void build_passphrase() {
    lv_obj_t *scr = screen();
    lv_obj_t *bar = topbar(scr, "Passphrase", to_menu_cb);
    pass_toggle = quiet_link(bar, "Hide", pass_toggle_cb, nullptr);
    lv_obj_align(pass_toggle, LV_ALIGN_RIGHT_MID, -4, 2);

    lv_obj_t *f = card(scr, CW, 86);
    lv_obj_set_style_pad_ver(f, 6, 0);
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, BAR);
    pass_field = text(f, "", &mono_16, TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(pass_field, LV_ALIGN_TOP_LEFT, 0, 0);
    pass_note = text(f, "", &inter_13, TEXT_3, LV_TEXT_ALIGN_LEFT);

    pass_kb = lv_buttonmatrix_create(scr);
    lv_obj_set_size(pass_kb, W, 4 * 44 + 8);
    lv_obj_align(pass_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    kb_style(pass_kb);
    lv_obj_set_style_bg_color(pass_kb, TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(pass_kb, BG, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(pass_kb, TEXT_2, LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_add_event_cb(pass_kb, pass_kb_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    pass_set_layer(pass_layer);
    pass_refresh();
    show(scr);
}

static void backup_existing_cb(lv_event_t *);

// Groups by importance: what most people need once (Connect wallet, Back up),
// then the advanced ones with grey icons, then Forget seed on its own. Every row says
// what it does. The list scrolls, so more options can join it.
static void build_wallet_options() {
    lv_obj_t *scr = screen();
    topbar(scr, "Options", to_wallet_cb);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, W, H - BAR);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, BAR);
    lv_obj_set_style_pad_hor(list, M, 0);
    lv_obj_set_style_pad_top(list, 2, 0);
    lv_obj_set_style_pad_bottom(list, M, 0);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, TEXT_3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    // Same row shape everywhere, so titles and sublines line up.
    const int RH = 58;
    struct Row { const char *icon, *title, *sub; lv_event_cb_t cb; void *ud; };
    const Row rows[] = {
        {ICON_WALLET, "Connect wallet", "Set up watch-only", go_cb, GO(UI_EXPORT_XPUB)},
        {ICON_QR, "Back up as QR", "Copy it onto a card", backup_existing_cb, nullptr},
        {ICON_SHIELD, "Passphrase", wallet_has_passphrase() ? "On, tap to change" : "Optional extra word", go_cb,
         GO(UI_PASSPHRASE)},
        {ICON_KEY, "Child seed", "Derived from this one", child_start_cb, nullptr},
        {ICON_COINS, "Network", wallet_testnet() ? "Testnet" : "Mainnet", go_cb, GO(UI_NETWORK)},
        {ICON_TRASH, "Forget seed", "Unplugging does it too", go_cb, GO(UI_WIPE_CONFIRM)},
    };
    const int groups[] = {2, 3, 1};
    int at = 0;
    for (int g = 0; g < 3; g++) {
        int n = groups[g];
        lv_obj_t *c = card(list, CW, n * RH + n - 1);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_clip_corner(c, true, 0);
        for (int i = 0; i < n; i++, at++) {
            if (i) divider(c, i * (RH + 1) - 1);
            const Row &d = rows[at];
            lv_obj_t *r = list_row(c, d.icon, d.title, d.sub, d.cb, d.ud);
            lv_obj_set_height(r, RH);
            lv_obj_align(r, LV_ALIGN_TOP_MID, 0, i * (RH + 1));
            if (g) {   // past the first group: the icon in grey
                lv_obj_t *ic = lv_obj_get_child(r, 0);
                lv_obj_set_style_bg_color(ic, SURFACE_2, 0);
                lv_obj_set_style_text_color(lv_obj_get_child(ic, 0), TEXT_2, 0);
            }
        }
    }
    show(scr);
}

static int address_chunks(lv_obj_t *parent, const char *a);

// Connect wallet shows the QR, and under it what's in it: the start and end of
// the key, to compare with what the wallet app imported. Tapping the card
// swaps the QR for everything in it: fingerprint, path and the whole key.
// Which key is a plain-words toggle; the network is in Wallet options.
static bool key_details;   // the card shows the text instead of the QR

void ui_key_details() {
    if (ui_current() != UI_EXPORT_XPUB) return;
    key_details = !key_details;
    ui_goto(UI_EXPORT_XPUB);
}
static void key_details_cb(lv_event_t *) { ui_key_details(); }
static void key_type_cb(lv_event_t *) {
    wallet_set_export_multisig(!wallet_export_multisig());
    ui_goto(UI_EXPORT_XPUB);
}

static void build_export_xpub() {
    lv_obj_t *scr = screen();
    bool tn = wallet_testnet(), ms = wallet_export_multisig();
    // In the details, Back returns to the QR.
    topbar(scr, "Watch-only wallet", key_details ? key_details_cb : to_menu_cb);

    // "[fingerprint/path]key": split into its parts for the screen.
    static char xpub[180];
    size_t n = wallet_export_xpub(xpub, sizeof(xpub));
    const char *close = strchr(xpub, ']');
    const char *key = close ? close + 1 : xpub;
    char fp[9] = "", path[40] = "m";
    if (close && close - xpub > 9) {
        memcpy(fp, xpub + 1, 8);
        size_t o = 1;
        for (const char *c = xpub + 9; c < close && o + 2 < sizeof(path); c++) path[o++] = *c == 'h' ? '\'' : *c;
        path[o] = 0;
    }

    if (key_details) {
        lv_obj_t *c = card(scr, CW, LV_SIZE_CONTENT);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 2);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, key_details_cb, LV_EVENT_CLICKED, nullptr);
        // The fingerprint (its icon says what it is), then the path under it:
        // side by side they don't fit a multisig path.
        char line[24];
        snprintf(line, sizeof(line), ICON_FINGERPRINT " %.4s %.4s", fp, fp + 4);
        lv_obj_align(text(c, line, &inter_15, TEXT, LV_TEXT_ALIGN_LEFT), LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_align(text(c, path, &inter_15, TEXT_2, LV_TEXT_ALIGN_LEFT), LV_ALIGN_TOP_LEFT, 0, 20);
        lv_obj_t *box = lv_obj_create(c);   // address_chunks puts its caption room at the top
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 46);
        lv_obj_align(caption(box, "KEY"), LV_ALIGN_TOP_LEFT, 0, 0);
        address_chunks(box, key);
        show(scr);
        return;
    }

    lv_obj_t *qr;
    lv_obj_t *qc = qr_card(scr, BAR + 2, &qr);
    if (n) qr_set(qr, xpub);
    lv_obj_add_flag(qc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(qc, key_details_cb, LV_EVENT_CLICKED, nullptr);

    // What the QR is, then the start and end of the key; tap for all of it.
    // Testnet says so in amber, so it isn't mistaken for the real thing.
    const char *title = ms ? (tn ? "Testnet multisig key" : "Your multisig key") : (tn ? "Your testnet key" : "Your public key");
    lv_obj_align(text(scr, title, &inter_17_semi, tn ? AMBER : TEXT), LV_ALIGN_TOP_MID, 0, BAR + 2 + QR_CARD + 8);
    char shortkey[24] = "";
    if (n && strlen(key) >= 12) snprintf(shortkey, sizeof(shortkey), "%.8s\xE2\x80\xA6%s", key, key + strlen(key) - 4);
    lv_obj_t *sk = text(scr, shortkey, &mono_16, TEXT_2);
    lv_obj_align(sk, LV_ALIGN_TOP_MID, 0, BAR + 2 + QR_CARD + 32);
    lv_obj_add_flag(sk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sk, 8);
    lv_obj_add_event_cb(sk, key_details_cb, LV_EVENT_CLICKED, nullptr);

    // The other kind of key, in plain words.
    lv_obj_align(quiet_link(scr, ms ? "For a single-key wallet  " ICON_RIGHT : "For a multisig wallet  " ICON_RIGHT,
                            key_type_cb, nullptr),
                 LV_ALIGN_BOTTOM_MID, 0, -6);
    show(scr);
}

// ---------------------------------------------------------------- network
// Which network Connect wallet's key is for, from Wallet options. Signing and
// Verify address read the network from what they scan, not from this.
static void network_cb(lv_event_t *e) {
    wallet_set_testnet(lv_event_get_user_data(e) != nullptr);
    ui_goto(UI_WALLET_OPTIONS);
}

static void build_network() {
    lv_obj_t *scr = screen();
    topbar(scr, "Network", to_menu_cb);
    bool tn = wallet_testnet();
    lv_obj_t *c = card(scr, CW, 2 * 62 + 1);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 8);
    lv_obj_t *r1 = list_row(c, nullptr, "Mainnet", "Real bitcoin", network_cb, nullptr);
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 0);
    divider(c, 62);
    lv_obj_t *r2 = list_row(c, nullptr, "Testnet", "Also signet and Mutinynet", network_cb, (void *)1);
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 63);
    lv_obj_align(text(tn ? r2 : r1, ICON_CHECK, &inter_17_semi, ACCENT), LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_align(para(scr, "For the watch-only key. Signing reads the network from the transaction.",
                      &inter_13, TEXT_3),
                 LV_ALIGN_TOP_MID, 0, BAR + 8 + 125 + 16);
    show(scr);
}

static void wipe_cb(lv_event_t *) {
    psbt_clear();
    multisig_clear();
    wallet_wipe();
    ui_goto(UI_HOME);
}

static void build_wipe_confirm() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_TRASH, RED, RED_DIM, 64), LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_align(text(scr, "Forget seed?", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, 126);
    char fp[9], msg[128];
    wallet_fingerprint_hex(fp);
    snprintf(msg, sizeof(msg), "Removes " ICON_FINGERPRINT " %.4s %.4s until you load it again.", fp, fp + 4);
    lv_obj_align(para(scr, msg), LV_ALIGN_TOP_MID, 0, 162);
    bottom_pair(btn_ghost(scr, "Cancel", go_cb, GO(UI_WALLET_OPTIONS), TEXT), btn_danger(scr, "Forget", wipe_cb));
    show(scr);
}

// ---------------------------------------------------------------- update
// The ROM bootloader instead of a BOOT button. Only reached from Home, so no
// seed is in RAM; restarting clears RAM anyway.
static void reboot_cb(lv_event_t *) { platform_reboot_to_bootloader(); }

static void build_update() {
    lv_obj_t *scr = screen();
    lv_obj_align(text(scr, "Update firmware?", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_align(para(scr, "It restarts as a USB drive for firmware.uf2 or the flasher page. Unplug to cancel."),
                 LV_ALIGN_TOP_MID, 0, 100);
    bottom_pair(btn_ghost(scr, "Cancel", go_cb, GO(UI_HOME), TEXT), btn_primary(scr, "Restart", reboot_cb));
    show(scr);
}

// ---------------------------------------------------------------- new seed, restore, backup
// New seed is a tool: entropy only ever comes from the user (dice, coins, or
// words drawn by hand with coin flips for the last word, see seed_create.h),
// then the user writes down the words and optionally copies a QR card. The
// new seed is never loaded: the user loads it like any other, by Scan seed or
// Enter words. Enter words loads straight away. "Back up as QR" runs the card
// flow for the loaded seed.
static int new_words = 12;               // 12; Enter words can switch to 24
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
static bool child_seed;                  // pending is a BIP-85 child of the loaded seed
static UiScreen backup_cancel = UI_NEW_SEED;   // where Cancel/Discard on the warning goes
static bool child_failed;                // the last Continue couldn't derive a child
static char child_num[11] = "0";         // child number; reset when a length is picked, not by new_seed_wipe
// Seed material shown on screen. lv_label_set_text would copy it into the LVGL
// heap, which is freed without wiping; these labels point at static buffers
// (lv_label_set_text_static) that new_seed_wipe() zeroes.
static char txt_words[24][12], txt_field[16], txt_sugg[2][12], txt_tail[40];

// ---------------------------------------------------------------- written page
// Written words on Scan seed: one still of the written backup (platform_camera_still), its
// words found by page_reader in reading order, each word's strip cut from a
// sharper picture taken in bands of rows (platform_camera_band), then each
// read by word_reader, then one list of the words ("slots") to check against
// the backup. The strips are kept in the still's memory until the list is
// done, to show each word's own picture; they and the words are wiped by
// page_flow_wipe().
struct PageCand { int16_t word; bool ok, cut; uint8_t piece; int16_t y0, y1; };
// word -1: unknown; piece -1: no picture; guess: an unknown word's best reading,
// not sure enough to fill in (often right in poor light), -1 if none
struct Slot { int16_t word; int16_t piece; bool typed; int16_t guess = -1; };
#define MAX_SLOTS 48
static Page page;
static uint8_t *page_still;   // the still, then the strips' store (page_room bytes)
static int page_w, page_h;
static size_t page_room;
// A word's strip in the store: its first sw columns (the word), the rest paper of shade med.
struct StoredStrip { uint32_t off; uint8_t sw, med; };   // sw 0: the store was full
static StoredStrip stored[PAGE_MAX_WORDS];
static size_t store_used;
static uint8_t *band_buf;   // one band of the sharper picture, band_rows x page_w * CAM_ZOOM
static int band_rows;
enum PageStep { PAGE_FIND, PAGE_BANDS, PAGE_READ };
static PageStep page_step;
static const char *page_err;   // the sharp picture failed: why, shown on Words found
static PageCand cands[PAGE_MAX_WORDS];
static int ncands, read_i;
static Slot slots[MAX_SLOTS];
static int nslots, open_slot;
// Check words' scroll, kept while a word is fixed: back on the list where it was left.
static lv_obj_t *page_review_list;
static int32_t page_review_y;

static int edit_slot = -1;   // the keyboard is filling (or inserting at) this slot
static bool edit_insert;
// "Remove extra word": the list waits for a tap on the word to drop (only the
// user can tell a heading or a passphrase line from a seed word).
static bool page_picking_extra;
// The last word's checksum-fitting suggestion, and the slot it was worked out for (-1: none).
static int page_suggest = -1, page_suggest_slot = -1;
static lv_timer_t *suggest_timer;
static uint8_t page_strip_buf[WR_H * WR_W];
static lv_image_dsc_t page_strip_dsc;

// page_strip_buf as an image source, its old picture dropped from LVGL's cache.
static const lv_image_dsc_t *page_strip_src() {
    page_strip_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    page_strip_dsc.header.cf = LV_COLOR_FORMAT_L8;
    page_strip_dsc.header.w = WR_W;
    page_strip_dsc.header.h = WR_H;
    page_strip_dsc.header.stride = WR_W;
    page_strip_dsc.data_size = sizeof(page_strip_buf);
    page_strip_dsc.data = page_strip_buf;
    lv_image_cache_drop(&page_strip_dsc);
    return &page_strip_dsc;
}

static void page_flow_wipe() {
    if (page_timer) { lv_timer_delete(page_timer); page_timer = nullptr; }
    if (suggest_timer) { lv_timer_delete(suggest_timer); suggest_timer = nullptr; }
    page_suggest = page_suggest_slot = -1;
    if (band_buf) { secure_zero(band_buf, (size_t)band_rows * page_w * CAM_ZOOM); free(band_buf); }
    band_buf = nullptr;
    if (page_still) { platform_camera_stop(); platform_camera_still_free(); }
    secure_zero(preview_buf, sizeof(preview_buf));   // Words found's copy of the photo
    page_still = nullptr;
    secure_zero(stored, sizeof(stored));
    store_used = 0;
    word_reader_free();
    secure_zero(&page, sizeof(page));
    secure_zero(cands, sizeof(cands));
    secure_zero(slots, sizeof(slots));
    secure_zero(page_strip_buf, sizeof(page_strip_buf));
    page_picking_extra = false;
    page_err = nullptr;
    ncands = read_i = nslots = open_slot = 0;
    edit_slot = -1;
}

static void new_seed_wipe() {
    page_flow_wipe();
    seedqr_wipe(&pending);
    secure_zero(entropy_in, sizeof(entropy_in));
    secure_zero(word_idx, sizeof(word_idx));
    secure_zero(word_prefix, sizeof(word_prefix));
    secure_zero(last_ok, sizeof(last_ok));
    secure_zero(qr_buf, sizeof(qr_buf));
    secure_zero(txt_words, sizeof(txt_words));
    secure_zero(txt_field, sizeof(txt_field));
    secure_zero(txt_sugg, sizeof(txt_sugg));
    secure_zero(txt_tail, sizeof(txt_tail));
    entropy_n = word_n = qr_n = block_i = 0;
    backup_existing = drawing = final_flips = card_checked = child_seed = false;
    backup_cancel = UI_NEW_SEED;
}

void ui_seed_entry(UiScreen method, int words, bool draw) {
    new_seed_wipe();
    new_words = words == 24 ? 24 : 12;
    drawing = draw;
    ui_goto(method);
}

// New seeds are always 12 words; 24 only exists for entering an existing seed.
static void method_cb(lv_event_t *e) { ui_seed_entry((UiScreen)(intptr_t)lv_event_get_user_data(e), 12); }
static void draw_cb(lv_event_t *) { ui_seed_entry(UI_ENTER_WORDS, 12, true); }
static void to_new_seed_cb(lv_event_t *) { ui_seed_entry(UI_NEW_SEED, 12); }
static void restore_cb(lv_event_t *) { ui_seed_entry(UI_ENTER_WORDS, 12); }
// "24 words" / "12 words" on the first word screen of Enter words.
static void restore_length_cb(lv_event_t *) { ui_seed_entry(UI_ENTER_WORDS, new_words == 12 ? 24 : 12); }

void ui_backup_loaded() {
    new_seed_wipe();
    if (!wallet_entropy(&pending)) return;
    new_words = wallet_word_count();
    backup_existing = true;
    backup_cancel = UI_WALLET_OPTIONS;
    ui_goto(UI_BACKUP_WARNING);
}
static void backup_existing_cb(lv_event_t *) { ui_backup_loaded(); }
// Leaving the card flow: back to the wallet, or for a new seed, to Seed ready.
static void backup_exit_cb(lv_event_t *) { ui_goto(backup_existing ? UI_WALLET_OPTIONS : UI_SEED_READY); }

static void build_new_seed() {
    lv_obj_t *scr = screen();
    topbar(scr, "New seed", to_home_cb);

    lv_obj_t *c = card(scr, CW, 3 * 62 + 2);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 4);
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
    snprintf(draw, sizeof(draw), "%d from a hat", new_words);
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
    char *t = txt_tail;
    int show_n = entropy_dice ? 10 : 16, from = entropy_n > show_n ? entropy_n - show_n : 0, o = 0;
    for (int i = from; i < entropy_n; i++) {
        if (i > from && (entropy_dice || (i % 4) == 0)) t[o++] = ' ';
        t[o++] = entropy_in[i];
    }
    t[o] = 0;
    lv_label_set_text_static(ent_tail, t);
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

// A keypad key: dice, coins and the child number keypad.
static lv_obj_t *key_button(lv_obj_t *scr, int x, int y, int w, int h, lv_event_cb_t cb, intptr_t v) {
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
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)v);
    return b;
}

static lv_obj_t *entropy_key(lv_obj_t *scr, int x, int y, int w, int h, const char *icon, const char *small, char v) {
    lv_obj_t *b = key_button(scr, x, y, w, h, entropy_key_cb, v);
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
    ent_count = text(scr, "", &rethink_24, TEXT);
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
    snprintf(txt_field, sizeof(txt_field), "%s_", word_prefix);
    lv_label_set_text_static(word_field, txt_field);
    int n = word_prefix[0] ? word_matches(word_prefix, sugg_idx, 2) : 0;
    for (int i = 0; i < 2; i++) {
        if (i < n) {
            lv_obj_remove_flag(word_sugg[i], LV_OBJ_FLAG_HIDDEN);
            snprintf(txt_sugg[i], sizeof(txt_sugg[i]), "%s", seed_word(sugg_idx[i]));
            lv_label_set_text_static(lv_obj_get_child(word_sugg[i], 0), txt_sugg[i]);
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

static bool load_words();
static bool page_set_word(int idx);

static bool word_accept(int idx) {
    if (edit_slot >= 0) return page_set_word(idx);   // the keyboard was opened from the list
    if (idx < 0 || !word_allowed(idx)) return false;
    word_idx[word_n++] = uint16_t(idx);
    word_prefix[0] = 0;
    if (drawing && word_n == new_words - 1) {   // the last word comes from coin flips
        final_flips = true;
        entropy_n = 0;
        ui_goto(UI_ENTROPY_COINS);
        return true;
    }
    if (word_n == new_words) return load_words();
    if (word_is_last()) {
        memset(last_ok, 0, sizeof(last_ok));
        uint16_t last[128];
        int n = seed_valid_last_words(word_idx, new_words, last);
        for (int i = 0; i < n; i++) last_ok[last[i] >> 3] |= 1 << (last[i] & 7);
        secure_zero(last, sizeof(last));
    }
    ui_goto(UI_ENTER_WORDS);
    return true;
}

// All of word_idx in: load the seed (or back to the start if the checksum is wrong).
static bool load_words() {
    SeedEntropy e;
    bool ok = seed_from_words(word_idx, word_n, &e);
    secure_zero(word_idx, sizeof(word_idx));
    word_n = 0;
    if (ok && word_field && ui_current() == UI_ENTER_WORDS) {
        lv_label_set_text(word_field, "Loading" "\xE2\x80\xA6");
        lv_refr_now(nullptr);
    }
    ok = ok && wallet_load(e);
    seedqr_wipe(&e);
    ui_goto(ok ? UI_WALLET : UI_HOME);
    return ok;
}

bool ui_enter_word(const char *w) {
    int first;
    if (ui_current() != UI_ENTER_WORDS || !seed_words_with_prefix(w, &first) || strcmp(seed_word(first), w)) return false;
    return word_accept(first);
}

static void sugg_cb(lv_event_t *e) { word_accept(sugg_idx[(intptr_t)lv_event_get_user_data(e)]); }

static void kb_cb(lv_event_t *e) {
    uint32_t b = lv_buttonmatrix_get_selected_button(word_kb);
    if (b == LV_BUTTONMATRIX_BUTTON_NONE || kb_sliding) return;
    size_t l = strlen(word_prefix);
    if (b == 26) { if (l) word_prefix[l - 1] = 0; }
    else if (l + 1 < sizeof(word_prefix)) { word_prefix[l] = lv_buttonmatrix_get_button_text(word_kb, b)[0]; word_prefix[l + 1] = 0; }
    words_refresh();
}

static void words_back_cb(lv_event_t *) {
    if (edit_slot >= 0) { edit_slot = -1; word_prefix[0] = 0; ui_goto(UI_PAGE_REVIEW); return; }   // back to the list
    if (!word_n) { if (drawing) to_new_seed_cb(nullptr); else ui_goto(UI_HOME); return; }
    word_idx[--word_n] = 0;
    word_prefix[0] = 0;
    ui_goto(UI_ENTER_WORDS);
}

static void build_enter_words() {
    lv_obj_t *scr = screen();
    char title[24];
    snprintf(title, sizeof(title), "Word %d of %d", (edit_slot >= 0 ? edit_slot : word_n) + 1,
             drawing ? new_words - 1 : new_words);
    topbar(scr, title, words_back_cb);
    lv_obj_t *f = card(scr, CW, 40);
    lv_obj_set_style_pad_ver(f, 0, 0);
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, BAR + 2);
    word_field = text(f, "", &mono_22, TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(word_field, LV_ALIGN_LEFT_MID, 0, 0);
    if (word_is_last()) lv_obj_align(text(f, "fits the checksum", &inter_13, TEXT_3), LV_ALIGN_RIGHT_MID, 0, 0);

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
    kb_style(word_kb);
    lv_obj_set_style_bg_opa(word_kb, LV_OPA_30, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(word_kb, TEXT_3, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_buttonmatrix_set_button_width(word_kb, 26, 2);   // "Del" spans two keys
    for (int b = 0; b < 26; b++) lv_buttonmatrix_set_button_ctrl(word_kb, b, lv_buttonmatrix_ctrl_t(KB_CHAR));
    lv_obj_add_event_cb(word_kb, kb_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    // Enter words starts at 12; a seed with 24 switches here, before the first word.
    if (!drawing && !word_n && edit_slot < 0)   // not while fixing one word of a page
        lv_obj_align(quiet_link(scr, new_words == 12 ? "24 words" : "12 words", restore_length_cb, nullptr),
                     LV_ALIGN_TOP_MID, 0, BAR + 92);
    lv_obj_move_foreground(word_kb);   // its key balloons go over the link
    words_refresh();
    show(scr);
}


// ---------------------------------------------------------------- written page screens
// A box over word candidate i on the photo under the top bar (amber: a gap
// where a word should be). Shared styles: a page can have 160 boxes, each a bare object.
static lv_style_t box_style, gap_style;
static lv_obj_t *word_box(lv_obj_t *scr, int i) {
    static bool styled;
    if (!styled) {
        lv_style_init(&box_style);
        lv_style_set_border_width(&box_style, 1);
        lv_style_set_border_color(&box_style, ACCENT);
        lv_style_set_bg_opa(&box_style, LV_OPA_TRANSP);
        lv_style_init(&gap_style);
        lv_style_set_border_color(&gap_style, AMBER);
        styled = true;
    }
    float xy[8], x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    page_corners(&page, cands[i].piece, xy);
    for (int k = 0; k < 4; k++) {
        x0 = fminf(x0, xy[2 * k]); x1 = fmaxf(x1, xy[2 * k]);
        y0 = fminf(y0, xy[2 * k + 1]); y1 = fmaxf(y1, xy[2 * k + 1]);
    }
    int bx = int(x0 * PREV_W / page_w) - 1, by = int(y0 * PREV_H / page_h) - 1;
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &box_style, 0);
    if (page.words[cands[i].piece].guessed) lv_obj_add_style(o, &gap_style, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, bx, BAR + by);
    lv_obj_set_size(o, int(x1 * PREV_W / page_w) + 2 - bx, int(y1 * PREV_H / page_h) + 2 - by);
    return o;
}

// Taking the picture on Scan seed: the frozen photo dimmed under an accent
// frame, so it reads as taken, not hung; then each word found boxed, grey
// until its sharper strip is cut, accent once it is (a gap stays amber): the
// only motion while the camera holds the screen.
static lv_obj_t *page_boxes[PAGE_MAX_WORDS];

bool ui_page_read() {
    int w, h;
    uint8_t *g = platform_camera_still(&w, &h, &page_room);
    if (!g) { scan_status("No picture yet", "Hold still and try again", AMBER); return false; }
    // The camera stays on for the sharper bands, and this screen stays up with the
    // preview frozen until Words found: about a second, the user holding still.
    if (scan_timer) { lv_timer_delete(scan_timer); scan_timer = nullptr; }
    qrscan_free();   // the QR scanner's buffers: the sharp picture and the reader need the RAM
    qr_w = qr_h = 0;
    page_still = g; page_w = w; page_h = h;
    ncands = 0;
    read_i = 0;
    lv_obj_t *taken = lv_obj_create(lv_obj_get_screen(preview_img));   // Scan seed, even mid fade-in
    lv_obj_remove_style_all(taken);
    lv_obj_remove_flag(taken, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(taken, 0, BAR);
    lv_obj_set_size(taken, PREV_W, PREV_H);
    lv_obj_set_style_bg_color(taken, BG, 0);
    lv_obj_set_style_bg_opa(taken, LV_OPA_40, 0);
    lv_obj_set_style_border_color(taken, ACCENT, 0);
    lv_obj_set_style_border_width(taken, 2, 0);
    scan_status("Hold still", "Taking the picture" "\xE2\x80\xA6", TEXT);
    lv_refr_now(nullptr);   // before the first slow step, or the tap seems to do nothing
    page_step = PAGE_FIND;   // finding the words is the timer's first step, not this tap's:
                             // slow work inside a tap leaves LVGL waiting for a release, and it ate the next tap
    if (page_timer) lv_timer_delete(page_timer);
    page_timer = lv_timer_create(page_tick, 10, nullptr);
    return true;
}

// The words on the page, as candidates to read. False if there are none.
static bool page_find_words() {
    if (page_find(page_still, page_w, page_h, &page)) {
        // Rows the page reader only guessed (a gap in a column) count only if
        // words are missing: a complete page gets no extra "?" rows.
        int found = 0;
        for (int i = 0; i < page.n; i++) found += !page.words[i].guessed;
        // nothing typed yet: 12 or 24 words, from how many are on the page
        // (strays and a few unfound words either way)
        if (!word_n) new_words = found > 16 ? 24 : 12;
        for (int i = 0; i < page.n && ncands < PAGE_MAX_WORDS; i++)
            if (!page.words[i].guessed || word_n + found < new_words) {
                int y0, y1;
                page_strip_rows(&page, i, CAM_ZOOM, PAGE_SHIFT * CAM_ZOOM, &y0, &y1);   // room to look for where it moved
                cands[ncands++] = {-1, false, false, uint8_t(i), int16_t(y0), int16_t(y1)};
            }
    }
    return ncands > 0;
}

// The band: as many rows as the tallest word needs, or more (fewer pictures),
// as far as RAM allows; a word taller than that is cut from what the band has.
static bool page_band_alloc() {
    int zw = page_w * CAM_ZOOM, zh = page_h * CAM_ZOOM, need = 1;
    for (int i = 0; i < ncands; i++) need = cands[i].y1 - cands[i].y0 > need ? cands[i].y1 - cands[i].y0 : need;
    band_rows = 96 * 1024 / zw > need ? 96 * 1024 / zw : need;
    if (band_rows > zh) band_rows = zh;
    for (; band_rows >= 16 && !(band_buf = (uint8_t *)malloc((size_t)band_rows * zw)); band_rows /= 2) {}
    return band_buf != nullptr;
}

// A strip, cut into page_strip_buf, into the store (the still's memory: the
// still itself isn't needed once the words are found).
static void page_store(int piece, int sw) {
    size_t n = (size_t)WR_H * sw;
    if (store_used + n > page_room) { stored[piece] = {0, 0, 0}; return; }
    stored[piece] = {uint32_t(store_used), uint8_t(sw), page_strip_buf[WR_W - 1]};
    for (int y = 0; y < WR_H; y++) memcpy(page_still + store_used + y * sw, page_strip_buf + y * WR_W, sw);
    store_used += n;
}

static void page_restore(int piece) {
    const StoredStrip &st = stored[piece];
    for (int y = 0; y < WR_H; y++) {
        memcpy(page_strip_buf + y * WR_W, page_still + st.off + y * st.sw, st.sw);
        memset(page_strip_buf + y * WR_W + st.sw, st.sw ? st.med : 230, WR_W - st.sw);
    }
}

// One band: from the highest word not cut yet down, every word that fits is
// cut from it. False if the camera gave no picture.
static bool page_band() {
    int start = -1;
    for (int i = 0; i < ncands; i++)
        if (!cands[i].cut && (start < 0 || cands[i].y0 < start)) start = cands[i].y0;
    int zh = page_h * CAM_ZOOM, b0 = start + band_rows > zh ? zh - band_rows : start;
    if (!platform_camera_band(b0, band_rows, band_buf)) return false;
    for (int i = 0; i < ncands; i++) {
        PageCand &c = cands[i];
        if (c.cut || !(c.y0 == start || (c.y0 >= b0 && c.y1 <= b0 + band_rows))) continue;
        // where the word is in this later, sharper frame: matched against the
        // photo as shown (preview_buf, a copy of the still: the still's memory
        // is filling up with strips)
        int dx, dy;
        page_band_shift(preview_buf, PREV_W, PREV_H, band_buf, b0, band_rows, CAM_ZOOM, &page, c.piece, &dx, &dy);
        page_store(c.piece, page_strip_band(band_buf, b0, band_rows, CAM_ZOOM, dx, dy, &page, c.piece, page_strip_buf));
        c.cut = true;
        if (!page.words[c.piece].guessed) lv_obj_set_style_border_color(page_boxes[i], ACCENT, 0);
    }
    return true;
}



// ---- the reading scene: the strip the word reader gets, the fish above
// it, and the words read line up below as they come. No animation: a read
// holds the screen for about a second on the board, so it couldn't run.
constexpr int ST_H = (WR_H * CW + WR_W - 1) / WR_W;      // the strip, scaled to fill a CW-wide pill
constexpr int LIST_ROWS = 3;                             // the last words read, as rows
static int st_y;                                         // st_y: under the fish
static lv_obj_t *page_strip_img, *page_count, *page_of;
static bool page_cut;   // the current word's strip is cut and shown, not read yet

// One word read, added to the list as a row like those on Check words
// (page_row and page_row_draw_cb): number, word (or "?"), and a check or a
// warning. Only the last LIST_ROWS stay: a page can have many stray pieces.
static lv_obj_t *page_box;
static void page_list_add(int n, bool ok, int word) {
    int rows = lv_obj_get_child_count(page_box);
    if (rows) {   // the card's divider, under every row but the last
        lv_obj_t *prev = lv_obj_get_child(page_box, -1);
        lv_obj_set_style_border_side(prev, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(prev, 1, 0);
        lv_obj_set_style_border_color(prev, BORDER, 0);
    }
    if (rows == LIST_ROWS) { lv_obj_delete(lv_obj_get_child(page_box, 0)); rows--; }
    for (int i = 0; i < rows; i++) lv_obj_set_y(lv_obj_get_child(page_box, i), i * 36);
    lv_obj_t *l = text(page_box, ok ? seed_word(word) : "?", &mono_22, ok ? TEXT : AMBER, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(l, CW - 2, 36);
    lv_obj_set_pos(l, 0, rows * 36);
    lv_obj_set_style_pad_left(l, 46, 0);
    lv_obj_set_style_pad_top(l, 5, 0);
    // placed from the row's corner, as page_row_draw_cb draws them (children start inside the padding)
    lv_obj_t *num = text(l, "", &inter_13, TEXT_3, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text_fmt(num, "%d", n);
    lv_obj_set_width(num, 26);
    lv_obj_set_pos(num, 4 - 46, 11 - 5);
    lv_obj_t *mark = text(l, ok ? ICON_CHECK : ICON_WARNING, &inter_13, ok ? GREEN : AMBER, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(mark, 40 - CARD_PX);
    lv_obj_set_pos(mark, CW - 2 - 40 - 46, 11 - 5);
    lv_obj_set_height(page_box, (rows + 1) * 36 + 2);
    lv_obj_remove_flag(page_box, LV_OBJ_FLAG_HIDDEN);
}

static void build_page_scene(lv_obj_t *scr) {
    // where it is: "Word 7 of 24", left of the fish
    lv_obj_align(caption(scr, "READING"), LV_ALIGN_TOP_LEFT, M, BAR + 10);
    page_count = text(scr, "", &rethink_32, TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(page_count, LV_ALIGN_TOP_LEFT, M, BAR + 28);
    page_of = text(scr, "", &inter_13, TEXT_3, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(page_of, LV_ALIGN_TOP_LEFT, M, BAR + 64);

    // The fish sits right of the count, centred in a 79px band so the strip
    // below starts where it always has.
    lv_obj_t *fish = lv_image_create(scr);
    lv_image_set_src(fish, &goldfish_reading);
    lv_obj_set_pos(fish, W - M - goldfish_reading.header.w, BAR + (79 - goldfish_reading.header.h) / 2);
    st_y = BAR + 79 + 8;

    // the strip the word reader gets
    lv_obj_t *frame = lv_obj_create(scr);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, CW, ST_H);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, st_y);
    lv_obj_set_style_radius(frame, 10, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, SURFACE_2, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    page_strip_img = lv_image_create(frame);
    lv_image_set_src(page_strip_img, page_strip_src());
    lv_image_set_pivot(page_strip_img, 0, 0);
    lv_image_set_scale(page_strip_img, (LV_SCALE_NONE * CW + WR_W - 1) / WR_W);
    lv_obj_set_size(page_strip_img, CW, ST_H);
    lv_image_set_inner_align(page_strip_img, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_add_flag(page_strip_img, LV_OBJ_FLAG_HIDDEN);

    // the last words read, in a card like Check words
    page_box = card(scr, CW, 38);
    lv_obj_align(page_box, LV_ALIGN_TOP_MID, 0, st_y + ST_H + 14);
    lv_obj_set_style_pad_all(page_box, 0, 0);
    lv_obj_set_style_clip_corner(page_box, true, 0);
    lv_obj_remove_flag(page_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page_box, LV_OBJ_FLAG_HIDDEN);   // until the first word
}

// Every piece found, in reading order (the camera is only offered before the first word).
static void page_make_slots() {
    nslots = 0;
    page_review_y = 0;
    for (int i = 0; i < ncands && nslots < MAX_SLOTS; i++)
        slots[nslots++] = {int16_t(cands[i].ok ? cands[i].word : -1), int16_t(cands[i].piece), false,
                           int16_t(cands[i].ok ? -1 : cands[i].word)};
    secure_zero(word_idx, sizeof(word_idx));
    secure_zero(cands, sizeof(cands));
    word_n = 0;
}

static void page_fail(const char *msg, const char *sub) {
    page_flow_wipe();
    page_retake = true;
    ui_goto(UI_SCAN_SEED);
    scan_status(msg, sub, AMBER);
}

// Does the camera's frame look like written words? Only how many are found
// counts; the page itself is found again from the still.
static bool page_probe(const uint8_t *frame, int w, int h) {
    bool ok = page_find(frame, w, h, &page);
    int found = 0;
    for (int i = 0; ok && i < page.n; i++) found += !page.words[i].guessed;
    secure_zero(&page, sizeof(page));
    return found >= PAGE_MIN_WORDS;
}

// Out of memory: which step, and the largest block left (found by trying),
// so a report from a board says where RAM ran out.
static void page_oom(const char *what) {
    static char sub[64];
    // the largest block, then everything allocatable, block by block, then all given back
    void *held[24];
    int nh = 0, largest = 0, total = 0;
    for (int kb = 160; kb > 0 && nh < 24;) {
        void *volatile p = malloc((size_t)kb * 1024);   // volatile: the compiler may not drop a malloc it can see freed
        if (p) { held[nh++] = p; total += kb; if (!largest) largest = kb; }
        else kb -= 2;
    }
    for (int i = 0; i < nh; i++) free(held[i]);
    snprintf(sub, sizeof(sub), "Out of memory (%s %d/%d KB)", what, largest, total);
    page_fail(sub, "Restart the device");
}

// One step per tick, so the screen keeps up: find the words, then one band
// at a time (the user holds still until the camera is done), then, after
// Words found, each word in two ticks: show its strip, then read it and add
// it to the list (a word can take a second on the board).
static void page_tick(lv_timer_t *t) {
    if (page_step == PAGE_FIND) {
        // Words found shows the photo as the preview did; kept in preview_buf
        // (idle now), as the still's memory becomes the strips' store.
        for (int y = 0; y < PREV_H; y++)
            for (int x = 0; x < PREV_W; x++)
                preview_buf[y * PREV_W + x] = page_still[(y * page_h / PREV_H) * page_w + x * page_w / PREV_W];
        lv_obj_invalidate(preview_img);   // the still, frozen on Scan seed
        if (page_find_words()) {
            page_step = PAGE_BANDS;   // the sharp picture right away, while the user holds still
            for (int i = 0; i < ncands; i++) {
                page_boxes[i] = word_box(lv_obj_get_screen(preview_img), i);
                if (!page.words[cands[i].piece].guessed) lv_obj_set_style_border_color(page_boxes[i], TEXT_3, 0);
            }
            lv_refr_now(nullptr);   // each band holds the screen: show the boxes first
        } else { platform_camera_stop(); ui_goto(UI_PAGE_FOUND); }   // nothing found: the photo, to see why
        return;
    }
    if (page_step == PAGE_BANDS) {
        if (!band_buf && !page_band_alloc()) { page_oom("Camera"); return; }
        if (!page_band()) {   // Words found says why, over the photo (Scan seed has no room for it)
            const char *err = platform_camera_error();
            page_err = err ? err : "Hold still and try again";
            platform_camera_stop();
            ui_goto(UI_PAGE_FOUND);
            return;
        }
        int cut = 0;
        for (int i = 0; i < ncands; i++) cut += cands[i].cut;
        if (cut < ncands) { lv_refr_now(nullptr); return; }
        platform_camera_stop();   // the still stays: it's the strips' store now
        secure_zero(band_buf, (size_t)band_rows * page_w * CAM_ZOOM);
        free(band_buf);
        band_buf = nullptr;
        page_step = PAGE_READ;   // once the user says so: the camera is done, they can move
        ui_goto(UI_PAGE_FOUND);
        return;
    }
    if (read_i < ncands && !page_cut) {
        page_restore(cands[read_i].piece);
        lv_obj_remove_flag(page_strip_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(page_strip_img, page_strip_src());
        lv_label_set_text_fmt(page_count, "%d", read_i + 1);
        lv_label_set_text_fmt(page_of, "of %d", ncands);   // pieces found: strays and guessed rows too
        page_cut = true;
        lv_timer_set_period(t, LV_DEF_REFR_PERIOD + 5);   // let the strip show before the read holds the screen
        return;
    }
    if (read_i < ncands) {
        WordResult r;
        cands[read_i].ok = word_reader_read(page_strip_buf, nullptr, &r);
        cands[read_i].word = int16_t(r.word);
        page_list_add(read_i + 1, cands[read_i].ok, r.word);
        read_i++;
        page_cut = false;
        lv_timer_set_period(t, LV_MIN(120, 2400 / ncands));   // a moment on the result, ~2.4 s at most in all
        return;
    }
    lv_timer_delete(page_timer);
    page_timer = nullptr;
    secure_zero(page_strip_buf, sizeof(page_strip_buf));
    word_reader_free();
    page_make_slots();
    ui_goto(UI_PAGE_REVIEW);
}

// Back while reading, or Retake: stop, and take another picture.
static void page_reading_back_cb(lv_event_t *) {
    page_flow_wipe();
    page_retake = true;
    ui_goto(UI_SCAN_SEED);
}

bool ui_page_found_read() {
    if (ui_current() != UI_PAGE_FOUND || !ncands || page_err) return false;
    if (!word_reader_init()) { page_oom("Reader"); return false; }
    page_cut = false;
    ui_goto(UI_PAGE_READING);
    return true;
}
static void page_found_read_cb(lv_event_t *) { ui_page_found_read(); }

// The photo as the preview showed it, each word found boxed (amber: a gap
// where a word should be), so a bad picture is retaken before the slow part.
static void build_page_found() {
    lv_obj_t *scr = screen();
    topbar(scr, page_err ? "No picture" : ncands ? "Words found" : "No words found", page_reading_back_cb);
    preview_image(scr);   // filled when the words were found
    for (int i = 0; i < ncands; i++) word_box(scr, i);
    lv_obj_t *msg = text(scr, "", &inter_13, ncands && !page_err ? TEXT_2 : AMBER, LV_TEXT_ALIGN_CENTER);
    if (page_err) lv_label_set_text(msg, page_err);
    else if (ncands) lv_label_set_text_fmt(msg, "%d found: one box per word?", ncands);
    else lv_label_set_text(msg, "Fill the view, words level");
    lv_obj_set_width(msg, CW);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_DOT);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, BAR + PREV_H + 6);
    if (ncands && !page_err) bottom_pair(btn_ghost(scr, "Retake", page_reading_back_cb, nullptr, TEXT),
                            btn_primary(scr, "Read", page_found_read_cb));
    else place_bottom(btn_primary(scr, ICON_CAMERA "   Retake", page_reading_back_cb));
    show(scr);
}

static void build_page_reading() {
    lv_obj_t *scr = screen();
    topbar(scr, "Reading words", page_reading_back_cb);
    build_page_scene(scr);
    show(scr);
    if (page_timer) lv_timer_delete(page_timer);
    page_timer = lv_timer_create(page_tick, 10, nullptr);
}

// ---- the list
static int page_unknown() {
    int n = 0;
    for (int i = 0; i < nslots; i++) n += slots[i].word < 0;
    return n;
}

// Exactly the backup's words, all known, and a valid checksum.
static bool page_checksum_ok() {
    if (nslots != new_words || page_unknown()) return false;
    uint16_t idx[24];
    for (int i = 0; i < nslots; i++) idx[i] = uint16_t(slots[i].word);
    SeedEntropy e;
    bool ok = seed_from_words(idx, nslots, &e);
    seedqr_wipe(&e);
    secure_zero(idx, sizeof(idx));
    return ok;
}

static bool page_set_word(int idx) {
    if (idx < 0 || edit_slot < 0 || (edit_insert && nslots >= MAX_SLOTS)) return false;
    if (edit_insert && nslots < MAX_SLOTS) {
        memmove(&slots[edit_slot + 1], &slots[edit_slot], sizeof(Slot) * (nslots - edit_slot));
        slots[edit_slot] = {int16_t(idx), -1, true};
        nslots++;
    } else if (!edit_insert) {
        slots[edit_slot] = {int16_t(idx), slots[edit_slot].piece, true};
    }
    edit_slot = -1;
    word_prefix[0] = 0;
    page_suggest_slot = -1;
    ui_goto(UI_PAGE_REVIEW);
    return true;
}

int ui_page_count() { return nslots; }
int ui_page_slot(int i) { return i >= 0 && i < nslots ? slots[i].word : -1; }
void ui_page_open(int i) {
    if (i < 0 || i >= nslots) return;
    open_slot = i;
    ui_goto(UI_PAGE_WORD);
}
static void page_keyboard(int slot, bool insert) {
    edit_slot = slot;
    edit_insert = insert;
    word_prefix[0] = 0;
    ui_goto(UI_ENTER_WORDS);
}
void ui_page_type() { if (ui_current() == UI_PAGE_WORD) page_keyboard(open_slot, false); }
void ui_page_insert() { if (ui_current() == UI_PAGE_WORD && nslots < MAX_SLOTS) page_keyboard(open_slot, true); }
void ui_page_add() { if (ui_current() == UI_PAGE_REVIEW && nslots < MAX_SLOTS) page_keyboard(nslots, true); }
void ui_page_remove() {
    if (ui_current() != UI_PAGE_WORD) return;
    memmove(&slots[open_slot], &slots[open_slot + 1], sizeof(Slot) * (nslots - open_slot - 1));
    secure_zero(&slots[--nslots], sizeof(Slot));
    page_suggest_slot = -1;
    ui_goto(UI_PAGE_REVIEW);
}
bool ui_page_confirm() {
    if (ui_current() != UI_PAGE_REVIEW || !page_checksum_ok()) return false;
    for (int i = 0; i < nslots; i++) word_idx[i] = uint16_t(slots[i].word);
    word_n = nslots;
    page_flow_wipe();
    return load_words();
}

static void page_row_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!page_picking_extra) { ui_page_open(i); return; }
    page_picking_extra = false;
    memmove(&slots[i], &slots[i + 1], sizeof(Slot) * (nslots - i - 1));
    secure_zero(&slots[--nslots], sizeof(Slot));
    page_suggest_slot = -1;
    ui_goto(UI_PAGE_REVIEW);
}
static void page_pick_extra_cb(lv_event_t *) { page_picking_extra = true; ui_goto(UI_PAGE_REVIEW); }
static void page_cancel_pick_cb(lv_event_t *) { page_picking_extra = false; ui_goto(UI_PAGE_REVIEW); }
static void page_add_cb(lv_event_t *) { ui_page_add(); }
static void page_confirm_cb(lv_event_t *) { ui_page_confirm(); }
static void page_back_cb(lv_event_t *) {
    if (page_picking_extra) { page_picking_extra = false; ui_goto(UI_PAGE_REVIEW); return; }   // cancel picking
    page_flow_wipe();
    ui_goto(UI_ENTER_WORDS);
}
static void page_list_cb(lv_event_t *) { ui_goto(UI_PAGE_REVIEW); }
static void page_type_cb(lv_event_t *) { ui_page_type(); }
static void page_insert_cb(lv_event_t *) { ui_page_insert(); }
static void page_remove_cb(lv_event_t *) { ui_page_remove(); }

// One row of the list: a single clickable label ("12  vendor"). One object
// per row, not a button with three labels: a 24-word list plus the screen
// fading out plus a pressed button's layer ran LVGL's 96 KB heap out, and its
// allocator hung.
// A row's number (small, grey) and status (read from the photo, typed, or
// unread), drawn onto the row label instead of being objects of their own:
// one object per row keeps a 24-word list well inside LVGL's heap.
static void page_row_draw_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= nslots) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target_obj(e), &a);
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &inter_13;
    d.color = TEXT_3;
    d.align = LV_TEXT_ALIGN_RIGHT;
    d.text = num;
    d.text_local = 1;   // copied: drawing may happen after this returns
    lv_area_t na = {a.x1 + 4, a.y1 + 11, a.x1 + 30, a.y2};
    lv_draw_label(layer, &d, &na);
    const Slot &sl = slots[i];
    d.align = LV_TEXT_ALIGN_RIGHT;
    d.text = sl.word < 0 ? ICON_WARNING : sl.typed || sl.piece < 0 ? ICON_KEYBOARD : ICON_CHECK;
    d.color = sl.word < 0 ? AMBER : sl.typed || sl.piece < 0 ? TEXT_3 : GREEN;
    lv_area_t ia = {a.x2 - 40, a.y1 + 11, a.x2 - CARD_PX, a.y2};
    lv_draw_label(layer, &d, &ia);
}

static lv_obj_t *page_row(lv_obj_t *parent, int y, const char *word, lv_color_t c, bool rule, lv_event_cb_t cb,
                          void *ud, bool marks) {
    lv_obj_t *l = text(parent, word, &mono_22, c, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(l, CW - 2, 36);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_style_pad_left(l, 46, 0);
    lv_obj_set_style_pad_top(l, 5, 0);
    lv_obj_set_style_bg_color(l, SURFACE_2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_STATE_PRESSED);
    if (rule) {   // the card's divider, as a border on the row itself: no extra object
        lv_obj_set_style_border_side(l, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(l, 1, 0);
        lv_obj_set_style_border_color(l, BORDER, 0);
    }
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, ud);
    if (marks) lv_obj_add_event_cb(l, page_row_draw_cb, LV_EVENT_DRAW_MAIN_END, ud);
    return l;
}

static int page_first_unknown() {
    for (int i = 0; i < nslots; i++) if (slots[i].word < 0) return i;
    return -1;
}
static void page_fix_cb(lv_event_t *) { ui_page_open(page_first_unknown()); }

static void build_page_review() {
    lv_obj_t *scr = screen();
    topbar(scr, "Check words", page_back_cb);
    int unknown = page_unknown(), first = page_first_unknown();
    bool ready = page_checksum_ok();

    // The status says what's wrong; the button below says (and does) the next
    // step, so it's clear without scrolling to the rows that need fixing.
    char status[64], action[40];
    lv_color_t sc = AMBER;
    lv_event_cb_t act = nullptr;
    if (unknown) {
        snprintf(status, sizeof(status), unknown == 1 ? "1 word couldn't be read" : "%d words couldn't be read", unknown);
        snprintf(action, sizeof(action), "Fix word %d", first + 1);
        act = page_fix_cb;
    } else if (nslots < new_words) {
        snprintf(status, sizeof(status), "%d of %d words found", nslots, new_words);
        snprintf(action, sizeof(action), ICON_KEYBOARD "   Add word %d", nslots + 1);
        act = page_add_cb;
    } else if (nslots > new_words && page_picking_extra) {
        snprintf(status, sizeof(status), "Tap the word to remove");
        snprintf(action, sizeof(action), "Cancel");
        act = page_cancel_pick_cb;
        sc = TEXT_2;
    } else if (nslots > new_words) {
        snprintf(status, sizeof(status), "%d words, your backup has %d", nslots, new_words);
        snprintf(action, sizeof(action), ICON_TRASH "   Remove extra word");
        act = page_pick_extra_cb;
    } else if (!ready) {
        snprintf(status, sizeof(status), "A word may be misread");
        snprintf(action, sizeof(action), "Invalid checksum");
    } else {
        // all in and the checksum fits: say so, and whether the photo read them all
        bool typed = false;
        for (int i = 0; i < nslots; i++) typed |= slots[i].typed || slots[i].piece < 0;
        if (typed) snprintf(status, sizeof(status), ICON_CHECK "  %d words \xC2\xB7 checksum valid", nslots);
        else snprintf(status, sizeof(status), ICON_CHECK "  All %d read \xC2\xB7 checksum valid", nslots);
        snprintf(action, sizeof(action), "Matches my backup");
        act = page_confirm_cb;
        sc = GREEN;
    }
    lv_obj_align(text(scr, status, &inter_13, sc), LV_ALIGN_TOP_MID, 0, BAR + 2);

    // the words in a card, like the menus: rows with a rule between them
    lv_obj_t *list = page_review_list = scroll_area(scr, BAR + 24);
    int rows = nslots + (nslots < new_words);
    lv_obj_t *box = card(list, CW, rows * 36 + 2);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    int y = 0;
    for (int i = 0; i < nslots; i++, y += 36) {
        bool known = slots[i].word >= 0;
        page_row(box, y, known ? seed_word(slots[i].word) : "?", known ? TEXT : AMBER, i + 1 < rows, page_row_cb,
                 (void *)(intptr_t)i, true);
    }
    if (nslots < new_words) page_row(box, y, "+ add a word", TEXT_3, false, page_add_cb, nullptr, false);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, page_review_y, LV_ANIM_OFF);   // bounded: a shorter list stops at its end

    lv_obj_t *btn = page_picking_extra ? btn_ghost(scr, action, act, nullptr, TEXT) : btn_primary(scr, action, act ? act : page_list_cb);
    place_bottom(btn);
    if (!act) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_DISABLED);
    }
    show(scr);
}

// ---- one word: its picture, and what to do with it
static uint8_t page_last_ok[256];   // bitmap: last words that fit the other words' checksum

static bool page_last_allowed(int i) { return (page_last_ok[i >> 3] >> (i & 7)) & 1; }

// The last word, when it's unread or doesn't fit the checksum but every other
// word is known: its picture read again among only the words that fit (8 for
// 24 words, 128 for 12). Offered, never put in by itself.
static int page_last_suggestion(const Slot &sl) {
    if (open_slot != new_words - 1 || nslots != new_words || sl.piece < 0) return -1;
    for (int i = 0; i < nslots - 1; i++) if (slots[i].word < 0) return -1;
    uint16_t idx[24], last[128];
    for (int i = 0; i < nslots; i++) idx[i] = uint16_t(slots[i].word < 0 ? 0 : slots[i].word);
    int n = seed_valid_last_words(idx, new_words, last);
    memset(page_last_ok, 0, sizeof(page_last_ok));
    for (int i = 0; i < n; i++) page_last_ok[last[i] >> 3] |= 1 << (last[i] & 7);
    secure_zero(idx, sizeof(idx));
    secure_zero(last, sizeof(last));
    if (sl.word >= 0 && page_last_allowed(sl.word)) return -1;   // it fits already
    WordResult r;
    int w = -1;
    if (word_reader_init()) {
        word_reader_read(page_strip_buf, page_last_allowed, &r);
        w = r.word;
        word_reader_free();
    }
    secure_zero(page_last_ok, sizeof(page_last_ok));
    return w;
}

// The re-read takes a moment on the board: done by a timer once the word's
// screen shows, never inside the tap that opened it.
static void page_suggest_tick(lv_timer_t *) {
    lv_timer_delete(suggest_timer);
    suggest_timer = nullptr;
    if (ui_current() != UI_PAGE_WORD) return;
    page_suggest = page_last_suggestion(slots[open_slot]);
    page_suggest_slot = open_slot;
    if (page_suggest >= 0) ui_goto(UI_PAGE_WORD);
}

int ui_page_suggestion() { return ui_current() == UI_PAGE_WORD ? page_suggest : -1; }
void ui_page_use_suggestion() {
    if (ui_current() != UI_PAGE_WORD || page_suggest < 0) return;
    slots[open_slot].word = int16_t(page_suggest);
    slots[open_slot].typed = true;
    ui_goto(UI_PAGE_REVIEW);
}
static void page_suggest_cb(lv_event_t *) { ui_page_use_suggestion(); }

// An unread word's best guess, offered on its screen (the checksum's suggestion comes first).
int ui_page_guess() {
    if (ui_current() != UI_PAGE_WORD || page_suggest >= 0) return -1;
    const Slot &sl = slots[open_slot];
    return sl.word < 0 && sl.piece >= 0 ? sl.guess : -1;
}
void ui_page_use_guess() {
    int g = ui_page_guess();
    if (g < 0) return;
    slots[open_slot].word = int16_t(g);
    slots[open_slot].typed = true;   // the user's pick, like a typed word
    ui_goto(UI_PAGE_REVIEW);
}
static void page_guess_cb(lv_event_t *) { ui_page_use_guess(); }

static void build_page_word() {
    const Slot &sl = slots[open_slot];
    lv_obj_t *scr = screen();
    char title[24];
    snprintf(title, sizeof(title), "Word %d of %d", open_slot + 1, new_words);
    topbar(scr, title, page_list_cb);

    int y = BAR + 8;
    if (sl.piece >= 0) {   // the word's own picture, as cut from the photo
        page_restore(sl.piece);
        lv_obj_t *frame = card(scr, CW, WR_H + 16);
        lv_obj_set_style_pad_all(frame, 8, 0);
        lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_t *img = lv_image_create(frame);
        lv_image_set_src(img, page_strip_src());
        lv_obj_center(img);
        y += WR_H + 28;
    } else {
        lv_obj_align(text(scr, "Typed", &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, y + 12);
        y += 40;
    }
    if (sl.word >= 0) lv_obj_align(text(scr, seed_word(sl.word), &mono_22, TEXT), LV_ALIGN_TOP_MID, 0, y + 8);
    else lv_obj_align(text(scr, "Couldn't read it", &inter_17_semi, AMBER), LV_ALIGN_TOP_MID, 0, y + 10);
    if (page_suggest_slot != open_slot) {
        page_suggest = -1;
        if (open_slot == new_words - 1 && !suggest_timer) suggest_timer = lv_timer_create(page_suggest_tick, 50, nullptr);
    }
    int guess = ui_page_guess();
    if (page_suggest >= 0 || guess >= 0) {
        lv_obj_align(text(scr, page_suggest >= 0 ? "This one fits the checksum:" : "Best guess, if it matches:",
                          &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, y + 44);
        lv_obj_t *b = page_suggest >= 0 ? btn_ghost(scr, seed_word(page_suggest), page_suggest_cb, nullptr, ACCENT)
                                        : btn_ghost(scr, seed_word(guess), page_guess_cb, nullptr, ACCENT);
        lv_obj_set_style_bg_color(b, ACCENT_DIM, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y + 64);
    }

    lv_obj_align(quiet_link(scr, "Add a word before this", page_insert_cb, nullptr), LV_ALIGN_BOTTOM_MID, 0, -M - 52);
    bottom_pair(btn_ghost(scr, ICON_TRASH, page_remove_cb, nullptr, TEXT),
                btn_primary(scr, sl.word >= 0 ? ICON_KEYBOARD "   Change" : ICON_KEYBOARD "   Type it", page_type_cb));
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

// Cancel/Discard: wipe the seed and go back to where the flow started.
static void backup_cancel_cb(lv_event_t *) {
    UiScreen to = backup_cancel;
    new_seed_wipe();
    ui_goto(to);
}

static void build_backup_warning() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_WARNING, AMBER, AMBER_DIM, 64), LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_align(text(scr, "Keep it offline", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_align(para(scr, backup_existing ? "Your seed QR is your private key. Never photograph it or scan it with a phone."
                                           : "Your words are your private key. Never photograph them or type them into a phone."),
                 LV_ALIGN_TOP_MID, 0, 154);
    bottom_pair(btn_ghost(scr, backup_existing ? "Cancel" : "Discard",
                          backup_cancel_cb, nullptr, TEXT),
                btn_primary(scr, "I understand", go_cb, GO(backup_existing ? UI_BACKUP_QR : UI_BACKUP_WORDS)));
    show(scr);
}

// Blocks of 5 modules; a remainder joins the last block instead of making a
// sliver (21 = 5+5+5+6). Lines on screen and on the card sit on these edges.
static int blocks_per_side() { return qr_n / 5; }
static bool block_edge(int m) { return m == qr_n || (m % 5 == 0 && m / 5 < blocks_per_side()); }
static int block_len(int b) { return b + 1 < blocks_per_side() ? 5 : qr_n - 5 * b; }

// Modules a SeedQR card template already has printed, the same in every QR of
// its size: the three finder squares with their light border, the timing
// lines (row 7, column G), the dark module beside the lower finder (I14 on
// 21x21), and on 25x25 the alignment square.
static bool preprinted(int x, int y) {
    int far = qr_n - 8;
    if ((x <= 7 || x >= far) && (y <= 7 || y >= far) && !(x >= far && y >= far)) return true;
    if ((y == 6 && x > 7 && x < far) || (x == 6 && y > 7 && y < far)) return true;
    if (x == 8 && y == far) return true;
    return qr_n == 25 && x >= 16 && x <= 20 && y >= 16 && y <= 20;
}

// Part of the SeedQR, drawn module by module: dark squares, light grid lines
// (darker every 5 modules, like the lines on the paper card). With `ring`,
// the view is the active part plus one module around it: neighbours greyed,
// pre-printed modules hatched, the part itself framed.
struct GridView { int c0, r0, cols, rows, cell; bool ring; int ac0, ar0, acols, arows; };
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
    const int o = g->ring ? 1 : 0;   // room for the part's frame at the edge
    auto rect = [&](int x1, int y1, int x2, int y2, lv_color_t c) {
        lv_area_t r = {a.x1 + o + x1, a.y1 + o + y1, a.x1 + o + x2, a.y1 + o + y2};
        d.bg_color = c;
        lv_draw_rect(layer, &d, &r);
    };
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.width = 1;
    auto line = [&](int x1, int y1, int x2, int y2, lv_color_t c) {
        ld.color = c;
        ld.p1.x = a.x1 + o + x1; ld.p1.y = a.y1 + o + y1;
        ld.p2.x = a.x1 + o + x2; ld.p2.y = a.y1 + o + y2;
        lv_draw_line(layer, &ld);
    };
    const lv_color_t ghost = LV_COLOR_MAKE(0xC4, 0xC0, 0xB8);
    const lv_color_t major = LV_COLOR_MAKE(0x9A, 0x96, 0x8E), minor = LV_COLOR_MAKE(0xD4, 0xD0, 0xC8);
    const int s = g->cell;
    for (int r = 0; r < g->rows; r++)
        for (int c = 0; c < g->cols; c++) {
            int x = g->c0 + c, y = g->r0 + r, x1 = c * s, y1 = r * s;
            if (!qrcodegen_getModule(qr_buf, x, y)) continue;
            bool active = x >= g->ac0 && x < g->ac0 + g->acols && y >= g->ar0 && y < g->ar0 + g->arows;
            if (g->ring && preprinted(x, y)) {
                // Already on the card: diagonal hatch, not a square to fill.
                for (int k = 4; k < 2 * s; k += 6)
                    line(x1 + LV_MIN(k, s), y1 + LV_MAX(0, k - s), x1 + LV_MAX(0, k - s), y1 + LV_MIN(k, s), major);
            } else {
                rect(x1, y1, x1 + s, y1 + s, active ? lv_color_black() : ghost);
            }
        }
    if (!g->ring) {
        for (int i = 0; i <= g->cols; i++)
            if (block_edge(g->c0 + i)) rect(i * s, 0, i * s, g->rows * s, major);
        for (int i = 0; i <= g->rows; i++)
            if (block_edge(g->r0 + i)) rect(0, i * s, g->cols * s, i * s, major);
        return;
    }
    const lv_color_t faint = LV_COLOR_MAKE(0xE6, 0xE2, 0xDA);
    for (int i = 0; i <= g->cols; i++) rect(i * s, 0, i * s, g->rows * s, faint);
    for (int i = 0; i <= g->rows; i++) rect(0, i * s, g->cols * s, i * s, faint);
    int ax = (g->ac0 - g->c0) * s, ay = (g->ar0 - g->r0) * s, aw = g->acols * s, ah = g->arows * s;
    for (int i = 1; i < g->acols; i++) rect(ax + i * s, ay, ax + i * s, ay + ah, minor);
    for (int i = 1; i < g->arows; i++) rect(ax, ay + i * s, ax + aw, ay + i * s, minor);
    const lv_color_t frame = LV_COLOR_MAKE(0x3A, 0x37, 0x32);
    rect(ax - 1, ay - 1, ax + aw + 1, ay, frame);
    rect(ax - 1, ay + ah, ax + aw + 1, ay + ah + 1, frame);
    rect(ax - 1, ay - 1, ax, ay + ah + 1, frame);
    rect(ax + aw, ay - 1, ax + aw + 1, ay + ah + 1, frame);
}

static lv_obj_t *grid(lv_obj_t *parent, GridView *g) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, g->cols * g->cell + 1 + (g->ring ? 2 : 0), g->rows * g->cell + 1 + (g->ring ? 2 : 0));
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
    lv_obj_t *scr = screen();
    if (!backup_make_qr()) {
        lv_obj_align(icon_badge(scr, ICON_WARNING, RED, RED_DIM, 64), LV_ALIGN_TOP_MID, 0, 44);
        lv_obj_align(text(scr, "No QR code", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, 126);
        lv_obj_align(para(scr, backup_existing ? "The QR code couldn't be made. Nothing was backed up."
                                               : "The QR code couldn't be made. Your written words are the backup."),
                     LV_ALIGN_TOP_MID, 0, 162);
        place_bottom(btn_primary(scr, "Continue", backup_exit_cb));
        show(scr);
        return;
    }
    topbar(scr, "Back up", to_warning_cb);
    const int cell = 180 / qr_n;
    whole_view = {0, 0, qr_n, qr_n, cell, false, 0, 0, qr_n, qr_n};
    lv_obj_t *c = white_card(scr, qr_n * cell + 17, qr_n * cell + 17);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 2);
    lv_obj_center(grid(c, &whole_view));
    place_bottom(btn_primary(scr, "Start copying", block_start_cb));
    show(scr);
}

// Columns A..Y: a Compact SeedQR is at most 25 modules wide.
static void col_label(int col, char out[4]) { snprintf(out, 4, "%c", 'A' + col); }

const uint8_t *ui_backup_qr() { return qr_n ? qr_buf : nullptr; }

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

// One 5x5 block at a time, labelled with the card's column letters and row
// numbers, inside one module of its neighbours. Every part of the card's grid
// is shown, also the corners that are all pre-printed (all hatched).
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

    // The part plus one module around it (at most 7x7, also for the 6-wide
    // last part, which has no neighbour past the edge). One cell size for
    // every part, so squares don't change size between parts.
    const int cell = 25;
    int ac0 = (block_i % bs) * 5, ar0 = (block_i / bs) * 5;
    int acols = block_len(block_i % bs), arows = block_len(block_i / bs);
    int c0 = LV_MAX(ac0 - 1, 0), r0 = LV_MAX(ar0 - 1, 0);
    int c1 = LV_MIN(ac0 + acols, qr_n - 1), r1 = LV_MIN(ar0 + arows, qr_n - 1);
    block_view = {c0, r0, c1 - c0 + 1, r1 - r0 + 1, cell, true, ac0, ar0, acols, arows};
    const int gx = 22, gy = 18;
    lv_obj_t *c = white_card(scr, CW, gy + 7 * cell + 4);   // clears the buttons
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BAR + 2);
    lv_obj_set_pos(grid(c, &block_view), gx - 1, gy - 1);
    const lv_color_t lab = LV_COLOR_MAKE(0x6E, 0x6A, 0x62), lab_dim = LV_COLOR_MAKE(0xB4, 0xB0, 0xA8);
    for (int i = 0; i < block_view.cols; i++) {
        char l[4];
        int col = c0 + i;
        col_label(col, l);
        lv_obj_t *t = text(c, l, &inter_13, col >= ac0 && col < ac0 + acols ? lab : lab_dim, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(t, cell);
        lv_obj_set_pos(t, gx + i * cell, 1);
    }
    for (int i = 0; i < block_view.rows; i++) {
        char l[12];
        int row = r0 + i;
        snprintf(l, sizeof(l), "%d", row + 1);
        lv_obj_t *t = text(c, l, &inter_13, row >= ar0 && row < ar0 + arows ? lab : lab_dim, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(t, 18);
        lv_obj_set_pos(t, 0, gy + i * cell + cell / 2 - 8);
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
// All the words on one list. "Written down" only works once the list has been
// scrolled to its end, so no word is skipped.
static lv_obj_t *words_done_btn;

static void words_done_cb(lv_event_t *) { ui_goto(UI_BACKUP_CHOICE); }
static void backup_words_back_cb(lv_event_t *) { ui_goto(UI_BACKUP_WARNING); }
static void to_words_cb(lv_event_t *) { ui_goto(UI_BACKUP_WORDS); }

static void words_scroll_cb(lv_event_t *e) {
    if (lv_obj_get_scroll_bottom((lv_obj_t *)lv_event_get_target(e)) <= 2)
        lv_obj_remove_state(words_done_btn, LV_STATE_DISABLED);
}

static void build_backup_words() {
    lv_obj_t *scr = screen();
    char title[32];
    snprintf(title, sizeof(title), "Your %d words", new_words);
    topbar(scr, title, backup_words_back_cb);
    uint16_t idx[24];
    seed_word_indexes(pending, idx);
    lv_obj_t *list = scroll_area(scr, BAR + 4);
    lv_obj_t *c = card(list, CW, new_words * 44 + 16);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    for (int w = 0; w < new_words; w++) {
        char num[12];
        snprintf(num, sizeof(num), "%d", w + 1);
        lv_obj_t *n = text(c, num, &inter_15, TEXT_3, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(n, 26);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, w * 44 + 8);
        bool last = w == new_words - 1;
        snprintf(txt_words[w], sizeof(txt_words[w]), "%s", seed_word(idx[w]));
        lv_obj_t *t = text(c, "", &mono_22, last && drawing ? ACCENT : TEXT, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text_static(t, txt_words[w]);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 40, w * 44 + 2);
        if (w) divider(c, w * 44 - 6);
    }
    secure_zero(idx, sizeof(idx));
    words_done_btn = btn_primary(scr, "Written down", words_done_cb);
    place_bottom(words_done_btn);
    lv_obj_add_state(words_done_btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(words_done_btn, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_add_event_cb(list, words_scroll_cb, LV_EVENT_SCROLL, nullptr);
    lv_obj_update_layout(list);
    lv_obj_send_event(list, LV_EVENT_SCROLL, nullptr);   // a list that fits needs no scrolling
    show(scr);
}

static void build_backup_choice() {
    lv_obj_t *scr = screen();
    topbar(scr, "QR code", to_words_cb);
    lv_obj_align(icon_badge(scr, ICON_QR, ACCENT, ACCENT_DIM, 64), LV_ALIGN_TOP_MID, 0, BAR + 8);
    lv_obj_align(text(scr, "Back up as QR?", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 86);
    lv_obj_align(para(scr, "Copy it square by square, then load the seed by scanning instead of typing.", &inter_13,
                      TEXT_2),
                 LV_ALIGN_TOP_MID, 0, BAR + 122);
    bottom_pair(btn_ghost(scr, "Skip", go_cb, GO(UI_SEED_READY), TEXT), btn_primary(scr, "Make card", go_cb, GO(UI_BACKUP_QR)));
    show(scr);
}

// ---------------------------------------------------------------- seed ready
// The end of New seed. The seed isn't loaded: Done wipes it from the device.
// The fingerprint takes seconds on the board (PBKDF2), so the screen shows
// first and a one-shot timer fills it in.
static lv_timer_t *ready_timer;
static void ready_tick(lv_timer_t *t) {
    lv_obj_t *l = (lv_obj_t *)lv_timer_get_user_data(t);
    ready_timer = nullptr;
    char fp[9], spaced[12];
    seed_fingerprint_hex(pending, fp);
    snprintf(spaced, sizeof(spaced), "%.4s %.4s", fp, fp + 4);
    lv_label_set_text(l, spaced);
}

static void build_seed_ready() {
    lv_obj_t *scr = screen();
    lv_obj_align(icon_badge(scr, ICON_CHECK, GREEN, GREEN_DIM, 52), LV_ALIGN_TOP_MID, 0, 28);
    char title[32];
    if (child_seed) snprintf(title, sizeof(title), "Child #%s %s", child_num, card_checked ? "checked" : "ready");
    else snprintf(title, sizeof(title), card_checked ? "Card checked" : "Seed ready");
    lv_obj_align(text(scr, title, &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_t *fp;
    lv_obj_align(fingerprint_pill(scr, "\xE2\x80\xA6", &fp), LV_ALIGN_TOP_MID, 0, 136);
    char note[64];
    if (child_seed) snprintf(note, sizeof(note), "Write it and #%s on your backup. Load the child like any seed.", child_num);
    else snprintf(note, sizeof(note), "Write it on your backup.");
    lv_obj_align(para(scr, note, &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, 180);
    place_bottom(btn_primary(scr, "Done", to_home_cb));
    show(scr);
    ready_timer = lv_timer_create(ready_tick, 200, fp);   // after the fade-in
    lv_timer_set_repeat_count(ready_timer, 1);
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
        ui_goto(UI_WALLET);
        return;
    }
    if (same) {
        scan_stop();
        card_checked = true;
        ui_goto(UI_SEED_READY);
        return;
    }
    if (r == SEEDQR_OK) scan_status("Wrong seed", "This card has another seed", RED);
    else if (r == SEEDQR_BAD_CHECKSUM || r == SEEDQR_BAD_WORD_INDEX) scan_status("Damaged QR", "Check the squares again", RED);
    else scan_status("No seed found", "Some squares look off", AMBER);
}

// ---------------------------------------------------------------- BIP-85 child seed
// A child of the loaded seed (bip85.h), handed out like a new seed: the same
// warning, words, optional QR card and Seed ready. Same seed and number, same child.
// Always 12 words, like New seed.
static void child_start_cb(lv_event_t *) {
    strcpy(child_num, "0");
    child_failed = false;
    ui_goto(UI_CHILD_INDEX);
}

static lv_obj_t *child_num_lbl;

void ui_child_digit(int k) {
    size_t n = strlen(child_num);
    if (k < 0) {                                     // Del
        if (n > 1) child_num[n - 1] = 0;
        else strcpy(child_num, "0");
    } else {
        char next[12];
        snprintf(next, sizeof(next), "%s%c", strcmp(child_num, "0") ? child_num : "", char('0' + k));
        // Hardened index: a digit that would pass 2147483647 is ignored.
        if (strlen(next) <= 10 && strtoull(next, nullptr, 10) <= BIP85_MAX_INDEX) strcpy(child_num, next);
    }
    if (ui_current() != UI_CHILD_INDEX) return;
    if (child_failed) { child_failed = false; ui_goto(UI_CHILD_INDEX); }   // drop the failure note
    else lv_label_set_text(child_num_lbl, child_num);
}
const char *ui_child_number() { return child_num; }
static void digit_cb(lv_event_t *e) { ui_child_digit((int)(intptr_t)lv_event_get_user_data(e)); }

static void digit_key(lv_obj_t *scr, int x, int y, int w, int h, const char *s, int k) {
    lv_obj_center(text(key_button(scr, x, y, w, h, digit_cb, k), s, k < 0 ? &inter_15 : &mono_22, TEXT));
}

static void child_continue_cb(lv_event_t *) { ui_child_seed((uint32_t)strtoul(child_num, nullptr, 10)); }

static void build_child_index() {
    lv_obj_t *scr = screen();
    topbar(scr, "Child number", to_menu_cb);
    // The number and what it does (or why it failed), then a phone-style keypad
    // whose last key is OK, so no separate button has to squeeze in below.
    child_num_lbl = text(scr, child_num, &mono_26, TEXT);
    lv_obj_align(child_num_lbl, LV_ALIGN_TOP_MID, 0, BAR + 4);
    lv_obj_align(text(scr, child_failed ? "Couldn't make it. Try again." : "Each number, another seed",
                      &inter_13, child_failed ? RED : TEXT_3),
                 LV_ALIGN_TOP_MID, 0, BAR + 40);
    const int GAP = 8, KW = (CW - 2 * GAP) / 3, KH = 40, Y0 = H - M - 4 * KH - 3 * GAP;
    for (int i = 0; i < 9; i++) {
        char d[2] = {char('1' + i), 0};
        digit_key(scr, M + (i % 3) * (KW + GAP), Y0 + (i / 3) * (KH + GAP), KW, KH, d, i + 1);
    }
    const int Y3 = Y0 + 3 * (KH + GAP);
    digit_key(scr, M, Y3, KW, KH, "Del", -1);
    digit_key(scr, M + KW + GAP, Y3, KW, KH, "0", 0);
    lv_obj_t *ok = key_button(scr, M + 2 * (KW + GAP), Y3, KW, KH, child_continue_cb, 0);
    lv_obj_set_style_bg_color(ok, TEXT, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_style_bg_color(ok, TEXT_2, LV_STATE_PRESSED);
    lv_obj_center(text(ok, "OK", &inter_17_semi, BG));
    show(scr);
}

void ui_child_seed(uint32_t index) {
    new_seed_wipe();
    new_words = 12;
    if (!wallet_has_seed() || !bip85_bip39_entropy(*wallet_root(), new_words, index, &pending)) {
        seedqr_wipe(&pending);
        child_failed = true;
        ui_goto(UI_CHILD_INDEX);
        return;
    }
    snprintf(child_num, sizeof(child_num), "%u", unsigned(index));
    child_seed = true;
    backup_cancel = UI_CHILD_INDEX;
    ui_goto(UI_BACKUP_WARNING);
}

// ---------------------------------------------------------------- transaction
static void discard_psbt_cb(lv_event_t *) {
    psbt_clear();
    ui_goto(UI_WALLET);
}

static void scan_descriptor_cb(lv_event_t *) {
    psbt_clear();
    ui_goto(UI_SCAN);
}

static void build_psbt_error() {
    lv_obj_t *scr = screen();
    bool sus = review.error == PSBT_SUSPICIOUS, desc = review.error == PSBT_NEEDS_DESCRIPTOR;
    lv_obj_align(icon_badge(scr, ICON_WARNING, sus ? RED : AMBER, sus ? RED_DIM : AMBER_DIM, 64), LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_align(text(scr, sus ? "Don't sign this" : desc ? "Scan the wallet" : "Can't sign this", &rethink_24, TEXT),
                 LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_align(para(scr, psbt_error_text(review.error)), LV_ALIGN_TOP_MID, 0, 162);
    // The Sign scanner takes the descriptor too.
    if (desc) bottom_pair(btn_ghost(scr, "Back", discard_psbt_cb, nullptr, TEXT), btn_primary(scr, "Scan it", scan_descriptor_cb));
    else place_bottom(btn_primary(scr, "Back to wallet", discard_psbt_cb));
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
    if (review.ms_m) {   // from the multisig wallet: "2 OF 3"
        char ms[16];
        snprintf(ms, sizeof(ms), "%d OF %d", review.ms_m, review.ms_n);
        topbar(scr, "Review", discard_psbt_cb, ms, ACCENT, ACCENT_DIM);
    } else {
        topbar(scr, "Review", discard_psbt_cb);
    }

    bool self_only = review.spend == 0;
    uint64_t headline = self_only ? review.self : review.spend;
    char cap[48];
    snprintf(cap, sizeof(cap), "%s%s", self_only ? "TO YOURSELF" : "YOU'RE SENDING", review.testnet ? "  \xC2\xB7  TESTNET" : "");
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
// chunk by chunk against the wallet. Four groups per line. Returns the height.
static int address_chunks(lv_obj_t *parent, const char *a) {
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
    lv_obj_update_layout(box);
    return lv_obj_get_height(box);
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

    lv_obj_t *c = card(scroll_area(scr, BAR + 88), CW, 0);
    lv_obj_align(caption(c, "ADDRESS"), LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_height(c, 20 + address_chunks(c, o.address) + 2 * CARD_PY);

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
    int top = BAR + 8;
    if (review.testnet) {
        lv_obj_align(chip(scr, "TESTNET", AMBER, AMBER_DIM), LV_ALIGN_TOP_MID, 0, BAR);
        top = BAR + 34;
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
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, top);

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
    lv_obj_set_style_text_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, swipe_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(sl, swipe_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -(M + 26));
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
static size_t anim_frag;
#define FRAGMENT_LEN 30   // SeedSigner's size: a 33-module QR, 5 px on the card, 6 px full screen

// Tap the QR: it fills the screen's width on black (33 modules at 6 px in a 3-module
// light border); tap again to go back.
constexpr int QR_FULL = 204;
static lv_obj_t *qr_full, *qr_full_canvas;

static void qr_full_close_cb(lv_event_t *) {
    lv_obj_delete(qr_full);
}

static void qr_full_deleted_cb(lv_event_t *) {
    qr_full = qr_full_canvas = nullptr;
}

static void qr_full_open_cb(lv_event_t *) {
    if (qr_full) return;
    qr_full = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(qr_full);
    lv_obj_set_size(qr_full, W, H);
    lv_obj_set_pos(qr_full, 0, 0);
    lv_obj_set_style_bg_color(qr_full, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(qr_full, LV_OPA_COVER, 0);
    lv_obj_add_flag(qr_full, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(qr_full, qr_full_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(qr_full, qr_full_deleted_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_t *sq = lv_obj_create(qr_full);   // the light quiet zone around it
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, W, W);
    lv_obj_set_style_bg_color(sq, QR_LIGHT, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_center(sq);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE);
    static uint8_t buf_data[LV_DRAW_BUF_SIZE(QR_FULL, QR_FULL, LV_COLOR_FORMAT_I1)];
    static lv_draw_buf_t buf;
    qr_full_canvas = qr_canvas(sq, &buf, buf_data, sizeof(buf_data), QR_FULL);
    lv_obj_remove_flag(qr_full_canvas, LV_OBJ_FLAG_CLICKABLE);
}

void ui_qr_full_screen() {
    if (qr_full) lv_obj_delete(qr_full);
    else qr_full_open_cb(nullptr);
}

static void anim_tick(lv_timer_t *) {
    static char part[400];
    anim_seq++;
    size_t n = ur_encode_part(signed_buf, signed_len, anim_frag, anim_seq, part, sizeof(part));
    if (n) qr_set(anim_qr, part);
    if (n && qr_full_canvas) qr_set(qr_full_canvas, part);
    // Frames last 250 and 350 ms in turn, 300 on average: a wallet that samples
    // at a fixed period (Sparrow: every 200 ms) can't lock in step with them
    // and keep catching the change from one frame to the next.
    if (anim_timer) lv_timer_set_period(anim_timer, anim_seq & 1 ? 250 : 350);
    size_t count = ur_part_count(signed_len, anim_frag);
    if (count > 1)
        lv_label_set_text_fmt(anim_lbl, "Scan with your wallet  \xC2\xB7  %u/%u", unsigned((anim_seq - 1) % count + 1),
                              unsigned(count));
}

static void signed_done_cb(lv_event_t *) {
    if (anim_timer) lv_timer_delete(anim_timer);
    anim_timer = nullptr;
    memset(signed_buf, 0, sizeof(signed_buf));
    signed_len = 0;
    ui_goto(UI_WALLET);
}

static void build_psbt_signed() {
    lv_obj_t *scr = screen();
    lv_obj_t *bar = topbar(scr, "Signed", nullptr);
    // Full screen: the button in the bar's right corner, or a tap on the QR.
    lv_obj_t *fb = lv_button_create(bar);
    lv_obj_remove_style_all(fb);
    lv_obj_set_size(fb, 44, 44);
    lv_obj_align(fb, LV_ALIGN_RIGHT_MID, -4, 2);
    lv_obj_set_ext_click_area(fb, 12);
    lv_obj_add_event_cb(fb, qr_full_open_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(text(fb, ICON_MAXIMIZE, &icons_17, TEXT_2));
    lv_obj_t *qc = qr_card(scr, BAR, &anim_qr);
    lv_obj_add_flag(qc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(qc, qr_full_open_cb, LV_EVENT_CLICKED, nullptr);
    // What to do, and which frame of an animated QR is showing.
    anim_lbl = text(scr, "Scan with your wallet", &inter_15, TEXT);
    lv_obj_align(anim_lbl, LV_ALIGN_TOP_MID, 0, BAR + QR_CARD + 4);
    place_bottom(btn_primary(scr, "Done", signed_done_cb));
    show(scr);
    // Larger fragments (a denser QR) only for a PSBT past what 30-byte ones carry.
    anim_frag = FRAGMENT_LEN;
    while (ur_part_count(signed_len, anim_frag) > UR_MAX_FRAGMENTS) anim_frag++;
    anim_seq = 0;
    anim_tick(nullptr);
    if (anim_timer) lv_timer_delete(anim_timer);
    anim_timer = lv_timer_create(anim_tick, 300, nullptr);
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
    lv_obj_t *c = card(parent, CW, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_align(caption(c, "ADDRESS"), LV_ALIGN_TOP_LEFT, 0, 0);
    int h = 20 + address_chunks(c, check.address);
    if (req) {
        char b[40] = "Not valid";
        if (!check.amount_bad) fmt_btc(check.amount, b, sizeof(b));
        divider(c, h + 8);
        h += 16 + kv(c, h + 16, "Requests", b, check.amount_bad ? AMBER : TEXT);
    }
    h += 2 * CARD_PY;
    lv_obj_set_height(c, h);
    return h;
}

bool verify_done() { return check.status != VERIFY_SEARCHING; }

static void build_verify() {
    lv_obj_t *scr = screen();
    if (check.status == VERIFY_SEARCHING) {
        topbar(scr, "Verify address");
        lv_obj_align(text(scr, "Checking" "\xE2\x80\xA6", &rethink_24, TEXT), LV_ALIGN_TOP_MID, 0, BAR + 12);
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
    // A requested amount makes the card taller; the badge goes to make room.
    bool req = check.amount || check.amount_bad;
    int y = 16;
    if (!req) {
        lv_obj_align(icon_badge(scr, ok ? ICON_CHECK : ICON_WARNING, ok ? GREEN : RED, ok ? GREEN_DIM : RED_DIM, 40),
                     LV_ALIGN_TOP_MID, 0, 14);
        y = 62;
    }
    lv_obj_align(text(scr, ok ? "Address verified" : "Not from this seed", &rethink_24, ok ? TEXT : RED),
                 LV_ALIGN_TOP_MID, 0, y);
    if (ok) {
        char tag[40];
        snprintf(tag, sizeof(tag), ICON_CHECK "  %s #%d%s", check.chain ? "CHANGE" : "RECEIVE", check.index,
                 check.testnet ? "  \xC2\xB7  TESTNET" : "");
        lv_obj_align(chip(scr, tag, GREEN, GREEN_DIM), LV_ALIGN_TOP_MID, 0, y + 34);
        address_card(scroll_area(scr, y + 66), 0);
    } else {
        char m[96];
        snprintf(m, sizeof(m), "Checked %d addresses. Don't send here unless you're sure.", check.total);
        lv_obj_align(para(scr, m, &inter_13, TEXT_2), LV_ALIGN_TOP_MID, 0, y + 34);
        address_card(scroll_area(scr, y + 80), 0);
    }
    bottom_pair(btn_ghost(scr, "Scan", go_cb, GO(UI_SCAN), TEXT), btn_primary(scr, "Done", to_wallet_cb));
    show(scr);
}

// ---------------------------------------------------------------- multisig wallet
// A descriptor is only as good as where it came from, so before it is used the
// user checks it against the wallet app: M of N, every key's fingerprint, and
// the first receive address.
void ui_multisig_use() {
    if (ui_current() != UI_MULTISIG) return;
    ms_confirmed = true;
    ui_goto(UI_SCAN);
}
static void ms_use_cb(lv_event_t *) { ui_multisig_use(); }
static void ms_cancel_cb(lv_event_t *) { ui_goto(UI_SCAN); }   // leaving unconfirmed drops it

static void build_multisig() {
    const MsInfo *ms = multisig_info();
    lv_obj_t *scr = screen();
    char title[24];
    snprintf(title, sizeof(title), "%d of %d multisig", ms->m, ms->n);
    topbar(scr, title, ms_cancel_cb);
    lv_obj_align(text(scr, "Check it against your wallet app", &inter_13, TEXT_3), LV_ALIGN_TOP_MID, 0, BAR + 2);

    lv_obj_t *area = scroll_area(scr, BAR + 28);
    static char keys[MS_MAX_KEYS][20], fps[MS_MAX_KEYS][12];
    KvRow rows[MS_MAX_KEYS];
    for (int i = 0; i < ms->n; i++) {
        const uint8_t *f = ms->fingerprint[i];
        if (i == ms->ours) snprintf(keys[i], sizeof(keys[i]), ICON_FINGERPRINT "  This seed");
        else snprintf(keys[i], sizeof(keys[i]), ICON_FINGERPRINT "  Key %d", i + 1);
        snprintf(fps[i], sizeof(fps[i]), "%02x%02x %02x%02x", f[0], f[1], f[2], f[3]);
        rows[i] = {keys[i], fps[i], i == ms->ours ? GREEN : TEXT, i > 0};
    }
    lv_obj_t *kc = kv_card(area, rows, ms->n, &mono_16);
    lv_obj_update_layout(kc);

    char first[96] = "";
    multisig_address(0, 0, first, sizeof(first));
    lv_obj_t *c = card(area, CW, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, lv_obj_get_height(kc) + 8);
    lv_obj_t *cap = caption(c, ms->testnet ? "FIRST  \xC2\xB7  TESTNET" : "FIRST ADDRESS");
    if (ms->testnet) lv_obj_set_style_text_color(cap, AMBER, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_height(c, 20 + address_chunks(c, first) + 2 * CARD_PY);

    bottom_pair(btn_ghost(scr, "Cancel", ms_cancel_cb, nullptr, TEXT), btn_primary(scr, "Use wallet", ms_use_cb));
    show(scr);
}

// ---------------------------------------------------------------- navigation
// Making or entering a seed: only while none is loaded.
static bool seed_entry_screen(UiScreen s) {
    return s == UI_NEW_SEED || s == UI_ENTROPY_DICE || s == UI_ENTROPY_COINS || s == UI_ENTER_WORDS ||
           s == UI_PAGE_FOUND || s == UI_PAGE_READING || s == UI_PAGE_REVIEW || s == UI_PAGE_WORD;
}
// The card flow: for a new seed, or the loaded one.
static bool backup_screen(UiScreen s) {
    return s == UI_BACKUP_WARNING || s == UI_BACKUP_WORDS || s == UI_BACKUP_CHOICE || s == UI_BACKUP_QR ||
           s == UI_BACKUP_BLOCK || s == UI_BACKUP_SCAN || s == UI_SEED_READY;
}

// ---------------------------------------------------------------- brightness
// Drag down from the top edge, on any screen: a panel with a brightness slider
// slides over it. Tap below it or drag it back up to close. RAM only, like
// everything else: full brightness again after unplugging.
constexpr int BRIGHT_MIN = 1;      // low enough that a webcam doesn't overexpose a QR in a dim room
constexpr int BRIGHT_EDGE = 24;    // a drag must start this close to the top
constexpr int BRIGHT_PULL = 32;    // and come down this far
constexpr int BRIGHT_H = 92;
static int brightness = 100;
// The slider has no padding to keep its knob inside the track: padding would
// also start the fill 14px in, a notch at the track's left end. Instead its
// value stops half a knob short of each end; the setting maps to that stretch.
constexpr int BRIGHT_SL_H = 28, BRIGHT_SPAN = 1000;
constexpr int BRIGHT_LO = BRIGHT_SPAN * (BRIGHT_SL_H / 2) / CW, BRIGHT_HI = BRIGHT_SPAN - BRIGHT_LO;
static lv_obj_t *bright_scrim, *bright_panel;
static lv_obj_t *saver;   // the screensaver, see below
static bool bright_closing;
static lv_point_t pull_from;
static lv_timer_t *pull_timer;
// The screensaver's level (or the setting, if lower), and the least the open
// brightness panel shows itself at: a setting too dark to see in daylight can
// always be undone, and takes effect when the panel closes.
constexpr int BRIGHT_SEEN = 10;

static void apply_backlight() {
    if (saver) return platform_backlight(LV_MIN(brightness, BRIGHT_SEEN));
    platform_backlight(bright_scrim ? LV_MAX(brightness, BRIGHT_SEEN) : brightness);
}

// The slider is quadratic, so the dim end (where a webcam reads a QR in a dark
// room) gets a fair share of its length: 1 to 5% is the first fifth.
static int bright_from_slider(int c) {
    float t = float(c - BRIGHT_LO) / (BRIGHT_HI - BRIGHT_LO);
    return BRIGHT_MIN + int(lroundf((100 - BRIGHT_MIN) * t * t));
}

static int bright_to_slider(int b) {
    return BRIGHT_LO + int(lroundf(sqrtf(float(b - BRIGHT_MIN) / (100 - BRIGHT_MIN)) * (BRIGHT_HI - BRIGHT_LO)));
}

static void bright_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(sl), c = LV_CLAMP(BRIGHT_LO, v, BRIGHT_HI);
    if (c != v) lv_slider_set_value(sl, c, LV_ANIM_OFF);
    brightness = bright_from_slider(c);
    apply_backlight();
}

static void bright_slide(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }
static void bright_closed(lv_anim_t *) {
    lv_obj_delete(bright_scrim);
    bright_scrim = bright_panel = nullptr;
    bright_closing = false;
    apply_backlight();
}

// Dragged back up: close.
static void bright_push_cb(lv_event_t *e) {
    static int32_t from;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) from = p.y;
    else if (from - p.y > BRIGHT_PULL / 2) {
        lv_indev_wait_release(lv_indev_active());
        ui_brightness_panel(false);
    }
}

void ui_brightness_panel(bool open) {
    if (open == (bright_scrim != nullptr) || bright_closing) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, bright_slide);
    lv_anim_set_duration(&a, 160);
    if (!open) {
        lv_anim_set_var(&a, bright_panel);
        lv_anim_set_values(&a, lv_obj_get_y(bright_panel), -BRIGHT_H - 16);
        lv_anim_set_completed_cb(&a, bright_closed);
        bright_closing = true;   // one close only; taps meanwhile still land on the scrim
        lv_anim_start(&a);
        return;
    }
    bright_scrim = lv_obj_create(lv_layer_top());
    apply_backlight();
    lv_obj_remove_style_all(bright_scrim);
    lv_obj_set_size(bright_scrim, W, H);
    lv_obj_set_style_bg_color(bright_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bright_scrim, LV_OPA_50, 0);
    lv_obj_add_flag(bright_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bright_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bright_scrim, [](lv_event_t *) { ui_brightness_panel(false); }, LV_EVENT_CLICKED, nullptr);

    bright_panel = lv_obj_create(bright_scrim);
    lv_obj_remove_style_all(bright_panel);
    // Taller than shown by its radius, which sits off screen: only the bottom is rounded.
    lv_obj_set_size(bright_panel, W, BRIGHT_H + 16);
    lv_obj_set_style_pad_top(bright_panel, 16, 0);
    lv_obj_set_style_bg_color(bright_panel, SURFACE, 0);
    lv_obj_set_style_bg_opa(bright_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bright_panel, 16, 0);
    lv_obj_add_flag(bright_panel, LV_OBJ_FLAG_CLICKABLE);         // taps on it don't close it
    lv_obj_remove_flag(bright_panel, LV_OBJ_FLAG_SCROLLABLE);      // a scroll would eat the swipe up
    lv_obj_add_event_cb(bright_panel, bright_push_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(bright_panel, bright_push_cb, LV_EVENT_PRESSING, nullptr);

    lv_obj_align(caption(bright_panel, "BRIGHTNESS"), LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *sl = lv_slider_create(bright_panel);
    lv_obj_remove_style_all(sl);
    lv_obj_set_size(sl, CW, BRIGHT_SL_H);
    lv_slider_set_range(sl, 0, BRIGHT_SPAN);
    lv_slider_set_value(sl, bright_to_slider(brightness), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, ACCENT_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 12);
    lv_obj_add_event_cb(sl, bright_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_t *grip = lv_obj_create(bright_panel);                // says: drag back up
    lv_obj_remove_style_all(grip);
    lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grip, 36, 4);
    lv_obj_set_style_radius(grip, 2, 0);
    lv_obj_set_style_bg_color(grip, TEXT_3, 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_align(grip, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_anim_set_var(&a, bright_panel);
    lv_anim_set_values(&a, -BRIGHT_H - 16, -16);
    lv_anim_start(&a);
}

// A touch that starts at the top edge is followed until it comes down far
// enough or lets go, whatever screen is under it: a scrolling list or a button
// would otherwise take the drag for itself. (LVGL tells an input device's
// callbacks about the press, not the movement, so a timer follows it.)
static void pull_tick(lv_timer_t *t) {
    lv_indev_t *in = (lv_indev_t *)lv_timer_get_user_data(t);
    lv_point_t p;
    lv_indev_get_point(in, &p);
    bool down = lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
    int dy = p.y - pull_from.y, dx = LV_ABS(p.x - pull_from.x);
    bool sideways = dx > dy && dx > BRIGHT_PULL / 2;
    if (down && !sideways && dy < BRIGHT_PULL) return;
    lv_timer_delete(t);
    pull_timer = nullptr;
    if (!down || sideways || bright_scrim) return;
    lv_indev_reset(in, nullptr);       // whatever was pressed doesn't get a click
    lv_indev_wait_release(in);
    ui_brightness_panel(true);
}

static void pull_cb(lv_event_t *e) {
    lv_indev_t *in = (lv_indev_t *)lv_event_get_target(e);
    lv_indev_get_point(in, &pull_from);
    if (pull_from.y >= BRIGHT_EDGE || bright_scrim || saver || pull_timer) return;
    pull_timer = lv_timer_create(pull_tick, 15, in);
}

// ---------------------------------------------------------------- screensaver
// A minute untouched on Home or Wallet: the goldfish swims about, with the
// backlight at BRIGHT_SEEN (or the setting, if lower), which also hides the fingerprint. A tap brings the screen back and does nothing else.
constexpr uint32_t SAVER_IDLE_MS = 60000;
constexpr int FISH_W = 84, FISH_H = 48, N_BUBBLES = 8;
// Where the fish swims (its top-left corner); bubbles go up off the screen.
constexpr int X_MIN = 8, X_MAX = W - FISH_W - 8, Y_MIN = 40, Y_MAX = H - FISH_H - 60;
static lv_timer_t *saver_timer, *idle_timer;
static lv_obj_t *saver_fish, *saver_thought, *saver_thought_l;
static struct Bubble { lv_obj_t *o; float x, y; int phase; } bubbles[N_BUBBLES];
static float fish_x, fish_y, fish_ty;
static int fish_dir;   // -1 swims left (the logo's own way), +1 right
static uint32_t saver_t0, saver_last, next_bubble, next_thought, thought_until;
static int thought_i;
// Goldfish memory meets bitcoin. One line each at inter_13, and nothing the device doesn't do.
static const char *const thoughts[] = {
    "Blub.",
    "Don't trust. Verify.",
    "Where was I?",
    "Not your keys... wait, whose?",
    "21 million bubbles. Max.",
    "Ooh, a castle!",
    "Tick tock, next block.",
    "Did I have a seed just now?",
    "Stay humble. Blow bubbles.",
    "Few understand. I forgot.",
    "Low time preference: 3 sec.",
    "No Wi-Fi. Just water.",
    "Unplug me and I forget.",
    "Sign and... what was it?",
};

// The logo turned to face right: the same pixels, each row reversed (RGB565 plane, then alpha).
// Built on the heap while the screensaver is up and freed when it closes, so the
// 12 KB are free again for reading words; without the memory the fish keeps facing left.
constexpr size_t FISH_BYTES = FISH_W * FISH_H * 3;
static uint8_t *fish_right_map;
static lv_image_dsc_t fish_right;
static const lv_image_dsc_t *fish_facing(int dir) {
    if (dir < 0 || goldfish_logo.header.w != FISH_W || goldfish_logo.header.h != FISH_H ||
        goldfish_logo.data_size != FISH_BYTES)
        return &goldfish_logo;
    if (!fish_right_map) {
        if (!(fish_right_map = (uint8_t *)malloc(FISH_BYTES))) return &goldfish_logo;
        const uint8_t *src = goldfish_logo.data;
        uint8_t *rgb = fish_right_map, *a = fish_right_map + FISH_W * FISH_H * 2;
        for (int y = 0; y < FISH_H; y++)
            for (int x = 0; x < FISH_W; x++) {
                int from = y * FISH_W + (FISH_W - 1 - x), to = y * FISH_W + x;
                rgb[to * 2] = src[from * 2];
                rgb[to * 2 + 1] = src[from * 2 + 1];
                a[to] = src[FISH_W * FISH_H * 2 + from];
            }
        fish_right = goldfish_logo;
        fish_right.data = fish_right_map;
    }
    return &fish_right;
}

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

static float wave(uint32_t t, int ms_per_turn, int phase_deg = 0) {
    return lv_trigo_sin((int16_t)((t % ms_per_turn) * 360 / ms_per_turn + phase_deg)) / 32767.0f;
}

static void saver_tick(lv_timer_t *) {
    uint32_t now = lv_tick_get(), t = now - saver_t0;
    float dt = LV_MIN(now - saver_last, 100u);
    saver_last = now;

    // Swim to one side, turn, and wander up and down on the way.
    fish_x += fish_dir * 0.03f * dt;
    if ((fish_dir > 0 && fish_x > X_MAX) || (fish_dir < 0 && fish_x < X_MIN)) {
        fish_dir = -fish_dir;
        lv_image_set_src(saver_fish, fish_facing(fish_dir));
        fish_ty = lv_rand(Y_MIN, Y_MAX);
    }
    fish_y += (fish_ty - fish_y) * 0.0015f * dt;
    float bob = 4 * wave(t, 2000);
    lv_obj_set_pos(saver_fish, (int)fish_x, (int)(fish_y + bob));
    lv_image_set_rotation(saver_fish, (int)(fish_dir * 30 * wave(t, 2000, 90)));   // nose follows the bob

    // Bubbles from the mouth, wobbling up.
    int mouth_x = fish_dir > 0 ? (int)fish_x + FISH_W - 8 : (int)fish_x + 8;
    if (reached(now, next_bubble)) {
        for (Bubble &b : bubbles) {
            if (!lv_obj_has_flag(b.o, LV_OBJ_FLAG_HIDDEN)) continue;
            int d = lv_rand(3, 7);
            lv_obj_set_size(b.o, d, d);
            b.x = mouth_x - d / 2;
            b.y = fish_y + bob + FISH_H / 3;
            b.phase = lv_rand(0, 359);
            lv_obj_remove_flag(b.o, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        next_bubble = now + lv_rand(700, 2200);
    }
    for (Bubble &b : bubbles) {
        if (lv_obj_has_flag(b.o, LV_OBJ_FLAG_HIDDEN)) continue;
        b.y -= 0.035f * dt;
        if (b.y < -12) { lv_obj_add_flag(b.o, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_set_pos(b.o, (int)(b.x + 2 * wave(t, 1400, b.phase)), (int)b.y);
    }

    // Now and then, a thought, in a bubble over the fish.
    if (reached(now, next_thought)) {
        lv_label_set_text(saver_thought_l, thoughts[thought_i++ % (sizeof(thoughts) / sizeof(*thoughts))]);
        lv_obj_remove_flag(saver_thought, LV_OBJ_FLAG_HIDDEN);
        thought_until = now + 3000;
        next_thought = now + 10000;
    }
    if (reached(now, thought_until)) lv_obj_add_flag(saver_thought, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(saver_thought);
    int tw = lv_obj_get_width(saver_thought), th = lv_obj_get_height(saver_thought);
    int ty = (int)fish_y - th - 6;
    if (ty < 4) ty = (int)fish_y + FISH_H + 6;
    lv_obj_set_pos(saver_thought, LV_CLAMP(4, (int)fish_x + FISH_W / 2 - tw / 2, W - tw - 4), ty);
}

static lv_obj_t *saver_part(lv_obj_t *o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void ui_screensaver(bool open) {
    if (open == (saver != nullptr)) return;
    if (!open) {
        lv_timer_delete(saver_timer);
        saver_timer = nullptr;
        lv_obj_delete(saver);
        saver = saver_fish = saver_thought = nullptr;
        free(fish_right_map);   // nothing shows it now
        fish_right_map = nullptr;
        apply_backlight();
        return;
    }
    saver = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(saver);
    lv_obj_set_size(saver, W, H);
    lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(saver, BG, 0);
    lv_obj_remove_flag(saver, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(saver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(saver, [](lv_event_t *) {
        lv_indev_wait_release(lv_indev_active());   // the tap only wakes the screen
        ui_screensaver(false);
    }, LV_EVENT_PRESSED, nullptr);

    for (Bubble &b : bubbles) {
        b.o = saver_part(lv_obj_create(saver));
        lv_obj_remove_style_all(b.o);
        lv_obj_set_style_radius(b.o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b.o, 1, 0);
        lv_obj_set_style_border_color(b.o, lv_color_hex(0xB9C4CA), 0);
        lv_obj_add_flag(b.o, LV_OBJ_FLAG_HIDDEN);
    }

    fish_dir = -1;
    fish_x = X_MAX;
    fish_y = fish_ty = (Y_MIN + Y_MAX) / 2;
    saver_fish = saver_part(lv_image_create(saver));
    lv_image_set_src(saver_fish, fish_facing(fish_dir));
    lv_image_set_pivot(saver_fish, FISH_W / 2, FISH_H / 2);

    saver_thought = saver_part(lv_obj_create(saver));
    lv_obj_remove_style_all(saver_thought);
    lv_obj_set_size(saver_thought, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(saver_thought, SURFACE, 0);
    lv_obj_set_style_bg_opa(saver_thought, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(saver_thought, 1, 0);
    lv_obj_set_style_border_color(saver_thought, BORDER, 0);
    lv_obj_set_style_radius(saver_thought, 12, 0);
    lv_obj_set_style_pad_hor(saver_thought, 10, 0);
    lv_obj_set_style_pad_ver(saver_thought, 5, 0);
    saver_thought_l = text(saver_thought, "", &inter_13, TEXT);
    lv_obj_add_flag(saver_thought, LV_OBJ_FLAG_HIDDEN);

    saver_t0 = saver_last = lv_tick_get();
    next_bubble = saver_t0 + 300;
    next_thought = saver_t0 + 2000;
    thought_i = 0;
    saver_tick(nullptr);
    saver_timer = lv_timer_create(saver_tick, 30, nullptr);
    apply_backlight();
}

static void idle_tick(lv_timer_t *) {
    if (saver || bright_scrim || (current != UI_HOME && current != UI_WALLET)) return;
    if (lv_display_get_inactive_time(nullptr) >= SAVER_IDLE_MS) ui_screensaver(true);
}

void ui_goto(UiScreen s) {
    ui_screensaver(false);
    if (ui_current() == UI_PAGE_REVIEW) page_review_y = lv_obj_get_scroll_y(page_review_list);
    // (Words found shows preview_buf: the photo, kept there; scanning stopped before it)
    // Reading words keeps the camera on: a camera just woken gave blank frames, and the
    // sharp picture comes right after the still (the reading step stops it).
    if (s != UI_SCAN_SEED && s != UI_SCAN && s != UI_BACKUP_SCAN && s != UI_PAGE_FOUND) scan_stop(s != UI_PAGE_READING);
    if (s != UI_PASSPHRASE) pass_wipe();
    if (s != UI_EXPORT_XPUB) key_details = false;
    // A scanned descriptor that wasn't confirmed on its screen is never used.
    if (s != UI_MULTISIG && !ms_confirmed) multisig_clear();
    if (s == UI_MULTISIG && !multisig_loaded()) s = UI_WALLET;
    // The new seed lives only inside its own flow: leaving it wipes everything.
    if (!seed_entry_screen(s) && !backup_screen(s)) new_seed_wipe();
    bool needs_pending = backup_screen(s) && s != UI_BACKUP_BLOCK && s != UI_BACKUP_SCAN;
    bool needs_qr = s == UI_BACKUP_BLOCK || s == UI_BACKUP_SCAN;
    if ((needs_pending && !pending.len) || (needs_qr && (!pending.len || !qr_n))) { new_seed_wipe(); s = UI_HOME; }
    if (s != UI_VERIFY_RESULT && verify_timer) { lv_timer_delete(verify_timer); verify_timer = nullptr; }
    if (s != UI_PAGE_READING && page_timer) { lv_timer_delete(page_timer); page_timer = nullptr; }
    if (s != UI_PSBT_SIGNED && anim_timer) { lv_timer_delete(anim_timer); anim_timer = nullptr; }
    if (ready_timer) { lv_timer_delete(ready_timer); ready_timer = nullptr; }
    bool no_seed_yet = s == UI_HOME || s == UI_UPDATE || s == UI_SCAN_SEED || seed_entry_screen(s);
    if (!no_seed_yet && !backup_screen(s) && !wallet_has_seed()) s = UI_HOME;
    if (no_seed_yet && wallet_has_seed()) { new_seed_wipe(); s = UI_WALLET; }
    current = s;
    switch (s) {
        case UI_HOME: build_home(); break;
        case UI_SCAN_SEED: build_scan(SCAN_SEED); break;
        case UI_WALLET: build_wallet(); break;
        case UI_PASSPHRASE: build_passphrase(); break;
        case UI_WALLET_OPTIONS: build_wallet_options(); break;
        case UI_SCAN: build_scan(SCAN_WALLET); break;
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
        case UI_ENTER_WORDS: build_enter_words(); break;
        case UI_BACKUP_WARNING: build_backup_warning(); break;
        case UI_BACKUP_WORDS: build_backup_words(); break;
        case UI_BACKUP_CHOICE: build_backup_choice(); break;
        case UI_SEED_READY: build_seed_ready(); break;
        case UI_BACKUP_QR: build_backup_qr(); break;
        case UI_BACKUP_BLOCK: build_backup_block(); break;
        case UI_BACKUP_SCAN: build_scan(SCAN_CONFIRM); break;
        case UI_CHILD_INDEX: build_child_index(); break;
        case UI_MULTISIG: build_multisig(); break;
        case UI_PAGE_FOUND: build_page_found(); break;
        case UI_PAGE_READING: build_page_reading(); break;
        case UI_PAGE_REVIEW: build_page_review(); break;
        case UI_PAGE_WORD: build_page_word(); break;
        case UI_UPDATE: build_update(); break;
    }
    apply_backlight();
}

UiScreen ui_current() { return current; }

void ui_build() {
    for (lv_indev_t *in = lv_indev_get_next(nullptr); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) continue;
        lv_indev_add_event_cb(in, pull_cb, LV_EVENT_PRESSED, nullptr);
    }
    idle_timer = lv_timer_create(idle_tick, 500, nullptr);
    ui_goto(UI_HOME);
}
