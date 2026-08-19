#pragma once

#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include "core/config.h"
#include <time.h>

const time_t g_safeTimestamp = 1609459200; // 01/01/2021

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

// Energise relay `index` for `duration` ms. Rejects an out-of-range index, a
// zero duration and anything above g_relayMaxTime. A relay already running is
// left alone rather than retriggered.
void
startRelay(unsigned index, unsigned int duration);

bool
relayIsOn(unsigned index);

// Remaining on-time in ms, 0 when the relay is idle.
unsigned long
relayRemaining(unsigned index);

// Relay 0 is the watering relay on every board.
void
startWatering(unsigned int wateringTime = g_wateringDefaultTime);

void
mqttEnable(bool enable);

// Schedules a reboot from loop(). A request handler must not call
// ESP.restart() itself: request->send() only queues the response, and the
// async_tcp task that would flush it is the same one the handler runs on — so
// the caller always saw a connection reset and could not tell a reboot from a
// failure.
void
requestRestart();
