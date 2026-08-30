// Whole-document validation of a config.json before it is written: the
// save-time counterpart of loadFile(), so POST /config.json cannot persist a
// document the boot path would reject.

#include "core/config.h"
#include "core/config_pins.h"
#include <Arduino_JSON.h>

const char* const g_configSecretMask = "********";
const unsigned g_configMinStringLength = 4;

// Reads a pin out of an io entry that may be a bare number or {pin, ...}, the
// same two shapes loadSensor() accepts. Returns false when the entry names no
// pin at all, which is a legitimate "not fitted" rather than an error.
static bool
documentPin(JSONVar node, int& out)
{
    const String type = JSON.typeof(node);
    if (type == "number") {
        out = (int)node;
        return true;
    }
    if (type != "object") {
        return false;
    }
    JSONVar entry = node;
    if (!entry.hasOwnProperty("pin")) {
        return false;
    }
    out = (int)entry["pin"];
    return true;
}

// A GPIO number outside the chip's range, rejected BEFORE the predicates.
//
// They take uint8_t, so 256 truncates to 0 and every check runs against GPIO 0
// — a strapping pin — and passes. The document is written, and at boot
// loadRelays truncates the same way and relayPinsSafeInit() drives GPIO 0 on
// every boot. This function exists because boot is too late.
static bool
pinNumberIsPlausible(int pin)
{
    return pin >= 0 && pin <= 39;
}

// The save-time half of validatePins(). validatePins() logs at boot, which is
// too late to be useful: by then the document is on flash and the device may
// be reading noise or driving the SPI flash. Refusing here means the mistake
// is caught while the device is still reachable to fix it.
static bool
documentPinsAreUsable(JSONVar& document, String& problem)
{
    if (JSON.typeof(document["io"]) != "object") {
        return true; // no io block: nothing to check
    }
    JSONVar io = document["io"];

    struct Check
    {
        const char* key;
        bool analog; // ADC1 only
    };
    static const Check checks[] = {
        { "luminosity", true },  { "waterLevel", true }, { "dht", false },
        { "flow", false },       { "floatSwitch", false },
    };

    int pin = 0;

    JSONVar relays = io["relays"];
    if (JSON.typeof(relays) == "array") {
        for (unsigned i = 0; i < (unsigned)relays.length(); ++i) {
            if (!documentPin(relays[i], pin)) {
                continue;
            }
            if (!pinNumberIsPlausible(pin)) {
                problem = "io.relays[" + String(i) + "] on GPIO " + String(pin) +
                          " (no such pin on an ESP32)";
                return false;
            }
            if (pinIsFlash(pin) || pinIsInputOnly(pin) || !pinIsBonded(pin)) {
                problem =
                  "io.relays[" + String(i) + "] on GPIO " + String(pin) +
                  (pinIsFlash(pin)
                     ? " (SPI flash)"
                     : (!pinIsBonded(pin) ? " (not bonded out on this module)"
                                          : " (input-only)"));
                return false;
            }
        }
    }

    JSONVar probes = io["soilMoisture"];
    if (JSON.typeof(probes) == "array") {
        for (unsigned i = 0; i < (unsigned)probes.length(); ++i) {
            if (documentPin(probes[i], pin) && !pinNumberIsPlausible(pin)) {
                problem = "io.soilMoisture[" + String(i) + "] on GPIO " +
                          String(pin) + " (no such pin on an ESP32)";
                return false;
            }
            if (documentPin(probes[i], pin) && !pinIsADC1(pin)) {
                // pinIsADC1 already excludes 37 and 38, the two ADC1 channels
                // the module does not bring out, so this covers bonding too.
                problem = "io.soilMoisture[" + String(i) + "] on GPIO " +
                          String(pin) + " (not an ADC1 channel)";
                return false;
            }

            // The power pin DRIVES, so it needs the output rules — and it needs
            // them here rather than only at boot, which is the whole point of
            // this function: by the time validatePins() complains the document
            // is already on flash. GPIO 6-11 is the SPI flash, and toggling one
            // of those every second does not misbehave, it hangs; 34-39 are
            // input-only, where pinMode and digitalWrite are silent no-ops and
            // the probe simply never gets powered.
            JSONVar probe = probes[i];
            if (JSON.typeof(probe) == "object" &&
                probe.hasOwnProperty("powerPin")) {
                const int power = (int)probe["powerPin"];
                if (power >= 0) {
                    if (!pinNumberIsPlausible(power)) {
                        problem = "io.soilMoisture[" + String(i) +
                                  "].powerPin GPIO " + String(power) +
                                  " (no such pin on an ESP32)";
                        return false;
                    }
                    const uint8_t p = (uint8_t)power;
                    if (!pinIsBonded(p) || pinIsFlash(p) ||
                        pinIsInputOnly(p)) {
                        problem = "io.soilMoisture[" + String(i) +
                                  "].powerPin GPIO " + String(power) +
                                  " cannot drive an output";
                        return false;
                    }
                }
            }
        }
    } else if (documentPin(probes, pin) && !pinIsADC1(pin)) {
        problem = "io.soilMoisture on GPIO " + String(pin) + " (not ADC1)";
        return false;
    }

    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (!io.hasOwnProperty(checks[i].key)) {
            continue; // not fitted
        }
        if (!documentPin(io[checks[i].key], pin)) {
            continue;
        }
        if (!pinNumberIsPlausible(pin)) {
            problem = String("io.") + checks[i].key + " on GPIO " +
                      String(pin) + " (no such pin on an ESP32)";
            return false;
        }
        if (pinIsFlash(pin) || !pinIsBonded(pin)) {
            // pinIsBonded was checked at boot but not here, so a save could
            // put a sensor on GPIO 20/24/28-31 — pins the WROOM-32 does not
            // bring out — and only the next boot log would say so. That is
            // exactly the "boot is too late" case this function exists for.
            problem = String("io.") + checks[i].key + " on GPIO " +
                      String(pin) +
                      (pinIsFlash(pin) ? " (SPI flash)"
                                       : " (not bonded out on this module)");
            return false;
        }
        if (checks[i].analog && !pinIsADC1(pin)) {
            problem =
              String("io.") + checks[i].key + " on GPIO " + String(pin) +
              " (not an ADC1 channel; ADC2 cannot be read with WiFi on)";
            return false;
        }
        if (!checks[i].analog && pinIsInputOnly(pin)) {
            problem = String("io.") + checks[i].key + " on GPIO " +
                      String(pin) + " (no internal pull-up)";
            return false;
        }
    }

    return true;
}

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

    return documentPinsAreUsable(document, problem);
}
