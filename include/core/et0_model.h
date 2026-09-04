#pragma once

#include "core/evapotranspiration.h"
#include <Arduino.h>

// The Arduino half of the reference-evapotranspiration estimate: the local
// clock, the day rollover, and a snapshot other tasks can read. The arithmetic
// is in src/evapotranspiration.cpp, which is free of all of this so
// test_evapotranspiration can reach it - the same split as cloud_cover.cpp
// against cloud_model.cpp.
//
// INERT UNLESS config.et0Enabled AND config.dhtFitted.
//
// DEFAULT OFF, and for a sharper reason than the cloud model's. That one ships
// off because its fitted table describes one sensor at one mounting. This one
// ships off because on the board it was written against it was measured to
// have NO demonstrated skill: the thermometer follows about a third of the
// outdoor temperature swing, so the diurnal range Hargreaves-Samani runs on
// describes the sensor's enclosure rather than the sky. Enabling it is a claim
// its owner makes after running scripts/et0_fit.py against that device's own
// archive and seeing the estimate beat a constant - which, on this one, it did
// not.

struct Et0Report
{
    bool valid;    ///< a complete day has been computed since boot
    double et0Mm;  ///< the last complete day's ET0, after config.et0Scale
    float tMin;    ///< ...and the extremes it was computed from, so the
    float tMax;    ///<    archive records the evidence beside the number
    float rangeK;
    int16_t dayOfYear;
    uint8_t hoursCovered;  ///< of the last CLOSED day, complete or refused
    uint8_t hoursSoFar;    ///< of the day currently in progress
    uint32_t daysComputed; ///< complete days since boot
    uint32_t daysRefused;  ///< days that closed without enough coverage
};

// Resets the day in progress and logs what was configured. Called from
// tasksSetup().
void
et0ModelSetup();

// One 1 Hz sample, taken from the DHT accumulator on the io task. Returns true
// exactly on the tick that completes a day with enough coverage to report -
// which is at most once every 24 hours, and never before the clock is set.
bool
et0ModelTick();

// A snapshot, published by the io task under a spinlock. Safe from any thread,
// for the reason cloudReport() and moistureReading() are: a request handler
// must never walk state the io task is writing.
Et0Report
et0Report();
