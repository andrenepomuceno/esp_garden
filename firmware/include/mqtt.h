#pragma once
#include <Arduino.h>

bool
mqttSetup();

void
mqttLoop();

bool
mqttPublish(long pubChannelID, String message);