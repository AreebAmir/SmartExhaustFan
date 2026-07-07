# ESP32 Smart Exhaust Fan Controller

An ESP32-based smart fan controller that automatically manages an exhaust fan (or ventilation relay) based on **humidity**, **temperature**, and **smoke** readings, with live monitoring and remote control through the **Blynk IoT platform**. Outdoor humidity is pulled from the OpenWeatherMap API so the system can compare indoor vs. outdoor humidity.

## Features

- **Smart / Manual modes** — run fully automatic on sensor thresholds, or take manual control of the fan from the Blynk app.
- **Multi-trigger logic** — fan turns on when humidity, temperature, and/or smoke exceed user-defined thresholds (each trigger can be individually enabled/disabled).
- **Adaptive smoke detection** — a self-calibrating baseline continuously adjusts to ambient air, so the sensor doesn't drift or false-trigger over time.
- **Runtime limiting** — caps how long the fan can run continuously on non-urgent (humidity-only) triggers, preventing it from running indefinitely.
- **Minimum ON time** — once triggered, the fan stays on for at least 1 minute to avoid rapid relay cycling.
- **Outdoor humidity comparison** — fetches local outdoor humidity via the OpenWeatherMap API and shows the indoor/outdoor humidity difference.
- **Blynk dashboard integration** — live readings, remote fan control, adjustable thresholds, and push/event alerts (humidity, temperature, smoke).
- **Fan usage tracking** — reports total runtime for the day and the current running session.

## Hardware Requirements

| Component | Notes |
|---|---|
| ESP32 dev board | Any standard ESP32 module |
| BME280 sensor | Temperature/humidity (I2C, address `0x76` or `0x77`) |
| MQ-series smoke/gas sensor (analog) | Connected to an analog pin |
| Relay module | Drives the exhaust fan |
| Wi-Fi network | 2.4 GHz, required for Blynk + weather API |

### Pin Configuration

| Function | Pin |
|---|---|
| Relay control | `D2` (`RELAY_PIN`) |
| Smoke sensor (analog input) | `A2` (`SMOKE_PIN`) |
| BME280 | I2C (SDA/SCL) |

> Adjust `RELAY_PIN` and `SMOKE_PIN` in the code to match your board's actual pin labeling, since `D2`/`A2` naming varies between ESP32 board variants.

## Required Libraries

Install these via the Arduino IDE Library Manager (or PlatformIO):

- `WiFi.h` (bundled with ESP32 core)
- `WiFiClientSecure.h` (bundled with ESP32 core)
- `HTTPClient.h` (bundled with ESP32 core)
- `ArduinoJson` (v6.x)
- `Wire.h` (bundled)
- `Adafruit Sensor`
- `Adafruit BME280 Library`
- `Blynk` (BlynkSimpleEsp32 — Blynk Legacy library)

## Configuration

Credentials are kept out of the main sketch in a separate `secrets.h` file so they never end up in version control by accident.

1. Copy the template and rename it:
   ```bash
   cp secrets.h.example secrets.h
   ```
2. Fill in your real values inside `secrets.h`:
   ```cpp
   #define BLYNK_TEMPLATE_ID   "XXXX"
   #define BLYNK_TEMPLATE_NAME "XXXX"
   #define BLYNK_AUTH_TOKEN    "XXXX"

   #define WIFI_SSID "XXXX"
   #define WIFI_PASS "XXXX"

   #define WEATHER_API_KEY "XXXX"
   #define WEATHER_CITY    "XXXX"
   #define WEATHER_COUNTRY "XXX"   // e.g. "US", "GB"
   ```
3. Place `secrets.h` in the same folder as the `.ino` file. It's already listed in `.gitignore`, so it won't be committed.

