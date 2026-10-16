#include "sync.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <img_converters.h>

#include <algorithm>
#include <map>

#include "covers.h"
#include "store.h"
#include "vault.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MALLOC(sz) ps_malloc(sz)
#define STBI_REALLOC(p, sz) ps_realloc(p, sz)
#define STBI_FREE(p) free(p)
#include <stb_image.h>

constexpr time_t RETRY_S = 300;         // after a failed attempt
constexpr int COVER_PX = 180;           // the front cover's size on the wheel
constexpr int MAX_BYTES = 2 * 1024 * 1024;

static SemaphoreHandle_t lock;
static SyncStatus status{};
static volatile bool requested = false;
static volatile int front_id = -1;
static Preferences prefs;

static void update(void (*fn)(SyncStatus &)) {
  xSemaphoreTake(lock, portMAX_DELAY);
  fn(status);
  xSemaphoreGive(lock);
}

SyncStatus sync_status() {
  xSemaphoreTake(lock, portMAX_DELAY);
  SyncStatus copy = status;
  xSemaphoreGive(lock);
  return copy;
}

void sync_now() {
  update([](SyncStatus &s) { s.unlinked = false; });  // after linking again
  requested = true;
}
void sync_hint_front(int id) { front_id = id; }

static int done_count, total_count;
static const char *step_name;
static void progress(const char *step, int done, int total) {
  step_name = step, done_count = done, total_count = total;
  update([](SyncStatus &s) { s.step = step_name, s.done = done_count, s.total = total_count; });
}

// ---- covers: Discogs' JPEG (mostly progressive) -> a small baseline JPEG on flash ----------------

// One connection to Discogs' image host, kept open between covers: a TLS handshake costs more
// than the download itself.
static NetworkClientSecure cover_tls;
static HTTPClient cover_http;

static uint8_t *download(const String &url, int &len) {
  static bool ready = false;
  if (!ready) {
    cover_tls.useBuiltinCACertBundle();
    cover_http.setReuse(true);
    cover_http.setTimeout(15000);
    cover_http.setUserAgent("VinylVaultScreen/0.1 +https://github.com/JakubMini/vinyl-vault-screen");
    ready = true;
  }
  len = 0;
  if (!cover_http.begin(cover_tls, url)) return nullptr;
  uint8_t *buf = nullptr;
  if (cover_http.GET() == 200 && (len = cover_http.getSize()) > 0 && len <= MAX_BYTES && (buf = (uint8_t *)ps_malloc(len))) {
    NetworkClient *stream = cover_http.getStreamPtr();
    int got = 0;
    for (const uint32_t deadline = millis() + 15000; got < len && millis() < deadline;) {
      const int n = stream->read(buf + got, len - got);
      if (n > 0) got += n;
      else if (!stream->connected()) break;
      else delay(2);
    }
    if (got != len) {
      free(buf);
      buf = nullptr;
    }
  }
  cover_http.end();
  return buf;
}

// Area-average down to at most COVER_PX a side, then encode as a baseline JPEG, which the
// screen decodes in a few milliseconds (Discogs' progressive originals take about a second).
static bool make_cover(const Record &r) {
  int len = 0;
  uint8_t *src = r.cover.isEmpty() ? nullptr : download(r.cover, len);
  if (!src) return false;
  int w, h, n;
  uint8_t *rgb = stbi_load_from_memory(src, len, &w, &h, &n, 3);
  free(src);
  if (!rgb) return false;

  const float s = std::max(1.0f, std::max(w, h) / (float)COVER_PX);
  const int ow = std::max(1, (int)lroundf(w / s)), oh = std::max(1, (int)lroundf(h / s));
  uint8_t *out = (uint8_t *)ps_malloc(ow * oh * 3);
  bool ok = false;
  if (out) {
    for (int y = 0; y < oh; y++) {
      const int y0 = y * s, y1 = std::min(h, std::max(y0 + 1, (int)((y + 1) * s)));
      for (int x = 0; x < ow; x++) {
        const int x0 = x * s, x1 = std::min(w, std::max(x0 + 1, (int)((x + 1) * s)));
        uint32_t sum[3] = {0, 0, 0}, count = (y1 - y0) * (x1 - x0);
        for (int yy = y0; yy < y1; yy++) {
          const uint8_t *p = rgb + (yy * w + x0) * 3;
          for (int xx = x0; xx < x1; xx++, p += 3) sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
        }
        // The camera library's RGB888 is stored blue first.
        uint8_t *o = out + (y * ow + x) * 3;
        o[0] = sum[2] / count, o[1] = sum[1] / count, o[2] = sum[0] / count;
      }
    }
    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    if (fmt2jpg(out, ow * oh * 3, ow, oh, PIXFORMAT_RGB888, 85, &jpeg, &jpeg_len)) {
      ok = store_write(store_cover_path(r.id), jpeg, jpeg_len);
      if (ok) covers_put(r.id, jpeg, jpeg_len);
      free(jpeg);
    }
    free(out);
  }
  stbi_image_free(rgb);
  return ok;
}

