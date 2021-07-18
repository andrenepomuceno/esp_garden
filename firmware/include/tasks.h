#pragma once

#include "accumulator.h"
#include <time.h>

const time_t g_safeTimestamp = 1609459200; // 01/01/2021

extern Accumulator g_soilMoisture;
extern Accumulator g_luminosity;
extern Accumulator g_temperature;
extern Accumulator g_airHumidity;

extern bool g_wateringState;

extern time_t g_bootTime;
extern bool g_hasInternet;
extern unsigned g_packagesSent;

extern bool g_thingSpeakEnabled;

extern unsigned g_dhtReadErrors;

extern const unsigned int g_wateringDefaultTime;
extern unsigned g_wateringCycles;

void
tasksSetup();

void
tasksLoop();

void
startWatering(unsigned int wateringTime = g_wateringDefaultTime);

void thingSpeakEnable(bool enable);