#pragma once
#include <Arduino.h>

// SDS200 remote control over the network "virtual serial" UDP port (50536).
// Commands are plain text ending in '\r'. Status comes from GSI (XML).
namespace Sds200 {

struct Status {
  bool online = false;          // got a reply recently
  String model;                 // from MDL
  String mode;                  // ScannerInfo Mode, e.g. "Scan Mode"
  String vScreen;               // ScannerInfo V_Screen
  String favorite;              // MonitorList Name
  String system;
  String department;
  String site;
  String channel;               // channel / talkgroup name
  String freq;                  // frequency text, e.g. "154.4150MHz"
  String tgid;                  // talkgroup id (trunked)
  String mod;                   // NFM / FM / AM / ...
  String service;               // service type
  bool sysHold = false, deptHold = false, chanHold = false;
  bool chanAvoid = false;
  int vol = -1, sql = -1, sig = 0;
  String rssi;
  bool scannerMuted = false;
  String info1, info2;          // ViewDescription InfoArea1/2
  String popup;                 // PopupScreen text
  uint32_t version = 0;         // increments whenever anything changes
};

void begin();
void loop();                    // call often from loop()
const Status& status();

void sendRaw(const char* cmd);  // without trailing \r
void key(char code, char mode = 'P');   // KEY,<code>,<mode>  (P = press, L = long press)
void setVolume(int level);      // 0-29
void setSquelch(int level);     // 0-19

}  // namespace Sds200
