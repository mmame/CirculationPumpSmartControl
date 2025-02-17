#include "Arduino.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <RTClib.h>
#include <ESPmDNS.h>

#define MDNS_NAME "pump"

#define IncreaseOnUsage 6
#define DecreaseWhenNoUsage 1

// Forward declarations
void startAccessPoint();
void synchronizeTime();
void fetchTimezoneOffset();
void saveWiFiCredentials(const String& newSSID, const String& newPassword);
void loadWiFiCredentials();
void recordUsage();
void updateUsagePattern();
int getCurrentDay();
int getCurrentTimeSlot();
bool shouldActivatePump(int day, int slot);
void activatePump();
void deactivatePump();
void saveUsagePattern();
void loadUsagePattern();
void generateVisualizationPage();
void setupOTA();
void temperatureMeasurement();
String getUptime();
String getCurrentDateTime();

bool isOtaUpdating = false;  // Flag to track OTA updates

RTC_PCF8563 rtc;
bool rtcAvailable = false;

int utcOffsetSeconds = 0; // Global variable to store the UTC offset
bool pumpActivated = false;
bool slotRecorded = false; // Tracks if usage has been recorded for the current slot

// Webserver
WebServer server(80);

// Preferences
Preferences preferences;

// Wi-Fi Credentials
char ssid[32] = "";      // Mutable array to hold the WiFi SSID
char password[64] = "";  // Mutable array to hold the WiFi password

// NTP Configuration
const char* ntpServer = "pool.ntp.org";

// GPIO Pins
#define PUMP_RELAY_PIN 27
#define ONE_WIRE_BUS 32  // GPIO for DS18B20 sensor

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

#define MeasureDurationDefault 5000
#define SleepDurationDefault 25000
#define TempChangeThresholdDefault 2.0

// Temperature tracking variables
float sumTemp = 0;
int numReadings = 0;
float threshold = TempChangeThresholdDefault; // Temp drop threshold for detection
float initialAvgTemp = 0; // Baseline temp for comparison
float currentAvgTemp = 0;
float deltaT = 0;

// Timing
unsigned long measureDuration = MeasureDurationDefault;  // Measurement duration
unsigned long sleepDuration = SleepDurationDefault;    // Pause between measurements
unsigned long cycleStartTime = 0;
bool measuring = false;
bool sleepPhase = false;

// Usage pattern (7 days x 96 slots for 15-minute intervals)
int usagePattern[7][96] = {0};

// Timer
unsigned long lastSampleTime = 0;
const unsigned long sampleInterval = 900000; // 15 minutes

// Offsets for pump activation
const int startOffset = 10; // Start 10 minutes before the recorded time
const int endOffset = 5;    // Stop 5 minutes after the recorded time

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Check if RTC is available
  rtcAvailable = rtc.begin();
  
  if (rtcAvailable) {
      Serial.println("RTC found.");
      if (rtc.lostPower()) {
          Serial.println("RTC not initialized, setting default time...");
          rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // Set RTC to compile-time
      }

      // Set system time from RTC if available
      DateTime now = rtc.now();
      struct timeval tv = { now.unixtime(), 0 };
      settimeofday(&tv, nullptr);
  } else {
      Serial.println("RTC not found. Falling back to NTP.");
  }

  // Initialize Preferences
  preferences.begin("water-usage", false);

  // Load stored settings (or use defaults if not found)
  threshold = preferences.getFloat("threshold", TempChangeThresholdDefault);
  measureDuration = preferences.getUInt("measureDuration", MeasureDurationDefault);
  sleepDuration = preferences.getUInt("sleepDuration", SleepDurationDefault);

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

  // Fetch timezone offset
  fetchTimezoneOffset();

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
server.on("/", HTTP_GET, []() {
  server.send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>ESP32 Circulation Pump Smart Control</title>
    </head>
    <body>
      <h1>Home</h1>
      <ul>
        <li><a href="/status">Status</a></li> 
        <li><a href="/visualize">View Usage Data</a></li>
        <li><a href="/grid">Edit Usage Grid</a></li>
        <li><a href="/config">Configuration</a></li>
        <li><a href="/wificonfig">WiFi Configuration</a></li>
        <li><a href="/timeconfig">Manually set Time/Date</a></li>
        <li><a href="/update">Over-the-Air (OTA) Update</a></li>
      </ul>
    </body>
    </html>
  )rawliteral");
});

