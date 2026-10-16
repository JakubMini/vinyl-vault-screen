// The screens: a message (Wi-Fi, errors), Wi-Fi setup, the link screen (QR + code), the wheel of
// covers and one record with its grades. Everything shown comes from the board's own copy of the
// collection (store.h), kept fresh by the sync task, so nothing here waits for the network except
// linking, joining Wi-Fi and saving a new grade.
#include "ui.h"

#include <Arduino.h>

#include "board.h"
#include "covers.h"
#include "store.h"
#include "sync.h"
#include "vault.h"
#include "wheel.h"

LV_FONT_DECLARE(font_16);
LV_FONT_DECLARE(font_22);
LV_FONT_DECLARE(font_30);
LV_FONT_DECLARE(font_44);

static const lv_color_t BG = lv_color_hex(0x101010), CHIP = lv_color_hex(0x2A2A2A), DIM = lv_color_hex(0x9A9A9A),
                        ACCENT = lv_color_hex(0xF2A33A), UP = lv_color_hex(0x5CC97B), DOWN = lv_color_hex(0xE5675C);

static std::vector<Record> records;
static std::map<int, Detail> details;  // each record's history and prices, by id

// The board's copy of the collection, into RAM.
static bool load_collection() { return store_load_records(records) && (store_load_details(details), true); }
static void start();
static void open_wifi();
static void open_link();
static void open_wheel();

// ---- shared bits ---------------------------------------------------------------------------

static lv_obj_t *screen() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, BG, 0);
  lv_obj_set_style_text_color(s, lv_color_white(), 0);
  lv_obj_set_style_text_font(s, &font_22, 0);
  lv_obj_set_scrollable(s, false);
  return s;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int32_t width) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_width(l, width);
  // One line, cut with "…". Without a fixed height a DOTS label wraps onto the next one.
  lv_obj_set_height(l, lv_font_get_line_height(font));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int32_t w, int32_t h) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, CHIP, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *t = lv_label_create(b);
  lv_obj_set_style_text_font(t, &font_16, 0);
  lv_label_set_text(t, text);
  lv_obj_center(t);
  return b;
}

// One screen at a time. The old one is deleted later, never inside its own event handler: a tap
// on the wheel opens a record while that tap is still being delivered to the wheel.
static void show(lv_obj_t *s) {
  lv_obj_t *old = lv_screen_active();
  lv_screen_load(s);
  if (old && old != s) lv_obj_delete_async(old);
}

static String money(int pence) {
  if (pence == NO_VALUE) return "–";
  const int pounds = (abs(pence) + 50) / 100;
  String s = pounds >= 1000 ? String(pounds / 1000.0f, 1) + "k" : String(pounds);
  return (pence < 0 ? "-£" : "£") + s;
}

static void set_change(lv_obj_t *l, int pence) {
  if (pence == NO_VALUE || pence == 0) {
    lv_label_set_text(l, pence == 0 ? "no change in 30 days" : "");
    lv_obj_set_style_text_color(l, DIM, 0);
    return;
  }
  String text = String(pence > 0 ? "↑ " : "↓ ") + money(abs(pence)) + " in 30 days";
  lv_label_set_text(l, text.c_str());
  lv_obj_set_style_text_color(l, pence > 0 ? UP : DOWN, 0);
}

static lv_obj_t *message(const char *text) {
  lv_obj_t *s = screen();
  lv_obj_t *l = label(s, &font_22, lv_color_white(), 400);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_height(l, LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  show(s);
  lv_refr_now(nullptr);  // the caller may be about to block on the network; draw this first
  return s;
}

// A message that retries on a tap and offers other Wi-Fi on a long press.
static void problem(const char *text, void (*retry)()) {
  lv_obj_t *s = message(text);
  lv_obj_add_event_cb(s, [](lv_event_t *e) { ((void (*)())lv_event_get_user_data(e))(); }, LV_EVENT_SHORT_CLICKED,
                      (void *)retry);
  lv_obj_add_event_cb(s, [](lv_event_t *) { open_wifi(); }, LV_EVENT_LONG_PRESSED, nullptr);
}

// The vault refused the token (revoked on /link): link again.
static bool unlinked(int status) {
  if (status != 401) return false;
  vault_forget();
  open_link();
  return true;
}

// ---- the vinyl placeholder, shown until a cover is cached -------------------------------------

static lv_image_dsc_t vinyl;

static void draw_vinyl() {
  const int n = WHEEL_FRONT_PX;
  uint16_t *px = (uint16_t *)ps_malloc(n * n * 2);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float r = hypotf(x - n / 2 + 0.5f, y - n / 2 + 0.5f);
      uint16_t c = 0x2945;                                  // sleeve grey
      if (r < n * 0.47f) c = ((int)r / 5) % 2 ? 0x0861 : 0x0841;  // grooves
      if (r < n * 0.16f) c = 0xF506;                        // label, the accent colour
      if (r < 4) c = 0x0841;                                // spindle hole
      px[y * n + x] = c;
    }
  }
  vinyl.header.magic = LV_IMAGE_HEADER_MAGIC;
  vinyl.header.cf = LV_COLOR_FORMAT_RGB565;
  vinyl.header.w = vinyl.header.h = n;
  vinyl.header.stride = n * 2;
  vinyl.data_size = n * n * 2;
  vinyl.data = (const uint8_t *)px;
}

