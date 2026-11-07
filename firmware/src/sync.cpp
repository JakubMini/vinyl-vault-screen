#include "sync.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <img_converters.h>

#include <algorithm>
#include <map>

#include "covers.h"
#include "store.h"
#include "vault.h"

constexpr time_t RETRY_S = 300;         // after a failed attempt
constexpr int COVER_PX = 180;           // the front cover's size on the wheel; the vault makes them this size

static SemaphoreHandle_t lock;
static SyncStatus status{};
static volatile bool requested = false;
static volatile int front_id = -1;
static volatile bool warm = false;
static TaskHandle_t sync_task;
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

// ---- covers: the vault's 180 px RGB -> a small baseline JPEG on flash --------------------------

// The vault shrinks Discogs' cover: decoding the 600 px progressive original here takes about 3 MB
// of PSRAM, more than is free. A baseline JPEG, as the screen decodes one in a few milliseconds.
// Returns the step that failed, or nullptr once the cover is on flash.
static const char *make_cover(const Record &r, int &code) {
  if (r.cover.isEmpty()) return "no_url";
  uint8_t *rgb;
  int w, h;
  if ((code = vault_cover(r.id, rgb, w, h)) < 0) vault_close(), code = vault_cover(r.id, rgb, w, h);
  if (!rgb) return "download";
  for (uint8_t *p = rgb; p < rgb + w * h * 3; p += 3) std::swap(p[0], p[2]);  // the camera library's order: blue first
  const char *failed = "encode";
  uint8_t *jpeg = nullptr;
  size_t jpeg_len = 0;
  if (fmt2jpg(rgb, w * h * 3, w, h, PIXFORMAT_RGB888, 85, &jpeg, &jpeg_len)) {
    failed = store_write(store_cover_path(r.id), jpeg, jpeg_len) ? nullptr : "write";
    if (!failed) covers_put(r.id, jpeg, jpeg_len);
    free(jpeg);
  }
  free(rgb);
  return failed;
}

static bool unlinked(int code);

// ---- new builds, over Wi-Fi ------------------------------------------------------------------
//
// A build fresh from the vault boots on trial. Until it has fetched the record list it isn't marked
// good, so a reset before then (a crash, the watchdog, the power) boots the build before it again.
static bool on_trial;

// Arduino would mark every build good as it boots.
extern "C" bool verifyRollbackLater() { return true; }

static void trial_passed() {
  if (!on_trial) return;
  on_trial = false;
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.println("{\"event\":\"update_kept\"}");
}

// Any build uploaded to the vault since: installed now, and the board restarts into it.
static void update_firmware() {
  int code = vault_firmware([](size_t done, size_t total) { progress("firmware", done / 1024, total / 1024); });
  if (code < 0) vault_close();  // a skipped or failed download leaves the reply half read
  if (unlinked(code) || code != 200) return;
  Serial.println("{\"event\":\"updated\"}");
  delay(100);  // the log line out
  ESP.restart();
}

// ---- one sync --------------------------------------------------------------------------------

static bool unlinked(int code) {
  if (code != 401) return false;
  update([](SyncStatus &s) { s.unlinked = true; });
  return true;
}

// ---- a new grade, from the screen ------------------------------------------------------------

static struct {
  bool asked, done;
  int id, status;
  const char *field, *grade;
  Detail detail;
} grading;

void sync_set_grade(int id, const char *field, const char *grade, const Detail &now) {
  xSemaphoreTake(lock, portMAX_DELAY);
  grading.asked = true, grading.done = false;
  grading.id = id, grading.field = field, grading.grade = grade, grading.detail = now;
  xSemaphoreGive(lock);
  xTaskNotifyGive(sync_task);
}

bool sync_grade_done(int &status, Detail &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool done = grading.done;
  if (done) status = grading.status, out = std::move(grading.detail), grading.done = false;
  xSemaphoreGive(lock);
  return done;
}

void sync_keep_warm(bool on) { warm = on; }

static bool grade_asked() {
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool asked = grading.asked;
  xSemaphoreGive(lock);
  return asked;
}

// ---- NFC tags --------------------------------------------------------------------------------

// The UI only ever holds tags_lock for a lookup or an insert: /tags.json is written by the sync task
// alone, from a copy, outside the lock, so a tap never waits on flash.
static SemaphoreHandle_t tags_lock;
static std::map<String, Tag> tags;
static bool tags_dirty;  // changed since /tags.json was written
static struct {
  bool asked, done;
  String uid;
  int status, id;
} looking;

int sync_tag_record(const char *uid) {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  auto t = tags.find(uid);
  const int id = t == tags.end() ? -1 : t->second.id;
  xSemaphoreGive(tags_lock);
  return id;
}

void sync_tag_lookup(const char *uid) {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  looking.asked = true, looking.done = false, looking.uid = uid;
  xSemaphoreGive(tags_lock);
  xTaskNotifyGive(sync_task);
}

bool sync_tag_found(int &status, int &id) {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  const bool done = looking.done;
  if (done) status = looking.status, id = looking.id, looking.done = false;
  xSemaphoreGive(tags_lock);
  return done;
}