server.on("/config", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>"
                  "<title>Temperature Detection Configuration</title>"
                  "<style>"
                  "body { font-family: Arial, sans-serif; margin: 20px; }"
                  "form { max-width: 400px; margin: auto; }"
                  "label { display: block; margin-top: 10px; }"
                  "input { width: 100%; padding: 5px; margin-top: 5px; }"
                  ".button-container { text-align: center; margin-top: 20px; }"
                  ".reset-button { background-color: red; color: white; border: none; padding: 10px; cursor: pointer; }"
                  ".reset-button:hover { background-color: darkred; }"
                  "</style>"
                  "<script>"
                  "function resetConfig() {"
                  " fetch('/resetconfig', { method: 'POST' }).then(() => location.reload());"
                  "}"
                  "</script>"
                  "</head><body>"
                  "<h2>Temperature Sensor Configuration</h2>"
                  "<form action='/saveconfig' method='POST'>"
                  
                  "<label for='threshold'>Temperature Drop Threshold (&deg;C):</label>"
                  "<input type='number' step='0.1' id='threshold' name='threshold' value='" + String(threshold) + "' min='0' max='10'>"

                  "<label for='measureDuration'>Measurement Duration (ms):</label>"
                  "<input type='number' id='measureDuration' name='measureDuration' value='" + String(measureDuration) + "' min='1000' max='20000'>"

                  "<label for='sleepDuration'>Sleep Duration (ms):</label>"
                  "<input type='number' id='sleepDuration' name='sleepDuration' value='" + String(sleepDuration) + "' min='1000' max='100000'>"

                  "<div class='button-container'>"
                  "<input type='submit' value='Save'>"
                  "<button type='button' class='reset-button' onclick='resetConfig()'>Reset to Defaults</button>"
                  "</div>"
                  "</form><br><a href='/'>Back to Home</a></body></html>";

    server.send(200, "text/html", html);
});


server.on("/saveconfig", HTTP_POST, []() {
    if (server.hasArg("threshold")) {
        threshold = server.arg("threshold").toFloat();
        preferences.putFloat("threshold", threshold);
    }
    if (server.hasArg("measureDuration")) {
        measureDuration = server.arg("measureDuration").toInt();
        preferences.putUInt("measureDuration", measureDuration);
    }
    if (server.hasArg("sleepDuration")) {
        sleepDuration = server.arg("sleepDuration").toInt();
        preferences.putUInt("sleepDuration", sleepDuration);
    }
    server.send(200, "text/html", "<h2>Configuration Saved! <a href='/config'>Back</a></h2>");
});


server.on("/resetconfig", HTTP_POST, []() {
    threshold = TempChangeThresholdDefault;
    measureDuration = MeasureDurationDefault;
    sleepDuration = SleepDurationDefault;

    preferences.putFloat("threshold", threshold);
    preferences.putUInt("measureDuration", measureDuration);
    preferences.putUInt("sleepDuration", sleepDuration);

    server.send(200, "text/plain", "Configuration reset to default values.");
});


