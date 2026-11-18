#pragma once
#include <Arduino.h>

// Minimal driver for the Sitronix ST77922 QSPI panel on the Freenove FNK0104N.
//
// The panel only accepts full-width window writes reliably, so this driver
// always sends whole rows (x = 0..319) in horizontal bands. The UI draws into
// a full-screen sprite in PSRAM and calls pushFrame() to send it.
namespace St77922 {

bool begin();                       // init SPI bus + panel, turn backlight on
void setBacklight(bool on);
void sleep();                       // panel off + sleep-in (before deep sleep)

// Send a full native-orientation (320x480) RGB565 frame.
// `pixels` is big-endian RGB565 (high byte first), as LovyanGFX sprites store it.
void pushFrame(const uint16_t* pixels);

}  // namespace St77922
