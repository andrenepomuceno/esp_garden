#include "SPIFFS.h"
#include "logger.h"
#include "tasks.h"
#include "web.h"
#include <Arduino.h>

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
    if (id != g_deviceID) {
        logger.println("Device ID not recognized!");
        error = true;
    }

    if (!SPIFFS.begin(true)) {
        logger.println("Failed to initialize SPIFFS.");
        error = true;
    }
    logger.dumpToFSSetup();
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
