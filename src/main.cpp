#include "Core.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include "Webserverconfig.hpp"

#define MDNS_NAME "pump"

#define IncreaseOnUsage 3

// Forward declarations
void startAccessPoint();
void synchronizeTime();
void fetchAndSetTimezone();
void loadWiFiCredentials();
void recordUsage();
void updateUsagePattern();
int getCurrentDay();
int getCurrentTimeSlot();
bool shouldActivatePump(int day, int slot);
void activatePump();
void deactivatePump();
void loadUsagePattern();
void setupOTA();
void temperatureMeasurement();
void traceInfo();

bool isOtaUpdating = false;  // Flag to track OTA updates

bool slotRecorded = false; // Tracks if usage has been recorded for the current slot

// Wi-Fi Credentials
char ssid[32] = "";      // Mutable array to hold the WiFi SSID
char password[64] = "";  // Mutable array to hold the WiFi password

// NTP Configuration
const char* ntpServer = "pool.ntp.org";

// GPIO Pins
#define PUMP_RELAY_PIN 27
#define ONE_WIRE_BUS 32  // GPIO for DS18B20 sensor
#define CUSTOM_SDA 21
#define CUSTOM_SCL 22

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Temperature tracking variables
float sumTemp = 0;
int numReadings = 0;

unsigned long cycleStartTime = 0;
bool measuring = false;
bool sleepPhase = false;

void setup() {
  Serial.begin(115200);
  Wire.begin(CUSTOM_SDA, CUSTOM_SCL);

  delay(1000);
  Serial.println("Startup...");

  // Check if RTC is available
  Core::rtcAvailable = Core::rtc.begin();
  
  if (Core::rtcAvailable) {
      Serial.println("RTC found.");
      if (Core::rtc.lostPower()) {
          Serial.println("RTC not initialized, setting default time...");
          Core::rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // Set RTC to compile-time
      }

      // Set system time from RTC if available
      DateTime now = Core::rtc.now();
      struct timeval tv = { static_cast<time_t>(now.unixtime()), 0 };
      settimeofday(&tv, nullptr);
  } else {
      Serial.println("RTC not found. Falling back to NTP.");
  }

  // Initialize Preferences
  Core::preferences.begin("water-usage", false);

  // Load stored settings (or use defaults if not found)
  Core::threshold = Core::preferences.getFloat("threshold", TempChangeThresholdDefault);
  Core::measureDuration = Core::preferences.getUInt("measureDuration", MeasureDurationDefault);
  Core::sleepDuration = Core::preferences.getUInt("sleepDuration", SleepDurationDefault);

  // Load Wi-Fi credentials
  loadWiFiCredentials();

  // Connect to Wi-Fi
  if (strlen(ssid) > 0) {
      WiFi.begin(ssid, password);
      Serial.printf("Connecting to WiFi: %s\n", ssid);
      for (int i = 0; i < 120; i++) {
          if (WiFi.status() == WL_CONNECTED) {
              break;
          }
          delay(500);
          Serial.print(".");
      }
      Serial.println("\nWiFi connected!");
      Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  } else {
      startAccessPoint();
  }

  synchronizeTime();
  
  // Set up OTA updates
  setupOTA();

  // Set up pump relay
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);

  // Load usage patterns
  loadUsagePattern();

  sensors.begin();
  sensors.setResolution(11);
  sensors.setWaitForConversion(false);

  // Start first measurement
  sensors.requestTemperatures();
  measuring = true;
  cycleStartTime = millis();

  // Set up web server endpoints
  webserversetup();
  server.begin();
}

