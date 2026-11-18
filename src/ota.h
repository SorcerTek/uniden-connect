#pragma once
#include <Arduino.h>

// Wireless firmware updates (ArduinoOTA). Upload from VS Code with the
// "fnk0104n_ota" environment once the board is on Wi-Fi.
namespace Ota {

void begin();           // after Wi-Fi is up; does nothing if no OTA password is set
void restart();         // after the OTA password changes
void loop();
bool enabled();

}  // namespace Ota
