// The screens: a message (Wi-Fi, errors), Wi-Fi setup, the link screen (QR + code), the wheel of
// covers and one record with its grades. Everything shown comes from the board's own copy of the
// collection (store.h), kept fresh by the sync task, so nothing here waits for the network except
// linking and joining Wi-Fi. A new grade goes to the vault from the sync task.
#include "ui.h"

#include <Arduino.h>

#include "board.h"
#include "covers.h"
#include "nfc.h"
#include "store.h"
#include "sync.h"
#include "vault.h"
#include "record_art.h"
#include "wheel.h"

LV_FONT_DECLARE(font_16);
LV_FONT_DECLARE(font_22);
LV_FONT_DECLARE(font_30);
LV_FONT_DECLARE(font_44);
LV_FONT_DECLARE(font_mono_14);

// The palette of jakubszypicyn.com, dark. Rules are pre-blended (the site's 25% and 12% white on BG):
// LVGL blends opacity pixel by pixel in software.
static const lv_color_t BG = lv_color_hex(0x0B0F0C), CHIP = lv_color_hex(0x181E1B), TEXT = lv_color_hex(0xF3F2EC),
                        BODY = lv_color_hex(0xC5C7C1), DIM = lv_color_hex(0x9A9E98), RULE = lv_color_hex(0x454844),
                        RULE_SOFT = lv_color_hex(0x262A26), METAL = lv_color_hex(0xA9ADA7), PART = lv_color_hex(0x3C403A),
                        ACCENT = lv_color_hex(0x2FF27C), EYEBROW = lv_color_hex(0x1CB35A), UP = lv_color_hex(0x5FD98F),
                        DOWN = lv_color_hex(0xEF7FA3);

static std::vector<Record> records;
static std::map<int, Detail> details;  // each record's history and prices, by id

// The board's copy of the collection, into RAM, and the sync generation it came from.
static uint32_t loaded_gen;
static bool load_collection() {
  loaded_gen = sync_status().records_gen;
  return store_load_records(records) && (store_load_details(details), true);
}
static void start();
static void open_wifi();
static void open_link();
static void open_wheel();
static void open_clock();

// The crate and a record's page give way to the clock when left alone; a tap on it calls `back`.
// Cleared when that screen goes, so a new screen at the same address is never mistaken for it.
static lv_obj_t *idle_screen;
static void (*resume)();
static void idle_here(lv_obj_t *s, void (*back)()) {
  idle_screen = s;
  resume = back;
  lv_obj_add_event_cb(s, [](lv_event_t *e) {
    if (lv_event_get_target_obj(e) == idle_screen) idle_screen = nullptr;
  }, LV_EVENT_DELETE, nullptr);
}

// ---- shared bits ---------------------------------------------------------------------------

static lv_obj_t *screen() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, BG, 0);
  lv_obj_set_style_text_color(s, TEXT, 0);
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

// An outline pill, filled while pressed.
static lv_obj_t *button(lv_obj_t *parent, const char *text, int32_t w, int32_t h) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(b, CHIP, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, RULE, 0);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *t = lv_label_create(b);
  lv_obj_set_style_text_font(t, &font_16, 0);
  lv_label_set_text(t, text);
  lv_obj_center(t);
  return b;
}

// The green line above a heading, as on the site.
static lv_obj_t *eyebrow(lv_obj_t *parent, const char *text) {
  lv_obj_t *l = label(parent, &font_16, EYEBROW, 300);
  lv_label_set_text(l, text);
  lv_obj_set_pos(l, 16, 18);
  return l;
}

static lv_obj_t *rule(lv_obj_t *parent, int32_t y, lv_color_t color) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, 448, 1);
  lv_obj_set_pos(r, 16, y);
  lv_obj_set_style_bg_color(r, color, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  return r;
}

