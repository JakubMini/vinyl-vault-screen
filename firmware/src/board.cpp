// The Waveshare ESP32-S3-Touch-LCD-4B: an ST7701 480x480 RGB panel and a GT911 touch controller.
// The panel's 3-wire SPI, its reset and the touch reset sit on a TCA9554 IO expander (I2C 0x20);
// pins and init are Waveshare's Arduino examples for this board. If a revision differs, fix it here.
#include "board.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <bb_captouch.h>
#include <rom/cache.h>

static constexpr int BUS_SDA = 47, BUS_SCL = 48;  // shared by the expander, the touch, the codec and the PMU
static Arduino_XCA9554SWSPI *expander =
    new Arduino_XCA9554SWSPI(7 /* LCD RST */, 0 /* CS */, 2 /* SCK */, 1 /* MOSI */, &Wire, 0x20);
static Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    17 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 9 /* PCLK */,
    10, 11, 12, 13, 14,     // R0-R4
    21, 8, 18, 45, 38, 39,  // G0-G5
    40, 41, 42, 2, 1,       // B0-B4
    1, 10, 8, 50,           // hsync polarity, front porch, pulse width, back porch
    1, 10, 8, 20,           // vsync polarity, front porch, pulse width, back porch
    0, 12000000, false, 0, 0,
    // Bounce buffers copy the PSRAM framebuffer through internal RAM, so the picture holds still
    // while Wi-Fi and the cover decoder are busy with PSRAM. 20 rows, not 10: on a busy network
    // Wi-Fi interrupts on core 0 delayed the refill and the panel showed stale lines.
    BOARD_W * 20);
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    BOARD_W, BOARD_H, panel, 0, true, expander, GFX_NOT_DEFINED, st7701_type1_init_operations,
    sizeof(st7701_type1_init_operations));

static BBCapTouch touch;
static constexpr int BACKLIGHT = 4;  // active low

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
  Wire.begin(BUS_SDA, BUS_SCL);  // before the panel: the expander's begin() takes Wire's default pins otherwise
  xTaskCreatePinnedToCore([](void *) {
    if (!gfx->begin()) Serial.println(R"({"event":"display_failed"})");
    xTaskNotifyGive(caller);
    vTaskDelete(nullptr);
  }, "panel", 4096, nullptr, 5, nullptr, 0);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  gfx->fillScreen(0x0000);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, LOW);
  // GT911 reset through the expander (EXIO5), its INT (EXIO6) held low meanwhile, as Waveshare does.
  expander->pinMode(6, OUTPUT);
  expander->digitalWrite(6, LOW);
  expander->pinMode(5, OUTPUT);
  expander->digitalWrite(5, LOW);
  delay(10);
  expander->digitalWrite(5, HIGH);
  delay(60);
  expander->pinMode(6, INPUT);
  if (touch.init(BUS_SDA, BUS_SCL) != CT_SUCCESS) Serial.println(R"({"event":"touch_failed"})");

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *display = lv_display_create(BOARD_W, BOARD_H);
  // Two partial buffers in internal RAM: LVGL draws one while the other is copied out. Small:
  // LVGL only draws text and controls (the wheel draws itself), and each TLS connection needs a
  // block of about 17 KB of internal RAM in one piece.
  const size_t bytes = BOARD_W * 20 * sizeof(uint16_t);
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
