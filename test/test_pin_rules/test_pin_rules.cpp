#include "BuildConfig.h"
#include "core/default_pins.h"
#include "core/pin_rules.h"
#include <stdio.h>
#include <unity.h>

// What a GPIO can do, per MCU family.
//
// A wrong answer in here does not crash and does not fail a build. It accepts
// a pin that cannot work — a relay that never switches, an analog channel that
// reads noise — or it refuses one that can, and either way the only symptom is
// a page of boot errors about correct assignments and silence about the wrong
// ones. Nothing downstream can catch that, so it is caught here.
//
// The ESP32-WROOM-32 half is a REGRESSION GUARD, not a description: five envs
// and one live garden run on those answers, and this suite exists so that
// adding a second family cannot move them by a single pin.
//
// Every ESP32-S3 answer below was taken from the ESP-IDF headers that ship
// with framework-arduinoespressif32, or from Espressif's own GPIO page:
//
//   ADC1 = GPIO 1-10          soc/adc_channel.h (ADC1_CHANNEL_0..9)
//   flash/PSRAM = 26-37       soc/io_mux_reg.h (SPI_CS1..SPI_DQS)
//   no input-only pins        soc/soc_caps.h "No GPIO is input only",
//                             SOC_GPIO_VALID_OUTPUT_GPIO_MASK ==
//                             SOC_GPIO_VALID_GPIO_MASK, and every entry of
//                             gpio_types.h's ESP32S3 enum
//   22-25 do not exist        SOC_GPIO_VALID_GPIO_MASK clears bits 22-25
//   last GPIO is 48           SOC_GPIO_PIN_COUNT 49
//   UART0 = 43/44             io_mux_reg.h U0TXD_U / U0RXD_U
//   USB = 19/20               io_mux_reg.h USB_DM_GPIO_NUM / USB_DP_GPIO_NUM
//   strapping = 0/3/45/46     Espressif ESP32-S3 GPIO documentation

namespace w = pin_rules::wroom32;
namespace s = pin_rules::esp32s3;
namespace dw = default_pins::wroom32;
namespace ds = default_pins::esp32s3;

// A predicate is checked over EVERY uint8_t, not over the pins somebody
// thought of. These take uint8_t and kNoPin is 255; a rule written as a bare
// range that happens to wrap would pass a hand-picked list and fail here.
typedef bool (*Predicate)(uint8_t);

static void
assertExactly(Predicate p, const uint8_t* expected, unsigned count,
              const char* what)
{
    for (unsigned pin = 0; pin <= 255; ++pin) {
        bool wanted = false;
        for (unsigned i = 0; i < count; ++i) {
            if (expected[i] == pin) {
                wanted = true;
                break;
            }
        }
        const bool got = p((uint8_t)pin);
        if (got != wanted) {
            char message[96];
            snprintf(message, sizeof(message), "%s: GPIO %u answered %s",
                     what, pin, got ? "true" : "false");
            TEST_FAIL_MESSAGE(message);
        }
    }
}

// ---------------------------------------------------------------------------
// ESP32-WROOM-32 — the answers five shipped envs already depend on
// ---------------------------------------------------------------------------

static void
test_wroom32_adc1_is_the_six_bonded_channels(void)
{
    // 37 and 38 are ADC1 on the die and are not brought out on the module.
    static const uint8_t expected[] = { 32, 33, 34, 35, 36, 39 };
    assertExactly(w::isADC1, expected, 6, "wroom32::isADC1");
}

static void
test_wroom32_input_only_is_34_to_39(void)
{
    static const uint8_t expected[] = { 34, 35, 36, 37, 38, 39 };
    assertExactly(w::isInputOnly, expected, 6, "wroom32::isInputOnly");
}

static void
test_wroom32_flash_is_6_to_11(void)
{
    static const uint8_t expected[] = { 6, 7, 8, 9, 10, 11 };
    assertExactly(w::isFlash, expected, 6, "wroom32::isFlash");
}

static void
test_wroom32_strapping_is_the_five_boot_mode_pins(void)
{
    static const uint8_t expected[] = { 0, 2, 5, 12, 15 };
    assertExactly(w::isStrapping, expected, 5, "wroom32::isStrapping");
}

static void
test_wroom32_serial_console_is_uart0(void)
{
    static const uint8_t expected[] = { 1, 3 };
    assertExactly(w::isSerialConsole, expected, 2, "wroom32::isSerialConsole");
}

