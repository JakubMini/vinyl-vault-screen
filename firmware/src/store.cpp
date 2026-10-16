#include "store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

bool store_begin() {
  const bool ok = LittleFS.begin(true);  // formats the partition on first boot
  if (ok) {
    LittleFS.mkdir("/c");
  }
  Serial.printf("{\"event\":\"store\",\"ok\":%s,\"used\":%u,\"total\":%u}\n", ok ? "true" : "false",
                LittleFS.usedBytes(), LittleFS.totalBytes());
  return ok;
}

bool store_write(const String &path, const uint8_t *data, size_t len) {
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const bool ok = f.write(data, len) == len;
  f.close();
  return ok && LittleFS.rename(tmp, path);
}

// Returns true only when the file had to change.
static bool write_json(const String &path, const JsonDocument &doc) {
  String json;
  serializeJson(doc, json);
  File f = LittleFS.open(path, "r");
  if (f && f.size() == json.length()) {
    const String old = f.readString();
    f.close();
    if (old == json) return false;
  } else if (f) {
    f.close();
  }
  return store_write(path, (const uint8_t *)json.c_str(), json.length());
}

static bool read_json(const String &path, JsonDocument &doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const bool ok = !deserializeJson(doc, f);
  f.close();
  return ok;
}

bool store_save_records(const std::vector<Record> &records) {
  JsonDocument doc;
  for (const Record &r : records) {
    JsonObject o = doc.add<JsonObject>();
    o["id"] = r.id;
    o["artist"] = r.artist;
    o["title"] = r.title;
    o["value"] = r.value;
    o["cover"] = r.cover;
    o["year"] = r.year;
    o["change30"] = r.change30;
    o["media"] = r.media;
    o["sleeve"] = r.sleeve;
  }
  return write_json("/records.json", doc);
}

bool store_load_records(std::vector<Record> &out) {
  JsonDocument doc;
  if (!read_json("/records.json", doc)) return false;
  out.clear();
  for (JsonObject o : doc.as<JsonArray>()) {
    out.push_back({o["id"], o["artist"].as<String>(), o["title"].as<String>(), o["value"].as<String>(),
                   o["cover"].as<String>(), o["year"], o["change30"], o["media"].as<String>(),
                   o["sleeve"].as<String>()});
  }
  return !out.empty();
}

bool store_save_details(const std::map<int, Detail> &details) {
  JsonDocument doc;
  for (const auto &[id, d] : details) {
    JsonObject o = doc[String(id)].to<JsonObject>();
    o["value"] = d.value;
    o["media"] = d.media;
    o["sleeve"] = d.sleeve;
    for (int v : d.history) o["history"].add(v);
    for (int p : d.price_by_grade) o["prices"].add(p);
  }
  return write_json("/details.json", doc);
}

bool store_load_details(std::map<int, Detail> &out) {
  JsonDocument doc;
  if (!read_json("/details.json", doc)) return false;
  out.clear();
  for (JsonPair kv : doc.as<JsonObject>()) {
    Detail &d = out[String(kv.key().c_str()).toInt()];
    JsonObject o = kv.value();
    d.value = o["value"].as<String>();
    d.media = o["media"].as<String>();
    d.sleeve = o["sleeve"].as<String>();
    for (int v : o["history"].as<JsonArray>()) d.history.push_back(v);
    for (int g = 0; g < GRADE_COUNT; g++) d.price_by_grade[g] = o["prices"][g] | NO_VALUE;
  }
  return true;
}

String store_cover_path(int id) { return "/c/" + String(id) + ".jpg"; }

std::set<int> store_cover_ids() {
  std::set<int> ids;
  File dir = LittleFS.open("/c");
  for (String name = dir.getNextFileName(); !name.isEmpty(); name = dir.getNextFileName()) {
    if (name.endsWith(".jpg")) ids.insert(name.substring(name.lastIndexOf('/') + 1).toInt());
    vTaskDelay(1);  // a long directory walk must not starve the watchdog
  }
  return ids;
}
