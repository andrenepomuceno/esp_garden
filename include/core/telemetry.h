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

// Body of the mqtt task: assembles the period's payload and drains the publish
// queue.
void
telemetryPublish();
