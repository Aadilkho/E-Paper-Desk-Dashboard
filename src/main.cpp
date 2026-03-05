#include <Arduino.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>
#include <cctype>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <cstring>

#include <ArduinoJson.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <GxEPD2_BW.h>

#include "config.h"

// ESP32-S3 DevKitM-1 -> Waveshare 2.9" e-paper (296x128)
static constexpr uint8_t EPD_CS = 7;
static constexpr uint8_t EPD_DC = 6;
static constexpr uint8_t EPD_RST = 5;
static constexpr uint8_t EPD_BUSY = 4;
static constexpr uint8_t EPD_SCK = 12;
static constexpr uint8_t EPD_MOSI = 11;

static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 12000;
static constexpr uint8_t RGB_LED_BRIGHTNESS = 40;
static constexpr uint32_t RGB_LED_STEP_MS = 12;
static constexpr uint32_t RGB_SPECTRUM_PERIOD_MS = 12000;
static constexpr bool CONTINUOUS_RGB_MODE = true;
static constexpr int16_t RGB_LED_PIN = RGB_LED_DATA_PIN;
static constexpr int16_t RGB_LED_PWR_PIN = RGB_LED_POWER_PIN;

GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(
    GxEPD2_290_T94_V2(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
RTC_DATA_ATTR uint32_t bootCount = 0;

struct WeatherData {
  bool valid;
  float temperatureC;
  float apparentTemperatureC;
  float humidityPercent;
  float uvIndex;
  int rainChancePercent;
  uint16_t weatherCode;
};

struct MarketData {
  bool valid;
  bool hasBtc;
  bool hasEth;
  bool hasGold;
  bool hasOil;
  bool hasZarUsd;
  bool hasZarMur;
  float btcUsd;
  float ethUsd;
  float goldUsd;
  float oilUsd;
  float zarUsd;
  float zarMur;
  char error[96];
};

struct ThoughtData {
  bool valid;
  char text[256];
  char source[48];
  char error[96];
};

RTC_DATA_ATTR MarketData lastMarketData = {
    false, false, false, false, false, false, false, 0.0f, 0.0f,
    0.0f,  0.0f,  0.0f,  0.0f,  ""};
static uint8_t activeCycle = 0;
static uint32_t lastCycleRefreshMs = 0;
static bool continuousModeActive = false;

void logLine(const char *msg) {
  char line[272];
  snprintf(line, sizeof(line), "[%10lu ms] %s", static_cast<unsigned long>(millis()),
           msg);
  Serial.println(line);
  Serial0.println(line);
}

void logf(const char *fmt, ...) {
  char buf[224];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logLine(buf);
}

void setRgbLed(uint8_t red, uint8_t green, uint8_t blue) {
  if (RGB_LED_PIN < 0) {
    (void)red;
    (void)green;
    (void)blue;
    return;
  }
  neopixelWrite(static_cast<uint8_t>(RGB_LED_PIN), red, green, blue);
}

void hueToRgb(uint16_t hue, uint8_t &red, uint8_t &green, uint8_t &blue) {
  const uint8_t segment = static_cast<uint8_t>(hue / 256U);
  const uint8_t x = static_cast<uint8_t>(hue % 256U);
  switch (segment) {
  case 0:
    red = 255;
    green = x;
    blue = 0;
    break;
  case 1:
    red = static_cast<uint8_t>(255 - x);
    green = 255;
    blue = 0;
    break;
  case 2:
    red = 0;
    green = 255;
    blue = x;
    break;
  case 3:
    red = 0;
    green = static_cast<uint8_t>(255 - x);
    blue = 255;
    break;
  case 4:
    red = x;
    green = 0;
    blue = 255;
    break;
  default:
    red = 255;
    green = 0;
    blue = static_cast<uint8_t>(255 - x);
    break;
  }
}

void setSpectrumHue(uint16_t hue) {
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  hueToRgb(hue, red, green, blue);
  red = static_cast<uint8_t>((static_cast<uint16_t>(red) * RGB_LED_BRIGHTNESS) / 255U);
  green = static_cast<uint8_t>((static_cast<uint16_t>(green) * RGB_LED_BRIGHTNESS) / 255U);
  blue = static_cast<uint8_t>((static_cast<uint16_t>(blue) * RGB_LED_BRIGHTNESS) / 255U);
  setRgbLed(red, green, blue);
}

void updateContinuousRgb() {
  if (RGB_LED_PIN < 0) {
    return;
  }
  const uint32_t phaseMs = millis() % RGB_SPECTRUM_PERIOD_MS;
  const uint16_t hue =
      static_cast<uint16_t>((phaseMs * 1536UL) / RGB_SPECTRUM_PERIOD_MS) % 1536U;
  setSpectrumHue(hue);
}

void runRgbSpectrum(uint32_t durationMs) {
  if (durationMs == 0 || RGB_LED_PIN < 0) {
    return;
  }
  const uint32_t startMs = millis();
  while (millis() - startMs < durationMs) {
    const uint32_t elapsed = millis() - startMs;
    const uint16_t hue =
        static_cast<uint16_t>((elapsed * 1536UL) / durationMs) % 1536U;
    setSpectrumHue(hue);
    delay(RGB_LED_STEP_MS);
  }
  setRgbLed(0, 0, 0);
}

void initRgbLed() {
  logf("RGB init. dataPin=%d powerPin=%d", static_cast<int>(RGB_LED_PIN),
       static_cast<int>(RGB_LED_PWR_PIN));
  if (RGB_LED_PWR_PIN >= 0) {
    pinMode(static_cast<uint8_t>(RGB_LED_PWR_PIN), OUTPUT);
    digitalWrite(static_cast<uint8_t>(RGB_LED_PWR_PIN), RGB_LED_POWER_ON);
    logf("RGB power pin %d -> %s", static_cast<int>(RGB_LED_PWR_PIN),
         RGB_LED_POWER_ON == HIGH ? "HIGH" : "LOW");
  }
  setRgbLed(0, 0, 0);
}

void setMarketError(MarketData &market, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(market.error, sizeof(market.error), fmt, args);
  va_end(args);
  logf("Market error detail: %s", market.error);
}

const char *wakeupCauseStr(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
  case ESP_SLEEP_WAKEUP_UNDEFINED:
    return "UNDEFINED";
  case ESP_SLEEP_WAKEUP_TIMER:
    return "TIMER";
  case ESP_SLEEP_WAKEUP_EXT0:
    return "EXT0";
  case ESP_SLEEP_WAKEUP_EXT1:
    return "EXT1";
  case ESP_SLEEP_WAKEUP_TOUCHPAD:
    return "TOUCHPAD";
  case ESP_SLEEP_WAKEUP_ULP:
    return "ULP";
  case ESP_SLEEP_WAKEUP_GPIO:
    return "GPIO";
  case ESP_SLEEP_WAKEUP_UART:
    return "UART";
  default:
    return "OTHER";
  }
}

const char *wifiStatusStr(wl_status_t status) {
  switch (status) {
  case WL_IDLE_STATUS:
    return "IDLE";
  case WL_NO_SSID_AVAIL:
    return "NO_SSID";
  case WL_SCAN_COMPLETED:
    return "SCAN_DONE";
  case WL_CONNECTED:
    return "CONNECTED";
  case WL_CONNECT_FAILED:
    return "CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "DISCONNECTED";
  default:
    return "UNKNOWN";
  }
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    logf("Wi-Fi already connected. IP=%s RSSI=%d dBm",
         WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  logf("Wi-Fi connect start. SSID='%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t started = millis();
  wl_status_t lastStatus = WiFi.status();
  uint32_t lastLogMs = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    wl_status_t s = WiFi.status();
    uint32_t elapsed = millis() - started;
    if (s != lastStatus || (millis() - lastLogMs) > 1000) {
      logf("Wi-Fi state: %s (%d), elapsed=%lu ms", wifiStatusStr(s),
           static_cast<int>(s), static_cast<unsigned long>(elapsed));
      lastStatus = s;
      lastLogMs = millis();
    }
    delay(250);
  }
  const bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {
    logf("Wi-Fi connected. IP=%s RSSI=%d dBm", WiFi.localIP().toString().c_str(),
         WiFi.RSSI());
  } else {
    logf("Wi-Fi connect timeout after %lu ms. Final state=%s (%d)",
         static_cast<unsigned long>(millis() - started),
         wifiStatusStr(WiFi.status()), static_cast<int>(WiFi.status()));
  }
  return ok;
}

void syncClock() {
  logf("NTP sync start. UTC offset seconds=%d", UTC_OFFSET_SECONDS);
  configTime(UTC_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com",
             "time.nist.gov");

  time_t now = 0;
  const uint32_t started = millis();
  while (now < 1700000000 && millis() - started < 15000) {
    time(&now);
    if (((millis() - started) % 2000) < 200) {
      logf("NTP waiting... now=%lld elapsed=%lu ms", static_cast<long long>(now),
           static_cast<unsigned long>(millis() - started));
    }
    delay(200);
  }

  if (now > 1700000000) {
    struct tm utcinfo;
    struct tm timeinfo;
    gmtime_r(&now, &utcinfo);
    localtime_r(&now, &timeinfo);
    char utcBuf[32];
    char buf[32];
    strftime(utcBuf, sizeof(utcBuf), "%Y-%m-%d %H:%M:%S", &utcinfo);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    logf("NTP sync ok. UTC: %s Local: %s", utcBuf, buf);
  } else {
    logf("NTP sync failed after %lu ms", static_cast<unsigned long>(millis() - started));
  }
}

bool hasValidTime() {
  time_t now;
  time(&now);
  return now > 1700000000;
}

void resetWeatherData(WeatherData &weather) {
  weather.valid = false;
  weather.temperatureC = 0.0f;
  weather.apparentTemperatureC = 0.0f;
  weather.humidityPercent = 0.0f;
  weather.uvIndex = -1.0f;
  weather.rainChancePercent = -1;
  weather.weatherCode = 65535;
}

bool fetchWeatherOpenMeteo(WeatherData &weather) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(WEATHER_LATITUDE, 4);
  url += "&longitude=";
  url += String(WEATHER_LONGITUDE, 4);
  url +=
      "&current=temperature_2m,apparent_temperature,weather_code,relative_humidity_2m,uv_index";
  url += "&hourly=precipitation_probability";
  url += "&forecast_days=1&timezone=auto";
  logf("Weather[OpenMeteo] URL: %s", url.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    logLine("Weather[OpenMeteo] request begin() failed.");
    return false;
  }

  const int code = http.GET();
  logf("Weather[OpenMeteo] HTTP GET returned: %d", code);
  if (code <= 0) {
    logf("Weather[OpenMeteo] transport error: %s", http.errorToString(code).c_str());
    http.end();
    return false;
  }
  if (code != HTTP_CODE_OK) {
    logf("Weather[OpenMeteo] HTTP status: %d", code);
    String errorBody = http.getString();
    if (errorBody.length() > 0) {
      logf("Weather[OpenMeteo] error body: %s", errorBody.substring(0, 160).c_str());
    }
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  logf("Weather[OpenMeteo] payload bytes: %u",
       static_cast<unsigned int>(payload.length()));

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    logf("Weather[OpenMeteo] JSON parse error: %s", err.c_str());
    return false;
  }

  JsonVariant current = doc["current"];
  if (current.isNull()) {
    logLine("Weather[OpenMeteo] JSON missing 'current' object.");
    return false;
  }
  if (current["temperature_2m"].isNull() || current["apparent_temperature"].isNull() ||
      current["relative_humidity_2m"].isNull() || current["weather_code"].isNull()) {
    logLine("Weather[OpenMeteo] JSON missing required current fields.");
    return false;
  }

  weather.temperatureC = current["temperature_2m"] | 0.0f;
  weather.apparentTemperatureC = current["apparent_temperature"] | 0.0f;
  weather.humidityPercent = current["relative_humidity_2m"] | 0.0f;
  weather.uvIndex = current["uv_index"] | -1.0f;
  weather.weatherCode = static_cast<uint16_t>(current["weather_code"] | 65535);

  JsonVariant rainValue = doc["hourly"]["precipitation_probability"][0];
  if (!rainValue.isNull()) {
    int rainPercent = static_cast<int>(rainValue | -1);
    if (rainPercent < 0) {
      rainPercent = -1;
    } else if (rainPercent > 100) {
      rainPercent = 100;
    }
    weather.rainChancePercent = rainPercent;
  }

  weather.valid = true;
  logf("Weather[OpenMeteo] ok: %.1fC feels %.1fC hum %.0f%% uv %.1f rain %d%% code %u",
       weather.temperatureC, weather.apparentTemperatureC,
       weather.humidityPercent, weather.uvIndex, weather.rainChancePercent,
       static_cast<unsigned int>(weather.weatherCode));
  return true;
}

bool fetchWeather(WeatherData &weather) {
  logLine("Weather fetch start");
  resetWeatherData(weather);

  if (fetchWeatherOpenMeteo(weather)) {
    return true;
  }
  logLine("Weather[OpenMeteo] failed");
  return false;
}

void resetThoughtData(ThoughtData &thought) {
  thought.valid = false;
  thought.text[0] = '\0';
  thought.source[0] = '\0';
  thought.error[0] = '\0';
}

void normalizeThoughtText(const char *in, char *out, size_t outSize) {
  if (outSize == 0) {
    return;
  }
  size_t j = 0;
  bool prevSpace = true;
  for (size_t i = 0; in[i] != '\0' && j + 1 < outSize; ++i) {
    char c = in[i];
    if (c == '\r' || c == '\n' || c == '\t') {
      c = ' ';
    }
    if (static_cast<unsigned char>(c) < 32) {
      continue;
    }
    if (static_cast<unsigned char>(c) > 126) {
      c = '?';
    }
    if (c == ' ') {
      if (prevSpace) {
        continue;
      }
      prevSpace = true;
    } else {
      prevSpace = false;
    }
    out[j++] = c;
  }
  while (j > 0 && out[j - 1] == ' ') {
    --j;
  }
  out[j] = '\0';
}

void setClassicalThought(ThoughtData &thought, bool clockValid) {
  struct ClassicalQuote {
    const char *text;
    const char *source;
  };
  static const ClassicalQuote kQuotes[] = {
      {"What you seek is seeking you.", "Rumi"},
      {"The wound is the place where the light enters you.", "Rumi"},
      {"Let yourself be silently drawn by what you truly love.", "Rumi"},
      {"Raise your words, not your voice.", "Rumi"},
      {"Love is the bridge between you and everything.", "Rumi"},
      {"Yesterday I was clever, so I wanted to change the world.", "Rumi"},
      {"Knowledge without action is wastefulness.", "Imam al-Ghazali"},
      {"Action without knowledge is foolishness.", "Imam al-Ghazali"},
      {"Patience turns hardship into wisdom.", "Imam al-Ghazali"}};
  constexpr size_t kCount = sizeof(kQuotes) / sizeof(kQuotes[0]);

  uint32_t seed = bootCount;
  if (clockValid) {
    time_t now;
    time(&now);
    seed += static_cast<uint32_t>(now / 3600);
  }
  const size_t idx = seed % kCount;
  strncpy(thought.text, kQuotes[idx].text, sizeof(thought.text) - 1);
  thought.text[sizeof(thought.text) - 1] = '\0';
  strncpy(thought.source, kQuotes[idx].source, sizeof(thought.source) - 1);
  thought.source[sizeof(thought.source) - 1] = '\0';
  thought.valid = true;
  thought.error[0] = '\0';
}

bool fetchShowerThought(ThoughtData &thought) {
  resetThoughtData(thought);
  const String url = "https://www.reddit.com/r/Showerthoughts/top.json?t=day&limit=25";
  logf("Thought[Reddit] URL: %s", url.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    strncpy(thought.error, "begin() failed", sizeof(thought.error) - 1);
    thought.error[sizeof(thought.error) - 1] = '\0';
    logLine("Thought[Reddit] request begin() failed.");
    return false;
  }
  http.addHeader("User-Agent", "EpaperDashboard/1.0 (Showerthoughts fetch)");
  http.addHeader("Accept", "application/json");

  const int code = http.GET();
  logf("Thought[Reddit] HTTP GET returned: %d", code);
  if (code <= 0) {
    strncpy(thought.error, http.errorToString(code).c_str(), sizeof(thought.error) - 1);
    thought.error[sizeof(thought.error) - 1] = '\0';
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  logf("Thought[Reddit] payload bytes: %u", static_cast<unsigned int>(payload.length()));
  if (code != HTTP_CODE_OK) {
    snprintf(thought.error, sizeof(thought.error), "HTTP %d", code);
    if (payload.length() > 0) {
      logf("Thought[Reddit] error body: %s", payload.substring(0, 180).c_str());
    }
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    snprintf(thought.error, sizeof(thought.error), "JSON: %s", err.c_str());
    logf("Thought[Reddit] JSON parse error: %s", err.c_str());
    return false;
  }

  JsonArray children = doc["data"]["children"];
  if (children.isNull() || children.size() == 0) {
    strncpy(thought.error, "no posts", sizeof(thought.error) - 1);
    thought.error[sizeof(thought.error) - 1] = '\0';
    logLine("Thought[Reddit] no posts in response.");
    return false;
  }

  for (JsonObject child : children) {
    JsonObject post = child["data"];
    if (post.isNull()) {
      continue;
    }
    if ((post["stickied"] | false) || (post["over_18"] | false)) {
      continue;
    }
    const char *title = post["title"] | "";
    if (title[0] == '\0') {
      continue;
    }
    normalizeThoughtText(title, thought.text, sizeof(thought.text));
    if (strlen(thought.text) < 16) {
      continue;
    }
    strncpy(thought.source, "r/Showerthoughts", sizeof(thought.source) - 1);
    thought.source[sizeof(thought.source) - 1] = '\0';
    thought.valid = true;
    thought.error[0] = '\0';
    logf("Thought[Reddit] selected len=%u", static_cast<unsigned int>(strlen(thought.text)));
    return true;
  }

  strncpy(thought.error, "no suitable post", sizeof(thought.error) - 1);
  thought.error[sizeof(thought.error) - 1] = '\0';
  logLine("Thought[Reddit] posts present but none suitable.");
  return false;
}

void selectThought(ThoughtData &thought, bool wifiConnected, bool clockValid) {
  ThoughtData classicalThought = {false, "", "", ""};
  setClassicalThought(classicalThought, clockValid);

  if (!wifiConnected) {
    thought = classicalThought;
    return;
  }

  ThoughtData redditThought = {false, "", "", ""};
  const bool hasReddit = fetchShowerThought(redditThought);
  if (!hasReddit) {
    thought = classicalThought;
    strncpy(thought.error, redditThought.error, sizeof(thought.error) - 1);
    thought.error[sizeof(thought.error) - 1] = '\0';
    return;
  }

  const bool chooseReddit = (esp_random() & 1U) == 0U;
  thought = chooseReddit ? redditThought : classicalThought;
}

bool fetchMarketPrices(MarketData &market) {
  auto resetMarket = [&market]() {
    market.valid = false;
    market.hasBtc = false;
    market.hasEth = false;
    market.hasGold = false;
    market.hasOil = false;
    market.hasZarUsd = false;
    market.hasZarMur = false;
    market.btcUsd = 0.0f;
    market.ethUsd = 0.0f;
    market.goldUsd = 0.0f;
    market.oilUsd = 0.0f;
    market.zarUsd = 0.0f;
    market.zarMur = 0.0f;
    market.error[0] = '\0';
  };

  auto finalizeMarket = [&market]() {
    market.hasZarUsd = market.zarUsd > 0.0f;
    market.hasZarMur = market.zarMur > 0.0f;
    market.valid = market.hasBtc || market.hasEth || market.hasGold || market.hasOil ||
                   market.hasZarUsd || market.hasZarMur;
  };

  auto httpGetWithLogs = [](const char *tag, const String &url, String &payload,
                            int &codeOut, String &transportErr) -> bool {
    logf("%s URL: %s", tag, url.c_str());
    payload = "";
    transportErr = "";
    codeOut = 0;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
      logf("%s begin() failed", tag);
      transportErr = "begin() failed";
      return false;
    }
    http.addHeader("User-Agent", "Mozilla/5.0");
    http.addHeader("Accept", "*/*");
    http.addHeader("Accept-Encoding", "identity");

    const int code = http.GET();
    codeOut = code;
    logf("%s HTTP GET returned: %d", tag, code);
    if (code <= 0) {
      transportErr = http.errorToString(code);
      logf("%s transport error: %s", tag, transportErr.c_str());
      http.end();
      return false;
    }

    payload = http.getString();
    http.end();
    logf("%s payload bytes: %u", tag, static_cast<unsigned int>(payload.length()));
    if (code != HTTP_CODE_OK) {
      if (payload.length() > 0) {
        logf("%s error body: %s", tag, payload.substring(0, 180).c_str());
      }
      return false;
    }
    return true;
  };

  auto tryYahooAll = [&market, &httpGetWithLogs]() -> bool {
    String payload;
    String transportErr;
    int code = 0;
    const String url =
        "https://query1.finance.yahoo.com/v7/finance/quote?symbols=BTC-USD,ETH-USD,GC=F,CL=F,USDZAR=X,USDMUR=X";
    if (!httpGetWithLogs("Market[Yahoo]", url, payload, code, transportErr)) {
      if (code <= 0) {
        setMarketError(market, "Yahoo transport: %s", transportErr.c_str());
      } else {
        setMarketError(market, "Yahoo HTTP %d", code);
      }
      return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      setMarketError(market, "Yahoo JSON: %s", err.c_str());
      return false;
    }

    JsonArray results = doc["quoteResponse"]["result"];
    if (results.isNull() || results.size() == 0) {
      setMarketError(market, "Yahoo: no quote results");
      return false;
    }
    logf("Market[Yahoo] symbols returned: %u",
         static_cast<unsigned int>(results.size()));

    float usdZar = 0.0f;
    float usdMur = 0.0f;
    bool hasUsdZar = false;
    bool hasUsdMur = false;
    for (JsonObject q : results) {
      const char *symbol = q["symbol"] | "";
      float price = q["regularMarketPrice"] | NAN;
      logf("Market[Yahoo] raw quote symbol=%s price=%.6f", symbol, price);
      if (isnan(price)) {
        continue;
      }
      if (strcmp(symbol, "BTC-USD") == 0) {
        market.btcUsd = price;
        market.hasBtc = true;
      } else if (strcmp(symbol, "ETH-USD") == 0) {
        market.ethUsd = price;
        market.hasEth = true;
      } else if (strcmp(symbol, "GC=F") == 0) {
        market.goldUsd = price;
        market.hasGold = true;
      } else if (strcmp(symbol, "CL=F") == 0) {
        market.oilUsd = price;
        market.hasOil = true;
      } else if (strcmp(symbol, "USDZAR=X") == 0) {
        usdZar = price;
        hasUsdZar = true;
      } else if (strcmp(symbol, "USDMUR=X") == 0) {
        usdMur = price;
        hasUsdMur = true;
      }
    }

    if (hasUsdZar && usdZar > 0.0f) {
      market.zarUsd = usdZar;
    }
    if (hasUsdZar && hasUsdMur && usdZar > 0.0f) {
      market.zarMur = usdMur / usdZar;
    }
    return true;
  };

  auto tryBinanceCrypto = [&market, &httpGetWithLogs]() {
    auto fetchOne = [&market, &httpGetWithLogs](const char *tag,
                                                const char *symbol,
                                                float &out,
                                                bool &flag) {
      String payload;
      String transportErr;
      int code = 0;
      String url = "https://api.binance.com/api/v3/ticker/price?symbol=";
      url += symbol;
      if (!httpGetWithLogs(tag, url, payload, code, transportErr)) {
        return;
      }
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        logf("%s JSON parse error: %s", tag, err.c_str());
        return;
      }
      const char *priceStr = doc["price"] | "";
      float price = static_cast<float>(atof(priceStr));
      if (price > 0.0f) {
        out = price;
        flag = true;
        logf("%s parsed price=%.4f", tag, price);
      } else {
        logf("%s parsed invalid price='%s'", tag, priceStr);
      }
    };

    if (!market.hasBtc) {
      fetchOne("Market[Binance][BTC]", "BTCUSDT", market.btcUsd, market.hasBtc);
    }
    if (!market.hasEth) {
      fetchOne("Market[Binance][ETH]", "ETHUSDT", market.ethUsd, market.hasEth);
    }
  };

  auto tryErApiFx = [&market, &httpGetWithLogs]() {
    if (market.hasZarUsd && market.hasZarMur) {
      return;
    }

    String payload;
    String transportErr;
    int code = 0;
    const String url = "https://open.er-api.com/v6/latest/USD";
    if (!httpGetWithLogs("Market[ER-API][FX]", url, payload, code, transportErr)) {
      return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      logf("Market[ER-API][FX] JSON parse error: %s", err.c_str());
      return;
    }

    JsonVariant rates = doc["rates"];
    if (rates.isNull()) {
      logLine("Market[ER-API][FX] missing rates object");
      return;
    }

    const float usdZar = rates["ZAR"] | 0.0f;
    const float usdMur = rates["MUR"] | 0.0f;
    logf("Market[ER-API][FX] raw USDZAR=%.6f USDMUR=%.6f", usdZar, usdMur);
    if (usdZar > 0.0f) {
      market.zarUsd = usdZar;
      market.hasZarUsd = true;
    }
    if (usdZar > 0.0f && usdMur > 0.0f) {
      market.zarMur = usdMur / usdZar;
      market.hasZarMur = true;
    }
  };

  auto tryStooqCommodities = [&market, &httpGetWithLogs]() {
    if (market.hasGold && market.hasOil) {
      return;
    }

    auto fetchOne = [&httpGetWithLogs](const char *tag, const char *symbol,
                                       float &out, bool &flag) {
      String payload;
      String transportErr;
      int code = 0;
      String url = "https://stooq.com/q/l/?s=";
      url += symbol;
      url += "&f=sd2t2ohlcv&h&e=csv";
      if (!httpGetWithLogs(tag, url, payload, code, transportErr)) {
        return;
      }

      bool sawDataLine = false;
      int start = 0;
      while (start < payload.length()) {
        int end = payload.indexOf('\n', start);
        if (end < 0) {
          end = payload.length();
        }
        String line = payload.substring(start, end);
        line.trim();
        start = end + 1;
        if (line.length() == 0 || line.startsWith("Symbol")) {
          continue;
        }

        sawDataLine = true;
        logf("%s raw line: %s", tag, line.c_str());

        String fields[8];
        int fieldIdx = 0;
        int p = 0;
        while (fieldIdx < 8) {
          int comma = line.indexOf(',', p);
          if (comma < 0) {
            fields[fieldIdx++] = line.substring(p);
            break;
          }
          fields[fieldIdx++] = line.substring(p, comma);
          p = comma + 1;
        }
        if (fieldIdx < 7) {
          continue;
        }

        for (int i = 0; i < fieldIdx; ++i) {
          fields[i].trim();
          fields[i].replace("\"", "");
        }

        float closePrice = fields[6].toFloat();
        if (closePrice <= 0.0f) {
          float openPrice = fields[3].toFloat();
          float highPrice = fields[4].toFloat();
          float lowPrice = fields[5].toFloat();
          if (openPrice > 0.0f) {
            closePrice = openPrice;
          } else if (highPrice > 0.0f) {
            closePrice = highPrice;
          } else if (lowPrice > 0.0f) {
            closePrice = lowPrice;
          }
        }

        if (closePrice > 0.0f) {
          out = closePrice;
          flag = true;
          logf("%s parsed price=%.4f", tag, closePrice);
          return;
        }
      }

      if (!sawDataLine) {
        logf("%s no data rows in response", tag);
      } else {
        logf("%s data rows present but no valid numeric price", tag);
      }
    };

    if (!market.hasGold) {
      fetchOne("Market[Stooq][Gold]", "gc.f", market.goldUsd, market.hasGold);
    }
    if (!market.hasOil) {
      fetchOne("Market[Stooq][Oil]", "cl.f", market.oilUsd, market.hasOil);
    }
  };

  logLine("Market fetch start");
  resetMarket();

  const bool yahooOk = tryYahooAll();
  if (!yahooOk) {
    logLine("Market[Yahoo] failed, trying free fallback providers...");
  }
  tryBinanceCrypto();
  tryErApiFx();
  tryStooqCommodities();

  finalizeMarket();
  logf("Market flags BTC:%d ETH:%d GOLD:%d OIL:%d USDZAR:%d ZARMUR:%d",
       market.hasBtc, market.hasEth, market.hasGold, market.hasOil, market.hasZarUsd,
       market.hasZarMur);

  if (market.valid) {
    market.error[0] = '\0';
    logf("Market values BTC %.2f ETH %.2f Gold %.2f Oil %.2f USDZAR %.5f ZARMUR %.5f",
         market.btcUsd, market.ethUsd, market.goldUsd, market.oilUsd,
         market.zarUsd, market.zarMur);
  } else if (market.error[0] == '\0') {
    setMarketError(market, "all providers failed");
  }

  return market.valid;
}

