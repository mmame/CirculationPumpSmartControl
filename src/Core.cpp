#include "Core.h"

// Static Variable Definitions
Preferences Core::preferences;

float Core::threshold = TempChangeThresholdDefault;
unsigned long Core::measureDuration = MeasureDurationDefault;
unsigned long Core::sleepDuration = SleepDurationDefault;
bool Core::pumpActivated = false;

RTC_PCF8563 Core::rtc;
bool Core::rtcAvailable = false;

float Core::initialAvgTemp = 0;
float Core::currentAvgTemp = 0;
float Core::deltaT = 0;

// 7 days x 96 slots for 15-minute intervals
int Core::usagePattern[7][96] = {0};

