#include "Webserverconfig.hpp"
#include "Mqtt.h"
#include "Stats.h"

WebServer server(80);

String getCurrentDateTime() {
  time_t now = time(nullptr);
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "Failed to obtain time";
  }

  char buffer[30];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d %s",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, Core::timezoneName.c_str());

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
  PumpStats::Result r = PumpStats::compute();
  StaticJsonDocument<512> doc;
  doc["covered_hours"] = serialized(String(r.coveredHours, 1));
  doc["run_hours_7d"] = serialized(String(r.runHours, 2));
  if (!isnan(r.runPercent)) doc["run_percentage_7d"] = serialized(String(r.runPercent, 1));
  doc["usage_events_7d"] = r.usageEvents;
  doc["suspect_events_7d"] = r.suspectEvents;
  if (!isnan(r.hitPercent)) doc["hit_percentage_7d"] = serialized(String(r.hitPercent, 1));
  if (!isnan(r.wastePercent)) doc["waste_percentage_7d"] = serialized(String(r.wastePercent, 1));
  doc["scheduled_percentage"] = serialized(String(PumpStats::scheduledPercent(), 1));
  String json;
  serializeJson(doc, json);
  return json;
}

void webserversetup()
{
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", R"rawliteral(
          <!DOCTYPE html>
          <html>
          <head>
            <meta charset="UTF-8">
            <title>ESP32 Circulation Pump Smart Control</title>
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <style>
              * {
                box-sizing: border-box;
                margin: 0;
                padding: 0;
              }
              body {
                font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
                background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
                min-height: 100vh;
                padding: 20px;
              }
              .container {
                max-width: 800px;
                margin: 0 auto;
              }
              h1 {
                text-align: center;
                color: white;
                font-size: 2.5em;
                margin-bottom: 30px;
                text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
              }
              .card {
                background: white;
                border-radius: 15px;
                padding: 25px;
                margin-bottom: 20px;
                box-shadow: 0 10px 30px rgba(0,0,0,0.3);
              }
              .card h2 {
                color: #667eea;
                margin-bottom: 20px;
                border-bottom: 3px solid #667eea;
                padding-bottom: 10px;
              }
              .status-grid {
                display: grid;
                grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
                gap: 15px;
                margin-bottom: 20px;
              }
              .status-item {
                background: #f8f9fa;
                padding: 15px;
                border-radius: 10px;
                border-left: 4px solid #667eea;
              }
              .status-item strong {
                display: block;
                color: #667eea;
                font-size: 0.9em;
                margin-bottom: 5px;
              }
              .status-item span {
                font-size: 1.1em;
                color: #333;
                font-weight: 600;
                word-break: break-word;
                hyphens: auto;
              }
              .pump-status-on {
                color: #28a745 !important;
                font-weight: bold;
              }
              .pump-status-off {
                color: #dc3545 !important;
                font-weight: bold;
              }
              .button-group {
                display: flex;
                gap: 15px;
                justify-content: center;
                flex-wrap: wrap;
                margin-top: 20px;
              }
              .btn {
                border: none;
                padding: 15px 35px;
                text-align: center;
                font-size: 16px;
                font-weight: 600;
                cursor: pointer;
                border-radius: 8px;
                transition: all 0.3s ease;
                box-shadow: 0 4px 6px rgba(0,0,0,0.1);
              }
              .btn:hover:not(:disabled) {
                transform: translateY(-2px);
                box-shadow: 0 6px 12px rgba(0,0,0,0.2);
              }
              .btn:active:not(:disabled) {
                transform: translateY(0);
              }
              .enable-pump-btn {
                background: linear-gradient(135deg, #28a745, #20c997);
                color: white;
              }
              .enable-pump-btn:disabled {
                background: #cccccc;
                cursor: not-allowed;
                opacity: 0.6;
              }
              .restart-btn {
                background: linear-gradient(135deg, #ff6b6b, #ee5a6f);
                color: white;
              }
              .config-links {
                display: grid;
                grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
                gap: 15px;
                list-style: none;
              }
              .config-links li {
                background: #f8f9fa;
                border-radius: 10px;
                transition: all 0.3s ease;
              }
              .config-links li:hover {
                transform: translateY(-3px);
                box-shadow: 0 5px 15px rgba(0,0,0,0.1);
              }
              .config-links a {
                display: block;
                padding: 20px;
                text-decoration: none;
                color: #667eea;
                font-weight: 600;
                text-align: center;
              }
              .loading {
                color: #999;
                font-style: italic;
              }
              @media (max-width: 600px) {
                h1 {
                  font-size: 1.8em;
                }
                .status-grid {
                  grid-template-columns: 1fr;
                }
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
                    
                    const pumpStatusEl = document.getElementById('pumpStatus');
                    pumpStatusEl.innerText = data.pumpStatus;
                    pumpStatusEl.className = data.pumpStatus === 'ON' ? 'pump-status-on' : 'pump-status-off';
                    
                    // Enable button only if pump is OFF
                    const enableBtn = document.getElementById('enablePumpBtn');
                    if (data.pumpStatus === 'OFF') {
                      enableBtn.disabled = false;
                    } else {
                      enableBtn.disabled = true;
                    }
                  })
                  .catch(error => console.error('Error fetching status:', error));
              }
              
              function enablePump() {
                fetch('/enablepump', { method: 'POST' })
                  .then(response => response.text())
                  .then(text => {
                    alert(text);
                    updateStatus();
                  })
                  .catch(error => alert('Failed to enable pump: ' + error));
              }
              
              function restartESP() {
                  if (confirm('Are you sure you want to restart the device?')) {
                      fetch('/restart', { method: 'POST' })
                          .then(response => response.text())
                          .then(text => alert(text))
                          .catch(error => alert('Restart failed: ' + error));
                  }
              }
              
              function fmt(v, unit) { return (v === undefined || v === null) ? '– (not enough data)' : v + unit; }

              function updateStats() {
                fetch('/getstats')
                  .then(response => response.json())
                  .then(data => {
                    document.getElementById('run_percentage_7d').innerText = fmt(data.run_percentage_7d, ' %');
                    document.getElementById('run_hours_7d').innerText = data.run_hours_7d + ' h';
                    document.getElementById('usage_events_7d').innerText = data.usage_events_7d;
                    document.getElementById('hit_percentage_7d').innerText = fmt(data.hit_percentage_7d, ' %');
                    document.getElementById('waste_percentage_7d').innerText = fmt(data.waste_percentage_7d, ' %');
                    document.getElementById('suspect_events_7d').innerText = data.suspect_events_7d;
                    document.getElementById('covered_hours').innerText = data.covered_hours + ' h';
                  })
                  .catch(error => console.error('Error fetching stats:', error));
              }

              setInterval(updateStatus, 3000); // Update every 3 seconds
              setInterval(updateStats, 15000); // Statistics change slowly
              window.onload = function() { updateStatus(); updateStats(); };
            </script>
          </head>
          <body>
            <div class="container">
              <h1>💧 Circulation Pump Control</h1>
              
              <div class="card">
                <h2>📊 System Status</h2>
                <div class="status-grid">
                  <div class="status-item">
                    <strong>📅 Date & Time</strong>
                    <span id="dateTime" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>⏱️ Uptime</strong>
                    <span id="uptime" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>🌡️ Previous Temp</strong>
                    <span id="initialAvgTemp" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>🌡️ Current Temp</strong>
                    <span id="currentAvgTemp" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>📉 Delta Temp</strong>
                    <span id="deltaTemp" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>🚰 Pump Status</strong>
                    <span id="pumpStatus" class="loading">Loading...</span>
                  </div>
                </div>
                
                <div class="button-group">
                  <button id="enablePumpBtn" class="btn enable-pump-btn" onclick="enablePump()" disabled>
                    🚀 Enable Pump
                  </button>
                  <button class="btn restart-btn" onclick="restartESP()">
                    🔄 Restart Device
                  </button>
                </div>
              </div>
              
              <div class="card">
                <h2>📈 Statistics (rolling 7 days, measured)</h2>
                <div class="status-grid">
                  <div class="status-item">
                    <strong>⏱️ Pump running</strong>
                    <span id="run_percentage_7d" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>🕒 Pump run time</strong>
                    <span id="run_hours_7d" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>🚿 Usage events</strong>
                    <span id="usage_events_7d" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>🎯 Pump already on at usage</strong>
                    <span id="hit_percentage_7d" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>♻️ Run time without usage</strong>
                    <span id="waste_percentage_7d" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>⚠️ Suspect detections (pump-induced?)</strong>
                    <span id="suspect_events_7d" class="loading">Loading...</span>
                  </div>
                  <div class="status-item">
                    <strong>📅 Data covers</strong>
                    <span id="covered_hours" class="loading">Loading...</span>
                  </div>
                </div>
              </div>

              <div class="card">
                <h2>⚙️ Configuration</h2>
                <ul class="config-links">
                  <li><a href="/grid">📅 Edit Usage Grid</a></li>
                  <li><a href="/config">🔧 Configuration</a></li>
                  <li><a href="/wificonfig">📶 WiFi Setup</a></li>
                  <li><a href="/timeconfig">🕐 Set Time/Date</a></li>
                  <li><a href="/mqttconfig">🏠 MQTT / Home Assistant</a></li>
                  <li><a href="/update">⬆️ OTA Update</a></li>
                </ul>
              </div>
            </div>
          </body>
          </html>
        )rawliteral");
      });
      
      server.on("/config", HTTP_GET, []() {
          String html = "<!DOCTYPE html><html><head>"
                        "<meta charset='UTF-8'>"
                        "<title>Temperature Detection Configuration</title>"
                        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                        "<style>"
                        "* { box-sizing: border-box; margin: 0; padding: 0; }"
                        "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }"
                        ".container { max-width: 600px; margin: 0 auto; }"
                        "h1 { text-align: center; color: white; font-size: 2em; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }"
                        ".card { background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 30px rgba(0,0,0,0.3); }"
                        "label { display: block; margin-top: 15px; color: #667eea; font-weight: 600; }"
                        "input { width: 100%; padding: 12px; margin-top: 5px; border: 2px solid #e0e0e0; border-radius: 8px; font-size: 16px; }"
                        "input:focus { outline: none; border-color: #667eea; }"
                        ".button-container { display: flex; gap: 15px; justify-content: center; margin-top: 25px; flex-wrap: wrap; }"
                        ".btn { border: none; padding: 12px 30px; font-size: 16px; font-weight: 600; cursor: pointer; border-radius: 8px; transition: all 0.3s ease; }"
                        ".save-btn { background: linear-gradient(135deg, #28a745, #20c997); color: white; }"
                        ".reset-button { background: linear-gradient(135deg, #ff6b6b, #ee5a6f); color: white; }"
                        ".btn:hover { transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.2); }"
                        ".back-link { display: block; text-align: center; margin-top: 20px; color: white; text-decoration: none; font-weight: 600; }"
                        ".back-link:hover { text-decoration: underline; }"
                        "</style>"
                        "<script>"
                        "function resetConfig() {"
                        " fetch('/resetconfig', { method: 'POST' }).then(() => location.reload());"
                        "}"
                        "</script>"
                        "</head><body>"
                        "<div class='container'>"
                        "<h1>Configuration</h1>"
                        "<div class='card'>"
                        "<form action='/saveconfig' method='POST'>"
                        
                        "<label for='threshold'>Temperature Drop Threshold (&deg;C):</label>"
                        "<input type='number' step='0.1' id='threshold' name='threshold' value='" + String(Core::threshold) + "' min='0' max='10'>"
      
                        "<label for='measureDuration'>Measurement Duration (ms):</label>"
                        "<input type='number' id='measureDuration' name='measureDuration' value='" + String(Core::measureDuration) + "' min='1000' max='20000'>"
      
                        "<label for='sleepDuration'>Sleep Duration (ms):</label>"
                        "<input type='number' id='sleepDuration' name='sleepDuration' value='" + String(Core::sleepDuration) + "' min='1000' max='100000'>"
      
                        "<label for='increaseOnUsage'>Usage Pattern Increase Value:</label>"
                        "<input type='number' id='increaseOnUsage' name='increaseOnUsage' value='" + String(Core::increaseOnUsage) + "' min='1' max='20'>"
      
                        "<div class='button-container'>"
                        "<input type='submit' value='Save Configuration' class='btn save-btn'>"
                        "<button type='button' class='btn reset-button' onclick='resetConfig()'>Reset to Defaults</button>"
                        "</div>"
                        "</form></div>"
                        "<a href='/' class='back-link'>Back to Home</a>"
                        "</div></body></html>";
      
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
          if (server.hasArg("increaseOnUsage")) {
            Core::increaseOnUsage = server.arg("increaseOnUsage").toInt();
            Core::preferences.putUInt("increaseOnUsage", Core::increaseOnUsage);
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
                <meta charset="UTF-8">
                <title>Set RTC Time</title>
                <meta name="viewport" content="width=device-width, initial-scale=1">
                <style>
                    * { box-sizing: border-box; margin: 0; padding: 0; }
                    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }
                    .container { max-width: 600px; margin: 0 auto; }
                    h1 { text-align: center; color: white; font-size: 2em; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }
                    .card { background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 30px rgba(0,0,0,0.3); }
                    label { display: block; margin-top: 15px; color: #667eea; font-weight: 600; }
                    input { width: 100%; padding: 12px; margin-top: 5px; border: 2px solid #e0e0e0; border-radius: 8px; font-size: 16px; }
                    input:focus { outline: none; border-color: #667eea; }
                    .button-container { display: flex; gap: 15px; justify-content: center; margin-top: 25px; flex-wrap: wrap; }
                    .btn { border: none; padding: 12px 30px; font-size: 16px; font-weight: 600; cursor: pointer; border-radius: 8px; transition: all 0.3s ease; color: white; }
                    .save-btn { background: linear-gradient(135deg, #28a745, #20c997); }
                    .browser-btn { background: linear-gradient(135deg, #667eea, #764ba2); }
                    .btn:hover { transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.2); }
                    .back-link { display: block; text-align: center; margin-top: 20px; color: white; text-decoration: none; font-weight: 600; }
                    .back-link:hover { text-decoration: underline; }
                </style>
                <script>
                    function setToBrowserTime() {
                        let now = new Date();
                        document.getElementById('year').value = now.getFullYear();
                        document.getElementById('month').value = now.getMonth() + 1;
                        document.getElementById('day').value = now.getDate();
                        document.getElementById('hour').value = now.getHours();
                        document.getElementById('minute').value = now.getMinutes();
                        document.getElementById('second').value = now.getSeconds();
                    }
                </script>
            </head>
            <body>
                <div class="container">
                    <h1>Set RTC Time</h1>
                    <div class="card">
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
                                <input type="submit" value="Set Time" class="btn save-btn">
                                <button type="button" onclick="setToBrowserTime()" class="btn browser-btn">Use Browser Time</button>
                            </div>
                        </form>
                    </div>
                    <a href="/" class="back-link">Back to Home</a>
                </div>
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
          <head>
            <meta charset="UTF-8">
            <title>WiFi Configuration</title>
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <style>
              * { box-sizing: border-box; margin: 0; padding: 0; }
              body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }
              .container { max-width: 600px; margin: 0 auto; }
              h1 { text-align: center; color: white; font-size: 2em; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }
              .card { background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 30px rgba(0,0,0,0.3); }
              label { display: block; margin-top: 15px; color: #667eea; font-weight: 600; }
              input[type="text"], input[type="password"] { width: 100%; padding: 12px; margin-top: 5px; border: 2px solid #e0e0e0; border-radius: 8px; font-size: 16px; }
              input:focus { outline: none; border-color: #667eea; }
              .button-container { display: flex; gap: 15px; justify-content: center; margin-top: 25px; flex-wrap: wrap; }
              .btn { border: none; padding: 12px 30px; font-size: 16px; font-weight: 600; cursor: pointer; border-radius: 8px; transition: all 0.3s ease; color: white; }
              .save-btn { background: linear-gradient(135deg, #28a745, #20c997); }
              .reset-btn { background: linear-gradient(135deg, #ff6b6b, #ee5a6f); }
              .btn:hover { transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.2); }
              .back-link { display: block; text-align: center; margin-top: 20px; color: white; text-decoration: none; font-weight: 600; }
              .back-link:hover { text-decoration: underline; }
            </style>
          </head>
          <body>
            <div class="container">
              <h1>WiFi Configuration</h1>
              <div class="card">
                <form action="/save" method="POST">
                  <label for="ssid">Network SSID:</label>
                  <input type="text" id="ssid" name="ssid" required>
                  <label for="password">Password:</label>
                  <input type="password" id="password" name="password" required>
                  <div class="button-container">
                    <input type="submit" value="Save WiFi Settings" class="btn save-btn">
                  </div>
                </form>
                <form action="/resetwifi" method="POST" style="margin-top: 20px;">
                  <div class="button-container">
                    <input type="submit" value="Reset to AP Mode" class="btn reset-btn">
                  </div>
                </form>
              </div>
              <a href='/' class="back-link">Back to Home</a>
            </div>
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
      
      // Handle Reset to AP Mode
      server.on("/resetwifi", HTTP_POST, []() {
        saveWiFiCredentials(String(), String());
        server.send(200, "text/html", "<h2>Device reset to AP mode! Restarting...</h2>");
        delay(2000);
        ESP.restart();
      });
      
        server.on("/restart", HTTP_POST, []() {
            server.send(200, "text/plain", "Restarting...");
            delay(1000);
            ESP.restart();
        });
      
   
      
      server.on("/update", HTTP_GET, []() {
        server.send(200, "text/html", R"rawliteral(
          <!DOCTYPE html>
          <html>
          <head>
            <meta charset="UTF-8">
            <title>Firmware Update</title>
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <style>
              * { box-sizing: border-box; margin: 0; padding: 0; }
              body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }
              .container { max-width: 600px; margin: 0 auto; }
              h1 { text-align: center; color: white; font-size: 2em; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }
              .card { background: white; border-radius: 15px; padding: 30px; box-shadow: 0 10px 30px rgba(0,0,0,0.3); text-align: center; }
              input[type="file"] {
                margin: 20px 0;
                padding: 12px 20px;
                border: 2px solid #667eea;
                border-radius: 8px;
                background: white;
                cursor: pointer;
                font-size: 14px;
                font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
                color: #333;
              }
              input[type="file"]::file-selector-button {
                border: none;
                padding: 10px 20px;
                font-size: 14px;
                font-weight: 600;
                cursor: pointer;
                border-radius: 6px;
                background: linear-gradient(135deg, #667eea, #764ba2);
                color: white;
                margin-right: 15px;
                transition: all 0.3s ease;
              }
              input[type="file"]::file-selector-button:hover {
                transform: translateY(-2px);
                box-shadow: 0 4px 8px rgba(0,0,0,0.2);
              }
              .btn { border: none; padding: 12px 30px; font-size: 16px; font-weight: 600; cursor: pointer; border-radius: 8px; transition: all 0.3s ease; color: white; background: linear-gradient(135deg, #667eea, #764ba2); }
              .btn:hover { transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.2); }
              .back-link { display: block; text-align: center; margin-top: 20px; color: white; text-decoration: none; font-weight: 600; }
              .back-link:hover { text-decoration: underline; }
            </style>
          </head>
          <body>
            <div class="container">
              <h1>Firmware Update (OTA)</h1>
              <div class="card">
                <form method="POST" action="/update" enctype="multipart/form-data">
                  <input type="file" name="firmware" accept=".bin" required><br>
                  <input type="submit" value="Upload and Update" class="btn">
                </form>
              </div>
              <a href='/' class="back-link">Back to Home</a>
            </div>
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
              <meta charset="UTF-8">
              <title>Edit Usage Grid</title>
              <meta name="viewport" content="width=device-width, initial-scale=1">
              <style>
                  * { box-sizing: border-box; margin: 0; padding: 0; }
                  body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }
                  .container { max-width: 95%; margin: 0 auto; }
                  h1 { text-align: center; color: white; font-size: 2em; margin-bottom: 20px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }
                  .table-container {
                      background: white;
                      border-radius: 15px;
                      padding: 20px;
                      box-shadow: 0 10px 30px rgba(0,0,0,0.3);
                      overflow-x: auto;
                      white-space: nowrap;
                  }
                  table {
                      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
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
                      border-left: 3px solid #667eea !important;
                  }
                  tr:nth-child(odd) {
                      background-color: #f8f9fa;
                  }
                  tr:nth-child(even) {
                      background-color: #ffffff;
                  }
                  input {
                      width: 30px;
                      text-align: center;
                      border: 1px solid #e0e0e0;
                      border-radius: 4px;
                  }
                  .button-container {
                      display: block;
                      height: 60px;
                      text-align: center;
                  }
                  .button-container span {
                      display: block;
                      height: 16px;
                      font-size: 11px;
                      line-height: 16px;
                  }
                  .column-btn {
                      font-size: 12px;
                      padding: 3px 0;
                      margin: 1px auto;
                      cursor: pointer;
                      width: 100%;
                      max-width: 30px;
                      border: none;
                      background: #667eea;
                      color: white;
                      border-radius: 3px;
                      display: block;
                      height: 18px;
                      line-height: 12px;
                  }
                  .column-btn:hover {
                      background: #5568d3;
                  }
                  .save-button-container {
                      text-align: center;
                      margin-top: 20px;
                      display: flex;
                      justify-content: center;
                      gap: 15px;
                      flex-wrap: wrap;
                  }
                  .btn {
                      font-size: 16px;
                      padding: 12px 30px;
                      border: none;
                      cursor: pointer;
                      border-radius: 8px;
                      font-weight: 600;
                      transition: all 0.3s ease;
                      color: white;
                  }
                  .save-btn {
                      background: linear-gradient(135deg, #28a745, #20c997);
                  }
                  .reset-button {
                      background: linear-gradient(135deg, #ff6b6b, #ee5a6f);
                  }
                  .btn:hover {
                      transform: translateY(-2px);
                      box-shadow: 0 6px 12px rgba(0,0,0,0.2);
                  }
                  .back-link { display: block; text-align: center; margin-top: 20px; color: white; text-decoration: none; font-weight: 600; }
                  .back-link:hover { text-decoration: underline; }
                  th:first-child, td:first-child {
                      position: sticky;
                      left: 0;
                      background: inherit;
                      z-index: 2;
                      font-weight: bold;
                  }
                  th {
                      position: sticky;
                      top: 0;
                      background: #e6e6e6;
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

                              // Generate column header with + / - buttons
                              for (let i = 0; i < 96; i++) {
                                  let timeLabel = (i % 4 === 0) ? `${Math.floor(i / 4)}:00` : '';
                                  html += `<th class="full-hour">
                                              <div class="button-container">
                                                  <button type="button" class="column-btn" onclick="changeColumn(${i}, 1)">+</button>
                                                  <button type="button" class="column-btn" onclick="changeColumn(${i}, -1)">&minus;</button>
                                                  <span>${timeLabel}</span>
                                              </div>
                                          </th>`;
                              }
                              html += '</tr>';

                              // Generate grid rows with input fields
                              for (let day = 0; day < 7; day++) {
                                  html += `<tr><td>${days[day]}</td>`;
                                  for (let slot = 0; slot < 96; slot++) {
                                      let extraClass = (slot % 4 === 0) ? "full-hour" : "";
                                      html += `<td class="${extraClass}"><input type="number" size="2" name="slot${day}_${slot}" id="slot${day}_${slot}" value="${data[day][slot]}" min="0" max="255"></td>`;
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
                          body: new URLSearchParams(formData)
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

                  function changeColumn(colIndex, delta) {
                      for (let day = 0; day < 7; day++) {
                          let input = document.getElementById(`slot${day}_${colIndex}`);
                          if (input) {
                              let newValue = parseInt(input.value) + delta;
                              input.value = Math.max(0, Math.min(255, newValue)); // Clamp between 0 and 255
                          }
                      }
                      highlightNonZeroValues();
                  }
              </script>
          </head>
          <body>
              <div class="container">
                  <h1>Edit Usage Grid</h1>
                  <div class="table-container" id="gridContainer">
                      <p>Loading grid...</p>
                  </div>
                  <div class="save-button-container">
                      <button onclick="saveGridData()" class="btn save-btn">Save Grid</button>
                      <button onclick="confirmReset()" class="btn reset-button">Reset Grid</button>
                  </div>
                  <a href="/" class="back-link">Back to Home</a>
              </div>
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
        // Redirect to home page since status is now on home
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "Redirecting to Home");
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
      
      server.on("/enablepump", HTTP_POST, []() {
        String message;
        int code = Core::enablePumpNow(message);
        server.send(code, "text/plain", message);
      });

      PumpMqtt::registerRoutes();
}