#include "core/onboarding.h"
#include "core/onboarding_templates.h"
#include "core/pin_rules.h"
#include <string>
#include <unity.h>
#include <vector>

// First-boot onboarding: when a board raises a setup AP, and whether the
// templates it would write are usable on the chip they ship for.
//
// TWO CLAIMS ARE TESTED, and both fail silently in the field.
//
// 1. A BOARD THAT HAS EVER REACHED THE NETWORK CANNOT ENTER THE PORTAL. The
//    portal's AP password is in this repository and its POST takes no token, so
//    a live garden falling into it — after a router reboot, a channel change,
//    an ISP outage — hands control of four pumps to whoever is in range. The
//    guarantee is structural rather than a timeout: no marker file, no portal,
//    and `decide()` is the only place in the tree that can answer "portal".
//
// 2. A TEMPLATE FROM THE WRONG FAMILY IS A SHORTED GPIO. The WROOM-32 map puts
//    relays on 15-18 and probes on 32-36; on the esp-garden-hardware carrier
//    those are FLOW_PULSE, FLOAT_SW, BTN_USER and the octal flash/PSRAM bus,
//    two of them switched to ground by the field hardware. The firmware
//    selects the family from CONFIG_IDF_TARGET_*, so nothing at runtime can
//    catch a table compiled into the wrong half — this is where it is caught.

namespace ob = onboarding;
namespace tpl = onboarding_templates;
namespace w = pin_rules::wroom32;
namespace s = pin_rules::esp32s3;

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

// The branch the live garden takes on every single boot, and the one that must
// never move. `associated` is swept because the whole point is that it is not
// read: a board with a stored config and no marker is Normal whether the radio
// came up or not.
static void
test_a_configured_board_without_a_marker_is_never_the_portal(void)
{
    for (int associated = 0; associated <= 1; ++associated) {
        const ob::Decision d = ob::decide(true, false, associated != 0);
        TEST_ASSERT_TRUE(d.mode == ob::Mode::Normal);
        TEST_ASSERT_TRUE(d.reason == ob::Reason::Configured);
        TEST_ASSERT_FALSE(d.clearMarker);
    }
}

// ...and it does not even LOOK. The probe costs 60 s of boot; spending it on a
// working board every time would be a regression nobody would notice until the
// garden took a minute longer to start watering.
static void
test_a_configured_board_without_a_marker_does_not_wait(void)
{
    TEST_ASSERT_FALSE(ob::mustProbeAssociation(true, false));
    TEST_ASSERT_TRUE(ob::mustProbeAssociation(true, true));
    // Arm 1 needs no probe either: there are no credentials to prove.
    TEST_ASSERT_FALSE(ob::mustProbeAssociation(false, false));
    TEST_ASSERT_FALSE(ob::mustProbeAssociation(false, true));
}

// Arm 1. A missing, unparseable or foreign /config.json means compiled
// defaults and a device that cannot associate, so there is nothing left to
// protect by staying quiet.
static void
test_a_config_that_did_not_load_is_always_the_portal(void)
{
    for (int marker = 0; marker <= 1; ++marker) {
        for (int associated = 0; associated <= 1; ++associated) {
            const ob::Decision d =
              ob::decide(false, marker != 0, associated != 0);
            TEST_ASSERT_TRUE(d.mode == ob::Mode::Portal);
            TEST_ASSERT_TRUE(d.reason == ob::Reason::NoUsableConfig);
            // Nothing is cleared here: the credentials have not been proven,
            // so the marker still has work to do at the next boot.
            TEST_ASSERT_FALSE(d.clearMarker);
        }
    }
}

// Arm 2. A mistyped Wi-Fi password produces a perfectly valid document, so
// loadFile() accepts it and arm 1 never fires.
static void
test_a_marked_board_that_cannot_associate_is_the_portal(void)
{
    const ob::Decision d = ob::decide(true, true, false);
    TEST_ASSERT_TRUE(d.mode == ob::Mode::Portal);
    TEST_ASSERT_TRUE(d.reason == ob::Reason::NeverAssociated);
    TEST_ASSERT_FALSE(d.clearMarker);
}