enum class WeatherIcon : uint8_t { Sun, Cloud, Rain, Storm, Snow, Fog, Unknown };

WeatherIcon iconFromWeatherCode(uint16_t code) {
  if (code <= 99) { // Open-Meteo weather codes
    if (code == 0) {
      return WeatherIcon::Sun;
    }
    if (code == 1 || code == 2 || code == 3) {
      return WeatherIcon::Cloud;
    }
    if (code == 45 || code == 48) {
      return WeatherIcon::Fog;
    }
    if (code == 95 || code == 96 || code == 99) {
      return WeatherIcon::Storm;
    }
    if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 ||
        code == 86) {
      return WeatherIcon::Snow;
    }
    if (code == 51 || code == 53 || code == 55 || code == 56 || code == 57 ||
        code == 61 || code == 63 || code == 65 || code == 66 || code == 67 ||
        code == 80 || code == 81 || code == 82) {
      return WeatherIcon::Rain;
    }
    return WeatherIcon::Unknown;
  }

  // Generic weather condition IDs (legacy mapping retained)
  if (code == 800) {
    return WeatherIcon::Sun;
  }
  if (code >= 801 && code <= 804) {
    return WeatherIcon::Cloud;
  }
  if (code >= 701 && code <= 781) {
    return WeatherIcon::Fog;
  }
  if (code >= 200 && code <= 232) {
    return WeatherIcon::Storm;
  }
  if (code >= 600 && code <= 622) {
    return WeatherIcon::Snow;
  }
  if (code >= 300 && code <= 531) {
    return WeatherIcon::Rain;
  }
  return WeatherIcon::Unknown;
}

