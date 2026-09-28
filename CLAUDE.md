# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Operating guide for **esp-garden**. [README.md](README.md) is the entry point and [docs/](docs/) holds the reference documentation, one file per subsystem.

**Reference repo:** `fullbot-firmware` (ESP32-S3 firmware for the FullBot solar-panel cleaning robot), at `~/solarbot/fullbot-firmware` **inside WSL** (`wsl.exe -e bash -lc '...'` from Windows — it is not on a `/mnt` path). It has its own `CLAUDE.md` plus a `docs/` directory. The user's explicit instruction is to **reuse it freely** — scheduler, MQTT, web UI, web server, OTA, config, logging. Read it before designing anything new here; most problems this repo is about to hit are already solved there. See [Porting from fullbot-firmware](docs/porting-fullbot.md) for what transplants cleanly and what does not.

## Project at a glance

ESP32 firmware for an automatic garden: soil moisture + luminosity + DHT11 or
SHT40 + optional water level, a watering relay, a local async web dashboard,
ThingsBoard MQTT/TLS telemetry, ThingsBoard RPC and browser OTA. **The
ThingSpeak uplink and TalkBack are compiled OUT by default** since 2.12.0.

| Layer | Path |
|---|---|
| Entry point | `src/main.cpp` |
| Orchestration, the single task-registration site | `src/tasks.cpp` |
| Task events: the async half of the io tick, and the two relay seams | `src/task_events.cpp` |
| Relays | `src/relays.cpp` |
| Sensors | `src/sensors.cpp`, `src/sht4x.cpp` |
| Telemetry | `src/telemetry.cpp` |
| Config | `src/config.cpp`, `config_io.cpp`, `config_pins.cpp`, `config_document.cpp` |
| Web | `src/web.cpp` + `web_*.cpp` |
| Onboarding | `src/web_onboarding.cpp` |
| Auth | `src/custom_login.cpp`, `src/user_store.cpp` |
| MQTT / ThingsBoard | `src/mqtt.cpp`, `src/thingsboard.cpp` |
| History | `src/io_history.cpp` |
| Models | `src/moisture_*.cpp`, `src/cloud_*.cpp`, `src/et0_model.cpp`, `src/evapotranspiration.cpp` |
| Web sources | `data/`, built into `.pio/assets/` by `scripts/build_assets.py` |
| Tooling | `scripts/` |
| Host tests | `test/`, under `[env:native]` |

[docs/architecture.md](docs/architecture.md) has the full table, what each file
owns, and why `tasks.cpp` was split twice.

**No source file exceeds 1000 lines, and `python scripts/check_lines.py` is
what says so.** The gate exists because the honour system had already failed:
four files crossed it unnoticed.

**The pattern for making something testable is to put the arithmetic in an
Arduino-free header** (`segment_index.h`, `step_publisher.h`, `pin_rules.h`,
`sht4x_protocol.h`) rather than to stub the platform. A stub is small enough to
look obviously right, and wrong in a way that produces plausible answers
instead of failures.

---

## Rules that bite

Every one of these was a defect first. The full story is in the linked
document.

- **Relays are active-low and a floating pin reads as "energise".**
  `relayPinsSafeInit()` is the first statement of `setup()` and runs again once
  the real pins are known. Do not move either call later.
  ([docs/relays.md](docs/relays.md))
- **A new veto on watering goes in `relayStartAllowed()`, never at a call
  site.** There are five paths to a pump, and the one that gets forgotten is
  the one that runs dry. ([docs/relays.md](docs/relays.md))
- **Never read an accumulator from a request handler.** Handlers run on
  `async_tcp` while a background task mutates the same window. `/data.json` is
  rendered from the io task into a cached string; anything needing live values
  reads a snapshot published under a spinlock.
  ([docs/sensors.md](docs/sensors.md))
- **Compute outside a spinlock; the critical section is a copy.** Calling
  `getAverage()` inside one panicked this board with `InterruptWDTTimoutCPU1`.
  ([docs/soil-moisture-sampling.md](docs/soil-moisture-sampling.md))
- **A request handler must never call `ESP.restart()` or block.**
  `request->send()` only queues the response, and the task that would flush it
  is the one the handler runs on. Use `requestRestart()`.
  ([docs/web-interface.md](docs/web-interface.md))
- **Route registration order is load-bearing.** ESPAsyncWebServer matches in
  order and `prefix` is a prefix match: the 403 shadows for `/spiffs/users`,
  `sessions` and `config` must stay above the `serveStatic("/spiffs", ...)`
  line, or the credential store and the live bearer tokens are served.
  ([docs/web-interface.md](docs/web-interface.md))
