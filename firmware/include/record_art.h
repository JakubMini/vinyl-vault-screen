// The record from jakubszypicyn.com/vinyls, one RGB565 pixel at a time: black vinyl, fine grooves,
// two sheens on the diagonal and a green label. Kept free of LVGL so it can be tested on the
// laptop. It is drawn once at boot into PSRAM, never while a frame is being drawn.
#pragma once
#include <math.h>
#include <stdint.h>

constexpr uint16_t ART_BG = 0x0861;          // #0B0F0C, the screen's background
constexpr uint16_t ART_RIM = 0x3A07;         // #3C403A
constexpr uint16_t ART_LABEL = 0x2F8F;       // #2FF27C
constexpr uint16_t ART_LABEL_RING = 0x25CB;  // a darker green

// Pixel (x, y) of an n x n record; everything off the disc is `outside`. Coordinates may lie
// beyond the square, so a smaller record can be drawn inset on a sleeve.
inline uint16_t record_px(int x, int y, int n, uint16_t outside) {
  const float dx = x - n / 2.0f + 0.5f, dy = y - n / 2.0f + 0.5f;
  const float r = hypotf(dx, dy), u = r / (n / 2.0f);
  if (u > 1) return outside;
  if (r < fmaxf(2.0f, n * 0.012f)) return ART_BG;  // the spindle hole
  if (u < 0.34f) return u > 0.29f && u < 0.305f ? ART_LABEL_RING : ART_LABEL;
  if (u > 0.965f) return ART_RIM;
  int v = (int)r % 4 == 0 && u > 0.38f && u < 0.94f ? 14 : 6;  // grey level: a groove every 4 px
  // Light catches the upper-left and lower-right: 0 on that diagonal, up to pi/2 off it.
  const float m = fmodf(atan2f(dy, dx) + 2.3562f + 6.2832f, 3.1416f);
  const float off = fminf(m, 3.1416f - m);
  if (off < 0.33f) v += (int)(26 * (1 - off / 0.33f));
  return (v >> 3) << 11 | (v >> 2) << 5 | v >> 3;
}
