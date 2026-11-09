#include "Core.h"

// Static Variable Definitions
Preferences Core::preferences;

float Core::threshold = TempChangeThresholdDefault;
unsigned long Core::measureDuration = MeasureDurationDefault;
unsigned long Core::sleepDuration = SleepDurationDefault;
unsigned int Core::increaseOnUsage = IncreaseOnUsageDefault;
bool Core::pumpActivated = false;

RTC_PCF8563 Core::rtc;
bool Core::rtcAvailable = false;

float Core::initialAvgTemp = 0;
float Core::currentAvgTemp = 0;
float Core::deltaT = 0;

// 7 days x 96 slots for 15-minute intervals
int Core::usagePattern[7][96] = {0};

long unsigned  Core::pumpRunTime = 0;
long unsigned  Core::lastPumpStart = 0;
int unsigned  Core::hotWaterUsedWithPump = 0;
int unsigned  Core::hotWaterUsedWithoutPump = 0;
int unsigned  Core::pumpRunWithoutUsage = 0;
String Core::timezoneName = "";

int Core::enablePumpNow(String& message) {
  if (pumpActivated) {
    message = "Pump is already running";
    return 400;
  }
  time_t now = time(nullptr);
  if (now && now > 100000) {
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    int currentDay = timeinfo.tm_wday;
    int currentSlot = (timeinfo.tm_hour * 4) + (timeinfo.tm_min / 15);

    // Increment usage pattern which will trigger pump activation
    usagePattern[currentDay][currentSlot] = max(1, usagePattern[currentDay][currentSlot] + 1);
    ::saveUsagePattern();  // global function (Core::saveUsagePattern is declared but not defined)

    message = "Pump enabled for current time slot";
    return 200;
  }
  message = "Time not synchronized";
  return 500;
}