int sync_tag_assign(const char *uid, int id) {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  tags[uid] = {id, false};
  tags_dirty = true;
  int count = 0;
  for (const auto &[u, t] : tags) count += t.id == id;
  xSemaphoreGive(tags_lock);
  xTaskNotifyGive(sync_task);
  return count;
}

// A lookup to answer, or an assignment to put on flash and send.
static bool tag_asked() {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  const bool asked = looking.asked || tags_dirty;
  xSemaphoreGive(tags_lock);
  return asked;
}

// Sync task only.
static void save_tags() {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  const bool dirty = tags_dirty;
  tags_dirty = false;
  const std::map<String, Tag> copy = dirty ? tags : std::map<String, Tag>();
  xSemaphoreGive(tags_lock);
  if (dirty) store_save_tags(copy);
}

// ---- plays -----------------------------------------------------------------------------------
//
// Each time a record's tag counts, a play goes to the vault for a yearly "wrapped". The UI only drops
// it in a queue, which never waits; everything else happens here on core 0. Sent within a second
// when online; written to flash only if the vault can't be reached, so a restart while offline
// doesn't lose them, and a play doesn't stall the panel when it can.

struct PlayNote {
  int id;
  char uid[21];
  time_t at;
};
static QueueHandle_t play_inbox;  // from the UI, 32 deep
static std::vector<Play> plays;   // sync task only: waiting to be sent
static bool plays_on_flash;       // /plays.json holds plays not yet sent

void sync_play(int id, const char *uid) {
  PlayNote note{id, "", time(nullptr)};
  strlcpy(note.uid, uid, sizeof note.uid);
  if (xQueueSend(play_inbox, &note, 0) == pdTRUE) xTaskNotifyGive(sync_task);
  else Serial.printf("{\"event\":\"play_dropped\",\"record\":%d}\n", id);  // 32 waiting: the sync task is stuck
}

static void serve_plays() {
  for (PlayNote n; xQueueReceive(play_inbox, &n, 0) == pdTRUE;) {
    Serial.printf("{\"event\":\"play\",\"record\":%d,\"uid\":\"%s\",\"at\":%ld}\n", n.id, n.uid, (long)n.at);
    plays.push_back({n.id, n.uid, n.at > 1700000000 ? n.at : 0});
  }
  static uint32_t failed_at;  // after a failure, try again in a minute
  if (plays.empty() || (failed_at && millis() - failed_at < 60000)) return;
  const std::vector<Play> batch(plays.begin(), plays.begin() + std::min<size_t>(plays.size(), 100));

  int code = -1;
  if (WiFi.isConnected() && vault_linked() && (code = vault_plays(batch)) < 0) vault_close(), code = vault_plays(batch);
  unlinked(code);
  // 400: the vault will never take this batch, so it is dropped rather than retried for ever.
  const bool done = code == 200 || code == 400;
  if (done) plays.erase(plays.begin(), plays.begin() + batch.size());
  // Flash keeps the queue only while the vault can't be reached: after a failure, all of it; after
  // a send, what is left, often nothing.
  if (!done || plays_on_flash) {
    store_save_plays(plays);
    plays_on_flash = !plays.empty();
  }
  failed_at = done ? 0 : millis() | 1;
}

// The screen is waiting on the vault: a sync stops fetching to let it go first.
static bool screen_waiting() { return grade_asked() || tag_asked(); }

// The lookup the screen asked for, then any tag the vault doesn't have yet. On the gateway
// connection, like a grade; both calls are safe to send twice.
static void serve_tags() {
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  const bool asked = looking.asked;
  looking.asked = false;
  const String uid = looking.uid;
  std::vector<std::pair<String, int>> unsent;
  for (const auto &[u, t] : tags) if (!t.sent) unsent.push_back({u, t.id});
  xSemaphoreGive(tags_lock);
  const bool online = WiFi.isConnected() && vault_linked();

  if (asked) {
    int id = -1, code = -1;
    if (online && (code = vault_tag(uid, id)) < 0) vault_close(), code = vault_tag(uid, id);
    unlinked(code);
    xSemaphoreTake(tags_lock, portMAX_DELAY);
    if (code == 200 && id > 0) tags[uid] = {id, true}, tags_dirty = true;
    looking.done = true, looking.status = code, looking.id = id;
    xSemaphoreGive(tags_lock);
  }

  save_tags();  // what was assigned or learnt, before trying the network
  static uint32_t failed_at;  // after a failure, try again in a minute
  if (unsent.empty() || !online || (failed_at && millis() - failed_at < 60000)) return;
  failed_at = 0;
  std::vector<std::pair<String, int>> sent;
  for (const auto &[u, id] : unsent) {
    int code = vault_set_tag(u, id);
    if (code < 0) vault_close(), code = vault_set_tag(u, id);
    if (unlinked(code)) break;
    if (code != 200 && code != 404) {  // 404: the record left the vault, so there's nothing to send
      failed_at = millis() | 1;
      break;
    }
    sent.push_back({u, id});
    Serial.printf("{\"event\":\"tag_sent\",\"uid\":\"%s\",\"record\":%d,\"status\":%d}\n", u.c_str(), id, code);
  }
  xSemaphoreTake(tags_lock, portMAX_DELAY);
  for (const auto &[u, id] : sent) {
    auto t = tags.find(u);
    // Assigned to another record meanwhile? Then it stays unsent, and goes next time.
    if (t != tags.end() && t->second.id == id && !t->second.sent) t->second.sent = tags_dirty = true;
  }
  xSemaphoreGive(tags_lock);
  save_tags();
}