// The one transition that exists, and it happens once in a board's life.
static void
test_the_first_association_clears_the_marker_for_good(void)
{
    const ob::Decision first = ob::decide(true, true, true);
    TEST_ASSERT_TRUE(first.mode == ob::Mode::Normal);
    TEST_ASSERT_TRUE(first.clearMarker);

    // What the caller does with clearMarker is delete the file, so every later
    // boot is the markerPresent == false row — including one where the router
    // never comes back.
    const ob::Decision later = ob::decide(true, false, false);
    TEST_ASSERT_TRUE(later.mode == ob::Mode::Normal);
}

// The marker path is a contract between three readers: the handler that writes
// it, the boot that deletes it, and uploadPathIsProtected(), which refuses to
// let an ADMIN put it back on a configured device.
static void
test_the_marker_path_fits_the_filesystem_limit(void)
{
    const std::string path(ob::kMarkerPath);
    TEST_ASSERT_TRUE(path.size() >= 2);
    TEST_ASSERT_EQUAL('/', path[0]);
    // FILESYSTEM_MAX_PATH, spelled here rather than included: filesystem.h
    // needs Arduino.
    TEST_ASSERT_TRUE(path.size() <= 31);
    TEST_ASSERT_TRUE(path.find("..") == std::string::npos);
}

// ---------------------------------------------------------------------------
// The CA pairing
// ---------------------------------------------------------------------------

// Crossing the two fails TLS silently — every connect returns -9984 while the
// dashboard keeps reporting MQTT enabled — which is how channel 1348790
// received nothing between 2023 and 2026.
static void
test_each_backend_names_its_own_root_bundle(void)
{
    TEST_ASSERT_EQUAL_STRING("/thingsboard.pem",
                             ob::expectedCaFor("thingsboard"));
    TEST_ASSERT_EQUAL_STRING("/thingspeak.pem",
                             ob::expectedCaFor("thingspeak"));
}

static void
test_an_unknown_backend_has_no_ca(void)
{
    TEST_ASSERT_NULL(ob::expectedCaFor("mosquitto"));
    TEST_ASSERT_NULL(ob::expectedCaFor(""));
    TEST_ASSERT_NULL(ob::expectedCaFor(nullptr));
}

// ---------------------------------------------------------------------------
// A JSON scanner, small enough to be obviously right
//
// The templates are compiled strings, so nothing in the host build can parse
// them — Arduino_JSON needs Arduino. This walks the text instead. It is
// deliberately literal about the shape the templates are written in (compact,
// no whitespace, no escapes), because a scanner clever enough to handle
// anything would be a second thing that could be wrong.
// ---------------------------------------------------------------------------

// [begin, end) of the value that follows "key": , matching brackets.
static bool
valueSpan(const std::string& json,
          const std::string& key,
          size_t& begin,
          size_t& end,
          size_t from = 0)
{
    const std::string needle = "\"" + key + "\":";
    const size_t at = json.find(needle, from);
    if (at == std::string::npos) {
        return false;
    }
    begin = at + needle.size();
    const char open = json[begin];
    if (open != '{' && open != '[') {
        end = json.find_first_of(",}]", begin);
        return end != std::string::npos;
    }
    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    for (size_t i = begin; i < json.size(); ++i) {
        if (json[i] == open) {
            ++depth;
        } else if (json[i] == close) {
            if (--depth == 0) {
                end = i + 1;
                return true;
            }
        }
    }
    return false;
}

static std::vector<int>
collectPins(const std::string& span, const std::string& key)
{
    std::vector<int> out;
    const std::string needle = "\"" + key + "\":";
    size_t at = 0;
    while ((at = span.find(needle, at)) != std::string::npos) {
        out.push_back(atoi(span.c_str() + at + needle.size()));
        at += needle.size();
    }
    return out;
}

// The bare-number form `"dht":23` and the object form `{"pin":23}` both occur
// in these templates, exactly as loadSensor() accepts both.
static bool
singlePin(const std::string& json, const std::string& key, int& pin)
{
    size_t begin = 0, end = 0;
    if (!valueSpan(json, key, begin, end)) {
        return false;
    }
    const std::string span = json.substr(begin, end - begin);
    if (span[0] != '{') {
        pin = atoi(span.c_str());
        return true;
    }
    const std::vector<int> pins = collectPins(span, "pin");
    if (pins.empty()) {
        return false;
    }
    pin = pins[0];
    return true;
}

static std::string
stringValue(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\":\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) {
        return std::string();
    }
    const size_t begin = at + needle.size();
    return json.substr(begin, json.find('"', begin) - begin);
}

// ---------------------------------------------------------------------------
// The templates
// ---------------------------------------------------------------------------