- **`addTask()` returns `bool` and every call site ignores it.** The cap is 16
  per bucket; past it, tasks are dropped silently.
  ([docs/task-schedule.md](docs/task-schedule.md))
- **Anything shorter than the publish period is an event or a sticky flag,
  never sampled.** A watering lasts seconds and the payload is built every five
  minutes. ([docs/telemetry.md](docs/telemetry.md))
- **A sticky mask is take-and-clear, so it is per CONSUMER.** One shared mask
  means whichever reader arrives first steals the event from the other.
  ([docs/telemetry.md](docs/telemetry.md))
- **`config.id` must equal `ESP.getEfuseMac() % 0x10000`.** A config from
  another chip is rejected wholesale, which means compiled defaults, `ssid
  "undefined"` and a board that cannot be reached to fix.
  ([docs/configuration.md](docs/configuration.md))
- **Presence IS the key.** A sensor is fitted if and only if its `io` key
  exists. There is no `enabled` flag to drift out of step with the pin it
  names. ([docs/configuration.md](docs/configuration.md))
- **`JSONVar::operator[]` returns BY VALUE.** Casting a chained subscript to
  `const char*` reads a freed buffer and silently yields an empty String; it
  once wrote eight asterisks over every credential. Bind to a named local
  first. ([docs/porting-fullbot.md](docs/porting-fullbot.md))
- **A filesystem deploy overwrites `/config.json`, the history and the
  moisture model.** Back up with `GET /config.json?secrets=1` first, because a
  masked GET cannot be restored from. Changed only files under `data/`? Use
  `POST /spiffs/upload`. ([docs/configuration.md](docs/configuration.md))
- **In-place mutation of a large file is what panicked this board under
  LittleFS.** Any copy-on-write filesystem turns a random write into a
  whole-file rewrite. ([docs/io-history.md](docs/io-history.md))
- **A stale CA pin fails silently for years.** Pin roots, never intermediates,
  and verify with `openssl s_client` before assuming a `.pem` is current.
  ([docs/telemetry.md](docs/telemetry.md))
- **An OTA client that times out has NOT necessarily failed.** Confirm by
  polling until the version changes, not by the upload's return value.
  ([docs/web-interface.md](docs/web-interface.md))

---

## Essential commands

PlatformIO is **not on `PATH`** on Windows. Use the penv binary; in WSL `pio` is on `PATH` directly.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
& $pio run -e espgarden1                      # compile only  (~9 s, verified)
& $pio run -e espgarden1 -t upload            # flash APP ONLY
& $pio run -e espgarden1 -t buildfs           # builds .pio/assets first, then littlefs.bin
& $pio run -e espgarden1 -t uploadfs          # TRAP: overwrites the device's /config.json
& $pio device monitor -b 115200               # serial @115200
```

- Envs: `espgarden1` (NodeMCU-32S), `espgarden2`, `espgarden3`, `espgarden4`, `espgarden5` (hardware v2) — all ESP32-WROOM-32 — plus `espgarden_s3` (**ESP32-S3**, the esp-garden-hardware carrier). CI builds all six in a matrix, and provisions `data/config.json` from a different template for the S3.
- **The filesystem image is BUILT from `data/`, never packed from it.** `[platformio] data_dir = .pio/assets` and a `pre:` hook run `scripts/build_assets.py` on every invocation, so there is no step to forget. `python scripts/build_assets.py --list` shows what it would produce.
- **`pio run` does not need `data/config.json`**; `-t buildfs` / `-t uploadfs` do. `data/config.json` is gitignored. CI does `cp data/config.template.json data/config.json` — **never replicate that locally**, it destroys the real Wi-Fi/ThingSpeak/OTA credentials of a physical device.
- Three boards have been on this workstation, and **the port belongs to the DevKitC, not to the carrier** — the carrier sockets a whole devkit and uses that devkit's own CP2102, so replacing the module moves the port number. **COM7** (Silicon Labs CP210x) is the WROOM-32 DevKit v1 built by `[env:espgarden2]` since the NodeMCU-32S died. **COM12** was device `b580`'s devkit, retired 2026-09-26. **COM5** is its replacement, device `cfd0`, in the same `esp-garden-hardware` carrier, built by `[env:espgarden_s3]`. `upload_port`/`monitor_port` are unset, so PlatformIO auto-detects; pass `--upload-port COM5` when both are attached, because auto-detect picks one of them and an S3 image on a WROOM-32 is a serial recovery. **Opening the S3's port with pyserial resets that board** — every boot of both modules has come up `POWERON` — so a passive listen is not passive there, and probe-health sampling starts again from zero every time. **Identify by MAC rather than by port**: esptool prints it, and `mac[0] | mac[1] << 8` is the id (`80:b5:4e` → `b580`, `d0:cf:13` → `cfd0`, `24:62:ab` → `6224`).
- **Host tests:** `pio test -e native` (CI gates the firmware build on them). `native` is the env; `test_*` is a filter, so `pio test -e test_accumulator` fails.
- **`[platformio] default_envs` excludes `native`**, so a bare `pio run` builds only the five firmware envs. Without it `pio run` builds the test environment as firmware and dies on Unity's config header.
- **There is no lint and no static analysis configured.** `.clang-format` exists (Mozilla base, 4-space indent) but nothing enforces it — format only the lines you touch.

```bash
python scripts/dev_server.py                  # http://127.0.0.1:8080, stdlib only
wsl.exe -e bash -lc 'cat ~/solarbot/fullbot-firmware/CLAUDE.md'
```

The simulator serves `data/` and mocks the device API — including the full nonce + SHA-256 login and the same public/guarded split. Credentials are fixed at **`admin` / `admin`** and printed at startup. **It is a second implementation of the device's HTTP contract** — a payload key, a route or an auth rule changed in `web.cpp` must be mirrored here, or the simulator silently drifts from the firmware.

`python scripts/dev_server.py --onboarding arm1` (or `arm2`) serves the **setup portal** instead of the normal UI, with the same all-or-nothing route split the firmware has. It is the only place that page can be rendered until a board exists, so its page and its templates are **extracted** from `src/web_onboarding.cpp` and `include/core/onboarding_templates.h` rather than copied; the refusals are reimplemented, which is what a mirror is for. `ESP_GARDEN_PIN_FAMILY=esp32s3` switches which templates it offers.
```bash
python scripts/history_export.py --plan          # cost + cadence, no network
python scripts/history_export.py                 # collect from espgarden1.local
python scripts/history_export.py --stats         # span, gaps, watering events
python scripts/moisture_fit.py --history-db      # fit from the 60 s record

