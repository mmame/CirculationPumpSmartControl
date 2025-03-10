#include "Webserverconfig.hpp"

WebServer server(80);

String getCurrentDateTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "Failed to obtain time";
  }

  char buffer[30];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  return String(buffer);
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

String getStatistics() {
  unsigned int totalWaterUsage = Core::hotWaterUsedWithPump + Core::hotWaterUsedWithoutPump;
  float hitPercentage = (totalWaterUsage > 0) ? (Core::hotWaterUsedWithPump * 100.0 / totalWaterUsage) : 0;
  float missPercentage = (totalWaterUsage > 0) ? (Core::hotWaterUsedWithoutPump * 100.0 / totalWaterUsage) : 0;
  float wastePercentage = (Core::pumpRunWithoutUsage > 0) ? (Core::pumpRunWithoutUsage * 100.0 / (Core::pumpRunWithoutUsage + totalWaterUsage)) : 0;

  const int totalSlots = 7 * 96;
  int activeSlots = 0;

  for (int day = 0; day < 7; day++) {
      for (int slot = 0; slot < 96; slot++) {
          if (Core::usagePattern[day][slot] > 0) {
              activeSlots++;
          }
      }
  }

  float weeklyPercentage = activeSlots * 100.0 / totalSlots;

  String json = "{";
  json += "\"hotWaterUsedWithPump\": " + String(Core::hotWaterUsedWithPump) + ",";
  json += "\"hotWaterUsedWithoutPump\": " + String(Core::hotWaterUsedWithoutPump) + ",";
  json += "\"pumpRunWithoutUsage\": " + String(Core::pumpRunWithoutUsage) + ",";
  json += "\"weekly_percentage\": " + String(weeklyPercentage, 2) + ",";
  json += "\"hit_percentage\": " + String(hitPercentage, 2) + ",";
  json += "\"miss_percentage\": " + String(missPercentage, 2) + ",";
  json += "\"waste_percentage\": " + String(wastePercentage, 2);
  json += "}";
  return json;
}

