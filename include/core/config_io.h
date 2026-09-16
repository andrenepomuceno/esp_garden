#pragma once

// The `io` block parsers, called only from ConfigFile::loadFile(). They live
// apart from it because every entry accepts several shapes, and that
// compatibility logic belongs in one place rather than spread through the load.

#include "core/config.h"
#include <Arduino_JSON.h>

// Fills cfg.relayCount / relayPin / relayPinOn / relayName from `io`.
void loadRelays(ConfigFile& cfg, JSONVar& io);

// Reads one sensor entry — a bare pin number or {pin, name} — into pin/name.
void loadSensor(JSONVar node, uint8_t& pin, String& name);

// Fills cfg.moistureCount and the per-probe pin and label arrays from `io`.
void loadSoilMoisture(ConfigFile& cfg, JSONVar& io);

// Reads `io.i2c` — {sda, scl, hz} — into the bus fields. Every key is optional
// and a missing one keeps the family default. It sets NO fitted flag: a bus is
// not a peripheral, and whether it comes up is decided by the devices on it.
void loadI2c(ConfigFile& cfg, JSONVar& io);

// Reads `io.sht4x` — {name, address} — and sets cfg.sht4xFitted from whether
// the key exists at all, the same "presence is the key" rule every other sensor
// follows. It takes no pin: the part is on the bus above.
void loadSht4x(ConfigFile& cfg, JSONVar& io);