# The simulator's history knobs. A rotation, a paging walk and a hole in the
# record take HOURS at the device's 60 s period; these make them reachable.
python scripts/dev_server.py --history-period 1 --history-records 24
python scripts/dev_server.py --history-gap 7200 --history-garden
python scripts/dev_server.py --history-rotate-every 2   # FAULT INJECTION
```

---

## Boot sequence — and why a device can hang in it

`setup()` runs in this order, and the order matters:

1. **`relayPinsSafeInit()`** — parks every relay at its idle level using the compiled defaults. Must stay first; see the relay constraint above.
2. `LED_BUILTIN` output; `logger` is constructed on first use and its constructor calls `Serial.begin(115200)`.
3. `id = ESP.getEfuseMac() % 0x10000`, printed as hex — the value that must appear in `config.json`'s `"id"`.
4. `FILESYSTEM.begin(true)` (LittleFS; formats on failure).
5. `loadConfigFile(id)` → applies `log.level` and re-parks the relays on their configured pins. **Its answer is kept**: it is the first arm of the onboarding decision and is passed to `webSetup()`.
6. `g_ledBlinkEnabled = error` — set here, not after `tasksSetup()`.
7. `logger.backupSetup()` → rotates `/log0..3.txt` via `/current.txt`.
8. `webSetup(configLoaded)` → `WiFi.begin()`, **the mode decision**, mDNS, **all routes registered, server listening**.
9. `tasksSetup()` → pins, TalkBack, **critical runner started**, then **two blocking loops** — all of it skipped but the two critical tasks when the portal is up.

**`webSetup()` is where a board decides whether it is a garden controller or a setup portal, and that is deliberately ONE branch.** An early return at the top registers either the three unauthenticated onboarding routes or the normal table, never both, so "does an endpoint that rewrites `/config.json` without a token exist on this device" is a question a reader answers by reading one `if`. See [First-boot onboarding](docs/onboarding.md).

**TRAP — `webSetup()` can now block for up to 60 s, and only ever on one specific board.** When `/config.json` loaded AND `/provisioned.pending` exists, it polls `WiFi.status()` before deciding. That is the boot after onboarding and no other: the first association deletes the marker, so a configured board never enters the branch and `onboarding::mustProbeAssociation()` is host-tested for exactly that.

**That 60 s is spent with the critical runner NOT yet started**, because `g_criticalRunner.start()` is in step 9 and this is step 8 — so relay timing and the error blink are down for it, which is a window that did not exist before. It is survivable for one reason and it should be checked against any future change: **no relay can be energised during it.** `relayPinsSafeInit()` has run twice, the web server has not called `begin()` yet, there is no schedule task, no TalkBack and no MQTT, so nothing in the firmware has a path to `startRelay()`. Moving the probation after `g_webServer.begin()` is not possible — the route table is what the decision picks.

**TRAP — `tasksSetup()` blocks `setup()`, but no longer forever.** It spins `while (!g_hasInternet)` pinging 8.8.8.8 / 8.8.4.4 / 1.1.1.1 every second, then `while (g_bootTime < g_safeTimestamp)` re-running NTP every 2 s. **Both are capped at `g_bootWaitMaxMs` = 60 s**, so a device with Wi-Fi but no internet reaches `loop()` after two minutes instead of never — which is what makes watering and MQTT start at all behind a captive portal. Two minutes of a dead `loop()` is still two minutes: relay timing and the error blink survive it only because they are CRITICAL tasks on their own runner. The web server is up throughout (step 8 precedes step 9), so `/data.json` answers with accumulators that have never been fed.

**A bad config used to be fatal. It is now the first arm of the onboarding decision.** `ConfigFile::loadFile` returns false when `/config.json` is missing, unparseable, has an `"id"` that does not match this chip's efuse MAC, or has any credential string shorter than 4 chars. `main.cpp` still continues with **compiled defaults** (`ssid = "undefined"`), which cannot associate — but instead of wedging in the loop above, `webSetup()` raises the setup AP. **A slow-blinking LED now means "this board needs attention": the config did not load, or it is serving the setup portal.** Arm 2 lights it explicitly from `main.cpp`, because there the config loaded perfectly and only the credentials in it are wrong, so `error` is false.

Three traps that used to live here are fixed; do not re-introduce them:

- `AccumulatorV2::getLast()` returns 0 on an empty window instead of dereferencing `std::list::back()`.
- **Never read an accumulator from a request handler.** ESPAsyncWebServer runs handlers on the `async_tcp` task while the io task mutates the same window at 1 Hz. `/data.json` avoids it by being rendered from `ioTaskHandler` into a cached string the handler copies under a mutex. **This trap was written down here and violated anyway**, by `/moisture.json`, which needs live values rather than a cached payload. The fix generalises: `moistureReading(index)` returns a snapshot published by the io task under a spinlock, and `moistureState()` reads that too, so it is safe from any thread. **A new endpoint that needs a live sensor value uses that, or publishes its own snapshot the same way.** Grepping for `g_soilMoisture` outside `sensors.cpp` and the background tasks should find nothing.
- `g_otaEnabled` is cleared in `handleUpdateRequest`, so a single `/updateEnable` no longer arms the device until reboot.

---

## Task scheduler

`CriticalTaskScheduler` (`andrenepomuceno/CriticalTaskScheduler@^1.0.3`, aliases `TSTask` / `TSScheduler`). Tasks are declared with `DECLARE_TASK(name, period)` at the top of `src/tasks.cpp`, which mints `name##TaskHandler`, `g_##name##TaskPeriod` and `g_##name##Task` together. Registration happens only in `tasksSetup()`.

