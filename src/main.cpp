#include "Arduino.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <time.h>

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
void IRAM_ATTR flowPulse();
void generateVisualizationPage();
void setupOTA();
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
int timezoneOffset = 0; // Default timezone offset in seconds

// GPIO Pins
#define FLOW_SENSOR_PIN 34
#define PUMP_RELAY_PIN 27

// Usage pattern (7 days x 96 slots for 15-minute intervals)
int usagePattern[7][96] = {0};

// Flow sensor variables
volatile int pulseCount = 0;

// Timer
unsigned long lastSampleTime = 0;
const unsigned long sampleInterval = 900000; // 15 minutes

// Offsets for pump activation
const int startOffset = 10; // Start 10 minutes before the recorded time
const int endOffset = 5;    // Stop 5 minutes after the recorded time

void setup() {
  Serial.begin(115200);

  // Initialize Preferences
  preferences.begin("water-usage", false);

  // Load Wi-Fi credentials
  loadWiFiCredentials();

  // Connect to Wi-Fi
  if(strlen(ssid) > 0)
  {
    WiFi.begin(ssid, password);
    Serial.printf("Connecting to WiFi: %s\n", ssid);
    for(int i=0; i<120; i++)
    {
      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    startAccessPoint();
  }


  // Fetch timezone offset
  fetchTimezoneOffset();

  // Synchronize time with NTP server
  synchronizeTime();

  // Set up OTA updates
  setupOTA();

  // Set up flow sensor
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulse, RISING);

  // Set up pump relay
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);

  // Load usage patterns
  loadUsagePattern();

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
        <li><a href="/grid">Edit Usage Grid</a></li>
        <li><a href="/visualize">View Usage Data</a></li>
        <li><a href="/wificonfig">WiFi Configuration</a></li>
        <li><a href="/update">Over-the-Air (OTA) Update</a></li>
      </ul>
    </body>
    </html>
  )rawliteral");
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
        <a href="/visualize">View Usage Data</a>
      </body>
      </html>
    )rawliteral");
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
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <style>
        .table-container {
          display: block;
          overflow-x: auto; /* Allow horizontal scrolling */
          white-space: nowrap; /* Prevent wrapping of table rows */
          position: relative;
          padding: 10px;
          margin-left: auto; /* Center align table within the scrollable container */
          margin-right: auto; /* Center align table within the scrollable container */
        }
        table {
          font-family: Arial, sans-serif;
          border-collapse: collapse;
          margin: 0 auto; /* Center table within its container */
          width: max-content; /* Ensure table is only as wide as its content */
        }
        td, th {
          border: 1px solid #dddddd;
          text-align: center;
          padding: 4px;
        }
        th.full-hour {
          border-left: 2px solid #000; /* Thicker border for full hour */
        }
        td.full-hour {
          border-left: 2px solid #000; /* Thicker border for full hour */
        }
        tr:nth-child(even) {
          background-color: #f9f9f9;
        }
        input {
          width: 30px; /* Smaller input field */
          text-align: center;
        }
        .save-button-container {
          text-align: center;
          margin-top: 20px;
        }
        .save-button-container input {
          font-size: 16px;
          padding: 10px 20px; /* Proper padding for the button */
          width: auto; /* Let the button adjust to its content */
          display: inline-block;
        }
        body {
          margin: 0;
          padding: 0;
          display: flex;
          flex-direction: column;
          align-items: center;
        }
      </style>
      <script>
        function scrollTableToStart() {
          const container = document.querySelector('.table-container');
          container.scrollLeft = 0; // Ensure scrolling starts at the beginning
        }

        function highlightNonZeroValues() {
          const inputs = document.querySelectorAll('input[type="number"]');
          inputs.forEach(input => {
            if (parseInt(input.value) > 0) {
              input.style.backgroundColor = 'yellow';
            } else {
              input.style.backgroundColor = '';
            }
          });
        }

        document.addEventListener('DOMContentLoaded', () => {
          scrollTableToStart(); // Scroll to the beginning on page load
          highlightNonZeroValues(); // Highlight on page load
          document.querySelectorAll('input[type="number"]').forEach(input => {
            input.addEventListener('input', highlightNonZeroValues); // Update on input
          });
        });
      </script>
    </head>
    <body>
      <h2>Usage Grid</h2>
      <div class="table-container">
        <form method="POST" action="/savegrid">
          <table>
            <tr>
              <th>Day</th>
  )rawliteral";

  // Add column descriptions for every full hour
  for (int i = 0; i < 96; i++) {
    if (i % 4 == 0) { // Start of each full hour
      html += "<th class='full-hour'>" + String(i / 4) + ":00</th>";
    } else {
      html += "<th></th>";
    }
  }
  html += "</tr>";

  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  for (int day = 0; day < 7; day++) {
    html += "<tr>";
    html += "<td>" + String(days[day]) + "</td>";
    for (int slot = 0; slot < 96; slot++) {
      if (slot % 4 == 0) { // Add thicker border for full hour
        html += "<td class='full-hour'><input type='number' size='2' name='slot" + String(day) + "_" + String(slot) + "' value='" + String(usagePattern[day][slot]) + "' min='0' max='255'></td>";
      } else {
        html += "<td><input type='number' size='2' name='slot" + String(day) + "_" + String(slot) + "' value='" + String(usagePattern[day][slot]) + "' min='0' max='255'></td>";
      }
    }
    html += "</tr>";
  }

  html += R"rawliteral(
          </table>
          <div class="save-button-container">
            <input type="submit" value="Save">
          </div>
        </form>
      </div>
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
});









