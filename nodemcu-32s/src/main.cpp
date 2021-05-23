#include "tasks.h"
#include "web.h"
#include <Arduino.h>

void
setup(void)
{
  Serial.begin(115200);
  Serial.println("");

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
