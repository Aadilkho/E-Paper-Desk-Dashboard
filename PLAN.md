# Implementation Plan

## Summary of Changes

All changes are in two files: `src/config.h` (user-facing settings) and `src/main.cpp` (logic + rendering).

---

## Phase 1 — High Priority Fixes

### 1a. Fix magic number time validation
- Replace all 3 occurrences of `1700000000` with a named constant `MIN_VALID_EPOCH`
- Define `MIN_VALID_EPOCH` in `main.cpp` based on build date macros `__DATE__` / `__TIME__` so it automatically advances with each firmware build
- Affects: `syncClock()`, `hasValidTime()`

### 1b. Selective data fetching per cycle
- Add `RTC_DATA_ATTR WeatherData cachedWeather` to store weather across deep sleep cycles (reused by the corner widget in Tier 1)
- In `runDashboardCycle()`: only call `fetchWeather()` when cycle is weather OR when cached data is stale (>30 min); only call `fetchMarketPrices()` when cycle is market; only call news/prayer fetches on their respective cycles
- Saves ~2 unnecessary HTTP calls per non-weather/non-market cycle

### 1c. HTTP retry logic
- Extract a `httpGetRetry()` helper that wraps any HTTP GET with up to 3 attempts, with 1s delay between attempts
- Replace raw `http.GET()` calls in all fetch functions with this helper
- Applies to: `fetchWeatherOpenMeteo`, `fetchShowerThought`, `httpGetWithLogs` lambda inside `fetchMarketPrices`

---

## Phase 2 — Tier 1: Per-screen enable/disable + configurable intervals

### 2a. Screen enable/disable flags in config.h
```cpp
#define ENABLE_WEATHER_SCREEN   true
#define ENABLE_MARKET_SCREEN    true
#define ENABLE_THOUGHTS_SCREEN  true
#define ENABLE_FOCUS_SCREEN     true
#define ENABLE_PRAYER_SCREEN    true
#define ENABLE_NEWS_SCREEN      true
```

### 2b. Dynamic screen rotation
- Replace `bootCount % 3` hardcode with a dynamic `screenList[]` array built at first boot
- Store `RTC_DATA_ATTR uint8_t screenList[8]` and `RTC_DATA_ATTR uint8_t screenCount` in RTC memory
- On `bootCount == 0` (or `screenCount == 0`), populate `screenList` from the enabled flags in config
- Screen IDs: 0=Weather, 1=Market, 2=Thought, 3=Focus, 4=Prayer, 5=News
- `cycle = screenList[bootCount % screenCount]`

### 2c. Per-screen wake intervals in config.h
```cpp
#define WEATHER_SCREEN_INTERVAL_SEC   60
#define MARKET_SCREEN_INTERVAL_SEC   120
#define THOUGHTS_SCREEN_INTERVAL_SEC 300
#define FOCUS_SCREEN_INTERVAL_SEC     60
#define PRAYER_SCREEN_INTERVAL_SEC   300
#define NEWS_SCREEN_INTERVAL_SEC     180
```
- Add `getIntervalForScreen(uint8_t screenId)` helper that returns the right interval
- Pass to `deepSleepUntilNextUpdate(uint32_t seconds)`

---

## Phase 3 — Tier 1: Moon phase on weather screen

### 3a. Moon phase calculation
- Add `uint8_t moonPhaseIndex(time_t t)` function using a standard Julian Day Number algorithm (no API needed, pure math)
- Returns 0–7: New, Waxing Crescent, First Quarter, Waxing Gibbous, Full, Waning Gibbous, Last Quarter, Waning Crescent

### 3b. Moon phase drawing
- Add `drawMoonIcon(int16_t x, int16_t y, uint8_t phase)` using existing `display.draw*` primitives
- Full moon = filled circle; new moon = outline; quarters = half-filled; crescents = arc approximation with overlapping circles
- Place in bottom-right of weather screen (around x=260, y=90), alongside a 1-char phase label

---

## Phase 4 — Tier 1: Tiny weather corner on all screens

### 4a. Cache weather in RTC memory
- `RTC_DATA_ATTR WeatherData cachedWeather` (already added in Phase 1b)
- `RTC_DATA_ATTR time_t weatherFetchedAt = 0`
- Weather is considered stale after 30 minutes (`WEATHER_CACHE_TTL_SEC 1800` in config)

### 4b. Corner widget function
- Add `drawWeatherCorner(int16_t x, int16_t y, const WeatherData &w)`
- Draws a scaled-down weather icon (12x12 px version of existing icons) + temperature string
- Called from `drawMarketScreen()`, `drawThoughtScreen()`, `drawFocusScreen()`, `drawPrayerScreen()`, `drawNewsScreen()`
- Positioned at top-right, left of the clock time (around x=200, y=36)

---

## Phase 5 — Tier 1: Productivity/Focus Screen

### 5a. Config additions
```cpp
#define FOCUS_WORD          "Build."          // Shown large in center
#define FOCUS_EVENT_NAME    "Next holiday"    // Label for countdown
#define FOCUS_EVENT_YEAR    2026
#define FOCUS_EVENT_MONTH   4                 // April
#define FOCUS_EVENT_DAY     27
```

