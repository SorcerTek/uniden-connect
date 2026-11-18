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

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nUniden Connect starting");

  if (!psramFound()) Serial.println("WARNING: PSRAM not found - check platformio.ini memory settings");

  if (!St77922::begin()) Serial.println("Display init failed");
  if (!Ui::begin()) Serial.println("UI init failed (not enough memory?)");
  Ui::showMessage("Starting...");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!Touch::begin()) Serial.println("Touch controller not found");
  if (!AudioOut::begin()) Serial.println("Audio init failed");

  Ui::showMessage("Connecting to Wi-Fi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // keeps audio latency low
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());
    Ui::showMessage("Wi-Fi connected", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Wi-Fi not connected yet - check include/secrets.h");
    Ui::showMessage("Wi-Fi not connected", "Check WIFI_SSID / WIFI_PASSWORD in include/secrets.h");
    delay(3000);
  }

  Sds200::begin();
  RtspAudio::begin();
}

void loop() {
  Sds200::loop();
  Ui::loop();
  delay(2);
}
