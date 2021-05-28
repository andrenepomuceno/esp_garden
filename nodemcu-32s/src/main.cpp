#include "tasks.h"
#include "web.h"
#include <Arduino.h>

void
setup(void)
{
    Serial.begin(115200);
    Serial.println("");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 0);

    webSetup();
    tasksSetup();

    Serial.println("Setup done!");
}

void
loop(void)
{
    webLoop();
    tasksLoop();
}
