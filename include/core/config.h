#pragma once

#include <Arduino.h>

class ConfigFile
{
  public:
    // device
    String hostname;
    String timezone;

    // wifi
    String ssid;
    String wifiPassword;

    // OTA
    String otaUser;
    String otaPassword;

    // ThingSpeak
    String thingSpeakAPIKey;
    long thingSpeakChannelNumber;

    // TalkBack
    String talkBackAPIKey;
    long talkBackID;

    // MQTT
    String mqttUser;
    String mqttPassword;
    String mqttClientID;
    String mqttServer;
    int mqttPort;
    String mqttCACert;

    // pins
    uint8_t buttonPin;
    uint8_t wateringPin;
    uint8_t wateringPinOn;
    uint8_t dhtPin;
    uint8_t soilMoisturePin;
    uint8_t luminosityPin;
    uint8_t waterLevelPin;

    // log
    int logLevel;

    ConfigFile();

    bool loadFile(unsigned deviceID);
};

extern ConfigFile config;

// Backward-compatible references to config members.
// Existing code continues to use g_* directly; new code should prefer config.<field>.
extern String& g_hostname;
extern String& g_timezone;

extern String& g_ssid;
extern String& g_wifiPassword;

extern String& g_otaUser;
extern String& g_otaPassword;

extern String& g_thingSpeakAPIKey;
extern long& g_thingSpeakChannelNumber;

extern String& g_talkBackAPIKey;
extern long& g_talkBackID;

extern String& g_mqttUser;
extern String& g_mqttPassword;
extern String& g_mqttClientID;
extern String& g_mqttServer;
extern int& g_mqttPort;
extern String& g_mqttCACert;

extern uint8_t& g_buttonPin;
extern uint8_t& g_wateringPin;
extern uint8_t& g_wateringPinOn;
extern uint8_t& g_dhtPin;
extern uint8_t& g_soilMoisturePin;
extern uint8_t& g_luminosityPin;
extern uint8_t& g_waterLevelPin;

bool
loadConfigFile(unsigned deviceID);