static void
test_wroom32_bonded_omits_the_pins_the_module_does_not_bring_out(void)
{
    // 0-39 less 20, 24, 28-31 (SoC numbers with no module pad) and 37, 38.
    static const uint8_t expected[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39
    };
    assertExactly(w::isBonded, expected, 32, "wroom32::isBonded");
}

static void
test_wroom32_last_gpio_is_39(void)
{
    TEST_ASSERT_EQUAL_INT(39, (int)w::kMaxGpio);
}

// The pins data/config.template.json actually assigns, each against the rule
// its role is held to. A template and a predicate that disagree ship a board
// that logs errors about its own factory settings.
static void
test_the_wroom32_template_pins_pass_their_own_rules(void)
{
    const uint8_t relays[] = { 19, 16, 17, 18 };
    for (unsigned i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(w::isBonded(relays[i]));
        TEST_ASSERT_FALSE(w::isFlash(relays[i]));
        TEST_ASSERT_FALSE(w::isInputOnly(relays[i]));
    }

    const uint8_t analog[] = { 36, 35, 32, 39, 34 }; // 3 probes, LDR, level
    for (unsigned i = 0; i < 5; ++i) {
        TEST_ASSERT_TRUE(w::isADC1(analog[i]));
    }

    const uint8_t digital[] = { 23, 27, 26 }; // dht, flow, float
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(w::isBonded(digital[i]));
        TEST_ASSERT_FALSE(w::isInputOnly(digital[i]));
        TEST_ASSERT_FALSE(w::isFlash(digital[i]));
    }
}

// ---------------------------------------------------------------------------
// ESP32-S3
// ---------------------------------------------------------------------------

