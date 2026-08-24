#pragma once

#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include "core/config.h"
#include "core/relays.h"
#include "core/sensors.h"
#include "core/telemetry.h"
#include <time.h>

const time_t g_safeTimestamp = 1609459200; // 01/01/2021

// Task periods, minted by the DECLARE_TASK macros in tasks.cpp. Declared here
// because the sensor and telemetry modules size their accumulator windows and
// their publish queue from them, so each average still covers exactly one
// publish interval.
extern const unsigned g_ioTaskPeriod;
extern const unsigned g_mqttTaskPeriod;
#ifdef HAS_DHT_SENSOR
extern const unsigned g_dhtTaskPeriod;
#endif

extern time_t g_bootTime;
extern bool g_hasInternet;

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

// Schedules a reboot from loop(). A request handler must not call
// ESP.restart() itself: request->send() only queues the response, and the
// async_tcp task that would flush it is the same one the handler runs on — so
// the caller always saw a connection reset and could not tell a reboot from a
// failure.
void
requestRestart();
