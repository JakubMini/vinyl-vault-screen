#include <Arduino.h>
#include <esp_app_desc.h>

#include "board.h"
#include "covers.h"
#include "nfc.h"
#include "store.h"
#include "sync.h"
#include "ui.h"

#ifdef PANEL_BENCH
#include <LittleFS.h>
#include <WiFi.h>

// Once Wi-Fi is up: 20 s idle, 20 s of flash writes, then a sync, logging the panel every second.
static void panel_bench(void *) {
  while (!WiFi.isConnected()) delay(500);
  // In PSRAM: 16 KB of internal RAM is the room TLS needs for the sync.
  static uint8_t *block = (uint8_t *)heap_caps_calloc(1, 16384, MALLOC_CAP_SPIRAM);
  auto phase = [](const char *name, uint32_t ms, bool write) {
    uint32_t sent0, missed0, sent, missed;
    board_panel_stats(sent0, missed0);
    for (uint32_t start = millis(), last = start; millis() - start < ms;) {
      if (write) {
        File f = LittleFS.open("/bench", "w");
        f.write(block, 16384);
        f.close();
      }
      delay(write ? 1 : 50);
      if (millis() - last < 1000) continue;
      last = millis();
      board_panel_stats(sent, missed);
      Serial.printf("{\"event\":\"panel_bench\",\"phase\":\"%s\",\"frames\":%lu,\"missed\":%lu,"
                    "\"internal_free\":%u,\"internal_largest\":%u}\n", name, sent - sent0, missed - missed0,
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
  };
  phase("idle", 20000, false);
  phase("flash", 20000, true);
  LittleFS.remove("/bench");
  sync_now();
  phase("sync", 120000, false);
  vTaskDelete(nullptr);
}
#endif

void setup() {
  // Ordinary allocations over 64 bytes go to PSRAM, so the record list doesn't break internal RAM
  // into pieces too small for TLS, which needs one block of about 17 KB per connection.
  heap_caps_malloc_extmem_enable(64);
  Serial.begin(115200);
  char build[9];  // the first 8 of what the vault's Screens page shows as the build
  esp_app_get_elf_sha256(build, sizeof build);
  Serial.printf("{\"event\":\"boot\",\"psram\":%u,\"build\":\"%s\"}\n", ESP.getPsramSize(), build);
  board_begin();
  store_begin();
  sync_begin();
  covers_begin();
  nfc_begin();
  Serial.printf("{\"event\":\"memory\",\"internal_free\":%u,\"internal_largest\":%u}\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  ui_begin();
#ifdef PANEL_BENCH
  xTaskCreatePinnedToCore(panel_bench, "bench", 4096, nullptr, tskIDLE_PRIORITY, nullptr, 0);
#endif
}

uint32_t profile_loop_us = 0;  // time in LVGL's handler, for the wheel's profiler

void loop() {
  const uint32_t start = micros();
  lv_timer_handler();
  profile_loop_us += micros() - start;
  delay(2);
}
