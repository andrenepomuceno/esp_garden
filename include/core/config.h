#pragma once

#include "BuildConfig.h"
// Pin capability rules moved to their own header; included here so that every
// file which has always got them from core/config.h still does.
#include "core/config_pins.h"
#include <Arduino.h>
#include <Arduino_JSON.h>

// One scheduled relay activation. `days` is a bitmask with bit 0 = Sunday
// through bit 6 = Saturday, so 127 is every day and 62 is weekdays. Times are
// LOCAL, which is why the timezone matters and why a schedule is skipped
// entirely until NTP has answered.
struct Schedule
{
    bool enabled;
    uint8_t relay;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    unsigned durationMs;
    String name;
};

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
    // ThingSpeak field for the SECOND soil probe, 0 to keep it off. Per-device
    // configuration rather than a build flag: field 4 is free on a board with
    // no water level sensor and taken on a board with one, and renumbering
    // rewrites the meaning of everything already stored under it.
    int thingSpeakMoisture2Field;

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

    // "thingspeak" (default) or "thingsboard". Only the topic and the payload
    // format differ — the broker connection is the same block above. On
    // ThingsBoard the access token goes in mqtt.username and the password is
    // empty, which is how that broker authenticates a device.
    String mqttBackend;
    // ThingsBoard is commonly self-hosted on plain 1883. TLS costs 30-45 KB of
    // heap for the handshake buffers, so it is not forced on.
    bool mqttUseTLS;

    // ThingsBoard downlink. Both are ignored by the ThingSpeak backend, which
    // has no downlink at all beyond TalkBack.
    //
    // mqttRpc accepts two-way RPC on v1/devices/me/rpc/request/+ — relay
    // control and status. The commands run through the same startRelay() as
    // the web UI, so the ceiling and the already-running guard still apply.
    bool mqttRpc;
    // mqttFwUpdate accepts a firmware image pushed from the broker. The arming
    // step is on the ThingsBoard side: an operator has to assign a package to
    // this device before anything is announced.
    bool mqttFwUpdate;
    // Only firmware whose fw_title matches is flashed. One tenant holds every
    // device an operator owns and assigning the wrong package is one wrong
    // click, so this is the catch that keeps another board's image out.
    String mqttFwTitle;

    // How often the PERIODIC payload goes out, in seconds, clamped to 60..300.
    //
    // It is configuration and not a constant because how finely a garden needs
    // sampling is a property of the garden. Only the CONTINUOUS sensor channels
    // ride this tick — step values publish on change (see mqttHeartbeatSec), so
    // lengthening it does not delay a relay transition by a millisecond.
    //
    // Every accumulator window is sized from this at sensorsSetup(), so each
    // average still covers exactly one publish interval, and the publish queue
    // is still an hour deep at any value in range.
    unsigned mqttPublishSec;

    // How long a step value may go unpublished while it does not change, in
    // seconds, clamped to 60..3600.
    //
    // Change-based publishing on its own leaves a key that never changes absent
    // from the cloud for as long as it stays put, and an operator reading
    // "latest" cannot tell a stable value from a dead device. The heartbeat is
    // the floor under that: every step key is re-sent at least this often
    // whether it moved or not.
    unsigned mqttHeartbeatSec;

    // pins
    uint8_t buttonPin;

    // Relays. Index 0 is the watering relay on every board — TalkBack, the
    // legacy `watering` control parameter and the ThingSpeak watering field all
    // address it. relayPinOn holds the logic level that ENERGISES the relay
    // (0 for the usual active-low opto-isolated boards).
    //
    // relayCount is how many of these are FITTED, from config.json. The array
    // is sized by RELAY_MAX, which is only capacity — every loop over relays
    // runs to relayCount, never to RELAY_MAX, or it would drive pins nothing
    // is connected to.
    uint8_t relayCount;
    // Sentinel for "this slot has no pin". relayPinsSafeInit() skips it rather
    // than driving GPIO 0, which is a strapping pin.
    static const uint8_t kNoPin = 255;
    uint8_t relayPin[RELAY_MAX];
    uint8_t relayPinOn[RELAY_MAX];
    String relayName[RELAY_MAX];

    // Presence of each sensor kind, decided by whether its key exists in
    // config.json's `io` object. Deleting the key in /devices.html is what
    // "remove this sensor" means — there is no separate enabled flag to fall
    // out of step with the pin it names.
    uint8_t moistureCount;
    bool dhtFitted;
    bool luminosityFitted;
    bool waterLevelFitted;
    bool flowFitted;
    bool floatFitted;

    uint8_t dhtPin;
    uint8_t soilMoisturePin[MOISTURE_MAX];

    // Optional pin that ENERGISES the probe, and the level that does it.
    // kNoPin means the probe is permanently powered, which is what every board
    // did before this existed and stays the default.
    //
    // This is the whole reason a resistive probe is usable at all. Its two
    // electrodes sit in wet soil with a DC potential across them, which is an
    // electrolysis cell: the anode dissolves, the readings drift, and the
    // sensor is scrap in weeks. Driving VCC from a GPIO and energising it only
    // around the reading takes the duty cycle from 100 % to well under 1 %.
    //
    // A capacitive probe does not need it and loses nothing by having it.
    uint8_t soilMoisturePowerPin[MOISTURE_MAX];
    uint8_t soilMoisturePowerOn[MOISTURE_MAX];

    // How long after energising before the reading means anything. The divider
    // itself settles in microseconds; what takes time is the module's own
    // regulator and comparator. Per probe because it is a property of the
    // module, and configurable because the point of all this is not needing a
    // new firmware when a different sensor arrives.
    uint16_t soilMoistureSettleMs[MOISTURE_MAX];

    // Display labels. /data.json keys Inputs by these, so they are what the
    // dashboard, the history charts and the table show. They are labels, not
    // identifiers: telemetry keys and the Relays array stay index-based, so
    // renaming a probe never rewrites stored history.
    String soilMoistureName[MOISTURE_MAX];
    String dhtName;
    String luminosityName;
    String waterLevelName;

    // Two-point calibration, per probe: the reading with the probe in air and
    // the reading submerged in water. Every capacitive probe has its own gain
    // and offset, so these are not shared. No ordering is assumed — with the
    // current 100-ADC% conversion the air reading is believed to be the
    // SMALLER number, but that has never been checked on hardware and nothing
    // depends on it: no ordering is assumed anywhere.
    // Equal values disable classification for that probe.
    float moistureDry[MOISTURE_MAX];
    float moistureWet[MOISTURE_MAX];

    // Which way the raw reading moves as the soil dries, per probe.
    //
    // It was a single `100 - pct` in sensorsReadIo(), correct for the
    // capacitive v2 modules — their 555 oscillator output FALLS as the soil
    // wets — and wrong for anything wired the other way. A resistive probe on
    // the usual divider does the opposite: wet soil conducts, the node rises.
    // With one probe of each fitted, no global sign is right, so the sign
    // belongs to the probe.
    //
    // The classifier never cared: both the two-point calibration and the
    // ordering gate accept either direction. What cared is the NUMBER on the
    // dashboard and in the history, which would have run backwards.
    bool moistureInvert[MOISTURE_MAX];

    // Free-text label for what is physically in the pot. It is not used to
    // decide anything — it is part of the trained model's IDENTITY, so
    // changing it throws that probe's statistics away.
    //
    // Without it, swapping a capacitive sensor for a resistive one on the same
    // pin keeps weeks of accumulated Gaussians: same pin, same relay, so the
    // model's own identity check sees no change and the new probe inherits the
    // old one's bands. That is confident nonsense of exactly the kind the
    // separation gate exists to prevent, arriving through the one door the
    // gate does not watch.
    String moistureKind[MOISTURE_MAX];

    // Which relay waters which probe. The Bayesian model labels a reading by
    // its distance from a watering EVENT, so it needs to know whose pump
    // matters — and on a rectangular planter with one pump per zone, probe i
    // is not necessarily fed by relay i.
    //
    // -1 means no pump feeds this probe: nothing labels its readings, so it
    // never gets a model and falls back to the two-point calibration.
    int8_t moistureRelay[MOISTURE_MAX];
    uint8_t luminosityPin;
    uint8_t waterLevelPin;

    // Pulse-output flow meter (YF-S201 and friends). pulsesPerLitre is the K
    // factor from the datasheet; it is per-model and per-plumbing, so it is
    // configuration rather than a constant.
    uint8_t flowPin;
    String flowName;
    float flowPulsesPerLitre;

    // Float switch: a bare contact, not an analog level. floatActiveLevel is
    // the logic level that means "float raised"; the input is driven with an
    // internal pull-up, so a normally-open switch to ground reads 0 when
    // raised.
    uint8_t floatPin;
    String floatName;
    uint8_t floatActiveLevel;

    // Reservoir interlock. When on, startRelay() refuses a zone relay while
    // the float reads empty, which is the difference between a pump that runs
    // dry for 30 s and one that does not run at all.
    //
    // DEFAULT OFF, deliberately. A float that is not wired yet reads at the
    // pull-up, i.e. "empty", so switching this on by default would stop every
    // watering on every board that has the sensor compiled in and not fitted.
    // It is a decision that belongs to whoever looked at the reading.
    bool floatInterlock;
    // Relay that refills the reservoir, exempt from the interlock — blocking
    // the one thing that fixes an empty tank would deadlock the system.
    // -1 when no relay refills.
    int floatFillRelay;

    Schedule schedules[SCHEDULE_COUNT];
    unsigned scheduleCount;

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

    // Clears the pin of every relay slot past relayCount, so relayPinsSafeInit()
    // cannot drive a pin the config gave to something else.
    void clearUndeclaredRelayPins();

    // Drops thingSpeak.moisture2Field when it names a slot another FITTED
    // sensor already publishes. Runs after the io block, because that is when
    // the fitted flags are known.
    void validateThingSpeakFields();

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
