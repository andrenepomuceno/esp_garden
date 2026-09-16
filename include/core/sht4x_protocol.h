#pragma once

// The SHT4x wire protocol, with no Arduino and no Wire in it: the command
// bytes, the CRC that guards every frame, the two conversions that turn raw
// codes into physical units, and the plausibility verdict.
//
// It is a separate header from src/sht4x.cpp for the reason segment_index.h,
// step_publisher.h and pin_rules.h are separate: this is the half a host test
// can reach. Stubbing <Wire.h> instead would have produced a test that proves
// the stub agrees with itself — small enough to look obviously right, and
// wrong in a way that yields plausible answers rather than failures.
//
// What IS evidence here: the CRC against Sensirion's own published check value,
// and the conversions against the endpoints their own formulae define. What is
// NOT: that a real SHT40 answers at 0x44, that it ACKs 0xFD, or that 8.2 ms is
// long enough. Those are read off the datasheet and no part has been on a bus.

#include <stdint.h>

namespace sht4x {

// I2C addresses the SHT4x family ships with. 0x44 is the SHT40-AD1B, which is
// what the esp-garden-hardware carrier fits as U3; the B and C suffixes answer
// one and two addresses up. Bounded rather than free so that a typo lands as a
// refused config instead of a bus scan that finds nothing.
enum : uint8_t
{
    kDefaultAddress = 0x44,
    kMinAddress = 0x44,
    kMaxAddress = 0x46,
};

// Commands. Only the first three are used; the serial-number read is named
// because it is the one way to tell "no part" from "a part that is not an
// SHT4x", and a future caller should not have to re-read the datasheet.
enum : uint8_t
{
    kCmdMeasureHigh = 0xFD, // T and RH, high repeatability
    kCmdSoftReset = 0x94,
    kCmdReadSerial = 0x89,
};

// How long to wait after the command before reading the six bytes.
//
// The datasheet's MAXIMUM for high repeatability is 8.2 ms (typical 6.9); this
// rounds up to a whole millisecond because the delay is taken with delay(),
// whose resolution is 1 ms. Reading early does not corrupt the frame -- the
// part simply NACKs -- but it costs a whole tick to find out, so the margin is
// cheaper than the retry.
//
// High repeatability and not medium (4.5 ms) or low (1.7 ms) deliberately: the
// +-1.8 %RH that makes this part worth an I2C driver at all is the
// high-repeatability figure, and 10 ms of a 1000 ms background tick is 1 %.
enum : uint16_t
{
    kMeasureMaxMs = 10,
    kSoftResetMaxMs = 2, // datasheet 1 ms, rounded up for the same reason
};

// Bytes in one measurement frame: T_MSB, T_LSB, CRC, RH_MSB, RH_LSB, CRC.
enum : uint8_t
{
    kFrameBytes = 6
};

// What a read attempt produced. Bus-level and frame-level outcomes share one
// enum because every one of them ends the same way -- no sample, one error
// counted -- and a caller that had to combine two enums would be a caller that
// could forget one of them.
//
// decodeFrame() only ever returns the frame-level half; kNoAck, kShortRead and
// kNotReady come from the transport in src/sht4x.cpp.
enum Status
{
    kOk = 0,
    kNotReady,               // begin() never succeeded
    kNoAck,                  // nothing acknowledged the address
    kShortRead,              // fewer than six bytes came back
    kCrcTemperature,         // the frame is corrupt in its first half
    kCrcHumidity,            // ... or its second
    kTemperatureImplausible, // CRC passed and the number still cannot be real
    kHumidityImplausible,
};

// The plausibility gate, and it is deliberately NOT the datasheet's rated
// accuracy range.
//
// This repo has made that mistake once, on the DHT: the gate used the part's
// rated 20..90 %RH, and this garden is in Brasilia, where the dry season takes
// the afternoon below 20 % routinely. 261 genuine archive readings would have
// been discarded on exactly the afternoons a garden most needs them. A rated
// range describes ACCURACY -- outside it a reading is less trustworthy, not
// impossible -- so the bounds here say only "the sensor cannot have measured
// this" and nothing else.
//
// TEMPERATURE: the part's full OPERATING range, -40..125 C. The conversion
// -45 + 175*S/65535 spans -45..130 exactly, so this rejects a 5 C sliver at
// each rail and nothing in between. No air and no sun-baked enclosure reaches
// either: the hottest reading in this project's whole archive is 45.04 C, from
// a DHT in 38 minutes of direct sun, and it is a real measurement that this
// gate keeps. What it does catch is a code pinned at a rail by a damaged part
// or by a bus artefact that happened to pass CRC.
//
// HUMIDITY is gated BEFORE the clip, at -5..105 %. The conversion
// -6 + 125*S/65535 spans -6..119 on purpose: Sensirion scales past the physical
// range so that a part a little out of tolerance still reports slightly beyond
// 0 or 100 rather than saturating, and their datasheet then instructs clipping
// to 0..100. So the honest order is gate wide on what the PART can report, then
// clip narrow on what the QUANTITY can be. +-5 points is generous room for a
// part drifting out of its +-1.8 % calibration while still measuring; beyond
// it the code is near a rail.
//
// The clip is not the DHT mistake repeated. That one DISCARDED real readings
// at 15 %RH. This accepts the reading and rounds a 100.4 that relative humidity
// cannot exceed at an instrument -- a bound from the definition of the
// quantity, not from a datasheet's confidence in it.
constexpr float kMinTempC = -40.0f;
constexpr float kMaxTempC = 125.0f;
constexpr float kMinHumidityPct = -5.0f;
constexpr float kMaxHumidityPct = 105.0f;

// CRC-8 as the SHT4x defines it: polynomial 0x31 (x^8 + x^5 + x^4 + 1),
// initialisation 0xFF, no reflection, no final XOR. Sensirion publish
// crc8(0xBE, 0xEF) == 0x92 as the check value, and test_sht4x asserts it.
//
// This is the part that makes the SHT40 a different proposition from the DHT11:
// every frame carries TWO independent CRCs over two bytes each, where the DHT's
// single 8-bit sum over four bytes is what let a mistimed frame through and put
// an impossible 15.1 %RH into a stored average.
inline uint8_t
crc8(const uint8_t* data, unsigned length)
{
    uint8_t crc = 0xFF;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// S_T -> degrees Celsius. Exact at both ends by construction: 0x0000 is
// -45.00 and 0xFFFF is +130.00.
inline float
rawToCelsius(uint16_t raw)
{
    return -45.0f + 175.0f * ((float)raw / 65535.0f);
}

// S_RH -> per cent, BEFORE the clip. Spans -6.00 to +119.00, which is why the
// gate above runs on this value and the clip below runs after it.
inline float
rawToHumidityPct(uint16_t raw)
{
    return -6.0f + 125.0f * ((float)raw / 65535.0f);
}

inline float
clipHumidityPct(float pct)
{
    if (pct < 0.0f) {
        return 0.0f;
    }
    if (pct > 100.0f) {
        return 100.0f;
    }
    return pct;
}

inline bool
addressIsValid(int address)
{
    return address >= (int)kMinAddress && address <= (int)kMaxAddress;
}

// Six bytes in, two physical readings out, or the reason there are none.
//
// Both CRCs are checked BEFORE either value is converted, and the temperature
// half is checked first so that a frame corrupt in both halves is reported by
// the earlier fault rather than by whichever branch happened to be written
// first. Neither output is touched unless the whole frame is good: a caller
// that ignored the return value would otherwise average a half-decoded frame.
inline Status
decodeFrame(const uint8_t* frame, float& celsius, float& humidityPct)
{
    if (crc8(frame, 2) != frame[2]) {
        return kCrcTemperature;
    }
    if (crc8(frame + 3, 2) != frame[5]) {
        return kCrcHumidity;
    }

    const uint16_t rawT = (uint16_t)((uint16_t)frame[0] << 8 | frame[1]);
    const uint16_t rawH = (uint16_t)((uint16_t)frame[3] << 8 | frame[4]);

    const float t = rawToCelsius(rawT);
    const float h = rawToHumidityPct(rawH);

    if (t < kMinTempC || t > kMaxTempC) {
        return kTemperatureImplausible;
    }
    if (h < kMinHumidityPct || h > kMaxHumidityPct) {
        return kHumidityImplausible;
    }

    celsius = t;
    humidityPct = clipHumidityPct(h);
    return kOk;
}

// For the log line. A read that fails silently is a sensor that looks dead,
// and this repo has already paid for one counter that said the opposite of
// what was happening.
inline const char*
statusName(Status status)
{
    switch (status) {
        case kOk:
            return "ok";
        case kNotReady:
            return "bus not started";
        case kNoAck:
            return "no ACK from the sensor";
        case kShortRead:
            return "short read";
        case kCrcTemperature:
            return "CRC failed on the temperature word";
        case kCrcHumidity:
            return "CRC failed on the humidity word";
        case kTemperatureImplausible:
            return "temperature outside what the part can measure";
        case kHumidityImplausible:
            return "humidity outside what the part can measure";
    }
    return "unknown";
}

} // namespace sht4x
