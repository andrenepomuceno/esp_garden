#pragma once

#include "accumulator_v2.h"
#include "config.h"
#include <time.h>

const time_t g_safeTimestamp = 1609459200; // 01/01/2021

extern time_t g_bootTime;
extern bool g_hasInternet;
extern unsigned g_packagesSent;

extern bool g_mqttEnabled;

extern bool g_ledBlinkEnabled;

extern AccumulatorV2 g_pingTime;
extern unsigned g_connectionLossCount;

void
tasksSetup();

void
tasksLoop();

void
mqttEnable(bool enable);