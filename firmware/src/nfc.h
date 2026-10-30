// The PN532 NFC reader on the H3 header: UART with the ESP32 receiving on TX0, GPIO43 (H3 pin 5)
// and sending on IO0 (pin 13). No IRQ wire, so a background task on core 0 polls it. For now it
// only logs the tags it sees. Leave RSTPDN unwired: EXIO4 (H3 pin 12) looks free but is the PWR
// key's sense line, held low, and it kept the PN532 in reset.
#pragma once
#include <Arduino.h>

void nfc_begin();
bool nfc_connected();  // the PN532 answered its last check
