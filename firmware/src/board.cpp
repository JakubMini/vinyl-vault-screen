// The ESP32-4848S040: an ST7701S 480x480 RGB panel, set up over 3-wire SPI, and a GT911 touch
// controller. The pins and init sequence are Arduino_GFX's for this board
// (ESP32_4848S040_86BOX_GUITION); if a board revision differs, fix it here only.
#include "board.h"

#include <Arduino_GFX_Library.h>
#include <bb_captouch.h>
#include <rom/cache.h>

static Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 39 /* CS */, 48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    18 /* DE */, 17 /* VSYNC */, 16 /* HSYNC */, 21 /* PCLK */,
    11, 12, 13, 14, 0,     // R0-R4
    8, 20, 3, 46, 9, 10,   // G0-G5
    4, 5, 6, 7, 15,        // B0-B4
    1, 10, 8, 50,          // hsync polarity, front porch, pulse width, back porch
    1, 10, 8, 20,          // vsync polarity, front porch, pulse width, back porch
    0, 12000000, false, 0, 0,
    // Bounce buffers copy the PSRAM framebuffer through internal RAM, so the picture holds still
    // while Wi-Fi and the cover decoder are busy with PSRAM.
    BOARD_W * 10);
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    BOARD_W, BOARD_H, panel, 0, true, bus, GFX_NOT_DEFINED, st7701_type9_init_operations,
    sizeof(st7701_type9_init_operations));

static BBCapTouch touch;
static constexpr int BACKLIGHT = 38;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *px) {
  board_draw(area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area), (const uint16_t *)px);
  lv_display_flush_ready(display);
}

uint32_t profile_touch_us = 0;  // time in GT911 reads, for the wheel's profiler

static void read_touch(lv_indev_t *, lv_indev_data_t *data) {
  const uint32_t start = micros();
  TOUCHINFO ti;
  if (touch.getSamples(&ti) && ti.count > 0) {
    data->point.x = ti.x[0];
    data->point.y = ti.y[0];
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  profile_touch_us += micros() - start;
}

void board_begin() {
  // The panel is started from core 0 so its interrupt runs there. With bounce buffers that
  // interrupt copies the whole frame out of PSRAM, about 19 MB/s, and on core 1 it took a third of
  // the time the UI has to draw the wheel.
  static TaskHandle_t caller;
  caller = xTaskGetCurrentTaskHandle();
  xTaskCreatePinnedToCore([](void *) {
    if (!gfx->begin()) Serial.println(R"({"event":"display_failed"})");
    xTaskNotifyGive(caller);
    vTaskDelete(nullptr);
  }, "panel", 4096, nullptr, 5, nullptr, 0);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  gfx->fillScreen(0x0000);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, HIGH);
  if (touch.init(19 /* SDA */, 45 /* SCL */) != CT_SUCCESS) Serial.println(R"({"event":"touch_failed"})");

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *display = lv_display_create(BOARD_W, BOARD_H);
  // Two partial buffers in internal RAM: LVGL draws one while the other is copied out. Kept
  // small, because each TLS connection needs about 40 KB of internal RAM too.
  const size_t bytes = BOARD_W * 40 * sizeof(uint16_t);
  void *a = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void *b = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_display_set_buffers(display, a, b, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, read_touch);
}

// Rows go into the panel's framebuffer with memcpy, then out of the CPU cache to PSRAM, where the
// panel reads them. About three times faster than the driver's own per-pixel copy.
void board_draw(int x, int y, int w, int h, const uint16_t *px) {
  uint16_t *fb = gfx->getFramebuffer() + y * BOARD_W + x;
  for (int r = 0; r < h; r++) memcpy(fb + r * BOARD_W, px + r * w, w * 2);
  Cache_WriteBack_Addr((uint32_t)fb, ((h - 1) * BOARD_W + w) * 2);
}
