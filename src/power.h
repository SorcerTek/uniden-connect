#pragma once
#include <Arduino.h>

// BOOT button (GPIO 0) power handling. The case has no power switch, so:
//   hold BOOT ~1.5 s  -> power menu (POWER OFF / RESTART / CANCEL)
//   POWER OFF         -> deep sleep; a short press of BOOT turns the board back on
namespace Power {

void begin();             // call first thing in setup(): releases pins held during sleep
bool bootHeld();          // true once per hold, when BOOT has been held for POWER_HOLD_MS
bool bootPressed();       // true once per short press (press + release)
void restart();           // clean RTSP TEARDOWN, then reboot
void off();               // clean shutdown, then deep sleep until BOOT is pressed

}  // namespace Power
