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

## Hardware v2 — `espgarden5`

The second-generation node carries **1 luminosity sensor (LDR, analog) · 1 DHT11 · 4 relays · 2 soil-moisture sensors (analog)**, built by `[env:espgarden5]`. Relay and probe counts are **runtime**, from `config.json` — see [Runtime hardware](configuration.md). `BuildConfig.h` only caps them at `RELAY_MAX` 8 and `MOISTURE_MAX` 4.

Default pin map (overridable per device through `config.json`): soil moisture 1 on **GPIO 36** (`VP`, ADC1), soil moisture 2 on **GPIO 34**, luminosity on **GPIO 39** (`VN`), DHT11 data on **GPIO 23** (output-capable with an internal pull-up; 34–39 are neither), relays 1–4 on **GPIO 19, 16, 17, 18** (output-capable, not strapping, not flash).

Hard constraints that shaped it, and that still apply to any change:

- **ADC2 is unusable while WiFi is on** (ESP32 silicon limitation). Every analog channel **must** land on ADC1: GPIO 32–39. GPIO 34–39 are input-only with no internal pull-up — fine for analog, useless for a relay or the DHT.
- **ADC1 is the scarce resource, and on a WROOM-32 it has six usable channels**: 32, 33, 34, 35, 36, 39. GPIO 37/38 are ADC1 on the die but not bonded out. `espgarden1` spends five of the six (3 probes + luminosity + water level), leaving GPIO 33. **GPIO 32/33 double as XTAL_32K_P/N** — free on a WROOM-32, which ships without that crystal, but unavailable on a module that has one fitted.
- **Relay pins must avoid the strapping pins** (0, 2, 5, 12, 15) and GPIO 6–11 (SPI flash). The legacy watering relay sits on **GPIO 15**, a strapping pin, and stays there so boards in the field are unaffected; `espgarden5` moves relay 0 to GPIO 19. Other free output-capable pins: 13, 14, 21, 22, 25, 26, 27.
- **Relays are active-low** (`relayPinOn = 0`) and a floating pin reads as "energise". `relayPinsSafeInit()` is therefore the **first statement of `setup()`**, and `loadConfigFile()` calls it again once the real pin assignment is known. Do not move either call later — everything between them (filesystem mount, config load, WiFi association) takes seconds, which is long enough to run a pump dry.
- **A ThingSpeak channel has only 8 fields**, and the numbering is a permanent contract with the history already in channel 1348790: `1` moisture0, `2` watering duration, `3` ping, `4` water level, `5` luminosity, `6` temperature, `7` air humidity, `8` boot time. **Relays 1–3 have no field and are local-only.**
- **The second probe's field is explicit configuration, not an implicit reuse.** `thingSpeak.moisture2Field` defaults to 0 (off) and `validateThingSpeakFields()` **refuses a number a fitted sensor already publishes**, rather than letting one silently win and overwrite the loser's history with its own units. Renumbering rewrites the meaning of everything already stored, so the choice belongs to whoever owns the channel; field 8 is the least destructive candidate, since `bootTime` is published once at startup.
- **Flash sits at ~63 % of the 1.69 MB app slot** since `partitions/esp_garden_4mb.csv` replaced the stock table (83.8 % of 1.31 MB before) — see [Porting](porting-fullbot.md).

## Hardware v3 — `espgarden_s3`, and the first board that is not a WROOM-32

