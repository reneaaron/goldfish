#pragma once
#include <lvgl.h>

// Bring up the panel and register it with LVGL. Call after lv_init().
lv_display_t *display_init();