struct Rules
{
    bool (*isADC1)(uint8_t);
    bool (*isInputOnly)(uint8_t);
    bool (*isFlash)(uint8_t);
    bool (*isBonded)(uint8_t);
};

static const Rules kWroom = {
    w::isADC1, w::isInputOnly, w::isFlash, w::isBonded
};
static const Rules kS3 = { s::isADC1, s::isInputOnly, s::isFlash, s::isBonded };

// The same roles documentPinsAreUsable() checks at save time, against the same
// predicates. Returns "" when the whole document is usable.
static std::string
templatePinProblem(const std::string& json, const Rules& r)
{
    size_t begin = 0, end = 0;

    if (!valueSpan(json, "io", begin, end)) {
        return "no io block";
    }
    const std::string io = json.substr(begin, end - begin);

    if (valueSpan(io, "relays", begin, end)) {
        const std::vector<int> pins =
          collectPins(io.substr(begin, end - begin), "pin");
        if (pins.empty()) {
            return "no relays declared";
        }
        for (size_t i = 0; i < pins.size(); ++i) {
            const uint8_t p = (uint8_t)pins[i];
            if (r.isFlash(p) || r.isInputOnly(p) || !r.isBonded(p)) {
                return "relay on GPIO " + std::to_string(pins[i]);
            }
        }
    }

    if (valueSpan(io, "soilMoisture", begin, end)) {
        const std::string span = io.substr(begin, end - begin);
        const std::vector<int> probes = collectPins(span, "pin");
        if (probes.empty()) {
            return "no probes declared";
        }
        for (size_t i = 0; i < probes.size(); ++i) {
            if (!r.isADC1((uint8_t)probes[i])) {
                return "probe on GPIO " + std::to_string(probes[i]);
            }
        }
        const std::vector<int> power = collectPins(span, "powerPin");
        for (size_t i = 0; i < power.size(); ++i) {
            if (power[i] < 0) {
                continue; // the UI's "none"
            }
            const uint8_t p = (uint8_t)power[i];
            if (!r.isBonded(p) || r.isFlash(p) || r.isInputOnly(p)) {
                return "probe power on GPIO " + std::to_string(power[i]);
            }
        }
    }

    int pin = 0;
    static const char* const kAnalog[] = { "luminosity", "waterLevel" };
    for (size_t i = 0; i < 2; ++i) {
        if (singlePin(io, kAnalog[i], pin)) {
            if (!r.isADC1((uint8_t)pin) || r.isFlash((uint8_t)pin) ||
                !r.isBonded((uint8_t)pin)) {
                return std::string(kAnalog[i]) + " on GPIO " +
                       std::to_string(pin);
            }
        }
    }

    static const char* const kDigital[] = { "dht", "flow", "floatSwitch" };
    for (size_t i = 0; i < 3; ++i) {
        if (singlePin(io, kDigital[i], pin)) {
            const uint8_t p = (uint8_t)pin;
            if (r.isFlash(p) || !r.isBonded(p) || r.isInputOnly(p)) {
                return std::string(kDigital[i]) + " on GPIO " +
                       std::to_string(pin);
            }
        }
    }

    // The bus, which is not a peripheral and so is not in either list above.
    // Both lines are open-drain: every device on an I2C bus talks by pulling
    // the line to ground, so the predicate is the OUTPUT one — a line that
    // cannot be driven low cannot talk, and Wire.begin() says nothing when it
    // is handed one. Every key of `io.i2c` is optional and falls back to the
    // per-family compiled default, so an absent line is not a problem.
    if (valueSpan(io, "i2c", begin, end)) {
        const std::string bus = io.substr(begin, end - begin);
        static const char* const kLines[] = { "sda", "scl" };
        for (size_t i = 0; i < 2; ++i) {
            const std::vector<int> line = collectPins(bus, kLines[i]);
            if (line.empty()) {
                continue;
            }
            const uint8_t p = (uint8_t)line[0];
            if (r.isFlash(p) || !r.isBonded(p) || r.isInputOnly(p)) {
                return std::string("i2c ") + kLines[i] + " on GPIO " +
                       std::to_string(line[0]);
            }
        }
    }

    return std::string();
}