// A mono readout ("33.3 rpm", "012 / 161"), at the left of the screen or, if right, its right.
static lv_obj_t *readout(lv_obj_t *parent, int32_t y, bool right, const char *text) {
  lv_obj_t *l = label(parent, &font_mono_14, DIM, 220);
  lv_label_set_text(l, text);
  if (right) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(l, right ? 244 : 16, y);
  return l;
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

static lv_image_dsc_t vinyl, disc, platter;  // records: the wheel's placeholder, the record screen's, the big one

static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *l = label(parent, font, color, 448);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_height(l, LV_SIZE_CONTENT);
  lv_label_set_text(l, text);
  return l;
}

// A filled circle centred on (x, y).
static lv_obj_t *dot(lv_obj_t *parent, int32_t x, int32_t y, int32_t size, lv_color_t color) {
  lv_obj_t *d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, size, size);
  lv_obj_set_pos(d, x - size / 2, y - size / 2);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, color, 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  return d;
}

// The turntable from jakubszypicyn.com/vinyls: the record and its arm, needle down, or lifted
// when something is wrong. It never turns, so it costs nothing after it is first drawn.
static void turntable(lv_obj_t *s, bool playing) {
  lv_obj_t *rec = lv_image_create(s);
  lv_image_set_src(rec, &platter);
  lv_obj_set_pos(rec, 100, 20);  // centre (220, 140), radius 120
  static const lv_point_precise_t down[] = {{408, 28}, {318, 206}}, up[] = {{408, 28}, {428, 230}};
  const lv_point_precise_t *at = playing ? down : up;
  lv_obj_t *arm = lv_line_create(s);
  lv_line_set_points(arm, at, 2);
  lv_obj_set_style_line_width(arm, 4, 0);
  lv_obj_set_style_line_color(arm, METAL, 0);
  lv_obj_set_style_line_rounded(arm, true, 0);
  lv_obj_t *pivot = dot(s, 408, 28, 22, PART);
  lv_obj_set_style_border_color(pivot, METAL, 0);
  lv_obj_set_style_border_width(pivot, 2, 0);
  dot(s, at[1].x, at[1].y, 10, ACCENT);  // the stylus
}

// The first line is the heading, the rest goes under it.
static lv_obj_t *message(const char *text, bool trouble = false) {
  lv_obj_t *s = screen();
  turntable(s, !trouble);
  rule(s, 292, RULE);
  readout(s, 300, false, trouble ? "00.0 rpm" : "33.3 rpm");
  lv_obj_t *state = readout(s, 300, true, trouble ? "offline" : "power on");
  if (trouble) lv_obj_set_style_text_color(state, DOWN, 0);
  const char *nl = strchr(text, '\n');
  lv_obj_t *head = wrapped(s, &font_30, TEXT, nl ? String(text).substring(0, nl - text).c_str() : text);
  lv_obj_set_pos(head, 16, 334);
  if (nl) lv_obj_align_to(wrapped(s, &font_16, BODY, nl + 1), head, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
  show(s);
  lv_refr_now(nullptr);  // the caller may be about to block on the network; draw this first
  return s;
}

// A message that retries on a tap and offers other Wi-Fi on a long press.
static void problem(const char *text, void (*retry)()) {
  lv_obj_t *s = message(text, true);
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

// ---- the records, drawn once at boot ---------------------------------------------------------

// An n x n RGB565 record in PSRAM, inset by `margin` on a square of `outside`. No alpha and drawn at
// its own size, so LVGL only copies it.
static void draw_record(lv_image_dsc_t &img, int n, int margin, uint16_t outside) {
  uint16_t *px = (uint16_t *)ps_malloc(n * n * 2);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) px[y * n + x] = record_px(x - margin, y - margin, n - 2 * margin, outside);
  }
  img.header.magic = LV_IMAGE_HEADER_MAGIC;
  img.header.cf = LV_COLOR_FORMAT_RGB565;
  img.header.w = img.header.h = n;
  img.header.stride = n * 2;
  img.data_size = n * n * 2;
  img.data = (const uint8_t *)px;
}

static const lv_image_dsc_t *cover_of(const Record &r) {
  const lv_image_dsc_t *c = covers_get(r.id);
  return c ? c : &vinyl;
}