void drawSunIcon(int16_t x, int16_t y) {
  static const int8_t rays[8][2] = {{0, -5}, {4, -4}, {5, 0}, {4, 4},
                                     {0, 5},  {-4, 4}, {-5, 0}, {-4, -4}};
  const int16_t cx = x + 10;
  const int16_t cy = y + 9;
  display.drawCircle(cx, cy, 4, GxEPD_BLACK);
  display.fillCircle(cx, cy, 2, GxEPD_BLACK);
  for (uint8_t i = 0; i < 8; ++i) {
    display.drawLine(cx + rays[i][0], cy + rays[i][1], cx + (rays[i][0] * 7) / 5,
                     cy + (rays[i][1] * 7) / 5, GxEPD_BLACK);
  }
}

void drawCloudBase(int16_t x, int16_t y) {
  display.drawCircle(x + 6, y + 10, 4, GxEPD_BLACK);
  display.drawCircle(x + 11, y + 8, 5, GxEPD_BLACK);
  display.drawCircle(x + 16, y + 10, 4, GxEPD_BLACK);
  display.drawRoundRect(x + 4, y + 10, 14, 6, 2, GxEPD_BLACK);
}

void drawRainIcon(int16_t x, int16_t y) {
  drawCloudBase(x, y);
  display.drawLine(x + 7, y + 15, x + 5, y + 18, GxEPD_BLACK);
  display.drawLine(x + 11, y + 15, x + 9, y + 18, GxEPD_BLACK);
  display.drawLine(x + 15, y + 15, x + 13, y + 18, GxEPD_BLACK);
}