static void
test_esp32s3_adc1_is_gpio_1_to_10(void)
{
    static const uint8_t expected[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    assertExactly(s::isADC1, expected, 10, "esp32s3::isADC1");
}

// GPIO 11-20 are ADC2, and ADC2 is unreadable while WiFi is associated. That
// is the same silicon limitation the ESP32 has, aimed at different numbers,
// and it is the reason an analog channel may not land there.
static void
test_esp32s3_adc2_is_refused_like_the_esp32s(void)
{
    for (uint8_t pin = 11; pin <= 20; ++pin) {
        TEST_ASSERT_FALSE(s::isADC1(pin));
    }
}

// The single most copy-pasteable mistake in this file. On the ESP32, 34-39
// have no output driver AND no internal pull-up, and one predicate covers
// both. The S3 has no such pins at all, so the honest answer is false for
// every one — including 46, which IS input-only on the ESP32-S2 and is not on
// the S3.
static void
test_esp32s3_has_no_input_only_pins(void)
{
    for (unsigned pin = 0; pin <= 255; ++pin) {
        TEST_ASSERT_FALSE(s::isInputOnly((uint8_t)pin));
    }
}

static void
test_esp32s3_flash_and_psram_is_26_to_37(void)
{
    static const uint8_t expected[] = { 26, 27, 28, 29, 30, 31,
                                        32, 33, 34, 35, 36, 37 };
    assertExactly(s::isFlash, expected, 12, "esp32s3::isFlash");
}

// 33-37 are the octal Flash/PSRAM lines. They are refused unconditionally
// because the board is specified with an N8R8 devkit, whose PSRAM is octal —
// on that module they are taken. Refusing them on a quad-PSRAM module costs
// three usable pins; accepting them on this one drives the PSRAM bus.
static void
test_esp32s3_octal_psram_pins_are_refused(void)
{
    for (uint8_t pin = 33; pin <= 37; ++pin) {
        TEST_ASSERT_TRUE(s::isFlash(pin));
    }
    TEST_ASSERT_FALSE(s::isFlash(38));
    TEST_ASSERT_FALSE(s::isFlash(25));
}

static void
test_esp32s3_strapping_is_0_3_45_46(void)
{
    static const uint8_t expected[] = { 0, 3, 45, 46 };
    assertExactly(s::isStrapping, expected, 4, "esp32s3::isStrapping");
}

// The ESP32's strapping pins are 0/2/5/12/15. Every one of 2, 5, 12 and 15 is
// an ordinary GPIO on the S3, and three of them are relay outputs on this
// board's config — so carrying the ESP32 list across would have warned about
// four correct assignments and stayed silent about GPIO 45 and 46.
static void
test_esp32s3_does_not_inherit_the_esp32_strapping_pins(void)
{
    TEST_ASSERT_FALSE(s::isStrapping(2));
    TEST_ASSERT_FALSE(s::isStrapping(5));
    TEST_ASSERT_FALSE(s::isStrapping(12));
    TEST_ASSERT_FALSE(s::isStrapping(15));
}

// UART0 plus USB-Serial-JTAG. Both are links you would need to diagnose — or
// reflash — a device whose config did not load, which is what this predicate
// has always meant. Usable, and flagged, never refused.
static void
test_esp32s3_console_covers_uart0_and_usb(void)
{
    static const uint8_t expected[] = { 19, 20, 43, 44 };
    assertExactly(s::isSerialConsole, expected, 4, "esp32s3::isSerialConsole");
}

static void
test_esp32s3_console_pins_are_flagged_but_still_usable(void)
{
    // A warning, not a refusal: /capabilities.json lists them separately and
    // validatePins() logs rather than erroring, so a board that genuinely
    // needs GPIO 19 can have it.
    TEST_ASSERT_TRUE(s::isBonded(19));
    TEST_ASSERT_TRUE(s::isBonded(43));
    TEST_ASSERT_FALSE(s::isFlash(43));
    TEST_ASSERT_FALSE(s::isInputOnly(43));
}

static void
test_esp32s3_bonded_covers_the_devkitc1_headers(void)
{
    // 0-21 and 26-48, less 33 and 34, which exist in the SoC and are not on
    // the ESP32-S3-DevKitC-1's two 1x22 headers.
    static const uint8_t expected[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 26, 27, 28, 29, 30, 31, 32, 35,
        36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48
    };
    assertExactly(s::isBonded, expected, 43, "esp32s3::isBonded");
}

static void
test_esp32s3_gpio_22_to_25_do_not_exist(void)
{
    // Not "not brought out": SOC_GPIO_VALID_GPIO_MASK clears these four bits
    // and gpio_types.h's enum steps straight from GPIO_NUM_21 to GPIO_NUM_26.
    for (uint8_t pin = 22; pin <= 25; ++pin) {
        TEST_ASSERT_FALSE(s::isBonded(pin));
    }
}

static void
test_esp32s3_last_gpio_is_48(void)
{
    TEST_ASSERT_EQUAL_INT(48, (int)s::kMaxGpio);
    TEST_ASSERT_TRUE(s::isBonded(48));
    TEST_ASSERT_FALSE(s::isBonded(49));
}

// templates/config.espgarden_s3.json, from the hardware repo's own contract
// table. If this ever goes red, either the board moved a pin or a rule is
// wrong, and both are two-repo changes.
static void
test_the_espgarden_s3_template_pins_pass_their_own_rules(void)
{
    const uint8_t relays[] = { 10, 11, 12, 13 };
    for (unsigned i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(s::isBonded(relays[i]));
        TEST_ASSERT_FALSE(s::isFlash(relays[i]));
        TEST_ASSERT_FALSE(s::isInputOnly(relays[i]));
        // No strapping-pin warning is expected on any of the four.
        TEST_ASSERT_FALSE(s::isStrapping(relays[i]));
    }

    const uint8_t probes[] = { 1, 2, 4, 5 };
    for (unsigned i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(s::isADC1(probes[i]));
    }
    TEST_ASSERT_TRUE(s::isADC1(6)); // LDR
    TEST_ASSERT_TRUE(s::isADC1(7)); // AUX_ADC, undeclared but wired

    // SOIL_PWR_EN drives the probe bank's FET: an output, checked as one.
    TEST_ASSERT_TRUE(s::isBonded(14));
    TEST_ASSERT_FALSE(s::isInputOnly(14));
    TEST_ASSERT_FALSE(s::isFlash(14));

    const uint8_t digital[] = { 15, 16, 18 }; // flow, float, button
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(s::isBonded(digital[i]));
        TEST_ASSERT_FALSE(s::isInputOnly(digital[i]));
        TEST_ASSERT_FALSE(s::isFlash(digital[i]));
    }
}

// ---------------------------------------------------------------------------
// The two families must actually disagree
// ---------------------------------------------------------------------------

