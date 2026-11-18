#include "touch.h"
#include <Arduino.h>
#include <Wire.h>
#include "board_pins.h"

// Register map and setup values from Waveshare's CST816D demo driver.
enum : uint8_t {
    REG_FINGERS = 0x02,
    REG_CHIP_ID = 0xA7,
    REG_IRQ_PULSE = 0xED,
    REG_SCAN_PERIOD = 0xEE,
    REG_IRQ_CTL = 0xFA,
    REG_DIS_AUTOSLEEP = 0xFE,
};

static void wr(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
}

static bool rd(uint8_t reg, uint8_t *out, size_t n) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TOUCH_ADDR, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) out[i] = Wire.read();
    return true;
}

// LVGL only polls between timers, and a scan screen's frame (preview redraw +
// quirc) can stall it longer than a tap lasts. The touch INT line latches that
// a touch happened, so a tap that came and went unseen still becomes a click.
static volatile bool touched;
static bool was_pressed;

static void touch_isr() { touched = true; }

static void read_cb(lv_indev_t *, lv_indev_data_t *data) {
    uint8_t r[5];  // fingers, XH, XL, YH, YL
    if (!rd(REG_FINGERS, r, sizeof(r))) {
        data->state = LV_INDEV_STATE_RELEASED;
        was_pressed = false;
        return;
    }
    bool down = r[0] > 0;
    if (!down && was_pressed) touched = false;   // the pulses of a press we already reported
    if (down || touched) {
        touched = false;
        // after a lift the chip keeps the last point: that's where the missed tap was
        int32_t x = ((r[1] & 0x0F) << 8) | r[2];
        int32_t y = ((r[3] & 0x0F) << 8) | r[4];
        // TODO: confirm orientation on hardware. The demo maps landscape as
        // (y, 240 - x), which implies the raw axes are native portrait.
        data->point.x = LV_CLAMP(0, x, LCD_W - 1);
        data->point.y = LV_CLAMP(0, y, LCD_H - 1);
        data->state = LV_INDEV_STATE_PRESSED;
        was_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        was_pressed = false;
    }
}

lv_indev_t *touch_init() {
    Wire.setSDA(PIN_I2C0_SDA);
    Wire.setSCL(PIN_I2C0_SCL);
    Wire.setClock(400000);
    Wire.begin();

    wr(REG_DIS_AUTOSLEEP, 0x01);
    wr(REG_IRQ_CTL, 0x41);        // point mode
    wr(REG_IRQ_PULSE, 0x01);
    wr(REG_SCAN_PERIOD, 0x01);

    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_isr, FALLING);

    lv_indev_t *t = lv_indev_create();
    lv_indev_set_type(t, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(t, read_cb);
    return t;
}
