#include "core/config.h"
#include "core/logger.h"
#include <Arduino_JSON.h>
#include <SPIFFS.h>

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

// Relay 0 keeps GPIO 15 so boards already in the field are unaffected. It is a
// strapping pin (MTDO) — new hardware should override it in config.json.
static const uint8_t g_defaultRelayPin[] = { 15, 16, 17, 18 };
// All ADC1: ADC2 cannot be read while WiFi is associated, so every analog
// channel has to come from GPIO 32-39.
//
// Index 1 is 34 because that is where espgarden5 — the only board that takes a
// second probe on compiled defaults — has it wired, and that board has no water
// level sensor to contend with. A board carrying BOTH a water level sensor and
// a second probe must assign pins in config.json; validatePins() logs the
// collision if it does not. (Briefly changed to 35 to dodge that collision,
// which silently moved espgarden5's probe onto a floating pin.)
//
// GPIO 32/33 double as XTAL_32K_P/N. The ESP32-WROOM-32 on a NodeMCU-32S ships
// without that crystal, so they are ordinary ADC1 inputs; a module that does
// have one fitted cannot use them.
static const uint8_t g_defaultSoilMoisturePin[] = { 36, 34, 32 };

ConfigFile::ConfigFile()
{
    // device
    deviceId = 0;
    hostname = "espgarden";
    timezone = "<-03>3";

    // wifi
    ssid = "undefined";
    wifiPassword = "undefined";

    // OTA
    otaUser = "admin";
    otaPassword = "password";

    // ThingSpeak
    thingSpeakAPIKey = "undefined";
    thingSpeakChannelNumber = 0;

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

    // pins
    buttonPin = 0;

    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        const unsigned defaults =
          sizeof(g_defaultRelayPin) / sizeof(g_defaultRelayPin[0]);
        relayPin[i] = (i < defaults) ? g_defaultRelayPin[i] : 0;
        relayPinOn[i] = 0; // active low
        relayName[i] = (i == 0) ? "Watering" : ("Relay " + String(i + 1));
    }

    dhtPin = 23;

    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        const unsigned defaults = sizeof(g_defaultSoilMoisturePin) /
                                  sizeof(g_defaultSoilMoisturePin[0]);
        soilMoisturePin[i] =
          (i < defaults) ? g_defaultSoilMoisturePin[i] : (uint8_t)A0;

        // Uncalibrated until measured: dry == wet means "do not classify",
        // which is honest rather than inventing a band from nothing.
        moistureDry[i] = 0.0;
        moistureWet[i] = 0.0;

        // A single probe keeps the unsuffixed historical label so existing
        // dashboards do not have to special-case one device.
        soilMoistureName[i] = (MOISTURE_SENSOR_COUNT == 1)
                                ? String("Soil Moisture")
                                : ("Soil Moisture " + String(i + 1));
    }

    dhtName = "";
    luminosityName = "Luminosity";
    waterLevelName = "Water Level";

    luminosityPin = A3;
    waterLevelPin = A6;

    // Both need an internal pull-up, which GPIO 34-39 do not have, and both
    // avoid the strapping pins and the flash pins.
    flowPin = 27;
    flowName = "Flow";
    flowPulsesPerLitre = 450.0; // YF-S201 nominal
    floatPin = 26;
    floatName = "Float Switch";
    floatActiveLevel = 0; // normally-open to ground, with the pull-up

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

    // ~24 h at one record per minute, 57.6 KB of the 512 KB SPIFFS.
    historyRecords = 1440;
    historyPeriodSec = 60;
}

// Reads `io.relays` (array of {pin, on, name}) when present, otherwise falls
// back to the legacy scalar `io.watering` / `io.wateringOn` pair so a config
// written for a single-relay device still loads unchanged.
static void
loadRelays(ConfigFile& cfg, JSONVar& io)
{
    JSONVar relays = io["relays"];

    if (JSON.typeof(relays) == "array") {
        const unsigned count = relays.length();
        if (count < RELAY_COUNT) {
            logger.warning("Config declares " + String(count) + " relays, " +
                           String(RELAY_COUNT) +
                           " compiled in. Missing entries keep defaults.");
        }

        for (unsigned i = 0; i < RELAY_COUNT && i < count; ++i) {
            JSONVar relay = relays[i];

            // An absent key must keep the compiled default, not become 0.
            // Without these checks a relay entry written without "pin" — which
            // a config editor can produce for a row the user left blank — parks
            // the relay on GPIO 0, the boot strapping pin, and "on" silently
            // becomes active-high on a board wired active-low.
            if (relay.hasOwnProperty("pin")) {
                cfg.relayPin[i] = (int)relay["pin"];
            }
            if (relay.hasOwnProperty("on")) {
                cfg.relayPinOn[i] = (int)relay["on"];
            }

            // An absent name keeps the compiled default rather than blanking
            // the label the dashboard renders.
            if (JSON.typeof(relay["name"]) == "string") {
                cfg.relayName[i] = (const char*)relay["name"];
            }
        }
        return;
    }

    if (JSON.typeof(io["watering"]) != "undefined") {
        cfg.relayPin[0] = (int)io["watering"];
        cfg.relayPinOn[0] = (int)io["wateringOn"];
    }
}

