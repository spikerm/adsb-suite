#pragma once

// Copy this file to include/config.h and edit the values below.
#define WIFI_SSID       "your-wifi"
#define WIFI_PASSWORD   "your-password"

#define MQTT_HOST       "192.168.1.10"
#define MQTT_PORT       1883
#define MQTT_USERNAME   ""
#define MQTT_PASSWORD   ""

#define MQTT_BASE_TOPIC "adsb/display"
#define DEVICE_NAME     "adsb-round-display"

// Radar range shown on the round LCD. Rotate the encoder on the radar page
// to switch between 25, 50, 100, 200 and 400 km.
#define DEFAULT_RADAR_RANGE_KM 100
