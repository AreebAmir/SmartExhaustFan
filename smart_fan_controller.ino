#include "secrets.h"   // BLYNK_TEMPLATE_ID, BLYNK_TEMPLATE_NAME, BLYNK_AUTH_TOKEN,
                        // WIFI_SSID, WIFI_PASS, WEATHER_API_KEY, WEATHER_CITY, WEATHER_COUNTRY
                        // -> copy secrets.h.example to secrets.h and fill in your values

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BlynkSimpleEsp32.h>

// Wifi and API settings
char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = WIFI_SSID;
char pass[] = WIFI_PASS;

String weatherApiKey = WEATHER_API_KEY;
String city = WEATHER_CITY;
String countryCode = WEATHER_COUNTRY;

// Hardware pins
#define RELAY_PIN D2
#define SMOKE_PIN A2

// Default threshold values (can be changed from Blynk dashboard)
float humidityThreshold = 60.0;
float temperatureThreshold = 35.0;
int smokeThreshold = 10;

// Runtime limit
int fanRuntimeLimitMinutes = 20;
unsigned long smartFanStartedAt = 0;
bool smartRuntimeActive = false;

// This prevents humidity from immediately turning the fan back on
// after the runtime limit has already stopped it.
bool humidityRuntimeLimitReached = false;

// Trigger enable settings (allow users to select sensor conditions to activate fan)
bool humidityTriggerEnabled = true;
bool temperatureTriggerEnabled = true;
bool smokeTriggerEnabled = true;

// Smoke sensor processing settings
#define SMOKE_RESET_THRESHOLD 15
#define SENSOR_WARMUP_TIME 60000
#define BASELINE_ALPHA 0.01

// Blynk virtual pins
#define VPIN_SMOKE V0
#define VPIN_TEMP V1
#define VPIN_HUMIDITY V2
#define VPIN_FAN_CONTROL V3
#define VPIN_AUTO_MODE V4
#define VPIN_OUTDOOR_HUMIDITY V5
#define VPIN_HUMIDITY_DIFFERENCE V6
#define VPIN_FAN_RUNTIME_TODAY V7
#define VPIN_CURRENT_FAN_SESSION V8
#define VPIN_HUMIDITY_THRESHOLD V9
#define VPIN_TEMP_THRESHOLD V10
#define VPIN_SMOKE_THRESHOLD V11
#define VPIN_ENABLE_HUMIDITY V12
#define VPIN_ENABLE_TEMP V13
#define VPIN_ENABLE_SMOKE V14
#define VPIN_FAN_RUNTIME_LIMIT V15

// Blynk event names
#define EVENT_SMOKE "smoke_alert"
#define EVENT_TEMP "temperature_alert"
#define EVENT_HUMIDITY "high_humidity"

// Sensor and Blynk objects
Adafruit_BME280 bme;
BlynkTimer timer;

// Smoke sensor values
// smokeOffset = smoothedSmokeValue - baselineSmokeValue
float baselineSmokeValue = 0; // clean-air reference
float smoothedSmokeValue = 0; // filtered live reading

// Weather API values
float outdoorHumidity = 0;
bool weatherApiAvailable = false;

// Fan mode and runtime tracking
bool manualOverride = false; // true: Manual mode, false: Smart mode
int requestedFanState = 0; // stores current Blynk fan switch value

bool fanIsOn = false;
unsigned long fanTurnedOnAt = 0;
unsigned long totalFanRuntimeMs = 0;
// Minimum ON time prevents rapid ON/OFF switching around the threshold.
const unsigned long MIN_FAN_ON_TIME_MS = 60000UL; // 1 minute
unsigned long minimumFanOnUntil = 0;

// Alert flags prevent repeated notifications
bool smokeAlertSent = false;
bool tempAlertSent = false;
bool humidityAlertSent = false;

bool blynkReady = false;

// Function declarations
void setFanState(bool turnOn);
void connectToWiFiAndBlynk();
void getWeatherHumidity();
void calibrateSmokeSensor();
void sendSensorData();
void sendFanUsageData();

// Blynk: Smart / Manual mode
BLYNK_WRITE(VPIN_AUTO_MODE) {
  int autoMode = param.asInt();

  if (autoMode == 1) {
    manualOverride = false;
    Serial.println("SMART/AUTO mode enabled from Blynk");
  } else {
    manualOverride = true;
    Serial.println("MANUAL mode enabled from Blynk");

    // When switching from Smart to Manual,
    // immediately apply the current manual Fan Switch value.
    if (requestedFanState == 1) {
      setFanState(true);
    } else {
      setFanState(false);
    }
  }
}