static void
checkFamily(const tpl::Template* table, unsigned count, const Rules& r)
{
    TEST_ASSERT_TRUE(count > 0);

    for (unsigned i = 0; i < count; ++i) {
        const std::string json(table[i].json);

        TEST_ASSERT_TRUE(table[i].id != nullptr && table[i].id[0] != '\0');
        TEST_ASSERT_TRUE(table[i].name != nullptr && table[i].name[0] != '\0');
        TEST_ASSERT_TRUE(table[i].summary != nullptr &&
                         table[i].summary[0] != '\0');

        // Ids are the form's key, so a duplicate silently makes one template
        // unreachable.
        for (unsigned j = i + 1; j < count; ++j) {
            TEST_ASSERT_TRUE(std::string(table[i].id) != table[j].id);
        }

        const std::string problem = templatePinProblem(json, r);
        TEST_ASSERT_EQUAL_STRING("", problem.c_str());

        // The six holes the handler fills, in the shape it expects to find
        // them. An `ota` block missing its `username` key is a document
        // configDocumentIsUsable() refuses AFTER the AP has already been
        // raised and a password typed.
        TEST_ASSERT_TRUE(
          json.find("\"wifi\":{\"ssid\":\"\",\"password\":\"\"}") !=
          std::string::npos);
        TEST_ASSERT_TRUE(
          json.find("\"ota\":{\"username\":\"\",\"password\":\"\"}") !=
          std::string::npos);
        TEST_ASSERT_TRUE(json.find("\"id\":\"") != std::string::npos);

        // The strings loadFile() length-checks and the template — not the form
        // — supplies. Under four characters and the whole document is rejected
        // at boot, on a board that has just been told it is configured.
        TEST_ASSERT_TRUE(stringValue(json, "hostname").size() >= 4);
        TEST_ASSERT_TRUE(stringValue(json, "apiKey").size() >= 4);
        size_t begin = 0, end = 0;
        TEST_ASSERT_TRUE(valueSpan(json, "talkBack", begin, end));
        TEST_ASSERT_TRUE(
          stringValue(json.substr(begin, end - begin), "apiKey").size() >= 4);

        // The backend and its root bundle have to agree, or the device
        // connects to nothing and says MQTT is enabled while it does.
        TEST_ASSERT_TRUE(valueSpan(json, "mqtt", begin, end));
        const std::string mqtt = json.substr(begin, end - begin);
        const std::string backend = stringValue(mqtt, "backend");
        const char* expected = ob::expectedCaFor(backend.c_str());
        TEST_ASSERT_NOT_NULL(expected);
        TEST_ASSERT_EQUAL_STRING(expected,
                                 stringValue(mqtt, "cacert").c_str());
    }
}

static void
test_every_wroom32_template_is_usable_on_a_wroom32(void)
{
    checkFamily(tpl::wroom32::kTemplates, tpl::wroom32::kCount, kWroom);
}

static void
test_every_s3_template_is_usable_on_an_s3(void)
{
    checkFamily(tpl::esp32s3::kTemplates, tpl::esp32s3::kCount, kS3);
}

// The test above passes trivially if the two tables happen to be compatible
// with both chips, in which case it proves nothing about the #if that selects
// them. This says the separation has teeth: every WROOM-32 template really is
// refused by the S3 rules, and vice versa.
static void
test_a_template_from_the_other_family_would_be_refused(void)
{
    for (unsigned i = 0; i < tpl::wroom32::kCount; ++i) {
        const std::string problem =
          templatePinProblem(tpl::wroom32::kTemplates[i].json, kS3);
        TEST_ASSERT_TRUE(!problem.empty());
    }
    for (unsigned i = 0; i < tpl::esp32s3::kCount; ++i) {
        const std::string problem =
          templatePinProblem(tpl::esp32s3::kTemplates[i].json, kWroom);
        TEST_ASSERT_TRUE(!problem.empty());
    }
}

// The scanner is test-only code, so it gets the same treatment every other
// detector in this repo gets: a case where it must say no. Without this, a
// scanner that silently found no pins at all would make every check above pass.
static void
test_the_scanner_reports_a_pin_it_should_refuse(void)
{
    // A WROOM-32 relay on GPIO 6: SPI flash.
    const std::string bad =
      "{\"io\":{\"relays\":[{\"pin\":6,\"on\":0}],"
      "\"soilMoisture\":[{\"pin\":36}]}}";
    TEST_ASSERT_TRUE(!templatePinProblem(bad, kWroom).empty());

    // ...and a probe on a digital-only pin.
    const std::string worse =
      "{\"io\":{\"relays\":[{\"pin\":19,\"on\":0}],"
      "\"soilMoisture\":[{\"pin\":21}]}}";
    TEST_ASSERT_TRUE(!templatePinProblem(worse, kWroom).empty());

    // The same document with both corrected passes, so the refusals above are
    // about the pins and not about the scanner failing to read anything.
    const std::string good =
      "{\"io\":{\"relays\":[{\"pin\":19,\"on\":0}],"
      "\"soilMoisture\":[{\"pin\":36}]}}";
    TEST_ASSERT_EQUAL_STRING("", templatePinProblem(good, kWroom).c_str());
}

