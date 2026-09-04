#pragma once

// The pin capability predicates on their own, so that ConfigFile::validatePins(),
// the save-time check behind POST /config.json and /capabilities.json all read
// the same rules.
//
// The rules themselves live in core/pin_rules.h, which is Arduino-free and
// carries one namespace per MCU family. This header is the seam: config_pins.cpp
// picks the family for the chip being compiled for, and every caller in the
// firmware keeps calling these six names.

#include <Arduino.h>

// Pin capability rules, shared by validatePins(), the save-time check in
// handleConfigPost and /capabilities.json — so the web UI cannot offer a pin
// the firmware would reject.
bool pinIsADC1(uint8_t pin);
bool pinIsInputOnly(uint8_t pin);
bool pinIsFlash(uint8_t pin);
// False for pins the SoC numbers but this module/carrier does not bring out.
bool pinIsBonded(uint8_t pin);
// The console link: usable, but taking it costs the boot log — and on the S3,
// the USB-Serial-JTAG recovery path — that diagnoses the mistake that took it.
bool pinIsSerialConsole(uint8_t pin);
bool pinIsStrapping(uint8_t pin);

// The highest GPIO number this chip has: 39 on a WROOM-32, 48 on an S3. Three
// callers need it and each one used to spell 39 into its own source —
// /capabilities.json's walk, the save-time plausibility check, and the probe
// power-pin clamp. A hardcoded 39 on an S3 silently refuses GPIO 40-48, which
// is where its UART and half its spare pins live.
uint8_t pinMaxGpio();
