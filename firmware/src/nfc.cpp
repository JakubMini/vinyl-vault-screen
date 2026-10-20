#include "nfc.h"

#include <Adafruit_PN532.h>
#include <Wire.h>

static volatile bool connected = false;
static volatile uint32_t tag_ms = 0;

bool nfc_connected() { return connected; }
uint32_t nfc_tag_ms() { return tag_ms; }

static void task(void *) {
  // Wire1, so the touch controller keeps Wire to itself. The I2C driver turns on the ESP32's own
  // pull-ups too; 50 kHz, since the PN532 is slow and the wires are long.
  Wire1.setPins(40 /* SDA */, 2 /* SCL */);
  Adafruit_PN532 nfc(0xFF, 1 /* RSTPDN */, &Wire1);  // no IRQ wire: it logs "Invalid IO 255" once
  nfc.begin();
  Wire1.setClock(50000);
  bool present = false;
  int quiet = 0;
  for (;;) {
    if (!connected) {
      // A reset first: if the ESP32 restarted mid-transfer, the PN532 holds SCL low until it gets one.
      nfc.reset();
      delay(10);
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
    uint8_t uid[7], len;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 100)) {
      tag_ms = millis(), quiet = 0;
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
