#include "core/config.h"
#include "core/logger.h"
#include <Arduino_JSON.h>
#include <SPIFFS.h>

ConfigFile config;

// Backward-compatible references — bind g_* names to ConfigFile members.
String& g_hostname = config.hostname;
String& g_timezone = config.timezone;

String& g_ssid = config.ssid;
String& g_wifiPassword = config.wifiPassword;

String& g_otaUser = config.otaUser;
String& g_otaPassword = config.otaPassword;

String& g_thingSpeakAPIKey = config.thingSpeakAPIKey;
long& g_thingSpeakChannelNumber = config.thingSpeakChannelNumber;

String& g_talkBackAPIKey = config.talkBackAPIKey;
long& g_talkBackID = config.talkBackID;

String& g_mqttUser = config.mqttUser;
String& g_mqttPassword = config.mqttPassword;
String& g_mqttClientID = config.mqttClientID;
String& g_mqttServer = config.mqttServer;
int& g_mqttPort = config.mqttPort;
String& g_mqttCACert = config.mqttCACert;

uint8_t& g_buttonPin = config.buttonPin;
uint8_t& g_wateringPin = config.wateringPin;
uint8_t& g_wateringPinOn = config.wateringPinOn;
uint8_t& g_dhtPin = config.dhtPin;
uint8_t& g_soilMoisturePin = config.soilMoisturePin;
uint8_t& g_luminosityPin = config.luminosityPin;
uint8_t& g_waterLevelPin = config.waterLevelPin;

ConfigFile::ConfigFile()
{
    // device
    hostname = "espgarden";
    timezone = "<-03>3";

    // wifi
    ssid = "undefined";
    wifiPassword = "undefined";

    // OTA
    otaUser = "admin";
    otaPassword = "password";

    // ThingSpeak
    thingSpeakAPIKey = "undefined";
    thingSpeakChannelNumber = 0;

    // TalkBack
    talkBackAPIKey = "undefined";
    talkBackID = 0;

    // MQTT
    mqttUser = "";
    mqttPassword = "";
    mqttClientID = "";
    mqttServer = "mqtt3.thingspeak.com";
    mqttPort = 8883;
    mqttCACert = "/thingspeak.pem";

    // pins
    buttonPin = 0;
    wateringPin = 15;
    wateringPinOn = 0;
    dhtPin = 23;
    soilMoisturePin = A0;
    luminosityPin = A3;
    waterLevelPin = A6;

    // log
    logLevel = LOG_INFO;
}

bool
ConfigFile::loadFile(unsigned deviceID)
{
    String filename = "/config.json";
    logger.info("Loading " + filename + "...");

    if (!SPIFFS.exists(filename)) {
        logger.error("Config file " + filename + " not found.");
        return false;
    }

    File configFile = SPIFFS.open(filename, FILE_READ);
    if (configFile == false) {
        logger.error("Failed to open " + filename + ".");
        return false;
    }

    String jsonData = configFile.readString();
    configFile.close();

    JSONVar configJson = JSON.parse(jsonData);
    if (JSON.typeof(configJson) == "undefined") {
        logger.error("Failed to parse " + filename);
        return false;
    }

    String id = (const char*)configJson["id"];
    char* endPtr;
    long configID = strtol(id.c_str(), &endPtr, 16);
    if (configID != (long)deviceID) {
        logger.error("Device ID does not match config file ID.");
        return false;
    }

    hostname = (const char*)configJson["hostname"];
    timezone = (const char*)configJson["timezone"];
    setenv("TZ", timezone.c_str(), 1);
    tzset();

    JSONVar wifi = configJson["wifi"];
    ssid = (const char*)wifi["ssid"];
    wifiPassword = (const char*)wifi["password"];

    JSONVar ota = configJson["ota"];
    otaUser = (const char*)ota["username"];
    otaPassword = (const char*)ota["password"];

    JSONVar thingSpeak = configJson["thingSpeak"];
    thingSpeakAPIKey = (const char*)thingSpeak["apiKey"];
    thingSpeakChannelNumber = (long)thingSpeak["channel"];

    JSONVar talkBack = configJson["talkBack"];
    talkBackAPIKey = (const char*)talkBack["apiKey"];
    talkBackID = (long)talkBack["channel"];

    JSONVar mqtt = configJson["mqtt"];
    mqttClientID = (const char*)mqtt["clientID"];
    mqttUser = (const char*)mqtt["username"];
    mqttPassword = (const char*)mqtt["password"];
    mqttServer = (const char*)mqtt["server"];
    mqttPort = (int)mqtt["port"];
    mqttCACert = (const char*)mqtt["cacert"];

    JSONVar io = configJson["io"];
    buttonPin = (int)io["button"];
    wateringPin = (int)io["watering"];
    wateringPinOn = (int)io["wateringOn"];

#if defined(HAS_DHT_SENSOR)
    dhtPin = (int)io["dht"];
#endif
#if defined(HAS_MOISTURE_SENSOR)
    soilMoisturePin = (int)io["soilMoisture"];
#endif
#if defined(HAS_LUMINOSITY_SENSOR)
    luminosityPin = (int)io["luminosity"];
#endif
#if defined(HAS_WATER_LEVEL_SENSOR)
    waterLevelPin = (int)io["waterLevel"];
#endif

    const int minChar = 4;
    if ((hostname.length() < minChar) || (ssid.length() < minChar) ||
        (wifiPassword.length() < minChar) || (otaUser.length() < minChar) ||
        (otaPassword.length() < minChar) ||
        (thingSpeakAPIKey.length() < minChar) ||
        (talkBackAPIKey.length() < minChar)) {
        logger.error(
          "Invalid config file. String fields must have at least 4 characters.");
        return false;
    }

    logger.info("Loading done.");
    return true;
}

bool
loadConfigFile(unsigned deviceID)
{
    return config.loadFile(deviceID);
}

