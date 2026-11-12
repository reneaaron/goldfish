#include "theme.h"
#include <initializer_list>
#include <stdio.h>
#include <string.h>

namespace th {
const lv_color_t BG = LV_COLOR_MAKE(0x0A, 0x0A, 0x0B);
const lv_color_t SURFACE = LV_COLOR_MAKE(0x16, 0x16, 0x18);
const lv_color_t SURFACE_2 = LV_COLOR_MAKE(0x22, 0x22, 0x25);
const lv_color_t BORDER = LV_COLOR_MAKE(0x2A, 0x2A, 0x2E);
const lv_color_t TEXT = LV_COLOR_MAKE(0xF4, 0xF4, 0xF2);
const lv_color_t TEXT_2 = LV_COLOR_MAKE(0xA3, 0xA3, 0xA8);
const lv_color_t TEXT_3 = LV_COLOR_MAKE(0x6E, 0x6E, 0x74);
const lv_color_t ACCENT = LV_COLOR_MAKE(0xF7, 0x93, 0x1A);
const lv_color_t ACCENT_DIM = LV_COLOR_MAKE(0x33, 0x22, 0x0E);
const lv_color_t GREEN = LV_COLOR_MAKE(0x3D, 0xD6, 0x8C);
const lv_color_t GREEN_DIM = LV_COLOR_MAKE(0x10, 0x2E, 0x20);
const lv_color_t RED = LV_COLOR_MAKE(0xFF, 0x5A, 0x4F);
const lv_color_t RED_DIM = LV_COLOR_MAKE(0x3A, 0x15, 0x13);
const lv_color_t AMBER = LV_COLOR_MAKE(0xFF, 0xC4, 0x4D);
const lv_color_t AMBER_DIM = LV_COLOR_MAKE(0x3A, 0x2C, 0x0E);

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
    return btn_base(p, s, cb, ud, ACCENT, LV_COLOR_MAKE(0x14, 0x0C, 0x02), LV_OPA_COVER);
}
lv_obj_t *btn_danger(lv_obj_t *p, const char *s, lv_event_cb_t cb, void *ud) {
    return btn_base(p, s, cb, ud, RED, LV_COLOR_MAKE(0x1A, 0x05, 0x04), LV_OPA_COVER);
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
    lv_obj_set_style_pad_hor(c, 14, 0);
    lv_obj_set_style_pad_ver(c, 12, 0);
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

int kv_room(const char *k) { return CW - 2 * 14 - lv_text_get_width(k, strlen(k), &inter_15, 0) - 10; }

const lv_font_t *fit(const char *s, int w, std::initializer_list<const lv_font_t *> fonts) {
    for (const lv_font_t *f : fonts)
        if (lv_text_get_width(s, strlen(s), f, 0) <= w) return f;
    return nullptr;
}

void kv(lv_obj_t *c, int y, const char *k, const char *v, lv_color_t vc, const lv_font_t *vf) {
    lv_obj_t *kl = text(c, k, &inter_15, TEXT_2, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(kl, LV_ALIGN_TOP_LEFT, 0, y);
    // Shrink a long value (a big amount) rather than let it run into the key.
    const lv_font_t *f = fit(v, kv_room(k), {vf, &inter_15, &inter_13});
    lv_obj_t *vl = text(c, v, f ? f : &inter_13, vc, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(vl, LV_ALIGN_TOP_RIGHT, 0, y);
}

void divider(lv_obj_t *c, int y) {
    lv_obj_t *d = lv_obj_create(c);
    plain(d);
    lv_obj_set_size(d, lv_pct(100), 1);
    lv_obj_set_style_bg_color(d, BORDER, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, y);
}

// "24,857"
static size_t group(unsigned long long v, char *out, size_t cap) {
    char raw[24];
    snprintf(raw, sizeof(raw), "%llu", v);
    size_t n = strlen(raw), o = 0;
    for (size_t i = 0; i < n && o + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
    return o;
}

void fmt_btc(uint64_t v, char *out, size_t cap) {
    size_t o = snprintf(out, cap, "\xE2\x82\xBF");   // U+20BF
    group(v, out + o, cap - o);
}

void fmt_btc_legacy(uint64_t v, char *out, size_t cap) {
    size_t o = group(v / 100000000ull, out, cap);
    char f[9];
    snprintf(f, sizeof(f), "%08llu", (unsigned long long)(v % 100000000ull));
    int n = 8;
    while (n && f[n - 1] == '0') n--;
    if (n) snprintf(out + o, cap - o, ".%.*s BTC", n, f);
    else snprintf(out + o, cap - o, " BTC");
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
    const lv_font_t *f = fit(b, CW, {&mono_22, &mono_16, &inter_15});
    text(col, b, f ? f : &inter_13, c);
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

    lv_obj_t *ic = lv_obj_create(b);
    plain(ic);
    lv_obj_set_size(ic, 36, 36);
    lv_obj_set_style_radius(ic, 10, 0);
    lv_obj_set_style_bg_color(ic, ACCENT_DIM, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *il = text(ic, icon, &inter_17_semi, ACCENT);
    lv_obj_center(il);

    lv_obj_t *t = text(b, title, &inter_17_semi, TEXT, LV_TEXT_ALIGN_LEFT);
    if (sub) {
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 58, 12);
        lv_obj_t *s = text(b, sub, &inter_13, TEXT_3, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 58, 34);
    } else {
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 58, 0);
    }
    return b;
}
}  // namespace th
