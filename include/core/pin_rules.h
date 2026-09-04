#pragma once

// What a GPIO can do, per MCU family, with no Arduino and no config in scope.
//
// This header exists so the rules can be TESTED. A wrong predicate here does
// not fail loudly: it accepts a pin that cannot work — a relay that never
// switches, an analog channel that reads noise — or it refuses one that can,
// and the only symptom is a page of boot errors about correct assignments.
// `test_pin_rules` compiles both families at once and pins every answer,
// including the ESP32-WROOM-32 answers that five shipped boards depend on.
//
// Both families are always compiled. `config_pins.cpp` picks one and forwards
// the six free functions the rest of the firmware calls; nothing else in the
// tree names a family.

#include <stdint.h>

namespace pin_rules {

// ---------------------------------------------------------------------------
// ESP32-WROOM-32 — espgarden1..5. These answers are load-bearing for a live
// garden; they are reproduced from the original config_pins.cpp unchanged.
// ---------------------------------------------------------------------------
namespace wroom32 {

enum : uint8_t
{
    kMaxGpio = 39
};

// ADC2 cannot be read while WiFi is associated, so every analog channel has to
// be ADC1. GPIO 37 and 38 are ADC1 on the die but are not bonded out on a
// WROOM-32 module, which leaves six usable channels.
inline bool
isADC1(uint8_t pin)
{
    return (pin >= 32 && pin <= 39) && pin != 37 && pin != 38;
}

// 34-39 have no output driver and no internal pull-up: fine for analog,
// useless for a relay, a DHT or a switch that needs one.
inline bool
isInputOnly(uint8_t pin)
{
    return pin >= 34 && pin <= 39;
}

// GPIO 20, 24 and 28-31 exist in the SoC's numbering but are not brought out
// on an ESP32-WROOM-32 module; 37 and 38 are the same story on the analog
// side. Offering one in a picker is offering a pin that can never be wired to
// anything, and the symptom is a sensor that reads nothing with no error.
inline bool
isBonded(uint8_t pin)
{
    if (pin == 20 || pin == 24 || (pin >= 28 && pin <= 31)) {
        return false;
    }
    return pin <= kMaxGpio && pin != 37 && pin != 38;
}

// UART0. The logger writes the whole boot sequence here at 115200, and it is
// the only way to see a device whose config did not load — which is precisely
// the situation a bad pin assignment creates.
inline bool
isSerialConsole(uint8_t pin)
{
    return pin == 1 || pin == 3;
}

// Wired to the SPI flash; touching one hangs the chip.
inline bool
isFlash(uint8_t pin)
{
    return pin >= 6 && pin <= 11;
}

// Sampled at reset to choose the boot mode. Usable, but a pull on one can stop
// the board booting, so it is worth saying out loud.
inline bool
isStrapping(uint8_t pin)
{
    return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15;
}

} // namespace wroom32

// ---------------------------------------------------------------------------
// ESP32-S3 — an ESP32-S3-WROOM-1 on an ESP32-S3-DevKitC-1, socketed on the
// esp-garden-hardware carrier.
//
// Every number below was read out of the ESP-IDF headers that ship with
// framework-arduinoespressif32, or off Espressif's own GPIO page, and NOT from
// the hardware repo's summary. The sources are named per predicate.
// ---------------------------------------------------------------------------
namespace esp32s3 {

// soc/soc_caps.h: SOC_GPIO_PIN_COUNT 49, so the last GPIO is 48.
enum : uint8_t
{
    kMaxGpio = 48
};

// soc/adc_channel.h: ADC1_CHANNEL_0..9 are GPIO 1..10; ADC2 is GPIO 11..20.
// The ADC2/WiFi conflict is the same silicon limitation the ESP32 has, so the
// rule is the same rule pointed at different pins.
inline bool
isADC1(uint8_t pin)
{
    return pin >= 1 && pin <= 10;
}

// There is no such class on this part. soc_caps.h says so twice —
// "// No GPIO is input only" above
// SOC_GPIO_VALID_OUTPUT_GPIO_MASK == SOC_GPIO_VALID_GPIO_MASK — and every
// entry of gpio_types.h's ESP32S3 enum reads "input and output", where the
// ESP32 enum marks 34-39 "input mode only" and the S2 enum marks GPIO46.
//
// The WROOM-32 predicate conflates "no output driver" with "no internal
// pull-up" because on that part the same six pins have both properties. On the
// S3 neither property has a pin, so one answer covers both callers.
inline bool
isInputOnly(uint8_t)
{
    return false;
}

// Two separate reasons, both meaning "cannot be wired to anything here":
//
//  - GPIO 22-25 do not exist. soc_caps.h clears bits 22-25 out of
//    SOC_GPIO_VALID_GPIO_MASK, and gpio_types.h's ESP32S3 enum jumps 21 -> 26.
//  - GPIO 33 and 34 exist but are not on the ESP32-S3-DevKitC-1's headers.
//    (35-37 ARE on the headers and are refused by isFlash instead, because the
//    N8R8 module this board is specified with uses them for octal PSRAM.)
inline bool
isBonded(uint8_t pin)
{
    if (pin >= 22 && pin <= 25) {
        return false;
    }
    if (pin == 33 || pin == 34) {
        return false;
    }
    return pin <= kMaxGpio;
}

// Both links you would need to diagnose a device whose config did not load,
// which is what this predicate has always meant:
//
//  - GPIO 43/44 are U0TXD/U0RXD. io_mux_reg.h maps IO_MUX_GPIO43_REG to
//    PERIPHS_IO_MUX_U0TXD_U and GPIO44 to U0RXD.
//  - GPIO 19/20 are USB_DM/USB_DP (io_mux_reg.h USB_DM_GPIO_NUM 19,
//    USB_DP_GPIO_NUM 20). They carry USB-Serial-JTAG, which on this board is
//    the console, the debugger AND the ROM download mode that recovers a brick.
//    Espressif: "GPIO 19 and 20 are used by USB-JTAG by default. In order to
//    use them as GPIOs, USB-JTAG will be disabled by the drivers." So they
//    work, and taking one costs the recovery path — a warning, not a refusal,
//    exactly as UART0 is on the WROOM-32.
inline bool
isSerialConsole(uint8_t pin)
{
    return pin == 43 || pin == 44 || pin == 19 || pin == 20;
}

// io_mux_reg.h: SPI_CS1 26, SPI_HD 27, SPI_WP 28, SPI_CS0 29, SPI_CLK 30,
// SPI_Q 31, SPI_D 32, SPI_D4 33, SPI_D5 34, SPI_D6 35, SPI_D7 36, SPI_DQS 37.
// Espressif: "GPIO26-32 are usually used for SPI flash and PSRAM and not
// recommended for other uses. When using Octal Flash or Octal PSRAM or both,
// GPIO33~37 are connected to SPIIO4 ~ SPIIO7 and SPIDQS."
//
// 33-37 are refused DELIBERATELY rather than conditionally. This board is
// specified with an ESP32-S3-DevKitC-1-N8R8, whose PSRAM is octal, so on the
// hardware that exists those five pins are taken; the hardware repo declares
// 35-37 unconnected for the same reason. On a quad-PSRAM module this refuses
// three pins that would work, which is the safe direction to be wrong in: the
// other one drives the PSRAM bus.
inline bool
isFlash(uint8_t pin)
{
    return pin >= 26 && pin <= 37;
}

// Espressif's ESP32-S3 GPIO page: "GPIO0, GPIO3, GPIO45 and GPIO46 are
// strapping pins." GPIO46 is corroborated in the framework's own
// esp_rom/esp32s3/rom/efuse.h, which describes ROM UART printing as selected
// by "GPIO46 ... low when digital reset".
inline bool
isStrapping(uint8_t pin)
{
    return pin == 0 || pin == 3 || pin == 45 || pin == 46;
}

} // namespace esp32s3

} // namespace pin_rules
