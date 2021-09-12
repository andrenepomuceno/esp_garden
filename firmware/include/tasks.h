#pragma once

#include "accumulator.h"
#include "config.h"
#include <time.h>

const time_t g_safeTimestamp = 1609459200; // 01/01/2021

#ifdef HAS_MOISTURE_SENSOR
extern Accumulator g_soilMoisture;
#endif
#ifdef HAS_LUMINOSITY_SENSOR
extern Accumulator g_luminosity;
#endif
#ifdef HAS_DHT_SENSOR
extern Accumulator g_temperature;
extern Accumulator g_airHumidity;
extern unsigned g_dhtReadErrors;
#endif

extern bool g_wateringState;

extern time_t g_bootTime;
extern bool g_hasInternet;
extern unsigned g_packagesSent;

extern bool g_thingSpeakEnabled;

extern const unsigned int g_wateringDefaultTime;
extern unsigned g_wateringCycles;

void
tasksSetup();

void
tasksLoop();

void
startWatering(unsigned int wateringTime = g_wateringDefaultTime);

void
thingSpeakEnable(bool enable);