#pragma once
#include <Arduino.h>

// Touch controller built into the ST77922 panel (I2C 0x55, 16-bit registers).
// Reports coordinates in the panel's native portrait frame (x 0..319, y 0..479).
namespace Touch {

bool begin();                                 // Wire must already be started
bool read(int16_t& x, int16_t& y);            // true while a finger is down

}  // namespace Touch
