#pragma once
#define LV_CONF_H
// Minimal LVGL 9.2 config for a 240x320 RGB565 panel on RP2350 (520KB SRAM).
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (96U * 1024U)   // two screens coexist during transitions
#define LV_DEF_REFR_PERIOD 20
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_QRCODE 1
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

#ifdef SIM_X11
#define LV_USE_X11 1
#define LV_X11_DIRECT_EXIT 1
#define LV_X11_DOUBLE_BUFFER 1
#define LV_X11_RENDER_MODE_PARTIAL 1
#endif