// A copy-paste that left one namespace answering as the other would pass every
// test above that only looks at one family. These are the pins where the two
// chips genuinely differ, so an accidental alias fails here.
static void
test_the_families_disagree_where_the_silicon_does(void)
{
    // GPIO 36: an ADC1 channel on a WROOM-32, octal PSRAM on this S3 module.
    TEST_ASSERT_TRUE(w::isADC1(36));
    TEST_ASSERT_FALSE(s::isADC1(36));
    TEST_ASSERT_FALSE(w::isFlash(36));
    TEST_ASSERT_TRUE(s::isFlash(36));

    // GPIO 6: SPI flash on a WROOM-32, an ADC1 channel on an S3.
    TEST_ASSERT_TRUE(w::isFlash(6));
    TEST_ASSERT_FALSE(s::isFlash(6));
    TEST_ASSERT_FALSE(w::isADC1(6));
    TEST_ASSERT_TRUE(s::isADC1(6));

    // GPIO 34: input-only on a WROOM-32; on an S3 it drives, and is refused
    // for an entirely different reason (not on the devkit's headers).
    TEST_ASSERT_TRUE(w::isInputOnly(34));
    TEST_ASSERT_FALSE(s::isInputOnly(34));

    // GPIO 1: UART0 TX on a WROOM-32, SOIL1 on this board.
    TEST_ASSERT_TRUE(w::isSerialConsole(1));
    TEST_ASSERT_FALSE(s::isSerialConsole(1));

    // GPIO 15: a strapping pin and the legacy watering relay on a WROOM-32;
    // an ordinary input carrying FLOW_PULSE here.
    TEST_ASSERT_TRUE(w::isStrapping(15));
    TEST_ASSERT_FALSE(s::isStrapping(15));

    TEST_ASSERT_NOT_EQUAL((int)w::kMaxGpio, (int)s::kMaxGpio);
}

// The concrete form of the hardware repo's "porting config_pins.cpp is a
// prerequisite, not a nicety": under the WROOM-32 rules this board's own pin
// map is a page of errors, and none of them is about a real problem.
static void
test_the_wroom32_rules_would_have_rejected_this_board(void)
{
    // Relays 10 and 11 read as SPI flash — a hard error, not a warning.
    TEST_ASSERT_TRUE(w::isFlash(10));
    TEST_ASSERT_TRUE(w::isFlash(11));
    TEST_ASSERT_FALSE(s::isFlash(10));
    TEST_ASSERT_FALSE(s::isFlash(11));

    // Relay 12 draws a strapping-pin warning it does not deserve.
    TEST_ASSERT_TRUE(w::isStrapping(12));
    TEST_ASSERT_FALSE(s::isStrapping(12));

    // Every analog channel — probes 1/2/4/5, LDR 6, aux 7 — reads as not-ADC1,
    // which is the error that says "this reads noise" about a correct pin.
    const uint8_t analog[] = { 1, 2, 4, 5, 6, 7 };
    for (unsigned i = 0; i < 6; ++i) {
        TEST_ASSERT_FALSE(w::isADC1(analog[i]));
        TEST_ASSERT_TRUE(s::isADC1(analog[i]));
    }

    // And the two real hazards go unreported: GPIO 45 and 46 are strapping
    // pins the WROOM-32 rules do not even consider bonded.
    TEST_ASSERT_FALSE(w::isStrapping(45));
    TEST_ASSERT_FALSE(w::isStrapping(46));
    TEST_ASSERT_TRUE(s::isStrapping(45));
    TEST_ASSERT_TRUE(s::isStrapping(46));
}

// ---------------------------------------------------------------------------
// The COMPILED defaults, held against the rules of their own family
//
// core/default_pins.h is the map a board uses before, and wherever, its
// config.json does not say otherwise — and three separate paths reach it:
// relayPinsSafeInit() 1.5 s before the config is read, loadSensor() whenever an
// io entry carries no "pin" key, and the constructor for every probe slot a
// short array leaves unset. Nothing REFUSES a bad one: validatePins() only
// logs and documentPinsAreUsable() skips an entry with no pin. So the rules
// have to be applied to the table here, where a wrong answer is a red test
// rather than a boot line nobody reads.
// ---------------------------------------------------------------------------

static void
assertProbeDefaultsAreDistinct(const uint8_t* probes, unsigned count,
                               const char* what)
{
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = i + 1; j < count; ++j) {
            if (probes[i] == probes[j]) {
                char message[96];
                snprintf(message, sizeof(message),
                         "%s: probes %u and %u share GPIO %u", what, i, j,
                         probes[i]);
                TEST_FAIL_MESSAGE(message);
            }
        }
    }
}

