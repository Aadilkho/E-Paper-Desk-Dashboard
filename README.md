# ESP32-S3 E-Paper Dashboard

Three rotating screens:

1. Clock + weather (temperature, feels-like, humidity, UV, hourly rain chance)
2. Markets and FX (Bitcoin, Ethereum, Gold futures, Oil futures, ZAR/USD, ZAR/MUR)
3. Thought page (randomly chosen from Reddit Showerthoughts or classical quotes)

The dashboard refreshes one page per interval and cycles through all three pages.

## Wiring

ESP32-S3 DevKitM-1 -> Waveshare 2.9" e-paper (296x128)

- `3V3` -> `VCC`
- `GND` -> `GND`
- `GPIO12` -> `CLK`
- `GPIO11` -> `DIN` (MOSI)
- `GPIO7` -> `CS`
- `GPIO6` -> `DC`
- `GPIO5` -> `RST`
- `GPIO4` -> `BUSY`

## Configure

Edit `src/config.h`:

- `WIFI_SSID`, `WIFI_PASSWORD`:
  put them in `src/config_private.h` (kept local, git-ignored)
- `WEATHER_LATITUDE`, `WEATHER_LONGITUDE`
- `RGB_LED_DATA_PIN` (try `48`, or `38` on some S3 boards)
- `RGB_LED_POWER_PIN` (set to a GPIO only if your board needs LED power enable)
- `DASHBOARD_UPDATE_INTERVAL_SEC` (set to `30` for 30-second updates)

## Build and Upload

```bash
/home/plentify/.platformio/penv/bin/pio run
/home/plentify/.platformio/penv/bin/pio run -t upload
```

## Notes

- Weather source: Open-Meteo (no API key required)
- Market source: Yahoo Finance quote endpoint (no API key required)
- Thought source: `r/Showerthoughts` plus built-in Rumi / Imam al-Ghazali quotes
- RGB LED runs continuously through a full spectrum while the board is awake.
- If your display is a different 2.9" hardware revision, change the GxEPD2 panel class in `src/main.cpp`.