void loop() {
  static unsigned long lastTimeSync = 0;
  static bool wifiConnecting = false;
  static unsigned long lastReconnectAttempt = 0;  // Tracks last reconnect attempt time

  traceInfo();

  ArduinoOTA.handle();

  if (isOtaUpdating) {
      server.stop();
      return;  // Skip the rest of the loop
  }

  if (strlen(ssid) > 0 && WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastReconnectAttempt >= 20000) {
        Serial.println("Wi-Fi lost. Reconnecting...");
        lastReconnectAttempt = currentMillis;  // Update last attempt time

        if (WiFi.reconnect()) {
            wifiConnecting = true;
        }
    }
  }

  if (wifiConnecting && WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      Serial.println("Wi-Fi reconnected successfully.");
      server.begin();
      setupOTA();
  }
    
  if(WiFi.status() == WL_CONNECTED)
  {
    // Periodically update the timezone and resync time
    if (millis() - lastTimeSync > 86400000) { 
      synchronizeTime();
      lastTimeSync = millis();
    }

    server.handleClient();
  }
  
  temperatureMeasurement();

  updateUsagePattern();

  // Activate or deactivate pump based on the usage pattern
  int currentDay = getCurrentDay();
  int currentSlot = getCurrentTimeSlot();
  if (shouldActivatePump(currentDay, currentSlot)) {
    activatePump();
  } else {
    deactivatePump();
  }
}

void temperatureMeasurement() {
  unsigned long currentMillis = millis();

  if (!sleepPhase) {
      // Measuring phase: collect as many readings as possible in the given time
      if (measuring && sensors.isConversionComplete()) {
          measuring = false;
          float currentTemp = sensors.getTempCByIndex(0);

          sumTemp += currentTemp;
          numReadings++;

          //Serial.print("Measured Temperature: ");
          //Serial.print(currentTemp);
          //Serial.println(" °C");

          // Start the next async measurement immediately
          sensors.requestTemperatures();
          measuring = true;
      }

      // End of measurement phase after `measureDuration` seconds
      if (currentMillis - cycleStartTime >= Core::measureDuration) {
          if (numReadings > 0) {
            Core::currentAvgTemp = sumTemp / numReadings;
            Core::deltaT = Core::currentAvgTemp - Core::initialAvgTemp;

              Serial.print("Average Temp: ");
              Serial.print(Core::currentAvgTemp);
              Serial.println(" °C");

              Serial.print("ΔT: ");
              Serial.print(Core::deltaT);
              Serial.println(" °C");

              if (Core::deltaT <= -Core::threshold) {
                  Serial.println(">>> Hot water usage detected! <<<");
                  recordUsage();  // Trigger logging & pump activation
              }
          }

          // Switch to sleep phase
          sleepPhase = true;
          cycleStartTime = millis();
          Serial.println(">>> Entering sleep phase...");
      }
  } 
  else { // Sleep phase
      if (currentMillis - cycleStartTime >= Core::sleepDuration) {
          sleepPhase = false;
          cycleStartTime = millis();

          // Update reference temperature
          Core::initialAvgTemp = (Core::currentAvgTemp > 0) ? Core::currentAvgTemp : Core::initialAvgTemp;

          // Reset measurement variables
          sumTemp = 0;
          numReadings = 0;

          // Start a new measurement cycle
          sensors.requestTemperatures();
          measuring = true;

          Serial.println(">>> Starting new measurement cycle...");
      }
  }
}