The `esp-garden-hardware` carrier (sibling repo `../esp-gargen-hardware`; the misspelling is the directory's real name): an **ESP32-S3-DevKitC-1-N8R8 in a socket**, four relays over a bare header, four resistive probes on one switched power bank, an LDR, a spare analog input, a flow meter, a float switch, a user button and an SHT40 on I²C. Built by `[env:espgarden_s3]`. **The board arrived on 2026-09-17 as device `b580`, and since 2026-09-26 the module in the socket is `cfd0`** — a full-flash clone, same part, see the top of [What has actually run](verification-log.md). It boots, `validatePins()` passes on the real wiring, the four ADC channels and the LDR read, and the SHT40 answers on I²C — see [What has actually run](verification-log.md). **No relay has been energised on it**, so every relay claim in this section, the inverted safe state included, is still reasoning about a schematic.

That repo's *"The cross-repo contract with `esp-garden`"* section is the specification, and this file deliberately does not restate its pin table; `templates/config.espgarden_s3.json` is the machine-readable copy and `test_pin_rules` holds every one of those pins against the rule its role is judged by. Two departures from the contract, recorded because a reader will otherwise read them as omissions:

- **`waterLevel` is NOT declared**, although the contract maps it to `AUX_ADC` on GPIO 7 and the board brings that out. The hardware repo's own `io` block omits it too, and presence *is* the key: declaring it makes `sensorsReadIo()` push whatever sits on a spare analog header through the water-level curve `9 - 12*sin(4.04 - 1.61*V)` and report it as a level. A header with no sensor on it gets no key.
- **No `dht`, and since 2.14.0 it does not need one.** The DHT22 header was removed in favour of the SHT40, and the firmware now has an I²C driver and a `sht4x` KIND for it — see [SHT40](ambient-sensors.md). The compiled default `dhtPin` is still `kNoPin` here: it used to stay 23 on both families — not a GPIO an S3 has — on the argument that `validatePins()` would answer *"GPIO 23 is not bonded out"*, a correct diagnosis of a pin nobody chose. There is no DHT header on this board, so there is no honest default, and the audit says *"dht is declared with no pin, and this build has no default pin for it on this chip"*.
- **`I2C_INT` on GPIO 21 is wired by the board and read by NOTHING here, deliberately.** The SHT4x family is I²C-only and has no interrupt output at all; the hardware repo's own roadmap records that net as pre-wiring for a GPIO expander somebody may later plug into `J8`. Attaching firmware to it on the assumption that it belongs to the SHT40 would be inventing a signal, so `default_pins::esp32s3` names `i2cSda` and `i2cScl` and stops there, and `test_the_s3_i2c_defaults_are_the_carriers_own_nets` asserts neither is 21.

### The S3 pin rules, and where each one came from

Every number was read out of the ESP-IDF headers shipping inside `framework-arduinoespressif32` (3.20017, Arduino core 2.0.17) or off Espressif's own ESP32-S3 GPIO page. **The hardware repo's summary was used only as the thing to check against**; it agreed on both claims it makes (ADC1 = 1-10, flash/PSRAM = 26-37).

| Rule | WROOM-32 | ESP32-S3 | Source for the S3 answer |
|---|---|---|---|
| `pinIsADC1` | 32-36, 39 | **1-10** | `soc/adc_channel.h`: `ADC1_CHANNEL_0..9` are GPIO 1..10; ADC2 is 11..20 |
| `pinIsInputOnly` | 34-39 | **never** | `soc/soc_caps.h`: `SOC_GPIO_VALID_OUTPUT_GPIO_MASK == SOC_GPIO_VALID_GPIO_MASK`, under the comment `// No GPIO is input only`; every entry of `gpio_types.h`'s `ESP32S3` enum reads "input and output" |
| `pinIsFlash` | 6-11 | **26-37** | `soc/io_mux_reg.h`: `SPI_CS1` 26 through `SPI_DQS` 37 |
| `pinIsStrapping` | 0, 2, 5, 12, 15 | **0, 3, 45, 46** | Espressif's GPIO page verbatim; GPIO 46 corroborated by `esp_rom/esp32s3/rom/efuse.h`, which selects ROM UART printing on it |
| `pinIsSerialConsole` | 1, 3 | **19, 20, 43, 44** | `io_mux_reg.h`: GPIO 43/44 are `U0TXD_U`/`U0RXD_U`; `USB_DM_GPIO_NUM` 19, `USB_DP_GPIO_NUM` 20 |
| `pinIsBonded` | not 20/24/28-31/37/38 | **not 22-25, not 33/34** | `soc_caps.h` clears bits 22-25 out of `SOC_GPIO_VALID_GPIO_MASK` and `gpio_types.h` steps straight from `GPIO_NUM_21` to `GPIO_NUM_26`; 33/34 exist but are not on the DevKitC-1's two 1x22 headers |
| `pinMaxGpio` | 39 | **48** | `soc_caps.h`: `SOC_GPIO_PIN_COUNT` 49 |

Three of those are worth saying out loud, because they are the ones a reader will get wrong:

- **There is no input-only class at all.** The WROOM-32 predicate conflates "has no output driver" with "has no internal pull-up" and gets away with it because the same six pins have both. On the S3 neither property has a pin, so the honest answer is `false` for every GPIO — including 46, which *is* input-only on the ESP32-**S2** and is not on the S3. A copy of the WROOM-32 predicate would have refused GPIO 34-39 for a reason that does not exist on this part.
- **33-37 are refused unconditionally, and that is a choice rather than a fact.** They are the octal Flash/PSRAM lines, taken on the N8R8 module this board is specified with; on a quad-PSRAM module they would work. Refusing them costs three pins on a variant nobody has ordered; accepting them drives the PSRAM bus on the one that has been.
- **19/20 are reserved, not refused.** They carry USB-Serial-JTAG — console, debugger *and* the ROM download mode that recovers a brick, the same argument that put UART0 in this predicate on the WROOM-32, aimed at a stronger link. They appear in `/capabilities.json`'s `reservedPins` and draw a boot warning; a board that genuinely needs GPIO 19 can still have it.

**The family is selected by `CONFIG_IDF_TARGET_*`, not by a build flag**, so `platformio.ini` still carries no hardware flags at all. That macro reaches `config_pins.cpp` through `<Arduino.h>` → `esp32-hal.h` → `sdkconfig.h`, and the framework sets it from the env's `board` line — one statement of "this is an S3", not two free to disagree. An unrecognised target is a `#error` rather than a fallback: the failure mode of guessing is a page of boot errors about correct assignments and silence about the wrong ones, which nobody traces back to a default. The rules live in `include/core/pin_rules.h`, Arduino-free with one namespace per family and **both always compiled**, so `test_pin_rules` can hold both to their answers in one host binary — the same reason `segment_index.h` and `step_publisher.h` exist.

### The relay safe state INVERTS on this board, and the firmware did not change for it

On every WROOM-32 board here the relay module is driven through a stage that makes a floating GPIO read as "energise", which is why `relayPinsSafeInit()` is the first statement of `setup()` and why every reset pulses every pump. **This board has no driver stage.** `J200` takes the GPIOs straight to the module's active-low IN pins, the module's own pull-up holds IN high while the ESP32's pins are inputs at reset, and every relay is released. The safe state is now the default, and `on: 0` is what makes the inverting chain come out right. **Nothing was weakened, and nothing needed to be**: `relayPinsSafeInit()`, `clearUndeclaredRelayPins()` and `kNoPin` are unchanged and still correct — they are simply no longer the only thing between a reset and a running pump on *this* board, and they remain exactly that on the other five, which is why not one line moved. **What DID have to change is the compiled default pin table, and it is a defect fix rather than tidiness.** `relayPinsSafeInit()` runs over `g_defaultRelayPin` about 1.5 s before `config.json` is read, and the WROOM-32 table is `{15, 16, 17, 18}`. On this board GPIO 15 is `FLOW_PULSE`, 16 is `FLOAT_SW` and 18 is `BTN_USER`, so the first statement of `setup()` would drive three *inputs* as push-pull outputs, high, for the length of a boot — and two of the three are switched to ground by the field hardware (`SW150` shorts `BTN_USER` to GND; `Q400`, a BSS138, pulls `FLOW_PULSE` down on every meter pulse). That is a GPIO driving into a short, not a mis-assignment. The table is now per-family, selected the same way the pin rules are, and reads `{10, 11, 12, 13}` on the S3.

### What it costs, and the 8 MB partition table

`espgarden_s3` links at **1 208 757 B flash (41.9 % of the 2.75 MB slot) and 65 472 B static RAM**; its filesystem image is 2 490 368 B, exactly the 0x260000 partition. The cost to the WROOM-32 boards is **+280 B flash and +0 B static RAM**, a clean A/B on `espgarden2` against the same commit in a separate worktree (1 256 253 → 1 256 533; static 66 600 both times). Per object file with `xtensa-esp32-elf-size`: `config_document.cpp.o` +147, `config_pins.cpp.o` +117, `config_io.cpp.o` +20, `web_capabilities.cpp.o` +18, and **`config.cpp.o` +0** — the family-aware default pin table costs the WROOM boards nothing, which is what the `#if` is for. The rest is mostly `pinMaxGpio()` being a real cross-translation-unit call where three literal `39`s sat. `partitions/esp_garden_8mb.csv` is the only `board_build` override the env carries: the N8R8 devkit has 8 MB where every other board has 4, and **a partition table cannot be delivered over OTA** — the running image would be rewriting the map underneath itself — so the layout a board is first flashed with is the one it keeps until somebody attaches USB again. 2.75 MB per OTA slot, a 2.375 MB filesystem, and the coredump window moves to `esptool read_flash 0x7F0000 0x10000`. **Neither size is a measurement of a need**: the app would have fitted the 4 MB table's 1.69 MB slot, and nothing today wants more than ~300 KB of filesystem. They are headroom bought with flash that has no other claimant, spent on the one decision this board cannot revisit remotely.