static const lv_image_dsc_t *cover_of(const Record &r) {
  const lv_image_dsc_t *c = covers_get(r.id);
  return c ? c : &vinyl;
}

// ---- record ------------------------------------------------------------------------------------

static struct {
  int index;
  Detail detail;
  lv_obj_t *value, *change, *chart, *media[GRADE_COUNT], *sleeve[GRADE_COUNT], *status;
} view;

static void paint_detail() {
  Record &r = records[view.index];
  lv_label_set_text(view.value, r.value.isEmpty() ? "Not priced" : r.value.c_str());
  set_change(view.change, r.change30);

  const std::vector<int> &h = view.detail.history;
  lv_chart_series_t *series = lv_chart_get_series_next(view.chart, nullptr);
  lv_chart_set_point_count(view.chart, max<size_t>(h.size(), 2));
  if (!h.empty()) {
    const int lo = *std::min_element(h.begin(), h.end()), hi = *std::max_element(h.begin(), h.end());
    const int pad = max(10, (hi - lo) / 8);
    lv_chart_set_axis_range(view.chart, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);
    for (size_t i = 0; i < h.size(); i++) lv_chart_set_series_value_by_id(view.chart, series, i, h[i]);
    if (h.size() == 1) lv_chart_set_series_value_by_id(view.chart, series, 1, h[0]);
  }
  lv_chart_refresh(view.chart);

  for (int g = 0; g < GRADE_COUNT; g++) {
    lv_obj_set_style_bg_color(view.media[g], r.media == GRADES[g] ? ACCENT : CHIP, 0);
    lv_obj_set_style_bg_color(view.sleeve[g], r.sleeve == GRADES[g] ? ACCENT : CHIP, 0);
    lv_label_set_text(lv_obj_get_child(view.media[g], 1), money(view.detail.price_by_grade[g]).c_str());
  }
}

// A new grade goes to the vault straight away; a new record grade re-prices the record there.
static void grade_tapped(lv_event_t *e) {
  const int code = (int)(intptr_t)lv_event_get_user_data(e);  // 0-7 record, 8-15 sleeve
  const bool media = code < GRADE_COUNT;
  const char *grade = GRADES[code % GRADE_COUNT];
  Record &r = records[view.index];
  if ((media ? r.media : r.sleeve) == grade) return;

  lv_label_set_text(view.status, "Saving…");
  lv_obj_set_style_text_color(view.status, DIM, 0);
  lv_refr_now(nullptr);
  int status = vault_set_grade(r.id, media ? "media_condition" : "sleeve_condition", grade);
  if (status == 200) status = vault_detail(r.id, view.detail);
  if (unlinked(status)) return;
  if (status != 200) {
    lv_label_set_text(view.status, "Couldn't save. Tap the grade to try again.");
    lv_obj_set_style_text_color(view.status, DOWN, 0);
    return;
  }
  r.value = view.detail.value;
  r.media = view.detail.media;
  r.sleeve = view.detail.sleeve;
  details[r.id] = view.detail;
  store_save_records(records);
  store_save_details(details);
  lv_label_set_text(view.status, "Saved.");
  paint_detail();
}

static void chip_row(lv_obj_t *parent, int y, int height, int code_base, lv_obj_t **chips, bool with_price) {
  for (int g = 0; g < GRADE_COUNT; g++) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 53, height);
    lv_obj_set_pos(c, 16 + g * 57, y);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, grade_tapped, LV_EVENT_CLICKED, (void *)(intptr_t)(code_base + g));
    lv_obj_t *grade = lv_label_create(c);
    lv_label_set_text(grade, GRADES[g]);
    lv_obj_align(grade, LV_ALIGN_CENTER, 0, with_price ? -10 : 0);
    if (with_price) {
      lv_obj_t *price = lv_label_create(c);
      lv_obj_set_style_text_font(price, &font_16, 0);
      lv_obj_align(price, LV_ALIGN_CENTER, 0, 13);
    }
    chips[g] = c;
  }
}

