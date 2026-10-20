#include "nfc.h"

#include <Adafruit_PN532.h>

static volatile bool connected = false;

bool nfc_connected() { return connected; }

static void task(void *) {
  // UART, not I2C: over I2C the PN532 refuses every poll while it's busy, and each refusal cost the
  // ESP32 about 80 ms, so a tag check took seconds. Serial1, with its pins set before the library
  // starts it: left to itself it would take UART1's default pins, which drive the panel.
  Serial1.begin(115200, SERIAL_8N1, 40 /* RX, the PN532's TXD */, 2 /* TX, its RXD */);
  Serial1.setTimeout(50);  // a read with no tag answers short: don't wait the default second for the rest
  Adafruit_PN532 nfc(1 /* RSTPDN */, &Serial1);
  bool present = false;
  int quiet = 0;
  for (;;) {
    if (!connected) {
      nfc.begin();  // reset through RSTPDN, then the wake-up preamble
      const uint32_t version = nfc.getFirmwareVersion();
      // One try per poll, so a read comes back at once when no tag is there.
      if (version && nfc.SAMConfig() && nfc.setPassiveActivationRetries(1)) {
        connected = true;
        Serial.printf("{\"event\":\"nfc\",\"connected\":true,\"firmware\":\"%lu.%lu\"}\n", (version >> 16) & 0xFF,
                      (version >> 8) & 0xFF);
      } else {
        delay(2000);
        continue;
      }
    }
    // The library reads fixed lengths; whatever a longer answer left behind would spoil the next ACK.
    while (Serial1.available()) Serial1.read();
    uint8_t uid[7], len;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 100)) {
      quiet = 0;
      if (!present) {
        char hex[15] = "";
        for (int i = 0; i < len && i < 7; i++) sprintf(hex + 2 * i, "%02X", uid[i]);
        Serial.printf("{\"event\":\"nfc_tag\",\"uid\":\"%s\"}\n", hex);
      }
      present = true;
    } else {
      present = false;
      // A failed read looks the same as no tag, so check the module is still there now and then.
      if (++quiet % 25 == 0 && !nfc.getFirmwareVersion()) {
        connected = false;
        Serial.println(R"({"event":"nfc","connected":false})");
      }
    }
    delay(200);
  }
}

void nfc_begin() { xTaskCreatePinnedToCore(task, "nfc", 4096, nullptr, 1, nullptr, 0); }
