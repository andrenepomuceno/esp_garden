#include "core/config.h"
#include "core/config_io.h"
#include "core/default_pins.h"
#include "core/io_history.h"
#include "core/tasks.h"
#include "core/logger.h"
#include <Arduino_JSON.h>
#include "core/filesystem.h"

ConfigFile config;

// Backward-compatible references — bind g_* names to ConfigFile members.
String& g_hostname = config.hostname;
String& g_timezone = config.timezone;

String& g_ssid = config.ssid;
String& g_wifiPassword = config.wifiPassword;

String& g_otaUser = config.otaUser;
String& g_otaPassword = config.otaPassword;

String& g_thingSpeakAPIKey = config.thingSpeakAPIKey;
long& g_thingSpeakChannelNumber = config.thingSpeakChannelNumber;

String& g_talkBackAPIKey = config.talkBackAPIKey;
long& g_talkBackID = config.talkBackID;

String& g_mqttUser = config.mqttUser;
String& g_mqttPassword = config.mqttPassword;
String& g_mqttClientID = config.mqttClientID;
String& g_mqttServer = config.mqttServer;
int& g_mqttPort = config.mqttPort;
String& g_mqttCACert = config.mqttCACert;

// The compiled pin defaults, per MCU family. The table itself lives in
// core/default_pins.h — Arduino-free, both families always compiled — so
// test_pin_rules can hold every number against the rules of the chip it
// belongs to. This file is only the seam that picks one.
//
// Selected the same way core/pin_rules.h is: CONFIG_IDF_TARGET_*, set by the
// framework from the env's `board` line, so it cannot drift. An unrecognised
// target is a hard error and not a fallback, for the reason config_pins.cpp
// gives: guessing wrong here parks the wrong GPIOs 1.5 s before anything can
// say so.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
namespace defaults = default_pins::esp32s3;
#elif defined(CONFIG_IDF_TARGET_ESP32)
namespace defaults = default_pins::wroom32;
#else
#error "Unknown CONFIG_IDF_TARGET: add a default_pins namespace and tests."
#endif

// The two spellings of "no pin" must agree. default_pins.h cannot include
// config.h without dragging Arduino into the host test, so this is the joint.
static_assert(default_pins::kNoPin == ConfigFile::kNoPin,
              "default_pins::kNoPin and ConfigFile::kNoPin disagree");

// Every probe slot gets a pin somebody chose. It used to fall through to the
// Arduino alias A0 for slots past the table, which is GPIO 36 on a WROOM-32
// and GPIO 1 on an S3 — probe 0's pin on both, so a fourth declared probe
// silently doubled the first one. Raising MOISTURE_MAX now means naming the
// pins, which is the point.
static_assert(sizeof(default_pins::wroom32::soilMoisture) >= MOISTURE_MAX,
              "the WROOM-32 soil moisture defaults do not cover MOISTURE_MAX");
static_assert(sizeof(default_pins::esp32s3::soilMoisture) >= MOISTURE_MAX,
              "the ESP32-S3 soil moisture defaults do not cover MOISTURE_MAX");