// The carrier has an SHT40 soldered to it, and from 2.15.0 to 2.17.0 this
// template did not say so — it was written while the firmware had no I2C
// driver and was not revisited when 2.14.0 added one. Board b580 was onboarded
// through the portal and came up with four probes and NO ambient sensor at
// all: no Temperature, no Air Humidity, no `Ambient Sensor` row. The part
// answered at 0x44 within seconds of the two blocks being added by hand
// (2026-09-17). Nothing but this string decides what a freshly onboarded
// carrier reads, so this is where that regression is caught.
static void
test_the_s3_carrier_template_declares_the_sht40_and_its_bus(void)
{
    bool seen = false;
    for (unsigned i = 0; i < tpl::esp32s3::kCount; ++i) {
        if (std::string(tpl::esp32s3::kTemplates[i].id) != "s3-carrier") {
            continue;
        }
        seen = true;
        const std::string json(tpl::esp32s3::kTemplates[i].json);

        // Presence IS the key: config.sht4xFitted is io.hasOwnProperty("sht4x")
        // and nothing else turns the ambient task on.
        TEST_ASSERT_TRUE(json.find("\"sht4x\":") != std::string::npos);

        size_t begin = 0, end = 0;
        TEST_ASSERT_TRUE(valueSpan(json, "i2c", begin, end));
        const std::string bus = json.substr(begin, end - begin);
        const std::vector<int> sda = collectPins(bus, "sda");
        const std::vector<int> scl = collectPins(bus, "scl");
        TEST_ASSERT_EQUAL_INT(1, (int)sda.size());
        TEST_ASSERT_EQUAL_INT(1, (int)scl.size());

        // The carrier's own nets. Neither may be GPIO 21: I2C_INT is wired by
        // the board and read by nothing here on purpose — the SHT4x family is
        // I2C-only with no interrupt output, and that net is pre-wiring for a
        // GPIO expander somebody may later plug into J8.
        TEST_ASSERT_EQUAL_INT(8, sda[0]);
        TEST_ASSERT_EQUAL_INT(9, scl[0]);
    }
    TEST_ASSERT_TRUE(seen);
}

// ...and the WROOM-32 templates must NOT declare one, because no such board
// here carries the part. Declaring it would clear dhtFitted through
// loadFile()'s mutual exclusion — fired at the wrong end, on a board whose
// only ambient sensor is the DHT11 it just switched off.
static void
test_the_wroom32_templates_declare_no_i2c_device(void)
{
    for (unsigned i = 0; i < tpl::wroom32::kCount; ++i) {
        const std::string json(tpl::wroom32::kTemplates[i].json);
        TEST_ASSERT_TRUE(json.find("\"sht4x\":") == std::string::npos);
    }
}

void
run_onboarding_tests(void)
{
    RUN_TEST(test_a_configured_board_without_a_marker_is_never_the_portal);
    RUN_TEST(test_a_configured_board_without_a_marker_does_not_wait);
    RUN_TEST(test_a_config_that_did_not_load_is_always_the_portal);
    RUN_TEST(test_a_marked_board_that_cannot_associate_is_the_portal);
    RUN_TEST(test_the_first_association_clears_the_marker_for_good);
    RUN_TEST(test_the_marker_path_fits_the_filesystem_limit);
    RUN_TEST(test_each_backend_names_its_own_root_bundle);
    RUN_TEST(test_an_unknown_backend_has_no_ca);
    RUN_TEST(test_every_wroom32_template_is_usable_on_a_wroom32);
    RUN_TEST(test_every_s3_template_is_usable_on_an_s3);
    RUN_TEST(test_a_template_from_the_other_family_would_be_refused);
    RUN_TEST(test_the_scanner_reports_a_pin_it_should_refuse);
    RUN_TEST(test_the_s3_carrier_template_declares_the_sht40_and_its_bus);
    RUN_TEST(test_the_wroom32_templates_declare_no_i2c_device);
}
