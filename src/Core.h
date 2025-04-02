#ifndef _CORE_H_
#define _CORE_H_

#include "Arduino.h"
#include <WebServer.h>
#include <Preferences.h>
#include <RTClib.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#define MeasureDurationDefault 5000
#define SleepDurationDefault 25000
#define TempChangeThresholdDefault 2.0

struct Core {
    // Preferences storage
    static Preferences preferences;

    // Configuration variables
    static float threshold;
    static unsigned long measureDuration;
    static unsigned long sleepDuration;
    static bool pumpActivated;

    // RTC variables
    static RTC_PCF8563 rtc;
    static bool rtcAvailable;

    static float initialAvgTemp;
    static float currentAvgTemp;
    static float deltaT;

    // Usage pattern (7 days x 96 slots for 15-minute intervals)
    static int usagePattern[7][96];

    // Statistical counters
    static unsigned long pumpRunTime; // Time in seconds
    static unsigned long lastPumpStart;
    static unsigned int hotWaterUsedWithPump;
    static unsigned int hotWaterUsedWithoutPump;
    static unsigned int pumpRunWithoutUsage;

    static String timezoneName;

    // Functions
    static void begin();
    static void initRTC();
    static DateTime getRTCTime();
    static void saveWiFiCredentials(const String& newSSID, const String& newPassword);
    static void saveUsagePattern();
};

void saveUsagePattern();
void saveWiFiCredentials(const String& newSSID, const String& newPassword);

#endif // _CORE_H_
