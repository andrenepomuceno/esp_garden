// The I2C transport for the SHT4x. Everything that can be decided without a
// bus lives in core/sht4x_protocol.h; this file is the part that cannot.
//
// Hand-rolled rather than taken from a library, and the reasoning is in
// CLAUDE.md: the whole protocol is one command byte, a delay and six bytes with
// two CRC8s, and the half of it worth checking — the CRC and the two
// conversions — is pure arithmetic a host test can reach. A library moves that
// arithmetic out of reach and brings a second unpinned dependency surface with
// it, which this repo has already been bitten by once.

#include "core/sht4x.h"
#include "core/logger.h"
#include <Wire.h>

static uint8_t g_address = sht4x::kDefaultAddress;
static bool g_ready = false;

bool
sht4xBegin(uint8_t sda, uint8_t scl, uint32_t hz, uint8_t address)
{
    g_ready = false;
    g_address = address;

    if (!Wire.begin((int)sda, (int)scl, hz)) {
        logger.error("I2C would not start on SDA " + String(sda) + " / SCL " +
                     String(scl) + ".");
        return false;
    }

    // A soft reset rather than a bare address probe. It is the cheapest
    // transaction that BOTH proves something is answering at this address and
    // leaves the part in a known state — which matters after a watchdog reset,
    // where the ESP32 restarts and the SHT40 does not.
    Wire.beginTransmission(g_address);
    Wire.write((uint8_t)sht4x::kCmdSoftReset);
    if (Wire.endTransmission() != 0) {
        // Named at boot, in the log, rather than left to show up as an error
        // rate climbing from nothing. The common causes are all installation
        // faults: the wrong address for a B- or C-suffix part, SDA and SCL
        // swapped, or no pull-ups on a bus whose pull-ups are on the board.
        logger.error("No I2C device acknowledged address 0x" +
                     String(g_address, HEX) + " on SDA " + String(sda) +
                     " / SCL " + String(scl) +
                     ". Temperature and humidity will not be read.");
        return false;
    }
    delay(sht4x::kSoftResetMaxMs);

    g_ready = true;
    return true;
}

sht4x::Status
sht4xRead(float& celsius, float& humidityPct)
{
    if (!g_ready) {
        return sht4x::kNotReady;
    }

    Wire.beginTransmission(g_address);
    Wire.write((uint8_t)sht4x::kCmdMeasureHigh);
    if (Wire.endTransmission() != 0) {
        return sht4x::kNoAck;
    }

    // The blocking half, and the only one. ~10 ms on the 1 Hz ambient task,
    // which is the same cooperative pump the DHT task used and a quarter of
    // what the DHT11's own bit-banged frame costs — that driver holds
    // interrupts off for 20-27 ms, where this is a delay() that yields.
    //
    // A two-phase version (command on tick N, read on tick N+1) would remove
    // even this. It was not written: it buys 1 % of one background tick at the
    // price of a state machine and a second of latency on a quantity that moves
    // in minutes, and the probe settle delay already accepts up to 250 ms on
    // this same task.
    delay(sht4x::kMeasureMaxMs);

    uint8_t frame[sht4x::kFrameBytes];
    if (Wire.requestFrom((int)g_address, (int)sht4x::kFrameBytes) !=
        (int)sht4x::kFrameBytes) {
        return sht4x::kShortRead;
    }
    for (unsigned i = 0; i < sht4x::kFrameBytes; ++i) {
        frame[i] = (uint8_t)Wire.read();
    }

    return sht4x::decodeFrame(frame, celsius, humidityPct);
}
