# SHT40 on I²C — a second ambient sensor behind the same two keys

**This ran on 2026-09-17**, on the `esp-garden-hardware` carrier, board `b580`: the part answers at **0x44 on GPIO 8/9 at 100 kHz**, reports 30.4-30.8 °C and 36-38 %RH on an indoor bench, and its CRC-checked frame decodes. The no-ACK branch, the address refusal and the DHT mutual exclusion were exercised deliberately. **What is still off a datasheet is the ACCURACY and the TIMING** — no reference instrument was on the bench and nothing has timed the `ambient` task — and the plausibility bounds have still never rejected a reading. `test_sht4x` remains what holds the CRC to Sensirion's own published check value and the two conversions to the endpoints their formulae define. See the unverified list for the line between the two.

The `esp-garden-hardware` carrier deleted its DHT22 header for an SHT40 — a Sensirion DFN at ±1.8 %RH and ±0.2 °C against a DHT11's ±5 and ±2 — which left that board reading no temperature and no humidity at all until this existed. `U3` sits at **0x44** on `I2C_SDA` GPIO 8 and `I2C_SCL` GPIO 9, with the bus pull-ups on the `mcu` sheet and `J8` bringing the same two lines off the board.

## The same keys, and why that is not the defect this repo keeps warning about

**`temperature` and `airHumidity` are shared. So are the `Temperature` / `Air Humidity` rows in `/data.json`, the accumulators behind them, and the `Now` and `Sd` twins.** This file records two cases of a stored key quietly acquiring a second meaning — `moisture2` becoming a different pot, and a relay index that was deleted rather than renumbered — and treats both as expensive mistakes. **A better thermometer is not one of them.** It is the same quantity in the same units, so no stored series changes meaning, no chart breaks, and nothing in the data needs a seam.

What a swap *does* change is **accuracy**, and that must be discoverable rather than silent — a reader who later notices the noise floor drop has to be able to find out why. Two places say it:

- **`ambient_sensor`, a ThingsBoard CLIENT ATTRIBUTE**, published from `tbOnConnect()` beside `current_fw_version`. An attribute and not telemetry for the reason `firmware` stopped being periodic telemetry: it changes at a reboot and at no other moment, so a copy in every payload restates a constant, and attributes are current state that overwrites — exactly right for "what is fitted". `snake_case` because that is the attribute namespace's convention here (`fw_title`, `fw_version`) where telemetry keys are camelCase. Named for the **question** rather than for one answer: a key called `sht40` cannot express "a DHT11", and `temperature_sensor` would have to be repeated for the humidity half of the same part. **Absent** when nothing is fitted.
- **`Status."Ambient Sensor"` in `/data.json`**, because the boot log is an 8 KB rolling buffer this device overwrites within hours and the attribute is only visible to somebody already inside ThingsBoard.

**`dhtErrorRate` keeps its name on a board with no DHT, and that is deliberate.** It is a published step key with a deadband and stored history behind it; renaming it orphans that series for the sake of a label, and a second key beside it doubles a diagnostic that reads 0 almost always. This repo kept the `/spiffs/` URL prefix through the move to LittleFS on exactly that argument. Its *meaning* is unchanged — the fraction of ambient-sensor reads that failed — the counters behind it are named for the role (`g_ambientReadErrors` / `g_ambientTotalReads`), and which part it describes is answered right next to it. **The `/data.json` Status LABEL did change**, `"DHT Error Rate"` → `"Ambient Error Rate"`, because a label is read by a human, rendered generically by `index.js`, and has no history under it.

## Two ambient sensors, one pair of keys, and the SHT40 wins

A document declaring **both** `io.dht` and `io.sht4x` is **not refused**, and that is the load-bearing half. `loadFile()` returning false means compiled defaults, `ssid "undefined"` and a board that can neither associate nor be reached to fix — a config authoring mistake must not brick a garden. So one wins, deterministically and out loud.

**It is the SHT40**, and the reason is not a coin toss: a board can only reach this state because somebody wired a Sensirion part onto a board that already had a DHT, which is an upgrade. Choosing the worse sensor would silently discard it.

**It is enforced by CLEARING `dhtFitted` in `loadFile()`, not by a check at each reader.** Everything downstream — the ambient task's enable, `validatePins()`, `/data.json`, `et0Available()`, the ThingSpeak field claims — then sees exactly one ambient sensor, and none of them has to know the case exists. One veto at one place; the same rule `relayStartAllowed()` is held to, for the same reason. `config.ambientFitted()`, `ambientName()` and `ambientSensorName()` are the three questions anything asks.

## The bus is not a peripheral, and `validatePins()` had to learn that sharing is legal

`io.i2c` carries the bus's **parameters** and never its presence. There is deliberately no `i2cFitted` to drift out of step with the pins it names — presence is still the key, it is just the **device's** key. `Wire.begin()` runs from exactly one place, `sensorsSetupAmbient()`, and only when an I²C device is declared.