**All three minted names are file-local to `tasks.cpp`, and only the period escapes** — `extern const unsigned g_##name##TaskPeriod`, declared in `core/tasks.h` because `sensors.cpp` and `telemetry.cpp` size their accumulator windows and their publish queue from it. The handler and the `TSTask` object are `static`, which is what makes "what touches the scheduler" a question with a one-file answer: `grep -n 'g_[a-zA-Z]*Task\.' src/` finds only `tasks.cpp`. **`src/task_events.cpp` is held to that**, and it is the reason `armMoistureCheck()` exists rather than a second file reaching for `g_checkMoistureTask` — see [the split note](#project-at-a-glance). A handler BODY therefore cannot leave this file without changing the macro; what left instead is the work the bodies call out to.

| Task | Period | Bucket | Notes |
|---|---|---|---|
| `relays` | 50 ms | **critical** | Switches each relay off when its timer expires |
| `ledBlink` | 1 s | **critical** | Only blinks while `g_ledBlinkEnabled` |
| `io` | 1 s | background | The luminosity, water-level, flow and float reads, `webUpdateDataCache()`, then the relay / float / **cloud** / **step-value** events, and the deferred **`/sessions.json` write**. The cloud and ET0 models ride this tick rather than taking slots of their own, and so does the session file. `ioTaskHandler()` is still the literal list of what happens in one second; the four event publishers it names live in `src/task_events.cpp` |
| `moisture` | `io.soilMoisturePeriodSec` (1 s) | background | The probe bank: power up, two conversions per probe, power down, then the snapshot other threads read. Split out of `io` because that task paces the relay, cloud and ET0 events and the `/data.json` cache, and slowing the ADC must not slow those. The compiled 1 s is only the fallback until `tasksSetup()` calls `setPeriod()` |
| `ambient` | 1 s | background | Air temperature and humidity, from whichever part is fitted — a DHT11 on one wire or an SHT40 on I²C. Only when `config.ambientFitted()`; tracks `g_ambientReadErrors` / `g_ambientTotalReads`, and an implausible reading is discarded **and counted as an error**. One task for both, because a task per sensor KIND is how a firmware reaches the 16-slot cap `addTask()` overruns silently |
| `checkInternet` | 15 s | background | |
| `history` | `history.periodSec` (60 s) | background | One `IoRecord` appended to the newest segment; the compiled 60 s is only the fallback until `tasksSetup()` calls `setPeriod()` |
| `schedules` | 20 s | background | Fires a due schedule; three ticks a minute, with a 10 min catch-up window |
| `mqtt` | `mqtt.publishSec` (5 min) | background | Builds and publishes the **periodic** payload. The compiled 1 min is only the fallback until `setPeriod()`. Step values do not ride this tick at all |
| `talkBack` | 1 min | background | Polls the TalkBack queue. **Not registered by default** — the `DECLARE_TASK`, the handler and the `addTask()` are all behind `USE_TALKBACK` (0), so it takes no slot |
| `clockUpdate` | 24 h | background | NTP re-sync |
| `logBackup` | 1 h | background | `logger.backup()` |
| `moistureModel` | 24 h | background | Trains the classifier; three streaming passes over the history, so it stalls other background tasks for seconds |
| `checkMoisture` | 4 h | background | Armed through `armMoistureCheck()` when a zone's pump starts, disables itself. The check it runs, `reportWateringResponse()`, is in `src/task_events.cpp` beside the hook that took the baseline it compares against |

Both buckets go through the same `g_taskScheduler.addTask()`; `DECLARE_CRITICAL_TASK` passes `critical = true`, routing the task to `executeCritical()` on the `TSFreeRTOSCriticalRunner` thread. **Register and enable every critical task before `g_criticalRunner.start()`** — the scheduler holds no internal locks and the library documents mutation-after-start as unsafe.

Consequences that bite:

- **`execute()` runs at most ONE background task per call** — the latest-due one. A handler that blocks stalls every other *background* task for its whole duration.
- **`checkInternetTaskHandler` blocks on up to three `Ping.ping(addr, 2)` calls**, `talkBackTaskHandler` on a socket with a 5 s timeout, and `mqttTaskHandler` on draining up to 30 queued messages over TLS. Relay switch-off and the error blink survive that only because they are critical — putting relay timing back on the cooperative pump means a pump that runs long.
- Background tasks reschedule from **end of callback**, so a slow handler pushes its own next run out; critical tasks reschedule from the tick time and keep a fixed cadence.
- **A request handler must never call `ESP.restart()` or block.** `request->send()` only *queues* the response and the `async_tcp` task that flushes it is the one the handler runs on, so both kill the connection before the client sees anything. `/control` sets a flag and `requestRestart()` reboots from `loop()` 500 ms later; moving the restart after the `send()` was tried first and was not enough.
- **`g_relay[]` is touched from three threads** — the critical runner, `loop()` (TalkBack) and `async_tcp` (`/control`). Every access goes through the `g_relayMux` spinlock, and `relayWrite()` is deliberately called *outside* it: a `digitalWrite` inside a `portENTER_CRITICAL` section is exactly the kind of work that must not hold a spinlock.
- **`Scheduler::addTask()` returns `bool` and every call site ignores it.** The cap is `CRITICALTASKSCHEDULER_MAX_TASKS` = **16** per bucket; this repo registers **11 background and 2 critical** (12 background with `USE_TALKBACK` on). One task per relay plus per sensor crosses the cap and tasks are then **silently dropped**. Either check the return value or raise it with `-D CRITICALTASKSCHEDULER_MAX_TASKS=32`.

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
- **A filesystem deploy also wipes the history and the moisture model.** `uploadfs` and the filesystem OTA rewrite the whole 512 KB partition, so the history segments and `/moisture_model.bin` go with the web assets — the watering-event counter restarts from zero and the classifier waits another six waterings. That happened three times in one day here. **Changed only files under `data/`? Use `POST /spiffs/upload`.**
- **A filesystem deploy overwrites `/config.json` with `data/config.json`, and a masked GET cannot be restored from.** Back up with `GET /config.json?secrets=1` (ADMIN, logged with the caller's IP) BEFORE any `uploadfs` or filesystem OTA, and write the result into `data/config.json`. A "backup" through the normal masked GET loses every credential — eight asterisks each — and the loss only surfaces at the next boot.
- **`GET`/`POST /config.json` (both ADMIN) mask secrets in transit.** `GET` replaces `wifi.password`, `ota.password`, `thingSpeak.apiKey`, `talkBack.apiKey` and `mqtt.password` with `********` (`g_configSecretMask`); `POST` restores any field still carrying the mask from the document on disk. This deliberately diverges from fullbot, which serves the raw config — credentials included — to any authenticated session.
- **`GET /config.json` fills in an optional key the stored document omits**, because `/config.html` builds its form from the response and a missing key has no field. `?secrets=1` is unaffected and stays verbatim.
- **`POST /config.json` replaces the whole file**: `saveFile()` opens with `FILE_WRITE` (truncate). The handler merges into the stored document first, so a caller sends a complete document rather than a diff. **Nothing re-reads the file at runtime** — the response carries `restartRequired: true` and the change lands at the next boot.
- The handler rejects a document whose `id` does not match `config.deviceId`. Without that check a config pasted from another garden would be written, `loadFile()` would reject it at boot, and the device would come up on compiled defaults it cannot connect with — unreachable to fix without USB.
- **Changing `ota.password` is pushed into `UserStore` too — the PASSWORD, and nothing else.** The login password lives in `/users.json`, so a config-only change would not take effect until a filesystem deploy wiped the user store. It goes through `UserStore::setPassword()`, which preserves the account's role; it used to call `upsert(..., Role::ADMIN)`, and `upsert()` writes the whole entry, so a document naming an existing **OPERATOR** in `ota.username` with a changed password silently promoted them. **That was never a privilege-escalation path** — only an ADMIN reaches `POST /config.json`, and an ADMIN can promote anyone through `POST /users` — but it bypassed that endpoint's role validation and last-admin guard, and the response says only `saved`, so the realistic failure was an admin promoting somebody **by accident while restoring a config**. `Role::ADMIN` survives as the CREATE default only, the same seeding rule `UserStore::load()` applies to an empty `/users.json`. **`upsert()` is the whole-entry writer and `setPassword()`/`setRole()` are its single-attribute twins**; a door that writes one attribute should not have to read the other back in order to leave it alone.
- **Every `io` entry accepts more than one shape, and all of them must keep working** — a device in the field has to survive a firmware update. `io.relays`: array of `{pin,on,name}`, or the pre-2.0 `io.watering` + `io.wateringOn` scalars. `io.soilMoisture`: array of `{pin,name,powerPin,powerOn,settleMs}`, array of `{pin,name}`, array of bare pins, or a single bare pin. `io.dht` / `io.luminosity` / `io.waterLevel`: `{pin,name}` or a bare pin. Entries past the compiled count are ignored and missing ones keep their defaults — a short array logs a warning rather than zeroing a pin.
- **`mqtt.publishSec` (60..300, default 300) sets the PERIODIC payload's period only**, applied by `tasksSetup()` through `setPeriod()` exactly as `history.periodSec` is — so `g_mqttTaskPeriod` is nothing but the compiled fallback, and **reading it where the real period is meant is a live trap**. Two things must follow it: every accumulator window is sized from it in `sensorsSetup()`, **scalars included** (they used to take their window from `g_mqttTaskPeriod` at *file scope*, which was right only while that period was a constant — static initialisers run long before `config.json` is read, the same trap that made a file-scope `DHT_Unified` run for ever on the compiled default pin); and the publish queue depth is `3600000 / mqttPublishPeriodMs()`, an hour of buffer at any configured period.
- **`io.soilMoisturePeriodSec` (1..10000, default 1) paces the probe bank**, applied by `tasksSetup()` through `setPeriod()` exactly as `history.periodSec` is. It is NOT the io task's period — the probe read has its own `moisture` task. See [docs/soil-moisture-sampling.md](docs/soil-moisture-sampling.md).
- **`mqtt.heartbeatSec` (60..3600, default 900) is the floor under change-based publishing**, not a publish period. See [Sampling vs events](docs/telemetry.md).
- **`cloud.enabled` (default FALSE) gates the cloud-cover model entirely** — the computation, the `/data.json` badge, `cloudState` and `cloudVariability`. Off for the same reason `floatInterlock` is: the compiled clear-sky table is an upper envelope of one sensor's own history at one mounting, and on any other board every reading under it reads as cloud. Enabling it is a claim its owner makes after running `scripts/cloud_fit.py` against that device's own archive.
- **`et0.enabled` (default FALSE), `et0.latitude` and `et0.scale` gate the evapotranspiration estimate.** Off for a stronger reason than `cloud.enabled`: that one is unproven, this one was **measured to have no skill on this device** — the thermometer follows 35 % of the outdoor swing, so the diurnal range Hargreaves-Samani runs on describes the enclosure. `latitude` has no sensible default (0.0 is the equator, a real place), so an unset one cannot be detected and is logged at boot; a value outside ±90 is refused rather than clamped, because a clamped typo hides which number was wrong. `scale` ships at 1.0, is refused outside 0.5..2.0, and logs a warning whenever it is not 1.0, because a value far from it is a siting fault wearing a calibration coefficient. See [Evapotranspiration](docs/evapotranspiration.md).
- **`io.i2c` is the BUS, and it carries no fitted flag.** `{sda, scl, hz}`, every key optional, falling back to a per-family compiled default (21/22 on a WROOM-32, **8/9** on the S3 carrier). Presence is still the key — it is just the DEVICE's key, not the bus's, because a bus with nothing on it is not a peripheral. `Wire.begin()` runs if and only if an I²C device is declared, so on all five WROOM-32 boards neither pin is ever touched. `hz` is clamped to 10 000..1 000 000 and ships at 100 000: the SHT4x reaches 1 MHz and the carrier's trace is two centimetres, but `J8` brings this bus off the board to whatever somebody plugs in, and standard mode is the speed every I²C part agrees on.
- **`io.sht4x` declares the SHT40, and it is the one sensor kind with no pin of its own.** `{name, address}`; `address` is refused outside 0x44..0x46 rather than clamped, for the reason `et0.latitude` is — a clamped typo hides which number was wrong, and here the wrong number is a bus that answers nothing with no clue why. `name` is a PREFIX, because one part produces two channels.
- **`postalCode` is metadata the firmware never acts on** — parsed, stored, inert. It exists so off-device tooling knows where the device is and the archive is self-describing. Optional. Named in English per the conventions even though the value here is a Brazilian CEP, because the device can be anywhere.
- **Sensor and relay names are LABELS, not identifiers.** `/data.json` keys `Inputs` and `Outputs` by them, so renaming changes the dashboard immediately — but the `Relays` array stays index-addressed, the history record is positional, and the ThingsBoard telemetry keys stay `moisture1..N`. Renaming therefore never rewrites stored history. A name on `io.dht` is a *prefix*, because one pin produces two channels.
- **`"version"` in the JSON is still never read** — the template says `2` but nothing enforces or migrates on it. `log.level` *is* read (clamped to `LOG_DISABLE..LOG_TRACE`) and applied in `loadConfigFile()`.
- **`loadFile()` mutates fields as it parses and only returns `false` at the end**, so a rejected config leaves a half-populated object behind.

---

## Documentation

`README.md` is the entry point. Reference material lives in `docs/`, one file
per subsystem: prose belongs there, not in code comments and not in this file.

| Document | What is in it |
|---|---|
| [docs/verification-log.md](docs/verification-log.md) | **What has actually run on hardware, and what has not.** Read it before believing any number |
| [docs/architecture.md](docs/architecture.md) | The file-by-file table, the 1000-line gate, the two `tasks.cpp` splits |
| [docs/hardware.md](docs/hardware.md) | Boards, pinouts, the `espgarden_s3` carrier, the S3 pin rules and where each came from |
| [docs/configuration.md](docs/configuration.md) | Every `config.json` key, the shapes each accepts, runtime against build-time hardware |
| [docs/authentication.md](docs/authentication.md) | Nonce + SHA-256 login, roles, the partition table, the reservoir interlock |
| [docs/web-interface.md](docs/web-interface.md) | Routes, the `/data.json` contract, sessions, OTA and its traps |
| [docs/web-assets.md](docs/web-assets.md) | Why the pages are bundled and gzipped |
| [docs/filesystem.md](docs/filesystem.md) | LittleFS, and why the driver name appears once |
| [docs/io-history.md](docs/io-history.md) | Append-only segments, the fit check, the boot-loop interlock |
| [docs/sensors.md](docs/sensors.md) | Accumulators, conversions, the publish queue |
| [docs/soil-moisture.md](docs/soil-moisture.md) | The classifier, its gates, probe health, the drying and temperature analyses |
| [docs/soil-moisture-sampling.md](docs/soil-moisture-sampling.md) | `io.soilMoisturePeriodSec` and the `moisture` task |
| [docs/ambient-sensors.md](docs/ambient-sensors.md) | SHT40 on I2C, and why it and the DHT share two keys |
| [docs/cloud-cover.md](docs/cloud-cover.md) | The empirical clear-sky reference |
| [docs/evapotranspiration.md](docs/evapotranspiration.md) | Hargreaves-Samani ET0, and why it ships disabled |
| [docs/telemetry.md](docs/telemetry.md) | Backends, sampling against events, RPC, cloud FOTA, the compiled-out flags |
| [docs/thingsboard-migration.md](docs/thingsboard-migration.md) | The move off ThingsBoard Cloud |
| [docs/onboarding.md](docs/onboarding.md) | The first-boot setup AP and its marker |
| [docs/relays.md](docs/relays.md) | The two seams, and why `startRelay()` is the only door |
| [docs/task-schedule.md](docs/task-schedule.md) | Every task, its period and its bucket |
| [docs/tooling.md](docs/tooling.md) | The PC-side scripts under `scripts/` |
| [docs/porting-fullbot.md](docs/porting-fullbot.md) | What came from `fullbot-firmware` and what did not |

---

## Conventions

- **English in code** — identifiers, comments, log strings, commit messages. Reply to the user in the language they write in (Portuguese here).
- **`.clang-format` is Mozilla base, 4-space indent.** Nothing enforces it; do not reformat lines you did not touch.
- Includes are prefixed: `core/`, `network/`. `BuildConfig.h` carries no prefix.
- Feature flags live in `include/BuildConfig.h` (`FW_VERSION`, `USE_WEBSERVER`, `USE_MQTT`, `USE_OTA`, `USE_THINGSPEAK`, `USE_TALKBACK`, `USE_WATERING_PWM`), along with the caps `RELAY_MAX` and `MOISTURE_MAX`. **`USE_THINGSPEAK` and `USE_TALKBACK` both ship at 0.** A flag is only real where it is honoured at EVERY site — `USE_TALKBACK` sat here at 1 for months and was read nowhere, which reads as "the feature is off" while the socket opens anyway. **`platformio.ini` carries no hardware flags at all** — what a board has is in its own `config.json`, and the envs differ only by `board`.
- `FW_VERSION` is the version ThingsBoard compares an offered package against and the string `/data.json` reports as `Status.Firmware`. **Bump it in the same commit as the change it names**, or a FOTA of that change is a no-op the broker reports as success.
- **No AI co-author trailers** in commits. **Never `git add -A`** — stage explicitly. **Commits and pushes no longer need to be asked for** (standing authorization from the repo owner), but the gate does not move: five envs build, `pio test -e native` passes and `python scripts/check_lines.py` is green before a commit exists. Never commit `data/config.json` or anything under `backups/`.
- Commit format: imperative + conventional tag (`feat | fix | refactor | chore | docs | test | perf | style`), e.g. `fix(tasks): construct DHT after config load`.
- **Never overwrite `data/config.json`** (gitignored, holds a real device's credentials) and never commit real credentials — only `data/config.template.json` is tracked.
- **Prose belongs in `docs/`, not in code comments and not here.** A comment is not documentation. When a file being changed carries a large explanatory comment block, the block goes to `docs/` and out of the source. `README.md` stays an entry point and an index; this file stays the operating guide.

---

## End-of-change checklist

- [ ] `pio test -e native` passes; new behaviour in testable code has a regression test
- [ ] `pio run` builds all five envs clean (they share one shape now, so a break in one is a break in all)
- [ ] `-t buildfs` run if `data/` changed, and **flash usage still under 100 %** of the app slot
- [ ] `/data.json` payload changes mirrored in `data/index.js` **and** the simulator (`scripts/sim_state.py` for the payload, `sim_config.py` for the document, `sim_moisture.py` for the model, `sim_onboarding.py` for the setup portal, `dev_server.py` for the route and `PUBLIC_PATHS`)
- [ ] New config key: all 5 edits, template included; new sensor KIND: all 8
- [ ] `data/config.json` untouched, no credentials introduced
- [ ] `python scripts/check_lines.py` passes
- [ ] **[What has actually run](docs/verification-log.md) is still honest** — anything that ran moved up *because it ran*, and anything newly written went into the unverified list
- [ ] No number in this file without an execution behind it
- [ ] Conventional commit message suggested — **no commit unless the user asked**
