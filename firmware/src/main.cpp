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

    String id = (const char*)configJson["id"];
    char* endPtr;
    long _id = strtol(id.c_str(), &endPtr, 16);
    if (_id != deviceID) {
        logger.println("Device ID does not match for config file.");
        return false;
    }

    g_hostname = configJson["hostname"];

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

    setenv("TZ", "<-03>3", 1); // America/Sao Paulo
    tzset();

    if (!loadConfigFile(id))
        error = true;
        
    webSetup();
    tasksSetup();
    logger.dumpToFSSetup();

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
