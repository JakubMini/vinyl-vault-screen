// Which NFC reads the screen acts on, kept free of Arduino so it can be tested on the laptop.
// A tag counts once. It counts again only after a different tag has counted, or once it has gone
// unread for TAG_AWAY_MS. The reader sees a tag every few hundred milliseconds while it sits on the
// stand, and each read pushes that time back, so a sleeve left there never fires twice.
#pragma once
#include <stdint.h>
#include <string.h>

constexpr uint32_t TAG_AWAY_MS = 30000;

struct TagGate {
  char last[21] = "";  // the UID that last counted, in hex: up to 10 bytes
  uint32_t seen_ms = 0;  // when it was last read

  // A read of uid at now_ms. True when the screen should act on it.
  bool read(const char *uid, uint32_t now_ms) {
    const bool same = strcmp(uid, last) == 0;
    const bool counts = !same || now_ms - seen_ms > TAG_AWAY_MS;
    if (!same) {
      strncpy(last, uid, sizeof last - 1);
      last[sizeof last - 1] = '\0';
    }
    seen_ms = now_ms;
    return counts;
  }
};