server.on("/timeconfig", HTTP_GET, []() {
  String html = R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
          <title>Set RTC Time</title>
          <style>
              body { font-family: Arial, sans-serif; margin: 20px; }
              form { max-width: 300px; margin: auto; }
              label { display: block; margin-top: 10px; }
              input { width: 100%; padding: 5px; margin-top: 5px; }
              .button-container { text-align: center; margin-top: 20px; }
          </style>
          <script>
              function setToBrowserTime() {
                  let now = new Date();
                  document.getElementById('year').value = now.getFullYear();
                  document.getElementById('month').value = now.getMonth() + 1; // Month is 0-based
                  document.getElementById('day').value = now.getDate();
                  document.getElementById('hour').value = now.getHours();
                  document.getElementById('minute').value = now.getMinutes();
                  document.getElementById('second').value = now.getSeconds();
              }
          </script>
      </head>
      <body>
          <h2>Set RTC Time</h2>
          <form action="/settime" method="POST">
              <label for="year">Year:</label>
              <input type="number" id="year" name="year" min="2020" max="2099" required>

              <label for="month">Month:</label>
              <input type="number" id="month" name="month" min="1" max="12" required>

              <label for="day">Day:</label>
              <input type="number" id="day" name="day" min="1" max="31" required>

              <label for="hour">Hour:</label>
              <input type="number" id="hour" name="hour" min="0" max="23" required>

              <label for="minute">Minute:</label>
              <input type="number" id="minute" name="minute" min="0" max="59" required>

              <label for="second">Second:</label>
              <input type="number" id="second" name="second" min="0" max="59" required>

              <div class="button-container">
                  <input type="submit" value="Set Time">
                  <button type="button" onclick="setToBrowserTime()">Use Browser Time</button>
              </div>
          </form>
          <br>
          <a href="/">Back to Home</a>
      </body>
      </html>
  )rawliteral";

  server.send(200, "text/html", html);
});



server.on("/settime", HTTP_POST, []() {
  if (server.hasArg("year") && server.hasArg("month") && server.hasArg("day") &&
      server.hasArg("hour") && server.hasArg("minute") && server.hasArg("second")) {

      int year = server.arg("year").toInt();
      int month = server.arg("month").toInt();
      int day = server.arg("day").toInt();
      int hour = server.arg("hour").toInt();
      int minute = server.arg("minute").toInt();
      int second = server.arg("second").toInt();

      if(rtcAvailable)
      {
        rtc.adjust(DateTime(year, month, day, hour, minute, second));
        server.send(200, "text/html", "<h2>RTC Time Updated! <a href='/'>Back to Home</a></h2>");
      }
      else
      {
        server.send(500, "text/html", "<h2>Missing RTC. <a href='/'>Back to Home</a></h2>");  
      }
  } else {
      server.send(400, "text/html", "<h2>Invalid Input. <a href='/timeconfig'>Back</a></h2>");
  }
});


  server.on("/wificonfig", HTTP_GET, []() {
    server.send(200, "text/html", R"rawliteral(
      <!DOCTYPE html>
      <html>
      <body>
        <h2>WiFi Configuration</h2>
        <form action="/save" method="POST">
          <label for="ssid">SSID:</label><br>
          <input type="text" id="ssid" name="ssid"><br><br>
          <label for="password">Password:</label><br>
          <input type="password" id="password" name="password"><br><br>
          <input type="submit" value="Save">
        </form>
        <br>
        <a href='/'>Back to Home</a>
      </body>
      </html>
    )rawliteral");
  });

  server.on("/restart", HTTP_POST, []() {
      server.send(200, "text/plain", "Restarting...");
      delay(1000);
      ESP.restart();
  });

  server.on("/visualize", HTTP_GET, generateVisualizationPage);

  server.on("/save", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");

    if (newSSID.length() > 0 && newPassword.length() > 0) {
      saveWiFiCredentials(newSSID, newPassword);
      server.send(200, "text/html", "<h2>WiFi credentials saved! Restarting...</h2>");
      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/html", "<h2>Error: SSID or Password is empty!</h2>");
    }
  });


server.on("/update", HTTP_GET, []() {
  server.send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html>
    <body>
      <h2>Firmware Update</h2>
      <form method="POST" action="/update" enctype="multipart/form-data">
        <input type="file" name="firmware"><br><br>
        <input type="submit" value="Upload and Update">
      </form>
      <br>
      <a href='/'>Back to Home</a>
    </body>
    </html>
  )rawliteral");
});