server.on("/savegrid", HTTP_POST, []() {
  bool updated = false;

  for (int day = 0; day < 7; day++) {
    for (int slot = 0; slot < 96; slot++) {
      String paramName = "slot" + String(day) + "_" + String(slot);
      if (server.hasArg(paramName)) {
        int value = server.arg(paramName).toInt();
        usagePattern[day][slot] = constrain(value, 0, 255); // Ensure within bounds
        updated = true;
      }
    }
  }

  if (updated) {
    saveUsagePattern();
    server.send(200, "text/html", "<h2>Usage grid saved successfully! <a href=\"/grid\">Back</a></h2>");
  } else {
    server.send(400, "text/html", "<h2>Error: No data received. <a href=\"/grid\">Back</a></h2>");
  }
});


  server.begin();
}

void loop() {
  server.handleClient();

  // Detect water flow and record usage
  if (pulseCount > 0) {
    recordUsage();
    pulseCount = 0;
  }

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

  ArduinoOTA.handle();
}

void startAccessPoint() {
  WiFi.softAP("CirculationPumpSmartControl", "12345678");
  Serial.printf("Access Point IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void synchronizeTime() {
  configTime(timezoneOffset, 0, ntpServer);
  Serial.println("Synchronizing time...");
  time_t now = time(nullptr);

  if(strlen(ssid) > 0)
  {
    for(int i=0; i<20; i++)
    {
      if (now < 100000) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
      }
    }
    if (now > 100000)
    {
      Serial.println("\nTime synchronized!");
    }
  }
}

