#include "Stats.h"
#include "Core.h"
#include <math.h>
#include <time.h>

namespace PumpStats {

static const int SLOTS = 672;               // 7 days x 96 slots of 15 minutes
static const uint32_t SLOT_SECONDS = 900;
static const unsigned long SUSPECT_WINDOW_MS = 180000UL;  // detection this soon after a pump change is suspect
static const unsigned int MIN_USAGE_EVENTS = 10;
static const float MIN_COVERED_HOURS = 1.0f;
static const float MIN_RUN_SECONDS = 3600.0f;

// Ring entry (uint16): bits 0..9 = pump run seconds in the slot (0..900),
// bit 13 = usage detected, bit 14 = pump was running at first detection (hit),
// bit 15 = first detection was suspect (right after a pump change).
static const uint16_t RUN_MASK = 0x03FF;
static const uint16_t F_USAGE = 1 << 13;
static const uint16_t F_HIT = 1 << 14;
static const uint16_t F_SUSPECT = 1 << 15;

static uint16_t ring[SLOTS];  // finalized slots, index = absSlot % SLOTS
static uint32_t headAbs = 0;  // absolute slot number (epoch / 900) of the current slot, 0 = not started
static uint32_t firstAbs = 0; // first slot with data
static unsigned long curRunMs = 0;
static uint16_t curFlags = 0;

static bool lastPump = false;
static bool havePumpChange = false;
static unsigned long lastChangeMs = 0;
static unsigned long lastTickMs = 0;

static void save() {
  Core::preferences.putBytes("stRing", ring, sizeof(ring));
  Core::preferences.putUInt("stHead", headAbs);
  Core::preferences.putUInt("stFirst", firstAbs);
}

static void clearAll(uint32_t abs) {
  memset(ring, 0, sizeof(ring));
  headAbs = abs;
  firstAbs = abs;
  curRunMs = 0;
  curFlags = 0;
}

void begin() {
  size_t n = Core::preferences.getBytes("stRing", ring, sizeof(ring));
  if (n != sizeof(ring)) {
    memset(ring, 0, sizeof(ring));
    headAbs = 0;
    firstAbs = 0;
  } else {
    headAbs = Core::preferences.getUInt("stHead", 0);
    firstAbs = Core::preferences.getUInt("stFirst", 0);
  }
  lastTickMs = millis();
}

static uint16_t currentEntry() {
  unsigned long s = curRunMs / 1000UL;
  if (s > 900) s = 900;
  return (uint16_t)(s & RUN_MASK) | curFlags;
}

static void advanceTo(uint32_t abs) {
  if (headAbs == 0) {  // first valid time after (re)start without stored data
    clearAll(abs);
    return;
  }
  if (abs < headAbs) {  // clock jumped backwards: old data is not trustworthy
    clearAll(abs);
    save();
    return;
  }
  ring[headAbs % SLOTS] = currentEntry();
  uint32_t gap = abs - headAbs - 1;  // slots without any tick (device off)
  if (gap > SLOTS) gap = SLOTS;
  for (uint32_t i = 1; i <= gap; i++) ring[(headAbs + i) % SLOTS] = 0;
  headAbs = abs;
  curRunMs = 0;
  curFlags = 0;
  save();
}

void tick(bool pumpOn) {
  unsigned long nowMs = millis();
  time_t now = time(nullptr);
  if (now < 100000) {  // time not synchronized yet
    lastTickMs = nowMs;
    lastPump = pumpOn;
    return;
  }

  uint32_t abs = (uint32_t)(now / SLOT_SECONDS);
  if (abs != headAbs) advanceTo(abs);

  if (pumpOn != lastPump) {
    lastPump = pumpOn;
    lastChangeMs = nowMs;
    havePumpChange = true;
  }
  if (pumpOn) curRunMs += (nowMs - lastTickMs);
  lastTickMs = nowMs;
}

void onUsageDetected(bool pumpWasOn) {
  if (time(nullptr) < 100000 || headAbs == 0) return;
  if (curFlags & F_USAGE) return;  // only the first detection per slot counts

  curFlags |= F_USAGE;
  if (pumpWasOn) curFlags |= F_HIT;
  if (havePumpChange && (millis() - lastChangeMs) < SUSPECT_WINDOW_MS) curFlags |= F_SUSPECT;
}

Result compute() {
  Result r;
  r.usageEvents = r.suspectEvents = 0;
  r.runHours = r.coveredHours = 0;
  r.runPercent = r.hitPercent = r.wastePercent = NAN;

  time_t now = time(nullptr);
  if (now < 100000 || headAbs == 0) return r;

  uint32_t slotsSinceStart = headAbs - firstAbs + 1;
  uint32_t covered = slotsSinceStart > (uint32_t)SLOTS ? SLOTS : slotsSinceStart;
  uint32_t secInCurrent = (uint32_t)(now % SLOT_SECONDS);
  float coveredSec = (covered - 1) * (float)SLOT_SECONDS + secInCurrent;

  float runSec = 0, wasteSec = 0;
  unsigned int usage = 0, suspect = 0, hits = 0;

  for (uint32_t i = 0; i < covered; i++) {
    uint16_t e;
    if (i == 0) {
      e = currentEntry();  // live data of the current slot
    } else {
      e = ring[(headAbs - i) % SLOTS];
    }
    uint16_t run = e & RUN_MASK;
    bool u = e & F_USAGE, h = e & F_HIT, s = e & F_SUSPECT;
    runSec += run;
    if (u) {
      if (s) suspect++;
      else {
        usage++;
        if (h) hits++;
      }
    }
    if (run > 0 && !(u && !s)) wasteSec += run;
  }

  r.coveredHours = coveredSec / 3600.0f;
  r.runHours = runSec / 3600.0f;
  r.usageEvents = usage;
  r.suspectEvents = suspect;
  if (r.coveredHours >= MIN_COVERED_HOURS && coveredSec > 0) r.runPercent = runSec * 100.0f / coveredSec;
  if (usage >= MIN_USAGE_EVENTS) r.hitPercent = hits * 100.0f / usage;
  if (runSec >= MIN_RUN_SECONDS) r.wastePercent = wasteSec * 100.0f / runSec;
  return r;
}

float scheduledPercent() {
  int active = 0;
  for (int d = 0; d < 7; d++)
    for (int s = 0; s < 96; s++)
      if (Core::usagePattern[d][s] > 0) active++;
  return active * 100.0f / SLOTS;
}

}  // namespace PumpStats
