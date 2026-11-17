// Board entry point: brings up the display and touch, then runs the shared UI.
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "touch.h"
#include "../app/ui.h"

void setup() {
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    display_init();
    touch_init();
    ui_build();
}

void loop() {
    uint32_t wait = lv_timer_handler();
    delay(wait < 5 ? wait : 5);
}