static void open_record(int index) {
  Record &r = records[index];
  view.index = index;
  // From the board's copy; only a record the first sync hasn't reached yet asks the vault.
  auto known = details.find(r.id);
  if (known != details.end()) {
    view.detail = known->second;
  } else {
    view.detail = Detail{"", r.media, r.sleeve, {}, {}};
    for (int &p : view.detail.price_by_grade) p = NO_VALUE;
    const int status = vault_detail(r.id, view.detail);
    if (unlinked(status)) return;
    if (status == 200) details[r.id] = view.detail;
  }

  lv_obj_t *s = screen();
  auto back = [](lv_event_t *) { open_wheel(); };
  lv_obj_add_event_cb(s, [](lv_event_t *) {
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) open_wheel();
  }, LV_EVENT_GESTURE, nullptr);

  lv_obj_t *cover = lv_image_create(s);
  const lv_image_dsc_t *img = cover_of(r);
  lv_image_set_src(cover, img);
  lv_image_set_scale(cover, 150 * 256 / img->header.w);
  lv_obj_set_size(cover, 150, 150);
  lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_pos(cover, 16, 52);
  lv_obj_set_clickable(cover, true);
  lv_obj_add_event_cb(cover, back, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *close = button(s, "‹  Back", 110, 40);
  lv_obj_set_pos(close, 16, 6);
  lv_obj_add_event_cb(close, back, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *artist = label(s, &font_22, lv_color_white(), 282);
  lv_label_set_text(artist, r.artist.c_str());
  lv_obj_set_pos(artist, 182, 52);
  lv_obj_t *title = label(s, &font_16, DIM, 282);
  const String line = r.title + (r.year ? " · " + String(r.year) : "");
  lv_label_set_text(title, line.c_str());
  lv_obj_set_pos(title, 182, 82);
  view.value = label(s, &font_44, lv_color_white(), 282);
  lv_obj_set_pos(view.value, 182, 110);
  view.change = label(s, &font_16, DIM, 282);
  lv_obj_set_pos(view.change, 182, 164);

  view.chart = lv_chart_create(s);
  lv_obj_set_size(view.chart, 448, 60);
  lv_obj_set_pos(view.chart, 16, 212);
  lv_chart_set_type(view.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(view.chart, 0, 0);
  lv_obj_set_style_bg_opa(view.chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(view.chart, 0, 0);
  lv_obj_set_style_pad_all(view.chart, 4, 0);
  lv_obj_set_style_line_width(view.chart, 3, LV_PART_ITEMS);
  lv_obj_set_style_size(view.chart, 0, 0, LV_PART_INDICATOR);
  lv_chart_add_series(view.chart, ACCENT, LV_CHART_AXIS_PRIMARY_Y);

  lv_obj_t *media = label(s, &font_16, DIM, 448);
  lv_label_set_text(media, "Record grade · price at each grade");
  lv_obj_set_pos(media, 16, 280);
  chip_row(s, 302, 62, 0, view.media, true);
  lv_obj_t *sleeve = label(s, &font_16, DIM, 448);
  lv_label_set_text(sleeve, "Sleeve grade");
  lv_obj_set_pos(sleeve, 16, 374);
  chip_row(s, 396, 46, GRADE_COUNT, view.sleeve, false);
  view.status = label(s, &font_16, DIM, 448);
  lv_label_set_text(view.status, "");
  lv_obj_set_pos(view.status, 16, 452);

  paint_detail();
  show(s);
}

// ---- wheel -------------------------------------------------------------------------------------
//
// The covers are drawn by hand, not by LVGL: LVGL scales images at about 1 µs a pixel from PSRAM,
// 6 frames a second. Here each band of rows is built in internal RAM with nearest-neighbour
// scaling and copied to the panel in one go, so a frame costs a few milliseconds. LVGL keeps the
// rows above and below the band (status, Refresh, the text) and never draws inside it.

extern uint32_t profile_loop_us, profile_touch_us;  // main.cpp, board.cpp

constexpr int POOL = 7;                       // covers drawn: the front, three before and three after
constexpr int BAND_TOP = 56, BAND_BOTTOM = 290;  // the rows the wheel owns
constexpr int STRIP = 16;                     // rows built at a time (internal RAM, 15 KB)
static const uint16_t BG565 = 0x1082;         // BG as RGB565

static struct {
  float pos = 0;          // the record at the front, fractional while turning
  float target = NAN;     // where the wheel is settling, or NAN while a finger holds it
  float velocity = 0;     // records per second, from the last drag
  float moved = 0;        // finger travel this touch, to tell a tap from a drag
  float drawn_pos = NAN;  // pos at the last frame, to skip frames when nothing moved
  float speed = 0;        // records per second, smoothed, whoever is turning the wheel
  int repaint = 0;        // frames to draw regardless, e.g. after LVGL painted the whole screen
  int front = -1;
  uint32_t last_ms = 0, records_gen = 0, status_ms = 0, fps_ms = 0;
  int frames = 0, frame_us = 0, copy_us = 0;
  const lv_image_dsc_t *shown[POOL];
  uint16_t *strip;
  Span drawn[BAND_BOTTOM - BAND_TOP];  // per band row, the columns covers took last frame
  lv_obj_t *artist, *title, *value, *change, *count, *fps;
  lv_timer_t *timer;
  String count_text;
} wheel;

static void want_covers_around(int front) {
  const int n = records.size();
  int ids[9], count = 0;
  for (int k : {0, 1, -1, 2, -2, 3, -3, 4, -4}) {
    if (count >= n) break;
    ids[count++] = records[wheel_index(front + k, n)].id;
  }
  covers_want(ids, count);
  sync_hint_front(records[front].id);
}

static String status_text() {
  const SyncStatus s = sync_status();
  String text = String(wheel.front + 1) + " / " + String(records.size()) + "  ·  ";
  if (s.running) {
    text += String(s.step) + " " + String(s.done) + "/" + String(s.total);
  } else if (s.failed) {
    text += "offline";
  } else if (s.last_ok) {
    const long mins = (time(nullptr) - s.last_ok) / 60;
    text += mins < 1 ? "updated just now" : "updated " + String(mins) + " min ago";
  }
  return text;
}

struct Sprite {
  const uint16_t *px;
  int src_w, src_h, x0, y0, w, h, opa;
  int16_t xmap[(int)WHEEL_FRONT_PX + 1];  // destination column -> source column
};
static Sprite sprites[POOL];

// Darken towards the background for covers further round the rim.
static inline IRAM_ATTR uint16_t fade(uint16_t p, int opa) {
  const int r = (p >> 11) * opa >> 8, g = ((p >> 5) & 0x3F) * opa >> 8, b = (p & 0x1F) * opa >> 8;
  return r << 11 | g << 5 | b;
}

// Columns [x, end) of one sprite row.
static inline IRAM_ATTR void blit(uint16_t *dst, const uint16_t *src, const Sprite &s, int x, int end) {
  const int16_t *map = s.xmap - s.x0;
  if (s.opa >= 255) {
    for (; x < end; x++) dst[x] = src[map[x]];
  } else {
    for (; x < end; x++) dst[x] = fade(src[map[x]], s.opa);
  }
}

// In internal RAM, like blit and fade: run from flash, these loops wait on the same bus that
// brings the cover pixels in from PSRAM.
// One screen row: covers nearest first, each only where no nearer one has drawn, then the
// background in the gaps.
static IRAM_ATTR Span draw_row(uint16_t *dst, int y, int count) {
  Span cov[POOL + 1];  // drawn columns, sorted and merged
  int nc = 0;
  for (int j = 0; j < count; j++) {
    const Sprite &s = sprites[j];
    if (s.opa <= 0 || y < s.y0 || y >= s.y0 + s.h) continue;
    const int a = max(0, s.x0), b = min(BOARD_W, s.x0 + s.w);
    if (a >= b) continue;
    const uint16_t *src = s.px + ((y - s.y0) * s.src_h / s.h) * s.src_w;
    int x = a;
    for (int i = 0; i <= nc && x < b; i++) {
      const int end = i < nc ? min(b, cov[i].a) : b;
      if (end > x) blit(dst, src, s, x, end);
      if (i < nc) x = max(x, cov[i].b);
    }
    nc = spans_add(cov, nc, a, b);
  }
  static uint16_t bg[BOARD_W];  // one row of background, copied into the gaps
  if (bg[0] != BG565) for (uint16_t &p : bg) p = BG565;
  int x = 0;
  for (int i = 0; i < nc; i++) {
    memcpy(dst + x, bg, (cov[i].a - x) * 2);
    x = cov[i].b;
  }
  memcpy(dst + x, bg, (BOARD_W - x) * 2);
  return nc ? Span{cov[0].a, cov[nc - 1].b} : Span{0, 0};
}

static void render() {
  const uint32_t start = micros();
  const int n = records.size();
  const int base = (int)floorf(wheel.pos + 0.5f);
  // Furthest first, so nearer covers paint over them.
  int order[POOL], count = 0;
  for (int k = 0; k < POOL; k++) {
    const int off = k - POOL / 2;
    if (off < -(n - 1) / 2 || off > n / 2) continue;  // with only a few records, don't draw one twice
    order[count++] = k;
  }
  // Nearest first: each pixel is drawn once, by the nearest cover over it.
  std::sort(order, order + count, [&](int a, int b) {
    return fabsf(base + a - POOL / 2 - wheel.pos) < fabsf(base + b - POOL / 2 - wheel.pos);
  });
  for (int j = 0; j < count; j++) {
    const int i = base + order[j] - POOL / 2;
    const lv_image_dsc_t *img = wheel.shown[order[j]] = cover_of(records[wheel_index(i, n)]);
    const WheelSlot slot = wheel_slot(i - wheel.pos);
    Sprite &s = sprites[j];
    s.px = (const uint16_t *)img->data;
    s.src_w = img->header.w, s.src_h = img->header.h;
    const float k = slot.scale * WHEEL_FRONT_PX / max(s.src_w, s.src_h);
    s.w = max(1, (int)lroundf(s.src_w * k)), s.h = max(1, (int)lroundf(s.src_h * k));
    s.x0 = (int)lroundf(slot.x) - s.w / 2, s.y0 = (int)lroundf(slot.y) - s.h / 2;
    s.opa = slot.opa;
    for (int x = 0; x < s.w; x++) s.xmap[x] = x * s.src_w / s.w;
  }

  // Each row is copied to the panel only between the leftmost and rightmost column a cover covers
  // now or covered last frame: the background around them hasn't changed. Copying into PSRAM is
  // the slowest step, since the panel is reading the same memory all the time.
  const bool full = wheel.repaint > 0;
  for (int y = BAND_TOP; y < BAND_BOTTOM; y += STRIP) {
    const int rows = min(STRIP, BAND_BOTTOM - y);
    Span now[STRIP];
    for (int r = 0; r < rows; r++) now[r] = draw_row(wheel.strip + r * BOARD_W, y + r, count);
    const uint32_t copy = micros();
    for (int r = 0; r < rows; r++) {
      Span &before = wheel.drawn[y + r - BAND_TOP];
      const int lo = full ? 0 : min(now[r].a, before.a), hi = full ? BOARD_W : max(now[r].b, before.b);
      if (hi > lo) board_draw(lo, y + r, hi - lo, 1, wheel.strip + r * BOARD_W + lo);
      before = now[r].b > now[r].a ? now[r] : Span{BOARD_W, 0};
    }
    wheel.copy_us += micros() - copy;
  }
  wheel.drawn_pos = wheel.pos;
  wheel.frames++;
  wheel.frame_us += micros() - start;
}

static void update_front() {
  const int n = records.size();
  const int front = wheel_index((int)floorf(wheel.pos + 0.5f), n);
  if (front == wheel.front) return;
  // The text only changes once the wheel is slow enough to read it: redrawing four labels for
  // every record that flies past costs more than drawing the covers does.
  if (wheel.front >= 0 && wheel.speed > 2.5f) return;
  wheel.front = front;
  const Record &r = records[front];
  lv_label_set_text(wheel.artist, r.artist.c_str());
  lv_label_set_text(wheel.title, r.title.c_str());
  lv_label_set_text(wheel.value, r.value.isEmpty() ? "Not priced" : (r.value + " · " + r.media).c_str());
  set_change(wheel.change, r.change30);
  wheel.status_ms = 0;
  want_covers_around(front);
}

// A cover that was loading has arrived for one of the drawn slots.
static bool covers_arrived() {
  const int n = records.size();
  const int base = (int)floorf(wheel.pos + 0.5f);
  for (int k = 0; k < POOL; k++) {
    if (cover_of(records[wheel_index(base + k - POOL / 2, n)]) != wheel.shown[k]) return true;
  }
  return false;
}

// The sync task replaced the list: reload it, keeping the same record at the front.
static void reload_records() {
  const int id = records.empty() ? -1 : records[wheel.front].id;
  if (!load_collection()) return;
  int at = 0;
  for (size_t i = 0; i < records.size(); i++) if (records[i].id == id) at = i;
  wheel.pos = wheel.target = at;
  wheel.front = -1;
  wheel.repaint = 1;
}

// Every frame: ease towards the target, pick up new data and covers, draw if anything changed.
static void tick(lv_timer_t *) {
  const uint32_t now = millis();
  const float dt = min(0.05f, (now - wheel.last_ms) / 1000.0f);
  wheel.last_ms = now;
#ifdef WHEEL_BENCH
  // Benchmark build: the wheel spins by itself, 4 records a second, for 12 s after it opens.
  static uint32_t bench_until = now + 12000;
  if (now < bench_until) wheel.pos += 4 * dt, wheel.target = NAN;
  else if (isnan(wheel.target)) wheel.target = roundf(wheel.pos);
#endif
  if (!isnan(wheel.target) && wheel.pos != wheel.target) {
    const float gap = wheel.target - wheel.pos;
    wheel.pos = fabsf(gap) < 0.002f ? wheel.target : wheel.pos + gap * fminf(1.0f, dt * 7.0f);
  }
  static float last_pos = wheel.pos;
  if (dt > 0) wheel.speed = 0.8f * wheel.speed + 0.2f * fabsf(wheel.pos - last_pos) / dt;
  last_pos = wheel.pos;

  const SyncStatus s = sync_status();
  if (s.unlinked) {
    lv_timer_pause(wheel.timer);  // the screen and its timer go once the link screen is up
    return (void)unlinked(401);
  }
  if (s.records_gen != wheel.records_gen) {
    wheel.records_gen = s.records_gen;
    reload_records();
  }
  update_front();
  if (wheel.repaint > 0 || wheel.pos != wheel.drawn_pos || covers_arrived()) {
    if (wheel.repaint > 0) wheel.repaint--;
    render();
  }

  if (now - wheel.status_ms > 1000) {
    wheel.status_ms = now;
    const String text = status_text();
    if (text != wheel.count_text) lv_label_set_text(wheel.count, (wheel.count_text = text).c_str());
  }
  if (now - wheel.fps_ms >= 1000) {
    const int f = max(1, wheel.frames);
    const int ms = wheel.frame_us / f / 1000;
    lv_label_set_text_fmt(wheel.fps, "%d fps · %d ms", wheel.frames, ms);
    // Profiling: where the last second went. loop is all of lv_timer_handler, wheel frames
    // included; touch is the GT911 reads inside it.
    if (wheel.frames) {
      Serial.printf("{\"event\":\"profile\",\"fps\":%d,\"draw_ms\":%.1f,\"copy_ms\":%.1f,\"busy_pct\":%u,"
                    "\"lvgl_ms\":%u,\"touch_ms\":%u}\n",
                    wheel.frames, (wheel.frame_us - wheel.copy_us) / 1000.0f / f, wheel.copy_us / 1000.0f / f,
                    profile_loop_us / ((now - wheel.fps_ms) * 10), (profile_loop_us - wheel.frame_us) / 1000,
                    profile_touch_us / 1000);
    }
    wheel.frames = wheel.frame_us = wheel.copy_us = 0;
    profile_loop_us = profile_touch_us = 0;
    wheel.fps_ms = now;
  }
}

static void touched(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    wheel.target = NAN;
    wheel.velocity = 0;
    wheel.moved = 0;
  } else if (code == LV_EVENT_PRESSING) {
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    const float step = -v.x / WHEEL_DRAG_PX;
    wheel.pos += step;
    wheel.moved += abs(v.x) + abs(v.y);
    wheel.velocity = 0.7f * wheel.velocity + 0.3f * (step / 0.016f);
  } else if (code == LV_EVENT_RELEASED) {
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const float base = roundf(wheel.pos);
    const bool in_band = p.y > BAND_TOP && p.y < BAND_BOTTOM;
    if (wheel.moved > 12) {
      // A fling carries on: land where the throw would take it, at most 20 records away.
      wheel.target = roundf(wheel.pos + fmaxf(-20.0f, fminf(20.0f, wheel.velocity * 0.35f)));
    } else if (in_band && p.x < WHEEL_CX - WHEEL_FRONT_PX / 2) {
      wheel.target = base - 1;
    } else if (in_band && p.x > WHEEL_CX + WHEEL_FRONT_PX / 2) {
      wheel.target = base + 1;
    } else if (in_band && fabsf(wheel.pos - base) < 0.05f) {
      wheel.target = base;
      open_record(wheel.front);
    } else {
      wheel.target = base;
    }
  }
}

static void open_wheel() {
  lv_obj_t *s = screen();
  wheel.front = -1;
  wheel.count_text = "";
  wheel.records_gen = sync_status().records_gen;
  // LVGL paints the whole new screen, band included, on its next refresh: draw over it after.
  wheel.repaint = 3;
  if (!wheel.strip) wheel.strip = (uint16_t *)heap_caps_malloc(BOARD_W * STRIP * 2, MALLOC_CAP_INTERNAL);
  if (isnan(wheel.target)) wheel.target = roundf(wheel.pos);
  for (auto &shown : wheel.shown) shown = nullptr;

  wheel.count = label(s, &font_16, DIM, 330);
  lv_obj_set_pos(wheel.count, 16, 18);
  lv_obj_t *refresh = button(s, "Refresh", 100, 40);
  lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_add_event_cb(refresh, [](lv_event_t *) {
    sync_now();
    wheel.status_ms = 0;
  }, LV_EVENT_CLICKED, nullptr);

  wheel.artist = label(s, &font_22, lv_color_white(), 420);
  wheel.title = label(s, &font_16, DIM, 420);
  wheel.value = label(s, &font_30, ACCENT, 420);
  wheel.change = label(s, &font_16, DIM, 420);
  int y = 300;
  for (lv_obj_t *l : {wheel.artist, wheel.title, wheel.value, wheel.change}) {
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    y += lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)) + 6;
  }
  wheel.fps = label(s, &font_16, DIM, 200);  // the frame-rate readout, for tuning
  lv_obj_align(wheel.fps, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
  lv_obj_set_style_text_align(wheel.fps, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_color(wheel.fps, lv_color_hex(0x5A5A5A), 0);

  for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED}) {
    lv_obj_add_event_cb(s, touched, code, nullptr);
  }
  // The timer belongs to the wheel screen and goes when it does.
  wheel.last_ms = wheel.fps_ms = millis();
  wheel.timer = lv_timer_create(tick, 16, nullptr);
  lv_obj_add_event_cb(s, [](lv_event_t *) { lv_timer_delete(wheel.timer); }, LV_EVENT_DELETE, nullptr);
  update_front();
  show(s);
}

