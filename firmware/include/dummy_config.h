#pragma once

#include <pgmspace.h>
#include <pins_arduino.h>

#define HAS_LUMINOSITY_SENSOR
#define HAS_DHT_SENSOR

const uint8_t g_buttonPin = 0;
const uint8_t g_wateringPin = 15;
const uint8_t g_dhtPin = 23;
const uint8_t g_soilMoisturePin = A0;
const uint8_t g_luminosityPin = A3;

const char g_hostname[] PROGMEM = "espgarden";

const char g_ssid[] PROGMEM = "ssid";
const char g_wifiPassword[] PROGMEM = "pwd";

const char g_otaUser[] PROGMEM = "username";
const char g_otaPassword[] PROGMEM = "pwd";

const char g_thingSpeakAPIKey[] PROGMEM = "key";
const unsigned long g_thingSpeakChannelNumber = 0;

const char g_talkBackAPIKey[] PROGMEM = "key";
const unsigned g_talkBackID = 0;