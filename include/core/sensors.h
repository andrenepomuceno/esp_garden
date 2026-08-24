#pragma once

#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include <Arduino.h>

#ifdef HAS_MOISTURE_SENSOR
extern AccumulatorV2 g_soilMoisture[MOISTURE_SENSOR_COUNT];

// "Dry" / "Humid" / "Wet" for probe `index`, or "" when that probe has no
// two-point calibration. Bands are thirds of the probe's own physical span, so
// they are comparable between probes with different gain and offset.
String
moistureState(unsigned index);
#endif

#ifdef HAS_LUMINOSITY_SENSOR
extern AccumulatorV2 g_luminosity;
#endif

#ifdef HAS_DHT_SENSOR
extern AccumulatorV2 g_temperature;
extern AccumulatorV2 g_airHumidity;
extern unsigned g_dhtReadErrors;
extern unsigned g_dhtTotalReads;
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
extern AccumulatorV2 g_waterLevel;
#endif

#ifdef HAS_FLOW_SENSOR
extern AccumulatorV2 g_flowRate;   // litres per minute
double flowTotalLitres();          // cumulative since boot
#endif

#ifdef HAS_FLOAT_SWITCH
bool floatRaised();
#endif

// Accumulator window sizing and input pin setup. Called from tasksSetup().
void
sensorsSetup();

// Body of the io task: every ADC read of the period. The caller is responsible
// for rebuilding the /data.json cache afterwards, on this same thread.
void
sensorsReadIo();

#ifdef HAS_DHT_SENSOR
// Constructs the DHT driver on the configured pin. Separate from
// sensorsSetup() because it must run after the blocking boot waits, exactly
// where tasksSetup() enables the dht task.
void
sensorsSetupDht();

// Body of the dht task.
void
sensorsReadDht();
#endif
