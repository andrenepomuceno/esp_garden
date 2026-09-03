#pragma once

#include "core/cloud_cover.h"
#include <Arduino.h>

// The Arduino half of the cloud-cover model: the local clock, the minute
// bucket over the 1 Hz luminosity samples, and a snapshot other tasks can read.
// The decision itself is in src/cloud_cover.cpp, which is free of all of this
// so test_cloud_cover can reach it — the same split as moisture_classifier.cpp
// against moisture_model.cpp.
//
// INERT UNLESS config.cloudEnabled AND config.luminosityFitted.
//
// The default is OFF, and that is not timidity. The compiled clear-sky table is
// an upper envelope of ONE sensor's own history at ONE mounting; on any other
// board, or on this one after the sensor moves, it is a curve the hardware
// never produced and every reading below it reads as cloud. A detector that
// cries wolf on a clear day is worse than no detector, which is a lesson this
// tree already paid for in probe_health.cpp. Turning it on is a claim its
// owner makes after running scripts/cloud_fit.py on that device's own archive.

struct CloudReport
{
    int state;         ///< CloudState; CLOUD_UNKNOWN outside the daylight window
    float clearness;   ///< k for the last closed minute, negative when unknown
    float reference;   ///< the clear-sky value that k was divided by
    float variability; ///< EWMA of |dk| per minute — the transient statistic
    bool inTransient;
    /// Length and highest variability of the episode that is running, or of the
    /// one that just closed. Read by publishCloudEvents() at the moment
    /// CLOUD_EVENT_TRANSIENT_ENDED comes back, which is the only moment the
    /// duration is known and before the next episode overwrites it.
    uint16_t transientMinutes;
    float transientPeak;
    uint32_t minutes; ///< minutes folded in since boot
};

// Resets the model and logs what was compiled in. Called from tasksSetup().
void
cloudModelSetup();

// One 1 Hz sample, taken from the luminosity accumulator on the io task.
// Returns a bitmask of CloudEvent — zero on all but the ~1 tick in 60 that
// closes a minute, and on most of those too.
int
cloudModelTick();

// A snapshot, published by the io task under a spinlock. Safe from any thread,
// for the reason moistureReading() is: a request handler must never walk state
// the io task is writing.
CloudReport
cloudReport();

// Mean |dk| over the minutes since the last call, and clears the accumulator.
// Negative when no minute has closed since — which the caller renders as an
// ABSENT key, never as zero. Zero is a perfectly still sky, and the two must
// not share a value in a stored series.
float
cloudTakeVariability();
