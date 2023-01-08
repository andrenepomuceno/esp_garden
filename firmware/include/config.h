#pragma once

#include <Arduino.h>
#include <pgmspace.h>
#include <pins_arduino.h>

extern String g_hostname;
extern String g_timezone;

extern String g_ssid;
extern String g_wifiPassword;

extern String g_otaUser;
extern String g_otaPassword;

extern String g_thingSpeakAPIKey;
extern long g_thingSpeakChannelNumber;

extern String g_talkBackAPIKey;
extern long g_talkBackID;

extern uint8_t g_buttonPin;
extern uint8_t g_wateringPin;
extern uint8_t g_wateringPinOn;
extern uint8_t g_dhtPin;
extern uint8_t g_soilMoisturePin;
extern uint8_t g_luminosityPin;

bool
loadConfigFile(unsigned deviceID);