server.on("/update", HTTP_POST, []() {
  server.send(200, "text/plain", "Update Complete. Rebooting...");
  delay(100);
  ESP.restart();
}, []() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start with unknown size
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Write the received bytes to the flash
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { // Finish updating
      Serial.printf("Update Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
});

server.on("/grid", HTTP_GET, []() {
  server.send(200, "text/html", R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
          <style>
              .table-container {
                  display: block;
                  overflow-x: auto;
                  white-space: nowrap;
                  position: relative;
                  padding: 10px;
                  margin-left: auto;
                  margin-right: auto;
              }
              table {
                  font-family: Arial, sans-serif;
                  border-collapse: collapse;
                  margin: 0 auto;
                  width: max-content;
              }
              td, th {
                  border: 1px solid #dddddd;
                  text-align: center;
                  padding: 4px;
              }
              th.full-hour, td.full-hour {
                  border-left: 3px solid black !important; /* Thicker border for full hours */
              }
              tr:nth-child(even) {
                  background-color: #f9f9f9;
              }
              input {
                  width: 30px;
                  text-align: center;
              }
              .save-button-container {
                  text-align: center;
                  margin-top: 20px;
                  display: flex;
                  justify-content: center;
                  gap: 15px;
              }
              .save-button-container button {
                  font-size: 16px;
                  padding: 10px 20px;
                  width: auto;
                  height: 50px;
                  box-sizing: border-box;
              }
              .reset-button {
                  color: white;
                  background-color: red;
                  border: none;
                  cursor: pointer;
              }
              .reset-button:hover {
                  background-color: darkred;
              }
          </style>
          <script>
              document.addEventListener("DOMContentLoaded", function () {
                  fetchGridData();
              });

              function fetchGridData() {
                  fetch('/getgrid')
                      .then(response => response.json())
                      .then(data => {
                          const days = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
                          let html = '<form id="gridForm"><table><tr><th>Day</th>';

                          // Generate time slot headers
                          for (let i = 0; i < 96; i++) {
                              if (i % 4 === 0) {
                                  html += `<th class="full-hour">${Math.floor(i / 4)}:00</th>`;
                              } else {
                                  html += "<th></th>";
                              }
                          }
                          html += '</tr>';

                          // Generate grid rows with input fields
                          for (let day = 0; day < 7; day++) {
                              html += `<tr><td>${days[day]}</td>`;
                              for (let slot = 0; slot < 96; slot++) {
                                  let extraClass = (slot % 4 === 0) ? "full-hour" : "";
                                  html += `<td class="${extraClass}"><input type="number" size="2" name="slot${day}_${slot}" value="${data[day][slot]}" min="0" max="255"></td>`;
                              }
                              html += '</tr>';
                          }
                          html += '</table></form>';

                          document.getElementById("gridContainer").innerHTML = html;
                          highlightNonZeroValues();
                      })
                      .catch(error => console.error('Error fetching grid data:', error));
              }

              function highlightNonZeroValues() {
                  document.querySelectorAll('input[type="number"]').forEach(input => {
                      input.style.backgroundColor = (parseInt(input.value) > 0) ? 'yellow' : '';
                  });
              }

              function saveGridData() {
                  const formData = new FormData(document.getElementById("gridForm"));
                  fetch('/savegrid', {
                      method: 'POST',
                      body: new URLSearchParams(formData) // Convert form data to a URL-encoded string
                  }).then(response => {
                      if (response.ok) {
                          alert("Grid saved successfully!");
                          highlightNonZeroValues();
                      } else {
                          alert("Error saving grid!");
                      }
                  }).catch(error => {
                      alert("Error: " + error);
                  });
              }

              function confirmReset() {
                  if (confirm('Are you sure you want to reset the entire grid?')) {
                      fetch('/resetgrid', { method: 'POST' })
                          .then(response => response.ok ? fetchGridData() : alert('Failed to reset grid'))
                          .catch(error => alert('Error resetting grid: ' + error));
                  }
              }
          </script>
      </head>
      <body>
          <h2>Edit Usage Grid</h2>
          <div class="table-container" id="gridContainer">
              <p>Loading grid...</p>
          </div>
          <div class="save-button-container">
              <button type="button" onclick="saveGridData()">Save</button>
              <button type="button" class="reset-button" onclick="confirmReset()">Reset All</button>
          </div>
          <br>
          <a href='/'>Back to Home</a>
      </body>
      </html>
  )rawliteral");
});


server.on("/getgrid", HTTP_GET, []() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);  // Enables chunked transfer
  server.send(200, "application/json", "[");  // Start JSON array

  for (int day = 0; day < 7; day++) {
      String dayJson = "[";  // Build day's data in a single string

      for (int slot = 0; slot < 96; slot++) {
          dayJson += String(usagePattern[day][slot]);
          if (slot < 95) dayJson += ",";  // Add comma between values
      }

      dayJson += "]";  // Close the day array
      server.sendContent(dayJson);  // Send the entire day's data at once

      if (day < 6) server.sendContent(",");  // Add comma between days
  }

  server.sendContent("]");  // End JSON array
});