// Blynk: Manual fan switch
BLYNK_WRITE(VPIN_FAN_CONTROL) {
  requestedFanState = param.asInt();

  if (!manualOverride) {
    Serial.println("Fan switch value saved, but not applied because SMART/AUTO mode is enabled");
    return;
  }

  if (requestedFanState == 1) {
    setFanState(true);
    Serial.println("Fan ON from Blynk manual control");
  } else {
    setFanState(false);
    Serial.println("Fan OFF from Blynk manual control");
  }
}

// Blynk: User threshold settings
BLYNK_WRITE(VPIN_HUMIDITY_THRESHOLD) {
  humidityThreshold = param.asFloat();

  Serial.print("Humidity threshold updated from Blynk: ");
  Serial.println(humidityThreshold);
}

BLYNK_WRITE(VPIN_TEMP_THRESHOLD) {
  temperatureThreshold = param.asFloat();

  Serial.print("Temperature threshold updated from Blynk: ");
  Serial.println(temperatureThreshold);
}

BLYNK_WRITE(VPIN_SMOKE_THRESHOLD) {
  smokeThreshold = param.asInt();

  Serial.print("Smoke threshold updated from Blynk: ");
  Serial.println(smokeThreshold);
}

// Blynk: Enable/disable trigger settings
BLYNK_WRITE(VPIN_ENABLE_HUMIDITY) {
  humidityTriggerEnabled = param.asInt();

  Serial.print("Humidity trigger enabled: ");
  Serial.println(humidityTriggerEnabled ? "YES" : "NO");
}

BLYNK_WRITE(VPIN_ENABLE_TEMP) {
  temperatureTriggerEnabled = param.asInt();

  Serial.print("Temperature trigger enabled: ");
  Serial.println(temperatureTriggerEnabled ? "YES" : "NO");
}

BLYNK_WRITE(VPIN_ENABLE_SMOKE) {
  smokeTriggerEnabled = param.asInt();

  Serial.print("Smoke trigger enabled: ");
  Serial.println(smokeTriggerEnabled ? "YES" : "NO");
}

// Blynk: Fan Runtime Limit
BLYNK_WRITE(VPIN_FAN_RUNTIME_LIMIT) {
  fanRuntimeLimitMinutes = param.asInt();

  Serial.print("Fan runtime limit updated from Blynk: ");
  Serial.print(fanRuntimeLimitMinutes);
  Serial.println(" minutes");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("ESP32 started");

  pinMode(RELAY_PIN, OUTPUT);
  setFanState(false);

  Serial.println("Starting BME280...");

  if (!bme.begin(0x77)) {
    Serial.println("Could not find BME280 at 0x77. Trying 0x76...");

    if (!bme.begin(0x76)) {
      Serial.println("Could not find BME280 sensor at 0x76 or 0x77!");
      Serial.println("Check wiring: SDA, SCL, VCC, GND");
      while (1);
    }
  }

  Serial.println("BME280 found!");

  Serial.println("Warming up smoke sensor...");
  delay(SENSOR_WARMUP_TIME);

  Serial.println("Calibrating smoke sensor in clean air...");
  calibrateSmokeSensor();

  connectToWiFiAndBlynk();

  getWeatherHumidity();

  timer.setInterval(2000L, sendSensorData);
  timer.setInterval(600000L, getWeatherHumidity);
  timer.setInterval(5000L, sendFanUsageData);

  Serial.println("Setup complete");
  Serial.println("V3 = Fan control, V4 = Auto mode");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!Blynk.connected()) {
      Blynk.connect(1000);
    }

    if (Blynk.connected()) {
      blynkReady = true;
      Blynk.run();
    } else {
      blynkReady = false;
    }
  } else {
    blynkReady = false;
  }

  timer.run();
}

// Connect to Wifi and Blynk
void connectToWiFiAndBlynk() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, pass);

  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    Blynk.config(auth);

    Serial.println("Connecting to Blynk...");
    if (Blynk.connect(5000)) {
      Serial.println("Blynk connected!");
      blynkReady = true;

      Blynk.syncVirtual(VPIN_FAN_CONTROL);
      Blynk.syncVirtual(VPIN_AUTO_MODE);
      Blynk.syncVirtual(VPIN_HUMIDITY_THRESHOLD);
      Blynk.syncVirtual(VPIN_TEMP_THRESHOLD);
      Blynk.syncVirtual(VPIN_SMOKE_THRESHOLD);
      Blynk.syncVirtual(VPIN_ENABLE_HUMIDITY);
      Blynk.syncVirtual(VPIN_ENABLE_TEMP);
      Blynk.syncVirtual(VPIN_ENABLE_SMOKE);
      Blynk.syncVirtual(VPIN_FAN_RUNTIME_LIMIT);
    } else {
      Serial.println("Blynk failed. Continuing without Blynk.");
      blynkReady = false;
    }
  } else {
    Serial.println("\nWiFi failed. Continuing without Blynk.");
    blynkReady = false;
  }
}