void drawStormIcon(int16_t x, int16_t y) {
  drawCloudBase(x, y);
  display.drawLine(x + 10, y + 14, x + 8, y + 18, GxEPD_BLACK);
  display.drawLine(x + 8, y + 18, x + 12, y + 18, GxEPD_BLACK);
  display.drawLine(x + 12, y + 18, x + 10, y + 22, GxEPD_BLACK);
}

void drawSnowIcon(int16_t x, int16_t y) {
  drawCloudBase(x, y);
  display.drawLine(x + 8, y + 16, x + 8, y + 20, GxEPD_BLACK);
  display.drawLine(x + 6, y + 18, x + 10, y + 18, GxEPD_BLACK);
  display.drawLine(x + 14, y + 16, x + 14, y + 20, GxEPD_BLACK);
  display.drawLine(x + 12, y + 18, x + 16, y + 18, GxEPD_BLACK);
}

void drawFogIcon(int16_t x, int16_t y) {
  drawCloudBase(x, y - 2);
  display.drawLine(x + 4, y + 14, x + 18, y + 14, GxEPD_BLACK);
  display.drawLine(x + 3, y + 17, x + 17, y + 17, GxEPD_BLACK);
}

void drawUnknownIcon(int16_t x, int16_t y) {
  display.drawCircle(x + 11, y + 10, 7, GxEPD_BLACK);
  display.drawLine(x + 8, y + 7, x + 14, y + 13, GxEPD_BLACK);
  display.drawLine(x + 14, y + 7, x + 8, y + 13, GxEPD_BLACK);
}