void webserversetup()
{
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
                        "<input type='number' step='0.1' id='threshold' name='threshold' value='" + String(Core::threshold) + "' min='0' max='10'>"
      
                        "<label for='measureDuration'>Measurement Duration (ms):</label>"
                        "<input type='number' id='measureDuration' name='measureDuration' value='" + String(Core::measureDuration) + "' min='1000' max='20000'>"
      
                        "<label for='sleepDuration'>Sleep Duration (ms):</label>"
                        "<input type='number' id='sleepDuration' name='sleepDuration' value='" + String(Core::sleepDuration) + "' min='1000' max='100000'>"
      
                        "<div class='button-container'>"
                        "<input type='submit' value='Save'>"
                        "<button type='button' class='reset-button' onclick='resetConfig()'>Reset to Defaults</button>"
                        "</div>"
                        "</form><br><a href='/'>Back to Home</a></body></html>";
      
          server.send(200, "text/html", html);
      });
      
      
      server.on("/saveconfig", HTTP_POST, []() {
          if (server.hasArg("threshold")) {
            Core::threshold = server.arg("threshold").toFloat();
            Core::preferences.putFloat("threshold", Core::threshold);
          }
          if (server.hasArg("measureDuration")) {
            Core::measureDuration = server.arg("measureDuration").toInt();
            Core::preferences.putUInt("measureDuration", Core::measureDuration);
          }
          if (server.hasArg("sleepDuration")) {
            Core::sleepDuration = server.arg("sleepDuration").toInt();
            Core::preferences.putUInt("sleepDuration", Core::sleepDuration);
          }
          server.send(200, "text/html", "<h2>Configuration Saved! <a href='/config'>Back</a></h2>");
      });
      
      
      server.on("/resetconfig", HTTP_POST, []() {
        Core::threshold = TempChangeThresholdDefault;
        Core::measureDuration = MeasureDurationDefault;
        Core::sleepDuration = SleepDurationDefault;
      
        Core::preferences.putFloat("threshold", Core::threshold);
        Core::preferences.putUInt("measureDuration", Core::measureDuration);
        Core::preferences.putUInt("sleepDuration", Core::sleepDuration);
      
          server.send(200, "text/plain", "Configuration reset to default values.");
      });
      
      
      server.on("/getstats", HTTP_GET, []() {
        server.send(200, "application/json", getStatistics());
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
      
            if(Core::rtcAvailable)
            {
              DateTime now = DateTime(year, month, day, hour, minute, second);
              Core::rtc.adjust(now);
              struct timeval tv = { static_cast<time_t>(now.unixtime()), 0 };
              settimeofday(&tv, nullptr);
        
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
      
        server.on("/visualize", HTTP_GET, []() {
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
                          width: 100%;
                          table-layout: fixed; /* Ensures even column widths */
                      }
                      td, th {
                          border: 1px solid #dddddd;
                          text-align: center;
                          padding: 4px;
                          width: 10px; /* More compact fixed width */
                      }
                      th.full-hour, td.full-hour {
                          border-left: 2px solid black !important; /* Only full-hour borders */
                      }
                      tr:nth-child(odd) {
                          background-color: #f2f2f2;
                      }
                      tr:nth-child(even) {
                          background-color: #ffffff;
                      }
                      .sticky-col {
                          position: sticky;
                          left: 0;
                          background: inherit;
                          z-index: 2;
                          font-weight: bold;
                          min-width: 90px; /* Wider for weekday names */
                      }
                      th {
                          position: sticky;
                          top: 0;
                          background: #e6e6e6;
                          z-index: 3;
                      }
                      td:not(.full-hour), th:not(.full-hour) {
                          border-left: none !important; /* Remove vertical borders except full hours */
                      }
                      .hour-label {
                          text-align: center;
                          font-weight: bold;
                      }
                  </style>
                  <script>
                      document.addEventListener("DOMContentLoaded", function () {
                          fetchVisualizationData();
                      });
      
                      function fetchVisualizationData() {
                          fetch('/getgrid')
                              .then(response => response.json())
                              .then(data => {
                                  const days = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
                                  let html = '<table><tr><th class="sticky-col">Day</th>';
      
                                  // Generate headers for time slots (span 4 columns per hour)
                                  for (let i = 0; i < 24; i++) {
                                      html += `<th class="full-hour hour-label" colspan="4">${i}:00</th>`;
                                  }
                                  html += '</tr>';
      
                                  // Generate usage data (readonly, only highlights yellow)
                                  for (let day = 0; day < 7; day++) {
                                      html += `<tr><td class="sticky-col">${days[day]}</td>`;
                                      for (let slot = 0; slot < 96; slot++) {
                                          let extraClass = (slot % 4 === 0) ? "full-hour" : "";
                                          let color = (data[day][slot] > 0) ? "background-color: yellow;" : "";
                                          html += `<td class="${extraClass}" style="${color}"></td>`;
                                      }
                                      html += '</tr>';
                                  }
      
                                  html += '</table>';
                                  document.getElementById("visualizationContainer").innerHTML = html;
                              })
                              .catch(error => console.error('Error fetching grid data:', error));
                      }
                  </script>
              </head>
              <body>
                  <h2>Usage Visualization</h2>
                  <div class="table-container" id="visualizationContainer">
                      <p>Loading data...</p>
                  </div>
                  <br>
                  <a href="/">Back to Home</a>
              </body>
              </html>
          )rawliteral");
      });
              

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

                    /* --- Alternating Row Colors for Readability --- */
                    tr:nth-child(odd) {
                        background-color: #f2f2f2; /* Light gray */
                    }
                    
                    tr:nth-child(even) {
                        background-color: #ffffff; /* white */
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

                    /* --- Sticky First Column (Weekdays) --- */
                    th:first-child, td:first-child {
                        position: sticky;
                        left: 0;
                        background: inherit; /* Inherit background color from the row */
                        z-index: 2;
                    }
                    /* Keep header fixed at the top */
                    th {
                        position: sticky;
                        top: 0;
                        background: #e6e6e6; /* Slightly darker background for contrast */
                        z-index: 3;
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
                dayJson += String(Core::usagePattern[day][slot]);
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
              Core::usagePattern[day][slot] = constrain(value, 0, 255);
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
        memset(Core::usagePattern, 0, sizeof(Core::usagePattern));
      
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
              function updateStatistics() {
                  fetch('/getstats')
                      .then(response => response.json())
                      .then(data => {
                          document.getElementById('weekly_percentage').innerText = data.weekly_percentage + "%";
                          document.getElementById('hit_percentage').innerText = data.hit_percentage + "%";
                          document.getElementById('miss_percentage').innerText = data.miss_percentage + "%";
                          document.getElementById('waste_percentage').innerText = data.waste_percentage + "%";
                      })
                      .catch(error => console.error('Error fetching statistics:', error));
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
              setInterval(updateStatistics, 10000);
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
              <p><strong>Pump running Weekly Percentage:</strong> <span id="weekly_percentage">Loading...</span></p>
              <p><strong>Pump Hit Percentage:</strong> <span id="hit_percentage">Loading...</span></p>
              <p><strong>Pump Miss Percentage:</strong> <span id="miss_percentage">Loading...</span></p>
              <p><strong>Wasted Pump Usage:</strong> <span id="waste_percentage">Loading...</span></p>              
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
        doc["initialAvgTemp"] = String(Core::initialAvgTemp);
        doc["currentAvgTemp"] = String(Core::currentAvgTemp);
        doc["deltaTemp"] = String(Core::deltaT);
        doc["pumpStatus"] = Core::pumpActivated ? "ON" : "OFF"; // Replace with actual logic
      
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
      });
}