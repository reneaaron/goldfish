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

// Font Awesome glyphs available in inter_13/15/17 and icons_34
#define ICON_CHECK        "\xEF\x80\x8C"
#define ICON_CLOSE        "\xEF\x80\x8D"
#define ICON_LEFT         "\xEF\x81\x93"
#define ICON_RIGHT        "\xEF\x81\x94"
#define ICON_WARNING      "\xEF\x81\xB1"
#define ICON_TRASH        "\xEF\x8B\xAD"
#define ICON_QR           "\xEF\x80\xA9"
#define ICON_KEY          "\xEF\x82\x84"
#define ICON_ARROW_RIGHT  "\xEF\x81\xA1"
#define ICON_CHECK_CIRCLE "\xEF\x81\x98"
#define ICON_SHIELD       "\xEF\x8F\xAD"
#define ICON_SIGN         "\xEF\x95\xB3"
#define ICON_WALLET       "\xEF\x95\x95"
#define ICON_CAMERA       "\xEF\x80\xB0"
#define ICON_EYE          "\xEF\x81\xAE"

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