server.on("/savegrid", HTTP_POST, []() {
  bool updated = false;

  for (int day = 0; day < 7; day++) {
    for (int slot = 0; slot < 96; slot++) {
      String paramName = "slot" + String(day) + "_" + String(slot);
      if (server.hasArg(paramName)) {
        int value = server.arg(paramName).toInt();
        // Constrain the value between 0 and 255
        usagePattern[day][slot] = constrain(value, 0, 255);
        updated = true;
      }
    }
  }

  if (updated) {
    saveUsagePattern(); // Save the updated grid persistently
    server.send(200, "text/html", "<h2>Usage grid saved successfully! <a href=\"/grid\">Back</a></h2>");
  } else {
    server.send(400, "text/html", "<h2>Error: No data received. <a href=\"/grid\">Back</a></h2>");
  }
});


server.on("/resetgrid", HTTP_POST, []() {
  // Clear the usage pattern array
  memset(usagePattern, 0, sizeof(usagePattern));

  // Save the cleared usage pattern
  saveUsagePattern();

  // Log the reset operation
  Serial.println("Usage grid reset to zero.");

  // Respond to the client
  server.send(200, "text/plain", "Grid reset successfully.");
});


server.on("/status", HTTP_GET, []() {
  server.send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>Status</title>
      <style>
        body {
          font-family: Arial, sans-serif;
          margin: 20px;
        }
        h1 {
          text-align: center;
        }
        .info-box {
          margin: 20px auto;
          text-align: center;
          padding: 10px;
          border: 1px solid #ccc;
          width: 50%;
        }
      </style>
      <script>
        function updateStatus() {
          fetch('/getstatus')
            .then(response => response.json())
            .then(data => {
              document.getElementById('dateTime').innerText = data.dateTime;
              document.getElementById('uptime').innerText = data.uptime;
              document.getElementById('initialAvgTemp').innerText = data.initialAvgTemp;
              document.getElementById('currentAvgTemp').innerText = data.currentAvgTemp;
              document.getElementById('deltaTemp').innerText = data.deltaTemp;
              document.getElementById('pumpStatus').innerText = data.pumpStatus;
            })
            .catch(error => console.error('Error fetching status:', error));
        }

        function restartESP() {
            if (confirm('Are you sure you want to restart the device?')) {
                fetch('/restart', { method: 'POST' })
                    .then(response => response.text())
                    .then(text => alert(text))
                    .catch(error => alert('Restart failed: ' + error));
            }
        }        

        setInterval(updateStatus, 3000); // Fetch all data every 3 seconds
      </script>
    </head>
    <body>
      <h1>Status</h1>
      <div class="info-box">
        <p><strong>Current Date & Time:</strong> <span id="dateTime">Loading...</span></p>
        <p><strong>Uptime:</strong> <span id="uptime">Loading...</span></p>
        <p><strong>Previous Temperature:</strong> <span id="initialAvgTemp">Loading...</span></p>
        <p><strong>Current Temperature:</strong> <span id="currentAvgTemp">Loading...</span></p>
        <p><strong>Delta Temperature:</strong> <span id="deltaTemp">Loading...</span></p>
        <p><strong>Pump Status:</strong> <span id="pumpStatus">Loading...</span></p>
        <p><button onclick="restartESP()">Restart</button></p>
      </div>
      <br>
      <a href='/'>Back to Home</a>
    </body>
    </html>
  )rawliteral");
});


