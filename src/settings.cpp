#include "settings.h"
#include "config.h"
#include <Preferences.h>
#if __has_include("secrets.h")
#include "secrets.h"      // optional: first-boot Wi-Fi / OTA password (copied into flash once)
#endif

namespace Settings {

static const uint32_t VERSION = 1;
static Data s_d;
static uint32_t s_changedAt = 0;

static void defaults() {
  memset(&s_d, 0, sizeof(s_d));
  s_d.spkVolume = SPEAKER_VOLUME_DEFAULT;
  s_d.muted = false;
  s_d.brightness = 100;
  s_d.dimAfterS = DIM_AFTER_S_DEFAULT;
  s_d.dimLevel = DIM_LEVEL_DEFAULT;
  s_d.record = true;
}

static String s_ssid, s_pass, s_ip, s_ota;
static int s_keyColor = 0;

static String getStr(const char* key, const String& def) {
  Preferences p;
  if (!p.begin("uconnect", true)) return def;
  String v = p.isKey(key) ? p.getString(key, def) : def;
  p.end();
  return v;
}
static void putStr(const char* key, const String& v) {
  Preferences p;
  if (!p.begin("uconnect", false)) return;
  p.putString(key, v);
  p.end();
}

String wifiSsid() { return s_ssid; }
String wifiPassword() { return s_pass; }
String scannerIp() { return s_ip; }
String otaPassword() { return s_ota; }
bool hasWifi() { return s_ssid.length() > 0; }
void setWifi(const String& ssid, const String& pw) { s_ssid = ssid; s_pass = pw; putStr("ssid", ssid); putStr("pass", pw); }
void forgetWifi() { setWifi("", ""); }
void setScannerIp(const String& ip) { s_ip = ip; putStr("sip", ip); }
void setOtaPassword(const String& pw) { s_ota = pw; putStr("otapw", pw); }
int keyColor() { return s_keyColor; }
void setKeyColor(int i) {
  s_keyColor = i;
  Preferences p;
  if (!p.begin("uconnect", false)) return;
  p.putUChar("keycol", (uint8_t)i);
  p.end();
}

static int s_screenLock = -1;
int screenLock() { return s_screenLock; }
void setScreenLock(int k) {
  s_screenLock = (k >= 0 && k <= 3) ? k : -1;
  Preferences p;
  if (!p.begin("uconnect", false)) return;
  p.putChar("rotlock", (int8_t)s_screenLock);
  p.end();
}

static void loadConnection() {
  s_ssid = getStr("ssid", "");
  s_pass = getStr("pass", "");
  s_ip = getStr("sip", SCANNER_IP);
  s_ota = getStr("otapw", "");
  {
    Preferences p;
    if (p.begin("uconnect", true)) {
      s_keyColor = p.getUChar("keycol", 0);
      s_screenLock = p.getChar("rotlock", -1);
      if (s_screenLock > 3) s_screenLock = -1;
      p.end();
    }
  }
  // First boot of this version: take the details from secrets.h if it was filled in.
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  if (!s_ssid.length() && strlen(WIFI_SSID) && strcmp(WIFI_SSID, "YourWiFiName") != 0) setWifi(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(OTA_PASSWORD)
  if (!s_ota.length() && strlen(OTA_PASSWORD) && strcmp(OTA_PASSWORD, "change-me") != 0) setOtaPassword(OTA_PASSWORD);
#endif
}

void begin() {
  loadConnection();
  defaults();
  Preferences p;
  if (!p.begin("uconnect", true)) return;     // nothing saved yet
  if (p.getUInt("ver", 0) == VERSION && p.getBytesLength("data") == sizeof(Data)) p.getBytes("data", &s_d, sizeof(Data));
  p.end();
  s_d.spkVolume = constrain(s_d.spkVolume, 0, 100);
  s_d.brightness = constrain(s_d.brightness, 10, 100);
  s_d.dimLevel = constrain(s_d.dimLevel, 0, 50);
}

Data& get() { return s_d; }
void changed() { s_changedAt = millis() | 1; }

void saveNow() {
  Preferences p;
  if (!p.begin("uconnect", false)) return;
  p.putUInt("ver", VERSION);
  p.putBytes("data", &s_d, sizeof(Data));
  p.end();
  s_changedAt = 0;
}

void loop() {
  if (s_changedAt && millis() - s_changedAt > 2000) saveNow();
}

}  // namespace Settings
