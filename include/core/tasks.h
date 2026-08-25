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
extern const unsigned g_dhtTaskPeriod;

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

// Why the last boot happened, as a readable string. A panic, a watchdog and a
// power cut are three different investigations and the enum alone hides that.
const char*
resetReasonName();

// Boot-loop interlock for the history subsystem.
//
// The history writer has already reboot-looped this board once: under LittleFS
// the old in-place ring divided by zero inside the allocator on its first
// append after every boot, so the device panicked every 75 s. Recovering meant
// disabling it through POST /config.json — which needs the device to stay up
// long enough to answer, and needs someone watching.
//
// A config key is the wrong place for that guard, because the change only
// takes effect at the next boot: turning history back on and being wrong about
// it leaves a device that panics before it can be told otherwise, and with no
// USB attached that is unrecoverable. So the count lives in RTC memory, which
// survives a panic reset, and the firmware refuses to start the writer after
// three consecutive panics.
//
// `historyGuardTripped()` is asked once at boot. `historyGuardClear()` is
// called by the history task after it has been running long enough to prove
// this firmware and this config are stable.
bool
historyGuardTripped();

void
historyGuardClear();
