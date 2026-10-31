#include "vault.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>

#include "config.h"

const char *const GRADES[GRADE_COUNT] = {"M", "NM", "VG+", "VG", "G+", "G", "F", "P"};

static Preferences prefs;
static String token;

// Wi-Fi details live in NVS next to the token, so a reset reconnects on its own.
// shortcut: NVS is not encrypted, so the Wi-Fi password sits in flash in plain text; turn on
// flash and NVS encryption before the board leaves the house.
static void open_prefs() {
  static bool opened = false;
  if (!opened) opened = prefs.begin("vault");
  token = prefs.getString("token");
#ifdef WIFI2_SSID
  // A one-off build seeds the fallback network; normal builds keep whatever NVS holds.
  if (prefs.getString("ssid2") != WIFI2_SSID || prefs.getString("pass2") != WIFI2_PASS) {
    prefs.putString("ssid2", WIFI2_SSID);
    prefs.putString("pass2", WIFI2_PASS);
  }
#endif
}

bool wifi_saved(String &ssid) {
  open_prefs();
  ssid = prefs.getString("ssid");
  return !ssid.isEmpty();
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

bool wifi_connect(const String &ssid, const String &pass) {
  open_prefs();
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
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  // TLS checks certificate dates, so the clock has to be right first.
  // UK time for the idle clock; time() stays UTC, so TLS and sync are unaffected.
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.cloudflare.com");
  for (uint32_t start = millis(); time(nullptr) < 1700000000 && millis() - start < 10000;) delay(100);
  Serial.printf("{\"event\":\"wifi\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"linked\":%s}\n", ssid.c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), token.isEmpty() ? "false" : "true");
  return true;
}

// Tries the remembered network, then the fallback. Whichever joins is tried first next time.
bool wifi_reconnect() {
  String ssid;
  if (!wifi_saved(ssid)) return false;
  const String pass = prefs.getString("pass");
  if (wifi_connect(ssid, pass)) return true;
  const String ssid2 = prefs.getString("ssid2"), pass2 = prefs.getString("pass2");
  if (ssid2.isEmpty() || ssid2 == ssid || !wifi_connect(ssid2, pass2)) return false;
  prefs.putString("ssid2", ssid);  // wifi_connect made the fallback the primary; keep the old one
  prefs.putString("pass2", pass);
  return true;
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

// One request to the gateway; the JSON reply (or error body) lands in doc. keep reuses one
// connection for the sync task's long runs of calls; anything else opens its own.
static int call(const char *method, const String &path, const String &body, JsonDocument &doc,
                const JsonDocument *filter = nullptr, bool keep = false) {
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

  const uint32_t start = millis();
  int status = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
  if (status > 0) {
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