void getWeatherHumidity() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather API skipped - WiFi not connected");
    weatherApiAvailable = false;
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = "https://api.openweathermap.org/data/2.5/weather?q=" 
               + city + "," + countryCode 
               + "&appid=" + weatherApiKey 
               + "&units=metric";

  Serial.println("Requesting weather data...");
  http.begin(client, url);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      outdoorHumidity = doc["main"]["humidity"];
      weatherApiAvailable = true;

      Serial.print("Outdoor Humidity from API: ");
      Serial.print(outdoorHumidity);
      Serial.println(" %");

      if (blynkReady && Blynk.connected()) {
        Blynk.virtualWrite(VPIN_OUTDOOR_HUMIDITY, outdoorHumidity);
      }
    } else {
      weatherApiAvailable = false;
      Serial.println("Weather JSON parse failed");
    }
  } else {
    weatherApiAvailable = false;
    Serial.print("Weather API error code: ");
    Serial.println(httpCode);
  }

  http.end();
}

// Fan control helper
void setFanState(bool turnOn) {
  if (turnOn && !fanIsOn) {
    digitalWrite(RELAY_PIN, HIGH);
    fanIsOn = true;
    fanTurnedOnAt = millis();

    // Once the fan turns ON, keep it ON for at least 1 minute.
    minimumFanOnUntil = millis() + MIN_FAN_ON_TIME_MS;

    Serial.println("Fan state changed: ON");
  } 
  else if (!turnOn && fanIsOn) {
    digitalWrite(RELAY_PIN, LOW);
    fanIsOn = false;
    totalFanRuntimeMs += millis() - fanTurnedOnAt;
    Serial.println("Fan state changed: OFF");
  }
  else if (!turnOn && !fanIsOn) {
    digitalWrite(RELAY_PIN, LOW);
  }
}

// Fan usage summary
void sendFanUsageData() {
  unsigned long currentRuntimeMs = totalFanRuntimeMs;

  if (fanIsOn) {
    currentRuntimeMs += millis() - fanTurnedOnAt;
  }

  float runtimeTodayMinutes = currentRuntimeMs / 60000.0;

  float currentSessionMinutes = 0;
  if (fanIsOn) {
    currentSessionMinutes = (millis() - fanTurnedOnAt) / 60000.0;
  }

  if (blynkReady && Blynk.connected()) {
    Blynk.virtualWrite(VPIN_FAN_RUNTIME_TODAY, runtimeTodayMinutes);
    Blynk.virtualWrite(VPIN_CURRENT_FAN_SESSION, currentSessionMinutes);
  }
}

// Smoke baseline calibration
void calibrateSmokeSensor() {
  long sum = 0;

  int discardReadings = 100;
  int baselineReadings = 100;

  Serial.println("Discarding initial smoke readings before baseline...");

  for (int i = 0; i < discardReadings; i++) {
    analogRead(SMOKE_PIN);
    delay(50);

    if (i % 10 == 0) {
      Serial.print(".");
    }
  }

  Serial.println("\nCalculating smoke baseline from stable clean-air readings...");

  for (int i = 0; i < baselineReadings; i++) {
    sum += analogRead(SMOKE_PIN);
    delay(50);

    if (i % 10 == 0) {
      Serial.print(".");
    }
  }

  Serial.println(" Done!");

  baselineSmokeValue = sum / baselineReadings;
  smoothedSmokeValue = baselineSmokeValue;

  Serial.print("Smoke baseline value: ");
  Serial.println(baselineSmokeValue);
  Serial.print("Smoke alert threshold offset: ");
  Serial.println(smokeThreshold);
  Serial.print("Smoke reset threshold offset: ");
  Serial.println(SMOKE_RESET_THRESHOLD);
  Serial.println("Sensor ready!\n");
}

