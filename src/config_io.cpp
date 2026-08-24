// Parsers for the `io` block of config.json, where every entry accepts more
// than one shape so that a device already in the field keeps loading after a
// firmware update.

#include "core/config_io.h"
#include "core/config.h"
#include "core/logger.h"
#include <Arduino_JSON.h>

// Reads `io.relays` (array of {pin, on, name}) when present, otherwise falls
// back to the legacy scalar `io.watering` / `io.wateringOn` pair so a config
// written for a single-relay device still loads unchanged.
void
loadRelays(ConfigFile& cfg, JSONVar& io)
{
    JSONVar relays = io["relays"];

    if (JSON.typeof(relays) == "array") {
        const unsigned count = relays.length();
        // The array LENGTH is how many relays this board has. That is the
        // whole point of the change: adding a row in /devices.html adds a
        // relay, with no rebuild and no flag to keep in step.
        if (count > RELAY_MAX) {
            logger.warning("Config declares " + String(count) + " relays; " +
                           String(RELAY_MAX) +
                           " is the firmware maximum. The rest are ignored.");
        }
        cfg.relayCount = (count > RELAY_MAX) ? RELAY_MAX : count;
        cfg.clearUndeclaredRelayPins();

        for (unsigned i = 0; i < cfg.relayCount; ++i) {
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
        // Pre-2.0 scalar form: exactly one relay.
        cfg.relayCount = 1;
        cfg.clearUndeclaredRelayPins();
        cfg.relayPin[0] = (int)io["watering"];
        cfg.relayPinOn[0] = (int)io["wateringOn"];
        return;
    }

    // No relays declared at all. Not an error — a sensor-only node is a
    // legitimate configuration — but it has to be said, because every relay
    // button in the UI disappearing otherwise looks like a bug.
    cfg.relayCount = 0;
    cfg.clearUndeclaredRelayPins();
    logger.warning("No io.relays in config: this device drives nothing.");
}

// Reads a sensor entry that may be a bare pin number or {pin, name}. Both
// shapes exist in the wild: the pin-only form predates naming, and a device in
// the field must keep loading after a firmware update.
void
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

// With exactly one probe fitted, and no name given for it, the label stays the
// unsuffixed "Soil Moisture" it has always been. /data.json keys Inputs by this
// string, so suffixing it on a one-probe board silently renames the channel
// every existing dashboard reads.
static void
applySingleProbeLabel(ConfigFile& cfg)
{
    if (cfg.moistureCount == 1 && cfg.soilMoistureName[0] == "Soil Moisture 1") {
        cfg.soilMoistureName[0] = "Soil Moisture";
    }
}

// `io.soilMoisture` is a bare pin number (legacy, one probe), an array of pins,
// or an array of {pin, name}.
void
loadSoilMoisture(ConfigFile& cfg, JSONVar& io)
{
    JSONVar pins = io["soilMoisture"];

    if (JSON.typeof(pins) == "array") {
        const unsigned count = pins.length();
        if (count > MOISTURE_MAX) {
            logger.warning("Config declares " + String(count) +
                           " moisture probes; " + String(MOISTURE_MAX) +
                           " is the firmware maximum (the history record has "
                           "that many slots). The rest are ignored.");
        }
        cfg.moistureCount = (count > MOISTURE_MAX) ? MOISTURE_MAX : count;

        for (unsigned i = 0; i < cfg.moistureCount; ++i) {
            loadSensor(pins[i], cfg.soilMoisturePin[i], cfg.soilMoistureName[i]);
        }
        applySingleProbeLabel(cfg);
        return;
    }

    if (JSON.typeof(pins) != "undefined") {
        // Legacy scalar: exactly one probe.
        cfg.moistureCount = 1;
        loadSensor(pins, cfg.soilMoisturePin[0], cfg.soilMoistureName[0]);
        applySingleProbeLabel(cfg);
        return;
    }

    cfg.moistureCount = 0;
}