ConfigFile::ConfigFile()
{
    // device
    deviceId = 0;
    hostname = "espgarden";
    timezone = "<-03>3";
    postalCode = "";

    // wifi
    ssid = "undefined";
    wifiPassword = "undefined";

    // OTA
    otaUser = "admin";
    otaPassword = "password";

    // ThingSpeak
    thingSpeakAPIKey = "undefined";
    thingSpeakChannelNumber = 0;
    // 0 keeps the second probe off ThingSpeak. See telemetry.cpp for why this
    // is per-device configuration and not a build flag.
    thingSpeakMoisture2Field = 0;

    // TalkBack
    talkBackAPIKey = "undefined";
    talkBackID = 0;

    // MQTT
    mqttUser = "";
    mqttPassword = "";
    mqttClientID = "";
    mqttServer = "mqtt3.thingspeak.com";
    mqttPort = 8883;
    mqttCACert = "/thingspeak.pem";
    mqttBackend = "thingspeak";
    mqttUseTLS = true;
    mqttRpc = true;
    mqttFwUpdate = true;
    mqttFwTitle = "esp-garden";
    // Five minutes, not one. Step values no longer wait for this tick at all,
    // so the only thing it sets is how finely the continuous sensor channels
    // are sampled — and soil moisture, air temperature and daylight do not move
    // fast enough for a minute to say five times as much as five minutes does.
    // Anything that DOES move faster than the tick is an event, and events go
    // out the moment they happen.
    mqttPublishSec = 300;
    mqttHeartbeatSec = 900;
    cloudEnabled = false;
    et0Enabled = false;
    et0Latitude = 0.0f;
    et0Scale = 1.0f;

    // pins
    buttonPin = 0;

    // Nothing is fitted until config.json says so. This has to be explicit:
    // the singleton is only zero here by accident of static initialisation,
    // and a ConfigFile on the stack would start with an indeterminate
    // moistureCount and loop analogRead over garbage pins.
    //
    // relayCount is the deliberate exception below — a device whose config
    // failed to load still has to park its relays, and parking a pin that is
    // not a relay is harmless while leaving one floating is not.
    moistureCount = 0;
    dhtFitted = false;
    luminosityFitted = false;
    waterLevelFitted = false;
    flowFitted = false;
    floatFitted = false;

    // The pre-config defaults. relayPinsSafeInit() runs on these as the FIRST
    // statement of setup(), long before config.json is read, and a floating
    // pin on an active-low board reads as "energise" — so every slot that has
    // a known pin has to be parked then, not after the load.
    //
    // A relay on a pin outside this table is only parked once the config is
    // read, roughly a second and a half later. That window is the price of
    // making the count configurable; keeping the common pins in this table is
    // what keeps it from mattering in practice.
    {
        const unsigned fitted = sizeof(defaults::relay);
        relayCount = fitted;
        for (unsigned i = 0; i < RELAY_MAX; ++i) {
            relayPin[i] = (i < fitted) ? defaults::relay[i] : kNoPin;
            relayPinOn[i] = 0; // active low
            relayName[i] = (i == 0) ? "Watering" : ("Relay " + String(i + 1));
        }
    }

    dhtPin = defaults::dht;

    for (unsigned i = 0; i < MOISTURE_MAX; ++i) {
        // Every slot, straight out of the table — the static_assert above is
        // what makes that safe. There used to be an `A0` fallback for slots
        // past the table's end, and A0 is probe 0's own pin on both families.
        soilMoisturePin[i] = defaults::soilMoisture[i];

        // Uncalibrated until measured: dry == wet means "do not classify",
        // which is honest rather than inventing a band from nothing.
        moistureDry[i] = 0.0;
        moistureWet[i] = 0.0;
        // True preserves what every existing board does: the capacitive v2
        // modules these were built around read LOWER as the soil wets.
        moistureInvert[i] = true;
        moistureKind[i] = "";
        soilMoisturePowerPin[i] = kNoPin;
        soilMoisturePowerOn[i] = 1;
        soilMoistureSettleMs[i] = 10;
        // One pump per zone is the common layout, so probe i defaults to
        // relay i. validated against relayCount at load.
        moistureRelay[i] = (int8_t)i;

        // Suffixed by default. The unsuffixed single-probe label is applied
        // in loadSoilMoisture(), once the fitted count is known — MOISTURE_MAX
        // is capacity and is always 4, so testing it here named every probe
        // "Soil Moisture 1".."Soil Moisture 4" on a one-probe board and broke
        // the /data.json key that dashboards had been reading for years.
        soilMoistureName[i] = "Soil Moisture " + String(i + 1);
    }

    dhtName = "";
    luminosityName = "Luminosity";
    waterLevelName = "Water Level";

    // From the family table, not from the Arduino aliases A3/A6 these used to
    // carry. An alias resolves per board VARIANT, which looks chip-relative and
    // is not the same question: on the esp32s3 variant A3 is GPIO 4, which is
    // this firmware's third soil probe, while the carrier's LDR is on GPIO 6.
    luminosityPin = defaults::luminosity;
    waterLevelPin = defaults::waterLevel;

    // Both need an internal pull-up and both have to stay off the flash pins —
    // which is the whole reason these are per-family: the WROOM-32 values 27
    // and 26 are SPI_HD and SPI_CS1 on an S3.
    flowPin = defaults::flow;
    flowName = "Flow";
    flowPulsesPerLitre = 450.0; // YF-S201 nominal
    floatPin = defaults::floatSwitch;
    floatName = "Float Switch";
    floatActiveLevel = 0; // normally-open to ground, with the pull-up
    floatInterlock = false;
    floatFillRelay = -1;

    scheduleCount = 0;
    for (unsigned i = 0; i < SCHEDULE_COUNT; ++i) {
        schedules[i].enabled = false;
        schedules[i].relay = 0;
        schedules[i].hour = 0;
        schedules[i].minute = 0;
        schedules[i].days = 0;
        schedules[i].durationMs = 0;
        schedules[i].name = "";
    }

    // log
    logLevel = LOG_INFO;

    // ~24 h at one record per minute, 57.6 KB of the 512 KB FILESYSTEM.
    historyRecords = 1440;
    historyPeriodSec = 60;
}

