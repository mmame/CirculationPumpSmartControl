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
void generateVisualizationPage();
void setupOTA();
void readAndSmoothFlowValue();

int utcOffsetSeconds = 0; // Global variable to store the UTC offset
bool pumpActivated = false;
bool slotRecorded = false; // Tracks if usage has been recorded for the current slot
const int FLOW_SENSOR_THRESHOLD = 600;    // Threshold for detecting flow
const int MAX_ALLOWED_PEAK_BUMP_SUPPRESSION_VALUE = 2000; 
float smoothedFlowValue = 0;   // Smoothed flow value
int peakFlowValue = 0;
u_int32_t rmsValue = 0;             // sum value detected within the current interval
unsigned long lastSmoothTime = 0; // Timestamp for the last smoothing operation
const unsigned long smoothingInterval = 2000; // Smoothing interval in milliseconds
const float alpha = 0.1;        // Smoothing factor (adjust for desired smoothness)

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
#define FLOW_SENSOR_PIN 34
#define PUMP_RELAY_PIN 27

#define FLOW_SENSOR_THRESHOLD 1000

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

  // Initialize Preferences
  preferences.begin("water-usage", false);

  // Set the ADC attenuation to 0dB for 0–1.1V range
  analogSetPinAttenuation(FLOW_SENSOR_PIN, ADC_0db);

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
  pinMode(FLOW_SENSOR_PIN, INPUT);
  
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
        <li><a href="/status">Status</a></li> 
        <li><a href="/visualize">View Usage Data</a></li>
        <li><a href="/grid">Edit Usage Grid</a></li>
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
          gap: 15px; /* Space between buttons */
        }
        .save-button-container input,
        .save-button-container button {
          font-size: 16px;
          padding: 10px 20px; /* Proper padding for both buttons */
          width: auto;
          height: 50px; /* Ensure both buttons are the same height */
          box-sizing: border-box; /* Ensure padding is included in the height */
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

        function confirmReset() {
          if (confirm('Are you sure you want to reset the entire grid? This action cannot be undone!')) {
            fetch('/resetgrid', { method: 'POST' })
              .then(response => {
                if (response.ok) {
                  // Reset successful: Clear inputs in the browser
                  const inputs = document.querySelectorAll('input[type="number"]');
                  inputs.forEach(input => {
                    input.value = 0;
                    input.style.backgroundColor = ''; // Clear yellow highlight
                  });
                  alert('Grid reset successfully!');
                } else {
                  throw new Error('Failed to reset grid.');
                }
              })
              .catch(error => {
                alert('Error resetting the grid: ' + error);
              });
          }
        }

        document.addEventListener('DOMContentLoaded', () => {
          highlightNonZeroValues();
          document.querySelectorAll('input[type="number"]').forEach(input => {
            input.addEventListener('input', highlightNonZeroValues);
          });
        });
      </script>
    </head>
    <body>
      <h2>Edit Usage Grid</h2>
      <div class="table-container">
        <form method="POST" action="/savegrid">
          <table>
            <tr>
              <th>Day</th>
  )rawliteral";

  // Add column headers for the time slots (local time)
  for (int i = 0; i < 96; i++) {
    if (i % 4 == 0) { // Add labels at full hours
      html += "<th>" + String(i / 4) + ":00</th>";
    } else {
      html += "<th></th>";
    }
  }
  html += "</tr>";

  // Add rows for each day of the week
  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  for (int day = 0; day < 7; day++) {
    html += "<tr>";
    html += "<td>" + String(days[day]) + "</td>";
    for (int slot = 0; slot < 96; slot++) {
      // Generate input fields for the usage grid
      html += "<td><input type='number' size='2' name='slot" + String(day) + "_" + String(slot) + 
              "' value='" + String(usagePattern[day][slot]) + "' min='0' max='255'></td>";
    }
    html += "</tr>";
  }

  html += R"rawliteral(
          </table>
          <div class="save-button-container">
            <input type="submit" value="Save">
            <button type="button" class="reset-button" onclick="confirmReset()">Reset All</button>
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