void drawMarketIcon() {
  const int16_t x = 273;
  const int16_t y = 2;
  display.drawRect(x + 1, y + 2, 20, 13, GxEPD_BLACK);
  display.drawLine(x + 3, y + 12, x + 8, y + 9, GxEPD_BLACK);
  display.drawLine(x + 8, y + 9, x + 12, y + 10, GxEPD_BLACK);
  display.drawLine(x + 12, y + 10, x + 17, y + 6, GxEPD_BLACK);
  display.drawLine(x + 17, y + 6, x + 19, y + 7, GxEPD_BLACK);
}

void drawWeatherIcon(const WeatherData &weather) {
  const int16_t x = 273;
  const int16_t y = 0;
  if (!weather.valid) {
    drawUnknownIcon(x, y);
    return;
  }

  switch (iconFromWeatherCode(weather.weatherCode)) {
  case WeatherIcon::Sun:
    drawSunIcon(x, y);
    break;
  case WeatherIcon::Cloud:
    drawCloudBase(x, y);
    break;
  case WeatherIcon::Rain:
    drawRainIcon(x, y);
    break;
  case WeatherIcon::Storm:
    drawStormIcon(x, y);
    break;
  case WeatherIcon::Snow:
    drawSnowIcon(x, y);
    break;
  case WeatherIcon::Fog:
    drawFogIcon(x, y);
    break;
  case WeatherIcon::Unknown:
  default:
    drawUnknownIcon(x, y);
    break;
  }
}

