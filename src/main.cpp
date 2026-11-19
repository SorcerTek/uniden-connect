// Uniden Connect
// Network remote for a Uniden SDS200 scanner on the Freenove ESP32-S3 3.5" display (FNK0104N).
//
//   - shows what the scanner is receiving (system / department / channel / frequency)
//   - touch buttons send scanner key presses
//   - plays the scanner's audio stream on the board's speaker
//   - call log, recordings and recent calls on the microSD card
//
// Settings: include/config.h.  Wi-Fi, scanner IP and the update password are set on
// the board (KEYS > SETTINGS > WI-FI SETUP); include/secrets.h is optional.
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include "config.h"
#include "version.h"
#if defined(BOARD_TAB5)
#define UC_BOARD "M5Stack Tab5"
#else
#define UC_BOARD "Freenove FNK0104N"
#endif
#include "settings.h"
#include "board.h"
#if defined(BOARD_TAB5)
#include "esp_hosted.h"
#endif
#if !defined(BOARD_TAB5)
#include "touch_st77922.h"
#endif
#include "audio_out.h"
#include "rtsp_audio.h"
#include "sds200.h"
#include "ui.h"
#include "battery.h"
#include "power.h"
#include "calls.h"
#include "ota.h"

void setup() {
#if defined(BOARD_TAB5)
  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  M5.begin(cfg);                     // display, touch, speaker, power, Wi-Fi (ESP32-C6) pins
  M5.Display.setBrightness(0);       // dark until the logo is drawn (the panel shows garbage while starting)
#endif
  Power::begin();                    // release pins held during power-off sleep
  Serial.begin(115200);
  delay(200);
  Serial.println("\nUniden Connect v" UC_VERSION " starting (" UC_BOARD ")");

  if (!psramFound()) Serial.println("WARNING: PSRAM not found - check platformio.ini memory settings");
  Settings::begin();

  if (!Ui::begin()) Serial.println("Display / UI init failed");
  Ui::showLogo("Starting...");
  Board::setBrightness(Settings::get().brightness);   // light up once the logo is drawn

#if !defined(BOARD_TAB5)
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!Touch::begin()) Serial.println("Touch controller not found");
#endif
  if (!AudioOut::begin()) Serial.println("Audio init failed");
  AudioOut::setVolume(Settings::get().spkVolume);
  AudioOut::setMuted(Settings::get().muted);
  Battery::begin();
  Calls::begin();

#if defined(BOARD_TAB5)
  WiFi.setPins(TAB5_WIFI_SDIO_PINS);   // the Wi-Fi chip is an ESP32-C6 on SDIO (ESP-Hosted)
#endif
  WiFi.mode(WIFI_STA);
#if defined(BOARD_TAB5)
  {
    // Wait until the Wi-Fi chip answers before connecting. Starting the connection while
    // the SDIO link is still settling makes ESP-Hosted start the network twice and crash
    // ("netif already added"). Asking for its firmware version is a reliable wait.
    esp_hosted_coprocessor_fwver_t v = {};
    if (esp_hosted_get_coprocessor_fwversion(&v) == 0)
      Serial.printf("Wi-Fi chip (ESP32-C6) firmware %u.%u.%u\n", (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
  }
#endif
#if !defined(BOARD_TAB5)
  WiFi.setSleep(false);              // keeps audio latency low
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(OTA_HOSTNAME);
#endif
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);   // clock syncs once Wi-Fi is up
  RtspAudio::begin();                // its task waits for Wi-Fi by itself

  if (!Settings::hasWifi()) {
    // Nothing saved yet: go straight to the setup screen.
    Serial.println("No Wi-Fi saved - opening setup");
    Sds200::begin();
    Ui::openSetup(true);
    return;
  }
  WiFi.begin(Settings::wifiSsid().c_str(), Settings::wifiPassword().c_str());

  // Boot animation: loops while Wi-Fi connects and until the scanner answers
  // (at least one full pass, at most BOOT_ANIM_TIMEOUT_MS), then the normal screen.
  uint32_t start = millis();
  bool sdsStarted = false;
  String line;
  for (;;) {
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (wifi && !sdsStarted) {
      Serial.printf("Wi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());
      Sds200::begin();
      sdsStarted = true;
    }
    if (sdsStarted) Sds200::loop();
    Battery::loop();
    bool online = Sds200::status().online;
    uint32_t t = millis() - start;
    if (online && t >= BOOT_ANIM_MIN_MS) break;
    if (t >= BOOT_ANIM_TIMEOUT_MS) {
      Serial.println(wifi ? "Scanner not answering yet - starting anyway" : "Wi-Fi not connected yet");
      break;
    }
    if (!wifi) line = "Connecting to Wi-Fi: " + Settings::wifiSsid();
    else if (!online) line = "Wi-Fi OK - looking for scanner at " + Settings::scannerIp();
    else line = "Scanner connected";
    Ui::bootAnimFrame(line.c_str());
  }
  if (!sdsStarted) Sds200::begin();
}

void loop() {
  Sds200::loop();
  Battery::loop();
  Settings::loop();
  Ota::loop();
  Ui::loop();
  delay(2);
}