// ---- one sync --------------------------------------------------------------------------------

static bool unlinked(int code) {
  if (code != 401) return false;
  update([](SyncStatus &s) { s.unlinked = true; });
  return true;
}

static void run() {
  const uint32_t start = millis();
  update([](SyncStatus &s) { s.running = true, s.failed = false; });
  progress("records", 0, 1);

  std::vector<Record> fresh, old;
  int code = vault_records(fresh);
  if (unlinked(code) || code != 200) {
    vault_close();
    update([](SyncStatus &s) { s.running = false, s.failed = true; });
    return;
  }
  store_load_records(old);
  // Only what changed is written, and the screen only reloads when something did.
  if (store_save_records(fresh)) update([](SyncStatus &s) { s.records_gen++; });

  std::map<int, Detail> saved, details;
  store_load_details(saved);
  for (size_t i = 0; i < fresh.size(); i++) {
    progress("prices", i, fresh.size());
    // The last known prices if this fetch fails. Moved, not copied: a copied 15-point history is
    // under 64 bytes, so it would land in internal RAM, 76 bytes a record.
    Detail &d = details[fresh[i].id] = std::move(saved[fresh[i].id]);
    code = vault_detail(fresh[i].id, d, true);
    if (unlinked(code)) return vault_close();
    vTaskDelay(1);
  }
  vault_close();  // its TLS memory is needed for the covers' connection
  if (store_save_details(details)) update([](SyncStatus &s) { s.records_gen++; });

  // Covers that are missing or have changed, nearest the front of the wheel first.
  // A change of COVER_PX re-makes them all.
  const bool resized = prefs.getInt("cover_px") != COVER_PX;
  const std::set<int> have = store_cover_ids();
  std::vector<const Record *> todo;
  for (const Record &r : fresh) {
    auto was = std::find_if(old.begin(), old.end(), [&](const Record &o) { return o.id == r.id; });
    if (resized || !have.count(r.id) || was == old.end() || was->cover != r.cover) todo.push_back(&r);
  }
  int made = 0;
  while (!todo.empty()) {
    progress("covers", made, made + todo.size());
    const int n = fresh.size();
    int at = 0;
    for (int i = 0; i < n; i++) if (fresh[i].id == front_id) at = i;
    auto distance = [&](const Record *r) {
      const int d = abs((int)(r - fresh.data()) - at);
      return std::min(d, n - d);
    };
    auto next = std::min_element(todo.begin(), todo.end(), [&](auto a, auto b) { return distance(a) < distance(b); });
    const Record *r = *next;
    todo.erase(next);
    const uint32_t t = millis();
    const bool ok = make_cover(*r);
    Serial.printf("{\"event\":\"cover_cached\",\"id\":%d,\"ok\":%s,\"ms\":%lu,\"internal_largest\":%u}\n", r->id,
                  ok ? "true" : "false", millis() - t, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (ok) update([](SyncStatus &s) { s.covers_gen++; });
    made++;
  }
  cover_tls.stop();  // so the next sync's gateway connection has the memory

  const time_t now = time(nullptr);
  prefs.putULong64("last_ok", now);
  if (resized) prefs.putInt("cover_px", COVER_PX);
  update([](SyncStatus &s) { s.running = false, s.last_ok = time(nullptr); });
  Serial.printf("{\"event\":\"synced\",\"records\":%u,\"covers\":%d,\"ms\":%lu}\n", fresh.size(), made, millis() - start);
}

// A sync runs when the clock crosses a full hour, when asked (Refresh, or a board with nothing
// cached yet), and five minutes after a failed attempt. Never just because the board restarted.
static void task(void *) {
  long hour_seen = -1;
  time_t retry_at = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const time_t now = time(nullptr);
    if (now < 1700000000) continue;  // no clock yet
    const long hour = now / 3600;
    if (hour_seen < 0) hour_seen = hour;
    const bool due = hour != hour_seen || (retry_at && now >= retry_at);
    if (!(requested || due) || !WiFi.isConnected() || !vault_linked() || sync_status().unlinked) continue;
    requested = false;
    hour_seen = hour;
    run();
    retry_at = sync_status().failed ? now + RETRY_S : 0;
  }
}

void sync_begin() {
  lock = xSemaphoreCreateMutex();
  prefs.begin("sync");
  status.last_ok = prefs.getULong64("last_ok", 0);
  // Core 0, away from LVGL on core 1: decoding a Discogs cover takes about a second.
  xTaskCreatePinnedToCore(task, "sync", 20480, nullptr, 1, nullptr, 0);
}
