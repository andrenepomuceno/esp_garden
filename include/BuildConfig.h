#pragma once

#define FW_VERSION "2.0.0"

// Feature flags
#define USE_WEBSERVER 1
#define USE_MQTT 1
#define USE_OTA 1
#define USE_TALKBACK 1

// Watering control
#define USE_WATERING_PWM 0

// Hardware counts. Overridden per environment in platformio.ini; the defaults
// keep the single-relay / single-probe boards building unchanged.
#ifndef RELAY_COUNT
#define RELAY_COUNT 1
#endif

#ifndef MOISTURE_SENSOR_COUNT
#define MOISTURE_SENSOR_COUNT 1
#endif

// Relay timing runs on a dedicated FreeRTOS task so a blocking background
// handler (ping, TalkBack, an MQTT drain) cannot delay switching a pump off.
#define CRITICAL_TASKS_PERIOD_MS 50
