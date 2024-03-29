#pragma once

#include "accumulator_v2.h"
#include "config.h"
#include <time.h>

const time_t g_safeTimestamp = 1609459200; // 01/01/2021

#ifdef HAS_MOISTURE_SENSOR
extern AccumulatorV2 g_soilMoisture;
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

extern bool g_wateringState;

extern time_t g_bootTime;
extern bool g_hasInternet;
extern unsigned g_packagesSent;

extern bool g_mqttEnabled;

extern const unsigned int g_wateringDefaultTime;
extern unsigned g_wateringCycles;

extern bool g_ledBlinkEnabled;

extern AccumulatorV2 g_pingTime;
extern unsigned g_connectionLossCount;

void
tasksSetup();

void
tasksLoop();

void
startWatering(unsigned int wateringTime = g_wateringDefaultTime);

void
mqttEnable(bool enable);