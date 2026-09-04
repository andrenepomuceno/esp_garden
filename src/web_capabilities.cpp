#include "BuildConfig.h"
#include "core/filesystem.h"
#include "core/config.h"
#include "network/web_capabilities.h"
#include <Arduino_JSON.h>

// Everything /devices.html needs in order to offer only choices the firmware
// would accept. It comes from the firmware rather than being duplicated in
// JavaScript, because a UI that hardcodes "GPIO 32-39 are analog" keeps saying
// so after someone raises MOISTURE_MAX or ports this to an S3 with a different
// pinout — and the first sign of the drift is a channel reading noise.
void
handleCapabilitiesJson(AsyncWebServerRequest* request)
{
    JSONVar doc;

    doc["firmware"] = FW_VERSION;
    doc["relayMax"] = (int)RELAY_MAX;
    doc["moistureMax"] = (int)MOISTURE_MAX;

    // The upload path limit, so /update.html can refuse a long path before
    // spending a whole upload on it without restating a rule the firmware owns.
    doc["maxPathLength"] = (int)FILESYSTEM_MAX_PATH;

    // The kinds this build has drivers for. A web page cannot add a kind that
    // is not in this list: a DHT needs the DHT library linked in, which is the
    // one thing that stayed a compile-time decision.
    const char* const kinds[] = { "relays",     "soilMoisture", "dht",
                                  "luminosity", "waterLevel",   "flow",
                                  "floatSwitch" };
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        doc["kinds"][i] = kinds[i];
    }

    // Walking every GPIO and asking the same predicates validatePins() asks,
    // so the two cannot disagree. The upper bound comes from the same place:
    // an S3 has GPIO 40-48 and a hardcoded 39 here would have hidden its UART,
    // its strapping pins and half its spares from the picker.
    //
    // Hoisted rather than re-asked: pinMaxGpio() is an out-of-line call in
    // another translation unit and the answer is a compile-time property of
    // the chip, so evaluating it once per iteration spent ~49 calls per
    // request on the single async_tcp task for a number that cannot move.
    unsigned analog = 0, output = 0, digital = 0, strapping = 0, reserved = 0;
    const uint8_t maxGpio = pinMaxGpio();
    for (uint8_t pin = 0; pin <= maxGpio; ++pin) {
        if (pinIsFlash(pin) || !pinIsBonded(pin)) {
            // Never offered. One hangs the chip; the other is a pin that
            // cannot be wired to anything, and a sensor assigned to it reads
            // nothing with no error to say why.
            continue;
        }

        if (pinIsSerialConsole(pin)) {
            // Listed, but separately: it works, and it costs the boot log —
            // the one diagnostic left on a device whose config did not load.
            doc["reservedPins"][reserved++] = (int)pin;
            continue;
        }

        if (pinIsADC1(pin)) {
            doc["analogPins"][analog++] = (int)pin;
        }
        if (!pinIsInputOnly(pin)) {
            // digitalPins is the same predicate and kept only because
            // devices_model.js asks for it by name: an input that needs a
            // pull-up and an output both require a pin that is not input-only.
            // If those rules ever diverge, this is the place they diverge.
            doc["outputPins"][output++] = (int)pin;
            doc["digitalPins"][digital++] = (int)pin;
        }
        if (pinIsStrapping(pin)) {
            doc["strappingPins"][strapping++] = (int)pin;
        }
    }

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(doc));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}
