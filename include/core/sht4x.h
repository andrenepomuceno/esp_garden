#pragma once

// The transport half of the SHT4x driver: the I2C bus and nothing else. Every
// byte it exchanges is interpreted by core/sht4x_protocol.h, which is
// Arduino-free and host-tested — so the part of this that can be checked from a
// workstation is checked there, and what is left here is the part that can only
// be checked by putting a sensor on a bus.
//
// NOTHING HERE HAS RUN. No SHT40 exists to read from; the carrier that carries
// one has not been built. See CLAUDE.md's unverified list.

#include "core/sht4x_protocol.h"
#include <Arduino.h>

// Brings the bus up on the configured pins and confirms something ACKs at
// `address`. Returns false — loudly, in the log — when it does not, which
// leaves sht4xRead() answering kNotReady rather than producing a silent stream
// of failed frames.
//
// The bus is started HERE and only here, and only when an I2C device is
// declared. io.i2c carries the bus's parameters and never its presence: a bus
// with nothing on it is not a peripheral, and driving two GPIOs on a board that
// has no I2C part would be exactly the "presence is the key" rule broken from
// the other side.
bool
sht4xBegin(uint8_t sda, uint8_t scl, uint32_t hz, uint8_t address);

// One high-repeatability measurement. Blocks for about 10 ms, almost all of it
// the datasheet's measurement time; see kMeasureMaxMs for why that is the
// command chosen. Both outputs are left untouched unless the return is
// sht4x::kOk.
sht4x::Status
sht4xRead(float& celsius, float& humidityPct);
