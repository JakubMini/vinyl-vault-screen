#include "vault.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_app_desc.h>

#include <algorithm>
#include <functional>

#include "config.h"

const char *const GRADES[GRADE_COUNT] = {"M", "NM", "VG+", "VG", "G+", "G", "F", "P"};

static Preferences prefs;
static String token;

// Wi-Fi details live in NVS next to the token. Every network joined, with its password, in one key
// as JSON ({"ssid": "password"}): NVS key names stop at 15 characters, network names at 32.
// shortcut: NVS is not encrypted, so the Wi-Fi passwords sit in flash in plain text; turn on
// flash and NVS encryption before the board leaves the house.
static void open_prefs() {
  static bool opened = false;
  if (!opened && (opened = prefs.begin("vault")) && !prefs.isKey("nets")) {
    // From the older firmware's one remembered network and its fallback.
    JsonDocument doc;
    doc.to<JsonObject>();
    const String ssid = prefs.getString("ssid"), ssid2 = prefs.getString("ssid2");
    if (!ssid2.isEmpty()) doc[ssid2] = prefs.getString("pass2");
    if (!ssid.isEmpty()) doc[ssid] = prefs.getString("pass");
    String json;
    serializeJson(doc, json);
    if (prefs.putString("nets", json)) for (const char *k : {"ssid", "pass", "ssid2", "pass2"}) prefs.remove(k);
  }
  token = prefs.getString("token");
}

bool wifi_known(const String &ssid, String &pass) {
  open_prefs();
  JsonDocument doc;
  deserializeJson(doc, prefs.getString("nets"));
  if (!doc[ssid].is<const char *>()) return false;
  pass = doc[ssid].as<String>();
  return true;
}

std::vector<String> wifi_scan() {
  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  std::vector<std::pair<int, String>> found;
  for (int i = 0; i < n; i++) {
    const String ssid = WiFi.SSID(i);
    bool seen = ssid.isEmpty();
    for (auto &f : found) seen |= f.second == ssid;
    if (!seen) found.push_back({WiFi.RSSI(i), ssid});
  }
  WiFi.scanDelete();
  std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first > b.first; });
  std::vector<String> out;
  for (auto &f : found) out.push_back(f.second);
  return out;
}

static bool joined;  // a network was joined since boot: keep it

