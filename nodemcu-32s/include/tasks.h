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
extern unsigned g_packagesSent;

extern unsigned g_tsErrors;
extern time_t g_tsLastError;
extern int g_tsLastCode;

extern unsigned g_dhtReadErrors;

extern unsigned g_wateringCycles;

void
tasksSetup();

void
tasksLoop();

void
startWatering();