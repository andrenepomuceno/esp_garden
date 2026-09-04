// The pin capability rules for the chip being compiled for, and the boot-time
// audit that applies them to a loaded config: the one place that decides
// whether a pin assignment is physically possible.
//
// The rules themselves are in core/pin_rules.h, one namespace per MCU family
// and both always compiled, so test_pin_rules can hold BOTH to their answers
// in one host binary. This file is only the seam that picks one.

#include "core/config_pins.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/pin_rules.h"

// <Arduino.h> (through config_pins.h) is what pulls in sdkconfig.h, which is
// where CONFIG_IDF_TARGET_* comes from. That is deliberately the selector
// rather than a -D in platformio.ini: it is set by the framework because of
// the env's `board` line, so the pin rules cannot drift from the chip the
// toolchain is actually building for, and platformio.ini keeps carrying no
// hardware flags at all.
//
// An unrecognised target is a hard error rather than a silent fallback. The
// failure mode of guessing wrong here is a page of boot errors about correct
// assignments and silence about wrong ones — which is precisely what a reader
// would never trace back to a default in this file.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
namespace family = pin_rules::esp32s3;
#elif defined(CONFIG_IDF_TARGET_ESP32)
namespace family = pin_rules::wroom32;
#else
#error "Unknown CONFIG_IDF_TARGET: add a pin_rules namespace and tests."
#endif

bool
pinIsADC1(uint8_t pin)
{
    return family::isADC1(pin);
}

bool
pinIsInputOnly(uint8_t pin)
{
    return family::isInputOnly(pin);
}

bool
pinIsBonded(uint8_t pin)
{
    return family::isBonded(pin);
}

bool
pinIsSerialConsole(uint8_t pin)
{
    return family::isSerialConsole(pin);
}

bool
pinIsFlash(uint8_t pin)
{
    return family::isFlash(pin);
}

bool
pinIsStrapping(uint8_t pin)
{
    return family::isStrapping(pin);
}

uint8_t
pinMaxGpio()
{
    return (uint8_t)family::kMaxGpio;
}

// Two peripherals on one GPIO is not a compile error and not a runtime fault —
// it just makes one of them read or drive garbage, which looks like a dead
// sensor. Reported at boot so it is visible in the log instead of being
// diagnosed from odd readings weeks later.
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

    PinUse used[1 + RELAY_MAX + 2 * MOISTURE_MAX + 5];
    size_t count = 0;
    static char relayLabel[RELAY_MAX][12];
    static char probeLabel[MOISTURE_MAX][12];
    static char powerLabel[MOISTURE_MAX][16];

    used[count++] = { buttonPin, "button", ROLE_DIGITAL };

    for (unsigned i = 0; i < relayCount; ++i) {
        // A declared relay with no pin is a state the rest of the firmware
        // treats as valid — relayWrite() and startRelay() both refuse it
        // explicitly. Auditing it here reported "GPIO 255 is not bonded out",
        // which turns the one diagnostic meant to name a wrong pin assignment
        // into a fault report for a correct configuration.
        if (relayPin[i] == kNoPin) {
            continue;
        }
        snprintf(relayLabel[i], sizeof(relayLabel[i]), "relay%u", i);
        used[count++] = { relayPin[i], relayLabel[i], ROLE_OUTPUT };
    }

    if (dhtFitted) {
        used[count++] = { dhtPin, "dht", ROLE_DIGITAL };
    }
    for (unsigned i = 0; i < moistureCount; ++i) {
        snprintf(probeLabel[i], sizeof(probeLabel[i]), "moisture%u", i);
        used[count++] = { soilMoisturePin[i], probeLabel[i], ROLE_ANALOG };

        // The probe's power pin DRIVES, so it needs the output rules, not the
        // analog ones — and it goes through the same duplicate check as
        // everything else. Two probes sharing one power pin is normal and
        // deliberate (one MOSFET, both sensors), so a repeat is only reported
        // when the OTHER user is not another probe's power pin.
        if (soilMoisturePowerPin[i] != kNoPin) {
            bool shared = false;
            for (unsigned j = 0; j < i; ++j) {
                if (soilMoisturePowerPin[j] == soilMoisturePowerPin[i]) {
                    shared = true;
                    break;
                }
            }
            if (!shared) {
                snprintf(powerLabel[i], sizeof(powerLabel[i]),
                         "moisture%uPower", i);
                used[count++] = { soilMoisturePowerPin[i], powerLabel[i],
                                  ROLE_OUTPUT };
            }
        }
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

        // A sensor declared without a `pin` key, on a chip whose compiled
        // default for that kind is kNoPin — the S3 carrier's DHT, because that
        // board has no DHT header at all. Named as the missing assignment it
        // is: reporting it as "GPIO 255 is not bonded out" sends the reader
        // looking for a pin nobody chose. Relays never reach here; the loop
        // above skips them, because for a relay kNoPin is a valid state.
        if (pin == kNoPin) {
            logger.error(String(owner) +
                         " is declared with no pin, and this build has no "
                         "default pin for it on this chip. Give it a pin in "
                         "config.json, or remove the key.");
            continue;
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
                           ") carries the console link; the boot log — and on "
                           "an S3 the USB recovery path — is lost.");
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
