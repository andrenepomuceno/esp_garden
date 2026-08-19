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
// A0 and A6 are both ADC1. ADC2 cannot be read while WiFi is associated, so
// every analog channel has to come from GPIO 32-39.
static const uint8_t g_defaultSoilMoisturePin[] = { A0, A6 };

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
    }

    luminosityPin = A3;
    waterLevelPin = A6;

    // log
    logLevel = LOG_INFO;
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
            cfg.relayPin[i] = (int)relay["pin"];
            cfg.relayPinOn[i] = (int)relay["on"];

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

// `io.soilMoisture` is either a bare pin number (legacy, one probe) or an
// array of pins.
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
            cfg.soilMoisturePin[i] = (int)pins[i];
        }
        return;
    }

    if (JSON.typeof(pins) != "undefined") {
        cfg.soilMoisturePin[0] = (int)pins;
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

    PinUse used[2 + RELAY_COUNT + MOISTURE_SENSOR_COUNT + 2];
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

    JSONVar io = configJson["io"];
    buttonPin = (int)io["button"];
    loadRelays(*this, io);

#if defined(HAS_DHT_SENSOR)
    dhtPin = (int)io["dht"];
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
    luminosityPin = (int)io["luminosity"];
#endif
#if defined(HAS_WATER_LEVEL_SENSOR)
    waterLevelPin = (int)io["waterLevel"];
#endif

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