// While the sync waits, or after its prices: on the gateway connection, warm if the page kept it so.
// Never mid-prices: with the sync's data in internal RAM, the next price call then loses the connection.
// Returns the record's id if the vault took the grade, with its detail in `out`; otherwise -1.
static int serve_grade(Detail &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool asked = grading.asked;
  grading.asked = false;
  const int id = grading.id;
  const char *field = grading.field, *grade = grading.grade;
  Detail d = std::move(grading.detail);
  xSemaphoreGive(lock);
  if (!asked) return -1;

  int code = vault_set_grade(id, field, grade, d);
  if (code < 0) {  // the gateway may have closed an idle connection; a grade is safe to send twice
    vault_close();
    code = vault_set_grade(id, field, grade, d);
  }
  out = d;
  xSemaphoreTake(lock, portMAX_DELAY);
  grading.done = true, grading.status = code, grading.detail = std::move(d);
  xSemaphoreGive(lock);
  return code == 200 ? id : -1;
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
  trial_passed();
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
    if (screen_waiting()) continue;  // a grade or a tag goes first; the rest keep their last known prices
    code = vault_detail(fresh[i].id, d, true);
    if (unlinked(code)) return vault_close();
    vTaskDelay(1);
  }
  // A grade saved now goes into this sync's copy too, or storing the copy would undo it.
  Detail graded;
  const int graded_id = serve_grade(graded);
  if (graded_id >= 0) {
    details[graded_id] = graded;
    for (Record &r : fresh) if (r.id == graded_id) r.value = graded.value, r.media = graded.media, r.sleeve = graded.sleeve;
    if (store_save_records(fresh)) update([](SyncStatus &s) { s.records_gen++; });
  }
  serve_tags();
  serve_plays();
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
  while (!todo.empty() && !screen_waiting()) {  // a grade or a tag goes first; the rest come next sync
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
    int code = 0;
    const char *failed = make_cover(*r, code);
    Serial.printf("{\"event\":\"cover_cached\",\"id\":%d,\"ok\":%s,\"failed\":\"%s\",\"status\":%d,\"ms\":%lu}\n", r->id,
                  failed ? "false" : "true", failed ? failed : "", code, millis() - t);
    if (unlinked(code)) return vault_close();
    if (!failed) update([](SyncStatus &s) { s.covers_gen++; });
    made++;
  }

  update_firmware();

  const time_t now = time(nullptr);
  prefs.putULong64("last_ok", now);
  if (resized) prefs.putInt("cover_px", COVER_PX);
  update([](SyncStatus &s) { s.running = false, s.last_ok = time(nullptr); });
  Serial.printf("{\"event\":\"synced\",\"records\":%u,\"covers\":%d,\"ms\":%lu}\n", fresh.size(), made, millis() - start);
}

// Opening costs a TLS handshake, about 2 s; after a failed one, wait before the next.
static void keep_warm() {
  static uint32_t tried_at;
  const bool on = warm && WiFi.isConnected() && vault_linked();
  if (on && millis() - tried_at < 5000) return;
  if (on) tried_at = millis();
  vault_warm(on);
}

// A sync runs when the clock crosses a full hour, when asked (Refresh, or a board with nothing
// cached yet), and five minutes after a failed attempt. Never just because the board restarted.
static void task(void *) {
  long hour_seen = -1;
  time_t retry_at = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));  // or sooner, for a grade or a tag
    wifi_keep();
    Detail unused;
    serve_grade(unused);
    serve_tags();
    serve_plays();
    keep_warm();
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
  tags_lock = xSemaphoreCreateMutex();
  store_load_tags(tags);
  plays_on_flash = store_load_plays(plays) && !plays.empty();
  play_inbox = xQueueCreate(32, sizeof(PlayNote));
  prefs.begin("sync");
  status.last_ok = prefs.getULong64("last_ok", 0);
  esp_ota_img_states_t state;
  on_trial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
             state == ESP_OTA_IMG_PENDING_VERIFY;
  requested = on_trial;  // a sync as soon as Wi-Fi is up, to pass the trial
  // Core 0, away from LVGL on core 1: decoding a Discogs cover takes about a second. At idle
  // priority, as a TLS handshake is two seconds of sums: above it, the handshake and the covers'
  // loader between them can keep core 0's idle task out long enough for the watchdog to reset.
  xTaskCreatePinnedToCore(task, "sync", 20480, nullptr, tskIDLE_PRIORITY, &sync_task, 0);
}