// ---- linking -----------------------------------------------------------------------------------

static LinkCode link_code;

static void open_link() {
  message("Getting a code…");
  const int status = vault_start_link(link_code);
  if (status != 200) {
    return problem(status == 429 ? "Too many screens are waiting to link.\nTap to try again in a few minutes."
                                 : "Can't reach the vault.\nTap to try again, or hold to change Wi-Fi.",
                   open_link);
  }
  lv_obj_t *s = screen();
  lv_obj_t *head = label(s, &font_30, lv_color_white(), 448);
  lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(head, "Link this screen");
  lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *qr = lv_qrcode_create(s);
  lv_qrcode_set_size(qr, 200);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  lv_qrcode_update(qr, link_code.url.c_str(), link_code.url.length());
  lv_obj_set_style_border_color(qr, lv_color_white(), 0);
  lv_obj_set_style_border_width(qr, 8, 0);
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 72);

  lv_obj_t *code = label(s, &font_44, ACCENT, 448);
  lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(code, link_code.user_code.c_str());
  lv_obj_align(code, LV_ALIGN_TOP_MID, 0, 300);

  lv_obj_t *hint = label(s, &font_16, DIM, 420);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_height(hint, LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(hint, "Scan with your phone and approve, or open\nvault.jakubszypicyn.com/link and type the code.");
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 370);

  lv_timer_t *poll = lv_timer_create([](lv_timer_t *t) {
    switch (vault_poll_link(link_code)) {
      case LinkState::Pending:
      case LinkState::Error: return;  // a dropped request is retried on the next tick
      // The screen's DELETE event deletes the timer; pause it until the screen goes.
      case LinkState::Linked: lv_timer_pause(t); sync_now(); start(); return;
      case LinkState::Expired: lv_timer_pause(t); open_link(); return;
    }
  }, link_code.interval * 1000, nullptr);
  lv_obj_add_event_cb(s, [](lv_event_t *e) { lv_timer_delete((lv_timer_t *)lv_event_get_user_data(e)); },
                      LV_EVENT_DELETE, poll);
  show(s);
}

