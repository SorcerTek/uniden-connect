#pragma once
#include <Arduino.h>

// User settings kept in flash (NVS) so they survive restarts and power-off.
namespace Settings {

static const int NUM_FAVS = 6;

struct Fav {
  bool used;
  uint32_t sysIndex;       // scanner's System Index (from GSI)
  uint32_t deptIndex;      // Department Index, or 0xFFFFFFFF = whole system
  char sys[48];
  char dept[64];
};

struct Data {
  uint8_t spkVolume;       // board speaker 0-100
  bool muted;
  uint8_t brightness;      // normal screen brightness 10-100
  uint16_t dimAfterS;      // dim after this many idle seconds (0 = never)
  uint8_t dimLevel;        // dimmed brightness 0-50 (0 = screen off)
  bool record;             // record calls to the SD card
  Fav favs[NUM_FAVS];
};

// Connection details (set on the SETUP screen; kept separately from Data).
String wifiSsid();
String wifiPassword();
String scannerIp();        // defaults to SCANNER_IP from config.h
String otaPassword();      // "" = wireless updates off
bool hasWifi();
void setWifi(const String& ssid, const String& password);
void forgetWifi();
void setScannerIp(const String& ip);
void setOtaPassword(const String& pw);
int keyColor();            // index into the UI's key-light colour list
void setKeyColor(int i);
int screenLock();          // Tab5 screen rotation: -1 = automatic, 0-3 = locked at that orientation
void setScreenLock(int k);

void begin();              // load (or defaults)
Data& get();
void changed();            // call after changing anything; saved a couple of seconds later
void loop();               // does the delayed save
void saveNow();

}  // namespace Settings
