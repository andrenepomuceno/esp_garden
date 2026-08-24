#pragma once

#include "BuildConfig.h"
#include <Arduino.h>
#include <Arduino_JSON.h>

class ConfigFile
{
  public:
    // device
    unsigned deviceId; // low 16 bits of the efuse MAC; must match the file's "id"
    String hostname;
    String timezone;

    // wifi
    String ssid;
    String wifiPassword;

    // OTA
    String otaUser;
    String otaPassword;

    // ThingSpeak
    String thingSpeakAPIKey;
    long thingSpeakChannelNumber;

    // TalkBack
    String talkBackAPIKey;
    long talkBackID;

    // MQTT
    String mqttUser;
    String mqttPassword;
    String mqttClientID;
    String mqttServer;
    int mqttPort;
    String mqttCACert;

    // pins
    uint8_t buttonPin;

    // Relays. Index 0 is the watering relay on every board — TalkBack, the
    // legacy `watering` control parameter and the ThingSpeak watering field all
    // address it. relayPinOn holds the logic level that ENERGISES the relay
    // (0 for the usual active-low opto-isolated boards).
    uint8_t relayPin[RELAY_COUNT];
    uint8_t relayPinOn[RELAY_COUNT];
    String relayName[RELAY_COUNT];

    uint8_t dhtPin;
    uint8_t soilMoisturePin[MOISTURE_SENSOR_COUNT];

    // Two-point calibration, per probe: the reading with the probe in air and
    // the reading submerged in water. Every capacitive probe has its own gain
    // and offset, so these are not shared. No ordering is assumed — with the
    // current 100-ADC% conversion the air reading is the SMALLER number.
    // Equal values disable classification for that probe.
    float moistureDry[MOISTURE_SENSOR_COUNT];
    float moistureWet[MOISTURE_SENSOR_COUNT];
    uint8_t luminosityPin;
    uint8_t waterLevelPin;

    // log
    int logLevel;

    // I/O history ring buffer. `historyRecords` is the file capacity — 0
    // disables it entirely. `historyPeriodSec` paces the appends: SPIFFS
    // rewrites a whole page per record, so a 1 s period would burn flash for
    // data nobody reads at that resolution.
    int historyRecords;
    int historyPeriodSec;

    ConfigFile();

    bool loadFile(unsigned deviceID);

    // Logs every GPIO assigned to more than one peripheral, and every relay
    // parked on an input-only pin. Diagnostic only — it never refuses a config.
    void validatePins() const;

    // Replaces /config.json wholesale — there is no merge at this level. The
    // caller is responsible for having produced a complete document; the
    // handler in web.cpp does that by merging into the file already on disk.
    // Takes effect on the next boot: nothing re-reads the file at runtime.
    bool saveFile(const String& content);

    // Raw text of /config.json, or an empty String when it cannot be read.
    String readFile();
};

// Value substituted for every secret in GET /config.json. Sending it back
// unchanged in POST /config.json keeps the stored value.
extern const char* const g_configSecretMask;

// Shortest acceptable credential/identity string. loadFile() refuses a document
// that violates this, so anything that writes /config.json must refuse it too.
extern const unsigned g_configMinStringLength;

// True when `doc` would survive loadFile(). On false, `problem` names the field
// so the caller can say which one. Exists so the write path cannot persist a
// document the boot path will reject — that combination leaves the device on
// compiled defaults it cannot connect with, and unreachable without USB.
bool
configDocumentIsUsable(const JSONVar& doc, String& problem);

extern ConfigFile config;

// Backward-compatible references to config members.
// Existing code continues to use g_* directly; new code should prefer
// config.<field>. Pin fields deliberately have no alias — they are arrays now
// and every user goes through config.
extern String& g_hostname;
extern String& g_timezone;

extern String& g_ssid;
extern String& g_wifiPassword;

extern String& g_otaUser;
extern String& g_otaPassword;

extern String& g_thingSpeakAPIKey;
extern long& g_thingSpeakChannelNumber;

extern String& g_talkBackAPIKey;
extern long& g_talkBackID;

extern String& g_mqttUser;
extern String& g_mqttPassword;
extern String& g_mqttClientID;
extern String& g_mqttServer;
extern int& g_mqttPort;
extern String& g_mqttCACert;

bool
loadConfigFile(unsigned deviceID);

// Drive every relay to its idle level. Called as the first statement of
// setup(): until pinMode() runs the pins float, and an active-low relay board
// reads a floating input as "energise". Boot takes seconds (SPIFFS mount, config
// load, WiFi association), which is long enough to run a pump dry.
void
relayPinsSafeInit();