// ---- Wi-Fi -------------------------------------------------------------------------------------

static String wifi_ssid;
static void open_password(const char *error);

static void open_wifi() {
  message("Looking for Wi-Fi networks…");
  const std::vector<String> networks = wifi_scan();
  lv_obj_t *s = screen();
  lv_obj_t *head = label(s, &font_30, lv_color_white(), 448);
  lv_label_set_text(head, "Choose Wi-Fi");
  lv_obj_set_pos(head, 16, 16);

  lv_obj_t *list = lv_obj_create(s);
  lv_obj_set_size(list, 448, 330);
  lv_obj_set_pos(list, 16, 64);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  for (const String &ssid : networks) {
    lv_obj_t *b = button(list, ssid.c_str(), 448, 52);
    lv_obj_t *t = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_font(t, &font_22, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      wifi_ssid = lv_label_get_text(lv_obj_get_child(lv_event_get_target_obj(e), 0));
      open_password(nullptr);
    }, LV_EVENT_CLICKED, nullptr);
  }
  if (networks.empty()) {
    lv_obj_t *none = label(list, &font_16, DIM, 448);
    lv_label_set_long_mode(none, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(none, LV_SIZE_CONTENT);
    lv_label_set_text(none, "No networks found. The screen only sees 2.4 GHz Wi-Fi.");
  }
  lv_obj_t *again = button(s, "Scan again", 448, 56);
  lv_obj_set_pos(again, 16, 408);
  lv_obj_add_event_cb(again, [](lv_event_t *) { open_wifi(); }, LV_EVENT_CLICKED, nullptr);
  show(s);
}

