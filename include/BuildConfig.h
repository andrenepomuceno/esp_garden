#pragma once

// Surfaced in /data.json's Status and published to ThingsBoard as
// current_fw_version, which is how the broker decides an update landed. It is
// also what fwVersionDiffers() compares an offered package against, so bumping
// it is what makes a FOTA a no-op instead of a reflash loop.
#define FW_VERSION "2.12.0"

// Feature flags
#define USE_WEBSERVER 1
#define USE_MQTT 1
#define USE_OTA 1

// The ThingSpeak uplink: the eight numbered fields, the `field1=..&` payload
// accumulator, and publishing to `channels/<id>/publish`. OFF, because this
// garden publishes to ThingsBoard and nothing in production reaches the
// ThingSpeak path any more.
//
// Turning it off does NOT erase channel 1348790 or the field numbering that is
// a permanent contract with what is already stored there — see
// include/core/telemetry.h, which keeps that contract documented in one place
// whatever this flag says. It stops WRITING to the channel; the history stays.
//
// mqtt.backend is still parsed and still accepts "thingspeak", because a
// field device's config document must keep loading across a firmware update.
// What changes is that a build with this off REFUSES to serve that backend
// rather than publishing nothing quietly: mqttBackendSupported() is false,
// mqttSetup() logs FATAL, no connection is attempted, and /data.json's
// `MQTT Link` says so for as long as the misconfiguration lasts.
#define USE_THINGSPEAK 0

// ThingSpeak TalkBack: a 1-minute poll of the command queue, parsed into
// startRelay()/startWatering(). OFF, and not only for the flash.
//
// talkback.cpp speaks plain HTTP/1.1 on port 80 with the API key in the
// REQUEST BODY, and carried a standing `// TODO use HTTPS`. Every poll put a
// ThingSpeak write key on the wire in clear text, once a minute, for ever.
// ThingsBoard RPC covers the same ground over the MQTT/TLS link that is
// already up, so switching this off removes a credential leak rather than
// trading a feature away for space.
//
// This flag EXISTED before and was honoured at exactly zero call sites: the
// task was registered, the socket was opened and the key was sent whatever it
// was set to. A flag that is only read where somebody remembered to read it is
// worse than no flag, because it reads as "the feature is off".
#define USE_TALKBACK 0

// Nonce + SHA-256 login with roles (ported from fullbot-firmware). With this
// off the web UI falls back to the pre-2.1 behaviour, where only the static
// file handler was protected and /control, /logs and /update were open to
// anyone on the LAN.
#define USE_CUSTOM_LOGIN 1

// Watering control
#define USE_WATERING_PWM 0

// Hardware CAPACITY, not hardware presence. These size fixed arrays; how many
// peripherals are actually fitted, and on which pins, comes from config.json at
// runtime — so adding a probe or a relay is an edit in the web UI, not a
// rebuild and a serial cable.
//
// Every driver is compiled into every build. That was measured before it was
// chosen: the minimal board came to 1.166 MB and the fully populated one to
// 1.187 MB, so the whole HAS_* split was buying 21 KB out of a 1.69 MB slot.
// The cost of the flags — 75 #ifdef sites, five build shapes to keep in step,
// and a sensor you could not add without a toolchain — was never worth that.
//
// What stays compile-time is the set of KINDS. A DHT needs the DHT driver
// linked in; no web page can add a kind the firmware has no code for.
#ifndef RELAY_MAX
#define RELAY_MAX 8
#endif

// Must equal IO_HISTORY_MAX_MOISTURE: the history record has a fixed positional
// layout, and a probe with no slot in it would be charted from nothing. There
// is a static_assert on this in io_history.h.
#ifndef MOISTURE_MAX
#define MOISTURE_MAX 4
#endif

// Scheduled waterings held in config.json. Compile-time because they live in a
// fixed array — no allocation in steady state.
#ifndef SCHEDULE_COUNT
#define SCHEDULE_COUNT 8
#endif

// Relay timing runs on a dedicated FreeRTOS task so a blocking background
// handler (ping, TalkBack, an MQTT drain) cannot delay switching a pump off.
#define CRITICAL_TASKS_PERIOD_MS 50
