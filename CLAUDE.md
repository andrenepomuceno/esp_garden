# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Operating guide for **esp-garden**. Companion doc: [README.md](README.md).

**Reference repo:** `fullbot-firmware` (ESP32-S3 firmware for the FullBot solar-panel cleaning robot), at `~/solarbot/fullbot-firmware` **inside WSL** (`wsl.exe -e bash -lc '...'` from Windows — it is not on a `/mnt` path). It has its own `CLAUDE.md` plus a `docs/` directory. The user's explicit instruction is to **reuse it freely** — scheduler, MQTT, web UI, web server, OTA, config, logging. Read it before designing anything new here; most problems this repo is about to hit are already solved there. See [Porting from fullbot-firmware](#porting-from-fullbot-firmware) for what transplants cleanly and what does not.

---

## Project at a glance

ESP32 firmware for an automatic garden: soil moisture + luminosity + DHT11 + optional water level, a watering relay, a local async web dashboard, ThingSpeak MQTT/TLS telemetry, ThingSpeak TalkBack for remote commands, and browser OTA.

| Layer | Path | Role |
|---|---|---|
| Entry point | `src/main.cpp` (79 lines) | Relay safe-init, device id, SPIFFS, config, logger backup, `webSetup()`, `tasksSetup()` |
| Orchestration | `src/tasks.cpp` (621) | **The single task-registration site.** Every `DECLARE_TASK`, every handler body, `tasksSetup()`, `tasksLoop()`, schedules, connectivity |
| Relays | `src/relays.cpp` (214) | Relay state under `g_relayMux`, `startRelay`/`stopRelay`/`startWatering`, the 50 ms critical tick |
| Sensors | `src/sensors.cpp` (238) | Every ADC read, the flow ISR, the float switch, the DHT, `moistureState()` |
| Telemetry | `src/telemetry.cpp` (201) | ThingSpeak field constants, `mqttAddField`, the ThingsBoard payload, the publish queue |
| Config | `src/config.cpp` (676), `include/core/config.h` | `ConfigFile` singleton + `g_*` reference aliases for the non-pin fields |
| Config — pins | `src/config_pins.cpp` | What a WROOM-32 GPIO can do, and the boot-time audit that applies it. One place, three consumers |
| Config — `io` | `src/config_io.cpp` | Parsers for the `io` block, where every entry accepts several shapes so a field device keeps loading after a firmware update |
| Config — save | `src/config_document.cpp` | Whole-document validation before a write: the save-time counterpart of `loadFile()` |
| Logging | `src/logger.cpp` | Level-filtered singleton, 8 KB rolling RAM buffer, SPIFFS backup rotating over 4 files |
| Web | `src/web.cpp` (430) | WiFi events, mDNS, `AsyncWebServer`, **the route table**, `/control`, `/logs`, `/history.json` |
| Web handlers | `src/web_data.cpp`, `web_config.cpp`, `web_ota.cpp`, `web_users.cpp` | `/data.json` cache · masked `GET`/`POST /config.json` · browser OTA · `/users.json` |
| Auth | `src/custom_login.cpp`, `src/user_store.cpp` | Nonce + SHA-256 login, role middleware, per-IP lockout, `/users.json`, `/sessions.json` — ported from fullbot |
| MQTT | `src/mqtt.cpp` | Transport only: `PubSubClient` over TLS or plain, reconnect backoff, buffer sizing. `mqtt.backend` picks ThingSpeak `channels/<id>/publish` or ThingsBoard `v1/devices/me/telemetry` |
| ThingsBoard | `src/thingsboard.cpp` (698) | The downlink half: client/shared attributes, two-way RPC, the chunked `v2/fw` firmware stream |
| Versions | `src/fw_version.cpp` | Semantic-version compare — the check deciding whether a cloud image is flashed. Host-tested |
| TalkBack | `src/talkback.cpp` | Hand-rolled HTTP/1.1 POST to `api.thingspeak.com` (**plain HTTP, port 80**) |
| History | `src/io_history.cpp`, `include/core/ring_index.h` | Fixed-size ring buffer of I/O snapshots on SPIFFS, served by `/history.json` |
| Moisture model | `src/moisture_classifier.cpp` (pure maths, host-tested), `src/moisture_model.cpp` (training, persistence) | Gaussian naive Bayes per probe, labelled by watering events. See [Soil moisture](#soil-moisture-a-classifier-trained-on-watering-events) |
| Stats | `src/accumulator_v2.cpp` | Rolling window mean + variance over a `std::list<float>` |
| Filesystem image | `data/` | `index.*`, `login.*`, `config.*`, `users.*`, `update.*`, `auth.js`; vendored `jquery.js`, `sha256.js`, `spark-md5.js` (all MIT); `favicon.ico`, `thingspeak.pem`, `config.template.json`; vendored `bootstrap.css.gz` |
| Partitions | `partitions/esp_garden_4mb.csv` | 1.69 MB per OTA slot, 512 KB SPIFFS. **Cannot be changed over OTA** |
| Tooling | `scripts/` | `dev_server.py` + `sim_state.py` · `sim_moisture.py` · `sim_config.py` · `sim_auth.py` (host simulator of the device HTTP API), `check_lines.py` (the file-size gate), `moisture_calibration.py`, `feeds_plot.py` |

**No source file exceeds 1000 lines, and `python scripts/check_lines.py` is what
says so.** The rule sat in this file unenforced for long enough that two files
crossed it unnoticed — the gate exists because the honour system had already
failed. It prints the largest files on success too: a failure arrives when the
split is expensive, and the useful signal is the file three commits away from
crossing. `tasks.cpp` (1123), `web.cpp` (1004), `config.cpp` (1125) and
`devices.js` (1155) were all split at that threshold. `tasks.cpp` kept every `DECLARE_TASK` and every handler and `web.cpp` kept `webSetup()`, in both cases because the ordering *inside* those functions is load-bearing — see the boot sequence and the route-order note below.

Host tests live in `test/` and run under **`[env:native]`** (`pio test -e native`). Coverage is `AccumulatorV2`, the history ring arithmetic and firmware-version comparison — everything else reaches WiFi, SPIFFS, `Arduino_JSON` or FreeRTOS. See [test/README.md](test/README.md), including why the JSON logic must not be trusted to a hand-written stub.

---

## What has actually run

Every claim below this line is either **verified** — it has been executed on the
hardware or in a test, with the date — or **unverified**, which means it was
written and compiles and nothing more. Move a line up only after running it.
Adding a section to this file does not promote anything.

This exists because this document has been wrong. It stated *"no page loads a
script from a CDN"* while all nine pages pulled Bootstrap from jsdelivr; it
stated *"no source file exceeds 1000 lines"* while two did; it recorded
`FW_VERSION` as `1.1.0` long after it was `2.1.0`. All three were found by a
reader, not by the project. A number without an execution behind it is a guess
that has been formatted to look like a fact.

**A hardware fault worth knowing before blaming firmware:** every boot prints

```
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
flash read err, 1000
ets_main.c 371
rst:0x10 (RTCWDT_RTC_RESET)   <- second attempt, boots fine
```

The ROM bootloader fails its first flash read and the RTC watchdog restarts it.
That is marginal supply — a long USB cable, a hub, or the regulator sagging —
not code, and it costs a few seconds of every boot. It will become a real boot
loop if the supply gets worse.

**Getting the board back after serial work:** `esptool.py --port COM7 run`.
Hand-rolled DTR/RTS toggling left it in `DOWNLOAD_BOOT` — off the network,
silent on serial, and looking exactly like a hang. An hour went into diagnosing
a board that was sitting in the ROM bootloader waiting for a flash.

**Reading serial:** `pio device monitor` under a `timeout` truncates its buffer
unpredictably, which made a healthy boot look like it stopped at three different
places. Read the port directly and flush per line when the answer matters.

**Verified on hardware (device `9e7c`, espgarden1):**

- Nonce + SHA-256 login, roles, per-IP lockout, persistent sessions.
- ThingSpeak *and* ThingsBoard MQTT over TLS. Channel 1348790 entry 304860 was
  the first publish since 2023-03-07, after the CA pin was corrected
  (2026-08-23). ThingsBoard connected on `thingsboard.cloud:8883` the same day.
- ThingsBoard downlink **subscriptions** — all four topics confirmed in the
  device log at debug level (2026-08-24).
- Browser OTA of both firmware and filesystem, including recovery from a
  half-written filesystem via serial.
- `GET /config.json?secrets=1` → edit → `POST` round trip, verified by byte
  count (1267 sent, 1267 written). The masked-secret restore path.
- The I/O history ring buffer, its wrap-around, and `/history.json`.
- Relay switching, the 30 s ceiling, the sticky mask in the history record.
- Probe readings: three probes at 42.9 / 57.0 / 52.6 with variances ≤ 0.10.
- **Heap under load** (2026-08-24, firmware 2.2.1). Six endpoints hammered in
  parallel — including `/history.json?limit=200` — eight rounds, zero failures.
  269 KB free at boot, ~115 KB in steady state, **76 KB at the low-water mark**.
  Memory is not the constraint it was assumed to be. The number to watch is the
  largest free block: it drifts 53 → 49 KB while free stays flat, which is
  fragmentation from `/history.json` building its response.

  **`AsyncResponseStream` was tried as the fix and made it worse — withdrawn.**
  The stream looked obviously right, since `beginResponse` copies the String it
  is handed and the path therefore costs the payload twice. Measured on the
  same eight-round parallel load: largest free block **53 KB → 33 KB** (once to
  16), low-water heap **76 KB → 60 KB**. The stream's buffer grows by
  reallocation and leaves a hole at every step, while one `reserve()` of the
  right size is a single allocation. The double copy costs PEAK; the stream
  costs FRAGMENTATION, and fragmentation is what bites a device that has to
  stay up for months. If the peak ever does matter, the answer is chunked
  generation — one record per callback — not a differently-shaped buffer.
- **Runtime hardware.** Firmware 2.2.0 boots off `config.json` alone and logs
  what it decided: `Sensors: 3 moisture, luminosity, DHT, water level, flow,
  float switch`, `DHT11 on GPIO 23`, `Flow sensor on GPIO 27`. No `HAS_*` flag
  exists any more (2026-08-24).
- **`GET /capabilities.json`** — returns `relayMax 8`, `moistureMax 4`,
  `analogPins [32,33,34,35,36,39]` and `reservedPins [1,3]`, i.e. it excludes
  the unbonded pins and separates the serial console, from the same predicates
  `validatePins()` uses.
- **`GET /moisture.json`** — answers on the device, reports the gates and
  `blockedBy: "not trained yet"` for all three probes. The endpoint and the
  refusal path are exercised; the FIT is not (see below).
- **`POST /spiffs/delete`** (2026-08-24). All four refusals fired on the
  device: `/users.json` (credential store), `/config.json` (use the validating
  endpoint), `..` in the path, and a 404 for a path that is not there — the
  last one deliberately, so a wrong path is reported rather than silently
  succeeding.
- **`POST /spiffs/upload`** (2026-08-24). A 48-byte file written and read back
  byte-identical through `/spiffs/`, with 139 KB still free. All four refusals
  fired on the device: wrong MD5, `/config.json`, `/users.json`, and `..` in
  the path. *(The probe file `/probe.js` is still on the filesystem — there is
  no delete endpoint. Harmless, and it is why one exists as a to-do.)*
- **The relay guard on the browser OTA** (2026-08-24). With Zona 2 energised,
  `/updateEnable` answered **409 "Zona 2 is running. Updating reboots the
  device, and a relay is energised across a reset."**; it answered 200 once the
  relay expired.
- **The vendored `bootstrap.css.gz`** — served from the device at 29 899 bytes.
  No page reaches a CDN any more.
- **The moisture classifier's TRAINING pass, on real data** (2026-08-24
  08:22). One watering of Zona 2 — a relay run by hand to test the OTA guard —
  was detected as a single rising edge, and the 33 readings inside the 60-minute
  window before it were labelled DRY:

  ```
  [moisture] probe 1: +1 events (1 total), J=0.0, dry/humid/wet = 57.1/0.0/0.0
  [moisture] trained on 56 records, 33 samples, 0 outliers dropped
  ```

  Humid and wet stayed empty, which is the consumption window working: the
  cycle that event opened is not complete until the NEXT watering bounds it, so
  everything after it waits. The gates then refused the probe at 1 of 6 events.
  What is verified is the labelling, the window and the refusal. What is still
  not is a **complete** cycle, and with it the first non-empty WET class.
