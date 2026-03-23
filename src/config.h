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

// How long to reuse cached weather before re-fetching (seconds)
#define WEATHER_CACHE_TTL_SEC 1800

// Onboard RGB (WS2812/NeoPixel) config.
// Common ESP32-S3 values are 48 (DevKit variants) or 38 (mini/zero variants).
#define RGB_LED_DATA_PIN 48
// Set to a GPIO number if your board requires a power-enable pin, else keep -1.
#define RGB_LED_POWER_PIN -1
#define RGB_LED_POWER_ON HIGH

// Keep USB serial alive briefly so logs are visible in monitor.
#define SERIAL_WAIT_MS 5000
#define POST_RENDER_LOG_MS 4000

// ---------------------------------------------------------------------------
// Screen enable / disable
// Set any to false to skip that screen entirely from the rotation.
// ---------------------------------------------------------------------------
#define ENABLE_WEATHER_SCREEN   true
#define ENABLE_MARKET_SCREEN    true
#define ENABLE_THOUGHTS_SCREEN  true
#define ENABLE_FOCUS_SCREEN     true
#define ENABLE_PRAYER_SCREEN    true
#define ENABLE_NEWS_SCREEN      true

// ---------------------------------------------------------------------------
// Per-screen deep-sleep wake intervals (seconds)
// ---------------------------------------------------------------------------
#define WEATHER_SCREEN_INTERVAL_SEC   60
#define MARKET_SCREEN_INTERVAL_SEC   120
#define THOUGHTS_SCREEN_INTERVAL_SEC 300
#define FOCUS_SCREEN_INTERVAL_SEC     60
#define PRAYER_SCREEN_INTERVAL_SEC   300
#define NEWS_SCREEN_INTERVAL_SEC     180

// ---------------------------------------------------------------------------
// Focus screen configuration
// ---------------------------------------------------------------------------
#define FOCUS_WORD  "Build."

// ---------------------------------------------------------------------------
// Prayer times configuration
// Calculation method (Aladhan API):
//   1 = University of Islamic Sciences, Karachi
//   2 = Islamic Society of North America (ISNA)
//   3 = Muslim World League (MWL)
//   4 = Umm Al-Qura University, Makkah
//   5 = Egyptian General Authority of Survey
// ---------------------------------------------------------------------------
#define PRAYER_CALC_METHOD 3

// ---------------------------------------------------------------------------
// News screen configuration
// NEWS_SOURCE: "hackernews" or "reddit"
// NEWS_SUBREDDIT: used when NEWS_SOURCE is "reddit"
// ---------------------------------------------------------------------------
#define NEWS_SOURCE      "hackernews"
#define NEWS_SUBREDDIT   "worldnews"

// ---------------------------------------------------------------------------
// OTA firmware update configuration
//
// The device checks raw.githubusercontent.com/{OWNER}/{REPO}/{BRANCH}/version.json
// on cold boot and when midnight is crossed.  If the SHA in that file differs
// from the SHA embedded in the running firmware at build time, the binary at
// the URL inside version.json is downloaded and flashed automatically.
//
// For PRIVATE repositories: define OTA_GITHUB_TOKEN in config_private.h
// (never commit a real token here).  A fine-grained PAT with read-only
// access to "Contents" and "Releases" is sufficient.
//   #define OTA_GITHUB_TOKEN "github_pat_..."
//
// Change OTA_BRANCH to "main" after merging.
// ---------------------------------------------------------------------------
#define OTA_REPO_OWNER  "Aadilkho"
#define OTA_REPO_NAME   "E-Paper-Desk-Dashboard"
#define OTA_BRANCH      "main"

// Minimum seconds between OTA checks (prevents hammering on rapid restarts)
#define OTA_CHECK_COOLDOWN_SEC 600