server.on("/getFlowValue", HTTP_GET, []() {
  // Prepare the response in JSON format
  String flowData = "{";
  flowData += "\"avgFlowValue\": " + String(smoothedFlowValue, 2) + ", ";
  flowData += "\"peakFlowValue\": " + String(peakFlowValue);
  flowData += "}";

  // Send the response with appropriate headers
  server.send(200, "application/json", flowData);
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
        function updateFlowValue() {
          fetch('/getFlowValue')
            .then(response => response.json())
            .then(data => {
              // Update the flow values on the webpage
              document.getElementById('avgFlowValue').innerText = data.avgFlowValue;
              document.getElementById('peakFlowValue').innerText = data.peakFlowValue;
            })
            .catch(error => console.error('Error fetching flow value:', error));
        }

        function updateDateTime() {
          fetch('/getDateTime')
            .then(response => response.json())
            .then(data => {
              document.getElementById('dateTime').innerText = data.dateTime;
            })
            .catch(error => console.error('Error fetching date and time:', error));
        }

        function updatePumpStatus() {
          fetch('/getPumpStatus')
            .then(response => response.json())
            .then(data => {
              document.getElementById('pumpStatus').innerText = data.pumpStatus;
            })
            .catch(error => console.error('Error fetching pump status:', error));
        }

        function updateAll() {
          updateFlowValue();
          updateDateTime();
          updatePumpStatus();
        }

        setInterval(updateAll, 1000); // Update every second
      </script>
    </head>
    <body>
      <h1>Status</h1>
      <div class="info-box">
        <p><strong>Current Date & Time:</strong> <span id="dateTime">Loading...</span></p>
        <p><strong>Avg Flow Value:</strong> <span id="avgFlowValue">Loading...</span></p>
        <p><strong>Peak Flow Value:</strong> <span id="peakFlowValue">Loading...</span></p>
        <p><strong>Pump Status:</strong> <span id="pumpStatus">Loading...</span></p>
      </div>
    </body>
    </html>
  )rawliteral");
});


server.on("/getDateTime", HTTP_GET, []() {
  // Get current local time
  time_t now = time(nullptr); // Get current UTC time
  now += utcOffsetSeconds;    // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time struct

  char timeString[128];
  strftime(timeString, sizeof(timeString), "%A, %Y-%m-%d %H:%M:%S", localTime); // Format: Day, Date, Time

  // Create JSON response
  String json = "{";
  json += "\"dateTime\": \"" + String(timeString) + "\", ";
  json += "\"utcOffset\": " + String(utcOffsetSeconds);
  json += "}";

  server.send(200, "application/json", json);
});

server.on("/getPumpStatus", HTTP_GET, []() {
  // Get the current local time
  time_t now = time(nullptr);
  now += utcOffsetSeconds; // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time

  // Format the local time
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", localTime);

  // Create JSON response
  String json = "{";
  json += "\"pumpStatus\": \"" + String(pumpActivated ? "Active" : "Inactive") + "\", ";
  json += "\"localTime\": \"" + String(timeString) + "\"";
  json += "}";

  // Send the JSON response
  server.send(200, "application/json", json);
});


  server.begin();
}

