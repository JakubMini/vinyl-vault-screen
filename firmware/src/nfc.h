// The PN532 NFC reader on the H3 header: UART with the ESP32 receiving on TX0, GPIO43 (H3 pin 5)
// and sending on IO0 (pin 13). No IRQ wire, so a background task on core 0 polls it. A tag counts
// once, then not again until another tag has counted or it has been away for 30 s (tag_gate.h).
// RSTPDN goes to EXIO3 (H3 pin 12), which the board drives high; the task pulses it low when the
// reader stops answering. Undriven, the speaker amp's pull-down on that line holds it in reset.
#pragma once
#include <Arduino.h>

void nfc_begin();
bool nfc_connected();  // the PN532 answered its last check
bool nfc_take(char uid[21]);  // the latest tag that counted, in hex, once; false if none since
