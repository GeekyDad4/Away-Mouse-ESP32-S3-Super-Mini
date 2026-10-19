// links.h - USB and Bluetooth mouse links with automatic selection.
//
// At power-up the USB mouse is started. If a computer enumerates it, the
// device is a USB mouse. If nothing enumerates it within USB_DETECT_MS
// (powered from a charger), it starts advertising as a Bluetooth mouse.
// If a computer enumerates it later, Bluetooth is shut off and USB wins.
#pragma once

#include <Arduino.h>

enum class Link : uint8_t { NONE, USB, BLE };

namespace links {

void begin();      // call once from setup()
void service();    // call often from loop(): handles switching
Link active();     // link that can carry reports right now (NONE if neither)
bool send(int8_t dx, int8_t dy, int8_t wheel);
uint32_t tickMs(); // report interval for the active link
const char *describe();

}  // namespace links