server.on("/getstatus", HTTP_GET, []() {
  StaticJsonDocument<256> doc;
  doc["dateTime"] = getCurrentDateTime();   // Replace with your function
  doc["uptime"] = getUptime();              // Replace with your function
  doc["initialAvgTemp"] = String(initialAvgTemp);
  doc["currentAvgTemp"] = String(currentAvgTemp);
  doc["deltaTemp"] = String(deltaT);
  doc["pumpStatus"] = pumpActivated ? "ON" : "OFF"; // Replace with actual logic

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
});

  server.begin();
}

void loop() {
  static unsigned long lastTimeSync = 0;
  static bool wifiConnecting = false;

  ArduinoOTA.handle();

  if (isOtaUpdating) {
      server.stop();
      return;  // Skip the rest of the loop
  }

  if (strlen(ssid) > 0 && WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost. Reconnecting...");
    WiFi.reconnect();
    wifiConnecting = true;
  }

  if(wifiConnecting && WiFi.status() == WL_CONNECTED)
  {
    wifiConnecting = false;
    server.begin();
    setupOTA();
  }
  
  // Periodically update the timezone and resync time (every 24 hours)
  if (millis() - lastTimeSync > 86400000) { // 24 hours in milliseconds
    fetchTimezoneOffset();
    synchronizeTime();
    lastTimeSync = millis();
  }

  server.handleClient();
  
  temperatureMeasurement();

  // Update usage patterns every 15 minutes
  if (millis() - lastSampleTime > sampleInterval) {
    lastSampleTime = millis();
    updateUsagePattern();
  }

  // Activate or deactivate pump based on the usage pattern
  int currentDay = getCurrentDay();
  int currentSlot = getCurrentTimeSlot();
  if (shouldActivatePump(currentDay, currentSlot)) {
    activatePump();
  } else {
    deactivatePump();
  }
}

String getCurrentDateTime() {
  time_t now = time(nullptr);
  now += utcOffsetSeconds;
  struct tm *timeinfo = gmtime(&now);
  
  if (timeinfo == nullptr) {
      return "Time not available";
  }

  char buffer[30];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
           timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

  return String(buffer);
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

          Serial.print("Measured Temperature: ");
          Serial.print(currentTemp);
          Serial.println(" °C");

          // Start the next async measurement immediately
          sensors.requestTemperatures();
          measuring = true;
      }

      // End of measurement phase after `measureDuration` seconds
      if (currentMillis - cycleStartTime >= measureDuration) {
          if (numReadings > 0) {
              currentAvgTemp = sumTemp / numReadings;
              deltaT = currentAvgTemp - initialAvgTemp;

              Serial.print("Average Temp: ");
              Serial.print(currentAvgTemp);
              Serial.println(" °C");

              Serial.print("ΔT: ");
              Serial.print(deltaT);
              Serial.println(" °C");

              if (deltaT <= -threshold) {
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
      if (currentMillis - cycleStartTime >= sleepDuration) {
          sleepPhase = false;
          cycleStartTime = millis();

          // Update reference temperature
          initialAvgTemp = (currentAvgTemp > 0) ? currentAvgTemp : initialAvgTemp;

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
  time_t now = time(nullptr); // Get the current UTC time
  now += utcOffsetSeconds;   // Adjust for the offset

  struct tm *timeInfo = gmtime(&now); // Use gmtime since we already adjusted for the offset

  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", timeInfo);
  Serial.printf("Local Time: %s (Offset: %d seconds)\n", timeString, utcOffsetSeconds);
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
      
      if (rtcAvailable) {
          rtc.adjust(DateTime(now));  // Store in RTC if available
          Serial.println("RTC updated with NTP time.");
      }
  } else {
      Serial.println("Failed to sync with NTP.");
  }
}



void fetchTimezoneOffset() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://ipwhois.app/json/");
    int httpResponseCode = http.GET();

    if (httpResponseCode == 200) {
      String payload = http.getString();
      Serial.println("API Response:");
      Serial.println(payload); // Log the API response

      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        int gmtOffset = doc["timezone_gmtOffset"].as<int>(); // Parse the correct field
        int dstOffset = doc["timezone_dstOffset"].as<int>(); // Parse DST adjustment

        // Combine the offsets
        utcOffsetSeconds = gmtOffset + dstOffset;

        Serial.printf("Fetched UTC Offset: %d seconds (GMT Offset: %d, DST Offset: %d)\n", 
                      utcOffsetSeconds, gmtOffset, dstOffset);
      } else {
        Serial.println("Failed to parse JSON, keeping existing offset.");
      }
    } else {
      Serial.printf("HTTP GET failed: %d, keeping existing offset.\n", httpResponseCode);
    }

    http.end();
  } else {
    Serial.println("Wi-Fi not connected, keeping existing offset.");
  }
}

