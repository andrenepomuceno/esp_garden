#pragma once

#include "BuildConfig.h"
#include <Arduino.h>

#define FLOAT_TO_STRING(x) (String(x, 2))

// ThingSpeak channels have exactly 8 fields and the numbering is a permanent
// contract with the data already stored. Relays 1..N have no field — they are
// local-only.
static const unsigned g_soilMoistureField = 1;
static const unsigned g_wateringField = 2;
static const unsigned g_pingField = 3;
static const unsigned g_waterLevelField = 4;
static const unsigned g_luminosityField = 5;
static const unsigned g_temperatureField = 6;
static const unsigned g_airHumidityField = 7;
static const unsigned g_bootTimeField = 8;

extern unsigned g_packagesSent;

// Epoch of the last publish the broker ACCEPTED; 0 until one has. A counter
// that stops moving is only visible to somebody already watching it, and the
// three-year outage here was found by a person reading a channel, not by the
// device. An age is the same fact stated so it cannot be missed.
extern uint32_t g_lastPublishTime;

void
mqttAddField(int field, String val);
void
mqttAddStatus(String status);

// The configured periodic publish period, in milliseconds.
//
// g_mqttTaskPeriod is only the COMPILED fallback the task starts on; the real
// period comes from mqtt.publishSec and is applied by tasksSetup() through
// setPeriod(), exactly as the history task does. Anything that has to agree
// with the publish interval — every accumulator window, the depth of the
// publish queue — asks this, because reading the compile-time constant is how
// a channel silently ends up averaging over a different interval from the one
// it is published on.
unsigned
mqttPublishPeriodMs();

// Body of the mqtt task: assembles the period's payload and drains the publish
// queue.
//
// Carries the CONTINUOUS channels only — the ones whose answer is a level, and
// which therefore have something new to say on every tick. Step values leave
// through telemetryPublishStepChanges() instead.
void
telemetryPublish();

// Publishes the step values that moved since the last call, plus any whose
// heartbeat came due. Called at 1 Hz from the io task.
//
// WHY THIS IS NOT PART OF THE PERIODIC PAYLOAD
//
// A relay state, a reservoir contact and a reboot counter are not levels that
// need sampling; they are values that sit still and then step. Sampling them
// once per publish spends a datapoint per key per tick re-stating something the
// cloud already knows — a value re-sent unchanged carries no information — and
// it still reports the step LATE, by up to a whole period. Publishing on change
// is both quieter and faster: it rides
// tbPublishEvent()'s outbox, which tbLoop() drains every loop() iteration, so
// latency is milliseconds rather than minutes.
//
// It runs on the io task and not on the critical runner for the reason
// publishRelayEvents() does: a JSON serialiser does not belong behind a 50 ms
// deadline. The critical runner sets bits; this turns bits into messages.
//
// A no-op on the ThingSpeak backend, which has eight numbered fields and no way
// to express any of these keys.
void
telemetryPublishStepChanges();