void fetchTimezoneOffset() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://ip-api.com/json/");
    int httpResponseCode = http.GET();

    if (httpResponseCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        timezoneOffset = doc["offset"].as<int>();
        String timezone = doc["timezone"].as<String>();
        Serial.printf("Timezone: %s, Offset: %d seconds\n", timezone.c_str(), timezoneOffset);
      } else {
        Serial.println("Failed to parse JSON");
      }
    } else {
      Serial.printf("HTTP GET failed: %d\n", httpResponseCode);
    }

    http.end();
  } else {
    Serial.println("Wi-Fi not connected");
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

  int currentDay = getCurrentDay();
  int currentSlot = getCurrentTimeSlot();

  // Check if the usage has already been recorded for the current slot
  if (slotRecorded) {
    Serial.printf("Usage already recorded for Day %d, Slot %d.\n", currentDay, currentSlot);
    return; // Skip recording
  }

  // Mark the slot as recorded
  slotRecorded = true;

  // Handle day rollover for neighboring slots
  int prevDay = (currentDay == 0) ? 6 : currentDay - 1; // Previous day
  int nextDay = (currentDay == 6) ? 0 : currentDay + 1; // Next day

  // Add 0.5 * IncreaseOnUsage to the previous slot (or last slot of the previous day)
  if (currentSlot == 0) {
    usagePattern[prevDay][95] = min(255, usagePattern[prevDay][95] + IncreaseOnUsage / 2);
  } else {
    usagePattern[currentDay][currentSlot - 1] = min(255, usagePattern[currentDay][currentSlot - 1] + IncreaseOnUsage / 2);
  }

  // Add IncreaseOnUsage to the target slot
  usagePattern[currentDay][currentSlot] = min(255, usagePattern[currentDay][currentSlot] + IncreaseOnUsage);

  // Add 0.5 * IncreaseOnUsage to the next slot (or first slot of the next day)
  if (currentSlot == 95) {
    usagePattern[nextDay][0] = min(255, usagePattern[nextDay][0] + IncreaseOnUsage / 2);
  } else {
    usagePattern[currentDay][currentSlot + 1] = min(255, usagePattern[currentDay][currentSlot + 1] + IncreaseOnUsage / 2);
  }

  saveUsagePattern();
  Serial.printf("Usage recorded: Day %d, Slot %d, Value %d\n", currentDay, currentSlot, usagePattern[currentDay][currentSlot]);
}

void updateUsagePattern() {
  static int lastSlot = -1;

  int currentSlot = getCurrentTimeSlot();
  if (currentSlot != lastSlot) {
    // Reset the flag when the timeslot changes
    slotRecorded = false;
    lastSlot = currentSlot;
    Serial.printf("New timeslot detected: Slot %d\n", currentSlot);
  }

  // Decrease the counter for non-usage
  for (int day = 0; day < 7; day++) {
    for (int slot = 0; slot < 96; slot++) {
      usagePattern[day][slot] = max(0, usagePattern[day][slot] - DecreaseWhenNoUsage);
    }
  }
  saveUsagePattern();
  Serial.println("Usage pattern updated (decremented).");
}

int getCurrentDay() {
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);
  return timeInfo->tm_wday; // 0=Sunday, 1=Monday, ..., 6=Saturday
}

int getCurrentTimeSlot() {
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);
  return (timeInfo->tm_hour * 4) + (timeInfo->tm_min / 15);
}

bool shouldActivatePump(int day, int slot) {
  return usagePattern[day][slot] > 0;
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

void IRAM_ATTR flowPulse() {
  pulseCount++;
}

void generateVisualizationPage() {
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
      </style>
    </head>
    <body>
      <h2>Usage Data</h2>
      <table>
        <tr>
          <th>Day</th>
          <th>Time Interval</th>
          <th>Usage Count</th>
        </tr>
  )rawliteral";

  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  for (int day = 0; day < 7; day++) {
    for (int slot = 0; slot < 96; slot++) {
      if (usagePattern[day][slot] > 0) {
        int startHour = (slot * 15) / 60;
        int startMinute = (slot * 15) % 60;
        int endHour = ((slot + 1) * 15) / 60;
        int endMinute = ((slot + 1) * 15) % 60;

        html += "<tr>";
        html += "<td>" + String(days[day]) + "</td>";
        html += "<td>" + String(startHour) + ":" + (startMinute < 10 ? "0" : "") + String(startMinute) + " - ";
        html += String(endHour) + ":" + (endMinute < 10 ? "0" : "") + String(endMinute) + "</td>";
        html += "<td>" + String(usagePattern[day][slot]) + "</td>";
        html += "</tr>";
      }
    }
  }

  html += R"rawliteral(
      </table>
      <br>
      <a href="/">Back to Home</a>
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
}


void setupOTA() {
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\n", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready");
}