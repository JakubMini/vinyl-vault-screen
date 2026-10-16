// Keeps the board's copy of the collection fresh, on a background task: once an hour, or when asked.
// A sync fetches the record list, each record's history and prices, and any cover that is new or
// has changed. The UI never waits for it.
#pragma once
#include <Arduino.h>

void sync_begin();
void sync_now();                // a manual refresh
void sync_hint_front(int id);   // the record at the front, so its cover is fetched first

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
