#pragma once
// OPTIONAL. Everything here can be set on the board's SETUP screen instead
// (KEYS > SETTINGS > WI-FI SETUP), and the board keeps it in its own flash.
//
// If you do fill this in (copy it to secrets.h), the values are copied into the
// board the first time it starts. secrets.h is in .gitignore so it never goes to
// GitHub, but a compiled firmware .bin WOULD contain them - empty this file
// before sharing a build.
#define WIFI_SSID     "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"
#define OTA_PASSWORD  "change-me"     // password for wireless (Wi-Fi) firmware updates