// Reads a sensor entry that may be a bare pin number or {pin, name}. Both
// shapes exist in the wild: the pin-only form predates naming, and a device in
// the field must keep loading after a firmware update.
static void
loadSensor(JSONVar node, uint8_t& pin, String& name)
{
    const String type = JSON.typeof(node);
    if (type == "number") {
        pin = (int)node;
        return;
    }
    if (type != "object") {
        return;
    }

    JSONVar entry = node;
    if (entry.hasOwnProperty("pin")) {
        pin = (int)entry["pin"];
    }
    // An absent or empty name keeps the compiled default rather than blanking
    // the label the dashboard renders.
    if (JSON.typeof(entry["name"]) == "string") {
        const String label = (const char*)JSONVar(entry["name"]);
        if (label.length() > 0) {
            name = label;
        }
    }
}

// `io.soilMoisture` is a bare pin number (legacy, one probe), an array of pins,
// or an array of {pin, name}.
static void
loadSoilMoisture(ConfigFile& cfg, JSONVar& io)
{
    JSONVar pins = io["soilMoisture"];

    if (JSON.typeof(pins) == "array") {
        const unsigned count = pins.length();
        if (count < MOISTURE_SENSOR_COUNT) {
            logger.warning("Config declares " + String(count) +
                           " moisture probes, " +
                           String(MOISTURE_SENSOR_COUNT) + " compiled in.");
        }

        for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT && i < count; ++i) {
            loadSensor(pins[i], cfg.soilMoisturePin[i], cfg.soilMoistureName[i]);
        }
        return;
    }

    if (JSON.typeof(pins) != "undefined") {
        loadSensor(pins, cfg.soilMoisturePin[0], cfg.soilMoistureName[0]);
    }
}

// Two peripherals on one GPIO is not a compile error and not a runtime fault —
// it just makes one of them read or drive garbage, which looks like a dead
// sensor. Reported at boot so it is visible in the log instead of being
// diagnosed from odd readings weeks later.
void
ConfigFile::validatePins() const
{
    struct PinUse
    {
        uint8_t pin;
        const char* owner;
    };

    PinUse used[4 + RELAY_COUNT + MOISTURE_SENSOR_COUNT + 2];
    size_t count = 0;
    static char relayLabel[RELAY_COUNT][12];
    static char probeLabel[MOISTURE_SENSOR_COUNT][12];

    used[count++] = { buttonPin, "button" };

    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        snprintf(relayLabel[i], sizeof(relayLabel[i]), "relay%u", i);
        used[count++] = { relayPin[i], relayLabel[i] };
    }

#ifdef HAS_DHT_SENSOR
    used[count++] = { dhtPin, "dht" };
#endif
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        snprintf(probeLabel[i], sizeof(probeLabel[i]), "moisture%u", i);
        used[count++] = { soilMoisturePin[i], probeLabel[i] };
    }
#endif
#ifdef HAS_LUMINOSITY_SENSOR
    used[count++] = { luminosityPin, "luminosity" };
#endif
#ifdef HAS_WATER_LEVEL_SENSOR
    used[count++] = { waterLevelPin, "waterLevel" };
#endif
#ifdef HAS_FLOW_SENSOR
    used[count++] = { flowPin, "flow" };
#endif
#ifdef HAS_FLOAT_SWITCH
    used[count++] = { floatPin, "floatSwitch" };
#endif

    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (used[i].pin == used[j].pin) {
                logger.error("Pin conflict: GPIO " + String(used[i].pin) +
                             " assigned to both " + used[i].owner + " and " +
                             used[j].owner + ".");
            }
        }

        // GPIO 34-39 are input-only on the ESP32 and cannot drive a relay.
        if ((strncmp(used[i].owner, "relay", 5) == 0) && (used[i].pin >= 34) &&
            (used[i].pin <= 39)) {
            logger.error("GPIO " + String(used[i].pin) + " (" + used[i].owner +
                         ") is input-only and cannot drive a relay.");
        }
    }
}