void drawHeaderBase(const char *title) {
  display.setFont(&FreeMono9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(4, 14);
  display.print(title);

  display.drawLine(0, 18, 295, 18, GxEPD_BLACK);
}

void drawWeatherHeader(const WeatherData &weather) {
  drawHeaderBase("Clock + Weather");
  drawWeatherIcon(weather);
}

void drawMarketHeader() {
  drawHeaderBase("Markets (USD)");
  drawMarketIcon();
}

void drawThoughtIcon() {
  const int16_t x = 273;
  const int16_t y = 2;
  display.drawRoundRect(x + 1, y + 2, 20, 13, 2, GxEPD_BLACK);
  display.fillRect(x + 5, y + 6, 2, 4, GxEPD_BLACK);
  display.fillRect(x + 9, y + 6, 2, 4, GxEPD_BLACK);
}

void drawThoughtHeader() {
  drawHeaderBase("Shower Thoughts");
  drawThoughtIcon();
}

void drawWrappedLineBlock(const char *text, int16_t x, int16_t y, int16_t maxWidth,
                          uint8_t maxLines, int16_t lineHeight) {
  String line = "";
  String word = "";
  uint8_t linesUsed = 0;

  auto lineWidth = [](const String &s) -> uint16_t {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return w;
  };

  auto flushLine = [&](const String &toPrint) -> bool {
    if (linesUsed >= maxLines) {
      return false;
    }
    display.setCursor(x, y + static_cast<int16_t>(linesUsed * lineHeight));
    display.print(toPrint);
    ++linesUsed;
    return linesUsed < maxLines;
  };

  const size_t len = strlen(text);
  for (size_t i = 0; i <= len; ++i) {
    const char c = (i < len) ? text[i] : ' ';
    const bool isBreak = (c == ' ' || c == '\n' || i == len);
    if (!isBreak) {
      word += c;
      continue;
    }

    if (word.length() > 0) {
      String candidate = line;
      if (candidate.length() > 0) {
        candidate += " ";
      }
      candidate += word;

      if (lineWidth(candidate) <= static_cast<uint16_t>(maxWidth)) {
        line = candidate;
      } else {
        if (line.length() > 0) {
          if (!flushLine(line)) {
            return;
          }
          line = word;
        } else {
          String chunk = "";
          for (size_t j = 0; j < word.length(); ++j) {
            const String test = chunk + word[j];
            if (lineWidth(test) <= static_cast<uint16_t>(maxWidth)) {
              chunk = test;
            } else {
              if (!flushLine(chunk)) {
                return;
              }
              chunk = String(word[j]);
            }
          }
          line = chunk;
        }
      }
      word = "";
    }

    if (c == '\n' && line.length() > 0) {
      if (!flushLine(line)) {
        return;
      }
      line = "";
    }
  }

  if (line.length() > 0 && linesUsed < maxLines) {
    flushLine(line);
  }
}

void drawThoughtScreen(const ThoughtData &thought, bool clockValid, bool wifiConnected) {
  logf("Render thought screen. clockValid=%d wifiConnected=%d thoughtValid=%d",
       clockValid, wifiConnected, thought.valid);
  display.setRotation(1);
  display.setFullWindow();

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawThoughtHeader();

    display.setFont(&FreeMono9pt7b);
    if (clockValid) {
      char timeBuf[12];
      time_t now;
      time(&now);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
      display.setCursor(228, 36);
      display.print(timeBuf);
    }

    if (thought.valid) {
      drawWrappedLineBlock(thought.text, 8, 44, 280, 6, 14);
      display.setCursor(8, 122);
      display.print("- ");
      if (thought.source[0] != '\0') {
        display.print(thought.source);
      } else {
        display.print("Thought");
      }
    } else {
      display.setCursor(8, 72);
      display.print("Thought unavailable");
      display.setCursor(8, 94);
      display.print(wifiConnected ? thought.error : "Wi-Fi disconnected");
    }
  } while (display.nextPage());
}

