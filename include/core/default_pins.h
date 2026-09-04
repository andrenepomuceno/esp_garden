#pragma once

// The COMPILED pin defaults, per MCU family — the map a board uses before, and
// wherever, `config.json` does not say otherwise.
//
// These are not cosmetic. Three separate paths reach them:
//
//  - `relayPinsSafeInit()` runs over the relay table as the FIRST statement of
//    setup(), about 1.5 s before config.json is read, so it decides which
//    GPIOs a board holds as outputs during its own boot.
//  - `loadSensor()` only overwrites a pin when the entry carries a `"pin"`
//    key, so `"flow": {"name": "Fluxo"}` keeps whatever is in this table — and
//    `sensorsSetup()` then runs `pinMode()`/`attachInterrupt()` on it.
//  - `documentPinsAreUsable()` skips an entry with no pin key, and
//    `validatePins()` only LOGS, so nothing refuses either of the above.
//
// A table from the wrong chip therefore does not merely miss a peripheral: on
// an ESP32-S3 the WROOM-32 flow and float defaults (27 and 26) are SPI_HD and
// SPI_CS1 — the octal flash bus — and the DHT default (23) is a GPIO the part
// does not have. Selected the same way core/pin_rules.h is, from
// CONFIG_IDF_TARGET_*, which the framework sets from the env's `board` line.
//
// This header is Arduino-free and config-free, with BOTH families always
// compiled, so `test_pin_rules` can hold every default against the rules of
// the family it belongs to — the same reason pin_rules.h and segment_index.h
// exist. Nothing here reads a chip register; it is a table of numbers and the
// test is what says they are the right ones.

#include <stdint.h>

namespace default_pins {

// Mirrors ConfigFile::kNoPin. Spelled again rather than included, because
// including config.h would drag Arduino in and take this header out of the
// host test; config.cpp carries a static_assert holding the two together.
enum : uint8_t
{
    kNoPin = 255
};

// ---------------------------------------------------------------------------
// ESP32-WROOM-32 — espgarden1..5. Every number here is what shipped; changing
// one changes what a live garden parks at boot.
// ---------------------------------------------------------------------------
namespace wroom32 {

// Relay 0 keeps GPIO 15 so boards already in the field are unaffected. It is a
// strapping pin (MTDO) — new hardware should override it in config.json.
// Slots past this table get kNoPin, which is what makes an undeclared relay
// unstartable rather than merely unparked.
constexpr uint8_t relay[] = { 15, 16, 17, 18 };

// All ADC1: ADC2 cannot be read while WiFi is associated, so every analog
// channel has to come from GPIO 32-39.
//
// Index 1 is 34 because that is where espgarden5 — the only board that takes a
// second probe on compiled defaults — has it wired, and that board has no water
// level sensor to contend with. A board carrying BOTH a water level sensor and
// a second probe must assign pins in config.json; validatePins() logs the
// collision if it does not. (Briefly changed to 35 to dodge that collision,
// which silently moved espgarden5's probe onto a floating pin.)
//
// Index 3 is 33 — the sixth and last ADC1 channel, and the one espgarden1
// leaves free. It used to be absent, so a fourth declared probe with no pin
// fell through to `A0`, which is GPIO 36 and is already probe 0: two probes on
// one pin, reported by validatePins() as a conflict on a board whose own
// hardware has a free channel for it.
//
// GPIO 32/33 double as XTAL_32K_P/N. The ESP32-WROOM-32 on a NodeMCU-32S ships
// without that crystal, so they are ordinary ADC1 inputs; a module that does
// have one fitted cannot use them.
constexpr uint8_t soilMoisture[] = { 36, 34, 32, 33 };

// Output-capable with an internal pull-up. GPIO 34-39 are neither.
constexpr uint8_t dht = 23;

// Was written as the Arduino aliases A3 and A6, which resolve per variant and
// so LOOK chip-relative. They are not the same thing as a per-family table:
// on the esp32s3 variant A3 is GPIO 4, which is this firmware's third soil
// probe, and the carrier's LDR is on GPIO 6. Same values as A3/A6 here.
constexpr uint8_t luminosity = 39;
constexpr uint8_t waterLevel = 34;

// Both need an internal pull-up, which GPIO 34-39 do not have, and both avoid
// the strapping pins and the flash pins.
constexpr uint8_t flow = 27;
constexpr uint8_t floatSwitch = 26;

} // namespace wroom32

// ---------------------------------------------------------------------------
// ESP32-S3 — the esp-garden-hardware carrier, an ESP32-S3-DevKitC-1-N8R8 in a
// socket. Every number is the carrier's own net, and every one of them is held
// against pin_rules::esp32s3 by test_pin_rules.
// ---------------------------------------------------------------------------
namespace esp32s3 {

// RELAY1..4. The WROOM-32 table's 15/16/18 are this board's FLOW_PULSE,
// FLOAT_SW and BTN_USER, and two of those three are switched to ground by the
// field hardware — SW150 shorts BTN_USER, and Q400 pulls FLOW_PULSE down on
// every flow pulse. Driving them as push-pull outputs for the length of a boot
// is a short, not a mis-assignment.
//
// Parking these EARLY is no longer what keeps the pumps off: this board has no
// driver stage, so at reset the GPIOs are inputs, the relay module's own
// pull-up holds IN high and `on: 0` means every relay is released. The table
// now earns its place by keeping setup()'s first statement off three inputs.
constexpr uint8_t relay[] = { 10, 11, 12, 13 };

// ADC1 on an S3 is GPIO 1-10 (soc/adc_channel.h), not 32-39. SOIL1..4 are 1,
// 2, 4 and 5. SOIL4 used to be missing from this table, so a fourth declared
// probe with no pin fell through to `A0` — GPIO 1 on the esp32s3 variant, and
// already SOIL1.
constexpr uint8_t soilMoisture[] = { 1, 2, 4, 5 };

// There is NO DHT on this carrier: the DHT22 header was dropped in favour of
// an SHT40 on I2C, which this firmware has no driver and no sensor KIND for.
// So there is no honest default pin, and kNoPin says exactly that — a config
// declaring `"dht"` with no pin gets an error naming the sensor. It used to
// inherit the WROOM-32's GPIO 23, which this part does not bring out at all
// (soc_caps.h clears 22-25), so the diagnosis was a pin number nobody chose.
constexpr uint8_t dht = kNoPin;

// LDR on GPIO 6, AUX_ADC on GPIO 7. The aux header is deliberately NOT
// declared in the carrier's config — presence is the key, and pushing whatever
// sits on a spare analog header through the water-level curve would report it
// as a level — but the default has to be a real ADC1 channel for the day
// somebody does declare it.
constexpr uint8_t luminosity = 6;
constexpr uint8_t waterLevel = 7;

// FLOW_PULSE and FLOAT_SW. The WROOM-32 values (27 and 26) are SPI_HD and
// SPI_CS1 on this part: pinMode() and attachInterrupt() on the octal flash
// bus, reached by nothing louder than a log line.
constexpr uint8_t flow = 15;
constexpr uint8_t floatSwitch = 16;

} // namespace esp32s3

} // namespace default_pins
