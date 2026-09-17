#pragma once

// The board templates `POST /onboarding` can write, per MCU family.
//
// -------------------------------------------------------------------------
// WHY THESE ARE COMPILED IN RATHER THAN PACKED INTO THE FILESYSTEM IMAGE
// -------------------------------------------------------------------------
//
// 1. THE FAMILY HAS TO BE SELECTED BY `CONFIG_IDF_TARGET_*`, and only the
//    compiler sees that macro. Offering a WROOM-32 template on an ESP32-S3 is
//    the exact hazard `config_guard()` in scripts/pio_assets.py exists to
//    refuse: it would put relays on that carrier's FLOW_PULSE, FLOAT_SW and
//    BTN_USER and probes on its octal flash/PSRAM bus. A file chosen by a build
//    script is a SECOND statement of "this is an S3", free to disagree with the
//    `board` line — which is precisely the argument platformio.ini makes for
//    carrying no hardware flags at all.
//
// 2. THE PORTAL'S OWN FAILURE MODE INCLUDES AN EMPTY FILESYSTEM. Arm 1 fires
//    when /config.json is missing, and one of the two ways it goes missing is
//    `FILESYSTEM.begin(true)` reformatting a partition that would not mount. A
//    template stored on that partition is absent exactly when it is needed, and
//    so is every web asset — which is why the setup page is a string in
//    web_onboarding.cpp too, and loads no script and no stylesheet.
//
// 3. IT IS ONE LESS CONFIG-SHAPED DOCUMENT ON A 512 KB PARTITION that already
//    runs at ~124 KB free, and `build_assets.py` copies .json verbatim, so each
//    template would cost a whole 4 KB LittleFS block.
//
// The price is flash in every image, including the family's own unused half —
// `pin_rules.h` and `default_pins.h` make the same trade for the same reason
// (both families always compiled so one host test can hold both), and the cost
// is measured in CLAUDE.md rather than estimated here.
//
// -------------------------------------------------------------------------
// WHAT A TEMPLATE IS AND IS NOT
// -------------------------------------------------------------------------
//
// It is a whole `/config.json` with six holes: `id`, `hostname`, `wifi.ssid`,
// `wifi.password`, `ota.username`, `ota.password`. The handler fills those and
// nothing else, then runs `configDocumentIsUsable()` over the result — the same
// check `POST /config.json` runs — so a template that is wrong for its chip is
// refused before a byte is written rather than after a reboot.
//
// It is NOT a claim that the board has these sensors. It is a starting pin map;
// everything else is edited on /devices.html once the device is on the LAN,
// which is the whole point of the runtime-hardware design.
//
// `mqtt.username` ships EMPTY on purpose. ThingsBoard carries the device access
// token there and nobody has one at setup time, so the link reports
// `down (rc=...)` until an operator pastes it into /config.html. An invented
// token would connect to nothing and report success, which this repo already
// paid three years for once.
//
// Arduino-free and both families always compiled, so `test_onboarding` can walk
// every pin of every template through the rules of the family it belongs to.

#include <stdint.h>

