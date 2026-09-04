#pragma once
#include "BuildConfig.h"
#include <Arduino.h>

bool
mqttSetup();

void
mqttLoop();

#if USE_THINGSPEAK
// Publishes to the ThingSpeak channel topic. Kept for the ThingSpeak backend.
bool
mqttPublish(long pubChannelID, String message);
#endif

// Publishes to an explicit topic — ThingsBoard telemetry, attributes, RPC.
bool
mqttPublishTopic(const String& topic, const String& message);

// True when the configured backend is ThingsBoard.
bool
mqttIsThingsBoard();

// True when THIS BUILD can actually serve the backend config.json selected.
//
// mqtt.backend is runtime configuration and the set of backends compiled in is
// a build decision, so the two can disagree — a document saying "thingspeak"
// loaded by an image built with USE_THINGSPEAK 0. That disagreement must not be
// survivable in silence: publishing nothing while /data.json reports
// "MQTT: enabled" is the exact shape of the 2023-2026 outage on this device,
// where the dashboard said everything was fine for three years and only
// `Packages Sent` staying at 0 gave it away.
//
// So the mismatch is REFUSED, not worked around, and it is refused in three
// places at once because each covers a different reader:
//
//   - mqttSetup() logs FATAL and returns false, for whoever is on serial;
//   - mqttLoop() never attempts a connection, so the device does not sit on a
//     broker it has no payload for;
//   - /data.json's `MQTT Link` names the mismatch for as long as it lasts,
//     which is the one that matters — the log is an 8 KB rolling buffer this
//     device overwrites within hours, so a boot-time line is gone by the time
//     anybody wonders why the channel is empty.
//
// NOT a fallback to the other backend. The credentials, server, port and CA in
// the document are the ones for the backend that was chosen; pointing a
// ThingsBoard payload at mqtt3.thingspeak.com would invent a destination the
// operator never configured and could report success for it.
//
// NOT a refusal at load time either. ConfigFile::loadFile() returning false
// means compiled defaults, ssid "undefined", and a board that cannot associate
// or be reached to fix — and the COMPILED default of mqtt.backend is
// "thingspeak", so a document that merely omits the key would brick every
// device this firmware is flashed to. Refusing to publish leaves WiFi, the web
// server and the relays up, which is the door the config is fixed through.
bool
mqttBackendSupported();

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