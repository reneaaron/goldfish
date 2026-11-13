#include "theme.h"
#include "amount.h"
#include <initializer_list>
#include <stdio.h>
#include <string.h>

namespace th {
// "Paper": warm off-white like the paper SeedQR card, ink-black primary
// buttons, burnt orange only for marks (icons, totals, the slide knob).
const lv_color_t BG = LV_COLOR_MAKE(0xF3, 0xEF, 0xE6);
const lv_color_t SURFACE = LV_COLOR_MAKE(0xFF, 0xFD, 0xF8);
const lv_color_t SURFACE_2 = LV_COLOR_MAKE(0xE6, 0xE0, 0xD3);
const lv_color_t BORDER = LV_COLOR_MAKE(0xD9, 0xD2, 0xC3);
const lv_color_t TEXT = LV_COLOR_MAKE(0x1B, 0x19, 0x17);
const lv_color_t TEXT_2 = LV_COLOR_MAKE(0x5C, 0x56, 0x50);
const lv_color_t TEXT_3 = LV_COLOR_MAKE(0x7A, 0x73, 0x69);
const lv_color_t ACCENT = LV_COLOR_MAKE(0xC2, 0x41, 0x0C);
const lv_color_t ACCENT_DIM = LV_COLOR_MAKE(0xF6, 0xE3, 0xCF);
const lv_color_t GREEN = LV_COLOR_MAKE(0x15, 0x80, 0x3D);
const lv_color_t GREEN_DIM = LV_COLOR_MAKE(0xDC, 0xEF, 0xE2);
const lv_color_t RED = LV_COLOR_MAKE(0xC6, 0x28, 0x28);
const lv_color_t RED_DIM = LV_COLOR_MAKE(0xF8, 0xDC, 0xD8);
const lv_color_t AMBER = LV_COLOR_MAKE(0xA1, 0x62, 0x07);
const lv_color_t AMBER_DIM = LV_COLOR_MAKE(0xF5, 0xE6, 0xC4);

static void plain(lv_obj_t *o) {
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *screen() {
    lv_obj_t *s = lv_obj_create(nullptr);
    plain(s);
    lv_obj_set_style_bg_color(s, BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, TEXT, 0);
    lv_obj_set_style_text_font(s, &inter_15, 0);
    return s;
}

lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

lv_obj_t *caption(lv_obj_t *parent, const char *s) {
    lv_obj_t *l = text(parent, s, &inter_13, TEXT_3);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

lv_obj_t *para(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = text(parent, s, f, c);
    lv_obj_set_width(l, CW);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(l, 2, 0);
    return l;
}

lv_obj_t *topbar(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb, const char *pill, lv_color_t pfg,
                 lv_color_t pbg) {
    lv_obj_t *bar = lv_obj_create(scr);
    plain(bar);
    lv_obj_set_size(bar, W, BAR);
    lv_obj_t *t = text(bar, title, &inter_17_semi, TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 2);
    if (back_cb) {
        lv_obj_t *b = lv_button_create(bar);
        plain(b);
        lv_obj_set_size(b, 44, 44);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 4, 2);
        lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *i = text(b, ICON_LEFT, &inter_17_semi, TEXT_2);
        lv_obj_center(i);
    }
    if (pill) {
        lv_obj_t *p = chip(bar, pill, pfg, pbg);
        lv_obj_align(p, LV_ALIGN_RIGHT_MID, -10, 2);
    }
    return bar;
}

static lv_obj_t *btn_base(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud, lv_color_t bg,
                          lv_color_t fg, lv_opa_t bg_opa) {
    lv_obj_t *b = lv_button_create(parent);
    plain(b);
    lv_obj_set_size(b, CW, 48);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, bg_opa, 0);
    // pressed: slightly darker / visible feedback
    lv_obj_set_style_bg_opa(b, bg_opa ? LV_OPA_80 : LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, bg_opa ? bg : SURFACE_2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(b, 245, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = text(b, s, &inter_17_semi, fg);
    lv_obj_center(l);
    return b;
}

lv_obj_t *btn_primary(lv_obj_t *p, const char *s, lv_event_cb_t cb, void *ud) {
    return btn_base(p, s, cb, ud, TEXT, BG, LV_OPA_COVER);
}
lv_obj_t *btn_danger(lv_obj_t *p, const char *s, lv_event_cb_t cb, void *ud) {
    return btn_base(p, s, cb, ud, RED, lv_color_white(), LV_OPA_COVER);
}
lv_obj_t *btn_ghost(lv_obj_t *p, const char *s, lv_event_cb_t cb, void *ud, lv_color_t c) {
    lv_obj_t *b = btn_base(p, s, cb, ud, SURFACE_2, c, LV_OPA_TRANSP);
    lv_obj_set_height(b, 40);
    return b;
}

lv_obj_t *card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    plain(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, SURFACE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_border_color(c, BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_hor(c, CARD_PX, 0);
    lv_obj_set_style_pad_ver(c, CARD_PY, 0);
    return c;
}

lv_obj_t *chip(lv_obj_t *parent, const char *s, lv_color_t fg, lv_color_t bg) {
    lv_obj_t *c = lv_obj_create(parent);
    plain(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, bg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(c, 9, 0);
    lv_obj_set_style_pad_ver(c, 3, 0);
    lv_obj_t *l = text(c, s, &inter_13, fg);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_center(l);
    return c;
}

lv_obj_t *icon_badge(lv_obj_t *parent, const char *icon, lv_color_t fg, lv_color_t bg, int size) {
    lv_obj_t *c = lv_obj_create(parent);
    plain(c);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, bg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_t *l = text(c, icon, &icons_34, fg);
    lv_obj_center(l);
    return c;
}

static int width(const char *s, const lv_font_t *f) { return lv_text_get_width(s, strlen(s), f, 0); }
static int line_h(const lv_font_t *f) { return lv_font_get_line_height(f); }

constexpr int KV_GAP = 10;    // between a key and a value beside it
constexpr int KV_UNDER = 14;  // a value under its key starts this far down
constexpr int KV_STEP = 8;    // between rows, and again after a divider

static bool kv_beside(const KvRow &r, const lv_font_t *f) {
    return width(r.v, f) <= CARD_IW - width(r.k, &inter_15) - KV_GAP;
}

// The one font for these values, and whether they go under their keys.
static const lv_font_t *kv_font(const KvRow *rows, int n, const lv_font_t *vf, bool *under) {
    const lv_font_t *chain[] = {vf, &inter_15, &inter_13};
    for (int pass = 0; pass < 2; pass++) {
        for (const lv_font_t *f : chain) {
            bool ok = true;
            for (int i = 0; i < n && ok; i++) ok = pass ? width(rows[i].v, f) <= CARD_IW : kv_beside(rows[i], f);
            if (ok) { *under = pass; return f; }
        }
    }
    *under = true;   // wider than the card even in inter_13: can't happen for amounts
    return &inter_13;
}

static int kv_row_h(const lv_font_t *f, bool under) {
    return under ? KV_UNDER + line_h(f) : LV_MAX(line_h(&inter_15), line_h(f));
}

static void kv_place(lv_obj_t *c, int y, const KvRow &r, const lv_font_t *f, bool under) {
    lv_obj_align(text(c, r.k, under ? &inter_13 : &inter_15, TEXT_2, LV_TEXT_ALIGN_LEFT), LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_align(text(c, r.v, f, r.c, LV_TEXT_ALIGN_RIGHT), LV_ALIGN_TOP_RIGHT, 0, under ? y + KV_UNDER : y);
}

int kv(lv_obj_t *c, int y, const char *k, const char *v, lv_color_t vc, const lv_font_t *vf) {
    KvRow r = {k, v, vc, false};
    bool under;
    const lv_font_t *f = kv_font(&r, 1, vf, &under);
    kv_place(c, y, r, f, under);
    return kv_row_h(f, under);
}

lv_obj_t *kv_card(lv_obj_t *parent, const KvRow *rows, int n, const lv_font_t *vf) {
    bool under;
    const lv_font_t *f = kv_font(rows, n, vf, &under);
    int rh = kv_row_h(f, under), ys[8], y = 0;
    for (int i = 0; i < n && i < 8; i++) {
        if (i) y += rh + (under ? KV_STEP / 2 : KV_STEP);   // stacked rows are already airy
        if (rows[i].rule) y += KV_STEP;
        ys[i] = y;
    }
    lv_obj_t *c = card(parent, CW, y + rh + 2 * CARD_PY);
    for (int i = 0; i < n && i < 8; i++) {
        if (rows[i].rule) divider(c, ys[i] - KV_STEP);
        kv_place(c, ys[i], rows[i], f, under);
    }
    return c;
}

void divider(lv_obj_t *c, int y) {
    lv_obj_t *d = lv_obj_create(c);
    plain(d);
    lv_obj_set_size(d, lv_pct(100), 1);
    lv_obj_set_style_bg_color(d, BORDER, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, y);
}

lv_obj_t *amount(lv_obj_t *parent, uint64_t v, lv_color_t c) {
    lv_obj_t *col = lv_obj_create(parent);
    plain(col);
    lv_obj_set_size(col, CW, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    char b[40];
    fmt_btc(v, b, sizeof(b));
    const lv_font_t *f = &inter_13;
    for (const lv_font_t *g : {&mono_22, &mono_16, &inter_15})
        if (width(b, g) <= CW) { f = g; break; }
    text(col, b, f, c);
    fmt_btc_legacy(v, b, sizeof(b));
    text(col, b, &inter_13, TEXT_3);
    return col;
}

lv_obj_t *list_row(lv_obj_t *parent, const char *icon, const char *title, const char *sub, lv_event_cb_t cb,
                   void *ud) {
    lv_obj_t *b = lv_button_create(parent);
    plain(b);
    lv_obj_set_size(b, lv_pct(100), 62);
    lv_obj_set_style_bg_color(b, SURFACE_2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    int tx = 16;
    if (icon) {
        tx = 58;
        lv_obj_t *ic = lv_obj_create(b);
        plain(ic);
        lv_obj_set_size(ic, 36, 36);
        lv_obj_set_style_radius(ic, 10, 0);
        lv_obj_set_style_bg_color(ic, ACCENT_DIM, 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *il = text(ic, icon, &inter_17_semi, ACCENT);
        lv_obj_center(il);
    }

    lv_obj_t *t = text(b, title, &inter_17_semi, TEXT, LV_TEXT_ALIGN_LEFT);
    if (sub) {
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, tx, 12);
        lv_obj_t *s = text(b, sub, &inter_13, TEXT_3, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, tx, 34);
    } else {
        lv_obj_align(t, LV_ALIGN_LEFT_MID, tx, 0);
    }
    return b;
}
}  // namespace th
