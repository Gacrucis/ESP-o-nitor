#pragma once

// Firmware local credentials template.
//
// Copy this file to secrets.h (same folder) and put your real values there.
// secrets.h is ignored by git and never versioned.
//
// If you do not create secrets.h, the firmware builds with empty defaults: the ESP
// boots into the emergency AP and WiFi/OTA are configured from its web page.

#define SECRET_WIFI_SSID "YOUR_WIFI"
#define SECRET_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define SECRET_OTA_PASSWORD "YOUR_OTA_PASSWORD"
