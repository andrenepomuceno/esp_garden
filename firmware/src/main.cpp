#include "SPIFFS.h"
#include "logger.h"
#include "tasks.h"
#include "web.h"
#include <Arduino.h>
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

    if ((g_hostname.length() < 4) || (g_ssid.length() < 4) ||
        (g_wifiPassword.length() < 4) || (g_otaUser.length() < 4) ||
        (g_otaPassword.length() < 4) || (g_thingSpeakAPIKey.length() < 4) ||
        (g_talkBackAPIKey.length() < 4)) {
        logger.println("Invalid config file. Strings fields must have at least "
                       "4 characters.");
        return false;
    }

    logger.println("Loading done.");

    return true;
}

void
setup(void)
{
    bool error = false;

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    logger.println("");
    logger.println("Initializing...");

    digitalWrite(LED_BUILTIN, 1);

    unsigned id = ESP.getEfuseMac() % 0x10000;
    logger.println("ID: " + String(id, 16));

    if (!SPIFFS.begin(true)) {
        logger.println("Failed to initialize SPIFFS.");
        error = true;
    }

    if (!loadConfigFile(id))
        error = true;

    setenv("TZ", g_timezone.c_str(), 1);
    tzset();

    logger.backupSetup();
    webSetup();
    tasksSetup();

    digitalWrite(LED_BUILTIN, 0);

    if (error) {
        g_ledBlinkEnabled = true;
    }
}

void
loop(void)
{
    webLoop();
    tasksLoop();
}
