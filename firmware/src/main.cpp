#include "SPIFFS.h"
#include "logger.h"
#include "tasks.h"
#include "web.h"
#include <Arduino.h>

void
setup(void)
{
    logger.println("");
    logger.println("Initializing...");

    unsigned id = ESP.getEfuseMac() % 0x10000;
    logger.println("ID: %d" + String(id, 16));

    pinMode(LED_BUILTIN, OUTPUT);

    digitalWrite(LED_BUILTIN, 1);
    if (!SPIFFS.begin(true)) {
        logger.println("Failed to initialize SPIFFS.");
    }
    webSetup();
    tasksSetup();
    digitalWrite(LED_BUILTIN, 0);
}

void
loop(void)
{
    webLoop();
    tasksLoop();
}
