#include "logger.h"
#include "tasks.h"
#include "web.h"
#include <Arduino.h>

void
setup(void)
{
    logger.println("");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    webSetup();
    tasksSetup();

    logger.println("Setup done!");
}

void
loop(void)
{
    webLoop();
    tasksLoop();
}
