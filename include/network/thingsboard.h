#pragma once
#include <Arduino.h>

// ThingsBoard downlink: shared/client attributes, two-way RPC and the chunked
// v2/fw firmware stream. Kept out of mqtt.cpp, which stays about the transport
// — this is a protocol with its own state machine.
//
// Everything here is inert unless config.mqttBackend == "thingsboard"; the
// ThingSpeak path never reaches it.

// Buffer PubSubClient needs for this backend, in bytes. mqttSetup() asks before
// connecting: a firmware chunk arrives as one MQTT packet, so a buffer smaller
// than the chunk size silently drops every chunk and FOTA stalls forever.
unsigned
tbRequiredBufferSize();

// Subscribe, publish client attributes and ask for the firmware shared keys.
// Called from mqttConnect() on every successful (re)connection.
void
tbOnConnect();

// Route one downlink message. Called from the PubSubClient callback, i.e. from
// inside mqttClient.loop(). Nothing here publishes directly — see tbLoop().
void
tbHandleMessage(const char* topic, const uint8_t* payload, unsigned length);

// Flushes the outbox and drives the FOTA state machine. Called from mqttLoop()
// AFTER mqttClient.loop() returns.
//
// The split matters: PubSubClient uses one buffer for both directions, and the
// `payload` handed to the callback points into it. Publishing from inside the
// callback would overwrite the packet being processed. So the callback only
// records intent and every outbound message leaves from here.
void
tbLoop();

// Queue one telemetry object for the NEXT tbLoop(), i.e. within milliseconds
// rather than at the next periodic publish.
//
// This exists because a relay that runs for five seconds is invisible to a
// payload built once a minute: the sample lands between the events. Anything
// whose duration is shorter than the publish period has to be recorded as an
// EVENT or as a sticky flag — never sampled. The history record learned that
// when the sticky mask was added; the telemetry had not.
//
// Safe from any task. It only appends to the outbox; the publish happens on
// the loop task, which is what keeps it out of the critical runner and out of
// the PubSubClient callback.
//
// A no-op on the ThingSpeak backend, which has eight numbered fields and no
// way to express an event.
void
tbPublishEvent(const String& json);

// True while a cloud firmware download is running. The web OTA endpoints check
// this — both drive the same single Update object, and letting a browser upload
// start mid-download corrupts whichever finishes second.
bool
tbFotaInProgress();

// Human-readable FOTA state for /data.json, empty when idle.
String
tbFotaStatus();