bool wifi_connect(const String &ssid, const String &pass) {
  open_prefs();
  static bool logging = false;
  if (!logging) {
    logging = true;
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
      Serial.printf("{\"event\":\"wifi_lost\",\"reason\":%u}\n", info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
  joined = false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // mains powered; sleeping only adds latency to every touch
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (uint32_t start = millis(); WiFi.status() != WL_CONNECTED; delay(100)) {
    if (millis() - start > 20000) {
      WiFi.disconnect();
      Serial.printf("{\"event\":\"wifi_failed\",\"ssid\":\"%s\"}\n", ssid.c_str());
      return false;
    }
  }
  joined = true;
  String known;
  if (!wifi_known(ssid, known) || known != pass) {  // NVS writes only what changed
    JsonDocument doc;
    deserializeJson(doc, prefs.getString("nets"));
    doc[ssid] = pass;
    String json;
    serializeJson(doc, json);
    prefs.putString("nets", json);
  }
  // TLS checks certificate dates, so the clock has to be right first.
  // UK time for the idle clock; time() stays UTC, so TLS and sync are unaffected.
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.cloudflare.com");
  for (uint32_t start = millis(); time(nullptr) < 1700000000 && millis() - start < 10000;) delay(100);
  Serial.printf("{\"event\":\"wifi\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"linked\":%s}\n", ssid.c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), token.isEmpty() ? "false" : "true");
  return true;
}

// Arduino rejoins by itself only after some kinds of drop, and stops after one failed try at others.
void wifi_keep() {
  static uint32_t lost_at;
  if (!joined || WiFi.isConnected()) return (void)(lost_at = 0);
  if (!lost_at) lost_at = millis() | 1;
  if (millis() - lost_at < 30000) return;
  Serial.println("{\"event\":\"wifi_rejoin\"}");
  WiFi.reconnect();
  lost_at = millis() | 1;
}

bool vault_linked() { return !token.isEmpty(); }

void vault_forget() {
  token = "";
  prefs.remove("token");
}

// Made on first use: a global would be built before setup() sends allocations to PSRAM.
static NetworkClientSecure *kept_tls;
static NetworkClient kept_plain;
static HTTPClient kept_http;

// An idle open connection still holds its TLS buffers, about 33 KB of internal RAM.
void vault_close() {
  if (kept_tls) kept_tls->stop();
  kept_plain.stop();
}

// One request to the gateway; the JSON reply (or error body) lands in doc, or a 200's body goes to
// raw instead (false: it couldn't be read). keep reuses one connection for the sync task's long
// runs of calls; anything else opens its own.
static int call(const char *method, const String &path, const String &body, JsonDocument &doc,
                const JsonDocument *filter = nullptr, bool keep = false,
                const std::function<bool(HTTPClient &)> &raw = nullptr) {
  NetworkClientSecure own_tls;
  NetworkClient own_plain;
  HTTPClient own_http;
  if (keep && !kept_tls) kept_tls = new NetworkClientSecure;
  NetworkClientSecure &tls = keep ? *kept_tls : own_tls;
  HTTPClient &http = keep ? kept_http : own_http;

  const String url = String(GATEWAY_URL) + path;
  const bool https = url.startsWith("https://");
  if (https) tls.useBuiltinCACertBundle();
  http.setReuse(keep);
  http.setTimeout(15000);
  if (!http.begin(https ? (NetworkClient &)tls : keep ? kept_plain : own_plain, url)) return -1;
  http.addHeader("Accept", "application/json");
  if (!token.isEmpty()) http.addHeader("Authorization", "Bearer " + token);
  if (!body.isEmpty()) http.addHeader("Content-Type", "application/json");
  static const char *RAW_HEADERS[] = {"X-Cover-Width", "X-Cover-Height", "X-Firmware-Build"};
  http.collectHeaders(RAW_HEADERS, 3);

  const uint32_t start = millis();
  int status = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
  if (status == 200 && raw) {
    if (!raw(http)) status = -2;
  } else if (status > 0 && status != 204) {  // a 204 has no body and no length: reading one waits for the
                                             // kept-open connection to close, minutes on the gateway
    const String reply = http.getString();  // handles chunked replies; lands in PSRAM
    DeserializationError err = filter ? deserializeJson(doc, reply, DeserializationOption::Filter(*filter))
                                      : deserializeJson(doc, reply);
    if (err && status < 300) status = -2;
  }
  http.end();
  Serial.printf("{\"event\":\"http\",\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,\"ms\":%lu,\"internal_largest\":%u,\"internal_free\":%u}\n",
                method, path.c_str(), status, millis() - start, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return status;
}

void vault_warm(bool on) {
  const bool open = (kept_tls && kept_tls->connected()) || kept_plain.connected();
  if (open && !on) vault_close();
  if (open || !on) return;
  JsonDocument doc;
  call("GET", "/", "", doc, nullptr, true);  // the gateway answers 404 itself; the vault never sees it
}

int vault_start_link(LinkCode &code) {
  JsonDocument doc;
  int status = call("POST", "/api/device/code", "", doc);
  if (status != 200) return status;
  code.device_code = doc["device_code"].as<String>();
  code.user_code = doc["user_code"].as<String>();
  code.url = doc["verification_uri_complete"].as<String>();
  code.interval = doc["interval"] | 5;
  code.expires_at = millis() + (doc["expires_in"] | 600) * 1000;
  return status;
}

LinkState vault_poll_link(const LinkCode &code) {
  if ((int32_t)(millis() - code.expires_at) > 0) return LinkState::Expired;
  JsonDocument body, doc;
  body["device_code"] = code.device_code;
  String json;
  serializeJson(body, json);
  int status = call("POST", "/api/device/token", json, doc);
  if (status == 200) {
    token = doc["access_token"].as<String>();
    prefs.putString("token", token);
    return LinkState::Linked;
  }
  String error = doc["error"] | "";
  if (error == "authorization_pending") return LinkState::Pending;
  if (error == "expired_token" || error == "invalid_grant") return LinkState::Expired;
  return LinkState::Error;
}

int vault_records(std::vector<Record> &out) {
  JsonDocument filter;
  for (const char *key : {"id", "artist", "title", "year", "cover_image_url", "current_value", "change_30d_minor",
                          "media_condition", "sleeve_condition"}) {
    filter["records"][0][key] = true;
  }
  JsonDocument doc;
  int status = call("GET", "/api/records?limit=1000", "", doc, &filter, true);
  if (status != 200) return status;
  out.clear();
  for (JsonObject r : doc["records"].as<JsonArray>()) {
    out.push_back({r["id"], r["artist"].as<String>(), r["title"].as<String>(), r["current_value"] | "",
                   r["cover_image_url"] | "", r["year"] | 0, r["change_30d_minor"] | NO_VALUE,
                   r["media_condition"].as<String>(), r["sleeve_condition"].as<String>()});
  }
  return status;
}

int vault_detail(int id, Detail &out, bool from_sync) {
  JsonDocument filter;
  for (const char *key : {"current_value", "media_condition", "sleeve_condition"}) filter[key] = true;
  filter["valuations"][0]["value_minor"] = true;
  filter["price_by_grade"][0]["grade"] = true;
  filter["price_by_grade"][0]["value_minor"] = true;
  JsonDocument doc;
  int status = call("GET", "/api/records/" + String(id) + "?limit=90", "", doc, &filter, from_sync);
  if (status != 200) return status;
  out.value = doc["current_value"] | "";
  out.media = doc["media_condition"].as<String>();
  out.sleeve = doc["sleeve_condition"].as<String>();
  out.history.clear();
  JsonArray valuations = doc["valuations"];
  for (int i = valuations.size() - 1; i >= 0; i--) out.history.push_back(valuations[i]["value_minor"]);
  for (int &p : out.price_by_grade) p = NO_VALUE;
  for (JsonObject p : doc["price_by_grade"].as<JsonArray>()) {
    for (int g = 0; g < GRADE_COUNT; g++) {
      if (p["grade"] == GRADES[g]) out.price_by_grade[g] = p["value_minor"] | NO_VALUE;
    }
  }
  return status;
}

int vault_set_grade(int id, const char *field, const char *grade, Detail &d) {
  JsonDocument body, filter, doc;
  body[field] = grade;
  String json;
  serializeJson(body, json);
  for (const char *key : {"current_value", "media_condition", "sleeve_condition"}) filter[key] = true;
  const int status = call("PATCH", "/api/records/" + String(id), json, doc, &filter, true);
  if (status != 200) return status;
  const bool regraded = d.media != doc["media_condition"].as<String>();
  d.value = doc["current_value"] | "";
  d.media = doc["media_condition"].as<String>();
  d.sleeve = doc["sleeve_condition"].as<String>();
  // The vault re-prices from its suggestion for the new grade, and only when it has one.
  for (int g = 0; g < GRADE_COUNT; g++) {
    if (regraded && d.media == GRADES[g] && d.price_by_grade[g] != NO_VALUE) d.history.push_back(d.price_by_grade[g]);
  }
  return status;
}

int vault_tag(const String &uid, int &id) {
  JsonDocument doc;
  const int status = call("GET", "/api/tags/" + uid, "", doc, nullptr, true);
  if (status == 200) id = doc["record_id"] | -1;
  return status;
}

int vault_set_tag(const String &uid, int id) {
  JsonDocument body, doc;
  body["record_id"] = id;
  String json;
  serializeJson(body, json);
  return call("PUT", "/api/tags/" + uid, json, doc, nullptr, true);
}

int vault_plays(const std::vector<Play> &plays) {
  JsonDocument body, doc;
  for (const Play &p : plays) {
    JsonObject o = body["plays"].add<JsonObject>();
    o["record_id"] = p.id;
    o["tag_uid"] = p.uid;
    if (p.at) {
      char at[24];
      tm t;
      gmtime_r(&p.at, &t);
      strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", &t);
      o["played_at"] = at;
    }
  }
  String json;
  serializeJson(body, json);
  return call("POST", "/api/plays", json, doc, nullptr, true);
}

int vault_cover(int id, uint8_t *&rgb, int &w, int &h) {
  rgb = nullptr;
  JsonDocument doc;
  return call("GET", "/api/records/" + String(id) + "/cover", "", doc, nullptr, true, [&](HTTPClient &http) {
    w = http.header("X-Cover-Width").toInt(), h = http.header("X-Cover-Height").toInt();
    const int len = w * h * 3;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || http.getSize() != len || !(rgb = (uint8_t *)ps_malloc(len))) return false;
    NetworkClient *stream = http.getStreamPtr();
    int got = 0;
    for (const uint32_t deadline = millis() + 15000; got < len && millis() < deadline;) {
      const int n = stream->read(rgb + got, len - got);
      if (n > 0) got += n;
      else if (!stream->connected()) break;
      else delay(2);
    }
    if (got == len) return true;
    free(rgb);
    rgb = nullptr;
    return false;
  });
}

int vault_firmware(const std::function<void(size_t, size_t)> &progress) {
  open_prefs();
  // All 64 hex digits, as the vault keeps them: esp_app_get_elf_sha256 stops at
  // CONFIG_APP_RETRIEVE_LEN_ELF_SHA (9), so it would never match and every sync would start a download.
  char running[65];
  const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
  for (int i = 0; i < 32; i++) snprintf(running + 2 * i, 3, "%02x", sha[i]);
  JsonDocument doc;
  return call("GET", "/api/firmware/bin?running=" + String(running), "", doc, nullptr, true, [&](HTTPClient &http) {
    const String build = http.header("X-Firmware-Build");
    // Installed before: it rolled back, or a build has gone on over USB since. Only a new upload goes on.
    if (build == prefs.getString("ota_build")) {
      Serial.printf("{\"event\":\"update_skipped\",\"build\":\"%s\"}\n", build.c_str());
      return false;
    }
    const int len = http.getSize();
    if (len <= 0 || !Update.begin(len)) return false;
    Update.onProgress(progress);
    // Update.end has the bootloader's checks run over the image, its SHA-256 among them, before it
    // will boot it: a cut or garbled download is refused.
    if (Update.writeStream(*http.getStreamPtr()) == (size_t)len && Update.end()) {
      prefs.putString("ota_build", build);
      return true;
    }
    Serial.printf("{\"event\":\"update_failed\",\"error\":\"%s\"}\n", Update.errorString());
    Update.abort();
    return false;
  });
}