// The eight ThingSpeak fields are a permanent contract with the data already
// stored in the channel. thingSpeak.moisture2Field exists so a device can put
// probe 2 in a free slot — but nothing stopped it naming a slot another fitted
// sensor publishes, and the payload would then carry the field twice: one wins
// silently and the loser's history is overwritten with the winner's units.
void
ConfigFile::validateThingSpeakFields()
{
    if (thingSpeakMoisture2Field == 0) {
        return; // probe 2 stays off the channel
    }

    // The field numbering is a real contract with what is stored in the
    // channel whatever USE_THINGSPEAK says, so the conflict check below runs
    // in both builds and a colliding field is normalised to 0 either way. The
    // flag decides only what is SAID at the end: an image with no ThingSpeak
    // publisher must not print "probe 2 publishes to field N", because that is
    // a boot line which is simply untrue.
    //
    // This guard used to sit here, above the loop, and returned — so with the
    // flag off the check the comment describes did not happen at all.
    struct Claim
    {
        int field;
        bool fitted;
        const char* owner;
    };

    const Claim claims[] = {
        { 1, moistureCount > 0, "moisture1" },
        { 2, relayCount > 0, "watering duration" },
        { 3, true, "ping" },
        { 4, waterLevelFitted, "water level" },
        { 5, luminosityFitted, "luminosity" },
        { 6, dhtFitted, "temperature" },
        { 7, dhtFitted, "air humidity" },
        { 8, true, "boot time" },
    };

    for (const Claim& claim : claims) {
        if (claim.field == thingSpeakMoisture2Field && claim.fitted) {
            logger.error("thingSpeak.moisture2Field " +
                         String(thingSpeakMoisture2Field) + " is already " +
                         String(claim.owner) +
                         " on this device. Probe 2 stays off the channel "
                         "rather than overwriting it.");
            thingSpeakMoisture2Field = 0;
            return;
        }
    }

#if USE_THINGSPEAK
    logger.info("Probe 2 publishes to ThingSpeak field " +
                String(thingSpeakMoisture2Field));
#else
    // Only reached when somebody actually set the key: at the default of 0
    // this function returned at its first statement.
    logger.warning("thingSpeak.moisture2Field is set to " +
                   String(thingSpeakMoisture2Field) +
                   " but ThingSpeak is not compiled into this build; the "
                   "setting is inert and probe 2 publishes nowhere.");
#endif
}

