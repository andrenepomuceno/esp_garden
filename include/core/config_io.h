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
