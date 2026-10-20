// The PN532 NFC reader on the relay header: I2C on IO40 (SDA) and IO2 (SCL), its RSTPDN on IO1, no
// IRQ wire, so a background task on core 0 polls it. For now it only reports that a tag is there.
#pragma once
#include <Arduino.h>

void nfc_begin();
bool nfc_connected();      // the PN532 answered its last check
uint32_t nfc_tag_ms();     // millis() when a tag was last seen; 0 if never
