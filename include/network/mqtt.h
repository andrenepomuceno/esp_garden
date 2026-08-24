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