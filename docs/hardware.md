# Hardware

| Environment | Board | Sensors | Relays |
|---|---|---|---|
| `espgarden1` | NodeMCU-32S | 3× soil moisture, luminosity, DHT11 (temp + humidity), water level, flow, float switch | 4 |
| `espgarden2` | ESP32 DOIT DevKit v1 | Luminosity | 1 |
| `espgarden3` | ESP32 DOIT DevKit v1 | Soil moisture, luminosity | 1 |
| `espgarden4` | ESP32 DOIT DevKit v1 | — (base config) | 1 |
| `espgarden5` | ESP32 DOIT DevKit v1 | 2× soil moisture, luminosity, DHT11 | 4 |
| `espgarden_s3` | ESP32-S3-DevKitC-1 (esp-garden-hardware carrier) | 4× soil moisture on a switched power bank, LDR, flow, float switch, SHT40 temperature + humidity on I²C | 4 |

## Sensors & Pinout (`espgarden5` defaults)

| Signal | Pin | Notes |
|---|---|---|
| Button | GPIO 0 | Boot button |
| Relay 1 — Watering | GPIO 19 | Active-low (`on = 0`) |
| Relay 2 | GPIO 16 | Active-low |
| Relay 3 | GPIO 17 | Active-low |
| Relay 4 | GPIO 18 | Active-low |
| DHT11 | GPIO 23 | Data line, 4.7–10 kΩ pull-up to 3V3 |
| Soil moisture 1 | GPIO 36 (`VP`, A0) | Capacitive sensor v2.0, % reported |
| Soil moisture 2 | GPIO 34 (A6) | Capacitive sensor v2.0, % reported |
| Luminosity | GPIO 39 (`VN`, A3) | 5 mm LDR in a divider with 10 kΩ to GND, % reported |
| Water level | GPIO 34 (A6) | Analog, converted via calibration curve (not on `espgarden5`) |
| Flow meter | GPIO 27 | Pulse input, interrupt-counted; `pulsesPerLitre` sets the scale (`espgarden1`) |
| Float switch | GPIO 26 | Digital, internal pull-up; `activeLevel` sets which level means "raised" (`espgarden1`) |

Two hardware rules constrain these choices:

- **Every analog input must be on ADC1 (GPIO 32–39).** ADC2 cannot be read while WiFi is associated.
- **GPIO 34–39 are input-only and have no internal pull-up** — fine for the LDR and the moisture probes, unusable for the DHT11 or a relay. Relay pins also avoid the strapping pins (0, 2, 5, 12, 15) and GPIO 6–11 (SPI flash).

Older boards keep the historical watering relay on GPIO 15; that is a strapping pin (MTDO) and new hardware should not reuse it.

## `espgarden_s3` — the esp-garden-hardware carrier

A different MCU family, not a sixth board of the same kind, which is why the
environment is named rather than numbered. **Nothing in this section has run: no
such board has been built.**

| Signal | Pin | Notes |
|---|---|---|
| Button | GPIO 18 | 10 kΩ pull-up, button to GND |
| Relay 1 — Zona 1 | GPIO 10 | `on = 0`; the chain is inverting, see below |
| Relay 2 — Zona 2 | GPIO 11 | |
| Relay 3 — Zona 3 | GPIO 12 | |
| Relay 4 — Reservatório | GPIO 13 | |
| Soil moisture 1–4 | GPIO 1, 2, 4, 5 | Resistive probes; ADC1 on an S3 is GPIO 1–10 |
| Probe power | GPIO 14 | One FET for all four probes, `settleMs` 50 |
| Luminosity | GPIO 6 | LDR divider |
| Aux analog | GPIO 7 | Wired, **not declared** — see below |
| Flow meter | GPIO 15 | BSS138 level shifter from a 5 V hall meter |
| Float switch | GPIO 16 | Board pull-up (1 kΩ series), `activeLevel` 0 |
| SHT40 | GPIO 8 / 9 (I²C) | **Unreadable by this firmware** |

The pin rules are different on every line that matters, and
`src/config_pins.cpp` now selects them from `CONFIG_IDF_TARGET_*` rather than
assuming a WROOM-32:

- **ADC1 is GPIO 1–10**, not 32–39; ADC2 (11–20) is still unusable with WiFi on.
- **There are no input-only pins.** Every S3 GPIO drives and has a pull-up.
- **SPI flash and PSRAM take GPIO 26–37**, not 6–11. 33–37 are the octal lines,
  taken on the N8R8 module this board is specified with.
- **GPIO 22–25 do not exist**, and 33/34 are not on the DevKitC-1's headers.
- **Strapping pins are 0, 3, 45 and 46** — none of 2, 5, 12, 15.
- **GPIO 43/44 are UART0 and 19/20 are USB-Serial-JTAG.** Both are listed as
  reserved rather than refused: usable, at the cost of the link that diagnoses
  a device whose config did not load.

**The relay safe-state is inverted from every other board, and the firmware did
not change for it.** There is no driver stage: `J200` takes the GPIOs straight
to the relay module's IN pins, the module's own pull-up holds IN high while the
ESP32's pins are inputs at reset, and every relay is released. So a reset no
longer pulses the pumps. `relayPinsSafeInit()` stays exactly as it is — it is
still the only thing standing between a reset and a running pump on the five
WROOM-32 boards — but on this board what it earns its place for is *not driving
the wrong pins*: the compiled default table is now per-family, because the
WROOM-32 relay pins 15/16/18 are this board's flow input, float switch and
button, and two of those three are switched to ground by the field hardware.

**`waterLevel` is deliberately absent from the template** even though the
hardware brings AUX_ADC out on GPIO 7. Declaring it is what makes the firmware
read that pin and push it through a fitted water-level curve; the header is a
spare analog input with no sensor on it, and presence *is* the key.

Its configuration template is **`templates/config.espgarden_s3.json`**, not
`data/config.template.json`. It lives outside `data/` on purpose: `data_dir`
points at `.pio/assets`, everything under `data/` is copied into the filesystem
image, and only one `config.json` ever reaches a device. Copy it to
`data/config.json` — changing `id` to the value the board prints as `ID:` on
boot — before `-t buildfs` or `-t uploadfs`.

It also takes its own partition table, `partitions/esp_garden_8mb.csv`: the
devkit has 8 MB of flash where every other board has 4, and a partition table
cannot be changed over OTA.

`espgarden1` (NodeMCU-32S) uses: button 0 · relays **15, 16, 17, 18** ·
DHT11 23 · soil moisture **36, 35 and 32** · luminosity 39 · water level 34.
That spends five of the six ADC1 channels a WROOM-32 exposes (32–36, 39; 37 and
38 are not bonded out), leaving GPIO 33 for a fourth probe.
Its second probe is dashboard-only — field 4 is the water level there, so
publishing it needs an explicit `thingSpeak.moisture2Field`, and the firmware
refuses a number a fitted sensor already owns. GPIO 16 and 17 are
free there because the ESP32-WROOM-32 has no PSRAM — on a WROVER module they are
not. `ConfigFile::validatePins()` logs every GPIO claimed by two peripherals and
every relay parked on an input-only pin (34–39) at boot.

Pin assignments can be overridden per device in `config.json` (see [configuration.md](configuration.md)).
