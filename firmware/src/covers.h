// Covers for the screen: the baseline JPEGs the sync task makes, held in PSRAM and decoded on a
// background task into RGB565, 240 px. Decoding one takes a few milliseconds.
#pragma once
#include <lvgl.h>

void covers_begin();
// A new or changed cover from the sync task (it also writes it to flash). The bytes are copied.
void covers_put(int id, const uint8_t *jpeg, size_t len);
// The covers to have ready, nearest the front first. Replaces the previous list.
void covers_want(const int *ids, int n);
// A record's cover, or nullptr while it decodes or is not cached yet. Valid while it stays wanted.
const lv_image_dsc_t *covers_get(int id);
