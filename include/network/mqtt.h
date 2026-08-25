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

// Whether the broker link is UP, as opposed to whether publishing is switched
// on. The two were never distinguished, and that is what let this channel
// receive nothing for three years while /data.json reported "MQTT: enabled":
// the flag it printed was the operator's intent, not the device's reality.
bool
mqttIsConnected();

// PubSubClient's state code, so a refusal names itself. -2 is a failed TCP or
// TLS connect (a stale CA pin lands here), -4 a timeout, 4 bad credentials,
// 5 not authorised — each a different investigation.
int
mqttState();

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