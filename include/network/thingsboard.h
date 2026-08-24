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

// True while a cloud firmware download is running. The web OTA endpoints check
// this — both drive the same single Update object, and letting a browser upload
// start mid-download corrupts whichever finishes second.
bool
tbFotaInProgress();

// Human-readable FOTA state for /data.json, empty when idle.
String
tbFotaStatus();
