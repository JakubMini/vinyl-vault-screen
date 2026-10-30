#include <Arduino.h>

#include "board.h"
#include "covers.h"
#include "store.h"
#include "sync.h"
#include "ui.h"

void setup() {
  // Ordinary allocations over 64 bytes go to PSRAM, so the record list doesn't break internal RAM
  // into pieces too small for TLS, which needs one block of about 17 KB per connection.
  heap_caps_malloc_extmem_enable(64);
  Serial.begin(115200);
  Serial.printf("{\"event\":\"boot\",\"psram\":%u}\n", ESP.getPsramSize());
  board_begin();
  store_begin();
  sync_begin();
  covers_begin();
  // shortcut: no NFC on this board yet; its old pins (IO40, IO2, IO1) now drive the panel. Wire
  // the PN532 up again (I2C or SPI) and call nfc_begin() here.
  Serial.printf("{\"event\":\"memory\",\"internal_free\":%u,\"internal_largest\":%u}\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  ui_begin();
}

uint32_t profile_loop_us = 0;  // time in LVGL's handler, for the wheel's profiler

void loop() {
  const uint32_t start = micros();
  lv_timer_handler();
  profile_loop_us += micros() - start;
  delay(2);
}