bool
ConfigFile::loadFile(unsigned deviceID)
{
    String filename = "/config.json";
    logger.info("Loading " + filename + "...");

    if (!SPIFFS.exists(filename)) {
        logger.error("Config file " + filename + " not found.");
        return false;
    }

    File configFile = SPIFFS.open(filename, FILE_READ);
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
    tzset();

    JSONVar wifi = configJson["wifi"];
    ssid = (const char*)wifi["ssid"];
    wifiPassword = (const char*)wifi["password"];

    JSONVar ota = configJson["ota"];
    otaUser = (const char*)ota["username"];
    otaPassword = (const char*)ota["password"];

    JSONVar thingSpeak = configJson["thingSpeak"];
    thingSpeakAPIKey = (const char*)thingSpeak["apiKey"];
    thingSpeakChannelNumber = (long)thingSpeak["channel"];

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

    JSONVar io = configJson["io"];
    buttonPin = (int)io["button"];
    loadRelays(*this, io);

#if defined(HAS_DHT_SENSOR)
    loadSensor(io["dht"], dhtPin, dhtName);
#endif
#if defined(HAS_MOISTURE_SENSOR)
    loadSoilMoisture(*this, io);

    // "moisture": [ {"dry": <air reading>, "wet": <submerged reading>}, ... ]
    JSONVar moisture = configJson["moisture"];
    if (JSON.typeof(moisture) == "array") {
        for (unsigned i = 0;
             i < MOISTURE_SENSOR_COUNT && i < (unsigned)moisture.length();
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
        }
    }
#endif
#if defined(HAS_LUMINOSITY_SENSOR)
    loadSensor(io["luminosity"], luminosityPin, luminosityName);
#endif
#if defined(HAS_WATER_LEVEL_SENSOR)
    loadSensor(io["waterLevel"], waterLevelPin, waterLevelName);
#endif
#if defined(HAS_FLOW_SENSOR)
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
#endif
#if defined(HAS_FLOAT_SWITCH)
    loadSensor(io["floatSwitch"], floatPin, floatName);
    if (JSON.typeof(io["floatSwitch"]) == "object") {
        JSONVar sw = io["floatSwitch"];
        if (sw.hasOwnProperty("activeLevel")) {
            floatActiveLevel = ((int)sw["activeLevel"] != 0) ? 1 : 0;
        }
    }
#endif

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
            sch.enabled = entry.hasOwnProperty("enabled")
                            ? (bool)entry["enabled"]
                            : true;
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
            if (sch.relay >= RELAY_COUNT) {
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
            // 6000 records is 240 KB, which fits beside the ~190 KB of web
            // assets in the 463 KB usable SPIFFS. The previous ceiling of 20000
            // was 800 KB — half again the whole partition — and a config that
            // asked for it filled the filesystem, taking the log backup, the
            // user store and POST /config.json down with it. begin() runs
            // before webSetup(), so the admin who caused it had no editor left
            // to undo it.
            if (records >= 0 && records <= 6000) {
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

    validatePins();

    logger.info("Loading done.");
    return true;
}

const char* const g_configSecretMask = "********";
const unsigned g_configMinStringLength = 4;

bool
configDocumentIsUsable(const JSONVar& doc, String& problem)
{
    // Mirrors the tail of loadFile(). Every JSONVar is bound to a named local
    // before being read: operator[] returns by value, and casting a chained
    // subscript straight to const char* reads a freed buffer.
    static const char* const required[][2] = {
        { "", "hostname" },          { "wifi", "ssid" },
        { "wifi", "password" },      { "ota", "username" },
        { "ota", "password" },       { "thingSpeak", "apiKey" },
        { "talkBack", "apiKey" },
    };

    JSONVar document = doc;
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        const char* section = required[i][0];
        const char* key = required[i][1];

        JSONVar node = (section[0] == '\0') ? document[key]
                                            : JSONVar(document[section])[key];
        const String value = (const char*)node;

        if (value.length() < g_configMinStringLength) {
            problem = (section[0] == '\0') ? String(key)
                                           : (String(section) + "." + key);
            return false;
        }
    }

    return true;
}

String
ConfigFile::readFile()
{
    File configFile = SPIFFS.open("/config.json", FILE_READ);
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
    File configFile = SPIFFS.open("/config.json", FILE_WRITE);
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

    logger.setLogLevel((LogLevel)config.logLevel);

    // The config may have moved a relay to a different pin than the one
    // relayPinsSafeInit() parked at boot; park the new one too.
    relayPinsSafeInit();

    return success;
}

void
relayPinsSafeInit()
{
    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        pinMode(config.relayPin[i], OUTPUT);
        digitalWrite(config.relayPin[i], !config.relayPinOn[i]);
    }
}
