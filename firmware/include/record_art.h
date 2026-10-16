// The record from jakubszypicyn.com/vinyls, one RGB565 pixel at a time: black vinyl, fine grooves,
// two sheens on the diagonal and a green label. Kept free of LVGL so it can be tested on the
// laptop. It is drawn once into PSRAM, never while a frame is being drawn.
#pragma once
#include <math.h>
#include <stdint.h>

constexpr uint16_t ART_BG = 0x0861;          // #0B0F0C, the screen's background
constexpr uint16_t ART_RIM = 0x3A07;         // #3C403A
constexpr uint16_t ART_LABEL = 0x2F8F;       // #2FF27C
constexpr uint16_t ART_LABEL_RING = 0x25CB;  // a darker green

// A colour in RGB565's own units (0-31, 0-63, 0-31), so the 565 constants above come back exactly.
struct ArtRgb {
  float r, g, b;
};
inline ArtRgb art_rgb(uint16_t c) { return {float(c >> 11), float((c >> 5) & 0x3F), float(c & 0x1F)}; }
inline ArtRgb art_mix(ArtRgb a, ArtRgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
// How much of the pixel at distance r lies inside a circle of radius edge: the edges are smooth.
inline float art_inside(float r, float edge) { return fminf(1.0f, fmaxf(0.0f, edge - r + 0.5f)); }

// Pixel (x, y) of an n x n record; everything off the disc is `outside`. Coordinates may lie
// beyond the square, so a smaller record can be drawn inset on a sleeve.
inline uint16_t record_px(int x, int y, int n, uint16_t outside) {
  const float R = n / 2.0f, dx = x - R + 0.5f, dy = y - R + 0.5f;
  const float r = hypotf(dx, dy), u = r / R;
  if (r > R + 0.5f) return outside;

  // Light catches the upper-left and lower-right, fading smoothly up to 0.33 rad off that diagonal.
  const float m = fmodf(atan2f(dy, dx) + 2.3562f + 6.2832f, 3.1416f);
  const float off = fminf(m, 3.1416f - m);
  const float sheen = off < 0.33f ? 0.5f + 0.5f * cosf(off / 0.33f * 3.1416f) : 0.0f;
  float v = 6 + 26 * sheen;  // grey, 0-255
  if (u > 0.38f && u < 0.94f) {
    // Grooves, barely there in the dark, showing where the light catches them, and three track gaps.
    v += (1.2f + 8.0f * sheen) * cosf(r * 2.5133f);  // a groove every 2.5 px
    float gaps = 0;
    for (int i = 0; i < 3; i++) {
      const float t = (r - (0.52f + 0.14f * i) * R) / 1.2f;  // at 0.52, 0.66 and 0.80 of the radius
      gaps += expf(-t * t);
    }
    v += (2.0f + 11.0f * sheen) * gaps;
  }
  ArtRgb c = {v * 31 / 255, v * 63 / 255, v * 31 / 255};
  c = art_mix(c, art_rgb(ART_RIM), 1 - art_inside(r, 0.965f * R));
  const ArtRgb ring = art_mix(art_rgb(ART_LABEL), art_rgb(ART_LABEL_RING),
                              art_inside(r, 0.305f * R) - art_inside(r, 0.29f * R));
  c = art_mix(c, ring, art_inside(r, 0.34f * R));
  c = art_mix(c, art_rgb(ART_BG), art_inside(r, fmaxf(2.0f, n * 0.012f)));  // the spindle hole
  c = art_mix(art_rgb(outside), c, art_inside(r, R));

  // Ordered dither into 565: the dark greys have only a few steps, which would band the sheen.
  static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  const float d = (bayer[(y & 3) * 4 + (x & 3)] + 0.5f) / 16 - 0.5f;
  auto q = [d](float ch, int max) { return (uint16_t)fminf(max, fmaxf(0, floorf(ch + 0.5f + d))); };
  return q(c.r, 31) << 11 | q(c.g, 63) << 5 | q(c.b, 31);
}