namespace onboarding_templates {

struct Template
{
    const char* id;      // stable key the form posts back
    const char* name;    // what the dropdown shows
    const char* summary; // one line under it
    const char* json;    // a whole config document, six fields short
};

// ---------------------------------------------------------------------------
// ESP32-WROOM-32 — espgarden1..5.
// ---------------------------------------------------------------------------
namespace wroom32 {

// Hardware v2, the pin map CLAUDE.md documents for espgarden5: relays clear of
// the strapping pins (0/2/5/12/15) and of the SPI flash (6-11), every analog
// channel on ADC1 because ADC2 cannot be read while WiFi is associated.
//
// Relay 0 is GPIO 19 and not the legacy GPIO 15. 15 is a strapping pin and
// stays in default_pins.h only so boards already in the field are unaffected;
// a board being set up for the first time has no such history.
static const char* const kJsonV2 =
  "{\"version\":2,\"id\":\"1a2b\",\"hostname\":\"espgarden\","
  "\"timezone\":\"<-03>3\",\"postalCode\":\"\","
  "\"wifi\":{\"ssid\":\"\",\"password\":\"\"},"
  "\"ota\":{\"username\":\"\",\"password\":\"\"},"
  "\"thingSpeak\":{\"apiKey\":\"0000\",\"channel\":0,\"moisture2Field\":0},"
  "\"talkBack\":{\"apiKey\":\"0000\",\"channel\":0},"
  "\"mqtt\":{\"clientID\":\"\",\"username\":\"\",\"password\":\"\","
  "\"server\":\"thingsboard.cloud\",\"port\":8883,"
  "\"cacert\":\"/thingsboard.pem\",\"backend\":\"thingsboard\","
  "\"useTLS\":true,\"rpc\":true,\"fwUpdate\":true,"
  "\"fwTitle\":\"esp-garden\",\"publishSec\":300,\"heartbeatSec\":900},"
  "\"cloud\":{\"enabled\":false},"
  "\"et0\":{\"enabled\":false,\"latitude\":0.0,\"scale\":1.0},"
  "\"log\":{\"level\":4},"
  "\"io\":{\"button\":0,"
  "\"relays\":[{\"pin\":19,\"on\":0,\"name\":\"Watering\"},"
  "{\"pin\":16,\"on\":0,\"name\":\"Relay 2\"},"
  "{\"pin\":17,\"on\":0,\"name\":\"Relay 3\"},"
  "{\"pin\":18,\"on\":0,\"name\":\"Relay 4\"}],"
  "\"dht\":23,"
  "\"soilMoisture\":[{\"pin\":36,\"name\":\"Soil Moisture 1\"},"
  "{\"pin\":34,\"name\":\"Soil Moisture 2\"}],"
  "\"luminosity\":{\"pin\":39,\"name\":\"Luminosity\"}},"
  "\"moisture\":[{\"dry\":0,\"wet\":0,\"relay\":0,\"invert\":true,"
  "\"kind\":\"capacitive-v2\"},"
  "{\"dry\":0,\"wet\":0,\"relay\":1,\"invert\":true,"
  "\"kind\":\"capacitive-v2\"}],"
  "\"history\":{\"records\":1440,\"periodSec\":60},"
  "\"schedules\":[]}";

// One pump, one probe. The smallest document this firmware runs, and the
// honest starting point for a breadboard: every other sensor is an entry
// somebody adds on /devices.html, and presence IS the key, so declaring one
// that is not wired reports a confident number for a pin that floats.
static const char* const kJsonMinimal =
  "{\"version\":2,\"id\":\"1a2b\",\"hostname\":\"espgarden\","
  "\"timezone\":\"<-03>3\",\"postalCode\":\"\","
  "\"wifi\":{\"ssid\":\"\",\"password\":\"\"},"
  "\"ota\":{\"username\":\"\",\"password\":\"\"},"
  "\"thingSpeak\":{\"apiKey\":\"0000\",\"channel\":0,\"moisture2Field\":0},"
  "\"talkBack\":{\"apiKey\":\"0000\",\"channel\":0},"
  "\"mqtt\":{\"clientID\":\"\",\"username\":\"\",\"password\":\"\","
  "\"server\":\"thingsboard.cloud\",\"port\":8883,"
  "\"cacert\":\"/thingsboard.pem\",\"backend\":\"thingsboard\","
  "\"useTLS\":true,\"rpc\":true,\"fwUpdate\":true,"
  "\"fwTitle\":\"esp-garden\",\"publishSec\":300,\"heartbeatSec\":900},"
  "\"cloud\":{\"enabled\":false},"
  "\"et0\":{\"enabled\":false,\"latitude\":0.0,\"scale\":1.0},"
  "\"log\":{\"level\":4},"
  "\"io\":{\"button\":0,"
  "\"relays\":[{\"pin\":19,\"on\":0,\"name\":\"Watering\"}],"
  "\"soilMoisture\":[{\"pin\":36,\"name\":\"Soil Moisture\"}]},"
  "\"moisture\":[{\"dry\":0,\"wet\":0,\"relay\":0,\"invert\":true,"
  "\"kind\":\"capacitive-v2\"}],"
  "\"history\":{\"records\":1440,\"periodSec\":60},"
  "\"schedules\":[]}";

static const Template kTemplates[] = {
    { "wroom32-v2",
      "ESP32-WROOM-32 - 4 relays, 2 probes",
      "Hardware v2: relays 19/16/17/18, probes 36/34, DHT11 23, LDR 39",
      kJsonV2 },
    { "wroom32-minimal",
      "ESP32-WROOM-32 - 1 relay, 1 probe",
      "Relay 19, probe 36. Add the rest on the Devices page",
      kJsonMinimal },
};

static const unsigned kCount = sizeof(kTemplates) / sizeof(kTemplates[0]);

} // namespace wroom32

// ---------------------------------------------------------------------------
// ESP32-S3 — the esp-garden-hardware carrier.
//
// The same pin map as templates/config.espgarden_s3.json, which is the
// machine-readable copy of that repo's cross-repo contract. No `dht`: the DHT22
// header was replaced by an SHT40, which this firmware HAS had a driver for
// since 2.14.0 — `io.i2c` and `io.sht4x` below are that part. No `waterLevel`:
// the carrier brings AUX_ADC out but declaring it pushes whatever sits on a
// spare header through the water-level curve and reports it as a level.
//
// This template shipped WITHOUT the two I2C blocks from 2.15.0 to 2.17.0,
// because it was written while the sentence above still ended "no driver for".
// A carrier onboarded through the portal in that window came up with four
// probes and no thermometer at all, which is how board b580 was found on
// 2026-09-17: /data.json carried no Temperature and no Air Humidity, and
// adding the two blocks by hand through POST /config.json is what made the
// SHT40 answer. Nothing but this string decides that, so it is the fix.
// ---------------------------------------------------------------------------
namespace esp32s3 {

static const char* const kJsonCarrier =
  "{\"version\":2,\"id\":\"1a2b\",\"hostname\":\"espgarden-s3\","
  "\"timezone\":\"<-03>3\",\"postalCode\":\"\","
  "\"wifi\":{\"ssid\":\"\",\"password\":\"\"},"
  "\"ota\":{\"username\":\"\",\"password\":\"\"},"
  "\"thingSpeak\":{\"apiKey\":\"0000\",\"channel\":0,\"moisture2Field\":0},"
  "\"talkBack\":{\"apiKey\":\"0000\",\"channel\":0},"
  "\"mqtt\":{\"clientID\":\"\",\"username\":\"\",\"password\":\"\","
  "\"server\":\"thingsboard.cloud\",\"port\":8883,"
  "\"cacert\":\"/thingsboard.pem\",\"backend\":\"thingsboard\","
  "\"useTLS\":true,\"rpc\":true,\"fwUpdate\":true,"
  "\"fwTitle\":\"esp-garden\",\"publishSec\":300,\"heartbeatSec\":900},"
  "\"cloud\":{\"enabled\":false},"
  "\"et0\":{\"enabled\":false,\"latitude\":0.0,\"scale\":1.0},"
  "\"log\":{\"level\":4},"
  "\"io\":{\"button\":18,"
  "\"relays\":[{\"pin\":10,\"on\":0,\"name\":\"Zona 1\"},"
  "{\"pin\":11,\"on\":0,\"name\":\"Zona 2\"},"
  "{\"pin\":12,\"on\":0,\"name\":\"Zona 3\"},"
  "{\"pin\":13,\"on\":0,\"name\":\"Reservatorio\"}],"
  "\"soilMoisture\":[{\"pin\":1,\"name\":\"Umidade Zona 1\",\"powerPin\":14,"
  "\"powerOn\":1,\"settleMs\":50},"
  "{\"pin\":2,\"name\":\"Umidade Zona 2\",\"powerPin\":14,\"powerOn\":1,"
  "\"settleMs\":50},"
  "{\"pin\":4,\"name\":\"Umidade Zona 3\",\"powerPin\":14,\"powerOn\":1,"
  "\"settleMs\":50},"
  "{\"pin\":5,\"name\":\"Umidade Zona 4\",\"powerPin\":14,\"powerOn\":1,"
  "\"settleMs\":50}],"
  "\"i2c\":{\"sda\":8,\"scl\":9,\"hz\":100000},"
  "\"sht4x\":{\"name\":\"\",\"address\":68},"
  "\"luminosity\":{\"pin\":6,\"name\":\"Luminosidade\"},"
  "\"flow\":{\"pin\":15,\"name\":\"Fluxo\",\"pulsesPerLitre\":450},"
  "\"floatSwitch\":{\"pin\":16,\"name\":\"Boia\",\"activeLevel\":0,"
  "\"interlock\":false,\"fillRelay\":3}},"
  "\"moisture\":[{\"dry\":0,\"wet\":0,\"relay\":0,\"invert\":true,"
  "\"kind\":\"resistive\"},"
  "{\"dry\":0,\"wet\":0,\"relay\":1,\"invert\":true,\"kind\":\"resistive\"},"
  "{\"dry\":0,\"wet\":0,\"relay\":2,\"invert\":true,\"kind\":\"resistive\"},"
  "{\"dry\":0,\"wet\":0,\"relay\":-1,\"invert\":true,\"kind\":\"resistive\"}],"
  "\"history\":{\"records\":1440,\"periodSec\":60},"
  "\"schedules\":[]}";

static const Template kTemplates[] = {
    { "s3-carrier",
      "ESP32-S3 - esp-garden-hardware carrier",
      "4 relays 10-13, 4 probes 1/2/4/5 on one power bank 14, LDR 6, "
      "SHT40 at 0x44 on I2C 8/9, flow 15, float 16",
      kJsonCarrier },
};

static const unsigned kCount = sizeof(kTemplates) / sizeof(kTemplates[0]);

} // namespace esp32s3

} // namespace onboarding_templates
