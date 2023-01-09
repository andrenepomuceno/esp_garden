#include "config.h"
#include "logger.h"
#include <SPIFFS.h>

// device
String g_hostname = "espgarden";
String g_timezone = "<-03>3";

// wifi
String g_ssid = "undefined";
String g_wifiPassword = "undefined";

// over the air update
String g_otaUser = "admin";
String g_otaPassword = "password";

// thingspeak
String g_thingSpeakAPIKey = "undefined";
long g_thingSpeakChannelNumber = 0;

// talkback
String g_talkBackAPIKey = "undefined";
long g_talkBackID = 0;

// mqtt
String g_mqttUser = "";
String g_mqttPassword = "";
String g_mqttClientID = "";
String g_mqttServer = "mqtt3.thingspeak.com";
int g_mqttPort = 8883;
String g_mqttCACert = "/thingspeak.pem";

// pins
uint8_t g_buttonPin = 0;
uint8_t g_wateringPin = 15;
uint8_t g_wateringPinOn = 0;
uint8_t g_dhtPin = 23;
uint8_t g_soilMoisturePin = A0;
uint8_t g_luminosityPin = A3;

#include <Arduino_JSON.h>

bool
loadConfigFile(unsigned deviceID)
{
    String filename = "/config.json";
    logger.println("Loading " + filename + "...");

    if (!SPIFFS.exists(filename)) {
        logger.println("Config file " + filename + " not found.");
        return false;
    }

    File config = SPIFFS.open(filename, FILE_READ);
    if (config == false) {
        logger.println("Failed to open " + filename + ".");
        return false;
    }

    String jsonData = config.readString();
    config.close();

    JSONVar configJson = JSON.parse(jsonData);
    if (JSON.typeof(configJson) == "undefined") {
        logger.println("Failed to parse " + filename);
        return false;
    }

    String id = (const char*)configJson["id"];
    char* endPtr;
    long configID = strtol(id.c_str(), &endPtr, 16);
    if (configID != deviceID) {
        logger.println("Device ID does not match config file ID.");
        return false;
    }

    g_hostname = configJson["hostname"];
    g_timezone = configJson["timezone"];
    setenv("TZ", g_timezone.c_str(), 1);
    tzset();

    JSONVar wifi = configJson["wifi"];
    g_ssid = wifi["ssid"];
    g_wifiPassword = wifi["password"];

    JSONVar ota = configJson["ota"];
    g_otaUser = ota["username"];
    g_otaPassword = ota["password"];

    JSONVar thingSpeak = configJson["thingSpeak"];
    g_thingSpeakAPIKey = thingSpeak["apiKey"];
    g_thingSpeakChannelNumber = thingSpeak["channel"];

    JSONVar talkBack = configJson["talkBack"];
    g_talkBackAPIKey = talkBack["apiKey"];
    g_talkBackID = talkBack["channel"];

    JSONVar mqtt = configJson["mqtt"];
    g_mqttClientID = mqtt["clientID"];
    g_mqttUser = mqtt["username"];
    g_mqttPassword = mqtt["password"];
    g_mqttServer = mqtt["server"];
    g_mqttPort = mqtt["port"];
    g_mqttCACert = mqtt["cacert"];

    JSONVar io = configJson["io"];
    g_buttonPin = (int)io["button"];
    g_wateringPin = (int)io["watering"];
    g_wateringPinOn = (int)io["wateringOn"];

#if defined(HAS_DHT_SENSOR)
    g_dhtPin = (int)io["dht"];
#endif
#if defined(HAS_MOISTURE_SENSOR)
    g_soilMoisturePin = (int)io["soilMoisture"];
#endif
#if defined(HAS_LUMINOSITY_SENSOR)
    g_luminosityPin = (int)io["luminosity"];
#endif

    const int minChar = 4;
    if ((g_hostname.length() < minChar) || (g_ssid.length() < minChar) ||
        (g_wifiPassword.length() < minChar) || (g_otaUser.length() < minChar) ||
        (g_otaPassword.length() < minChar) ||
        (g_thingSpeakAPIKey.length() < minChar) ||
        (g_talkBackAPIKey.length() < minChar)) {
        logger.println("Invalid config file. Strings fields must have at least "
                       "4 characters.");
        return false;
    }

    logger.println("Loading done.");

    return true;
}
