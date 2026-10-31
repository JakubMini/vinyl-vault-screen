// The vault, reached through the gateway Worker. Calls block; each returns the HTTP status, or a
// negative number when the gateway could not be reached. 401 means the screen is no longer linked.
#pragma once
#include <Arduino.h>
#include <vector>

constexpr int GRADE_COUNT = 8;
extern const char *const GRADES[GRADE_COUNT];  // Goldmine, best first: M, NM, VG+ ... P
constexpr int NO_VALUE = INT32_MIN;            // pence that the vault doesn't know

struct Record {
  int id;
  String artist, title, value, cover;  // value is formatted, e.g. "£25.00"; empty when unpriced
  int year;                            // 0 when unknown
  int change30;                        // pence over 30 days, or NO_VALUE
  String media, sleeve;                // grades
};

struct Detail {
  String value, media, sleeve;
  std::vector<int> history;           // pence, oldest first
  int price_by_grade[GRADE_COUNT];    // pence in GRADES order, or NO_VALUE
};

struct LinkCode {
  String device_code, user_code, url;
  int interval;          // seconds between polls
  uint32_t expires_at;   // millis()
};
enum class LinkState { Pending, Linked, Expired, Error };

// Wi-Fi, chosen on the screen and remembered across resets.
bool wifi_saved(String &ssid);                              // a network is remembered
bool wifi_reconnect();                                      // join the remembered network, else the fallback
bool wifi_connect(const String &ssid, const String &pass);  // join, set the clock, remember; false after 20 s
std::vector<String> wifi_scan();                            // nearby networks, strongest first

bool vault_linked();     // a device token is stored
void vault_forget();     // drop the token, e.g. after a 401
void vault_close();      // close the connection the sync's calls keep open
void vault_warm(bool on);  // on: open it ahead of a call (~0.1 s on it, not ~2 s); off: close it

int vault_start_link(LinkCode &code);
LinkState vault_poll_link(const LinkCode &code);

// from_sync: only the sync task passes true, to reuse its kept-open connection.
int vault_records(std::vector<Record> &out);  // sync task only
int vault_detail(int id, Detail &out, bool from_sync = false);
// Sync task only. On a 200, d takes the vault's value and grades, and the price it added to the
// history if the new grade re-priced the record.
int vault_set_grade(int id, const char *field, const char *grade, Detail &d);