### 5b. `drawFocusScreen(bool clockValid)` function
Layout (296x128, rotated):
```
+---------------------------+
| Focus            [WX] HH:MM |
|________________________________|
|                               |
|         << FOCUS_WORD >>      |
|                               |
| Week 12 · Mon Mar 17          |
| Next holiday: 41 days         |
+---------------------------+
```
- `FOCUS_WORD` shown in `FreeMonoBold18pt7b` centered
- Week number computed via `tm.tm_yday / 7 + 1` approximation (or `strftime %V`)
- Countdown: `difftime(event_epoch, now) / 86400` days
- No network required — pure RTC time

---

## Phase 6 — Tier 2 #6: Prayer Times Screen

### 6a. Config additions
```cpp
#define PRAYER_CALC_METHOD  2   // ISNA=2, MWL=3, Egyptian=5, Makkah=4, Karachi=1
// Uses WEATHER_LATITUDE / WEATHER_LONGITUDE already defined
```

### 6b. `PrayerData` struct
```cpp
struct PrayerData {
  bool valid;
  char fajr[8];
  char dhuhr[8];
  char asr[8];
  char maghrib[8];
  char isha[8];
  char date[12];   // "17 Mar 2026"
  char error[64];
};
```

### 6c. `fetchPrayerTimes(PrayerData &prayer)` function
- API: `https://api.aladhan.com/v1/timings/{unix_timestamp}?latitude=LAT&longitude=LON&method=METHOD`
- Free, no API key, returns JSON with `data.timings` object
- Parse: Fajr, Dhuhr, Asr, Maghrib, Isha (already in local time from API)
- Store `RTC_DATA_ATTR PrayerData cachedPrayer` — re-fetch only once per day

### 6d. `drawPrayerScreen(const PrayerData &prayer, bool clockValid)` function
Layout:
```
+-----------------------------+
| Prayer Times      [WX] HH:MM |
|______________________________|
| Fajr    05:12                |
| Dhuhr   12:48   * current *  |
| Asr     16:23                |
| Maghrib 18:51                |
| Isha    20:15                |
+-----------------------------+
```
- Highlight current/next prayer by comparing current time to prayer times
- Bold indicator `*` next to the upcoming prayer
- Date shown in header area

---

## Phase 7 — Tier 2 #9: News/RSS Headline Screen

### 7a. Config additions
```cpp
// Source options: "hackernews" or "reddit:<subreddit>"
#define NEWS_SOURCE         "hackernews"
#define NEWS_SUBREDDIT      "worldnews"    // used if NEWS_SOURCE = "reddit:..."
```

### 7b. `NewsData` struct
```cpp
struct NewsData {
  bool valid;
  char headline[200];
  char source[32];
  char score[16];    // e.g. "1.2k pts"
  char error[64];
};
```

### 7c. `fetchNewsHeadline(NewsData &news)` function
- **HackerNews path**: `https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=5` — free, no key, JSON
  - Pick top story: `hits[0].title`, `hits[0].points`, `hits[0].url` (domain only for attribution)
- **Reddit path**: reuse existing `fetchShowerThought()` pattern against configured subreddit
- Fallback: if fetch fails, show last cached headline from RTC memory

### 7d. `drawNewsScreen(const NewsData &news, bool clockValid)` function
Layout:
```
+-----------------------------+
| Top News          [WX] HH:MM |
|______________________________|
|                              |
|  Word-wrapped headline text  |
|  spanning up to 5 lines      |
|                              |
|  - Hacker News · 847 pts     |
+-----------------------------+
```
- Reuse existing `drawWrappedLineBlock()` — no new word-wrap code needed
- Source + score shown in bottom-left (same pattern as thought attribution)

---

## File Change Summary

### `src/config.h` additions
- `MIN_VALID_EPOCH` (or computed from build date in main.cpp)
- `WEATHER_CACHE_TTL_SEC`
- `ENABLE_*_SCREEN` flags (6 screens)
- Per-screen `*_SCREEN_INTERVAL_SEC` values
- `FOCUS_WORD`, `FOCUS_EVENT_NAME`, `FOCUS_EVENT_YEAR/MONTH/DAY`
- `PRAYER_CALC_METHOD`
- `NEWS_SOURCE`, `NEWS_SUBREDDIT`

### `src/main.cpp` changes
- ~+800 lines (new structs, fetch functions, draw functions, helpers)
- Modified: `syncClock()`, `hasValidTime()`, `runDashboardCycle()`, `setup()`, `loop()`
- New RTC vars: `cachedWeather`, `weatherFetchedAt`, `cachedPrayer`, `cachedNews`, `screenList[]`, `screenCount`
- New functions: `httpGetRetry()`, `moonPhaseIndex()`, `drawMoonIcon()`, `drawWeatherCorner()`, `drawFocusScreen()`, `fetchPrayerTimes()`, `drawPrayerScreen()`, `fetchNewsHeadline()`, `drawNewsScreen()`, `buildScreenList()`, `getIntervalForScreen()`

---

## Execution Order
1. Phase 1 (fixes) — low risk, touch existing code minimally
2. Phase 2 (screen rotation) — architectural change, required for all new screens
3. Phase 3 (moon phase) — standalone addition to weather screen
4. Phase 4 (weather corner) — requires Phase 1b RTC cache
5. Phase 5 (focus screen) — requires Phase 2
6. Phase 6 (prayer screen) — requires Phase 2
7. Phase 7 (news screen) — requires Phase 2
