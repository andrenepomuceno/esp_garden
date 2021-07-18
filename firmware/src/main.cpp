#include "logger.h"
#include "tasks.h"
#include "web.h"
#include <Arduino.h>

void
setup(void)
{
    logger.println("");
    logger.println("Initializing...");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    webSetup();
    tasksSetup();
}

void
loop(void)
{
    webLoop();
    tasksLoop();
}
