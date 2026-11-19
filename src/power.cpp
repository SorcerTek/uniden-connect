#if !defined(BOARD_TAB5)   // FNK0104N only (Tab5 version: board_tab5.cpp)
#include "power.h"
#include "config.h"
#include "display_st77922.h"
#include "audio_out.h"
#include "rtsp_audio.h"
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace Power {

static uint32_t s_downSince = 0;
static bool s_holdReported = false;

void begin() {
  // Undo the pin holds set before deep sleep (amp off, backlight off).
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_AMP_EN);
  gpio_hold_dis((gpio_num_t)PIN_LCD_BL);
  rtc_gpio_deinit(GPIO_NUM_0);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) Serial.println("Woken by BOOT button");
}

// Tracks the button; returns the event for this call.
enum Event { E_NONE, E_HELD, E_SHORT };
static Event poll() {
  bool down = digitalRead(PIN_BOOT_BTN) == LOW;
  uint32_t now = millis();
  if (down) {
    if (!s_downSince) { s_downSince = now; s_holdReported = false; }
    if (!s_holdReported && now - s_downSince >= POWER_HOLD_MS) { s_holdReported = true; return E_HELD; }
    return E_NONE;
  }
  Event e = E_NONE;
  if (s_downSince && !s_holdReported && now - s_downSince > 40) e = E_SHORT;   // debounce
  s_downSince = 0;
  return e;
}

static Event s_pending = E_NONE;
bool bootHeld() {
  if (s_pending == E_NONE) s_pending = poll();
  if (s_pending == E_HELD) { s_pending = E_NONE; return true; }
  return false;
}
bool bootPressed() {
  if (s_pending == E_NONE) s_pending = poll();
  if (s_pending == E_SHORT) { s_pending = E_NONE; return true; }
  return false;
}

static void quiet() {
  RtspAudio::shutdown();          // TEARDOWN so the scanner frees its single audio slot
  AudioOut::setMuted(true);       // codec mute + amp off
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void restart() {
  quiet();
  Serial.println("Restarting");
  Serial.flush();
  delay(100);
  ESP.restart();
}

void off() {
  quiet();
  St77922::sleep();
  St77922::releaseBacklightPin();
  // Keep the amplifier off and the backlight dark while asleep.
  digitalWrite(PIN_AMP_EN, HIGH);
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en((gpio_num_t)PIN_AMP_EN);
  gpio_hold_en((gpio_num_t)PIN_LCD_BL);
  gpio_deep_sleep_hold_en();

  // Wait for BOOT to be let go, or it would wake us straight away.
  while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
  delay(100);

  Serial.println("Powering off (deep sleep) - press BOOT to turn on");
  Serial.flush();
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

}  // namespace Power
#endif  // !BOARD_TAB5