void loop() {
  static unsigned long lastTimeSync = 0;

  // Periodically update the timezone and resync time (every 24 hours)
  if (millis() - lastTimeSync > 86400000) { // 24 hours in milliseconds
    fetchTimezoneOffset();
    synchronizeTime();
    lastTimeSync = millis();
  }

  server.handleClient();

  // Perform ADC reading and smoothing
  readAndSmoothFlowValue();

  // Check if the smoothed value exceeds the threshold
  if (smoothedFlowValue > FLOW_SENSOR_THRESHOLD) {
    recordUsage(); // Record usage if the smoothed value crosses the threshold
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

  // Configure the NTP server
  configTime(0, 0, "pool.ntp.org"); // Set NTP server

  // Wait for time synchronization
  time_t now = time(nullptr);
  for (int i = 0; i < 20; i++) { // Wait up to 10 seconds for time sync
    if (now > 100000) break;    // Valid time synchronized
    delay(500);
    now = time(nullptr);
    Serial.print(".");
  }

  if (now > 100000) {
    Serial.println("\nTime synchronized!");
    logCurrentTime(); // Log the adjusted local time
  } else {
    Serial.println("\nFailed to synchronize time.");
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

void readAndSmoothFlowValue() {
  // Read the flow sensor value
  int rawFlowValue = analogRead(FLOW_SENSOR_PIN);

  // Update rms value
   rmsValue += rawFlowValue;

  if(rawFlowValue > peakFlowValue)
  {
    peakFlowValue = rawFlowValue;
  }

  // Check if it's time to update the smoothed value
  if (millis() - lastSmoothTime >= smoothingInterval) {
    lastSmoothTime = millis(); // Update the timestamp

    if(peakFlowValue < MAX_ALLOWED_PEAK_BUMP_SUPPRESSION_VALUE)
    {
      // Apply an exponential moving average to the rms value
      smoothedFlowValue = (alpha * rmsValue) + ((1 - alpha) * smoothedFlowValue);
    }

    // Debug output
    Serial.printf("peakValue=%d rmsValue=%d, smoothedFlowValue=%.3f\n", peakFlowValue, rmsValue, smoothedFlowValue);

    // Reset the peak/rms value for the next interval
    rmsValue = 0;
    peakFlowValue = 0;
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

  if(slotRecorded)
  {
    return;
  }

  // Get current local time
  time_t now = time(nullptr);
  now += utcOffsetSeconds;    // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time struct

  int currentDay = localTime->tm_wday; // Local day (0 = Sunday, ..., 6 = Saturday)
  int currentSlot = (localTime->tm_hour * 4) + (localTime->tm_min / 15); // Local 15-min slot index

  // Handle day rollover for neighboring slots
  int prevDay = (currentDay == 0) ? 6 : currentDay - 1; // Previous day
  int nextDay = (currentDay == 6) ? 0 : currentDay + 1; // Next day

  // Add 0.5 * IncreaseOnUsage to the previous slot (or last slot of the previous day)
  if (currentSlot == 0) {
    usagePattern[prevDay][95] = min(255, usagePattern[prevDay][95] + IncreaseOnUsage / 2);
  } else {
    usagePattern[currentDay][currentSlot - 1] = min(255, usagePattern[currentDay][currentSlot - 1] + IncreaseOnUsage / 2);
  }

  // Add full IncreaseOnUsage to the current slot
  usagePattern[currentDay][currentSlot] = min(255, usagePattern[currentDay][currentSlot] + IncreaseOnUsage);

  // Add 0.5 * IncreaseOnUsage to the next slot (or first slot of the next day)
  if (currentSlot == 95) {
    usagePattern[nextDay][0] = min(255, usagePattern[nextDay][0] + IncreaseOnUsage / 2);
  } else {
    usagePattern[currentDay][currentSlot + 1] = min(255, usagePattern[currentDay][currentSlot + 1] + IncreaseOnUsage / 2);
  }

  // Save the updated usage pattern
  saveUsagePattern();
  Serial.printf("Usage recorded: Day %d, Slot %d, Value %d\n", currentDay, currentSlot, usagePattern[currentDay][currentSlot]);
  slotRecorded = true;
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
  // Get current local time
  time_t now = time(nullptr);
  now += utcOffsetSeconds;    // Adjust for local time
  struct tm *localTime = gmtime(&now); // Convert to local time struct

  // Calculate current day and 15-minute slot in local time
  int currentDay = localTime->tm_wday;    // Local day (0 = Sunday, ..., 6 = Saturday)
  int currentSlot = (localTime->tm_hour * 4) + (localTime->tm_min / 15); // Local 15-min slot index

  // Check if the pump should activate
  return (day == currentDay && slot == currentSlot && usagePattern[day][slot] > 0);
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