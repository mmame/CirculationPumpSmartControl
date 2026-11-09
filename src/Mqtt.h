/* @file Mqtt.h - MQTT client with Home Assistant MQTT discovery */
#pragma once
#include <Arduino.h>

namespace PumpMqtt {

// Load settings from flash and prepare the client. Call once in setup().
void begin();

// Non-blocking housekeeping (reconnect with back-off, publish state). Call from loop().
void loop();

// Register /mqttconfig and /mqttsave on the web server. Call from webserversetup().
void registerRoutes();

bool isConnected();

}  // namespace PumpMqtt
