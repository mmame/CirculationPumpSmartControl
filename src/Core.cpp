#include "Core.h"

// Static Variable Definitions
Preferences Core::preferences;

float Core::threshold = TempChangeThresholdDefault;
unsigned long Core::measureDuration = MeasureDurationDefault;
unsigned long Core::sleepDuration = SleepDurationDefault;
unsigned int Core::increaseOnUsage = IncreaseOnUsageDefault;
unsigned int Core::pumpLeadMinutes = PumpLeadMinutesDefault;
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

void Core::slotAt(time_t t, int& day, int& slot) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  day = tmv.tm_wday;  // 0 = Sunday ... 6 = Saturday
  slot = (tmv.tm_hour * 4) + (tmv.tm_min / 15);
}

int Core::enablePumpNow(String& message) {
  if (pumpActivated) {
    message = "Pump is already running";
    return 400;
  }
  time_t now = time(nullptr);
  if (now && now > 100000) {
    // The pump follows the slot of (now + lead), so that slot has to be raised to start it right away.
    int currentDay, currentSlot;
    slotAt(now + (time_t)pumpLeadMinutes * 60, currentDay, currentSlot);

    // Increment usage pattern which will trigger pump activation
    usagePattern[currentDay][currentSlot] = max(1, usagePattern[currentDay][currentSlot] + 1);
    ::saveUsagePattern();  // global function (Core::saveUsagePattern is declared but not defined)

    message = "Pump enabled for current time slot";
    return 200;
  }
  message = "Time not synchronized";
  return 500;
}

float Core::weeklyPercentage() {
  const int totalSlots = 7 * 96;
  int activeSlots = 0;
  for (int day = 0; day < 7; day++) {
    for (int slot = 0; slot < 96; slot++) {
      if (usagePattern[day][slot] > 0) activeSlots++;
    }
  }
  return activeSlots * 100.0f / totalSlots;
}
