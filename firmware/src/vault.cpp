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
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  for (uint32_t start = millis(); time(nullptr) < 1700000000 && millis() - start < 10000;) delay(100);
  Serial.printf("{\"event\":\"wifi\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"linked\":%s}\n", ssid.c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), token.isEmpty() ? "false" : "true");
  return true;
}

bool wifi_reconnect() {
  String ssid;
  return wifi_saved(ssid) && wifi_connect(ssid, prefs.getString("pass"));
}

bool vault_linked() { return !token.isEmpty(); }

void vault_forget() {
  token = "";
  prefs.remove("token");
}

// One request to the gateway; the JSON reply (or error body) lands in doc. keep reuses one
// connection for the sync task's long runs of calls; anything else opens its own.
static int call(const char *method, const String &path, const String &body, JsonDocument &doc,
                const JsonDocument *filter = nullptr, bool keep = false) {
  static NetworkClientSecure kept_tls;
  static NetworkClient kept_plain;
  static HTTPClient kept_http;
  NetworkClientSecure own_tls;
  NetworkClient own_plain;
  HTTPClient own_http;
  NetworkClientSecure &tls = keep ? kept_tls : own_tls;
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
  Serial.printf("{\"event\":\"http\",\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,\"ms\":%lu,\"internal_largest\":%u}\n",
                method, path.c_str(), status, millis() - start, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return status;
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

int vault_set_grade(int id, const char *field, const char *grade) {
  JsonDocument body, doc;
  body[field] = grade;
  String json;
  serializeJson(body, json);
  return call("PATCH", "/api/records/" + String(id), json, doc);
}
