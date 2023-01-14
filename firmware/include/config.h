#pragma once

#include <Arduino.h>

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

extern String g_mqttUser;
extern String g_mqttPassword;
extern String g_mqttClientID;
extern String g_mqttServer;
extern int g_mqttPort;
extern String g_mqttCACert;

bool
loadConfigFile(unsigned deviceID);