void drawClockWeatherScreen(const WeatherData &weather, bool clockValid,
                            bool wifiConnected) {
  logf("Render weather screen. clockValid=%d wifiConnected=%d weatherValid=%d",
       clockValid, wifiConnected, weather.valid);
  display.setRotation(1);
  display.setFullWindow();

  char timeBuf[12] = "--:--";
  char dateBuf[24] = "No time sync";

  if (clockValid) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
    strftime(dateBuf, sizeof(dateBuf), "%a %b %d", &timeinfo);
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawWeatherHeader(weather);

    display.setFont(&FreeMonoBold18pt7b);
    display.setCursor(8, 62);
    display.print(timeBuf);

    display.setFont(&FreeMono9pt7b);
    display.setCursor(8, 86);
    display.print(dateBuf);

    if (weather.valid) {
      display.setCursor(8, 102);
      display.printf("Temp %.1fC Feels %.1fC", weather.temperatureC,
                     weather.apparentTemperatureC);
      display.setCursor(8, 118);
      display.printf("Hum %.0f%% UV ", weather.humidityPercent);
      if (weather.uvIndex >= 0.0f) {
        display.print(weather.uvIndex, 1);
      } else {
        display.print("n/a");
      }
      display.print(" Rain ");
      if (weather.rainChancePercent >= 0) {
        display.print(weather.rainChancePercent);
        display.print("%");
      } else {
        display.print("n/a");
      }
    } else {
      display.setCursor(8, 108);
      display.print("Weather unavailable");
      display.setCursor(8, 126);
      display.print(wifiConnected ? "API error" : "Wi-Fi disconnected");
    }
  } while (display.nextPage());
}

