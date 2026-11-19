#if !defined(BOARD_TAB5)   // FNK0104N only (Tab5 version: board_tab5.cpp)
#include "battery.h"
#include "config.h"

namespace Battery {

static float s_mv = 0;
static uint32_t s_last = 0;

static float sample() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BATTERY_ADC);
  return (sum / 8.0f) * BATTERY_DIVIDER;
}

void begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  s_mv = sample();
  s_last = millis();
}

void loop() {
  if (millis() - s_last < 1000) return;
  s_last = millis();
  s_mv = s_mv * 0.8f + sample() * 0.2f;
#if BATTERY_DEBUG
  Serial.printf("battery %d mV  %d%%\n", millivolts(), percent());
#endif
}

int millivolts() { return (int)(s_mv + 0.5f); }

int percent() {
  float v = s_mv;
  if (v < 2500) return -1;            // no battery
  if (v >= 4150) return 100;
  if (v >= 3700) return 50 + (int)((v - 3700) * 50 / 450);
  if (v >= 3300) return (int)((v - 3300) * 50 / 400);
  return 0;
}

}  // namespace Battery
#endif  // !BOARD_TAB5
