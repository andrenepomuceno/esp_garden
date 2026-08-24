#pragma once

// Surfaced in /data.json's Status and published to ThingsBoard as
// current_fw_version, which is how the broker decides an update landed. It is
// also what fwVersionDiffers() compares an offered package against, so bumping
// it is what makes a FOTA a no-op instead of a reflash loop.
#define FW_VERSION "2.1.0"

// Feature flags
#define USE_WEBSERVER 1
#define USE_MQTT 1
#define USE_OTA 1
#define USE_TALKBACK 1

// Nonce + SHA-256 login with roles (ported from fullbot-firmware). With this
// off the web UI falls back to the pre-2.1 behaviour, where only the static
// file handler was protected and /control, /logs and /update were open to
// anyone on the LAN.
#define USE_CUSTOM_LOGIN 1

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

// Scheduled waterings held in config.json. Compile-time because they live in a
// fixed array — no allocation in steady state.
#ifndef SCHEDULE_COUNT
#define SCHEDULE_COUNT 8
#endif

// Relay timing runs on a dedicated FreeRTOS task so a blocking background
// handler (ping, TalkBack, an MQTT drain) cannot delay switching a pump off.
#define CRITICAL_TASKS_PERIOD_MS 50