- **The `IoRecord` layout change** — the 40 → 48 byte growth was detected by
  the header check and the buffer reformatted rather than being misread:
  `io_history: incompatible or corrupt file, recreating` →
  `formatted /io_history.bin for 1440 records (69136 bytes)`.

**Unverified — written, compiles, never run on hardware:**

- **ThingsBoard RPC.** No command has been sent. The device's credential is
  MQTT Basic rather than an access token, so the tenant REST API is not
  reachable from here; it needs a person in the ThingsBoard UI.
- **ThingsBoard FOTA.** No package has ever been assigned. The chunk stream,
  the MD5 verification, the relay-idle deferral and the 409 interlock against
  the browser path are all unexercised.

- **The reservoir interlock.** `floatInterlock` is off, and the float switch is
  not wired.
- **The flow meter.** Not wired. `flowPulsesPerLitre` is a datasheet number, not
  a measured one.

- **Scheduled watering.** Four schedules exist, all disabled. None has fired.

**Measured, and contradicting something this file used to say:**

- A **disconnected** probe on GPIO 32 does not sit at a rail, and does not sit
  anywhere in particular. Two readings hours apart in the same session:
  **52.6 (variance 0.01)** and **86.7**. Quiet at any instant, and free to be
  somewhere else entirely later — which is worse than a rail, because each
  sample looks like plausible soil.

  *(An earlier version of this line reported only the 52.6 and treated it as
  characteristic. **Withdrawn** — one sample of a floating input describes that
  minute. The "~94 here" it replaced was equally unreproducible.)*

  The consequence stands and gets stronger: the 3σ outlier rejection will not
  fire on any single one of these, so what rejects a disconnected probe is the
  SEPARATION gate — a floating pin does not respond to its pump, so dry, humid
  and wet collapse onto one number and J goes to 0.

## Hardware v2 — `espgarden5`