void drawMarketScreen(const MarketData &market, bool clockValid, bool wifiConnected) {
  logf("Render market screen. clockValid=%d wifiConnected=%d marketValid=%d",
       clockValid, wifiConnected, market.valid);
  display.setRotation(1);
  display.setFullWindow();

  auto drawTrendSuffix = [](bool hasCurrent, float current, bool hasPrevious,
                            float previous, float threshold) {
    if (!hasCurrent || !hasPrevious) {
      return;
    }
    const float delta = current - previous;
    char trend = '=';
    if (delta > threshold) {
      trend = '^';
    } else if (delta < -threshold) {
      trend = 'v';
    }
    display.print(" ");
    display.print(trend);
  };

  const bool hasPreviousMarket = lastMarketData.valid;
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawMarketHeader();

    display.setFont(&FreeMono9pt7b);
    if (clockValid) {
      char timeBuf[12];
      time_t now;
      time(&now);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
      display.setCursor(228, 36);
      display.print(timeBuf);
    }

    if (market.valid) {
      display.setCursor(8, 36);
      if (market.hasBtc) {
        display.printf("BTC : $%.2f", market.btcUsd);
        drawTrendSuffix(market.hasBtc, market.btcUsd, lastMarketData.hasBtc,
                        lastMarketData.btcUsd, 0.005f);
      } else {
        display.print("BTC : n/a");
      }
      display.setCursor(8, 50);
      if (market.hasEth) {
        display.printf("ETH : $%.2f", market.ethUsd);
        drawTrendSuffix(market.hasEth, market.ethUsd, lastMarketData.hasEth,
                        lastMarketData.ethUsd, 0.005f);
      } else {
        display.print("ETH : n/a");
      }
      display.setCursor(8, 64);
      if (market.hasGold) {
        display.printf("Gold: $%.2f", market.goldUsd);
        drawTrendSuffix(market.hasGold, market.goldUsd, lastMarketData.hasGold,
                        lastMarketData.goldUsd, 0.005f);
      } else {
        display.print("Gold: n/a");
      }
      display.setCursor(8, 78);
      if (market.hasOil) {
        display.printf("Oil : $%.2f", market.oilUsd);
        drawTrendSuffix(market.hasOil, market.oilUsd, lastMarketData.hasOil,
                        lastMarketData.oilUsd, 0.005f);
      } else {
        display.print("Oil : n/a");
      }
      display.setCursor(8, 92);
      if (market.hasZarUsd) {
        display.printf("USD/ZAR: %.5f", market.zarUsd);
        drawTrendSuffix(market.hasZarUsd, market.zarUsd, lastMarketData.hasZarUsd,
                        lastMarketData.zarUsd, 0.000005f);
      } else {
        display.print("USD/ZAR: n/a");
      }
      display.setCursor(8, 106);
      if (market.hasZarMur) {
        display.printf("ZAR/MUR: %.5f", market.zarMur);
        drawTrendSuffix(market.hasZarMur, market.zarMur, lastMarketData.hasZarMur,
                        lastMarketData.zarMur, 0.000005f);
      } else {
        display.print("ZAR/MUR: n/a");
      }
    } else {
      display.setCursor(8, 72);
      display.print("Market data unavailable");
      display.setCursor(8, 94);
      if (!wifiConnected) {
        display.print("Wi-Fi disconnected");
      } else if (market.error[0] != '\0') {
        display.print(market.error);
      } else {
        display.print("API error");
      }
    }
  } while (display.nextPage());

  if (market.valid) {
    lastMarketData = market;
    lastMarketData.error[0] = '\0';
    if (!hasPreviousMarket) {
      logLine("Saved first market snapshot for trend arrows");
    } else {
      logLine("Updated market snapshot for trend arrows");
    }
  }
}

void runDashboardCycle(uint8_t cycle) {
  bool wifiConnected = false;
  bool clockValid = false;
  WeatherData weather = {false, 0.0f, 0.0f, 0.0f, -1.0f, -1, 65535};
  MarketData market = {false, false, false, false, false, false, false,
                       0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f, ""};
  ThoughtData thought = {false, "", "", ""};

  wifiConnected = connectWiFi();
  logf("Wi-Fi: %s", wifiConnected ? "connected" : "disconnected");
  if (wifiConnected) {
    syncClock();
    clockValid = hasValidTime();
    logf("Clock valid: %s", clockValid ? "yes" : "no");
    // Fetch weather and market every cycle so both API paths are logged.
    fetchWeather(weather);
    fetchMarketPrices(market);
    if (cycle == 2) {
      selectThought(thought, true, clockValid);
    }
  } else if (cycle == 2) {
    selectThought(thought, false, clockValid);
  }

  if (cycle == 0) {
    drawClockWeatherScreen(weather, clockValid, wifiConnected);
  } else if (cycle == 1) {
    drawMarketScreen(market, clockValid, wifiConnected);
  } else {
    drawThoughtScreen(thought, clockValid, wifiConnected);
  }
  logf("Display updated for cycle=%u", cycle);
}

void setupDisplay() {
  logLine("Display init start");
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  display.init(115200, true, 2, false);
  logLine("Display init done");
}

void deepSleepUntilNextUpdate() {
  logf("Deep sleep start. Interval=%u sec", DASHBOARD_UPDATE_INTERVAL_SEC);
  setRgbLed(0, 0, 0);
  display.hibernate();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(DASHBOARD_UPDATE_INTERVAL_SEC) *
                                 1000000ULL);
  Serial.flush();
  Serial0.flush();
  esp_deep_sleep_start();
}

void setup() {
  const uint8_t cycle = bootCount % 3; // 0=weather, 1=market, 2=thought
  const uint32_t bootBefore = bootCount;
  ++bootCount;
  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();

  Serial.begin(115200);
  Serial0.begin(115200);
  delay(SERIAL_WAIT_MS);
  logf("Boot start. bootCount(before)=%lu cycle=%u wakeCause=%s",
       static_cast<unsigned long>(bootBefore), cycle, wakeupCauseStr(wakeCause));
  logf("Build: %s %s", __DATE__, __TIME__);

  setupDisplay();
  initRgbLed();
  runDashboardCycle(cycle);

  if (CONTINUOUS_RGB_MODE) {
    continuousModeActive = true;
    activeCycle = static_cast<uint8_t>((cycle + 1U) % 3U);
    lastCycleRefreshMs = millis();
    logLine("Continuous RGB mode enabled; staying awake for nonstop LED animation");
    return;
  }

  logf("Display updated for cycle=%u, entering deep sleep soon...", cycle);
  runRgbSpectrum(POST_RENDER_LOG_MS);
  deepSleepUntilNextUpdate();
}

void loop() {
  if (!continuousModeActive) {
    return;
  }

  updateContinuousRgb();

  const uint32_t refreshIntervalMs = DASHBOARD_UPDATE_INTERVAL_SEC * 1000UL;
  if (millis() - lastCycleRefreshMs >= refreshIntervalMs) {
    runDashboardCycle(activeCycle);
    activeCycle = static_cast<uint8_t>((activeCycle + 1U) % 3U);
    lastCycleRefreshMs = millis();
  }
  delay(RGB_LED_STEP_MS);
}
