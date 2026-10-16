#pragma once
#include <lvgl.h>

constexpr int BOARD_W = 480;
constexpr int BOARD_H = 480;

// Panel, backlight, touch and LVGL. Call once from setup().
void board_begin();
// Copy RGB565 pixels straight into the panel, bypassing LVGL (the wheel draws itself this way).
void board_draw(int x, int y, int w, int h, const uint16_t *px);