// The keyboard's tick (READY) joins; its keyboard key (CANCEL) goes back to the list.
static void keyboard_done(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CANCEL) return open_wifi();
  const String pass = lv_textarea_get_text(lv_keyboard_get_textarea(lv_event_get_target_obj(e)));
  message(("Joining " + wifi_ssid + "…").c_str());
  if (wifi_connect(wifi_ssid, pass)) return start();
  open_password("Couldn't join. Check the password and try again.");
}

static void open_password(const char *error) {
  lv_obj_t *s = screen();
  lv_obj_t *head = label(s, &font_22, lv_color_white(), 448);
  lv_label_set_text_fmt(head, "Password for %s", wifi_ssid.c_str());
  lv_obj_set_pos(head, 16, 16);
  lv_obj_t *note = label(s, &font_16, error ? DOWN : DIM, 448);
  lv_label_set_text(note, error ? error : "Tick to join, or the keyboard key to go back to the list.");
  lv_obj_set_pos(note, 16, 48);

  lv_obj_t *field = lv_textarea_create(s);
  lv_textarea_set_one_line(field, true);
  lv_textarea_set_password_mode(field, true);  // each letter shows for a moment, then hides
  lv_obj_set_size(field, 448, 56);
  lv_obj_set_pos(field, 16, 76);
  lv_obj_set_style_bg_color(field, CHIP, 0);
  lv_obj_set_style_border_width(field, 0, 0);
  lv_obj_add_state(field, LV_STATE_FOCUSED);

  lv_obj_t *kb = lv_keyboard_create(s);
  lv_obj_set_size(kb, 480, 330);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, 0);
  lv_keyboard_set_textarea(kb, field);
  lv_obj_add_event_cb(kb, keyboard_done, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, keyboard_done, LV_EVENT_CANCEL, nullptr);
  show(s);
}

