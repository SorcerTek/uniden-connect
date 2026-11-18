#pragma once
#include <Arduino.h>

// Battery level from the ADC on PIN_BATTERY_ADC (through a resistor divider).
namespace Battery {

void begin();
void loop();          // samples once a second
int  millivolts();    // smoothed battery voltage in mV
int  percent();       // 0..100, or -1 when no battery reading

}  // namespace Battery