`validatePins()` gained a fourth role, `ROLE_I2C`, and three rules:

- **Must be able to drive LOW.** An I²C line is open-drain: every device on it, the master included, talks by pulling it to ground. So the predicate is the *output* one, not the analog or pull-up one. On a WROOM-32 the natural-looking 34-39 have no output driver, and a bus parked on one never starts — with nothing in `Wire.begin()` to say so.
- **Strapping pins draw a warning.** The bus pull-ups hold both lines high from the instant power arrives, which is exactly when a strapping pin is sampled; on a WROOM-32 GPIO 12 held high selects 1.8 V flash and the board does not boot at all.
- **Two I²C owners on one pin is NOT a conflict.** Several devices on one bus is the normal case and the whole point of the carrier's `J8`; an audit that called it a fault is an audit an operator learns to ignore — the same reason two probes sharing one `powerPin` is allowed. The exemption is between two `ROLE_I2C` entries and no further: a relay or a probe on SDA still reports, and that is the case that matters, because it breaks the bus *and* the peripheral, silently. **Nothing exercises the exemption today** — one bus registers its two pins once, so there is no repeat to exempt until a second I²C kind exists. It is written now rather than then because the rule belongs with the role, not with whichever device happens to arrive second.

`documentPinsAreUsable()` mirrors the drive-low rule at **save** time, because boot is too late, and checks `io.i2c` whenever it is present rather than only when a device is on it: a value that cannot carry a bus is worth refusing on the day it is typed.

## Hand-rolled, and what that bought

**No library.** The whole protocol is one command byte, a ~10 ms wait and six bytes with two CRC8s. Three reasons, in order of weight:

1. **The half worth checking is pure arithmetic, and a library moves it out of reach.** The CRC and the two conversions live in `include/core/sht4x_protocol.h`, Arduino-free, so `test_sht4x` reaches them from the host — the same pattern as `segment_index.h`, `step_publisher.h` and `pin_rules.h`. Stubbing `<Wire.h>` instead would have produced a test that proves the stub agrees with itself.
2. **A dependency is a surface this repo has been bitten by.** `lib_deps` once pointed at an untagged git URL and silently tracked upstream HEAD for months. `Adafruit_SHT4x` pulls `Adafruit_BusIO` transitively, which is a second one.
3. **It is ~60 lines.** `src/sht4x.cpp` links at **1 499 B** on `espgarden2`.

**What it did NOT buy is flash, and the number is worth stating plainly.** The cost is almost entirely the Arduino/ESP-IDF I²C stack that `#include <Wire.h>` drags in, and a library would have paid that too. Measured on `espgarden2` against the same commit in a separate worktree, attributed from the link map: **+25 636 B flash total, of which 18 033 B is `libdriver.a(i2c.c)`, `libWire.a`, `esp32-hal-i2c.c` and `i2c_hal.c`** — and only **7 433 B is this repo's own code**. `espgarden_s3`: +24 724 total, 17 754 of it the same stack and 7 203 this repo's.

