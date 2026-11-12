#include "display.h"
#include <Arduino.h>
#include "hardware/spi.h"
#include "board_pins.h"

// Panel init sequence copied from Waveshare's RP2350-Touch-LCD-2 demo
// (LCD_2in.cpp, MIT-style license). 0xFF marks a delay in ms, 0xFE a command.
static const uint8_t INIT_SEQ[] = {
    0xFE, 0x11, 0xFF, 120,
    0xFE, 0x36, 0x08,                  // portrait, BGR order as in the demo
    0xFE, 0x3A, 0x05,                  // RGB565
    0xFE, 0xF0, 0xC3,
    0xFE, 0xF0, 0x96,
    0xFE, 0xB4, 0x01,
    0xFE, 0xB7, 0xC6,
    0xFE, 0xC0, 0x80, 0x45,
    0xFE, 0xC1, 0x13,
    0xFE, 0xC2, 0xA7,
    0xFE, 0xC5, 0x0A,
    0xFE, 0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,
    0xFE, 0xE0, 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31,
    0xFE, 0xE1, 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32,
    0xFE, 0xF0, 0x3C,
    0xFE, 0xF0, 0x69,
    0xFF, 120,
    0xFE, 0x21,                        // inversion on
    0xFE, 0x29,                        // display on
};

static inline void cs(bool on) { gpio_put(PIN_LCD_CS, on ? 0 : 1); }

static void cmd(uint8_t c) {
    gpio_put(PIN_LCD_DC, 0);
    cs(true);
    spi_write_blocking(spi0, &c, 1);
    cs(false);
}

static void data(const uint8_t *d, size_t n) {
    gpio_put(PIN_LCD_DC, 1);
    cs(true);
    spi_write_blocking(spi0, d, n);
    cs(false);
}

static void run_init() {
    size_t i = 0;
    while (i < sizeof(INIT_SEQ)) {
        uint8_t b = INIT_SEQ[i++];
        if (b == 0xFF) {
            delay(INIT_SEQ[i++]);
        } else if (b == 0xFE) {
            cmd(INIT_SEQ[i++]);
            size_t start = i;
            while (i < sizeof(INIT_SEQ) && INIT_SEQ[i] != 0xFE && INIT_SEQ[i] != 0xFF) i++;
            if (i > start) data(&INIT_SEQ[start], i - start);
        }
    }
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t xa[] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
    uint8_t ya[] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
    cmd(0x2A); data(xa, 4);
    cmd(0x2B); data(ya, 4);
    cmd(0x2C);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
    uint32_t n = lv_area_get_size(a);
    lv_draw_sw_rgb565_swap(px, n);     // panel wants big-endian RGB565
    set_window(a->x1, a->y1, a->x2, a->y2);
    data(px, n * 2);
    lv_display_flush_ready(disp);
}

static uint8_t buf1[LCD_W * 40 * 2];
static uint8_t buf2[LCD_W * 40 * 2];

lv_display_t *display_init() {
    spi_init(spi0, 62500000);
    gpio_set_function(PIN_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LCD_MOSI, GPIO_FUNC_SPI);
    const uint pins[] = {PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST, PIN_LCD_BL};
    for (uint p : pins) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_OUT);
    }
    gpio_put(PIN_LCD_CS, 1);
    gpio_put(PIN_LCD_BL, 0);

    // Hardware reset (also resets the touch controller)
    gpio_put(PIN_LCD_RST, 1); delay(100);
    gpio_put(PIN_LCD_RST, 0); delay(100);
    gpio_put(PIN_LCD_RST, 1); delay(100);

    run_init();
    gpio_put(PIN_LCD_BL, 1);

    lv_display_t *d = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_set_buffers(d, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    return d;
}
