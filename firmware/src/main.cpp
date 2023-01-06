#include "SPIFFS.h"
#include "logger.h"
#include "tasks.h"
#include "web.h"
#include <Arduino.h>

static bool initialized = false;

void
setup(void)
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    logger.println("");
    logger.println("Initializing...");

    unsigned id = ESP.getEfuseMac() % 0x10000;
    logger.println("ID: " + String(id, 16));
    if (id != g_deviceID) {
        logger.println("Device ID not recognized!");
    }

    digitalWrite(LED_BUILTIN, 1);
    if (!SPIFFS.begin(true)) {
        logger.println("Failed to initialize SPIFFS.");
    }
    webSetup();
    tasksSetup();
    digitalWrite(LED_BUILTIN, 0);

    initialized = true;
}

void
loop(void)
{
    if (initialized) {
        webLoop();
        tasksLoop();
    } else {
        sleep(1);
    }
}
