// The PN532 NFC reader on the relay header: UART on IO40 (RX) and IO2 (TX), its RSTPDN on IO1, no
// IRQ wire, so a background task on core 0 polls it. For now it only logs the tags it sees.
#pragma once
#include <Arduino.h>

void nfc_begin();
bool nfc_connected();  // the PN532 answered its last check
