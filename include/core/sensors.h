#pragma once

#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include <Arduino.h>

// Every sensor exists in every build. Whether one is FITTED is a runtime
// question answered by config.json — config.moistureCount, config.dhtFitted
// and friends — so a probe added in /devices.html works after a restart rather
// than after a rebuild.
//
// An accumulator belonging to an unfitted sensor is simply never fed, and
// getSamples() == 0 is already the contract every consumer uses to tell "no
// reading" from "read zero". That is why dropping the #ifdefs needed no new
// signalling: the not-fitted case was already representable.

extern AccumulatorV2 g_soilMoisture[MOISTURE_MAX];

// A probe's latest window mean and sample count, published by the io task and
// safe to read from ANY thread.
//
// This exists because the accumulators must never be touched from a request
// handler: ESPAsyncWebServer runs those on the async_tcp task while the io task
// is doing push_back/pop_front on the same std::list at 1 Hz, and iterating a
// list across a pop_front dereferences a freed node. /data.json avoids it by
// being rendered from the io task into a cached string; /moisture.json needs
// live values instead, so it reads this.
struct MoistureReading
{
    float average;
    uint32_t samples;
};

MoistureReading
moistureReading(unsigned index);

// What the settling test makes of this probe. A snapshot, published by the io
// task under the same spinlock as the reading itself, because the alternative
// is a request handler walking a struct the io task is writing.
struct ProbeHealthReport
{
    int verdict;      ///< ProbeVerdict
    float stepSd;     ///< spread BETWEEN consecutive readings, in ADC counts
    uint32_t samples; ///< decayed, so it is evidence in hand rather than a total
};

ProbeHealthReport probeHealthReport(unsigned index);

// "Dry" / "Humid" / "Wet" for probe `index`, or "" when that probe has no
// two-point calibration. Bands are thirds of the probe's own physical span, so
// they are comparable between probes with different gain and offset.
String
moistureState(unsigned index);

extern AccumulatorV2 g_luminosity;

// Air temperature and humidity, from whichever ambient sensor this board has —
// a DHT11 on one wire or an SHT40 on I2C. ONE pair of accumulators, and the
// same telemetry keys behind them.
//
// That is a deliberate choice and the argument is worth keeping. This repo
// refuses to give a stored key a second meaning: `moisture2` silently became a
// different pot once, and a relay index was deleted rather than renumbered. A
// better thermometer is not that. It is the same quantity in the same units,
// measured more accurately, so every existing chart keeps working and no series
// changes meaning part-way through. What DOES change is the accuracy, and that
// is discoverable rather than silent: config.ambientSensorName() reaches
// /data.json as Status."Ambient Sensor" and ThingsBoard as the `ambient_sensor`
// client attribute.
extern AccumulatorV2 g_temperature;
extern AccumulatorV2 g_airHumidity;

// Named for the ROLE, not for the part. They used to be g_dhtReadErrors /
// g_dhtTotalReads and an SHT40 feeding a counter called "dht" is a counter a
// reader has to decode. The published telemetry key is still `dhtErrorRate`,
// and that is on purpose — see telemetry.cpp.
extern unsigned g_ambientReadErrors;
extern unsigned g_ambientTotalReads;

extern AccumulatorV2 g_waterLevel;

extern AccumulatorV2 g_flowRate; // litres per minute
double flowTotalLitres();        // cumulative since boot

// Reservoir float. Reads false when no switch is fitted, which is the safe
// direction only because the interlock that consults it is off by default —
// see config.floatInterlock.
bool floatRaised();

// Accumulator window sizing and input pin setup. Called from tasksSetup().
// Only configures pins for peripherals config.json actually declares.
void
sensorsSetup();

// Body of the io task: every ADC read of the period. The caller is responsible
// for rebuilding the /data.json cache afterwards, on this same thread.
void
sensorsReadIo();

// Brings up whichever ambient sensor is declared: constructs the DHT driver on
// its configured pin, or starts the I2C bus and probes for the SHT40. Separate
// from sensorsSetup() because it must run after the blocking boot waits,
// exactly where tasksSetup() enables the ambient task. Does nothing when
// neither is declared, and that task is then left disabled.
void
sensorsSetupAmbient();

// Body of the ambient task. Reads one of the two, never both — loadFile()
// clears dhtFitted when a document declares both, so the two flags are mutually
// exclusive by the time anything here runs.
void
sensorsReadAmbient();
