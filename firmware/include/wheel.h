// The ferris wheel's geometry, kept free of LVGL and Arduino so it can be tested on the laptop.
#pragma once
#include <math.h>

// Covers ride the top of a big circle whose centre is below the screen. d is a cover's distance
// from the front, in records: 0 is the front cover, -1 the one before it, 0.5 halfway round.
struct WheelSlot {
  float x, y;   // the cover's centre on screen
  float scale;  // 1 at the front, smaller round the rim
  int opa;      // 255 at the front, fading round the rim
};

constexpr float WHEEL_CX = 240, WHEEL_CY = 162;  // the front cover's centre
constexpr float WHEEL_R = 260;                   // the circle's radius
constexpr float WHEEL_STEP = 0.5f;               // radians between neighbours, about 29 degrees
constexpr float WHEEL_FRONT_PX = 180;            // the front cover's width
constexpr float WHEEL_DRAG_PX = 125;             // finger travel that turns the wheel one record

inline WheelSlot wheel_slot(float d) {
  const float a = d * WHEEL_STEP, ad = fabsf(d);
  const float scale = fmaxf(0.35f, 1.0f - 0.22f * ad);
  const float opa = fmaxf(0.0f, 255.0f - 80.0f * ad);
  return {WHEEL_CX + WHEEL_R * sinf(a), WHEEL_CY + WHEEL_R * (1.0f - cosf(a)), scale, (int)opa};
}

// Any index onto 0..n-1, so the wheel goes round: the last record sits before the first.
inline int wheel_index(int i, int n) { return ((i % n) + n) % n; }

// Columns [a, b) of a screen row. The wheel keeps the drawn ones sorted and merged, so a cover
// further back is only drawn where nothing nearer already is.
struct Span {
  int a, b;
};

// Add [a, b) to n sorted, merged spans (room for n + 1, n under 16); returns the new count.
inline int spans_add(Span *spans, int n, int a, int b) {
  Span out[16], add = {a, b};
  int m = 0;
  bool placed = false;
  for (int i = 0; i < n; i++) {
    if (spans[i].b < add.a) {
      out[m++] = spans[i];
    } else if (spans[i].a > add.b) {
      if (!placed) out[m++] = add, placed = true;
      out[m++] = spans[i];
    } else {
      add = {spans[i].a < add.a ? spans[i].a : add.a, spans[i].b > add.b ? spans[i].b : add.b};
    }
  }
  if (!placed) out[m++] = add;
  for (int i = 0; i < m; i++) spans[i] = out[i];
  return m;
}
