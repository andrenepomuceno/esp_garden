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

// "Dry" / "Humid" / "Wet" for probe `index`, or "" when that probe has no
// two-point calibration. Bands are thirds of the probe's own physical span, so
// they are comparable between probes with different gain and offset.
String
moistureState(unsigned index);

extern AccumulatorV2 g_luminosity;

extern AccumulatorV2 g_temperature;
extern AccumulatorV2 g_airHumidity;
extern unsigned g_dhtReadErrors;
extern unsigned g_dhtTotalReads;

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

// Constructs the DHT driver on the configured pin. Separate from
// sensorsSetup() because it must run after the blocking boot waits, exactly
// where tasksSetup() enables the dht task. Does nothing when no DHT is
// declared, and the dht task is then left disabled.
void
sensorsSetupDht();

// Body of the dht task.
void
sensorsReadDht();