bool
ConfigFile::loadFile(unsigned deviceID)
{
    String filename = "/config.json";
    logger.info("Loading " + filename + "...");

    if (!FILESYSTEM.exists(filename)) {
        logger.error("Config file " + filename + " not found.");
        return false;
    }

    File configFile = FILESYSTEM.open(filename, FILE_READ);
    if (configFile == false) {
        logger.error("Failed to open " + filename + ".");
        return false;
    }

    String jsonData = configFile.readString();
    configFile.close();

    JSONVar configJson = JSON.parse(jsonData);
    if (JSON.typeof(configJson) == "undefined") {
        logger.error("Failed to parse " + filename);
        return false;
    }

    String id = (const char*)configJson["id"];
    char* endPtr;
    long configID = strtol(id.c_str(), &endPtr, 16);
    if (configID != (long)deviceID) {
        logger.error("Device ID does not match config file ID.");
        return false;
    }

    hostname = (const char*)configJson["hostname"];
    timezone = (const char*)configJson["timezone"];
    setenv("TZ", timezone.c_str(), 1);
    // Optional: a device that has never been told where it is still boots.
    if (configJson.hasOwnProperty("postalCode")) {
        postalCode = (const char*)configJson["postalCode"];
    }
    tzset();

    JSONVar wifi = configJson["wifi"];
    ssid = (const char*)wifi["ssid"];
    wifiPassword = (const char*)wifi["password"];

    JSONVar ota = configJson["ota"];
    otaUser = (const char*)ota["username"];
    otaPassword = (const char*)ota["password"];

    // The thingSpeak and talkBack blocks keep parsing WHATEVER USE_THINGSPEAK
    // and USE_TALKBACK say, and the length check further down keeps counting
    // both API keys. That is deliberate and it is the conservative half of this
    // change: loadFile() returning false means compiled defaults, ssid
    // "undefined", and a board that cannot associate or be reached to fix, so
    // whether a field device's existing document still loads must not depend on
    // which features were compiled in. The keys parse into fields nothing reads
    // — inert, not absent.
    JSONVar thingSpeak = configJson["thingSpeak"];
    thingSpeakAPIKey = (const char*)thingSpeak["apiKey"];
    thingSpeakChannelNumber = (long)thingSpeak["channel"];
    if (thingSpeak.hasOwnProperty("moisture2Field")) {
        const int field = (int)thingSpeak["moisture2Field"];
        // Field 1..8 or 0 for "do not publish". A number outside that range
        // would be silently dropped by ThingSpeak, which looks exactly like a
        // probe that stopped reporting.
        if (field < 0 || field > 8) {
            logger.warning("Ignoring out-of-range thingSpeak.moisture2Field " +
                           String(field));
        } else {
            thingSpeakMoisture2Field = field;
        }
    }

    JSONVar talkBack = configJson["talkBack"];
    talkBackAPIKey = (const char*)talkBack["apiKey"];
    talkBackID = (long)talkBack["channel"];

    JSONVar mqtt = configJson["mqtt"];
    mqttClientID = (const char*)mqtt["clientID"];
    mqttUser = (const char*)mqtt["username"];
    mqttPassword = (const char*)mqtt["password"];
    mqttServer = (const char*)mqtt["server"];
    mqttPort = (int)mqtt["port"];
    mqttCACert = (const char*)mqtt["cacert"];

    if (mqtt.hasOwnProperty("backend")) {
        const String backend = (const char*)JSONVar(mqtt["backend"]);
        if (backend == "thingspeak" || backend == "thingsboard") {
            mqttBackend = backend;
        } else {
            logger.warning("Unknown mqtt.backend '" + backend +
                           "'; keeping " + mqttBackend);
        }
    }
    if (mqtt.hasOwnProperty("useTLS")) {
        mqttUseTLS = (bool)mqtt["useTLS"];
    }

    if (mqtt.hasOwnProperty("rpc")) {
        mqttRpc = (bool)mqtt["rpc"];
    }
    if (mqtt.hasOwnProperty("fwUpdate")) {
        mqttFwUpdate = (bool)mqtt["fwUpdate"];
    }
    if (mqtt.hasOwnProperty("fwTitle")) {
        const String title = (const char*)JSONVar(mqtt["fwTitle"]);
        if (title.length() > 0) {
            mqttFwTitle = title;
        } else {
            // An empty title would match nothing, so every update would be
            // ignored with only a warning per announcement to say why.
            logger.warning("Empty mqtt.fwTitle; keeping " + mqttFwTitle);
        }
    }
    if (mqtt.hasOwnProperty("publishSec")) {
        const int sec = (int)mqtt["publishSec"];
        // Clamped rather than accepted: below 60 s the accumulator windows stop
        // covering a publish interval usefully, and above 300 s a watering that
        // finishes between two ticks leaves a moisture rise with no sampled
        // cause. Out of range is refused loudly instead of being silently
        // honoured -- a device publishing on a period nobody chose is exactly
        // the kind of thing that goes unnoticed for years here.
        if (sec < 60 || sec > 300) {
            logger.warning("Ignoring out-of-range mqtt.publishSec " +
                           String(sec) + "; keeping " + String(mqttPublishSec));
        } else {
            mqttPublishSec = (unsigned)sec;
        }
    }
    if (mqtt.hasOwnProperty("heartbeatSec")) {
        const int sec = (int)mqtt["heartbeatSec"];
        if (sec < 60 || sec > 3600) {
            logger.warning("Ignoring out-of-range mqtt.heartbeatSec " +
                           String(sec) + "; keeping " +
                           String(mqttHeartbeatSec));
        } else {
            mqttHeartbeatSec = (unsigned)sec;
        }
    }

    if (configJson.hasOwnProperty("cloud")) {
        JSONVar cloud = configJson["cloud"];
        if (cloud.hasOwnProperty("enabled")) {
            cloudEnabled = (bool)cloud["enabled"];
        }
    }

    if (configJson.hasOwnProperty("et0")) {
        JSONVar et0 = configJson["et0"];
        if (et0.hasOwnProperty("enabled")) {
            et0Enabled = (bool)et0["enabled"];
        }
        if (et0.hasOwnProperty("latitude")) {
            const double latitude = (double)et0["latitude"];
            // Outside this the sunset hour angle is meaningless and Ra would be
            // computed for a place that does not exist. Refuse the value rather
            // than clamp it: a typo'd latitude is a wrong estimate, and a
            // clamped one hides which.
            if (latitude >= -90.0 && latitude <= 90.0) {
                et0Latitude = (float)latitude;
            } else {
                logger.warning("et0.latitude out of range; ET0 left at " +
                               String(et0Latitude, 4) + ".");
            }
        }
        if (et0.hasOwnProperty("scale")) {
            const double scale = (double)et0["scale"];
            // A correction beyond 2x is not a calibration, it is a different
            // sensor. Refusing keeps a fat-fingered value from silently
            // doubling every number the model reports.
            if (scale >= 0.5 && scale <= 2.0) {
                et0Scale = (float)scale;
            } else {
                logger.warning("et0.scale outside 0.5..2.0; ET0 left at " +
                               String(et0Scale, 3) + ".");
            }
        }
    }

    // A sensor is FITTED if and only if its key exists in `io`. There is no
    // separate enabled flag to drift out of step with the pin it names, and
    // "delete this sensor" in /devices.html is exactly "remove this key".
    JSONVar io = configJson["io"];
    buttonPin = (int)io["button"];
    loadRelays(*this, io);

    dhtFitted = io.hasOwnProperty("dht");
    if (dhtFitted) {
        loadSensor(io["dht"], dhtPin, dhtName);
    }

    loadSoilMoisture(*this, io);

    // "moisture": [ {"dry": <air reading>, "wet": <submerged reading>}, ... ]
    JSONVar moisture = configJson["moisture"];
    if (JSON.typeof(moisture) == "array") {
        for (unsigned i = 0;
             i < moistureCount && i < (unsigned)moisture.length();
             ++i) {
            JSONVar entry = moisture[i];
            if (JSON.typeof(entry) != "object") {
                continue;
            }
            if (entry.hasOwnProperty("dry")) {
                moistureDry[i] = (double)entry["dry"];
            }
            if (entry.hasOwnProperty("wet")) {
                moistureWet[i] = (double)entry["wet"];
            }
            if (entry.hasOwnProperty("relay")) {
                const int relay = (int)entry["relay"];
                if (relay >= -1 && relay < (int)relayCount) {
                    moistureRelay[i] = (int8_t)relay;
                } else {
                    logger.warning("moisture[" + String(i) + "].relay " +
                                   String(relay) + " out of range; ignored.");
                }
            }
            if (entry.hasOwnProperty("invert")) {
                // NOT a bare (bool) cast. Arduino_JSON's operator bool is true
                // only for the JSON literal `true`, so `"invert": 1` — the
                // obvious hand-edit for "yes" — would have read as FALSE and
                // mirrored every reading from this probe: the dashboard, the
                // history, the charts and the telemetry all backwards, with no
                // log line anywhere. The other (bool) casts in this file
                // disable a feature when they get it wrong, which is visible;
                // this one inverts a measurement while everything keeps
                // working.
                JSONVar node = entry["invert"];
                const String kind = JSON.typeof(node);
                if (kind == "boolean") {
                    moistureInvert[i] = (bool)node;
                } else if (kind == "number") {
                    moistureInvert[i] = ((double)node != 0.0);
                } else {
                    logger.warning("moisture[" + String(i) +
                                   "].invert is not a boolean; keeping " +
                                   String(moistureInvert[i] ? "true" : "false"));
                }
            }
            if (JSON.typeof(entry["kind"]) == "string") {
                moistureKind[i] = (const char*)JSONVar(entry["kind"]);
            }
        }
    }

    // Two probes on one MOSFET is the normal wiring and validatePins() allows
    // the shared pin deliberately — but they have to agree on which level
    // energises it. They did not have to before, and the result was silent and
    // exactly inverted: moisturePowerUp() wrote HIGH then LOW, so the first
    // probe was DE-energised during its own conversion, and moisturePowerDown()
    // left the pin HIGH, so its module stayed powered between readings. That
    // second half is the electrolysis this feature exists to prevent.
    for (unsigned i = 0; i < moistureCount; ++i) {
        if (soilMoisturePowerPin[i] == kNoPin) {
            continue;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (soilMoisturePowerPin[j] == soilMoisturePowerPin[i] &&
                soilMoisturePowerOn[j] != soilMoisturePowerOn[i]) {
                logger.warning(
                  "moisture probes " + String(j) + " and " + String(i) +
                  " share power GPIO " + String(soilMoisturePowerPin[i]) +
                  " but disagree on the active level; using probe " +
                  String(j) + "'s.");
                soilMoisturePowerOn[i] = soilMoisturePowerOn[j];
            }
        }
    }

    for (unsigned i = 0; i < moistureCount; ++i) {
        if (moistureRelay[i] >= (int8_t)relayCount) {
            // The default (probe i -> relay i) does not hold on a board with
            // fewer relays than probes. Claiming a relay that does not exist
            // would label every reading against an event that never fires.
            moistureRelay[i] = -1;
        }
    }

    luminosityFitted = io.hasOwnProperty("luminosity");
    if (luminosityFitted) {
        loadSensor(io["luminosity"], luminosityPin, luminosityName);
    }

    waterLevelFitted = io.hasOwnProperty("waterLevel");
    if (waterLevelFitted) {
        loadSensor(io["waterLevel"], waterLevelPin, waterLevelName);
    }

    flowFitted = io.hasOwnProperty("flow");
    if (flowFitted) {
        loadSensor(io["flow"], flowPin, flowName);
        if (JSON.typeof(io["flow"]) == "object") {
            JSONVar flow = io["flow"];
            if (flow.hasOwnProperty("pulsesPerLitre")) {
                const double k = (double)flow["pulsesPerLitre"];
                if (k > 0.0) {
                    flowPulsesPerLitre = (float)k;
                } else {
                    logger.warning("Ignoring non-positive flow.pulsesPerLitre");
                }
            }
        }
    }

    floatFitted = io.hasOwnProperty("floatSwitch");
    if (floatFitted) {
        loadSensor(io["floatSwitch"], floatPin, floatName);
        if (JSON.typeof(io["floatSwitch"]) == "object") {
            JSONVar sw = io["floatSwitch"];
            if (sw.hasOwnProperty("activeLevel")) {
                floatActiveLevel = ((int)sw["activeLevel"] != 0) ? 1 : 0;
            }
            if (sw.hasOwnProperty("interlock")) {
                floatInterlock = (bool)sw["interlock"];
            }
            if (sw.hasOwnProperty("fillRelay")) {
                const int relay = (int)sw["fillRelay"];
                // Out of range would exempt nothing, so the refill relay would
                // be blocked by the interlock along with the pumps it is meant
                // to supply — an empty reservoir that can never be filled.
                if (relay >= -1 && relay < (int)relayCount) {
                    floatFillRelay = relay;
                } else {
                    logger.warning("floatSwitch.fillRelay " + String(relay) +
                                   " out of range; keeping " +
                                   String(floatFillRelay));
                }
            }
        }
        if (floatInterlock) {
            logger.info("Reservoir interlock ON" +
                        (floatFillRelay >= 0
                           ? (" (relay " + String(floatFillRelay) + " exempt)")
                           : String(" (no refill relay exempt)")));
        }
    } else if (floatInterlock) {
        // Refusing to run pumps on a reading no sensor produces would stop
        // every watering, so the interlock cannot survive its sensor.
        logger.warning("Reservoir interlock disabled: no io.floatSwitch.");
        floatInterlock = false;
    }

    // "schedules": [ {enabled, relay, hour, minute, days, durationMs, name} ]
    scheduleCount = 0;
    if (JSON.typeof(configJson["schedules"]) == "array") {
        JSONVar list = configJson["schedules"];
        const unsigned count = list.length();
        if (count > SCHEDULE_COUNT) {
            logger.warning("Config declares " + String(count) +
                           " schedules, " + String(SCHEDULE_COUNT) +
                           " compiled in. The rest are ignored.");
        }

        for (unsigned i = 0; i < SCHEDULE_COUNT && i < count; ++i) {
            JSONVar entry = list[i];
            if (JSON.typeof(entry) != "object") {
                continue;
            }

            Schedule& sch = schedules[scheduleCount];
            // Absent means off. A schedule is a thing that starts a pump on
            // its own, so the fail-safe default is the one that does nothing —
            // and it is the default data/schedules.js gives a new row, which
            // this used to contradict on the very same document.
            sch.enabled = entry.hasOwnProperty("enabled")
                            ? (bool)entry["enabled"]
                            : false;
            sch.relay = entry.hasOwnProperty("relay") ? (int)entry["relay"] : 0;
            sch.hour = entry.hasOwnProperty("hour") ? (int)entry["hour"] : 0;
            sch.minute = entry.hasOwnProperty("minute") ? (int)entry["minute"] : 0;
            sch.days = entry.hasOwnProperty("days") ? (int)entry["days"] : 127;
            sch.durationMs =
              entry.hasOwnProperty("durationMs") ? (int)entry["durationMs"] : 0;
            if (JSON.typeof(entry["name"]) == "string") {
                sch.name = (const char*)JSONVar(entry["name"]);
            } else {
                sch.name = "Schedule " + String(scheduleCount + 1);
            }

            // A schedule pointing at a relay this board does not have, or at
            // an impossible time, would never fire and never say why.
            if (sch.relay >= relayCount) {
                logger.warning(sch.name + ": relay " + String(sch.relay) +
                               " does not exist; ignored.");
                continue;
            }
            if (sch.hour > 23 || sch.minute > 59) {
                logger.warning(sch.name + ": invalid time; ignored.");
                continue;
            }
            if (sch.durationMs == 0) {
                logger.warning(sch.name + ": zero duration; ignored.");
                continue;
            }
            // startRelay() refuses anything above g_relayMaxTime, so a longer
            // schedule would load as valid, count as loaded, and then fail at
            // every single firing with a log line that names no schedule.
            if (sch.durationMs > g_relayMaxTime) {
                logger.warning(sch.name + ": duration " +
                               String(sch.durationMs) + " ms exceeds the " +
                               String(g_relayMaxTime) +
                               " ms relay ceiling; ignored.");
                continue;
            }

            ++scheduleCount;
        }
        logger.info("Loaded " + String(scheduleCount) + " schedule(s).");
    }

    // An out-of-range log level would silence the device entirely, so it is
    // clamped rather than trusted.
    if (JSON.typeof(configJson["log"]) != "undefined") {
        JSONVar log = configJson["log"];
        if (JSON.typeof(log["level"]) != "undefined") {
            int level = (int)log["level"];
            if ((level >= LOG_DISABLE) && (level <= LOG_TRACE)) {
                logLevel = level;
            } else {
                logger.warning("Ignoring out-of-range log level " +
                               String(level));
            }
        }
    }

    if (JSON.typeof(configJson["history"]) != "undefined") {
        JSONVar history = configJson["history"];
        if (history.hasOwnProperty("records")) {
            const int records = (int)history["records"];
            // 5000 records is 256 KB of segment files, rounded to whole
            // LittleFS blocks: 8 * ceil((12 + 625 * 48) / 4096) * 4096. That is
            // what the RECORD costs, and it is the only question this ceiling
            // is entitled to answer. 235 KB is the UNROUNDED figure — the same
            // 8 * (12 + 625 * 48) this repo used to reason with — and it stood
            // here beside the formula that corrects it. Rounding is the whole
            // point: 32 KB of block slack across eight segments is a fifth of
            // what was free on the board this ceiling was first written for.
            //
            // It is deliberately larger than any board here can currently
            // hold. What decides whether a value fits is the state of that
            // device's partition — the web assets, the log backups and the
            // moisture model move with every deploy — so the ceiling can only
            // be an upper bound on the arithmetic, and ioHistoryFitCapacity()
            // below is what compares it against the space that actually
            // exists. A number that was small enough to be safe on one device
            // was necessarily wrong for every other one.
            //
            // The ceiling has been 20000 (800 KB, half again the whole
            // partition, and that one did happen), 6000 while a record was 40
            // bytes, 5000 under a "463 KB usable" SPIFFS figure that survived
            // the LittleFS migration because the driver rename was mechanical,
            // and 2500. Each move was somebody re-deriving the device's free
            // space by hand and writing the answer down as a constant. The
            // constant is in RECORDS and still has to come down if the record
            // grows; the free space is no longer its job.
            if (records >= 0 && records <= 5000) {
                historyRecords = records;
            } else {
                logger.warning("Ignoring out-of-range history.records " +
                               String(records));
            }
        }
        if (history.hasOwnProperty("periodSec")) {
            const int period = (int)history["periodSec"];
            // Floor of 10 s, matching what config.h says: at 1 s this rewrites
            // a flash page 86400 times a day for data nobody reads that fast.
            if (period >= 10 && period <= 3600) {
                historyPeriodSec = period;
            } else {
                logger.warning("Ignoring out-of-range history.periodSec " +
                               String(period));
            }
        }
    }

    const unsigned minChar = g_configMinStringLength;
    if ((hostname.length() < minChar) || (ssid.length() < minChar) ||
        (wifiPassword.length() < minChar) || (otaUser.length() < minChar) ||
        (otaPassword.length() < minChar) ||
        (thingSpeakAPIKey.length() < minChar) ||
        (talkBackAPIKey.length() < minChar)) {
        logger.error(
          "Invalid config file. String fields must have at least 4 characters.");
        return false;
    }

    validateThingSpeakFields();
    validatePins();

    logger.info("Loading done.");
    return true;
}

