#include "nfc.h"

#include <Adafruit_PN532.h>

static volatile bool connected = false;

bool nfc_connected() { return connected; }

static void task(void *) {
  // UART, not I2C: over I2C the PN532 refuses every poll while it's busy, and each refusal cost the
  // ESP32 about 80 ms, so a tag check took seconds. Serial1, with its pins set before the library
  // starts it: left to itself it would take UART1's default pins, which drive the panel.
  // RX on GPIO43, not IO0: on IO0 (pull-ups, the BOOT key) the first byte of every ACK was lost, and
  // the PN532 holding it low at reset would boot the ESP32 into download mode.
  pinMode(43, INPUT);  // GPIO43 boots as UART0's TX and keeps driving the line unless released first
  Serial1.begin(115200, SERIAL_8N1, 43 /* RX, the PN532's TXD */, 0 /* TX, its RXD */);
  Serial1.setTimeout(50);  // a read with no tag answers short: don't wait the default second for the rest
  // No reset pin (255; the library logs it as invalid once): the module pulls RSTPDN high, and the
  // library only pulses it in begin(), which this skips.
  Adafruit_PN532 nfc(255, &Serial1);
  bool present = false;
  int quiet = 0;
  for (;;) {
    if (!connected) {
      // It is asleep at boot. A long preamble wakes it, but only with a command straight after, and
      // the first try usually fails anyway: hence the quick retry. Connects within a second.
      static const uint8_t wake[16] = {0x55, 0x55};  // then zeros
      while (Serial1.available()) Serial1.read();
      Serial1.write(wake, sizeof wake);
      uint32_t version = 0;
      // One try per poll, so a read comes back at once when no tag is there.
      if (nfc.SAMConfig() && (version = nfc.getFirmwareVersion()) && nfc.setPassiveActivationRetries(1)) {
        connected = true;
        Serial.printf("{\"event\":\"nfc\",\"connected\":true,\"firmware\":\"%lu.%lu\"}\n", (version >> 16) & 0xFF,
                      (version >> 8) & 0xFF);
      } else {
        delay(200);
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
