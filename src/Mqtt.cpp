#include "Mqtt.h"
#include "Core.h"
#include "Webserverconfig.hpp"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_timer.h>

// Topic layout (single device):
//   circulationpump/status        online|offline (retained, LWT)
//   circulationpump/state         JSON with all values
//   circulationpump/enable/set    payload PRESS -> same as the "Enable Pump" button
//   homeassistant/<component>/<uid>/config   discovery (retained)
#define BASE_TOPIC "circulationpump"
#define DISCOVERY_PREFIX "homeassistant"

namespace PumpMqtt {

static WiFiClient net;
static PubSubClient mqtt(net);

static bool cfgEnabled = false;
static String cfgHost;
static uint16_t cfgPort = 1883;
static String cfgUser;
static String cfgPass;

static bool reconfigure = false;
static bool triedOnce = false;
static unsigned long lastAttempt = 0;
static unsigned long lastPublish = 0;
static String macId;  // 12 hex chars, no colons

static const unsigned long RECONNECT_INTERVAL_MS = 30000;  // back-off, connect() may block ~2 s
static const unsigned long PUBLISH_INTERVAL_MS = 10000;

static String topicStatus() { return String(BASE_TOPIC) + "/status"; }
static String topicState() { return String(BASE_TOPIC) + "/state"; }
static String topicEnableSet() { return String(BASE_TOPIC) + "/enable/set"; }

// ---------------------------------------------------------------- config

static void loadConfig() {
  cfgEnabled = Core::preferences.getBool("mqEn", false);
  cfgHost = Core::preferences.getString("mqHost", "");
  cfgPort = Core::preferences.getUShort("mqPort", 1883);
  cfgUser = Core::preferences.getString("mqUser", "");
  cfgPass = Core::preferences.getString("mqPass", "");
}

// ---------------------------------------------------------------- publishing

static void publishState() {
  // Do not publish before the first temperature average exists.
  if (Core::currentAvgTemp == 0 && Core::initialAvgTemp == 0) return;

  StaticJsonDocument<512> doc;
  doc["temp"] = serialized(String(Core::currentAvgTemp, 2));
  doc["prev_temp"] = serialized(String(Core::initialAvgTemp, 2));
  doc["delta"] = serialized(String(Core::deltaT, 2));
  doc["pump"] = Core::pumpActivated ? "ON" : "OFF";
  doc["weekly_pct"] = serialized(String(Core::weeklyPercentage(), 1));
  doc["uptime"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  doc["rssi"] = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topicState().c_str(), payload.c_str(), false);
}

// Publish one discovery message. `extra` is a JSON fragment (without braces) with entity specific keys.
static void discover(const char* component, const char* objId, const char* name, const String& extra) {
  String uid = "cps_" + macId + "_" + objId;
  String topic = String(DISCOVERY_PREFIX) + "/" + component + "/" + uid + "/config";

  String p = "{";
  p += "\"name\":\"" + String(name) + "\",";
  p += "\"uniq_id\":\"" + uid + "\",";
  p += "\"has_entity_name\":true,";
  p += "\"avty_t\":\"" + topicStatus() + "\",";
  p += "\"dev\":{\"ids\":[\"cps_" + macId + "\"],\"name\":\"Zirkulationspumpe\","
       "\"mf\":\"DIY\",\"mdl\":\"CirculationPumpSmartControl\",\"cu\":\"http://" + WiFi.localIP().toString() + "/\"},";
  p += extra;
  p += "}";
  mqtt.publish(topic.c_str(), p.c_str(), true);
}

static String stateSensor(const char* field, const char* unit, const char* devClass, bool diagnostic = false) {
  String e = "\"stat_t\":\"" + topicState() + "\",\"val_tpl\":\"{{ value_json." + String(field) + " | default('None') }}\"";
  if (unit && *unit) e += ",\"unit_of_meas\":\"" + String(unit) + "\"";
  if (devClass && *devClass) e += ",\"dev_cla\":\"" + String(devClass) + "\"";
  e += ",\"stat_cla\":\"measurement\"";
  if (diagnostic) e += ",\"ent_cat\":\"diagnostic\"";
  return e;
}

static void publishDiscovery() {
  discover("sensor", "temp", "Leitungstemperatur", stateSensor("temp", "°C", "temperature"));
  discover("sensor", "delta", "Temperaturänderung", stateSensor("delta", "°C", ""));

  discover("binary_sensor", "pump", "Pumpe",
           "\"stat_t\":\"" + topicState() + "\",\"val_tpl\":\"{{ value_json.pump }}\","
           "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"running\"");

  discover("sensor", "weekly", "Laufanteil", stateSensor("weekly_pct", "%", ""));

  discover("sensor", "rssi", "WLAN-Signal", stateSensor("rssi", "dBm", "signal_strength", true));
  discover("sensor", "uptime", "Laufzeit seit Start", stateSensor("uptime", "s", "duration", true));

  discover("button", "enable", "Pumpe jetzt einschalten",
           "\"cmd_t\":\"" + topicEnableSet() + "\",\"pl_prs\":\"PRESS\"");
}

// ---------------------------------------------------------------- MQTT callbacks / connect

static void onMessage(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String body;
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];

