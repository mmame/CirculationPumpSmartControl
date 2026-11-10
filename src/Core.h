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
#define IncreaseOnUsageDefault 5
#define PumpLeadMinutesDefault 5   // pump runs this many minutes earlier than the learned slot (0 = off)

struct Core {
    // Preferences storage
    static Preferences preferences;

    // Configuration variables
    static float threshold;
    static unsigned long measureDuration;
    static unsigned long sleepDuration;
    static unsigned int increaseOnUsage;
    static unsigned int pumpLeadMinutes;  // 0..10, shifts the pump window earlier
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

    // Switch the pump on for the current 15 min slot (same as the web UI button).
    // Returns the HTTP status code (200 ok, 400 already running, 500 time not synced).
    static int enablePumpNow(String& message);

    // Share of the week (7 x 96 slots of 15 min) in which the learned pattern runs the pump, in percent.
    static float weeklyPercentage();

    // Weekday (0 = Sunday) and 15 min slot (0..95) of a point in time, in local time.
    // Handles day and week rollover (Saturday 23:55 + 5 min -> Sunday slot 0) and DST.
    static void slotAt(time_t t, int& day, int& slot);

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