void saveWiFiCredentials(const String& newSSID, const String& newPassword) {
  preferences.putString("ssid", newSSID);
  preferences.putString("password", newPassword);
  Serial.println("WiFi credentials saved to flash.");
}

void loadWiFiCredentials() {
  String storedSSID = preferences.getString("ssid", "");
  String storedPassword = preferences.getString("password", "");

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

  // Get current local time
  time_t now = time(nullptr);
  now += utcOffsetSeconds;    // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time struct

  int currentDay = localTime->tm_wday; // Local day (0 = Sunday, ..., 6 = Saturday)
  int currentSlot = (localTime->tm_hour * 4) + (localTime->tm_min / 15); // Local 15-min slot index

  // Update the current slot now
  usagePattern[currentDay][currentSlot] = min(255, usagePattern[currentDay][currentSlot] + IncreaseOnUsage);

  // Save the updated usage pattern
  saveUsagePattern();
  Serial.printf("Usage recorded: Day %d, Slot %d, Value %d\n", currentDay, currentSlot, usagePattern[currentDay][currentSlot]);

  slotRecorded = true;
}


void updateUsagePattern() {
  static int lastSlot = -1;
  static int lastDay = -1;

  int currentSlot = getCurrentTimeSlot();
  int currentDay = getCurrentDay();

  if (currentSlot != lastSlot) {  // Timeslot changed
    if (lastSlot != -1 && !slotRecorded) {  // If last slot exists and no usage was recorded
      int targetDay = lastDay;  // Default: Decrement the previous slot of the same day
      int targetSlot = lastSlot;

      // Handle day overflow (previous slot was 95, new slot is 0 → move to the previous day)
      if (currentSlot == 0) {
        targetDay = (currentDay == 0) ? 6 : currentDay - 1;  // Wrap to previous day (Sunday → Saturday)
        targetSlot = 95;  // Last slot of the previous day
      }

      usagePattern[targetDay][targetSlot] = max(0, usagePattern[targetDay][targetSlot] - DecreaseWhenNoUsage);
      Serial.printf("No usage detected in Slot %d of Day %d → Decrementing usage count.\n", targetSlot, targetDay);
    }

    // Reset for the new timeslot
    slotRecorded = false;
    lastSlot = currentSlot;
    lastDay = currentDay;
    Serial.printf("New timeslot detected: Slot %d on Day %d\n", currentSlot, currentDay);
  }

  saveUsagePattern();
}

int getCurrentDay() {
  // Get current local time
  time_t now = time(nullptr);
  now += utcOffsetSeconds;    // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time struct

  return localTime->tm_wday; // Local day (0 = Sunday, ..., 6 = Saturday)
  }

int getCurrentTimeSlot() {
  // Get current local time
  time_t now = time(nullptr);
  now += utcOffsetSeconds;    // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time struct
  
  return (localTime->tm_hour * 4) + (localTime->tm_min / 15); // Local 15-min slot index
  }

bool shouldActivatePump(int day, int slot) {
  return (usagePattern[day][slot] > 0);
}


void activatePump() {
  if(!pumpActivated)
  {
    digitalWrite(PUMP_RELAY_PIN, HIGH);
    pumpActivated = true;
    Serial.println("Pump activated");
  }
}

void deactivatePump() {
  if(pumpActivated)
  {
    digitalWrite(PUMP_RELAY_PIN, LOW);
    pumpActivated = false;
    Serial.println("Pump deactivated");
  }
}

void saveUsagePattern() {
  preferences.putBytes("usagePattern", usagePattern, sizeof(usagePattern));
}

void loadUsagePattern() {
  if (preferences.getBytes("usagePattern", usagePattern, sizeof(usagePattern)) == 0) {
    memset(usagePattern, 0, sizeof(usagePattern));
  }
}

