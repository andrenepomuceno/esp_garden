#pragma once

// The pin capability predicates on their own, so that ConfigFile::validatePins(),
// the save-time check behind POST /config.json and /capabilities.json all read
// the same rules.

#include <Arduino.h>

// Pin capability rules, shared by validatePins(), the save-time check in
// handleConfigPost and /capabilities.json — so the web UI cannot offer a pin
// the firmware would reject.
bool pinIsADC1(uint8_t pin);
bool pinIsInputOnly(uint8_t pin);
bool pinIsFlash(uint8_t pin);
// False for pins the SoC numbers but the WROOM-32 module does not bring out.
bool pinIsBonded(uint8_t pin);
// UART0: usable, but taking it costs the boot log that diagnoses the mistake.
bool pinIsSerialConsole(uint8_t pin);
bool pinIsStrapping(uint8_t pin);
