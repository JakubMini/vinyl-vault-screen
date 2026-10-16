#include "covers.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <LittleFS.h>

#include <map>

#include "store.h"

constexpr int SLOTS = 16;  // decoded covers kept; each is about 115 KB of PSRAM
constexpr int WANT_MAX = 12;

struct Slot {
  int id = -1;
  uint32_t used = 0;
  lv_image_dsc_t dsc{};  // dsc.data stays null while there is no JPEG for it yet
};

static Slot slots[SLOTS];
static int want_ids[WANT_MAX];
static int want_n = 0;
static uint32_t clock_tick = 0;
static SemaphoreHandle_t lock;
// Every cached cover's JPEG, about 2 MB for the collection, read from flash once at boot: a flash
// read stalls the RGB panel, so browsing must not touch flash at all.
struct Jpeg {
  uint8_t *data;
  size_t len;
};
static std::map<int, Jpeg> jpegs;

static bool cached(int id) {
  for (const Slot &s : slots) if (s.id == id) return true;
  return false;
}

static bool wanted(int id) {
  for (int i = 0; i < want_n; i++) if (want_ids[i] == id) return true;
  return false;
}

static int draw(JPEGDRAW *d) {
  lv_image_dsc_t *dsc = (lv_image_dsc_t *)d->pUser;
  uint16_t *dst = (uint16_t *)dsc->data;
  for (int y = 0; y < d->iHeight && d->y + y < (int)dsc->header.h; y++) {
    memcpy(dst + (d->y + y) * dsc->header.w + d->x, d->pPixels + y * d->iWidth, d->iWidthUsed * 2);
  }
  return 1;
}

static bool decode(int id, lv_image_dsc_t &dsc) {
  // Copied out under the lock, so covers_put can replace the JPEG while this one decodes.
  uint8_t *jpeg = nullptr;
  size_t len = 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  auto found = jpegs.find(id);
  if (found != jpegs.end() && (jpeg = (uint8_t *)ps_malloc(found->second.len))) {
    len = found->second.len;
    memcpy(jpeg, found->second.data, len);
  }
  xSemaphoreGive(lock);
  if (!jpeg) return false;
  static JPEGDEC *dec = new (ps_malloc(sizeof(JPEGDEC))) JPEGDEC;
  bool ok = false;
  if (dec->openRAM(jpeg, len, draw)) {
    const int w = dec->getWidth(), h = dec->getHeight();
    if (uint16_t *px = (uint16_t *)ps_malloc(w * h * 2)) {
      dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      dsc.header.cf = LV_COLOR_FORMAT_RGB565;
      dsc.header.w = w;
      dsc.header.h = h;
      dsc.header.stride = w * 2;
      dsc.data_size = w * h * 2;
      dsc.data = (const uint8_t *)px;
      dec->setPixelType(RGB565_LITTLE_ENDIAN);
      dec->setUserPointer(&dsc);
      ok = dec->decode(0, 0, 0);
      if (!ok) free(px), dsc.data = nullptr;
    }
    dec->close();
  }
  free(jpeg);
  return ok;
}

static void load_from_flash() {
  for (int id : store_cover_ids()) {
    File f = LittleFS.open(store_cover_path(id), "r");
    if (!f) continue;
    const size_t len = f.size();
    if (uint8_t *data = (uint8_t *)ps_malloc(len)) {
      if (f.read(data, len) == len) covers_put(id, data, len);
      free(data);
    }
    f.close();
    vTaskDelay(1);
  }
  Serial.printf("{\"event\":\"covers_loaded\",\"count\":%u}\n", jpegs.size());
}

static void loader(void *) {
  load_from_flash();
  for (;;) {
    int id = -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < want_n && id < 0; i++) if (!cached(want_ids[i])) id = want_ids[i];
    xSemaphoreGive(lock);
    if (id < 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    lv_image_dsc_t dsc{};
    decode(id, dsc);  // no JPEG yet: the slot stays empty until covers_put brings one

    // Evict the least recently used cover that is no longer wanted, so nothing on screen is freed.
    xSemaphoreTake(lock, portMAX_DELAY);
    Slot *victim = nullptr;
    for (Slot &s : slots) {
      if (s.id < 0) { victim = &s; break; }
      if (!wanted(s.id) && (!victim || s.used < victim->used)) victim = &s;
    }
    if (victim) {
      free((void *)victim->dsc.data);
      *victim = {id, ++clock_tick, dsc};
    } else {
      free((void *)dsc.data);
    }
    xSemaphoreGive(lock);
  }
}

void covers_begin() {
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(loader, "covers", 8192, nullptr, 2, nullptr, 0);
}

void covers_put(int id, const uint8_t *jpeg, size_t len) {
  uint8_t *copy = (uint8_t *)ps_malloc(len);
  if (!copy) return;
  memcpy(copy, jpeg, len);
  xSemaphoreTake(lock, portMAX_DELAY);
  Jpeg &j = jpegs[id];
  free(j.data);
  j = {copy, len};
  // Drop the decoded picture (or the note that there was none) so the new one shows. If it is
  // on screen right now it stays until it drops out of the wanted list.
  for (Slot &s : slots) {
    if (s.id == id && (!s.dsc.data || !wanted(id))) {
      free((void *)s.dsc.data);
      s = Slot{};
    }
  }
  xSemaphoreGive(lock);
}

void covers_want(const int *ids, int n) {
  xSemaphoreTake(lock, portMAX_DELAY);
  want_n = min(n, WANT_MAX);
  for (int i = 0; i < want_n; i++) want_ids[i] = ids[i];
  xSemaphoreGive(lock);
}

const lv_image_dsc_t *covers_get(int id) {
  const lv_image_dsc_t *found = nullptr;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (Slot &s : slots) {
    if (s.id == id && s.dsc.data) {
      s.used = ++clock_tick;
      found = &s.dsc;
    }
  }
  xSemaphoreGive(lock);
  return found;
}
