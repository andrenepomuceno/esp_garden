// Pin capability rules for the ESP32-WROOM-32, and the boot-time audit that
// applies them to a loaded config: the one place that decides whether a pin
// assignment is physically possible.

#include "core/config_pins.h"
#include "core/config.h"
#include "core/logger.h"

// Two peripherals on one GPIO is not a compile error and not a runtime fault —
// it just makes one of them read or drive garbage, which looks like a dead
// sensor. Reported at boot so it is visible in the log instead of being
// diagnosed from odd readings weeks later.
// Pin capability rules, in ONE place. validatePins() checks them at boot,
// handleConfigPost refuses a document that breaks them, and /capabilities.json
// hands the same numbers to /devices.html so the UI cannot drift from the
// firmware it is configuring.
bool
pinIsADC1(uint8_t pin)
{
    // ADC2 cannot be read while WiFi is associated, so every analog channel
    // has to be ADC1. GPIO 37 and 38 are ADC1 on the die but are not bonded
    // out on a WROOM-32 module, which leaves six usable channels.
    return (pin >= 32 && pin <= 39) && pin != 37 && pin != 38;
}

bool
pinIsInputOnly(uint8_t pin)
{
    // 34-39 have no output driver and no internal pull-up: fine for analog,
    // useless for a relay, a DHT or a switch that needs one.
    return pin >= 34 && pin <= 39;
}

bool
pinIsBonded(uint8_t pin)
{
    // GPIO 20, 24 and 28-31 exist in the SoC's numbering but are not brought
    // out on an ESP32-WROOM-32 module. Offering one in a picker is offering a
    // pin that can never be wired to anything, and the symptom is a sensor
    // that reads nothing with no error anywhere.
    if (pin == 20 || pin == 24 || (pin >= 28 && pin <= 31)) {
        return false;
    }
    // 37 and 38 are the same story on the analog side; pinIsADC1 already
    // excludes them.
    return pin <= 39 && pin != 37 && pin != 38;
}

bool
pinIsSerialConsole(uint8_t pin)
{
    // UART0. The logger writes the whole boot sequence here at 115200, and it
    // is the only way to see a device whose config did not load — which is
    // precisely the situation a bad pin assignment creates. Taking TX means
    // losing the diagnosis of the mistake that took it.
    return pin == 1 || pin == 3;
}

bool
pinIsFlash(uint8_t pin)
{
    return pin >= 6 && pin <= 11; // wired to the SPI flash; touching one hangs
}

bool
pinIsStrapping(uint8_t pin)
{
    // Sampled at reset to choose the boot mode. Usable, but a pull on one can
    // stop the board booting, so it is worth saying out loud.
    return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15;
}

void
ConfigFile::validatePins() const
{
    enum Role
    {
        ROLE_OUTPUT, // relay: must drive
        ROLE_ANALOG, // must be on ADC1
        ROLE_DIGITAL // needs an internal pull-up
    };

    struct PinUse
    {
        uint8_t pin;
        const char* owner;
        Role role;
    };

    PinUse used[1 + RELAY_MAX + MOISTURE_MAX + 5];
    size_t count = 0;
    static char relayLabel[RELAY_MAX][12];
    static char probeLabel[MOISTURE_MAX][12];

    used[count++] = { buttonPin, "button", ROLE_DIGITAL };

    for (unsigned i = 0; i < relayCount; ++i) {
        snprintf(relayLabel[i], sizeof(relayLabel[i]), "relay%u", i);
        used[count++] = { relayPin[i], relayLabel[i], ROLE_OUTPUT };
    }

    if (dhtFitted) {
        used[count++] = { dhtPin, "dht", ROLE_DIGITAL };
    }
    for (unsigned i = 0; i < moistureCount; ++i) {
        snprintf(probeLabel[i], sizeof(probeLabel[i]), "moisture%u", i);
        used[count++] = { soilMoisturePin[i], probeLabel[i], ROLE_ANALOG };
    }
    if (luminosityFitted) {
        used[count++] = { luminosityPin, "luminosity", ROLE_ANALOG };
    }
    if (waterLevelFitted) {
        used[count++] = { waterLevelPin, "waterLevel", ROLE_ANALOG };
    }
    if (flowFitted) {
        used[count++] = { flowPin, "flow", ROLE_DIGITAL };
    }
    if (floatFitted) {
        used[count++] = { floatPin, "floatSwitch", ROLE_DIGITAL };
    }

    for (size_t i = 0; i < count; ++i) {
        const uint8_t pin = used[i].pin;
        const char* owner = used[i].owner;

        for (size_t j = i + 1; j < count; ++j) {
            if (pin == used[j].pin) {
                logger.error("Pin conflict: GPIO " + String(pin) +
                             " assigned to both " + owner + " and " +
                             used[j].owner + ".");
            }
        }

        if (pinIsFlash(pin)) {
            logger.error("GPIO " + String(pin) + " (" + owner +
                         ") is wired to the SPI flash and cannot be used.");
            continue;
        }

        if (!pinIsBonded(pin)) {
            logger.error("GPIO " + String(pin) + " (" + owner +
                         ") is not bonded out on this module.");
            continue;
        }

        if (pinIsSerialConsole(pin)) {
            logger.warning("GPIO " + String(pin) + " (" + owner +
                           ") is the serial console; the boot log is lost.");
        }

        switch (used[i].role) {
            case ROLE_OUTPUT:
                if (pinIsInputOnly(pin)) {
                    logger.error("GPIO " + String(pin) + " (" + owner +
                                 ") is input-only and cannot drive a relay.");
                }
                if (pinIsStrapping(pin)) {
                    logger.warning("GPIO " + String(pin) + " (" + owner +
                                   ") is a strapping pin; a pull on it can "
                                   "stop the board booting.");
                }
                break;

            case ROLE_ANALOG:
                if (!pinIsADC1(pin)) {
                    logger.error("GPIO " + String(pin) + " (" + owner +
                                 ") is not an ADC1 channel. ADC2 cannot be "
                                 "read while WiFi is on, so this reads noise.");
                }
                break;

            case ROLE_DIGITAL:
                if (pinIsInputOnly(pin)) {
                    logger.error("GPIO " + String(pin) + " (" + owner +
                                 ") has no internal pull-up; this input will "
                                 "float.");
                }
                break;
        }
    }
}