- Get a Blynk template ID/name and auth token from your [Blynk Console](https://blynk.cloud) device.
- Get a free API key from [OpenWeatherMap](https://openweathermap.org/api).

### Default Thresholds (adjustable live from Blynk)

| Setting | Default | Virtual Pin |
|---|---|---|
| Humidity threshold | 60.0 % | V9 |
| Temperature threshold | 35.0 °C | V10 |
| Smoke threshold (offset) | 10 | V11 |
| Fan runtime limit | 20 minutes | V15 |

## Blynk Virtual Pin Map

| Pin | Purpose | Direction |
|---|---|---|
| V0 | Smoke (smoothed) reading | Device → App |
| V1 | Temperature | Device → App |
| V2 | Indoor humidity | Device → App |
| V3 | Fan control switch | App ↔ Device |
| V4 | Auto/Manual mode switch | App ↔ Device |
| V5 | Outdoor humidity (from weather API) | Device → App |
| V6 | Indoor − outdoor humidity difference | Device → App |
| V7 | Fan runtime today (minutes) | Device → App |
| V8 | Current fan session length (minutes) | Device → App |
| V9 | Humidity threshold | App ↔ Device |
| V10 | Temperature threshold | App ↔ Device |
| V11 | Smoke threshold | App ↔ Device |
| V12 | Enable humidity trigger | App ↔ Device |
| V13 | Enable temperature trigger | App ↔ Device |
| V14 | Enable smoke trigger | App ↔ Device |
| V15 | Fan runtime limit (minutes) | App ↔ Device |

Set up matching widgets (gauges, switches, sliders) on these pins in your Blynk template/dashboard.

## How It Works

### Startup Sequence
1. Initializes the relay (fan OFF) and BME280 sensor.
2. Warms up the smoke sensor for 60 seconds, then samples clean air to calculate a smoke baseline.
3. Connects to Wi-Fi and Blynk, syncing all threshold/mode settings from the cloud.
4. Fetches the initial outdoor humidity reading.
5. Starts three periodic timers:
   - Every 2s — read sensors, push data to Blynk, run smart fan logic.
   - Every 10 min — refresh outdoor humidity from the weather API.
   - Every 5s — publish fan runtime/session stats.

### Smart Mode Logic
When **Auto/Smart mode** is active, the fan turns ON if any *enabled* trigger condition is met:

- **Humidity** > humidity threshold
- **Temperature** > temperature threshold
- **Smoke offset** (smoothed reading − adaptive baseline) > smoke threshold

The fan turns OFF once all enabled conditions fall back to their "reset" values:
- Humidity drops below `threshold − 3.0`
- Temperature drops below the threshold
- Smoke offset drops below the fixed reset threshold (15)

A **1-minute minimum ON time** prevents rapid switching, and a **runtime limit** (default 20 min) force-stops the fan on humidity-only triggers to avoid it running forever — temperature and smoke triggers are treated as "urgent" and bypass this cap.

### Manual Mode
Switching to Manual mode immediately applies whatever value is currently set on the Fan Control switch (V3), and subsequent switch changes directly turn the fan on/off, bypassing all sensor logic.

### Adaptive Smoke Baseline
The smoke sensor's baseline slowly self-adjusts toward the current smoothed reading whenever the smoke offset stays below the reset threshold, compensating for slow sensor drift without needing periodic recalibration.

### Alerts
Blynk push events fire once per trigger episode (not repeatedly) for:
- `smoke_alert`
- `temperature_alert`
- `high_humidity`

Each flag resets automatically once its corresponding reading returns to normal.

## Uploading

1. Install the ESP32 board package in Arduino IDE (or use PlatformIO).
2. Install all libraries listed above.
3. Create `secrets.h` from `secrets.h.example` and fill in your credentials, as described in **Configuration**.
4. Select your ESP32 board and correct COM port.
5. Upload, then open the Serial Monitor at **115200 baud** to watch startup and runtime logs.

## Troubleshooting

| Issue | Likely Cause |
|---|---|
| `Could not find BME280 sensor` | Check wiring (SDA/SCL/VCC/GND) or try the other I2C address (0x76/0x77) |
| Fan never turns off | Check `humidityReset`/threshold values, or that a trigger isn't stuck enabled with a very low threshold |
| No outdoor humidity data | Verify `weatherApiKey`, `city`, and `countryCode`, and that the device has internet access |
| Blynk not connecting | Verify `BLYNK_AUTH_TOKEN`, Wi-Fi credentials, and that the Blynk template matches your device |
| Smoke sensor triggers immediately | Increase sensor warm-up time, or re-run calibration in genuinely clean air |

## Project Files

| File | Purpose |
|---|---|
| `smart_fan_controller.ino` | Main sketch |
| `secrets.h.example` | Template for credentials — copy to `secrets.h` |
| `secrets.h` | Your real credentials (git-ignored, create this yourself) |
| `.gitignore` | Excludes `secrets.h` and build artifacts from version control |

## Notes

- Never commit `secrets.h` — it's excluded via `.gitignore` by default. Only `secrets.h.example` (with placeholder values) should be tracked in version control.
- `WiFiClientSecure::setInsecure()` is used for the weather API call, which skips TLS certificate validation. This is convenient for prototyping but not recommended for production use.
