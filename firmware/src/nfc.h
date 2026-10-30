// The PN532 NFC reader on the H3 header: UART with the ESP32 receiving on IO0 (H3 pin 13) and
// sending on TX0, GPIO43 (pin 5). No IRQ wire, so a background task on core 0 polls it. For now it
// only logs the tags it sees.
#pragma once
#include <Arduino.h>

void nfc_begin();
bool nfc_connected();  // the PN532 answered its last check
