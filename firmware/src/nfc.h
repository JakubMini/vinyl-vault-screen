// The PN532 NFC reader on the H3 header: UART with the ESP32 receiving on TX0, GPIO43 (H3 pin 5)
// and sending on IO0 (pin 13). No IRQ wire, so a background task on core 0 polls it. A tag counts
// once, then not again until another tag has counted or it has been away for 30 s (tag_gate.h).
// Leave RSTPDN unwired: EXIO4 (H3 pin 12) looks free but is the PWR key's sense line, held low,
// and it kept the PN532 in reset.
#pragma once
#include <Arduino.h>

void nfc_begin();
bool nfc_connected();  // the PN532 answered its last check
bool nfc_take(char uid[21]);  // the latest tag that counted, in hex, once; false if none since
