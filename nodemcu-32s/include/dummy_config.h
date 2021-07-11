#pragma once

#include <pgmspace.h>

#define HAS_LUMINOSITY_SENSOR
#define HAS_DHT_SENSOR

const char g_hostname[] PROGMEM = "espgarden";

const char g_ssid[] PROGMEM = "ssid";
const char g_wifiPassword[] PROGMEM = "pwd";

const char g_otaUser[] PROGMEM = "username";
const char g_otaPassword[] PROGMEM = "pwd";

const char g_thingSpeakAPIKey[] PROGMEM = "key";
const unsigned long g_thingSpeakChannelNumber = 0;

const char g_talkBackAPIKey[] PROGMEM = "key";
const unsigned g_talkBackID = 0;