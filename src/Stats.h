/* @file Stats.h - measured statistics over a rolling 7 day window
 *
 * Everything here is *measured* (pump run time from the relay state, usage events from the
 * temperature detector), stored per 15 min slot in a ring buffer that survives reboots.
 * Values that cannot be determined reliably (too little data) are reported as "not available"
 * instead of being estimated.
 */
#pragma once
#include <Arduino.h>

namespace PumpStats {

// Load the ring buffer from flash. Call once in setup() after Core::preferences.begin().
void begin();

// Call every loop(): accumulates pump run time and rolls the ring over at slot boundaries.
void tick(bool pumpOn);

// Call when the temperature detector reports a hot water usage event.
// `pumpWasOn` = relay state at the moment of detection.
void onUsageDetected(bool pumpWasOn);

struct Result {
  float coveredHours;       // how much of the 7 day window is already covered by data
  float runHours;           // pump run time in the window
  float runPercent;         // run time / covered time        (NAN if covered < 1 h)
  unsigned int usageEvents; // slots with a trustworthy usage detection
  unsigned int suspectEvents;  // detections within 3 min after a pump on/off change (likely caused by the pump itself)
  float hitPercent;         // usage events while the pump was already running   (NAN if < 10 events)
  float wastePercent;       // pump run time in slots without usage             (NAN if < 1 h run time)
};

Result compute();

// Share of the 7x96 slots in which the learned pattern schedules the pump (deterministic).
float scheduledPercent();

}  // namespace PumpStats
