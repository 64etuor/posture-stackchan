#pragma once

// Copy this file to include/secrets.h and fill in real values.
// include/secrets.h is git-ignored and must never be committed.
//
// Not used by stage 1 (offline hardware bring-up); stage 2 adds Wi-Fi.

#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"

// Device identity advertised on the network.
#define DEVICE_NAME "posture-stackchan"

// Reserved for the MQTT migration described in spec section 8.
#define MQTT_HOST "192.168.0.10"
#define MQTT_PORT 1883