// "012 / 161": the record's place in the crate, padded to the width of the count, as on the site.
static String count_text(int index) {
  const int n = records.size();
  char text[24];
  snprintf(text, sizeof text, "%0*d / %d", (int)String(n).length(), index + 1, n);
  return text;
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

  // The chosen grade is solid green with dark text.
  auto mark = [](lv_obj_t *chip, bool on) {
    lv_obj_set_style_bg_color(chip, on ? ACCENT : CHIP, 0);
    lv_obj_set_style_text_color(chip, on ? BG : TEXT, 0);
    if (lv_obj_get_child_count(chip) > 1) lv_obj_set_style_text_color(lv_obj_get_child(chip, 1), on ? BG : DIM, 0);
  };
  for (int g = 0; g < GRADE_COUNT; g++) {
    mark(view.media[g], r.media == GRADES[g]);
    mark(view.sleeve[g], r.sleeve == GRADES[g]);
    lv_label_set_text(lv_obj_get_child(view.media[g], 1), money(view.detail.price_by_grade[g]).c_str());
  }
}

static void set_status(const char *text, lv_color_t color) {
  lv_label_set_text(view.status, text);
  lv_obj_set_style_text_color(view.status, color, 0);
}

// A new grade shows at once and goes to the vault from the sync task; a new record grade re-prices
// the record there. If the vault says no, the old grade comes back. One save at a time.
static struct {
  int id = -1;  // the record being saved, or -1
  bool media;
  String was;   // its grade before the tap
  uint32_t at;  // millis() at the tap
} saving;

static Record *record_by_id(int id) {
  for (Record &r : records) if (r.id == id) return &r;
  return nullptr;
}

static void grade_saved(lv_timer_t *t) {
  int status;
  Detail d;
  if (!sync_grade_done(status, d)) return;
  lv_timer_delete(t);
  const int id = saving.id;
  saving.id = -1;
  Serial.printf("{\"event\":\"grade_saved\",\"id\":%d,\"status\":%d,\"ms\":%lu}\n", id, status, millis() - saving.at);
  if (unlinked(status)) return;
  Record *r = record_by_id(id);
  // The record's page may have gone (or come back) while the vault answered.
  const bool shown = view.status && records[view.index].id == id;
  if (!r) return;
  if (status != 200) {
    (saving.media ? r->media : r->sleeve) = saving.was;
    if (shown) set_status("Couldn't save. Tap the grade to try again.", DOWN), paint_detail();
    return;
  }
  r->value = d.value;
  r->media = d.media;
  r->sleeve = d.sleeve;
  details[id] = d;  // not on flash: writing it stalls the panel, and the next sync stores it
  if (!shown) return;
  view.detail = d;
  set_status("Saved.", DIM);
  paint_detail();
}

static void save_grade(Record &r, bool media, const char *grade) {
  String &now = media ? r.media : r.sleeve;
  saving = {r.id, media, now, millis()};
  now = grade;
  sync_set_grade(r.id, media ? "media_condition" : "sleeve_condition", grade, view.detail);
  lv_timer_create(grade_saved, 50, nullptr);
  set_status("Saving…", DIM);
  paint_detail();
}

static void grade_tapped(lv_event_t *e) {
  const int code = (int)(intptr_t)lv_event_get_user_data(e);  // 0-7 record, 8-15 sleeve
  const bool media = code < GRADE_COUNT;
  const char *grade = GRADES[code % GRADE_COUNT];
  Record &r = records[view.index];
  if (saving.id < 0 && (media ? r.media : r.sleeve) != grade) save_grade(r, media, grade);
}