static void
test_the_wroom32_defaults_pass_the_wroom32_rules(void)
{
    for (unsigned i = 0; i < sizeof(dw::relay); ++i) {
        TEST_ASSERT_TRUE(w::isBonded(dw::relay[i]));
        TEST_ASSERT_FALSE(w::isFlash(dw::relay[i]));
        TEST_ASSERT_FALSE(w::isInputOnly(dw::relay[i]));
    }

    for (unsigned i = 0; i < sizeof(dw::soilMoisture); ++i) {
        TEST_ASSERT_TRUE(w::isADC1(dw::soilMoisture[i]));
    }
    TEST_ASSERT_TRUE(w::isADC1(dw::luminosity));
    TEST_ASSERT_TRUE(w::isADC1(dw::waterLevel));

    // The DHT, the flow input and the float switch all need an internal
    // pull-up, which GPIO 34-39 do not have.
    const uint8_t digital[] = { dw::dht, dw::flow, dw::floatSwitch };
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(w::isBonded(digital[i]));
        TEST_ASSERT_FALSE(w::isFlash(digital[i]));
        TEST_ASSERT_FALSE(w::isInputOnly(digital[i]));
    }
}

static void
test_the_esp32s3_defaults_pass_the_esp32s3_rules(void)
{
    for (unsigned i = 0; i < sizeof(ds::relay); ++i) {
        TEST_ASSERT_TRUE(s::isBonded(ds::relay[i]));
        TEST_ASSERT_FALSE(s::isFlash(ds::relay[i]));
        TEST_ASSERT_FALSE(s::isStrapping(ds::relay[i]));
    }

    for (unsigned i = 0; i < sizeof(ds::soilMoisture); ++i) {
        TEST_ASSERT_TRUE(s::isADC1(ds::soilMoisture[i]));
    }
    TEST_ASSERT_TRUE(s::isADC1(ds::luminosity));
    TEST_ASSERT_TRUE(s::isADC1(ds::waterLevel));

    const uint8_t digital[] = { ds::flow, ds::floatSwitch };
    for (unsigned i = 0; i < 2; ++i) {
        TEST_ASSERT_TRUE(s::isBonded(digital[i]));
        TEST_ASSERT_FALSE(s::isFlash(digital[i]));
        TEST_ASSERT_FALSE(s::isSerialConsole(digital[i]));
    }

    // This carrier has no DHT at all — the DHT22 header was dropped for an
    // SHT40 this firmware has no driver for — so there is no pin to default
    // to and kNoPin is the honest answer. It used to inherit the WROOM-32's
    // GPIO 23, which this part does not bring out, so a config declaring a DHT
    // without a pin was diagnosed as a bad pin number nobody had chosen.
    TEST_ASSERT_EQUAL_UINT8(default_pins::kNoPin, ds::dht);
}

// MOISTURE_MAX is 4 and both tables used to be three long, so the fourth slot
// fell through to the Arduino alias A0 — GPIO 36 on a WROOM-32 and GPIO 1 on
// an S3, which is probe 0's own pin on both. validatePins() then reported a
// conflict on a board that has a free ADC1 channel for it.
static void
test_no_family_defaults_a_probe_onto_another_probes_pin(void)
{
    assertProbeDefaultsAreDistinct(dw::soilMoisture, sizeof(dw::soilMoisture),
                                   "wroom32");
    assertProbeDefaultsAreDistinct(ds::soilMoisture, sizeof(ds::soilMoisture),
                                   "esp32s3");
}

// Every probe slot must come out of the table, because the fallback that used
// to fill the rest was A0. Raising MOISTURE_MAX has to mean naming the pins;
// this is the same claim config.cpp makes with a static_assert, made where it
// can be read.
static void
test_the_default_tables_cover_every_probe_slot(void)
{
    TEST_ASSERT_TRUE(sizeof(dw::soilMoisture) >= MOISTURE_MAX);
    TEST_ASSERT_TRUE(sizeof(ds::soilMoisture) >= MOISTURE_MAX);
}

// The other half of the same alias problem. luminosityPin and waterLevelPin
// were spelled A3 and A6, which resolve per board VARIANT and so look
// chip-relative — but on the esp32s3 variant A3 is GPIO 4, which is this
// firmware's third soil probe, while the carrier's LDR is on GPIO 6.
static void
test_no_family_defaults_the_ldr_onto_a_probe_pin(void)
{
    for (unsigned i = 0; i < sizeof(dw::soilMoisture); ++i) {
        TEST_ASSERT_NOT_EQUAL(dw::luminosity, dw::soilMoisture[i]);
    }
    for (unsigned i = 0; i < sizeof(ds::soilMoisture); ++i) {
        TEST_ASSERT_NOT_EQUAL(ds::luminosity, ds::soilMoisture[i]);
    }

    // GPIO 4 is what A3 resolved to on the S3 variant, and it IS a probe pin.
    TEST_ASSERT_EQUAL_UINT8(4, ds::soilMoisture[2]);
    TEST_ASSERT_NOT_EQUAL(4, ds::luminosity);
}

