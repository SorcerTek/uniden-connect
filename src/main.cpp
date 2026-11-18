// Uniden Connect
// Network remote for a Uniden SDS200 scanner on the Freenove ESP32-S3 3.5" display (FNK0104N).
//
//   - shows what the scanner is receiving (system / department / channel / frequency)
//   - touch buttons send scanner key presses, volume and squelch
//   - plays the scanner's audio stream on the board's speaker
//
// Settings: include/config.h    Wi-Fi: include/secrets.h
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include "config.h"
#include "secrets.h"
#include "display_st77922.h"
#include "touch_st77922.h"
#include "audio_out.h"
#include "rtsp_audio.h"
#include "sds200.h"
#include "ui.h"
#include "battery.h"
#include "power.h"

void setup() {
  Power::begin();                    // release pins held during power-off sleep
  Serial.begin(115200);
  delay(200);
  Serial.println("\nUniden Connect starting");

  if (!psramFound()) Serial.println("WARNING: PSRAM not found - check platformio.ini memory settings");

  if (!St77922::begin()) Serial.println("Display init failed");
  if (!Ui::begin()) Serial.println("UI init failed (not enough memory?)");
  Ui::showLogo("Starting...");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!Touch::begin()) Serial.println("Touch controller not found");
  if (!AudioOut::begin()) Serial.println("Audio init failed");
  Battery::begin();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // keeps audio latency low
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  RtspAudio::begin();                // its task waits for Wi-Fi by itself

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
      Serial.println(wifi ? "Scanner not answering yet - starting anyway" : "Wi-Fi not connected yet - check include/secrets.h");
      break;
    }
    if (!wifi) line = String("Connecting to Wi-Fi: ") + WIFI_SSID;
    else if (!online) line = String("Wi-Fi OK - looking for scanner at ") + SCANNER_IP;
    else line = "Scanner connected";
    Ui::bootAnimFrame(line.c_str());
  }
  if (!sdsStarted) Sds200::begin();
}

void loop() {
  Sds200::loop();
  Battery::loop();
  Ui::loop();
  delay(2);
}