void startAccessPoint() {
  WiFi.softAP("CirculationPumpSmartControl", "12345678");
  Serial.printf("Access Point IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void logCurrentTime() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  Serial.printf("Local Time: %s\n", timeString);
}


void synchronizeTime() {
  Serial.println("Synchronizing time with NTP...");

  configTime(0, 0, "pool.ntp.org");

  time_t now = time(nullptr);
  for (int i = 0; i < 20; i++) {
      if (now > 100000) break;
      delay(500);
      now = time(nullptr);
  }

  if (now > 100000) {
      Serial.println("Time synchronized with NTP.");
      
      fetchAndSetTimezone();

      if (Core::rtcAvailable) {
          Core::rtc.adjust(DateTime(now));  // Store in RTC if available
          Serial.println("RTC updated with NTP time.");
      }
  } else {
      Serial.println("Failed to sync with NTP.");
  }
}

// Function to map timezone_name to POSIX TZ format
String getPosixTZ(String timezoneName) {
  if (timezoneName == "UTC") return "UTC0";
  if (timezoneName == "GMT") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (timezoneName == "CET") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (timezoneName == "EET") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (timezoneName == "MSK") return "MSK-3";
  if (timezoneName == "IST") return "IST-5:30";
  if (timezoneName == "WIB") return "WIB-7";
  if (timezoneName == "WITA") return "WITA-8";
  if (timezoneName == "WIT") return "WIT-9";
  if (timezoneName == "JST") return "JST-9";
  if (timezoneName == "KST") return "KST-9";
  if (timezoneName == "CST") return "CST6CDT,M3.2.0,M11.1.0";
  if (timezoneName == "EST") return "EST5EDT,M3.2.0,M11.1.0";
  if (timezoneName == "PST") return "PST8PDT,M3.2.0,M11.1.0";
  if (timezoneName == "MST") return "MST7MDT,M3.2.0,M11.1.0";
  if (timezoneName == "HST") return "HST10";
  if (timezoneName == "AKST") return "AKST9AKDT,M3.2.0,M11.1.0";
  if (timezoneName == "AST") return "AST4ADT,M3.2.0,M11.1.0";
  if (timezoneName == "NZST") return "NZST-12NZDT,M9.5.0,M4.1.0/3";
  if (timezoneName == "AEST") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  if (timezoneName == "ACST") return "ACST-9:30ACDT,M10.1.0,M4.1.0/3";
  if (timezoneName == "AWST") return "AWST-8";
  if (timezoneName == "BST") return "BST-6";
  if (timezoneName == "PKT") return "PKT-5";
  if (timezoneName == "HKT") return "HKT-8";
  if (timezoneName == "SGT") return "SGT-8";
  if (timezoneName == "IRKT") return "IRKT-8";
  if (timezoneName == "YAKT") return "YAKT-9";
  if (timezoneName == "VLAT") return "VLAT-10";

  return "UTC0";  // Default fallback
}

void fetchAndSetTimezone() {
  HTTPClient http;
  http.begin("http://ipwhois.app/json/");
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      
      String timezoneName = doc["timezone_name"].as<String>();
      Serial.print("Timezone Name: ");
      Serial.println(timezoneName);

      String posixTZ = getPosixTZ(timezoneName);
      Serial.print("POSIX Timezone: ");
      Serial.println(posixTZ);

      setenv("TZ", posixTZ.c_str(), 1);
      tzset();
  } else {
      Serial.println("Failed to fetch timezone!");
  }
  http.end();
}


void saveWiFiCredentials(const String& newSSID, const String& newPassword) {
  Core::preferences.putString("ssid", newSSID);
  Core::preferences.putString("password", newPassword);
  Serial.println("WiFi credentials saved to flash.");
}

void loadWiFiCredentials() {
  String storedSSID = Core::preferences.getString("ssid", "");
  String storedPassword = Core::preferences.getString("password", "");

  if (storedSSID.length() > 0) {
    storedSSID.toCharArray(ssid, sizeof(ssid));         // Copy SSID into the mutable array
    storedPassword.toCharArray(password, sizeof(password)); // Copy Password into the mutable array
    Serial.printf("Loaded WiFi credentials: SSID=%s\n", ssid);
  } else {
    Serial.println("No WiFi credentials found in flash.");
  }
}