// The finding this table was split for, as the thing that must not come back.
// The commit that made the relay table per-family did not carry the same
// reasoning to dht, flow and float, which stayed at the WROOM-32's 23, 27 and
// 26 — and on an S3, 27 and 26 are SPI_HD and SPI_CS1, the octal flash bus
// that pinMode() and attachInterrupt() would then be pointed at.
static void
test_the_wroom32_sensor_defaults_are_hazardous_on_an_s3(void)
{
    TEST_ASSERT_TRUE(s::isFlash(dw::flow));        // GPIO 27, SPI_HD
    TEST_ASSERT_TRUE(s::isFlash(dw::floatSwitch)); // GPIO 26, SPI_CS1
    TEST_ASSERT_FALSE(s::isBonded(dw::dht));       // GPIO 23 does not exist

    // So the S3 table may not reuse any of them, and does not.
    TEST_ASSERT_NOT_EQUAL(dw::flow, ds::flow);
    TEST_ASSERT_NOT_EQUAL(dw::floatSwitch, ds::floatSwitch);
    TEST_ASSERT_NOT_EQUAL(dw::dht, ds::dht);

    // And the mirror image: the S3 relay table on a WROOM-32 is the SPI flash,
    // which is why the tables are per family in the first place.
    TEST_ASSERT_TRUE(w::isFlash(ds::relay[0]));
    TEST_ASSERT_TRUE(w::isFlash(ds::relay[1]));
}

void
run_pin_rules_tests(void)
{
    RUN_TEST(test_wroom32_adc1_is_the_six_bonded_channels);
    RUN_TEST(test_wroom32_input_only_is_34_to_39);
    RUN_TEST(test_wroom32_flash_is_6_to_11);
    RUN_TEST(test_wroom32_strapping_is_the_five_boot_mode_pins);
    RUN_TEST(test_wroom32_serial_console_is_uart0);
    RUN_TEST(test_wroom32_bonded_omits_the_pins_the_module_does_not_bring_out);
    RUN_TEST(test_wroom32_last_gpio_is_39);
    RUN_TEST(test_the_wroom32_template_pins_pass_their_own_rules);

    RUN_TEST(test_esp32s3_adc1_is_gpio_1_to_10);
    RUN_TEST(test_esp32s3_adc2_is_refused_like_the_esp32s);
    RUN_TEST(test_esp32s3_has_no_input_only_pins);
    RUN_TEST(test_esp32s3_flash_and_psram_is_26_to_37);
    RUN_TEST(test_esp32s3_octal_psram_pins_are_refused);
    RUN_TEST(test_esp32s3_strapping_is_0_3_45_46);
    RUN_TEST(test_esp32s3_does_not_inherit_the_esp32_strapping_pins);
    RUN_TEST(test_esp32s3_console_covers_uart0_and_usb);
    RUN_TEST(test_esp32s3_console_pins_are_flagged_but_still_usable);
    RUN_TEST(test_esp32s3_bonded_covers_the_devkitc1_headers);
    RUN_TEST(test_esp32s3_gpio_22_to_25_do_not_exist);
    RUN_TEST(test_esp32s3_last_gpio_is_48);
    RUN_TEST(test_the_espgarden_s3_template_pins_pass_their_own_rules);

    RUN_TEST(test_the_families_disagree_where_the_silicon_does);
    RUN_TEST(test_the_wroom32_rules_would_have_rejected_this_board);

    RUN_TEST(test_the_wroom32_defaults_pass_the_wroom32_rules);
    RUN_TEST(test_the_esp32s3_defaults_pass_the_esp32s3_rules);
    RUN_TEST(test_no_family_defaults_a_probe_onto_another_probes_pin);
    RUN_TEST(test_the_default_tables_cover_every_probe_slot);
    RUN_TEST(test_no_family_defaults_the_ldr_onto_a_probe_pin);
    RUN_TEST(test_the_wroom32_sensor_defaults_are_hazardous_on_an_s3);
}