String
ConfigFile::readFile()
{
    File configFile = FILESYSTEM.open("/config.json", FILE_READ);
    if (configFile == false) {
        logger.error("Failed to open /config.json for reading.");
        return String();
    }

    String data = configFile.readString();
    configFile.close();
    return data;
}

bool
ConfigFile::saveFile(const String& content)
{
    // FILE_WRITE truncates. A short write leaves a corrupt document behind, so
    // the length is checked rather than trusting print().
    File configFile = FILESYSTEM.open("/config.json", FILE_WRITE);
    if (configFile == false) {
        logger.error("Failed to open /config.json for writing.");
        return false;
    }

    const size_t written = configFile.print(content);
    configFile.close();

    if (written != content.length()) {
        logger.error("Short write to /config.json: " + String(written) + "/" +
                     String(content.length()) + " bytes.");
        return false;
    }

    logger.info("Saved /config.json (" + String(written) + " bytes).");
    return true;
}

bool
loadConfigFile(unsigned deviceID)
{
    // Recorded before the load can fail: /nonce derives the decoy salt for an
    // unknown user from it, and that must stay stable even on a device whose
    // config did not load.
    config.deviceId = deviceID;

    bool success = config.loadFile(deviceID);

    // Outside loadFile() on purpose, and this is the whole point of it being
    // here. loadFile() returns early — missing file, unopenable, unparseable,
    // foreign id — and every one of those paths leaves historyRecords at the
    // constructor's 1440 while main.cpp still calls ioHistory.begin() with it.
    // That is exactly the state after a filesystem OTA that dropped
    // /config.json: the partition is at its fullest and the one check that
    // stops append() filling it is the one that did not run. A default that
    // does not fit has to fail as loudly as a configured one that does not,
    // which the comment here used to claim and the code did not do.
    //
    // Clamps in memory and never touches the stored document — the operator's
    // number stays in /config.json, so freeing space is all it takes to get it
    // back. Before setLogLevel() so the clamp is logged at the level that was
    // in force while the rest of the load was.
    config.historyRecords =
      (int)ioHistoryFitCapacity((uint32_t)config.historyRecords);

    logger.setLogLevel((LogLevel)config.logLevel);

    // The config may have moved a relay to a different pin than the one
    // relayPinsSafeInit() parked at boot; park the new one too.
    relayPinsSafeInit();

    return success;
}

void
ConfigFile::clearUndeclaredRelayPins()
{
    // Slots past relayCount still hold the constructor's default pins, and
    // relayPinsSafeInit() runs again after the config loads — over every slot,
    // because at its FIRST call the count is not known yet. Without this, a
    // config declaring one relay would still have GPIO 16/17/18 driven as
    // outputs, so a flow meter or a probe moved onto one of them reads a pin
    // this firmware is holding high. validatePins() cannot catch it either: it
    // only iterates the declared relays.
    for (unsigned i = relayCount; i < RELAY_MAX; ++i) {
        relayPin[i] = kNoPin;
    }
}

void
relayPinsSafeInit()
{
    // Every slot, not just the fitted count: this is called once before the
    // config is known, when relayCount is still the compiled default, and a
    // slot left floating is a slot energised.
    for (unsigned i = 0; i < RELAY_MAX; ++i) {
        if (config.relayPin[i] == ConfigFile::kNoPin) {
            continue;
        }
        pinMode(config.relayPin[i], OUTPUT);
        digitalWrite(config.relayPin[i], !config.relayPinOn[i]);
    }
}
