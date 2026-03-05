#pragma once

// Wi-Fi credentials
#ifdef __has_include
#if __has_include("config_private.h")
#include "config_private.h"
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// South Africa Standard Time = UTC+2, no DST.
#define UTC_OFFSET_SECONDS 7200

// Weather location - Plentify office area, Oude Molen Rd, Ndabeni
#define WEATHER_LATITUDE -33.9310f
#define WEATHER_LONGITUDE 18.4890f

// Onboard RGB (WS2812/NeoPixel) config.
// Common ESP32-S3 values are 48 (DevKit variants) or 38 (mini/zero variants).
#define RGB_LED_DATA_PIN 48
// Set to a GPIO number if your board requires a power-enable pin, else keep -1.
#define RGB_LED_POWER_PIN -1
#define RGB_LED_POWER_ON HIGH

// Wake interval: 30 seconds
#define DASHBOARD_UPDATE_INTERVAL_SEC 60

// Keep USB serial alive briefly so logs are visible in monitor.
#define SERIAL_WAIT_MS 5000
#define POST_RENDER_LOG_MS 4000
