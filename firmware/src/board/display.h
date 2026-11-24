#pragma once
#include <lvgl.h>

// Bring up the panel and register it with LVGL. Call after lv_init().
lv_display_t *display_init();

// Show the UI turned 180° (board held upside down); redraws the whole screen.
// Touch reads this to map its points the same way.
void display_set_flipped(bool flipped);
bool display_flipped();
