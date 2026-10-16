// LVGL settings. Anything not set here keeps LVGL's default (lv_conf_internal.h).
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
// The C library's malloc puts big blocks in PSRAM, so LVGL needs no heap of its own.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 16

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_QRCODE 1
// The on-screen keyboard uses this one: unlike our own fonts it has the key symbols.
#define LV_FONT_MONTSERRAT_20 1
#endif