void recordUsage() {
  if (!time(nullptr) || time(nullptr) < 100000) {
      Serial.println("Time not synchronized, skipping usage record.");
      return;
  }

  if (slotRecorded) {
      return;
  }

  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int currentDay = timeinfo.tm_wday;
  int currentSlot = (timeinfo.tm_hour * 4) + (timeinfo.tm_min / 15);

  Core::usagePattern[currentDay][currentSlot] = min(255, Core::usagePattern[currentDay][currentSlot] + IncreaseOnUsage);

  // Check hit/miss conditions
  if (Core::pumpActivated) {
      Core::hotWaterUsedWithPump++;
  } else {
      Core::hotWaterUsedWithoutPump++;
  }

  slotRecorded = true;
  saveUsagePattern();
}

void updateUsagePattern() {
  static int lastSlot = -1;
  static int lastDay = -1;

  int currentSlot = getCurrentTimeSlot();
  int currentDay = getCurrentDay();

  if (currentSlot != lastSlot) {  
      // If last slot exists and no usage was recorded, decrement it
      if (lastSlot != -1 && !slotRecorded) {  
          if (Core::usagePattern[lastDay][lastSlot] > 0) {
              Core::usagePattern[lastDay][lastSlot]--;
              saveUsagePattern();
              Serial.printf("No usage detected in Slot %d of Day %d → Decrementing usage count to %d.\n", 
                             lastSlot, lastDay, Core::usagePattern[lastDay][lastSlot]);
          }

          // Track "pump running without usage" statistics
          Core::pumpRunWithoutUsage++;  
          Serial.printf("Pump was running without usage in Slot %d on Day %d → Count: %d\n", 
                         lastSlot, lastDay, Core::pumpRunWithoutUsage);
      }

      // Reset for the new timeslot
      slotRecorded = false;
      lastSlot = currentSlot;
      lastDay = currentDay;
      Serial.printf("New timeslot detected: Slot %d on Day %d\n", currentSlot, currentDay);
  }
}



int getCurrentDay() {
  // Get current local time
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  return timeinfo.tm_wday; // Local day (0 = Sunday, ..., 6 = Saturday)
  }

int getCurrentTimeSlot() {
  // Get current local time
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  
  return (timeinfo.tm_hour * 4) + (timeinfo.tm_min / 15); // Local 15-min slot index
  }

bool shouldActivatePump(int day, int slot) {
  return (Core::usagePattern[day][slot] > 0);
}


void activatePump() {
  if (!Core::pumpActivated) {
      digitalWrite(PUMP_RELAY_PIN, HIGH);
      Core::pumpActivated = true;
      Core::lastPumpStart = millis(); // Record pump start time
      Serial.println("Pump activated");
  }
}

void deactivatePump() {
  if (Core::pumpActivated) {
      digitalWrite(PUMP_RELAY_PIN, LOW);
      Core::pumpActivated = false;

      // Calculate running time
      unsigned long elapsed = (millis() - Core::lastPumpStart) / 1000; // Convert to seconds
      Core::pumpRunTime += elapsed;
      Serial.printf("Pump deactivated. Total runtime: %lu seconds\n", Core::pumpRunTime);
  }
}


void saveUsagePattern() {
  Core::preferences.putBytes("usagePattern", Core::usagePattern, sizeof(Core::usagePattern));
}

void loadUsagePattern() {
  if (Core::preferences.getBytes("usagePattern", Core::usagePattern, sizeof(Core::usagePattern)) == 0) {
    memset(Core::usagePattern, 0, sizeof(Core::usagePattern));
  }
}

void traceInfo(){

  static unsigned long previousMillis = 0;

  if (millis() - previousMillis >= 10000) {
    previousMillis = millis();
    
    Serial.print("Free RAM: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
  }
}


void setupOTA() {
  ArduinoOTA.setHostname(MDNS_NAME);
  
  ArduinoOTA.onStart([]() {
      Serial.println("OTA update started...");
  });

  ArduinoOTA.onEnd([]() {
      Serial.println("\nOTA update complete!");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\n", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
      server.begin();
      Serial.printf("OTA Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.printf("OTA Ready! Accessible via %s.local\n", MDNS_NAME);
}