**Five WROOM-32 boards with no I²C part pay 18.0 KB for a bus they never start**, and that is accepted rather than fixed. It is precisely the trade [Runtime hardware](configuration.md) already made deliberately at 21 KB: every driver compiles into every image and `config.json` decides what is read. A build flag would buy it back and reintroduce the thing 75 `#ifdef` sites were removed to escape — and it would make an SHT40 on a WROOM-32's GPIO 21/22 unbuildable, which is a real configuration somebody could want tomorrow. **Nothing is executed on those boards**: `TwoWire`'s constructor sets seven fields and touches no hardware (read from the core's `Wire.cpp`), `Wire.begin()` is never called, and neither GPIO is driven. The app slot goes **71.1 % → 72.5 %** of 1.69 MB.

The one option that would genuinely have avoided the 18 KB is **bit-banging**, ~500 B of code, and it was declined: it is untested code on a bus `J8` brings off the board to arbitrary devices, some of which clock-stretch, with no hardware in existence to test it on.

## The plausibility gate, and the DHT lesson applied rather than repeated

**The CRC is the primary gate and it is the real difference.** Every frame carries **two independent CRC8s over two bytes each**, checked before either value is converted. The DHT11's single 8-bit sum over four bytes is what let a mistimed frame put an impossible 15.1 %RH into a stored average in the first place.

The bounds left over are therefore doing much less work, and they are **deliberately wider than the datasheet's rated range**:

| | DHT11 (rated 0-50 °C, 20-90 %RH) | SHT40 |
|---|---|---|
| temperature | 0 .. 50 °C | **-40 .. 125 °C** — the part's full OPERATING range |
| humidity | 5 .. 100 % | **-5 .. 105 %** pre-clip, then clipped to 0..100 |

- **The temperature gate can only ever reject a rail.** The conversion `-45 + 175·S/65535` spans exactly -45..130, so -40..125 rejects a 5 °C sliver at each end and nothing in between. No air and no sun-baked enclosure reaches either: the hottest reading in this project's whole archive is **45.04 °C**, a DHT in 38 minutes of direct sun, and it is a real measurement this gate keeps — `test_a_sun_baked_enclosure_reading_is_not_rejected` pins that.
- **The humidity gate runs BEFORE the clip, and the clip is Sensirion's own instruction.** `-6 + 125·S/65535` spans -6..119 *on purpose*: the scaling goes past the physical range so a part slightly out of tolerance reports a little beyond 0 or 100 rather than saturating, and the datasheet then says to clip to 0..100. So the honest order is **gate wide on what the PART can report, clip narrow on what the QUANTITY can be**. ±5 points is generous room for a part drifting out of its ±1.8 % calibration while still measuring.
- **The clip is not the DHT mistake repeated.** That one *discarded* real readings at 15 %RH — a rated range describes ACCURACY, not what a sensor can physically report, and this garden is in Brasília where the dry season takes the afternoon below 20 % routinely. This *accepts* the reading and rounds a 100.4 that relative humidity cannot exceed at an instrument, which condenses first. `test_real_brasilia_dry_season_air_is_not_rejected` is the red test anything trying to tighten these toward the SHT40's own ±1.8 % specification has to argue with.

**A failed read is counted as an error, not merely dropped** — the DHT's other compounding failure, where the one counter that exists to say the sensor is misbehaving said the opposite. Bus-level outcomes (`kNoAck`, `kShortRead`, `kNotReady`) and frame-level ones share one enum, because every one of them ends the same way and a caller that had to combine two enums is a caller that could forget one.

## What the read costs, and where it runs

**~10 ms, on the 1 Hz `ambient` task**, in the background bucket — the same cooperative pump the DHT task used and that the cloud model, the step events and the `/data.json` cache share. Almost all of it is the datasheet's measurement time for the high-repeatability command 0xFD: typical 6.9 ms, **maximum 8.2**, rounded up to a whole millisecond because `delay()`'s resolution is 1 ms. The two transactions add ~0.8 ms at 100 kHz.

**It is cheaper than the DHT read it replaces.** The Adafruit DHT driver bit-bangs a single-wire frame for 20-27 ms with interrupts disabled; this is a `delay()` that yields to FreeRTOS.

High repeatability and not medium (4.5 ms) or low (1.7 ms) deliberately: the ±1.8 %RH that makes the part worth an I²C driver at all is the high-repeatability figure, and 10 ms of a 1000 ms tick is 1 %. A **two-phase read** — command on tick N, read on tick N+1 — would remove even that and was not written: it buys 1 % of one background tick for a state machine and a second of latency on a quantity that moves in minutes.

## What ET0 gains, which is nothing yet

`et0Available()` now asks `ambientFitted()`, and **`et0.enabled` still ships false.** A correctly sited SHT40 is exactly the case Hargreaves-Samani was written for — but what was measured on this project's board was a **siting** failure, not an accuracy one: the device tracked **0.346** of the outdoor temperature swing while its dewpoint tracked **1.052**, which diagnoses thermal mass around the sensor. A ±0.2 °C part in the same box reports the same 0.35 slope more precisely. Enabling it stays a claim its owner makes after running `scripts/et0_fit.py` against that board's own archive.

## `/devices.html` cannot add or remove it, and that is stated rather than hidden

That page is pin-centric: every row it renders is one GPIO picked from a capability list, and an SHT40 has no pin of its own. `sht4x` is in `/capabilities.json`'s `kinds` anyway — that array is the firmware's statement of what it has a **driver** for — and `devices_model.js` filters its own table by `hasKind()`, so a kind it does not know about is simply not rendered. A save from that page carries `io.sht4x` and `io.i2c` through **untouched**, because `buildDocument()` starts from the served document and only replaces the keys of the `io` block it renders. Editing it today means `/config.html`. **That sentence used to end "only deletes keys it renders", and it was true one level up and false one level down**: the top-level `io` block really was passed through, while each `io.relays` / `io.soilMoisture` / `moisture` ENTRY was rebuilt from the page's model, which deletes every key of that entry the page has no control for. It cost `io.soilMoisture[0].powerAlways` on a live board on 2026-09-18. Entries now carry their unrendered keys forward too, so the claim holds at both levels; the exception is a single-instance sensor whose box is UNTICKED, where the whole key goes by design. One consequence worth knowing: that page's pin-conflict check does not claim SDA or SCL, so it will not warn about a relay put on GPIO 8 — the firmware's own `validatePins()` reports it at the next boot.
