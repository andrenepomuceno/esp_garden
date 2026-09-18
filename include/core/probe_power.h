#pragma once

#include <stdint.h>

// Who owns a probe power pin, when several probes share one.
//
// The normal wiring is one MOSFET for the whole bank — all four probes on the
// esp-garden-hardware carrier name GPIO 14 — so every question about that pin
// is a question about the SET of probes naming it, never about one of them.
// The settle delay was already coalesced for that reason; `powerAlways` is the
// second rule that has to be, and it is the one that is easy to get wrong:
// answered per probe, the last index in the loop wins and writes its own
// verdict over everyone else's.
namespace probe_power {

// Must match ConfigFile::kNoPin; a static_assert in sensors.cpp holds them
// together. Spelled here rather than included, because reaching for config.h
// would drag Arduino in behind it.
static const uint8_t kNoPin = 255;

struct Probe
{
    uint8_t powerPin;  ///< kNoPin when this probe drives no power pin
    uint16_t settleMs;
    bool always;       ///< io.soilMoisture[i].powerAlways
};

// Does any probe naming this pin ask for it to stay energised?
//
// ANY, not all: the operator wires one FET and plugs in one probe, so
// requiring the flag on all four would mean the feature is off until it is set
// on three probes that are not connected. A shared pin cannot be always-on for
// one probe and gated for another anyway.
inline bool
pinIsAlwaysOn(const Probe* probes, unsigned count, uint8_t pin)
{
    if (pin == kNoPin) {
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (probes[i].powerPin == pin && probes[i].always) {
            return true;
        }
    }
    return false;
}

// Should this pin be de-energised once the conversions are done?
inline bool
pinPowersDown(const Probe* probes, unsigned count, uint8_t pin)
{
    return pin != kNoPin && !pinIsAlwaysOn(probes, count, pin);
}

// The settle this tick owes: the longest any probe asked for, among the pins
// that actually transitioned off to on. A pin held up since sensorsSetup() has
// nothing to settle, and this delay runs inside the 1 Hz io task — the same
// cooperative pump MQTT and the /data.json cache share — so the carrier's
// 50 ms would otherwise be spent every second waiting on a rail that did not
// move. Zero when every powered pin is always-on.
inline uint16_t
settleMsForTick(const Probe* probes, unsigned count)
{
    uint16_t settle = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (probes[i].powerPin == kNoPin ||
            pinIsAlwaysOn(probes, count, probes[i].powerPin)) {
            continue;
        }
        if (probes[i].settleMs > settle) {
            settle = probes[i].settleMs;
        }
    }
    return settle;
}

} // namespace probe_power
