// Board entry point: brings up the display, touch and IMU, then runs the shared UI.
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "touch.h"
#include "imu.h"
#include "../app/ui.h"

void platform_reboot_to_bootloader() { rp2040.rebootToBootloader(); }

// The second core does one thing: its half of reading a word
// (platform_parallel in word_reader.h). Otherwise it sleeps in WFE.
static void (*volatile core1_fn)(void *);
static void *volatile core1_arg;

void platform_parallel(void (*fn)(void *), void *a, void *b) {
    core1_arg = a;
    __dmb();
    core1_fn = fn;
    __sev();
    fn(b);
    while (core1_fn) __wfe();
    __dmb();   // core1's writes before ours from here
}

// Its own stack (8 KB, from the heap): the default one sits in the scratch RAM
// that core 0's stack otherwise has to itself (8 KB, not 4).
bool core1_separate_stack = true;

void setup1() {}

void loop1() {
    while (!core1_fn) __wfe();
    __dmb();
    core1_fn(core1_arg);
    __dmb();
    core1_fn = nullptr;
    __sev();
}

void setup() {
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    display_init();
    touch_init();
    imu_init();
    ui_build();
}

void loop() {
    imu_poll();
    uint32_t wait = lv_timer_handler();
    delay(wait < 5 ? wait : 5);
}