// Main sensor reading and smart fan logic
void sendSensorData() {
  float humidity = bme.readHumidity();
  float temperature = bme.readTemperature();
  float humidityDifference = humidity - outdoorHumidity;

  int rawSmokeValue = analogRead(SMOKE_PIN);

  smoothedSmokeValue = (0.9 * smoothedSmokeValue) + (0.1 * rawSmokeValue);

  int smokeOffset = max(0, (int)(smoothedSmokeValue - baselineSmokeValue));

  if (smokeOffset < SMOKE_RESET_THRESHOLD) {
    baselineSmokeValue =
      (1.0 - BASELINE_ALPHA) * baselineSmokeValue +
      BASELINE_ALPHA * smoothedSmokeValue;
  }

  if (blynkReady && Blynk.connected()) {
    Blynk.virtualWrite(VPIN_SMOKE, smoothedSmokeValue);
    Blynk.virtualWrite(VPIN_TEMP, temperature);
    Blynk.virtualWrite(VPIN_HUMIDITY, humidity);
    Blynk.virtualWrite(VPIN_OUTDOOR_HUMIDITY, outdoorHumidity);
    Blynk.virtualWrite(VPIN_HUMIDITY_DIFFERENCE, humidityDifference);
  }

  Serial.print("Temp: ");
  Serial.print(temperature);
  Serial.print(" C  |  Humidity: ");
  Serial.print(humidity);
  Serial.print(" %  |  Smoke raw: ");
  Serial.print(rawSmokeValue);
  Serial.print(" | smoothed: ");
  Serial.print(smoothedSmokeValue);
  Serial.print(" | baseline: ");
  Serial.print(baselineSmokeValue);
  Serial.print(" | offset: ");
  Serial.print(smokeOffset);

  if (manualOverride) {
    Serial.println(" | Mode: MANUAL");
  } else {
    Serial.println(" | Mode: AUTO");
  }

  // Humidity reset is used both inside and outside smart mode
  float humidityReset = humidityThreshold - 3.0;

  if (!manualOverride) {
    bool humidityTriggered = humidityTriggerEnabled && humidity > humidityThreshold && !humidityRuntimeLimitReached;
    bool tempTriggered = temperatureTriggerEnabled && temperature > temperatureThreshold;
    bool smokeTriggered = smokeTriggerEnabled && smokeOffset > smokeThreshold;

    bool anyTriggerActive = humidityTriggered || tempTriggered || smokeTriggered;
    bool urgentTriggerActive = tempTriggered || smokeTriggered;

    if (anyTriggerActive) {
      setFanState(true);

      if (!smartRuntimeActive) {
        smartRuntimeActive = true;
        smartFanStartedAt = millis();
      }

      if (humidityTriggered) {
        Serial.println("Fan ON - Humidity trigger enabled and above threshold");

        if (!humidityAlertSent && blynkReady && Blynk.connected()) {
          Blynk.logEvent(EVENT_HUMIDITY, "Humidity exceeded user threshold!");
          humidityAlertSent = true;
        }
      }

      if (tempTriggered) {
        Serial.println("Fan ON - Temperature trigger enabled and above threshold");

        if (!tempAlertSent && blynkReady && Blynk.connected()) {
          Blynk.logEvent(EVENT_TEMP, "Temperature exceeded user threshold!");
          tempAlertSent = true;
        }
      }

      if (smokeTriggered) {
        Serial.println("Fan ON - Smoke trigger enabled and above threshold");

        if (!smokeAlertSent && blynkReady && Blynk.connected()) {
          Blynk.logEvent(EVENT_SMOKE, "Smoke offset exceeded user threshold!");
          smokeAlertSent = true;
        }
      }
    }
    else if (
      millis() >= minimumFanOnUntil &&
      (!humidityTriggerEnabled || humidity < humidityReset || humidityRuntimeLimitReached) &&
      (!temperatureTriggerEnabled || temperature < temperatureThreshold) &&
      (!smokeTriggerEnabled || smokeOffset < SMOKE_RESET_THRESHOLD)
    ) {
      setFanState(false);
      smartRuntimeActive = false;
      Serial.println("Fan OFF - Enabled conditions returned to normal or humidity runtime limit reached");
    }

    unsigned long maxRuntimeMs = (unsigned long)fanRuntimeLimitMinutes * 60UL * 1000UL;

    if (smartRuntimeActive && fanIsOn && !urgentTriggerActive && millis() - smartFanStartedAt >= maxRuntimeMs) {
      
      setFanState(false);
      smartRuntimeActive = false;
      humidityRuntimeLimitReached = true;

      Serial.println("Fan OFF - Fan runtime limit reached");
    }
  }

  // Reset alert flags once values return to normal
  if (humidity < humidityReset) {
    humidityAlertSent = false;
    humidityRuntimeLimitReached = false;
  }

  if (smokeOffset < SMOKE_RESET_THRESHOLD) {
    smokeAlertSent = false;
  }

  if (temperature < temperatureThreshold) {
    tempAlertSent = false;
  }
}