#ifdef GRADE_BENCH
static void open_record(int index);
// Benchmark build: opens a record and saves its sleeve grade as it is, so the vault keeps what it
// had: as the page opens, on a warm connection, during a sync's prices and just after it. Look for
// grade_saved.
static void grade_bench(lv_timer_t *) {
  static const float at[] = {0.5, 8, 23, 50};  // s after the page opens; a sync starts at 20 s
  static uint32_t opened;
  static size_t next;
  if (!opened) {
    if (records.empty() || sync_status().running || lv_screen_active() != idle_screen) return;
    for (size_t i = 0; i < records.size() && !opened; i++) {
      for (int g = 0; g < GRADE_COUNT && !opened; g++) if (records[i].sleeve == GRADES[g]) open_record(i), opened = millis();
    }
    return;
  }
  lv_display_trigger_activity(nullptr);  // stay on the page, not the clock
  if (next == 2 && millis() - opened > 20000 && !sync_status().running) sync_now();
  if (next >= sizeof at / sizeof *at || millis() - opened < at[next] * 1000 || saving.id >= 0) return;
  Record &r = records[view.index];
  for (int g = 0; g < GRADE_COUNT; g++) if (r.sleeve == GRADES[g]) save_grade(r, false, GRADES[g]);
  Serial.printf("{\"event\":\"grade_bench\",\"step\":%u}\n", next++);
}
#endif

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
      lv_obj_set_style_text_font(price, &font_mono_14, 0);
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
  idle_here(s, [] { open_record(view.index); });
  auto back = [](lv_event_t *) { open_wheel(); };
  lv_obj_add_event_cb(s, [](lv_event_t *) {
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) open_wheel();
  }, LV_EVENT_GESTURE, nullptr);

  lv_obj_t *close = button(s, "‹", 40, 40);
  lv_obj_set_style_text_font(lv_obj_get_child(close, 0), &font_22, 0);
  lv_obj_set_pos(close, 16, 8);
  lv_obj_add_event_cb(close, back, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_x(eyebrow(s, "Now playing"), 68);
  readout(s, 19, true, count_text(index).c_str());

  // The record slides out of its sleeve: the disc first, the cover over it.
  lv_obj_t *record = lv_image_create(s);
  lv_image_set_src(record, &disc);
  lv_obj_set_pos(record, 134, 64);
  lv_obj_t *cover = lv_image_create(s);
  const lv_image_dsc_t *img = cover_of(r);
  lv_image_set_src(cover, img);
  lv_image_set_scale(cover, 144 * 256 / img->header.w);
  lv_obj_set_size(cover, 144, 144);
  lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_pos(cover, 16, 64);
  lv_obj_set_clickable(cover, true);
  lv_obj_add_event_cb(cover, back, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *artist = label(s, &font_22, TEXT, 174);
  lv_label_set_text(artist, r.artist.c_str());
  lv_obj_set_pos(artist, 290, 66);
  lv_obj_t *title = label(s, &font_16, DIM, 174);
  const String line = r.title + (r.year ? " · " + String(r.year) : "");
  lv_label_set_text(title, line.c_str());
  lv_obj_set_pos(title, 290, 94);
  view.value = label(s, &font_44, TEXT, 174);
  lv_obj_set_pos(view.value, 290, 120);
  view.change = label(s, &font_16, DIM, 174);
  lv_obj_set_pos(view.change, 290, 178);

  view.chart = lv_chart_create(s);
  lv_obj_set_size(view.chart, 448, 74);
  lv_obj_set_pos(view.chart, 16, 216);
  lv_chart_set_type(view.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(view.chart, 0, 0);
  lv_obj_set_style_bg_opa(view.chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(view.chart, 0, 0);
  lv_obj_set_style_pad_all(view.chart, 4, 0);
  lv_obj_set_style_line_width(view.chart, 3, LV_PART_ITEMS);
  lv_obj_set_style_size(view.chart, 0, 0, LV_PART_INDICATOR);
  lv_chart_add_series(view.chart, ACCENT, LV_CHART_AXIS_PRIMARY_Y);
  rule(s, 290, RULE_SOFT);

  lv_obj_t *media = label(s, &font_16, DIM, 448);
  lv_label_set_text(media, "Record grade · price at each grade");
  lv_obj_set_pos(media, 16, 298);
  chip_row(s, 320, 62, 0, view.media, true);
  lv_obj_t *sleeve = label(s, &font_16, DIM, 448);
  lv_label_set_text(sleeve, "Sleeve grade");
  lv_obj_set_pos(sleeve, 16, 390);
  chip_row(s, 412, 46, GRADE_COUNT, view.sleeve, false);
  view.status = label(s, &font_mono_14, DIM, 448);
  lv_label_set_text(view.status, "");
  lv_obj_set_pos(view.status, 16, 462);
  lv_obj_add_event_cb(view.status, [](lv_event_t *e) {
    if (lv_event_get_target_obj(e) == view.status) view.status = nullptr, sync_keep_warm(false);
  }, LV_EVENT_DELETE, nullptr);
  sync_keep_warm(true);  // the page's first grade then saves in about 0.1 s, not a handshake's 2 s

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
static const uint16_t BG565 = 0x0861;         // BG as RGB565

static struct {
  float pos = 0;          // the record at the front, fractional while turning
  float target = NAN;     // where the wheel is settling, or NAN while a finger holds it
  float velocity = 0;     // records per second, from the last drag
  float moved = 0;        // finger travel this touch, to tell a tap from a drag
  float drawn_pos = NAN;  // pos at the last frame, to skip frames when nothing moved
  float speed = 0;        // records per second, smoothed, whoever is turning the wheel
  int repaint = 0;        // frames to draw regardless, e.g. after LVGL painted the whole screen
  int front = -1;
  uint32_t last_ms = 0, status_ms = 0, fps_ms = 0;
  int frames = 0, frame_us = 0, copy_us = 0;
  const lv_image_dsc_t *shown[POOL];
  uint16_t *strip;
  Span drawn[BAND_BOTTOM - BAND_TOP];  // per band row, the columns covers took last frame
  lv_obj_t *artist, *title, *value, *grade, *change, *track, *needle, *sync, *count, *fps;
  lv_timer_t *timer;
  String sync_text;
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
  String text;
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
  lv_label_set_text(wheel.value, r.value.isEmpty() ? "Not priced" : r.value.c_str());
  lv_label_set_text(wheel.grade, r.media.c_str());
  lv_obj_set_hidden(wheel.grade, r.media.isEmpty());
  set_change(wheel.change, r.change30);
  // The needle tracks across the side: how far through the crate the front record is.
  const int x = 16 + 448 * front / max(1, n - 1);
  lv_obj_set_width(wheel.track, x - 16);
  lv_obj_set_x(wheel.needle, x - 4);
  lv_label_set_text(wheel.count, count_text(front).c_str());
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
  const int id = records.empty() ? -1 : records[wheel_index((int)roundf(wheel.pos), records.size())].id;
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
  if (s.records_gen != loaded_gen) reload_records();
  update_front();
  if (wheel.repaint > 0 || wheel.pos != wheel.drawn_pos || covers_arrived()) {
    if (wheel.repaint > 0) wheel.repaint--;
    render();
  }

  if (now - wheel.status_ms > 1000) {
    wheel.status_ms = now;
    const String text = status_text();
    if (text != wheel.sync_text) lv_label_set_text(wheel.sync, (wheel.sync_text = text).c_str());
  }
  if (now - wheel.fps_ms >= 1000) {
    const int f = max(1, wheel.frames);
#ifdef WHEEL_BENCH
    lv_label_set_text_fmt(wheel.fps, "%d fps · %d ms", wheel.frames, wheel.frame_us / f / 1000);
#endif
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
  idle_here(s, open_wheel);
  wheel.front = -1;
  wheel.sync_text = "";
  // LVGL paints the whole new screen, band included, on its next refresh: draw over it after.
  wheel.repaint = 3;
  if (!wheel.strip) wheel.strip = (uint16_t *)heap_caps_malloc(BOARD_W * STRIP * 2, MALLOC_CAP_INTERNAL);
  if (isnan(wheel.target)) wheel.target = roundf(wheel.pos);
  for (auto &shown : wheel.shown) shown = nullptr;

  eyebrow(s, "The crate");
  lv_obj_t *refresh = button(s, LV_SYMBOL_REFRESH, 40, 40);
  lv_obj_set_style_text_font(lv_obj_get_child(refresh, 0), &lv_font_montserrat_20, 0);
  lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -16, 8);
  lv_obj_add_event_cb(refresh, [](lv_event_t *) {
    sync_now();
    wheel.status_ms = 0;
  }, LV_EVENT_CLICKED, nullptr);

  // Below the band, from y 300: nothing here may reach into the rows the wheel draws.
  wheel.artist = label(s, &font_30, TEXT, 448);
  lv_obj_set_pos(wheel.artist, 16, 300);
  wheel.title = label(s, &font_16, DIM, 448);
  lv_obj_set_pos(wheel.title, 16, 338);
  lv_obj_t *row = lv_obj_create(s);  // the price, then the record's grade in an outline pill
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 448, 35);
  lv_obj_set_pos(row, 16, 364);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 12, 0);
  wheel.value = label(row, &font_30, ACCENT, LV_SIZE_CONTENT);
  wheel.grade = label(row, &font_16, BODY, LV_SIZE_CONTENT);
  lv_obj_set_height(wheel.grade, LV_SIZE_CONTENT);
  lv_obj_set_style_border_width(wheel.grade, 1, 0);
  lv_obj_set_style_border_color(wheel.grade, RULE, 0);
  lv_obj_set_style_radius(wheel.grade, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(wheel.grade, 10, 0);
  lv_obj_set_style_pad_ver(wheel.grade, 3, 0);
  wheel.change = label(s, &font_16, DIM, 448);
  lv_obj_set_pos(wheel.change, 16, 404);

  rule(s, 440, RULE);
  wheel.track = rule(s, 439, ACCENT);
  lv_obj_set_height(wheel.track, 3);
  wheel.needle = dot(s, 16, 440, 9, ACCENT);
  wheel.sync = readout(s, 452, false, "");
  wheel.count = readout(s, 452, true, "");
#ifdef WHEEL_BENCH
  wheel.fps = label(s, &font_mono_14, DIM, 200);  // the frame-rate readout, for tuning
  lv_obj_align(wheel.fps, LV_ALIGN_TOP_MID, 0, 19);
  lv_obj_set_style_text_align(wheel.fps, LV_TEXT_ALIGN_CENTER, 0);
#endif

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

// ---- clock -------------------------------------------------------------------------------------
//
// Left alone for two minutes, the crate or a record's page becomes a clock drawn on the record:
// two hands, the stylus going round the groove for the seconds, and the date. A tap goes back.

constexpr int DIAL_CX = 240, DIAL_CY = 210, DIAL_R = 200;
constexpr uint32_t IDLE_MS = 2 * 60 * 1000;
static lv_image_dsc_t face;  // drawn the first time the clock opens, not at boot

static struct {
  lv_obj_t *hour, *minute, *second, *date, *year;
  lv_point_precise_t hour_at[2], minute_at[2];
  int minute_shown;
} dial;

// The point `turns` of the way round from 12 o'clock, r from the centre.
static lv_point_precise_t on_dial(float turns, float r) {
  const float a = turns * 2 * PI;
  return {(lv_value_precise_t)lroundf(DIAL_CX + r * sinf(a)), (lv_value_precise_t)lroundf(DIAL_CY - r * cosf(a))};
}

// Every second the stylus moves on; the hands and the date only when the minute changes.
static void dial_tick(lv_timer_t *) {
  const time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  const lv_point_precise_t s = on_dial(t.tm_sec / 60.0f, DIAL_R * 0.9f);
  lv_obj_set_pos(dial.second, s.x - 6, s.y - 6);
  if (t.tm_min == dial.minute_shown) return;
  dial.minute_shown = t.tm_min;
  dial.hour_at[1] = on_dial((t.tm_hour % 12 + t.tm_min / 60.0f) / 12, DIAL_R * 0.5f);
  dial.minute_at[1] = on_dial(t.tm_min / 60.0f, DIAL_R * 0.8f);
  lv_line_set_points(dial.hour, dial.hour_at, 2);
  lv_line_set_points(dial.minute, dial.minute_at, 2);
  char day[12], month[12];
  strftime(day, sizeof day, "%A", &t);
  strftime(month, sizeof month, "%B", &t);
  lv_label_set_text_fmt(dial.date, "%s %d %s", day, t.tm_mday, month);
  lv_label_set_text_fmt(dial.year, "%d · tap for the crate", t.tm_year + 1900);
}

static void open_clock() {
  if (!face.data) draw_record(face, 2 * DIAL_R, 0, ART_BG);
  lv_obj_t *s = screen();
  lv_obj_t *rec = lv_image_create(s);
  lv_image_set_src(rec, &face);
  lv_obj_set_pos(rec, DIAL_CX - DIAL_R, DIAL_CY - DIAL_R);
  for (int h = 0; h < 12; h++) {
    const lv_point_precise_t p = on_dial(h / 12.0f, DIAL_R * 0.9f);
    dot(s, p.x, p.y, h % 3 ? 5 : 9, h % 3 ? DIM : TEXT);
  }
  auto hand = [s](lv_point_precise_t *at, int width, lv_color_t color) {
    at[0] = at[1] = {DIAL_CX, DIAL_CY};
    lv_obj_t *l = lv_line_create(s);
    lv_line_set_points(l, at, 2);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, color, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
  };
  dial.hour = hand(dial.hour_at, 9, METAL);
  dial.minute = hand(dial.minute_at, 5, TEXT);
  dial.second = dot(s, DIAL_CX, DIAL_CY, 12, ACCENT);  // the stylus, in the groove
  lv_obj_t *spindle = dot(s, DIAL_CX, DIAL_CY, 22, PART);
  lv_obj_set_style_border_color(spindle, METAL, 0);
  lv_obj_set_style_border_width(spindle, 2, 0);

  dial.date = label(s, &font_30, TEXT, 448);
  lv_obj_set_pos(dial.date, 16, 414);
  dial.year = label(s, &font_mono_14, DIM, 448);
  lv_obj_set_pos(dial.year, 16, 454);
  for (lv_obj_t *l : {dial.date, dial.year}) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  // Nothing on the dial takes the tap from the screen.
  for (uint32_t i = 0; i < lv_obj_get_child_count(s); i++) lv_obj_set_clickable(lv_obj_get_child(s, i), false);
  lv_obj_add_event_cb(s, [](lv_event_t *) { resume(); }, LV_EVENT_CLICKED, nullptr);

  dial.minute_shown = -1;
  dial_tick(nullptr);
  lv_timer_t *timer = lv_timer_create(dial_tick, 1000, nullptr);
  lv_obj_add_event_cb(s, [](lv_event_t *e) { lv_timer_delete((lv_timer_t *)lv_event_get_user_data(e)); },
                      LV_EVENT_DELETE, timer);
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
  eyebrow(s, "First spin");
  lv_obj_t *head = label(s, &font_30, TEXT, 448);
  lv_label_set_text(head, "Link this screen");
  lv_obj_set_pos(head, 16, 42);

  // The QR code is the sleeve, with the record half out of it.
  lv_obj_t *rec = lv_image_create(s);
  lv_image_set_src(rec, &platter);
  lv_obj_set_pos(rec, 180, 72);
  lv_obj_t *qr = lv_qrcode_create(s);
  lv_qrcode_set_size(qr, 192);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, TEXT);
  lv_qrcode_update(qr, link_code.url.c_str(), link_code.url.length());
  lv_obj_set_style_border_color(qr, TEXT, 0);
  lv_obj_set_style_border_width(qr, 8, 0);
  lv_obj_set_pos(qr, 72, 88);

  lv_obj_t *code = label(s, &font_44, ACCENT, 448);
  lv_obj_set_style_text_letter_space(code, 2, 0);
  lv_label_set_text(code, link_code.user_code.c_str());
  lv_obj_set_pos(code, 16, 318);
  lv_obj_set_pos(wrapped(s, &font_16, BODY,
                         "Scan with your phone and approve, or open\nvault.jakubszypicyn.com/link and type the code."),
                 16, 376);
  rule(s, 440, RULE);
  readout(s, 452, false, "00.0 rpm");
  readout(s, 452, true, "waiting for approval");

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
  eyebrow(s, "Side A · Wi-Fi");
  lv_obj_t *head = label(s, &font_30, TEXT, 448);
  lv_label_set_text(head, "Choose a network");
  lv_obj_set_pos(head, 16, 42);

  // The networks as a side's tracks, A1, A2…, as in the site's menu.
  lv_obj_t *list = lv_obj_create(s);
  lv_obj_set_size(list, 448, 300);
  lv_obj_set_pos(list, 16, 92);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_row(list, 0, 0);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  for (size_t i = 0; i < networks.size(); i++) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 448, 52);
    lv_obj_set_clickable(row, true);
    lv_obj_set_style_bg_color(row, CHIP, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, RULE_SOFT, 0);
    lv_obj_t *track = label(row, &font_mono_14, DIM, 40);
    lv_label_set_text_fmt(track, "A%u", (unsigned)i + 1);
    lv_obj_align(track, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *name = label(row, &font_22, TEXT, 370);
    lv_label_set_text(name, networks[i].c_str());
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_t *more = label(row, &font_22, DIM, 20);
    lv_label_set_text(more, "›");
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(row, [](lv_event_t *e) {
      wifi_ssid = lv_label_get_text(lv_obj_get_child(lv_event_get_target_obj(e), 1));
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
  lv_obj_t *head = label(s, &font_22, TEXT, 448);
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
  // Dark, with the site's green: the keyboard, text field and pressed states follow it.
  lv_display_set_theme(nullptr, lv_theme_default_init(lv_display_get_default(), ACCENT, DOWN, true, &font_16));
  draw_record(vinyl, WHEEL_FRONT_PX, 6, 0x18E3);  // the placeholder cover: a record on a CHIP sleeve
  draw_record(disc, 144, 0, ART_BG);
  draw_record(platter, 240, 0, ART_BG);
  // Before the time is known (offline since boot) there is no clock to show.
  lv_timer_create([](lv_timer_t *) {
    if (idle_screen && lv_screen_active() == idle_screen && lv_display_get_inactive_time(nullptr) > IDLE_MS &&
        time(nullptr) > 1700000000) {
      open_clock();
    }
  }, 1000, nullptr);
  // Temporary: the NFC reader's state, top centre over every screen, above the wheel's band.
  lv_obj_t *nfc = label(lv_layer_top(), &font_mono_14, DIM, 200);
  lv_obj_align(nfc, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_text_align(nfc, LV_TEXT_ALIGN_CENTER, 0);
  lv_timer_create([](lv_timer_t *t) {
    lv_obj_t *l = (lv_obj_t *)lv_timer_get_user_data(t);
    const uint32_t seen = nfc_tag_ms();
    const bool tag = seen && millis() - seen < 2000;
    const char *text = tag ? "NFC detected" : nfc_connected() ? "NFC connected" : "NFC not found";
    if (text == lv_label_get_text(l)) return;  // redraw only on a change: the wheel shares core 1
    lv_label_set_text_static(l, text);
    lv_obj_set_style_text_color(l, tag ? ACCENT : DIM, 0);
  }, 250, nfc);
#ifdef GRADE_BENCH
  lv_timer_create(grade_bench, 100, nullptr);
#endif
  String ssid;
  if (!wifi_saved(ssid)) return open_wifi();
  message(("Joining " + ssid + "…").c_str());
  if (wifi_reconnect()) return start();
  // Offline: show what the board has, if anything; the sync task retries when Wi-Fi returns.
  if (vault_linked() && load_collection()) return open_wheel();
  problem(("Couldn't join " + ssid + ".\nTap to try again, or hold to choose another network.").c_str(),
          [] { if (wifi_reconnect()) start(); else open_wifi(); });
}
