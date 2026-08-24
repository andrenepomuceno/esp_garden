#pragma once
#include <Arduino.h>

bool
mqttSetup();

void
mqttLoop();

// Publishes to the ThingSpeak channel topic. Kept for the ThingSpeak backend.
bool
mqttPublish(long pubChannelID, String message);

// Publishes to an explicit topic — ThingsBoard telemetry, attributes, RPC.
bool
mqttPublishTopic(const String& topic, const String& message);

// True when the configured backend is ThingsBoard.
bool
mqttIsThingsBoard();

// Subscribes to an explicit topic, logging either outcome. Only useful once
// connected — a broker forgets every subscription when the session drops, so
// call sites live in the on-connect path.
bool
mqttSubscribeTopic(const String& topic);

// Reconnect backoff bounds. The floor is one second so a momentary drop
// recovers immediately; the ceiling keeps a permanently misconfigured broker
// from filling the log.
static const unsigned long g_mqttRetryMinMs = 1000;
static const unsigned long g_mqttRetryMaxMs = 60000;