#pragma once

#include <Arduino.h>
#include <pgmspace.h>
#include <pins_arduino.h>

extern String g_hostname;

extern String g_ssid;
extern String g_wifiPassword;

extern String g_otaUser;
extern String g_otaPassword;

extern String g_thingSpeakAPIKey;
extern long g_thingSpeakChannelNumber;

extern String g_talkBackAPIKey;
extern long g_talkBackID;

#if defined(ESPGARDEN1)

#define HAS_MOISTURE_SENSOR
#define HAS_LUMINOSITY_SENSOR
#define HAS_DHT_SENSOR

const uint8_t g_buttonPin = 0;
const uint8_t g_wateringPin = 15;
const uint8_t g_wateringPinOn = 0;
const uint8_t g_dhtPin = 23;
const uint8_t g_soilMoisturePin = A0;
const uint8_t g_luminosityPin = A3;

#else
#warning "Board not defined!"
#endif
