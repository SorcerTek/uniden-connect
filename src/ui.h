#pragma once
#include <Arduino.h>

// Screen drawing and touch handling (480x320 landscape).
namespace Ui {

bool begin();      // display + touch must be ready
void loop();       // call often: handles touch and redraws when needed
void showMessage(const char* line1, const char* line2 = nullptr);   // boot / error screen
void showLogo(const char* status = nullptr);   // boot logo, with a status line at the bottom
void bootAnimFrame(const char* status);
void openSetup(bool firstRun);                 // Wi-Fi / scanner setup screen         // draw the next boot animation frame, paced to its frame rate

}  // namespace Ui
