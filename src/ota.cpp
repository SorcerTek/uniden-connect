#include "ota.h"
#include "config.h"
#include "settings.h"
#include "rtsp_audio.h"
#include "audio_out.h"
#include "calls.h"
#include "ui.h"
#include <ArduinoOTA.h>
#include <WiFi.h>

namespace Ota {

static bool s_on = false;

bool enabled() { return s_on; }

void begin() {
  String pw = Settings::otaPassword();
  if (!pw.length()) { Serial.println("Wireless updates off (no OTA password set)"); return; }
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(pw.c_str());
  ArduinoOTA.onStart([]() {
    Serial.println("OTA update starting");
    Calls::closeAll();
    RtspAudio::shutdown(800);
    AudioOut::setMuted(true);
    Ui::showMessage("Updating...", "0%");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int last = -1;
    int pct = total ? done * 100 / total : 0;
    if (pct != last && (pct % 5 == 0)) {
      last = pct;
      char b[16];
      snprintf(b, sizeof(b), "%d%%", pct);
      Ui::showMessage("Updating...", b);
    }
  });
  ArduinoOTA.onEnd([]() { Ui::showMessage("Update done", "Restarting..."); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error %d\n", e);
    Ui::showMessage("Update failed", "Restarting...");
    delay(1500);
    ESP.restart();
  });
  ArduinoOTA.begin();
  s_on = true;
  Serial.printf("Wireless updates on: %s.local\n", OTA_HOSTNAME);
}

void restart() {
  if (s_on) { ArduinoOTA.end(); s_on = false; }
  if (WiFi.status() == WL_CONNECTED) begin();
}

void loop() {
  if (!s_on) {
    // start once Wi-Fi is up
    static uint32_t last = 0;
    if (WiFi.status() == WL_CONNECTED && Settings::otaPassword().length() && millis() - last > 5000) { last = millis(); begin(); }
    return;
  }
  ArduinoOTA.handle();
}

}  // namespace Ota
