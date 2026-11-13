#pragma once
// Visual system for the 240x320 screen: colour tokens, fonts and the few
// components every screen is built from. Layout grid: 16px side margins,
// 208px content width, 48px top bar, primary action pinned 16px from the bottom.
#include <lvgl.h>

LV_FONT_DECLARE(inter_13);
LV_FONT_DECLARE(inter_15);
LV_FONT_DECLARE(inter_17_semi);
LV_FONT_DECLARE(inter_22_semi);
LV_FONT_DECLARE(inter_30_semi);
LV_FONT_DECLARE(mono_16);
LV_FONT_DECLARE(mono_22);
LV_FONT_DECLARE(mono_26);
LV_FONT_DECLARE(icons_34);
LV_FONT_DECLARE(icons_40);

// Lucide icons (lucide.dev), available in inter_13/15/17 and icons_34
#define ICON_CHECK        "\xEE\x81\xB0"   // check
#define ICON_CLOSE        "\xEE\x86\xB2"   // x
#define ICON_LEFT         "\xEE\x81\xB2"   // chevron-left
#define ICON_RIGHT        "\xEE\x81\xB3"   // chevron-right
#define ICON_WARNING      "\xEE\x86\x93"   // triangle-alert
#define ICON_TRASH        "\xEE\x86\x8E"   // trash-2
#define ICON_QR           "\xEE\x87\x9F"   // qr-code
#define ICON_KEY          "\xEE\x92\xA8"   // key-round
#define ICON_ARROW_RIGHT  "\xEE\x81\x8D"   // arrow-right
#define ICON_CHECK_CIRCLE "\xEE\x88\xA6"   // circle-check
#define ICON_SHIELD       "\xEE\x85\x9C"   // shield
#define ICON_SIGN         "\xEE\x97\xB7"   // signature
#define ICON_WALLET       "\xEE\x88\x84"   // wallet
#define ICON_CAMERA       "\xEE\x81\xA8"   // camera
#define ICON_EYE          "\xEE\x82\xBE"   // eye
#define ICON_KEYBOARD     "\xEE\x8A\x84"   // keyboard
#define ICON_DICE         "\xEE\x8B\x85"   // dices
#define ICON_COINS        "\xEE\x82\x9B"   // coins

// icons_40 only: dice faces and the two sides of a coin
#define ICON_DICE_1       "\xEE\x8A\x87"   // dice-1
#define ICON_DICE_2       "\xEE\x8A\x88"   // dice-2
#define ICON_DICE_3       "\xEE\x8A\x89"   // dice-3
#define ICON_DICE_4       "\xEE\x8A\x8A"   // dice-4
#define ICON_DICE_5       "\xEE\x8A\x8B"   // dice-5
#define ICON_DICE_6       "\xEE\x8A\x8C"   // dice-6
#define ICON_HEADS        "\xEE\x91\xAD"   // user-round: a head
#define ICON_TAILS        "\xEE\x87\xB2"   // binary: the number side

namespace th {
extern const lv_color_t BG, SURFACE, SURFACE_2, BORDER, TEXT, TEXT_2, TEXT_3;
extern const lv_color_t ACCENT, ACCENT_DIM, GREEN, GREEN_DIM, RED, RED_DIM, AMBER, AMBER_DIM;

constexpr int W = 240, H = 320, M = 16, CW = W - 2 * M, BAR = 48;
constexpr int CARD_PX = 14, CARD_PY = 12, CARD_IW = CW - 2 * CARD_PX;   // card padding, inner width
constexpr int BTN_H = 48;

lv_obj_t *screen();
// Top bar: optional back chevron (calls back_cb), centred title, optional pill on the right.
lv_obj_t *topbar(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb = nullptr,
                 const char *pill = nullptr, lv_color_t pill_fg = TEXT_2, lv_color_t pill_bg = SURFACE_2);
lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c,
               lv_text_align_t align = LV_TEXT_ALIGN_CENTER);
lv_obj_t *caption(lv_obj_t *parent, const char *s);     // small upper-case label
// Centred paragraph wrapped to the content width.
lv_obj_t *para(lv_obj_t *parent, const char *s, const lv_font_t *f = &inter_15, lv_color_t c = TEXT_2);
lv_obj_t *btn_primary(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud = nullptr);
lv_obj_t *btn_danger(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud = nullptr);
lv_obj_t *btn_ghost(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud = nullptr,
                    lv_color_t c = TEXT_2);
lv_obj_t *card(lv_obj_t *parent, int w, int h);
lv_obj_t *chip(lv_obj_t *parent, const char *s, lv_color_t fg, lv_color_t bg);
lv_obj_t *icon_badge(lv_obj_t *parent, const char *icon, lv_color_t fg, lv_color_t bg, int size = 64);
// "key ........ value" inside a CW-wide card, at row y (relative to the card's
// content box). A long value shrinks (vf, then inter_15, inter_13) and, if it
// still can't sit beside the key, goes under it. Never overlaps or truncates.
// Returns the row's height.
int kv(lv_obj_t *card, int y, const char *k, const char *v, lv_color_t vc = TEXT,
       const lv_font_t *vf = &inter_15);
void divider(lv_obj_t *card, int y);
struct KvRow {
    const char *k, *v;
    lv_color_t c;
    bool rule;   // divider above this row
};
// A CW-wide card of kv rows, sized to fit. All values share one font so the
// digits line up, and either all sit beside their keys or all go under them.
lv_obj_t *kv_card(lv_obj_t *parent, const KvRow *rows, int n, const lv_font_t *vf = &inter_15);
// BIP177: amounts are whole bitcoins (the base unit), shown as "₿24,857".
// Headline amount with the legacy BTC value under it, for checking against
// wallets that still show BTC. Shrinks to fit; never truncates.
lv_obj_t *amount(lv_obj_t *parent, uint64_t v, lv_color_t c = TEXT);
// List row for menus: icon, title, optional subtitle (nullptr). Whole row is the button.
lv_obj_t *list_row(lv_obj_t *parent, const char *icon, const char *title, const char *sub,
                   lv_event_cb_t cb, void *ud = nullptr);
}  // namespace th