  if (t == topicEnableSet() && body == "PRESS") {
    String msg;
    int code = Core::enablePumpNow(msg);
    Serial.printf("MQTT enable pump -> %d (%s)\n", code, msg.c_str());
    lastPublish = 0;  // publish new state soon
  }
}

static bool connectMqtt() {
  String clientId = "circulationpump-" + macId.substring(6);
  Serial.printf("MQTT: connecting to %s:%u ...\n", cfgHost.c_str(), cfgPort);
  bool ok;
  if (cfgUser.length() > 0) {
    ok = mqtt.connect(clientId.c_str(), cfgUser.c_str(), cfgPass.c_str(), topicStatus().c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), nullptr, nullptr, topicStatus().c_str(), 0, true, "offline");
  }
  if (!ok) {
    Serial.printf("MQTT: connect failed, rc=%d\n", mqtt.state());
    return false;
  }
  Serial.println("MQTT: connected");
  mqtt.publish(topicStatus().c_str(), "online", true);
  mqtt.subscribe(topicEnableSet().c_str());
  publishDiscovery();
  lastPublish = 0;
  return true;
}

// ---------------------------------------------------------------- public API

void begin() {
  macId = WiFi.macAddress();
  macId.replace(":", "");
  macId.toLowerCase();

  loadConfig();

  mqtt.setBufferSize(1100);
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMessage);
  if (cfgHost.length() > 0) mqtt.setServer(cfgHost.c_str(), cfgPort);
}

bool isConnected() { return mqtt.connected(); }

void loop() {
  if (!cfgEnabled || cfgHost.length() == 0 || WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }

  if (reconfigure) {
    reconfigure = false;
    if (mqtt.connected()) mqtt.disconnect();
    mqtt.setServer(cfgHost.c_str(), cfgPort);
    triedOnce = false;
  }

  unsigned long now = millis();

  if (!mqtt.connected()) {
    if (!triedOnce || now - lastAttempt >= RECONNECT_INTERVAL_MS) {
      triedOnce = true;
      lastAttempt = now;
      connectMqtt();
    }
    return;
  }

  mqtt.loop();

  if (lastPublish == 0 || now - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = now ? now : 1;
    publishState();
  }
}

// ---------------------------------------------------------------- web UI

static String esc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

void registerRoutes() {
  server.on("/mqttconfig", HTTP_GET, []() {
    String html =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>MQTT</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:'Segoe UI',Tahoma,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;padding:20px}"
        ".card{max-width:600px;margin:0 auto;background:#fff;border-radius:15px;padding:30px;box-shadow:0 10px 30px rgba(0,0,0,.3)}"
        "h1{color:#667eea;margin-bottom:20px}label{display:block;margin-top:15px;color:#667eea;font-weight:600}"
        "input[type=text],input[type=password],input[type=number]{width:100%;padding:12px;margin-top:5px;border:2px solid #e0e0e0;border-radius:8px;font-size:16px}"
        "button{margin-top:25px;padding:12px 25px;border:0;border-radius:8px;background:#667eea;color:#fff;font-size:16px;cursor:pointer}"
        "a{color:#667eea}.s{margin-top:15px;color:#555}</style></head><body><div class='card'>"
        "<h1>MQTT / Home Assistant</h1><form method='POST' action='/mqttsave'>"
        "<label><input type='checkbox' name='en' value='1'";
    if (cfgEnabled) html += " checked";
    html += "> MQTT aktiv</label>"
            "<label>Broker (IP oder Hostname)</label><input type='text' name='host' value='" + esc(cfgHost) + "'>"
            "<label>Port</label><input type='number' name='port' min='1' max='65535' value='" + String(cfgPort) + "'>"
            "<label>Benutzer</label><input type='text' name='user' value='" + esc(cfgUser) + "'>"
            "<label>Passwort (leer lassen = unverändert)</label><input type='password' name='pass' value=''>"
            "<button type='submit'>Speichern</button></form>"
            "<p class='s'>Status: " + String(mqtt.connected() ? "verbunden" : "nicht verbunden") + "</p>"
            "<p class='s'><a href='/'>Zurück</a></p></div></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/mqttsave", HTTP_POST, []() {
    cfgEnabled = server.hasArg("en");
    cfgHost = server.arg("host");
    cfgHost.trim();
    long port = server.arg("port").toInt();
    cfgPort = (port >= 1 && port <= 65535) ? (uint16_t)port : 1883;
    cfgUser = server.arg("user");
    if (server.arg("pass").length() > 0) cfgPass = server.arg("pass");

    Core::preferences.putBool("mqEn", cfgEnabled);
    Core::preferences.putString("mqHost", cfgHost);
    Core::preferences.putUShort("mqPort", cfgPort);
    Core::preferences.putString("mqUser", cfgUser);
    Core::preferences.putString("mqPass", cfgPass);

    reconfigure = true;
    server.send(200, "text/html", "<h2>MQTT-Einstellungen gespeichert. <a href='/mqttconfig'>Zurück</a></h2>");
  });
}

}  // namespace PumpMqtt
