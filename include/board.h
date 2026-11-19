#pragma once
// Board abstraction: the few things that differ between supported hardware.
//   BOARD_FNK0104N (default)  Freenove FNK0104N, 3.5" 480x320
//   BOARD_TAB5                M5Stack Tab5, 5" 1280x720 (ESP32-P4)
#include <Arduino.h>
#if defined(BOARD_TAB5)
  #include <M5Unified.h>          // M5GFX (LovyanGFX v1 inside), speaker, power, touch
#else
  #define LGFX_USE_V1
  #include <LovyanGFX.hpp>
#endif

namespace Board {

const char* name();
bool beginDisplay();            // panel, backlight and the full-screen canvas
LGFX_Sprite& canvas();          // landscape canvas the UI draws into
void present();                 // send the canvas to the screen
void setBrightness(int pct);    // 0-100
bool touch(int& x, int& y);     // finger position in canvas pixels; false if not touched
bool hasBootButton();           // BOOT button opens the power menu
bool hasLauncher();             // installed under bmorcelli's Launcher (offer "exit to Launcher")
void exitToLauncher();

}  // namespace Board
