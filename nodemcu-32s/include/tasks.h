#pragma once

#include "accumulator.h"
#include <time.h>

extern Accumulator g_soilMoisture;
extern Accumulator g_luminosity;
extern Accumulator g_temperature;
extern Accumulator g_airHumidity;

extern bool g_buttonState;
extern bool g_wateringState;

extern time_t g_bootTime;
extern bool g_hasInternet;
extern unsigned g_packagesSent;

extern bool g_thingSpeakEnabled;
extern unsigned g_tsErrors;
extern time_t g_tsLastError;
extern int g_tsLastCode;

extern unsigned g_dhtReadErrors;

extern const unsigned int g_wateringDefaultTime;
extern unsigned g_wateringCycles;
extern time_t g_lastWateringCycle;

void
tasksSetup();

void
tasksLoop();

void
startWatering(unsigned int wateringTime = g_wateringDefaultTime);

void thingSpeakEnable(bool enable);