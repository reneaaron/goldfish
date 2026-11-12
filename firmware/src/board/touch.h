#pragma once
#include <lvgl.h>

// Set up the CST816D on I2C0 and register it as an LVGL pointer.
// Call after display_init(), which has already pulsed the shared reset line.
lv_indev_t *touch_init();