The second-generation node carries **1 luminosity sensor (LDR, analog) · 1 DHT11 · 4 relays · 2 soil-moisture sensors (analog)**, built by `[env:espgarden5]`. Relay and probe counts are **runtime**, from `config.json` — see [Runtime hardware](#runtime-hardware--the-build-no-longer-knows-what-is-fitted). `BuildConfig.h` only caps them at `RELAY_MAX` 8 and `MOISTURE_MAX` 4.

Default pin map (overridable per device through `config.json`):

| Signal | Pin | Why that pin |
|---|---|---|
| Soil moisture 1 | GPIO 36 (`VP`, A0) | ADC1 |
| Soil moisture 2 | GPIO 34 (A6) | ADC1 |
| Luminosity (LDR) | GPIO 39 (`VN`, A3) | ADC1 |
| DHT11 data | GPIO 23 | Output-capable with an internal pull-up; 34–39 cannot do either |
| Relays 1–4 | GPIO 19, 16, 17, 18 | Output-capable, not strapping, not flash |

Hard constraints that shaped it, and that still apply to any change:

- **ADC2 is unusable while WiFi is on** (ESP32 silicon limitation). Every analog channel **must** land on ADC1: GPIO 32–39. GPIO 34–39 are input-only and have no internal pull-up — fine for analog, useless for a relay or the DHT.
- **ADC1 is the scarce resource, and on a WROOM-32 it has six usable channels**: 32, 33, 34, 35, 36, 39. GPIO 37 and 38 are ADC1 on the die but are not bonded out on the module. `espgarden1` now spends five of the six (3 probes + luminosity + water level), leaving GPIO 33. **GPIO 32/33 double as XTAL_32K_P/N** — free on a WROOM-32, which ships without that crystal, but unavailable on a module that has one fitted.
- **Relay pins must avoid the strapping pins** (0, 2, 5, 12, 15) and GPIO 6–11 (SPI flash). The legacy watering relay sits on **GPIO 15**, a strapping pin (MTDO), and stays there so boards in the field are unaffected; `espgarden5` moves relay 0 to GPIO 19. Other free output-capable pins: 13, 14, 21, 22, 25, 26, 27.
- **Relays are active-low** (`relayPinOn = 0`) and a floating pin reads as "energise". `relayPinsSafeInit()` is therefore the **first statement of `setup()`**, and `loadConfigFile()` calls it again once the real pin assignment is known. Do not move either call later — everything between them (SPIFFS mount, config load, WiFi association) takes seconds, which is long enough to run a pump dry.
- **A ThingSpeak channel has only 8 fields**, and the numbering is a permanent contract with the history already in channel 1348790. Map: `1` moisture0, `2` watering duration, `3` ping, `4` water level, `5` luminosity, `6` temperature, `7` air humidity, `8` boot time. **Relays 1–3 have no field and are local-only.**
- **The second soil probe's field is explicit configuration, not an implicit reuse.** `thingSpeak.moisture2Field` defaults to 0 (off), and `validateThingSpeakFields()` **refuses a number a fitted sensor already publishes** rather than letting the payload carry the field twice — one silently wins and the loser's history is overwritten with the winner's units. Renumbering rewrites the meaning of everything already stored, so the choice belongs to whoever owns the channel. Field 8 is the least destructive candidate: `bootTime` is published once at startup and is empty in nearly every record.
- **Flash sits at ~63 % of the 1.69 MB app slot** since `partitions/esp_garden_4mb.csv` replaced the stock table (it was 83.8 % of 1.31 MB before). Roughly 640 KB of headroom remain for the rest of the fullbot port — see [Porting](#porting-from-fullbot-firmware).

---

## Essential commands

PlatformIO is **not on `PATH`** on Windows. Use the penv binary; in WSL `pio` is on `PATH` directly.

```powershell
# ---- Build / flash (PowerShell) ------------------------------------------
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $pio run -e espgarden1                      # compile only  (~9 s, verified)
& $pio run -e espgarden1 -t upload            # flash APP ONLY
& $pio run -e espgarden1 -t buildfs           # -> .pio/build/espgarden1/spiffs.bin
& $pio run -e espgarden1 -t uploadfs          # TRAP: overwrites the device's /config.json
& $pio device monitor -b 115200               # serial @115200
& $pio run -t clean -e espgarden1
```

- Envs: `espgarden1` (NodeMCU-32S — moisture + luminosity + DHT), `espgarden2`, `espgarden3`, `espgarden4`, `espgarden5` (hardware v2 — 2 probes + LDR + DHT + 4 relays). CI builds all five in a matrix.
- **`pio run` does not need `data/config.json`**; `-t buildfs` / `-t uploadfs` do. `data/config.json` is gitignored. CI does `cp data/config.template.json data/config.json` — **never replicate that locally**, it destroys the real Wi-Fi/ThingSpeak/OTA credentials of a physical device.
- The ESP32 currently attached is on **COM7** (Silicon Labs CP210x). `upload_port`/`monitor_port` are unset, so PlatformIO auto-detects; pass `--upload-port COM7` when several boards are attached.
- **Host tests:** `pio test -e native` (CI gates the firmware build on them). `native` is the env; `test_*` is a filter, so `pio test -e test_accumulator` fails.
- **`[platformio] default_envs` excludes `native`**, so a bare `pio run` builds only the five firmware envs. Without it `pio run` tries to build the test environment as firmware and dies on Unity's config header.
- **There is no lint and no static-analysis configured.** `.clang-format` exists (Mozilla base, 4-space indent) but nothing enforces it — format only the lines you touch.

```bash
# ---- Frontend without hardware -------------------------------------------
python scripts/dev_server.py                  # http://127.0.0.1:8080, stdlib only
python scripts/dev_server.py --port 9000 --host 0.0.0.0
```

The simulator serves `data/` and mocks the device API — including the full nonce + SHA-256 login (`/nonce`, `/login`, `/logout`) and the same public/guarded split. Simulator credentials are fixed at **`admin` / `admin`** and printed at startup. **It is a second implementation of the device's HTTP contract** — a payload key, a route or an auth rule changed in `web.cpp` must be mirrored here, or the simulator silently drifts from the firmware.

```bash
# ---- Read the reference firmware (WSL) -----------------------------------
wsl.exe -e bash -lc 'cat ~/solarbot/fullbot-firmware/CLAUDE.md'
wsl.exe -e bash -lc 'ls ~/solarbot/fullbot-firmware/docs'
```

---

## Boot sequence — and why a device can hang in it

`setup()` runs in this order, and the order matters:

1. **`relayPinsSafeInit()`** — parks every relay at its idle level using the compiled defaults. Must stay first; see the relay constraint above.
2. `LED_BUILTIN` output; `logger` is constructed on first use and its constructor calls `Serial.begin(115200)`.
3. `id = ESP.getEfuseMac() % 0x10000`, printed as hex — this is the value that must appear in `config.json`'s `"id"`.
4. `SPIFFS.begin(true)` (formats on failure).
5. `loadConfigFile(id)` → applies `log.level` and re-parks the relays on their configured pins.
6. `g_ledBlinkEnabled = error` — set here, not after `tasksSetup()`.
7. `logger.backupSetup()` → rotates `/log0..3.txt` via `/current.txt`.
8. `webSetup()` → `WiFi.begin()`, mDNS, **all routes registered, server listening**.
9. `tasksSetup()` → pins, TalkBack, **critical runner started**, then **two blocking loops**.

**TRAP — `tasksSetup()` blocks `setup()` indefinitely.** It spins `while (!g_hasInternet)` pinging 8.8.8.8 / 8.8.4.4 / 1.1.1.1 every second, then `while (g_bootTime < g_safeTimestamp)` re-running NTP every 2 s. A device with Wi-Fi but no internet, or with a wrong SSID, **never leaves `setup()`**: no task ever runs, `loop()` is never reached, watering and MQTT never start. The web server *is* up (step 6 precedes it), so `/data.json` answers — with accumulators that have never been fed.

**TRAP — a bad config is still fatal, it is just visible now.** `ConfigFile::loadFile` returns false when `/config.json` is missing, unparseable, has an `"id"` that does not match this chip's efuse MAC, or has any credential string shorter than 4 chars. `main.cpp` then continues with **compiled defaults** (`ssid = "undefined"`), which cannot associate — so the device wedges in the loop above. The error blink does now run (`ledBlink` is a critical task enabled before the blocking waits, and `g_ledBlinkEnabled` is set before `tasksSetup()`), but the device is still stuck. **A slow-blinking LED means "config did not load".**

Three traps that used to live here are fixed; do not re-introduce them:

- `AccumulatorV2::getLast()` now returns 0 on an empty window instead of dereferencing `std::list::back()`.
- **Never read an accumulator from a request handler.** ESPAsyncWebServer runs handlers on the `async_tcp` task while the io task does `push_back`/`pop_front` on the same `std::list` at 1 Hz; walking a list across a `pop_front` dereferences a freed node. `/data.json` avoids it by being rendered from `ioTaskHandler` into a cached string the handler copies under a mutex.

  This trap was written down here **and violated anyway**, by `/moisture.json`, which needs live values rather than a cached payload. The fix generalises: `moistureReading(index)` returns a snapshot published by the io task under a spinlock, and `moistureState()` reads it too, so it is safe from any thread. **A new endpoint that needs a live sensor value uses that, or publishes its own snapshot the same way.** Grepping for `g_soilMoisture` outside `sensors.cpp` and the background tasks should find nothing.
- `g_otaEnabled` is cleared in `handleUpdateRequest`, so a single `/updateEnable` no longer arms the device until reboot.

---

## Task scheduler

`CriticalTaskScheduler` (`andrenepomuceno/CriticalTaskScheduler@^1.0.3`, aliases `TSTask` / `TSScheduler`). Tasks are declared with the `DECLARE_TASK(name, period)` macro at the top of `src/tasks.cpp`, which mints `name##TaskHandler`, `g_##name##TaskPeriod` and `g_##name##Task` together. Registration happens only in `tasksSetup()`.

| Task | Period | Bucket | Notes |
|---|---|---|---|
| `relays` | 50 ms | **critical** | Switches each relay off when its timer expires |
| `ledBlink` | 1 s | **critical** | Only blinks while `g_ledBlinkEnabled` |
| `io` | 1 s | background | All ADC reads, then `webUpdateDataCache()` |
| `dht` | 1 s | background | Enabled only when `config.dhtFitted`; tracks `g_dhtReadErrors` / `g_dhtTotalReads` |
| `checkInternet` | 15 s | background | |
| `history` | `history.periodSec` (60 s) | background | One `IoRecord` into the ring buffer; the 60 s here is only the fallback until `tasksSetup()` calls `setPeriod()` |
| `schedules` | 20 s | background | Fires a due schedule; three ticks a minute, with a 10 min catch-up window |
| `mqtt` | 1 min | background | Builds and publishes the payload for whichever backend is configured |
| `talkBack` | 1 min | background | Polls the TalkBack queue |
| `clockUpdate` | 24 h | background | NTP re-sync |
| `logBackup` | 1 h | background | `logger.backup()` |
| `moistureModel` | 24 h | background | Trains the classifier; three streaming passes over the history, so it stalls other background tasks for seconds |
| `checkMoisture` | 4 h | background | Armed by `startRelay(0, …)`, disables itself |

Both buckets are registered with the same `g_taskScheduler.addTask()`; `DECLARE_CRITICAL_TASK` passes `critical = true`, which routes the task to `executeCritical()` on the `TSFreeRTOSCriticalRunner` thread. **Register and enable every critical task before `g_criticalRunner.start()`** — the scheduler holds no internal locks and the library documents mutation-after-start as unsafe.

Consequences that bite:

- **`execute()` runs at most ONE background task per call** — the latest-due one. A handler that blocks stalls every other *background* task for its whole duration.
- **`checkInternetTaskHandler` blocks on up to three `Ping.ping(addr, 2)` calls**, `talkBackTaskHandler` on a socket with a 5 s timeout, and `mqttTaskHandler` on draining up to 30 queued messages over TLS. Relay switch-off and the error blink survive that only because they are critical — putting relay timing back on the cooperative pump means a pump that runs long.
- Background tasks reschedule from **end of callback**, so a slow handler pushes its own next run out; critical tasks reschedule from the tick time and keep a fixed cadence.
- **A request handler must never call `ESP.restart()` or block.** `request->send()` only *queues* the response and the `async_tcp` task that flushes it is the one the handler runs on, so both kill the connection before the client sees anything. `/control` sets a flag and `requestRestart()` reboots from `loop()` 500 ms later; moving the restart after the `send()` was tried first and was not enough.
- **`g_relay[]` is touched from three threads** — the critical runner, `loop()` (TalkBack) and `async_tcp` (`/control`). Every access goes through the `g_relayMux` spinlock, and `relayWrite()` is deliberately called *outside* it: a `digitalWrite` inside a `portENTER_CRITICAL` section is exactly the kind of work that must not hold a spinlock.
- **`Scheduler::addTask()` returns `bool` and every call site ignores it.** The cap is `CRITICALTASKSCHEDULER_MAX_TASKS` = **16** per bucket. This repo registers **10 background and 2 critical** today; one task per relay plus per sensor crosses the cap and tasks are then **silently dropped**. Either check the return value or raise it with `-D CRITICALTASKSCHEDULER_MAX_TASKS=32`.

---

## Configuration

`ConfigFile` is a singleton (`config`) whose members are also exposed as `g_*` **reference aliases** bound in `config.cpp`. Old code uses `g_*`; new code should prefer `config.<field>`. Do not add new aliases.

**Adding a config key = 5 edits, all required:**
1. field in `include/core/config.h`
2. default in the `ConfigFile()` constructor (`src/config.cpp`)
3. parse in `ConfigFile::loadFile()` — a sensor's pin is parsed inside `if (xFitted)`, and the fitted flag is `io.hasOwnProperty("key")`
4. key in `data/config.template.json`
5. row in the README config table

Facts worth knowing before touching it:

- **`config.id` must equal `ESP.getEfuseMac() % 0x10000` in hex.** A config from another device is rejected wholesale. The id is printed on every boot as `ID: 1a2b`.
- **A filesystem deploy overwrites `/config.json` with `data/config.json`, and a masked GET cannot be restored from.** Back up with `GET /config.json?secrets=1` (ADMIN, logged with the caller's IP) BEFORE any `uploadfs` or filesystem OTA, and write the result into `data/config.json`. Taking a "backup" through the normal masked GET loses every credential — eight asterisks each — and the loss only surfaces at the next boot.
- **`GET`/`POST /config.json` exist (both ADMIN) and secrets are masked in transit.** `GET` replaces `wifi.password`, `ota.password`, `thingSpeak.apiKey`, `talkBack.apiKey` and `mqtt.password` with `********` (`g_configSecretMask`); `POST` restores any field still carrying the mask from the document on disk. This deliberately diverges from fullbot, which serves the raw config — including plaintext credentials — to any authenticated session.
- **`POST /config.json` replaces the whole file**, exactly like fullbot's: `ConfigFile::saveFile()` opens with `FILE_WRITE` (truncate). The handler merges into the stored document first, so a caller only has to send a complete document, not a diff. **Nothing re-reads the file at runtime** — the response carries `restartRequired: true` and the change lands on the next boot.
- The handler rejects a document whose `id` does not match `config.deviceId`. Without that check a config pasted from another garden would be written, `loadFile()` would reject it at boot, and the device would come up on compiled defaults it cannot connect with — unreachable to fix without USB.
- **Changing `ota.password` is pushed into `UserStore` too.** The login password lives in `/users.json`, so a config-only change would otherwise not take effect until a filesystem deploy wiped the user store.
- **Every `io` entry accepts more than one shape, and all of them must keep working** — a device in the field has to survive a firmware update. `io.relays`: array of `{pin,on,name}`, or the pre-2.0 `io.watering` + `io.wateringOn` scalars. `io.soilMoisture`: array of `{pin,name}`, array of bare pins, or a single bare pin. `io.dht` / `io.luminosity` / `io.waterLevel`: `{pin,name}` or a bare pin. `loadSensor()` handles the sensor cases in one place. Entries past the compiled count are ignored and missing ones keep their defaults — a short array logs a warning rather than zeroing a pin.
- **Sensor and relay names are LABELS, not identifiers.** `/data.json` keys `Inputs` and `Outputs` by them, so renaming changes the dashboard immediately — but the `Relays` array stays index-addressed, the history record is positional, and the ThingsBoard telemetry keys stay `moisture1..N`. Renaming therefore never rewrites stored history. A name on `io.dht` is a *prefix*, because one pin produces two channels.
- **`"version"` in the JSON is still never read** — the template says `2` but nothing enforces or migrates on it. `log.level` *is* read now (clamped to `LOG_DISABLE..LOG_TRACE`) and applied in `loadConfigFile()`.
- **`loadFile()` mutates fields as it parses and only returns `false` at the end**, so a rejected config leaves a half-populated object behind.
- **`GET /config.json` is reachable** (matched by the trailing `serveStatic("/", SPIFFS, "/")`) and returns Wi-Fi, MQTT and OTA passwords in plaintext. It is behind HTTP Basic auth; nothing else protects it. `fullbot-firmware` fixed the equivalent by moving credentials into a salted-hash `UserStore` and shadowing the path with an explicit 403 handler registered *before* the static handler.

---

## Web server, endpoints & OTA

`src/web.cpp`, one `AsyncWebServer` on port 80, mDNS as `<hostname>.local`.

| Route | Method | Auth | Handler |
|---|---|---|---|
| `/`, `/index.*`, `/login.*`, `/update.*`, `/auth.js`, `/sha256.js`, `/favicon.ico` | GET | **public** | `servePublicFile()` — an explicit allow-list, no blanket `serveStatic` |
| `/nonce` | GET | **public** | issues the login challenge |
| `/login`, `/logout` | POST | **public** | `CustomLogin` |
| `/data.json` | GET | session | serves the cache built by the `io` task |
| `/control` | POST | **OPERATOR** | `relay`+`relayTime`, `watering`, `wateringTime`, `mqtt`, `reset` |
| `/history.json` | GET | session | last N I/O snapshots from the ring buffer, `?limit=` (cap 200) |
| `/logs` | GET | **ADMIN** | the whole 8 KB log buffer as `text/plain` |
| `/config.json` | GET/POST | **ADMIN** | read with secrets masked / write the whole document. `?secrets=1` exports verbatim for a restorable backup |
| `/config.html`, `/config.js` | GET | **public** | configuration editor in tabs (the data behind it is ADMIN) |
| `/history.html`, `/history.js` | GET | **public** | charts over 1 h / 6 h / 12 h / 1 d / 7 d / 30 d |
| `/users.html`, `/users.js` | GET | **public** | account management page (the data behind it is ADMIN) |
| `/schedules.html`, `/schedules.js` | GET | **public** | scheduled watering editor |
| `/moisture.html`, `/moisture.js` | GET | **public** | the classifier's inference and its fitted parameters (data behind it needs a session) |
| `/moisture.json` | GET | session | per-probe class means, spreads, priors, separation, and which gate refused an unclassified probe |
| `/capabilities.json` | GET | session | per-kind maxima, the kinds this build has drivers for, and the usable pins — so the UI never restates a rule the firmware owns |
| `/spiffs/upload` | POST | **ADMIN** | replace ONE file instead of rewriting the partition |
| `/updateEnable`, `/update` | POST | **ADMIN** | OTA arm + upload |
| `/users.json` | GET | **ADMIN** | usernames and roles only — never the salt or hash |
| `/users` | POST | **ADMIN** | `action=upsert\|delete`, `username`, `password`, `role` |
| `/devices.html`, `/devices.js` | GET | **public** | sensor + actuator management page (data behind it is ADMIN) |
| `/spiffs/*` | GET | **ADMIN** | file browse, with `users*` / `sessions*` / `config*` shadowed 403 |
| `/spiffs/delete` | POST | **ADMIN** | remove ONE file, refusing exactly what the upload refuses |

**Auth is nonce + SHA-256, not Basic Auth** (`USE_CUSTOM_LOGIN=1`, `src/custom_login.cpp`, ported from fullbot):

1. `GET /nonce?username=<u>` → `{nonce, salt, ttlMs}` — an unknown user still gets a deterministic decoy salt derived from `config.deviceId`, so the endpoint cannot enumerate accounts.
2. `passwordHash = sha256hex(salt + ":" + password)`
3. `response = sha256hex(nonce + ":" + passwordHash)`
4. `POST /login` with `username`, `nonce`, `response` (+ optional `remember=true`) → `{token, role, ttlMs}`
5. every later request carries header **`Authorization-Token: <token>`**

**TRAP:** `curl -u user:pass` returns 401 on every guarded route — there is no Basic-Auth path. Nonces are one-shot with a 30 s TTL; sessions idle out after 24 h; 5 failures from one IP → `429 Retry-After: 60`. `remember=true` persists the token to `/sessions.json`, so **a filesystem deploy signs everyone out**.

**A `Session` stores the user's INDEX, not the username.** `UserStore::remove()` erases from a `std::vector`, so every later entry shifts and a live session silently starts resolving to a different account — and to its role. `POST /users` with `action=delete` therefore calls `customLogin.invalidateAllSessions()` and answers `{"reauth":true}`; the page signs itself out. Any future code path that reorders the store owes the same call.

**There is no default password compiled into the firmware.** `UserStore::load()` seeds the first account by migrating `config.json`'s `ota.username` / `ota.password` into `/users.json` as ADMIN (salted SHA-256). A device whose config never loaded has no users, logs a FATAL, and the web UI is unreachable by design.

**Registration order is load-bearing.** ESPAsyncWebServer matches handlers in the order added, and `AsyncURIMatcher::prefix` is a prefix match — the 403 shadows for `/spiffs/users`, `/spiffs/sessions` and `/spiffs/config` must stay *above* the `serveStatic("/spiffs", …)` line, or the credential store, the live bearer tokens and the plaintext WiFi/MQTT passwords are served to any admin session.

**TRAP — an OTA client that times out has NOT necessarily failed.** A 1.2 MB
upload takes minutes; the device writes, verifies the MD5 and reboots, and by
then the HTTP connection the client was waiting on is long gone. Reading the
version 30 seconds after arming reports the OLD firmware because the old
firmware is still the one running. Both happened here and the second was
misdiagnosed as a failed flash. **Confirm by polling until the version changes
or the uptime resets, not by the upload's return value** — and run the upload
in the background, because a 10-minute tool timeout landing mid-write is how
this session already corrupted a filesystem once.

OTA details: `/updateEnable` arms a module-level `g_otaEnabled` which `handleUpdateRequest` clears again, so one arm buys one upload. `handleUpdateUpload` picks `U_SPIFFS` when the uploaded *filename* is exactly `filesystem`, else `U_FLASH`; `data/update.js` renames the file to the selected radio value (`firmware` | `filesystem`) and sends an `MD5` form field computed client-side with SparkMD5. On success the device `delay(500)` then `ESP.restart()`.

`/data.json` shape — the contract shared by `data/index.js`, `scripts/dev_server.py` and any future frontend:

```jsonc
{ "Status":  { "Hostname": "...", "Firmware": "2.0.0", "Uptime": "...", "Internet": "online|offline", ... },
  "Inputs":  { "Soil Moisture 1": { "val": "...", "avg": "...", "var": "..." }, ... },
  "Outputs": { "Watering": "0|1", "Relay 2": "0|1", ... },      // keyed by relay NAME
  "Relays":  [ { "index": 0, "name": "Watering", "on": 0, "remaining": 0 }, ... ],
  "Channel": "1348790" }
```

`Inputs` and `Outputs` keys are **human-readable labels, not identifiers** — `Outputs` is keyed by the configurable `relayName`, so it is display-only. Anything that needs to *address* a relay uses the `Relays` array and its `index`. With one probe the moisture label stays `"Soil Moisture"` (no suffix) so existing dashboards keep working; with two it becomes `"Soil Moisture 1"` / `"Soil Moisture 2"`.

The current UI (`data/index.html`) loads **Bootstrap and jQuery from CDNs**, so the dashboard degrades without internet — on a device whose whole point is surviving connectivity loss. `fullbot-frontend` bundles everything with Webpack and ships gzipped assets into `data/frontend/`; that is the fix.

---

## Sensors, accumulators & telemetry

Every sensor compiles into every image; `config.*Fitted` and `config.moistureCount` decide which are read. See [Runtime hardware](#runtime-hardware--the-build-no-longer-knows-what-is-fitted).

**Adding a sensor KIND = 8 edits** (adding an *instance* of an existing kind is now a web-UI edit): `config.h` pin field + fitted flag → `config.cpp` default, parse and `validatePins()` role → `config.template.json` → `sensors.cpp` accumulator + read → `sensors.h` extern → `telemetry.cpp` payload → `web_data.cpp` `Inputs` block (gated on the fitted flag) → `web_capabilities.cpp` kinds list → `dev_server.py` mock. Miss the last one and the simulator lies.

- Accumulator windows are sized as `g_mqttTaskPeriod / g_<source>TaskPeriod`, so each average covers exactly one publish interval: at today's 1 min publish that is **60 samples** for luminosity, moisture, water level and flow, 60 for the DHT, and 4 for the ping. `g_soilMoisture[]` is an array and cannot take a constructor argument, so `sensorsSetup()` calls `setMaxLen()` on each element explicitly — it used to rely on `AccumulatorV2`'s default window happening to match, which broke silently the moment a period changed.
- Conversions live in macros at the top of `tasks.cpp`: `ADC_TO_PERCENT(x) = x*100/4095`, moisture is **inverted** (`100 - pct`), and the water level uses a fitted curve `9 - 12*sin(4.04 - 1.61*V)`.
- **`AccumulatorV2` allocates on every sample** — a `std::list<float>` push/pop at 1 Hz, forever. That contradicts fullbot's "no dynamic allocation in steady state" rule. `fullbot-firmware`'s `TelemetryAggregator` is the drop-in replacement: fixed-size accumulators, `double` sums, non-finite samples dropped instead of poisoning the mean, and it publishes *both* the instantaneous reading and the window mean.
- **The DHT is placement-`new`ed into `g_dhtStorage` inside `tasksSetup()`**, not constructed at file scope. `DHT_Unified` copies the pin in its constructor, and at static-init time `config.json` has not been read — a file-scope instance permanently ran on the compiled default and silently ignored `io.dht`. Keep the construction late; the storage buffer exists so this costs no heap.
- The MQTT payload is accumulated into a global `String g_mqttMessage` by `mqttAddField()` / `mqttAddStatus()` **called from several tasks** (watering start, connectivity change) and flushed by `mqttTaskHandler`. Safe today only because one background task runs at a time; a critical task must not call it without a lock.
- The publish queue holds at most `60*60*1000 / g_mqttTaskPeriod` = **60 messages** (1 h) **in RAM**, dropped oldest-first, lost entirely on reboot. `fullbot-firmware` backs its queue to a file (`/telemetry_backup.json`) and drains it on reconnect.
- **TalkBack is plain HTTP on port 80** with the API key in the request body (there is a `// TODO use HTTPS` in `talkback.cpp`). The only command parsed is `watering:<ms>`, capped at 20 000 ms by `startWatering()`.
- **`data/thingspeak.pem` pins the CA for the MQTT TLS connection, and a stale pin fails silently for years.** It used to hold the *intermediate* `DigiCert TLS RSA SHA256 2020 CA1`; ThingSpeak now serves a chain under `DigiCert Global G2 TLS RSA SHA256 2020 CA1` / `DigiCert Global Root G2`, so every connect returned `-9984 X509 - Certificate verification failed` and channel 1348790 received nothing between **2023-03-07 and 2026-08-19**. Nothing surfaces this: `/data.json` keeps reporting `MQTT: enabled`, and only `Packages Sent` staying at 0 gives it away. It now pins the **root**, not the intermediate — roots last until 2038, intermediates rotate. Verify with `openssl s_client -connect mqtt3.thingspeak.com:8883 -showcerts` before assuming the file is current.
- **`mqttLoop()` retries the connection on every `loop()` iteration with no backoff**, so a TLS failure produces hundreds of log lines per minute and drowns everything else in the serial log. Worth a backoff if you touch `mqtt.cpp`.

---

## Soil moisture: a classifier trained on watering events

Three sources decide the Dry/Humid/Wet badge, in this order, and the ladder
matters — each rung is only reached when the one above refuses:

1. **The trained model** (`src/moisture_model.cpp`) — Gaussian naive Bayes,
   one Gaussian per class per probe, fitted from the probe's own history.
2. **The two-point calibration** (`moisture[i].dry` / `.wet`) — thirds of the
   probe's measured span.
3. **Nothing.** An empty string, and `/data.json` omits `state` entirely.

### Why the labels come from the relays and not from the data

Clustering the history was tried first and it produces confident nonsense.
Measured on 29.7 days / 20 417 samples: a linear drying trend alone explains
**86.4 %** of the variance (-0.323 points/day). With the trend removed, BIC on
the residuals prefers k=2, and that second component sits at **-24.5** — the
near-zero readings of a disconnected probe, not a soil state. Clustering the
*raw* series does return three groups (8.3, 35.9, 41.3), which are the outliers
plus two arbitrary slices of the trend: thresholds from them label "dry"
whatever comes late in any drying period.

The relay record is what changes the problem. A watering is an **event**, and
an event is a label:

| Window | Label | Why |
|---|---|---|
| `(T, T + 30 min]` after a watering | **Wet** | Not from T itself: absorption takes time, and the reading at the pump's own edge is still the old soil |
| `[T_next - 60 min, T_next)` | **Dry** | Arithmetic, not assumption — moisture decreases monotonically between waterings, so the minimum of a cycle is just before the next one |
| everything between | **Humid** | |
| before the first event in the buffer | *none* | There is no cycle to place the reading in |

`moisture[i].relay` says whose pump matters. **-1 means no pump feeds this
probe**, and such a probe never gets a model at all — nothing labels its
readings — so it falls through to the two-point calibration.

### Why training accumulates instead of refitting

The history buffer holds 24 h and a zone is watered once or twice a day. A
from-scratch daily fit therefore has **one or two events**, which is a
description of yesterday rather than a model. So `moistureModelTrain()`
multiplies the stored sufficient statistics by `g_moistureDecayPerRun` (0.93,
a half-life of about ten days) and folds the new day into them. All three
moments decay together, so the *estimate* is unchanged and only the confidence
in it ages — which is the intent: yesterday's soil is still evidence about
today's, just less of it.

Gaussian naive Bayes was chosen partly for this: it needs only `weight`, `sum`
and `sumSq` per class, so training **streams** over the history file
(`IoHistory::forEach`) instead of holding 69 KB of records in RAM, and the
whole model is 12 doubles per probe.

### The gates — refusing is the feature

`moistureModelIsUsable()` reports nothing unless **all** of these hold:

- **`g_moistureMinEvents` = 6 watering events** (decayed). A model fitted to
  one cycle describes that cycle.
- **`g_moistureMinWeightPerClass` = 20** accumulated weight in every class.
- **`g_moistureMinSeparation` = 4** — Fisher's `J = (μ_wet - μ_dry)² / (σ²_wet +
  σ²_dry)`, so the means are at least two pooled standard deviations apart. A
  probe watered so often it never dries lands near 0 here.
- **Ordering**: humid must lie *between* dry and wet. Not decoration — if it
  does not, the labelling disagrees with the physics that produced it, and
  every classification is a coin toss wearing a posterior. Polarity is not
  assumed, exactly as the two-point calibration does not assume it.

`/moisture.json` reports **which gate refused**, per probe. A classifier that
silently declines is indistinguishable from one that is broken.

### Traps

- **The fit is two passes, and it has to be.** Pass one fits everything; pass
  two refits rejecting samples beyond 3σ of that first fit. The rejection
  threshold is a function of the fit it protects, so one pass cannot do it.
  This is what keeps an out-of-family reading from dragging a class mean to a
  value the soil never had — the same component BIC found when the history was
  clustered blind.

  **It does not catch every disconnected probe, and it does not have to.**
  Measured on this board: with probe 3 unplugged, floating GPIO 32 reads 52.6
  with a variance of 0.01 — mid-scale, quiet, and indistinguishable from soil
  by value alone. The 3σ test never fires on it. What catches it is the
  SEPARATION gate: a disconnected pin does not respond to its pump, so dry,
  humid and wet all land on the same number, J collapses toward 0 and the
  model is refused. That is the gate earning its place — the probe is rejected
  for the reason it is actually broken.
- **All probes are trained in the same three passes**, not three passes each.
  This is a background task, and background tasks are cooperative: twelve
  passes over 1440 records on a four-probe board would stall MQTT and TalkBack
  for ten seconds once a day.
- **The sticky relay mask means one watering spans several records.** Only the
  rising edge counts, or a long watering becomes several events.
- **Variance is floored** at `g_gaussianVarianceFloor`. A class whose samples
  are identical has zero variance, an infinite log-likelihood, and wins every
  comparison regardless of the reading.
- **Likelihoods are computed in the log domain.** They differ by many orders of
  magnitude here; computed directly they underflow to zero for every class at
  once, which reads as a tie.
- **The model file is discarded on a layout change** (`MOI1` magic +
  `sizeof(MoistureModelState)`), not reinterpreted. Weeks of evidence are
  cheaper to rebuild than a wrong band is to notice.
- Training also runs **5 minutes after boot**, not only every 24 h: a device
  power-cycled each evening would otherwise never reach its daily tick.

`test/test_moisture_classifier/` covers the maths on the host — the ordering
gate, the separation gate, inverted polarity, the variance floor, the outlier
z-score, and that confidence tracks how much the classes actually overlap.

### The two-point fallback

The rung below the model, and the only one an uncalibrated device has.

Absolute thresholds come from **two reference readings per probe** — in air and
submerged. Each capacitive probe has its own gain and offset, which is exactly
why they cannot share a threshold.

- `config.json` carries a `moisture` array parallel to `io.soilMoisture`:
  `[{"dry": <air>, "wet": <submerged>, "relay": <index>}, ...]`.
- `moistureState(i)` (`sensors.cpp`) asks the model first, then falls back to
  thirds of that probe's own span, and returns **an empty string while
  `dry == wet`**. An uncalibrated probe with no model shows no badge rather
  than a fabricated one. `/data.json` only carries `state` when it is non-empty.
- **Ordering is not assumed anywhere, and the docs should not assert it
  either.** `include/core/config.h` says the air reading is the smaller number
  under the `100 - ADC%` conversion; an earlier version of this file said the
  larger. Neither was verified on hardware, and it cannot be settled without
  lifting a probe out of the soil. It does not need to be: both the two-point
  calibration and the classifier's ordering gate accept either direction, which
  is why the disagreement never produced a bug.
- **A floating input is not "in air".** A disconnected probe reads whatever the
  pin floats to, which is not a usable dry anchor. Do not assume it is a rail:
  the earlier "~94 here" in this file was not reproducible — measured
  2026-08-24, an unplugged probe on GPIO 32 sat at **52.6** with variance 0.01,
  squarely inside the range wet soil produces.
- `scripts/moisture_calibration.py` runs the same analysis off-device on any
  channel/field and states which question the data can answer. It refuses to
  report a drying rate over a window shorter than three days, after an early run
  extrapolated a 0.2-day window into "704 points/day".

---

## I/O history ring buffer

`/io_history.bin` holds the last N snapshots of every input and output. Sized
once at `begin()` and never grown: appends overwrite the oldest slot in place,
so flash usage is exactly `16 + capacity * 48` bytes forever and there is no
rotation to get wrong. Defaults are 1440 records at 60 s — 24 h of history in
69 KB of the 512 KB SPIFFS.

- **Both parameters are config-driven**: `history.records` (0 disables the
  feature) and `history.periodSec`. Both are range-checked at load, because a
  typo here asks for a file larger than the partition.
- **The record layout is fixed regardless of build flags.** A board with one
  probe still writes four moisture slots, filled with NaN, which serializes as
  `null`. Making the layout depend on the fitted probe count would mean two
  firmwares disagree about how to read the same file with no way to tell which
  wrote it. The header carries `recordSize` anyway, and any mismatch — magic,
  size, capacity, or file length — discards the file and reformats.
- **Do not lower `history.periodSec` toward 1 s.** SPIFFS rewrites a whole page
  per append, so the period is the flash-wear knob.
- **Growing `IoRecord` discards the stored history, on purpose.** The header
  carries `recordSize` and `begin()` reformats on any mismatch — reading
  40-byte records out of a 48-byte file would return garbage shaped like data.
  The record went 40 → 48 bytes when flow rate and the cumulative litres were
  added; the float switch rides in `flags` as two bits (VALID and RAISED,
  because "not fitted" and "reads empty" must not look alike) rather than
  costing another float. **`history.records` is capped in RECORDS, so the cap
  has to come down whenever the record grows** — it went 6000 → 5000 to stay at
  the same 240 KB.
- **`/history.json` caps a response at 200 records** (`g_historyMaxResponse`).
  1440 records is 57 KB of raw struct and roughly 170 KB rendered as JSON, more
  than half this chip's DRAM with WiFi already holding a share. The handler also
  keeps a `static IoRecord buffer[200]` — 9.6 KB of DRAM, visible in the build.
- The wrap-around arithmetic lives in `include/core/ring_index.h`, deliberately
  free of Arduino and SPIFFS so `test_ring_index` can reach it. A wrong answer
  there reorders history instead of failing, which is why it is the one part
  with unit tests.

---

## Runtime hardware — the build no longer knows what is fitted

`HAS_MOISTURE_SENSOR` and friends are **gone**, along with `RELAY_COUNT` and
`MOISTURE_SENSOR_COUNT`. Every driver compiles into every image; what a board
actually has comes from `config.json` and is counted at load:

| Was | Is | Decided by |
|---|---|---|
| `-D RELAY_COUNT=4` | `config.relayCount`, capped by `RELAY_MAX` (8) | length of `io.relays` |
| `-D MOISTURE_SENSOR_COUNT=3` | `config.moistureCount`, capped by `MOISTURE_MAX` (4) | length of `io.soilMoisture` |
| `-D HAS_DHT_SENSOR` | `config.dhtFitted` | `io.dht` key exists |
| `-D HAS_LUMINOSITY_SENSOR` | `config.luminosityFitted` | `io.luminosity` key exists |
| `-D HAS_WATER_LEVEL_SENSOR` | `config.waterLevelFitted` | `io.waterLevel` key exists |
| `-D HAS_FLOW_SENSOR` | `config.flowFitted` | `io.flow` key exists |
| `-D HAS_FLOAT_SWITCH` | `config.floatFitted` | `io.floatSwitch` key exists |
| `-D MOISTURE2_FIELD=n` | `config.thingSpeakMoisture2Field` | `thingSpeak.moisture2Field` |

**Presence IS the key.** There is no separate `enabled` flag, because one would
drift out of step with the pin it names. Deleting a sensor in `/devices.html`
means removing its key.

Measured before it was chosen: the minimal board was 1.166 MB and the fully
populated one 1.187 MB, so the whole `HAS_*` split was buying **21 KB out of a
1.69 MB slot**. What it cost was 75 `#ifdef` sites, five build shapes to keep in
step, and a sensor you could not add without a toolchain.

**What is still compile-time, and must stay so:** the set of KINDS. A DHT needs
the DHT driver linked in; no web page adds a kind there is no code for.
`GET /capabilities.json` publishes the kinds, the maxima and the usable pins so
the UI never restates a rule the firmware owns.

Traps this created, every one of which was a real defect first:

- **`relayPinsSafeInit()` runs over `RELAY_MAX`, not `relayCount`** — at its
  first call, before the config is read, the count is not known and a floating
  pin on an active-low board reads as energise. So slots past the count must
  have **no pin**: `clearUndeclaredRelayPins()` sets them to `kNoPin` after the
  load, or the second call force-drives the compiled defaults over whatever the
  config gave those GPIOs to.
- **`kNoPin` has to be honoured everywhere, not just at init.** A relay row
  saved without a pin is startable otherwise: the index check passes,
  `startRelay()` reports success, the dashboard counts down, and
  `digitalWrite(255, …)` does nothing.
- **Dropping an `#ifdef` is not the same as deleting it.** Every guard removed
  needs a runtime test put back in its place. Missing ones made `/data.json`
  report a confident `0.00` for sensors that were not there, and made the
  history record write `FLOAT_VALID` with "lowered" on a board with no float —
  the exact case that flag exists to distinguish.
- **A count constant is not a presence test.** `MOISTURE_MAX == 1` is always
  false, so the single-probe `"Soil Moisture"` label — the `/data.json` key
  dashboards have read for years — silently became `"Soil Moisture 1"`. The
  test belongs where `moistureCount` is known.
- **Bounds that were compile-time are now per-device.** A schedule aimed at
  relay 3 on a two-relay board has to be rejected at load, not fire daily into
  `startRelay()`'s index check.
- **`config.floatInterlock` cannot outlive `io.floatSwitch`.** `loadFile()`
  clears it when no switch is declared; otherwise removing the sensor leaves a
  veto that refuses every watering on a reading nothing produces.

Pin rules live in exactly one place — `pinIsADC1`, `pinIsInputOnly`,
`pinIsFlash`, `pinIsStrapping` in `config.cpp`. `validatePins()` uses them at
boot, `documentPinsAreUsable()` refuses a bad map at **save** time (boot is too
late: the document is already on flash), and `/capabilities.json` derives the
UI's pin lists by walking every GPIO through the same predicates.

## Relay seams — why `startRelay()` is the only door

`src/relays.cpp` owns switching and nothing else. Two seams connect it to the
rest of the firmware, both implemented in `tasks.cpp`:

- **`relayStartAllowed(index, reason)`** — asked BEFORE the relay is claimed.
  Today it is the reservoir interlock (`io.floatSwitch.interlock`): it refuses
  a pump while the float reads empty, and exempts `fillRelay`, because blocking
  the only thing that fixes an empty tank would deadlock the interlock against
  itself. Put a new veto here, never at a call site — there are five paths to a
  pump (web `/control`, TalkBack, a schedule, a ThingsBoard RPC,
  `checkMoisture`) and the one that gets forgotten is the one that runs dry.
- **`relayStartedHook(index, duration)`** — called after energising, for the
  watering bookkeeping (ThingSpeak field 2, the pre-watering moisture snapshot,
  arming `checkMoisture`) that switching itself knows nothing about.

`startRelay()` returns `bool`. A refusal used to be a log line and silence;
every caller can now say why nothing happened, which is what makes the RPC
answer `{"ok":false,"error":...}` instead of a lie.

**The interlock defaults to off** and must stay that way: an unwired float sits
at the internal pull-up, which reads as empty, so a default-on interlock would
stop every watering on every board that declares `io.floatSwitch` without a
sensor actually wired to it. There is deliberately no auto-refill — one float failing toward
"empty" would hold the fill relay on, and that failure mode is a flood.

## ThingsBoard downlink — RPC and firmware over MQTT

`src/thingsboard.cpp`. Inert unless `config.mqttBackend == "thingsboard"`; the
ThingSpeak path never reaches it. Two config gates: `mqtt.rpc` and
`mqtt.fwUpdate`, both default true, plus `mqtt.fwTitle` (default `esp-garden`).

| Topic | Direction | Purpose |
|---|---|---|
| `v1/devices/me/attributes` | both | client attributes out (`current_fw_title`/`current_fw_version`), shared-attribute push in |
| `v1/devices/me/attributes/request/<id>` → `.../response/<id>` | out/in | ask for the `fw_*` shared keys on every connect |
| `v1/devices/me/rpc/request/+` → `.../response/<id>` | in/out | `getStatus`, `getRelays`, `startRelay`, `startWatering`, `stopRelay`, `getFirmware`, `checkFirmware`, `restart` |
| `v2/fw/request/<id>/chunk/<n>` → `v2/fw/response/...` | out/in | the firmware stream, 4 KB per packet |

**TRAP — nothing may publish from inside the PubSubClient callback.**
PubSubClient uses ONE buffer for both directions, and the `payload` handed to
the callback points into it; publishing from there overwrites the packet being
processed. `tbHandleMessage()` therefore only records intent and every outbound
message leaves from `tbLoop()`, which `mqttLoop()` calls *after*
`mqttClient.loop()` returns. The outbox is a bounded `std::vector` and a full
one logs an error rather than dropping silently — a lost RPC reply looks like a
device that ignored the command.

**The receive buffer must hold a whole chunk.** `mqttSetup()` calls
`tbRequiredBufferSize()` before connecting. Undersize it and PubSubClient
discards every chunk without a word: the download sits at 0 % forever.

Guards that are not in fullbot's version and should not be removed:

- **`fw_title` must match `config.mqttFwTitle`.** One tenant holds every device
  an operator owns; assigning the wrong package to a device profile is one
  wrong click, and an image built for another board needs USB to recover from.
- **A download will not start while any relay is energised.** It ends in a
  reboot, and on an ESP32 reset every GPIO floats until `relayPinsSafeInit()`
  runs — which on these active-low boards means the pump switches back on for
  the length of the boot. `tbLoop()` holds the announcement until relays idle.
- **Both OTA paths share one `Update` object.** `/updateEnable` answers `409`
  while `tbFotaInProgress()`, and the cloud path waits on `Update.isRunning()`.
  Letting both run corrupts whichever finishes second, and `Update` reports
  success for it — the damage only shows at the next boot.
- **A stalled download aborts after 5 retries**, releasing `Update`. Otherwise
  an abandoned cloud FOTA locks out the browser OTA that exists to recover from
  a bad cloud FOTA.
- **`fwVersionDiffers()` accepts a downgrade.** Rolling back is an operator
  decision made in ThingsBoard, and it is the only recovery that does not need
  USB. `test/test_fw_version/` covers the comparison, including the
  lexicographic trap (`2.10.0` vs `2.9.0`).

The filesystem image cannot be delivered this way: `handleUpdateUpload` picks
`U_SPIFFS` from the uploaded *filename*, and the FOTA path always writes
`U_FLASH`. `spiffs.bin` still goes through `/update.html`.

`mqttLoop()` now backs off exponentially (1 s → 60 s) instead of retrying every
`loop()` iteration, which used to bury the reason for a refused connection in
hundreds of identical log lines a minute.

## Porting from fullbot-firmware

What the user asked for. Ordered by value, with the real blockers.

| Bring over | From | Status |
|---|---|---|
| Custom partition table | `partitions/hw_v2_0_4mb.csv` | **Done** — `partitions/esp_garden_4mb.csv`. Needs a serial reflash on every existing board |
| `CustomLogin` + `UserStore` + `Role` | `src/network/CustomLogin.cpp`, `src/core/UserStore.cpp` | **Done** — `src/custom_login.cpp`, `src/user_store.cpp`, seeded from `ota.*` instead of a compiled password |
| Critical-task pattern | `src/core/Controller.cpp` + `docs/task-scheduler.md` | **Done** — relay timing and the error blink |
| Cached status payload | `handlers/StatusHandlers.cpp` | **Done** — `webUpdateDataCache()` |
| `WebServer` + `handlers/` split | `src/network/` | **Done** — `web.cpp` keeps the route table; `web_data`, `web_config`, `web_ota`, `web_users`, `web_files`, `web_capabilities`, `web_moisture` hold the handlers |
| `FOTA` class | `src/core/FOTA.cpp` | Not done. The free functions work but do not share fullbot's structure |
| `ConfigFile::saveFile()` + `POST /config.json` | `src/core/ConfigFile.cpp`, `handlers/ConfigHandlers.cpp` | **Done** — plus a masked `GET`, an id check and a refuse-to-save guard that fullbot does not have |
| `/users.json` + `POST /users` management UI | `handlers/UsersHandlers.cpp` | **Done** — plus guards fullbot lacks: last-admin demotion, password length, and session invalidation on delete |
| `Logger` with `webRead()` + size-based rotation | `src/core/Logger.cpp` | Not done. This repo's logger rotates only on boot and always returns the whole 8 KB buffer |
| `TelemetryAggregator` | `src/core/TelemetryAggregator.cpp` | Not done. Would replace `AccumulatorV2` and its steady-state allocation, but the UI depends on `val`/`avg`/`var` and the aggregator exposes only a mean |
| Webpack frontend | `~/solarbot/fullbot-frontend` | Not done, but the reason it mattered is gone: Bootstrap and jQuery are vendored and gzipped, so nothing degrades offline. What a bundler would still buy is dead-CSS elimination — 29.9 KB of Bootstrap for the handful of classes these pages use |
| `SelfTest` | `src/core/SelfTest.cpp` | Not done. A boot that reports a dead moisture probe is worth more than a season of unusable data |
| `test/` harness (`[env:native]`) | `platformio.ini` + `test/support/native_includes/` | **Started** — env, CI job and `test_accumulator`. The stub layer for SPIFFS/JSON/FreeRTOS is still to transplant |

Concrete gotchas measured in this tree:

- **The partition change is the reason anything else fits.** On the stock table `espgarden5` sat at 83.8 % of a 1.31 MB app slot (~210 KB free); on `partitions/esp_garden_4mb.csv` it is at **63.1 % of 1.69 MB (~652 KB free)**. The whole auth stack cost ~10 KB. **This cannot be delivered over OTA** — the running image would be rewriting the partition map underneath itself, so every board already flashed needs one serial upload to move over.
- **`lib_deps` used to point at a bare `me-no-dev/ESPAsyncWebServer` git URL with no tag.** That repo moved to the ESP32Async org, so the dependency silently tracked whatever HEAD was. It is now pinned to `ESP32Async/ESPAsyncWebServer@^3.7.8` + `ESP32Async/AsyncTCP@^3.4.4` (resolves to 3.12.0). **3.7+ is required for `AsyncURIMatcher`**; middleware alone works from 3.3.
- **3.7+ no longer pulls in `WiFi.h` transitively.** `web.cpp` includes it explicitly; a file that used to compile on the old fork may now need the same.
- **`Arduino_JSON` is bumped to `^0.2.0`** to match fullbot, so code copied across needs no adaptation. Its quirks apply here too: build nested payloads by writing through `parent[key][...]`, never by returning a `JSONVar` by value, and never subscript a `const JSONVar`.
- **`JSONVar::operator[]` returns BY VALUE, and this has already cost a live incident.** Casting a chained subscript straight to `const char*` — `(const char*)doc["wifi"]["password"]` — reads a buffer the temporary has already freed and silently yields an **empty String**, not the value. In `handleConfigPost` that made every mask comparison fail, so `POST /config.json` wrote eight asterisks over the real WiFi, OTA, ThingSpeak, TalkBack and MQTT credentials; the device kept running on its in-memory config and would only have died at the next boot. **Always bind to a named `JSONVar` local before converting**, and assign `String::c_str()` rather than another `JSONVar` (move-assign from an rvalue yields a null child). The handler now also refuses to save a document that still carries a mask.
- **Verify a config write by the byte count, not by reading it back.** `GET /config.json` masks secrets, so a clobbered credential and an intact one look identical. `saveFile()` logs `Saved /config.json (N bytes)` and `/logs` is ADMIN-readable — compare N against the compact length of the known-good document. That check is what caught the incident above before a reboot.
- Fullbot runs on **ESP32-S3 with PSRAM**; these boards are plain ESP32, 320 KB DRAM, none. Anything sized for PSRAM (the ML models, large static buffers) does not come across.
- **Both brokers are supported now**, selected by `mqtt.backend` (`thingspeak` | `thingsboard`). The transport block is shared; only the topic and the payload differ. ThingsBoard authenticates with the device access token in `mqtt.username` and an empty password, and `mqtt.useTLS = false` selects a plain `WiFiClient` for a self-hosted broker on 1883 — TLS costs 30–45 KB of heap for the handshake.
- **ThingsBoard is the way out of the 8-field ceiling.** Its JSON payload carries every probe, each probe's Dry/Humid/Wet state, every relay, the DHT error rate and `FW_VERSION` with no field numbering to negotiate — which is what keeps `thingSpeak.moisture2Field` an open question on ThingSpeak and not on ThingsBoard.
- Still to take from fullbot's `MQTTClient`: the file-backed publish queue, the reconnect backoff, and shared-attribute FOTA.

---

## Conventions

- **English in code** — identifiers, comments, log strings, commit messages. Reply to the user in the language they write in (Portuguese here).
- **`.clang-format` is Mozilla base, 4-space indent.** Nothing enforces it; do not reformat lines you did not touch.
- Includes are prefixed: `core/`, `network/`. `BuildConfig.h` carries no prefix.
- Feature flags live in `include/BuildConfig.h` (`FW_VERSION`, `USE_WEBSERVER`, `USE_MQTT`, `USE_OTA`, `USE_TALKBACK`, `USE_WATERING_PWM`), along with the capacity caps `RELAY_MAX` and `MOISTURE_MAX`. **`platformio.ini` carries no hardware flags at all** — what a board has is in its own `config.json`, and the envs differ only by `board`.
- `FW_VERSION` in `BuildConfig.h` is the version ThingsBoard compares an offered package against and the string `/data.json` reports as `Status.Firmware`. **Bump it in the same commit as the change it names**, or a FOTA of that change is a no-op the broker reports as success.
- **No AI co-author trailers** in commits. **Never `git add -A`** — stage explicitly. **No commits or pushes without an explicit instruction** from the user; suggest the message and stop.
- Commit format: imperative + conventional tag (`feat | fix | refactor | chore | docs | test | perf | style`), e.g. `fix(tasks): construct DHT after config load`.
- **Never overwrite `data/config.json`** (gitignored, holds a real device's credentials) and never commit real credentials — only `data/config.template.json` is tracked.
- Do not create new `.md` files unless asked; update this file or `README.md`.

## End-of-change checklist

- [ ] `pio test -e native` passes; new behaviour in testable code has a regression test
- [ ] `pio run` builds all five envs clean (they share one shape now, so a break in one is a break in all)
- [ ] `-t buildfs` run if `data/` changed, and **flash usage still under 100 %** of the app slot
- [ ] `/data.json` payload changes mirrored in `data/index.js` **and** the simulator (`scripts/sim_state.py` for the payload, `sim_config.py` for the document, `sim_moisture.py` for the model, `dev_server.py` for the route and `PUBLIC_PATHS`)
- [ ] New config key: all 5 edits, template included; new sensor KIND: all 8
- [ ] `data/config.json` untouched, no credentials introduced
- [ ] `python scripts/check_lines.py` passes
- [ ] **[What has actually run](#what-has-actually-run) is still honest** — anything that ran moved up *because it ran*, and anything newly written went into the unverified list
- [ ] No number in this file without an execution behind it
- [ ] Conventional commit message suggested — **no commit unless the user asked**
