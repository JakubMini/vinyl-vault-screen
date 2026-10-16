// Keeps the board's copy of the collection fresh, on a background task: once an hour, or when asked.
// A sync fetches the record list, each record's history and prices, and any cover that is new or
// has changed. The UI never waits for it.
#pragma once
#include <Arduino.h>

#include "vault.h"

void sync_begin();
void sync_now();                // a manual refresh
void sync_hint_front(int id);   // the record at the front, so its cover is fetched first

// A new grade, sent from the sync task so it shares the sync's connection: one at a time. `now` is
// the record's detail as shown; when done, it comes back as the vault left it (vault_set_grade).
void sync_set_grade(int id, const char *field, const char *grade, const Detail &now);
bool sync_grade_done(int &status, Detail &out);  // true once, when the vault has answered
void sync_keep_warm(bool on);  // while a record's page is up: its grades save on an open connection

struct SyncStatus {
  bool running;
  int done, total;              // progress through the current step
  const char *step;             // "records", "prices" or "covers" while running
  time_t last_ok;               // when the last complete sync finished; 0 if never
  bool failed;                  // the last attempt could not reach the vault
  bool unlinked;                // the vault refused the token: link again
  uint32_t records_gen;         // goes up when the record list on flash changes
  uint32_t covers_gen;          // goes up when a cover on flash changes
};
SyncStatus sync_status();