// ---- start -------------------------------------------------------------------------------------

static void start() {
  if (!vault_linked()) return open_link();
  if (load_collection()) {
    wheel.pos = fminf(wheel.pos, records.size() - 1);
    wheel.target = NAN;
    return open_wheel();
  }
  // Nothing on the board yet: wait for the first sync to bring the record list.
  sync_now();
  lv_obj_t *s = message("Getting your collection…");
  lv_timer_t *wait = lv_timer_create([](lv_timer_t *t) {
    const SyncStatus s = sync_status();
    if (s.unlinked) return lv_timer_pause(t), (void)unlinked(401);
    if (s.records_gen > 0) return lv_timer_pause(t), start();
    if (s.failed) return lv_timer_pause(t), problem("Can't reach the vault.\nTap to try again, or hold to change Wi-Fi.", start);
  }, 500, nullptr);
  lv_obj_add_event_cb(s, [](lv_event_t *e) { lv_timer_delete((lv_timer_t *)lv_event_get_user_data(e)); },
                      LV_EVENT_DELETE, wait);
}

void ui_begin() {
  draw_vinyl();
  String ssid;
  if (!wifi_saved(ssid)) return open_wifi();
  message(("Joining " + ssid + "…").c_str());
  if (wifi_reconnect()) return start();
  // Offline: show what the board has, if anything; the sync task retries when Wi-Fi returns.
  if (vault_linked() && load_collection()) return open_wheel();
  problem(("Couldn't join " + ssid + ".\nTap to try again, or hold to choose another network.").c_str(),
          [] { if (wifi_reconnect()) start(); else open_wifi(); });
}