void generateVisualizationPage() {
  int totalActiveMinutes = 0;  // Tracks total pump runtime in minutes per week

  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <style>
        table {
          font-family: Arial, sans-serif;
          border-collapse: collapse;
          width: 100%;
        }
        td, th {
          border: 1px solid #dddddd;
          text-align: center;
          padding: 8px;
        }
        tr:nth-child(even) {
          background-color: #f9f9f9;
        }
        .summary-box {
          margin-top: 20px;
          padding: 10px;
          border: 1px solid #000;
          text-align: center;
          font-size: 18px;
          font-weight: bold;
          background-color: #f0f0f0;
          width: fit-content;
          margin-left: auto;
          margin-right: auto;
        }
      </style>
    </head>
    <body>
      <h2>Usage Data</h2>
      <table>
        <tr>
          <th>Day</th>
          <th>Time Interval</th>
        </tr>
  )rawliteral";

  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

  for (int day = 0; day < 7; day++) {
    int startSlot = -1;  // Start slot of a usage block
    for (int slot = 0; slot < 96; slot++) {
      if (usagePattern[day][slot] > 0) {
        totalActiveMinutes += 15;  // Count each active 15-min slot
        if (startSlot == -1) {
          startSlot = slot; // Start new usage block
        }
      } else if (startSlot != -1) {
        // End of a usage block, add it to HTML
        int startHour = (startSlot * 15) / 60;
        int startMinute = (startSlot * 15) % 60;
        int endHour = (slot * 15) / 60;
        int endMinute = (slot * 15) % 60;

        html += "<tr>";
        html += "<td>" + String(days[day]) + "</td>";
        html += "<td>" + String(startHour) + ":" + (startMinute < 10 ? "0" : "") + String(startMinute) + " - ";
        html += String(endHour) + ":" + (endMinute < 10 ? "0" : "") + String(endMinute) + "</td>";
        html += "</tr>";

        startSlot = -1; // Reset for the next block
      }
    }

    // If the last slot was a usage slot, close the block
    if (startSlot != -1) {
      int startHour = (startSlot * 15) / 60;
      int startMinute = (startSlot * 15) % 60;
      int endHour = 23;
      int endMinute = 59;

      html += "<tr>";
      html += "<td>" + String(days[day]) + "</td>";
      html += "<td>" + String(startHour) + ":" + (startMinute < 10 ? "0" : "") + String(startMinute) + " - ";
      html += String(endHour) + ":" + (endMinute < 10 ? "0" : "") + String(endMinute) + "</td>";
      html += "</tr>";
    }
  }

  // **Calculate total pump runtime**
  float totalHours = totalActiveMinutes / 60.0;
  float percentage = (totalActiveMinutes / 10080.0) * 100.0; // 10080 minutes in a week

  html += R"rawliteral(
      </table>
      <div class="summary-box">
        <p><strong>Total Pump Running Time:</strong> )rawliteral" + 
        String(totalHours, 1) + " hours per week (" + String(percentage, 1) + "%)</p>" + 
        R"rawliteral(</div>
      <br>
      <a href="/">Back to Home</a>
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

String getUptime() {
  static unsigned long prevMillis = 0;    // Last recorded millis value
  static unsigned int overflowCount = 0;  // Counts how many times millis() has overflowed

  unsigned long currentMillis = millis();

  // Detect millis() overflow (rollover)
  if (currentMillis < prevMillis) {  
      overflowCount++;  // Increment overflow count
  }
  prevMillis = currentMillis;  // Update last recorded value

  // Calculate total elapsed time including overflowed periods
  uint64_t totalMillis = (uint64_t(overflowCount) * 4294967296ULL) + currentMillis;

  // Convert to human-readable format
  unsigned long seconds = totalMillis / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;

  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  // Format uptime string
  char uptimeStr[50];
  snprintf(uptimeStr, sizeof(uptimeStr), "%lu days, %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  return String(uptimeStr);
}

void setupOTA() {
  ArduinoOTA.setHostname(MDNS_NAME);  // Set the OTA hostname to "pump.local"
  
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
